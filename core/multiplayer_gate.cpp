// Steam Messages v002 boundary. No gameplay message is sent/delivered before a
// fresh two-way SWML challenge completes. This is a compatibility gate, not remote attestation.
#include "multiplayer_gate.h"
#include "multiplayer_protocol.h"
#include "core.h"
#include "core_v3.h"
#include "health.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>

namespace {
struct Identity { int32_t type, size; uint8_t data[128]; };
static_assert(sizeof(Identity) == 136);
struct Message {
    void* data; int32_t size; uint32_t connection; Identity identity;
    int64_t user, received, number;
    void (*free_data)(Message*); void (*release)(Message*);
    int32_t channel, flags;
};
static_assert(offsetof(Message, identity) == 0x10 && offsetof(Message, release) == 0xb8 &&
              offsetof(Message, channel) == 0xc0);
using Get = void*(__cdecl*)();
// Identities are passed by reference in Steam's C++ ABI, i.e. as pointers. They are taken as pointers here so a
// null one can be refused instead of read.
using Send = int(__fastcall*)(void*, const Identity*, const void*, uint32_t, int, int);
using Receive = int(__fastcall*)(void*, int, Message**, int);
using Accept = bool(__fastcall*)(void*, const Identity*);
using Close = bool(__fastcall*)(void*, const Identity*);
using Info = int(__fastcall*)(void*, const Identity*, void*, void*);
using Shutdown = void(__cdecl*)();
Get original_get = nullptr;
Send original_send = nullptr;
Receive original_receive = nullptr;
Accept original_accept = nullptr;
Close original_close = nullptr;
Info session_info = nullptr;
Shutdown original_shutdown = nullptr;
void* messages = nullptr;
bool stopped = false, bound = false, installed = false, bind_failed = false;
std::recursive_mutex mutex;
swml_gate::Gate gate;
struct Held { Message* message; uint64_t peer; };
std::deque<Held> held;
std::map<uint64_t, uint32_t> connections;
size_t held_bytes = 0;
constexpr size_t max_held_bytes = 8 * 1024 * 1024, max_held_messages = 256;
constexpr int result_ok = 1, result_no_connection = 3, result_limit = 25;
std::atomic<int> notice{0}; // 1 waiting, 2 rejected, 3 backend unavailable
uint64_t rejection_time = 0;
uint64_t last_pump = 0;
bool have_pumped = false;
uint64_t last_bind_attempt = 0;
// The gate hooks the ISteamNetworkingMessages object only, by giving it a copy of its vtable with slots 0..3
// replaced. The code behind those slots is not patched, since it may also serve other interfaces. The copy
// has 16 slots so other mods that copy 16 entries from the object's vtable keep working.
constexpr size_t vtable_slots = 16, max_tables = 4;
void* tables[max_tables][vtable_slots]{}; // never freed: a call may still be on its way through an old one
size_t tables_used = 0;
void** active_table = nullptr;
bool rewrap_reported = false;

uint64_t key(const Identity* identity) {
    // Stormworks Steam peers use SteamID identities. Never key uninitialized padding.
    if (!identity || identity->type != 16 || identity->size != 8) return 0;
    uint64_t id = 0; std::memcpy(&id, identity->data, 8); return id;
}
Identity identity_for(uint64_t id) {
    Identity out{}; out.type = 16; out.size = 8; std::memcpy(out.data, &id, 8); return out;
}
int session_check(uint64_t id) { const auto identity = identity_for(id); return session_info(messages, &identity, nullptr, nullptr); }
swml_gate::Nonce random_nonce() {
    swml_gate::Nonce nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), ULONG(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        nonce.fill(0);
    return nonce;
}
void release_message(Message* m) { if (m && m->release) m->release(m); }
void discard(uint64_t id) {
    for (auto it = held.begin(); it != held.end();) {
        if (it->peer != id) { ++it; continue; }
        held_bytes -= size_t(it->message->size); release_message(it->message); it = held.erase(it);
    }
}
void reject(uint64_t id, const char* reason, uint64_t now) {
    auto it = gate.peers.find(id);
    const bool already = it != gate.peers.end() && it->second.rejection_reported;
    if (it != gate.peers.end()) {
        it->second.state = swml_gate::State::rejected;
        it->second.rejection_reported = true;
    }
    discard(id);
    notice.store(2); rejection_time = now;
    // Log once for tracked peers; capacity floods are summarized at most once per second.
    static uint64_t last_overflow_log = 0;
    if (!already && (it != gate.peers.end() || now - last_overflow_log >= 1000)) {
        corelog("multiplayer blocked: %s; both sides need a compatible SWML core", reason);
        if (it == gate.peers.end()) last_overflow_log = now;
    }
    if (messages && original_close) { const auto identity = identity_for(id); original_close(messages, &identity); }
}
swml_gate::Peer* touch(uint64_t id, uint64_t now) {
    auto it = gate.peers.find(id);
    if (it != gate.peers.end()) {
        if (it->second.state == swml_gate::State::allowed && session_info && now - it->second.last_session_check >= 100 &&
            session_check(id) != 3) {
            discard(id); connections.erase(id); gate.peers.erase(it);
        } else {
            if (it->second.state == swml_gate::State::allowed && now - it->second.last_session_check >= 100)
                it->second.last_session_check = now;
            return &it->second;
        }
    }
    auto* p = gate.begin(id, now, random_nonce());
    if (!p) { if (id) reject(id, "handshake capacity or secure randomness unavailable", now); return nullptr; }
    notice.store(1);
    return p;
}
void observe_connection(uint64_t id, uint32_t connection) {
    if (!id || !connection) return;
    auto found = connections.find(id);
    if (found != connections.end() && found->second != connection) {
        discard(id); gate.peers.erase(id); found->second = connection;
    } else if (found == connections.end() && gate.peers.contains(id) && connections.size() < swml_gate::max_peers) {
        connections.emplace(id, connection);
    }
}
void send_control(uint64_t id, const swml_gate::Packet& packet) {
    const auto bytes = swml_gate::encode(packet);
    const auto identity = identity_for(id);
    original_send(messages, &identity, bytes.data(), uint32_t(bytes.size()), 8, swml_gate::channel);
}
void pump(uint64_t now, bool force = true) {
    if (stopped || !bound || !messages) return;
    if (!force && have_pumped && now - last_pump < 10) return;
    last_pump = now; have_pumped = true;
    // Bounded draining: a noisy peer cannot monopolize a frame/network call.
    for (int batch = 0; batch < 4; ++batch) {
        Message* incoming[16]{};
        const int count = original_receive(messages, swml_gate::channel, incoming, 16);
        if (count <= 0) break;
        for (int i = 0; i < std::min(count, 16); ++i) {
            auto* m = incoming[i];
            if (!m) continue;
            const auto id = key(&m->identity);
            swml_gate::Packet packet;
            if (id && m->size >= 0 && swml_gate::decode(m->data, size_t(m->size), packet)) {
                observe_connection(id, m->connection);
                auto* peer = touch(id, now);
                if (peer && peer->state != swml_gate::State::rejected) {
                    observe_connection(id, m->connection);
                    const auto before = peer->state;
                    // Fresh randomness is needed when a peer advertises a new session challenge.
                    const auto fresh = swml_gate::nonzero(peer->remote) && peer->remote != packet.nonce
                        ? random_nonce() : peer->local;
                    if (!swml_gate::nonzero(fresh)) reject(id, "secure challenge unavailable", now);
                    else gate.receive(id, packet, now, fresh);
                    if (peer->state == swml_gate::State::rejected)
                        reject(id, "incompatible or expired handshake", now);
                    else if (before != swml_gate::State::allowed && peer->state == swml_gate::State::allowed)
                        corelog("multiplayer: peer completed SWML protocol %u handshake", swml_gate::protocol);
                }
            }
            release_message(m);
        }
        if (count < 16) break;
    }
    for (auto it = gate.peers.begin(); it != gate.peers.end();) {
        const auto id = it->first;
        auto& p = it->second;
        if (p.state == swml_gate::State::pending && now - p.started >= swml_gate::timeout_ms)
            reject(id, "no SWML handshake within 10 seconds", now);
        if ((p.state == swml_gate::State::rejected && now - p.started >= swml_gate::timeout_ms + swml_gate::cooldown_ms) ||
            (p.state == swml_gate::State::allowed && now - p.activity >= swml_gate::idle_ms)) {
            discard(id); connections.erase(id); it = gate.peers.erase(it); continue;
        }
        if (p.state == swml_gate::State::allowed && session_info && now - p.last_session_check >= 100 &&
            session_check(id) != 3) {
            discard(id); connections.erase(id); it = gate.peers.erase(it); continue;
        }
        if (p.state == swml_gate::State::allowed && now - p.last_session_check >= 100) p.last_session_check = now;
        if (p.state != swml_gate::State::rejected) {
            if (p.state == swml_gate::State::pending && (!p.sent_hello || now - p.last_hello >= swml_gate::retry_ms)) {
                send_control(id, {swml_gate::protocol, swml_gate::Kind::hello, p.local, {}});
                p.sent_hello = true; p.last_hello = now;
            }
            if (p.answer_needed && (!p.sent_answer || now - p.last_answer >= 100)) {
                send_control(id, {swml_gate::protocol, swml_gate::Kind::answer, p.local, p.remote});
                p.sent_answer = true; p.last_answer = now; p.answer_needed = false;
            }
        }
        ++it;
    }
    bool pending = false;
    for (const auto& [id, p] : gate.peers) pending |= p.state == swml_gate::State::pending;
    if (pending) notice.store(1);
    else if (!rejection_time || now - rejection_time >= swml_gate::cooldown_ms) notice.store(0);
}
// The hooks are only reachable through the Messages object's own vptr, so self is that object; the checks
// below stay as a guard against a stale table after shutdown.
int __fastcall send_hook(void* self, const Identity* identity, const void* data, uint32_t size, int flags, int channel) {
    std::lock_guard lock(mutex);
    if (stopped || !bound || self != messages) return result_no_connection;
    // The reserved channel is never available to the game or external mods via this interface.
    if (channel == swml_gate::channel) return result_no_connection;
    const auto now = GetTickCount64(); pump(now, false);
    const auto id = key(identity);
    if (!id) return result_no_connection;
    auto* p = touch(id, now);
    if (!p || p->state == swml_gate::State::rejected) return result_no_connection;
    if (!gate.allowed(id)) { pump(now, !p->sent_hello); return result_limit; } // Game retains/retries its own packet.
    p->activity = now;
    return original_send(self, identity, data, size, flags, channel);
}
int __fastcall receive_hook(void* self, int channel, Message** out, int capacity) {
    std::lock_guard lock(mutex);
    if (stopped || !bound || self != messages || !out || capacity <= 0 || channel == swml_gate::channel) return 0;
    const auto now = GetTickCount64(); pump(now, false);
    int delivered = 0;
    for (auto it = held.begin(); it != held.end() && delivered < capacity;) {
        if (it->message->channel != channel || !gate.allowed(it->peer)) { ++it; continue; }
        out[delivered++] = it->message;
        gate.peers[it->peer].activity = now;
        held_bytes -= size_t(it->message->size); it = held.erase(it);
    }
    if (delivered == capacity) return delivered;
    // Bound native work independently of the caller's capacity.
    Message* incoming[32]{};
    const int requested = std::min(capacity - delivered, 32);
    const int count = original_receive(self, channel, incoming, requested);
    for (int i = 0; i < std::clamp(count, 0, requested); ++i) {
        auto* m = incoming[i];
        if (!m) continue;
        const auto id = key(&m->identity);
        if (!id || m->size < 0 || !m->release) { release_message(m); continue; }
        observe_connection(id, m->connection);
        auto* p = touch(id, now);
        if (!p || p->state == swml_gate::State::rejected) { release_message(m); continue; }
        observe_connection(id, m->connection);
        if (gate.allowed(id)) { p->activity = now; out[delivered++] = m; }
        else if (held.size() < max_held_messages && size_t(m->size) <= max_held_bytes - held_bytes) {
            // Hold, never acknowledge to the game and discard: channels can race the final answer.
            held.push_back({m, id}); held_bytes += size_t(m->size);
        } else { release_message(m); reject(id, "pre-handshake message budget exceeded", now); }
    }
    pump(now, false);
    return delivered;
}
bool __fastcall accept_hook(void* self, const Identity* identity) {
    std::lock_guard lock(mutex);
    if (stopped || !bound || self != messages) return false;
    const auto id = key(identity);
    // Accept is the game's session-request boundary, including reconnections.
    // Require a fresh proof even when the SteamID was authorized in an older session.
    discard(id); connections.erase(id); gate.peers.erase(id);
    if (!id || !touch(id, GetTickCount64()) || gate.peers[id].state == swml_gate::State::rejected) return false;
    // Steam must accept the transport to receive our control channel. Gameplay remains gated.
    const bool accepted = original_accept(self, identity);
    pump(GetTickCount64(), true); return accepted;
}
bool __fastcall close_hook(void* self, const Identity* identity) {
    std::lock_guard lock(mutex);
    if (stopped || !original_close) return false;
    if (self == messages) { const auto id = key(identity); if (id) { discard(id); connections.erase(id); gate.peers.erase(id); } }
    return original_close(self, identity);
}
bool readable(const void* p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return static_cast<const uint8_t*>(p) + bytes <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}
// Gives the object a copy of its current vtable with the gate in slots 0..3; the replaced entries become the
// originals. Copying the current table (not Steam's) keeps any other wrapper, e.g. network_fixes, in the chain.
bool wrap(void* object) {
    auto** current = *static_cast<void***>(object);
    if (tables_used == max_tables || !readable(current, sizeof(void*) * vtable_slots)) return false;
    auto* table = tables[tables_used++];
    std::memcpy(table, current, sizeof(void*) * vtable_slots);
    original_send = reinterpret_cast<Send>(current[0]);
    original_receive = reinterpret_cast<Receive>(current[1]);
    original_accept = reinterpret_cast<Accept>(current[2]);
    original_close = reinterpret_cast<Close>(current[3]);
    session_info = reinterpret_cast<Info>(current[5]);
    table[0] = reinterpret_cast<void*>(&send_hook);
    table[1] = reinterpret_cast<void*>(&receive_hook);
    table[2] = reinterpret_cast<void*>(&accept_hook);
    table[3] = reinterpret_cast<void*>(&close_hook);
    active_table = table;
    InterlockedExchangePointer(static_cast<void**>(object), table);
    return true;
}
// Another vptr swap that copied the table before ours drops the gate; slot 3 is ours alone, so it tells.
void keep_wrapped() {
    auto** current = *static_cast<void***>(messages);
    if (current == active_table || (readable(current, sizeof(void*) * 4) && current[3] == reinterpret_cast<void*>(&close_hook)))
        return;
    if (wrap(messages)) {
        if (!rewrap_reported) corelog("multiplayer: another vtable replacement dropped the gate; wrapped it again");
        rewrap_reported = true;
    } else {
        corelog("multiplayer: the gate was replaced and cannot be restored; multiplayer protection unavailable");
        bind_failed = true; bound = false; notice.store(3); swml_health::note_fault();
    }
}
bool bind(void* object) {
    if (!object || stopped || bind_failed) return false;
    if (bound) return object == messages;
    messages = object;
    bound = wrap(object);
    if (!bound) {
        messages = nullptr;
        corelog("multiplayer: Steam Messages vtable unreadable; multiplayer protection unavailable");
        bind_failed = true; notice.store(3); swml_health::note_fault();
    }
    return bound;
}
void* __cdecl get_hook() {
    std::lock_guard lock(mutex);
    if (stopped) return nullptr;
    auto* object = original_get();
    last_bind_attempt = GetTickCount64();
    return bind(object) ? object : nullptr;
}
void __cdecl shutdown_hook() {
    {
        std::lock_guard lock(mutex);
        stopped = true;
        for (auto& entry : held) release_message(entry.message);
        held.clear(); held_bytes = 0; gate.peers.clear(); connections.clear(); messages = nullptr;
    }
    original_shutdown();
}
}

bool core_multiplayer_install() {
    std::lock_guard lock(mutex);
    if (installed) return !bind_failed;
    const auto api = GetModuleHandleW(L"steam_api64.dll");
    const auto get = api ? GetProcAddress(api, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002") : nullptr;
    const auto shutdown = api ? GetProcAddress(api, "SteamAPI_Shutdown") : nullptr;
    installed = get && shutdown &&
        core_internal_hook(reinterpret_cast<void*>(get), reinterpret_cast<void*>(&get_hook), reinterpret_cast<void**>(&original_get), "SWML Steam Messages accessor") &&
        core_internal_hook(reinterpret_cast<void*>(shutdown), reinterpret_cast<void*>(&shutdown_hook), reinterpret_cast<void**>(&original_shutdown), "SWML multiplayer shutdown");
    if (!installed) { notice.store(3); return false; }
    auto* object = original_get();
    if (object && !bind(object)) return false;
    corelog("multiplayer: SWML protocol %u required; gameplay held until fresh challenge succeeds", swml_gate::protocol);
    return true;
}
void core_multiplayer_tick() {
    std::lock_guard lock(mutex);
    if (!installed || stopped || bind_failed) return;
    const auto now = GetTickCount64();
    if (!bound && now - last_bind_attempt >= 500) {
        last_bind_attempt = now;
        auto* object = original_get(); if (object) bind(object);
    }
    if (bound) keep_wrapped();
    pump(now, false);
}
const char* core_multiplayer_notice() {
    switch (notice.load()) {
    case 1: return "multiplayer: waiting for SWML handshake";
    case 2: return "multiplayer blocked: compatible SWML required";
    case 3: return "multiplayer protection unavailable";
    default: return "";
    }
}
