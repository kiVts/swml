#include "core.h"
#include "health.h"
#include "core_v2.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace {
constexpr size_t kMaxEntries = 2048, kPerModEntries = 256, kMaxEvents = 1024, kMaxNetwork = 256;
constexpr uint64_t kMaxBytes = 8 * 1024 * 1024, kMaxDelay = 365ull * 24 * 60 * 60 * 1000;
constexpr uint32_t kMaxStorage = 16 * 1024 * 1024;
struct Entry {
    swml_handle id = 0;
    swml_mod* owner = nullptr;
    swml_callback callback = nullptr;
    void* context = nullptr;
    std::string topic;
    std::vector<uint8_t> data;
    uint32_t lane = 0, running = 0;
    uint64_t due = 0, period = 0;
    bool active = true;
};
struct Pending {
    std::string topic, sender;
    std::vector<uint8_t> data;
    swml_mod* owner = nullptr; // producer; remove its undelivered events if initialization fails
    swml_mod* recipient = nullptr; // null = all subscribers
};
struct Service { swml_mod* owner; const swml_service_header* table; };
struct Outgoing { swml_mod* owner; swml_transport_packet packet; };
using TaskKey = std::pair<uint64_t, swml_handle>;
std::mutex g_mutex;
std::set<swml_mod*> g_owners;
std::unordered_map<swml_handle, std::shared_ptr<Entry>> g_entries;
std::unordered_map<std::string, std::vector<std::shared_ptr<Entry>>> g_topics;
std::map<TaskKey, std::shared_ptr<Entry>> g_tasks[2];
std::deque<Pending> g_events;
std::unordered_map<std::string, Service> g_services;
std::deque<Outgoing> g_outgoing;
std::set<std::string> g_peers;
swml_mod* g_provider_owner = nullptr;
swml_handle g_provider = 0, g_next = 1;
uint64_t g_bytes = 0, g_callbacks = 0, g_rejected = 0, g_failures = 0;
std::atomic<uint64_t> g_caps{SWML_CAP_EVENTS | SWML_CAP_TASKS | SWML_CAP_SERVICES | SWML_CAP_STORAGE};
// Native game/render hooks each own their lane. Guard reentrant pumping without serializing lanes.
std::atomic_flag g_pumping[2] = {ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT};

uint64_t now_ms() {
    static const uint64_t frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    uint64_t ticks = static_cast<uint64_t>(value.QuadPart);
    return frequency ? ticks / frequency * 1000 + ticks % frequency * 1000 / frequency : GetTickCount64();
}
uint64_t capabilities() { return g_caps.load(std::memory_order_acquire); }
int lane_index(uint32_t lane) { return lane == SWML_LANE_GAME ? 0 : lane == SWML_LANE_RENDER ? 1 : -1; }
bool named(const char* name) {
    if (!name) return false;
    size_t n = strnlen_s(name, SWML_NAME_CAPACITY);
    if (!n || n >= SWML_NAME_CAPACITY) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}
bool owned_name(swml_mod* m, const char* name) {
    return m && named(name) && std::string(name).starts_with(m->id + ".");
}
bool owner_ok(swml_mod* m) { return m && g_owners.contains(m); } // g_mutex held
std::string peer_key(const swml_peer& p) { return std::string(reinterpret_cast<const char*>(&p), sizeof(p)); }
const char* mod_id(swml_mod* m) {
    std::lock_guard lock(g_mutex);
    return owner_ok(m) ? m->id.c_str() : "";
}
bool room(swml_mod* owner, uint32_t bytes) {
    if (g_entries.size() >= kMaxEntries || g_bytes + bytes > kMaxBytes) return false;
    size_t count = 0;
    for (const auto& [id, e] : g_entries) if (e->owner == owner) ++count;
    return count < kPerModEntries;
}
swml_result queue_event(swml_mod* producer, swml_mod* recipient, const char* sender,
                        const char* topic, const void* data, uint32_t size) {
    // g_mutex held. Even with no listeners, validate arguments before reaching here.
    if (!g_topics.contains(topic)) return SWML_OK;
    if (g_events.size() >= kMaxEvents || g_bytes + size > kMaxBytes) { ++g_rejected; return SWML_LIMIT; }
    Pending p;
    p.owner = producer; p.recipient = recipient; p.sender = sender; p.topic = topic;
    if (size) p.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    g_bytes += size;
    g_events.push_back(std::move(p));
    return SWML_OK;
}
void unlink_entry(const std::shared_ptr<Entry>& e) {
    // g_mutex held; only once, when cancellation/one-shot completion becomes quiescent.
    if (e->lane) g_tasks[lane_index(e->lane)].erase({e->due, e->id});
    else {
        auto t = g_topics.find(e->topic);
        if (t != g_topics.end()) {
            auto& list = t->second;
            list.erase(std::remove(list.begin(), list.end(), e), list.end());
            if (list.empty()) g_topics.erase(t);
        }
    }
    g_bytes -= e->data.size();
    g_entries.erase(e->id);
}
int invoke_raw(swml_callback callback, const swml_event* event, void* context) {
    __try { callback(event, context); return 1; }
    __except (GetExceptionCode() == 0xe06d7363 ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void invoke(const std::shared_ptr<Entry>& e, const char* topic, const char* sender,
             uint32_t lane, const void* data, uint32_t size) {
    {
        std::lock_guard lock(g_mutex);
        if (!e->active || !owner_ok(e->owner)) return;
        ++e->running;
    }
    swml_event event{sizeof(swml_event), lane, now_ms(), topic, sender, data, size};
    bool profiling = std::atomic_ref(e->owner->profiling).load(std::memory_order_relaxed) != 0;
    LARGE_INTEGER started{};
    if (profiling) QueryPerformanceCounter(&started);
    bool okay = false;
    try { okay = invoke_raw(e->callback, &event, e->context) != 0; } catch (...) { okay = false; }
    if (profiling) {
        LARGE_INTEGER finished{}, frequency{}; QueryPerformanceCounter(&finished); QueryPerformanceFrequency(&frequency);
        uint64_t ticks=uint64_t(std::max<LONGLONG>(0,finished.QuadPart-started.QuadPart));
        uint64_t rate=uint64_t(frequency.QuadPart);
        uint64_t nanoseconds=rate?ticks/rate*1000000000ull+(ticks%rate)*1000000000ull/rate:0;
        std::atomic_ref(e->owner->profiled_callbacks).fetch_add(1,std::memory_order_relaxed);
        std::atomic_ref(e->owner->callback_ns).fetch_add(nanoseconds,std::memory_order_relaxed);
        auto longest=std::atomic_ref(e->owner->longest_callback_ns); auto previous=longest.load(std::memory_order_relaxed);
        while(previous<nanoseconds&&!longest.compare_exchange_weak(previous,nanoseconds,std::memory_order_relaxed)) {}
    }
    {
        std::lock_guard lock(g_mutex);
        ++g_callbacks;
        --e->running;
        if (!okay) { ++g_failures; e->active = false; }
        if (!e->active && !e->running && g_entries.contains(e->id)) unlink_entry(e);
    }
    if (!okay) {
        swml_health::note_fault();
        core_log(e->owner->id.c_str(), "SDK callback failed; subscription/task disabled");
    }
}
void deliver(uint32_t lane, const char* topic, const char* sender,
              const void* data, uint32_t size, swml_mod* recipient = nullptr) {
    std::vector<std::shared_ptr<Entry>> listeners;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_topics.find(topic);
        if (it == g_topics.end()) return;
        listeners = it->second;
    }
    for (const auto& e : listeners)
        if (!recipient || e->owner == recipient) invoke(e, topic, sender, lane, data, size);
}
swml_result subscribe(swml_mod* m, const char* topic, swml_callback callback, void* context, swml_handle* out) {
    if (out) *out = 0;
    if (!out || !named(topic) || !callback) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    if (!room(m, 0)) { ++g_rejected; return SWML_LIMIT; }
    auto e = std::make_shared<Entry>();
    e->id = g_next++; e->owner = m; e->topic = topic; e->callback = callback; e->context = context;
    g_entries[e->id] = e; g_topics[topic].push_back(e); *out = e->id;
    return SWML_OK;
}
swml_result publish(swml_mod* m, const char* topic, const void* data, uint32_t size) {
    if (!named(topic) || size > SWML_MAX_PAYLOAD || (size && !data)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m) || !owned_name(m, topic)) return SWML_DENIED;
    if (!(capabilities() & SWML_CAP_GAME_LOOP)) return SWML_UNAVAILABLE;
    return queue_event(m, nullptr, m->id.c_str(), topic, data, size);
}
swml_result schedule(swml_mod* m, uint32_t lane, uint64_t delay, uint64_t period,
                      swml_callback callback, void* context, const void* data, uint32_t size, swml_handle* out) {
    if (out) *out = 0;
    int index = lane_index(lane);
    if (!out || index < 0 || !callback || delay > kMaxDelay || period > kMaxDelay ||
        size > SWML_MAX_PAYLOAD || (size && !data)) return SWML_INVALID;
    uint64_t cap = lane == SWML_LANE_GAME ? SWML_CAP_GAME_LOOP : SWML_CAP_RENDER;
    if (!(capabilities() & cap)) return SWML_UNAVAILABLE;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    if (!room(m, size)) { ++g_rejected; return SWML_LIMIT; }
    auto e = std::make_shared<Entry>();
    e->id = g_next++; e->owner = m; e->callback = callback; e->context = context;
    e->lane = lane; e->due = now_ms() + delay; e->period = period;
    if (size) e->data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    g_bytes += size; g_entries[e->id] = e; g_tasks[index][{e->due, e->id}] = e; *out = e->id;
    return SWML_OK;
}
swml_result cancel(swml_mod* m, swml_handle id) {
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    auto it = g_entries.find(id);
    if (it == g_entries.end()) return SWML_NOT_FOUND;
    auto e = it->second;
    if (e->owner != m) return SWML_DENIED;
    e->active = false;
    if (e->running) return SWML_PENDING;
    unlink_entry(e);
    return SWML_OK;
}
swml_result service_publish(swml_mod* m, const char* name, const swml_service_header* table) {
    if (!named(name) || !table || !table->version || table->size < sizeof(*table) || table->size > SWML_MAX_PAYLOAD)
        return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m) || !owned_name(m, name)) return SWML_DENIED;
    if (g_services.contains(name)) return SWML_CONFLICT;
    if (g_services.size() >= 256) return SWML_LIMIT;
    g_services.emplace(name, Service{m, table});
    return SWML_OK;
}
swml_result service_query(swml_mod* m, const char* name, uint32_t version, uint32_t min_size,
                           const swml_service_header** out) {
    if (out) *out = nullptr;
    if (!out || !named(name) || !version || min_size < sizeof(swml_service_header)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    auto it = g_services.find(name);
    if (it == g_services.end()) return SWML_UNAVAILABLE;
    const auto* table = it->second.table;
    if (table->version != version || table->size < min_size) return SWML_UNAVAILABLE;
    *out = table;
    return SWML_OK;
}
bool valid_setting(const wchar_t* value) {
    if (!value || !*value || wcsnlen_s(value, 256) >= 256) return false;
    return !wcspbrk(value, L"\r\n[]=");
}
bool snapshot_paths(swml_mod* m, std::wstring& dir, std::wstring& settings) {
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return false;
    dir = m->dir; settings = m->settings; return true;
}
swml_result setting_read(swml_mod* m, const wchar_t* section, const wchar_t* key,
                          wchar_t* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity) || !valid_setting(section) || !valid_setting(key)) return SWML_INVALID;
    std::wstring dir, settings;
    if (!snapshot_paths(m, dir, settings)) return SWML_DENIED;
    wchar_t buffer[32768];
    // A sentinel distinguishes an absent key from an intentionally empty string.
    constexpr wchar_t missing[] = L"\x1fSWML_MISSING\x1f";
    DWORD n = GetPrivateProfileStringW(section, key, missing, buffer, 32768, settings.c_str());
    if (wcscmp(buffer, missing) == 0) return SWML_NOT_FOUND;
    if (n >= 32767) return SWML_LIMIT;
    *needed = n + 1;
    if (capacity < *needed) return SWML_BUFFER_SMALL;
    memcpy(out, buffer, size_t(*needed) * sizeof(wchar_t));
    return SWML_OK;
}
bool storage_path(swml_mod* m, const wchar_t* name, std::wstring& path) {
    if (!name || !*name || wcsnlen_s(name, SWML_NAME_CAPACITY) >= SWML_NAME_CAPACITY || wcsstr(name, L"..")) return false;
    for (const wchar_t* p = name; *p; ++p)
        if (!(iswalnum(*p) || *p == L'_' || *p == L'-' || *p == L'.')) return false;
    if (name[wcslen(name) - 1] == L'.') return false;
    std::wstring stem(name, wcscspn(name, L"."));
    for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9')) return false;
    std::wstring dir, settings;
    if (!snapshot_paths(m, dir, settings)) return false;
    dir += L"\\data";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT) || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    path = dir + L"\\" + name;
    attr = GetFileAttributesW(path.c_str());
    return attr == INVALID_FILE_ATTRIBUTES || !(attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}
swml_result storage_read(swml_mod* m, const wchar_t* name, void* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity)) return SWML_INVALID;
    std::wstring path;
    if (!storage_path(m, name, path)) return SWML_INVALID;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? SWML_NOT_FOUND : SWML_IO_ERROR;
    LARGE_INTEGER length{};
    bool okay = GetFileSizeEx(f, &length) != 0;
    swml_result result = SWML_IO_ERROR;
    if (okay && length.QuadPart >= 0 && length.QuadPart <= kMaxStorage) {
        *needed = static_cast<uint32_t>(length.QuadPart);
        if (capacity < *needed) result = SWML_BUFFER_SMALL;
        else {
            DWORD read = 0;
            result = !*needed || (ReadFile(f, out, *needed, &read, nullptr) && read == *needed) ? SWML_OK : SWML_IO_ERROR;
        }
    } else if (okay) result = SWML_LIMIT;
    CloseHandle(f); return result;
}
swml_result storage_write(swml_mod* m, const wchar_t* name, const void* data, uint32_t size) {
    if ((size && !data) || size > kMaxStorage) return SWML_INVALID;
    std::wstring path;
    if (!storage_path(m, name, path)) return SWML_INVALID;
    static std::atomic<uint64_t> sequence{0};
    std::wstring temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return SWML_IO_ERROR;
    DWORD written = 0;
    bool okay = (!size || (WriteFile(f, data, size, &written, nullptr) && written == size)) && FlushFileBuffers(f);
    CloseHandle(f);
    if (okay) okay = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!okay) DeleteFileW(temp.c_str());
    return okay ? SWML_OK : SWML_IO_ERROR;
}
bool provider_ok(swml_mod* m, swml_handle handle) {
    return owner_ok(m) && g_provider && m == g_provider_owner && handle == g_provider;
}
swml_result transport_bind(swml_mod* m, swml_handle* out) {
    if (out) *out = 0;
    if (!out) return SWML_INVALID;
    if (!(capabilities() & SWML_CAP_GAME_LOOP)) return SWML_UNAVAILABLE;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    if (g_provider) return SWML_CONFLICT;
    g_provider_owner = m; g_provider = g_next++; *out = g_provider;
    return SWML_OK;
}
swml_result transport_peer(swml_mod* m, swml_handle handle, const swml_peer* peer, int connected) {
    if (!peer) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!provider_ok(m, handle)) return SWML_DENIED;
    auto key = peer_key(*peer);
    if (connected) {
        if (g_peers.size() >= 64 && !g_peers.contains(key)) return SWML_LIMIT;
        g_peers.insert(key);
    } else g_peers.erase(key);
    if (g_peers.empty()) g_caps.fetch_and(~uint64_t(SWML_CAP_NETWORK));
    else g_caps.fetch_or(SWML_CAP_NETWORK);
    return SWML_OK;
}
swml_result network_peers(swml_mod* m, swml_peer* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    *needed = static_cast<uint32_t>(g_peers.size());
    if (capacity < *needed) return SWML_BUFFER_SMALL;
    uint32_t i = 0;
    for (const auto& p : g_peers) memcpy(&out[i++], p.data(), sizeof(swml_peer));
    return SWML_OK;
}
swml_result network_send(swml_mod* m, const swml_peer* peer, const char* topic,
                          const void* data, uint32_t size, swml_handle* operation) {
    if (operation) *operation = 0;
    if (!operation || !peer || !named(topic) || size > SWML_NET_MAX_PAYLOAD || (size && !data)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m) || !owned_name(m, topic)) return SWML_DENIED;
    if (!g_provider || !g_peers.contains(peer_key(*peer))) return SWML_UNAVAILABLE;
    if (g_outgoing.size() >= kMaxNetwork) { ++g_rejected; return SWML_LIMIT; }
    Outgoing out{}; out.owner = m; out.packet.operation = g_next++; out.packet.peer = *peer;
    strcpy_s(out.packet.topic, topic); out.packet.data_size = size;
    if (size) memcpy(out.packet.data, data, size);
    *operation = out.packet.operation; g_outgoing.push_back(out);
    return SWML_OK;
}
swml_result transport_peek(swml_mod* m, swml_handle handle, swml_transport_packet* out) {
    if (!out) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!provider_ok(m, handle)) return SWML_DENIED;
    if (g_outgoing.empty()) return SWML_NOT_FOUND;
    *out = g_outgoing.front().packet;
    return SWML_OK;
}
swml_result transport_complete(swml_mod* m, swml_handle handle, swml_handle operation, swml_result result) {
    if (result != SWML_OK && result != SWML_UNAVAILABLE && result != SWML_IO_ERROR) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!provider_ok(m, handle)) return SWML_DENIED;
    if (g_outgoing.empty() || g_outgoing.front().packet.operation != operation) return SWML_NOT_FOUND;
    const auto& out = g_outgoing.front();
    swml_network_result event{operation, out.packet.peer, result};
    std::string topic = out.owner->id + ".net.result";
    swml_result queued = queue_event(nullptr, out.owner, "swml", topic.c_str(), &event, sizeof(event));
    if (queued != SWML_OK) return queued; // retain completion until its event can be queued
    g_outgoing.pop_front(); return SWML_OK;
}
swml_result transport_receive(swml_mod* m, swml_handle handle, const swml_peer* peer,
                               const char* topic, const void* data, uint32_t size) {
    if (!peer || !named(topic) || std::string(topic).starts_with("swml.") || std::string(topic).ends_with(".net.result") ||
        size > SWML_NET_MAX_PAYLOAD || (size && !data)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!provider_ok(m, handle)) return SWML_DENIED;
    if (!g_peers.contains(peer_key(*peer))) return SWML_UNAVAILABLE;
    swml_network_message message{}; message.peer = *peer; message.data_size = size;
    if (size) memcpy(message.data, data, size);
    return queue_event(m, nullptr, "swml.network", topic, &message,
                        static_cast<uint32_t>(offsetof(swml_network_message, data)) + size);
}
swml_result stats(swml_mod* m, swml_runtime_stats* out) {
    if (!out || out->size < sizeof(*out)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(m)) return SWML_DENIED;
    uint32_t subscriptions = 0, tasks = 0;
    for (const auto& [id, e] : g_entries) { if (e->lane) ++tasks; else ++subscriptions; }
    *out = {sizeof(*out), subscriptions, tasks, static_cast<uint32_t>(g_events.size()),
            static_cast<uint32_t>(g_outgoing.size()), g_bytes, g_callbacks, g_rejected, g_failures};
    return SWML_OK;
}
const swml_api_v2 kApi{
    SWML_V2_VERSION, sizeof(swml_api_v2), capabilities, now_ms, mod_id,
    subscribe, publish, schedule, cancel, service_publish, service_query, setting_read,
    storage_read, storage_write, network_peers, network_send, stats,
    transport_bind, transport_peer, transport_peek, transport_complete, transport_receive
};
} // namespace

const swml_api_v2* core_v2_api() { return &kApi; }
swml_result core_v2_forward(swml_mod* sender, swml_mod* recipient, const char* topic, const void* data, uint32_t size) {
    if (!named(topic) || size > SWML_MAX_PAYLOAD || (size && !data)) return SWML_INVALID;
    std::lock_guard lock(g_mutex);
    if (!owner_ok(sender) || !owner_ok(recipient)) return SWML_DENIED;
    if (!(capabilities() & SWML_CAP_GAME_LOOP)) return SWML_UNAVAILABLE;
    return queue_event(sender, recipient, sender->id.c_str(), topic, data, size);
}
void core_v2_attach(swml_mod* m) { std::lock_guard lock(g_mutex); g_owners.insert(m); }
void core_v2_available(bool game, bool render) {
    uint64_t caps = SWML_CAP_EVENTS | SWML_CAP_TASKS | SWML_CAP_SERVICES | SWML_CAP_STORAGE;
    if (game) caps |= SWML_CAP_GAME_LOOP;
    if (render) caps |= SWML_CAP_RENDER;
    g_caps.store(caps, std::memory_order_release);
}
void core_v2_drop(swml_mod* m) {
    std::lock_guard lock(g_mutex);
    g_owners.erase(m);
    std::vector<std::shared_ptr<Entry>> entries;
    for (const auto& [id, e] : g_entries) if (e->owner == m) entries.push_back(e);
    for (const auto& e : entries) { e->active = false; if (!e->running) unlink_entry(e); }
    for (auto it = g_services.begin(); it != g_services.end();)
        if (it->second.owner == m) it = g_services.erase(it); else ++it;
    for (auto it = g_events.begin(); it != g_events.end();)
        if (it->owner == m || it->recipient == m) { g_bytes -= it->data.size(); it = g_events.erase(it); } else ++it;
    for (auto it = g_outgoing.begin(); it != g_outgoing.end();)
        if (it->owner == m) it = g_outgoing.erase(it); else ++it;
    if (g_provider_owner == m) {
        g_provider = 0; g_provider_owner = nullptr; g_peers.clear();
        // Initialization failure revokes the provider and all unsent transport work.
        g_outgoing.clear(); g_caps.fetch_and(~uint64_t(SWML_CAP_NETWORK));
    }
}
void core_v2_ready() {
    std::lock_guard lock(g_mutex);
    queue_event(nullptr, nullptr, "swml", "swml.mods.ready", nullptr, 0);
}
void core_v2_dispatch(uint32_t lane, const char* topic, const void* data, uint32_t size) {
    deliver(lane, topic, "swml", data, size);
}
swml_result core_v2_queue_builtin(const char* topic,const void* data,uint32_t size) {
    if(!named(topic)||!std::string_view(topic).starts_with("swml.")||size>SWML_MAX_PAYLOAD||(size&&!data))return SWML_INVALID;
    std::lock_guard lock(g_mutex);if(!(capabilities()&SWML_CAP_GAME_LOOP))return SWML_UNAVAILABLE;
    return queue_event(nullptr,nullptr,"swml",topic,data,size);
}
void core_v2_pump(uint32_t lane) {
    int index = lane_index(lane);
    if (index < 0 || g_pumping[index].test_and_set(std::memory_order_acquire)) return;
    const uint64_t start = now_ms();
    // Bounded between queued operations; callbacks themselves cannot be preempted.
    for (unsigned work = 0; work < 64 && now_ms() - start < 2; ++work) {
        std::shared_ptr<Entry> task;
        Pending event;
        bool has_event = false;
        {
            std::lock_guard lock(g_mutex);
            // Alternate sources so a large event burst cannot starve due tasks (or vice versa).
            if (lane == SWML_LANE_GAME && !g_events.empty() && (work % 2 == 0 || g_tasks[index].empty() ||
                g_tasks[index].begin()->first.first > now_ms())) {
                event = std::move(g_events.front()); g_events.pop_front();
                g_bytes -= event.data.size(); has_event = true;
            } else if (!g_tasks[index].empty() && g_tasks[index].begin()->first.first <= now_ms()) {
                task = g_tasks[index].begin()->second; g_tasks[index].erase(g_tasks[index].begin());
            } else break;
        }
        if (has_event) deliver(lane, event.topic.c_str(), event.sender.c_str(), event.data.data(),
                               static_cast<uint32_t>(event.data.size()), event.recipient);
        else if (task) {
            invoke(task, "swml.task", "swml", lane, task->data.data(), static_cast<uint32_t>(task->data.size()));
            std::lock_guard lock(g_mutex);
            if (!g_entries.contains(task->id)) continue;
            if (!task->active || !task->period) { task->active = false; unlink_entry(task); }
            else { task->due = now_ms() + task->period; g_tasks[index][{task->due, task->id}] = task; }
        }
    }
    g_pumping[index].clear(std::memory_order_release);
}
