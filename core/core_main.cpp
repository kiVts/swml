// swml_core.dll: loaded into the game by swml.exe. Sets up logging, crash capture and the shared
// event hooks, then loads every enabled mod from <root>\mods\<id>\ and calls its swml_mod_init.
//
// Root = the folder this DLL is in (%LOCALAPPDATA%\swml). Enabled mods: swml.ini [mods] <id>=1|0,
// or the mod's default_enabled when it isn't listed.
//
// Logs: logs\swml.log is the loader's own; each enabled mod with a DLL writes logs\mods\<id>.log. Five sessions each.

#include "core.h"
#include "settings_store.h"
#include "core_v2.h"
#include "core_v3.h"
#include "core_v4.h"
#include "log_sessions.h"
#include "health.h"
#include "status_display.h"

#include "game.h"
#include "log.h"
#include "mod_dependencies.h"
#include "mod_manifest.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace {

HMODULE g_self = nullptr;
std::wstring g_root, g_logs, g_mods_dir, g_config;
std::mutex g_log_mu;
FILE* g_log = nullptr;
std::map<std::string, FILE*> g_mod_logs; // by mod id, guarded by g_log_mu
std::vector<swml_mod*> g_mods;
uintptr_t g_base = 0;
bool g_build_known = false;
std::atomic<bool> g_enabled_all{false};
std::mutex g_hook_mu;

// Several mods may hook the same game function. MinHook can hook a target only once, so each target gets one
// MinHook hook to a stub, "jmp [entry]", and the mods' detours form a chain behind it: entry = the newest detour,
// each detour's *original = the one hooked before it, the oldest's = MinHook's trampoline (the game code). The
// newest mod runs first and decides whether to call the rest. Links are only ever re-pointed with single
// 8-byte stores, so a thread already inside the chain finishes on the old path.
struct HookLink {
    swml_mod* mod;
    void* detour;
    void** original; // the mod's variable: what its detour calls next
};
struct HookChain {
    void* target;
    void* trampoline = nullptr;
    uint8_t* stub = nullptr; // jmp qword ptr [rip+0] ; dq entry
    std::vector<HookLink> links; // oldest first
    void* volatile* entry() const { return reinterpret_cast<void* volatile*>(stub + 8); }
};
std::vector<HookChain*> g_chains;

HookChain* find_chain(void* target)
{
    for (HookChain* c : g_chains)
        if (c->target == target) return c;
    return nullptr;
}

struct KnownBuild {
    const wchar_t* exe;
    uint32_t timestamp, size_of_image;
};
constexpr KnownBuild kKnownBuilds[] = {
    {L"stormworks64.exe", 0x6a9a9e20, 0xd8b000},
};

std::string narrow(const std::wstring& w)
{
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, result.data(), needed, nullptr, nullptr);
    result.resize(static_cast<size_t>(needed - 1)); return result;
}

// --- API implementation ----------------------------------------------------------------------
void api_log(swml_mod* m, const char* msg) { core_log(m ? m->id.c_str() : "?", msg); }
const wchar_t* api_mod_dir(swml_mod* m) { return m->dir.c_str(); }
const wchar_t* api_logs_dir() { return g_logs.c_str(); }
const wchar_t* api_settings(swml_mod* m) { return m->settings.c_str(); }
uintptr_t api_game_base() { return g_base; }
int api_build_known() { return g_build_known ? 1 : 0; }

int api_hook(swml_mod* m, void* target, void* detour, void** original, const char* what)
{
    std::lock_guard<std::mutex> lock(g_hook_mu);
    char msg[256];
    HookChain* c = find_chain(target);
    if (!c) {
        auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stub) {
            snprintf(msg, sizeof(msg), "hook %s failed: no memory for the stub", what ? what : "?");
            api_log(m, msg);
            return 0;
        }
        // jmp qword ptr [rip+2] (rip = stub+6, so it reads stub+8: entry, 8-aligned for atomic stores); int3 padding.
        static const uint8_t kJmp[8] = {0xff, 0x25, 0x02, 0x00, 0x00, 0x00, 0xcc, 0xcc};
        memcpy(stub, kJmp, sizeof(kJmp));
        void* tramp = nullptr;
        MH_STATUS st = MH_CreateHook(target, stub, &tramp);
        if (st != MH_OK) {
            VirtualFree(stub, 0, MEM_RELEASE);
            snprintf(msg, sizeof(msg), "hook %s failed: %s", what ? what : "?", MH_StatusToString(st));
            api_log(m, msg);
            return 0;
        }
        c = new HookChain{target};
        c->trampoline = tramp;
        c->stub = stub;
        *c->entry() = tramp;
        g_chains.push_back(c);
    } else {
        snprintf(msg, sizeof(msg), "hook %s: also hooked by mod '%s', chained (this mod's detour runs first)",
                 what ? what : "?", c->links.back().mod->id.c_str());
        api_log(m, msg);
    }
    *original = *c->entry();             // next in line: the previous newest detour, or the game code
    MemoryBarrier();
    *c->entry() = detour;                // publish
    c->links.push_back({m, detour, original});
    m->hooks.push_back(target);
    if (g_enabled_all && c->links.size() == 1) MH_EnableHook(target); // created after start-up (e.g. once Steam is ready)
    return 1;
}

// Takes one mod's link out of a target's chain; the last one out removes the MinHook hook.
void unhook(swml_mod* m, void* target)
{
    HookChain* c = find_chain(target);
    if (!c) return;
    for (size_t i = 0; i < c->links.size(); i++) {
        if (c->links[i].mod != m) continue;
        void* next = *c->links[i].original;
        if (i + 1 < c->links.size()) *c->links[i + 1].original = next; // the newer detour now skips this one
        else *c->entry() = next;
        c->links.erase(c->links.begin() + i);
        break;
    }
    if (c->links.empty()) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        // The stub stays allocated: a thread may still be on its way through it.
        g_chains.erase(std::remove(g_chains.begin(), g_chains.end(), c), g_chains.end());
        delete c;
    }
}

int api_hook_export(swml_mod* m, const wchar_t* module, const char* proc, void* detour, void** original)
{
    HMODULE h = GetModuleHandleW(module);
    if (!h) h = LoadLibraryW(module);
    void* target = h ? reinterpret_cast<void*>(GetProcAddress(h, proc)) : nullptr;
    if (!target) {
        char msg[256];
        snprintf(msg, sizeof(msg), "hook %ls!%s failed: not found", module, proc);
        api_log(m, msg);
        return 0;
    }
    char what[160];
    snprintf(what, sizeof(what), "%ls!%s", module, proc);
    return api_hook(m, target, detour, original, what);
}

void api_on_present(swml_mod* m, swml_present_cb cb, void* ctx) { core_on_present(m, cb, ctx); }
void api_on_frame(swml_mod* m, swml_frame_cb cb, void* ctx) { core_on_frame(m, cb, ctx); }
int api_on_loop(swml_mod* m, swml_loop_begin_cb b, swml_loop_end_cb e, swml_loop_idle_cb i, void* ctx)
{
    return core_on_loop(m, b, e, i, ctx) ? 1 : 0;
}
void api_set_status(swml_mod* m, const char* text)
{
    if (text && swml_health::bad_status(text)) swml_health::note_fault();
    m->status = text ? text : "";
    core_write_status(m);
}

const swml_api g_api = {
    SWML_API_VERSION, sizeof(swml_api), &api_log,         &api_mod_dir,    &api_logs_dir, &api_settings,
    &api_game_base,   &api_build_known, &api_hook,        &api_hook_export, &api_on_present, &api_on_frame,
    &api_on_loop,     &api_set_status,
    core_v2_api(),
    core_v3_api(),
    core_v4_api(),
    swml_health::snapshot,
    [](swml_mod*) { swml_health::note_fault(); },
};

// --- start-up ----------------------------------------------------------------------------------
void game_init()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    g_base = reinterpret_cast<uintptr_t>(exe);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(exe, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    for (const KnownBuild& b : kKnownBuilds) {
        if (_wcsicmp(name, b.exe) == 0 && nt->FileHeader.TimeDateStamp == b.timestamp && nt->OptionalHeader.SizeOfImage == b.size_of_image)
            g_build_known = true;
    }
    corelog("game: %ls timestamp=0x%08x size=0x%x -> %s", name, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
            g_build_known ? "known build" : "UNKNOWN build (game updated?): mods using fixed addresses switch those parts off");
}

void ensure_settings(swml_mod* m)
{
    swml_settings_store::prepare(m->dir);
}

bool mod_enabled(const std::wstring& id, const std::wstring& manifest)
{
    // [profile] default_disabled: written by install_linux.sh on a fresh Linux install, unlisted mods stay off there.
    int def = GetPrivateProfileIntW(L"profile",L"default_disabled",0,g_config.c_str())?0:GetPrivateProfileIntW(L"mod", L"default_enabled", 1, manifest.c_str());
    return GetPrivateProfileIntW(L"mods", id.c_str(), def, g_config.c_str()) != 0;
}

// SEH can't share a function with C++ objects that need unwinding.
int call_init(swml_mod_init_fn init, swml_mod* m, bool* crashed)
{
    __try {
        return init(&g_api, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return 0;
    }
}

void unload(swml_mod* m)
{
    core_v4_drop(m);
    core_v3_drop(m);
    core_v2_drop(m);
    {
        std::lock_guard<std::mutex> lock(g_hook_mu);
        for (void* t : m->hooks) unhook(m, t);
        m->hooks.clear();
    }
    core_events_drop(m);
    // The DLL stays loaded: a thread it started may still be running; with no hooks or callbacks
    // left it does nothing.
}

std::string session_line()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char line[64];
    snprintf(line, sizeof(line), "session started %04d-%02d-%02d %02d:%02d:%02d local", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return line;
}

// If the file can't be opened the mod's lines stay in swml.log.
void open_mod_log(const swml_mod* m, const std::wstring& id)
{
    auto session = swml_logs::open_session(g_logs + L"\\mods", id);
    if (!session.warning.empty()) corelog("mod %s: log history: %s", m->id.c_str(), session.warning.c_str());
    if (!session.file) return;
    {
        std::lock_guard<std::mutex> lock(g_log_mu);
        g_mod_logs[m->id] = session.file;
    }
    core_log(m->id.c_str(), session_line().c_str());
}

void load_mods()
{
    g_config = g_root + L"\\swml.ini";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_mods_dir + L"\\*").c_str(), &fd);
    std::vector<std::wstring> ids;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                if (!_wcsicmp(fd.cFileName, L"mod_status")) {
                    corelog("mod_status is built into SWML; ignoring the legacy standalone mod");
                    continue;
                }
                ids.push_back(fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<swml_dependencies::Mod> dependency_input;
    std::map<std::string, std::wstring> native_ids;
    for (const auto& id : ids) {
        std::wstring manifest = g_mods_dir + L"\\" + id + L"\\mod.ini";
        if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        auto name = narrow(id); native_ids[name] = id;
        try { dependency_input.push_back(swml_manifest::inspect(g_mods_dir + L"\\" + id, id, g_config)); }
        catch (const std::exception& e) { swml_dependencies::Mod bad{name}; bad.problem = e.what(); dependency_input.push_back(std::move(bad)); }
    }
    auto dependency_plan = swml_dependencies::plan(dependency_input);
    std::vector<std::wstring> ordered_ids;
    for (const auto& name : dependency_plan.order) ordered_ids.push_back(native_ids[name]);
    for (const auto& id : ids) if (std::find(ordered_ids.begin(), ordered_ids.end(), id) == ordered_ids.end()) ordered_ids.push_back(id);
    for (const std::wstring& id : ordered_ids) {
        std::wstring dir = g_mods_dir + L"\\" + id, manifest = dir + L"\\mod.ini";
        if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        auto* m = new swml_mod();
        m->id = narrow(id);
        m->dir = dir;
        m->settings = swml_settings_store::effective(dir);
        g_mods.push_back(m);
        if (!mod_enabled(id, manifest)) {
            m->status = "disabled";
            core_write_status(m);
            continue;
        }
        uint32_t required_api = GetPrivateProfileIntW(L"mod", L"api", 1, manifest.c_str());
        if (required_api > SWML_API_VERSION) {
            m->status = "failed: requires a newer SWML core API";
            core_write_status(m);
            continue;
        }
        auto dependency_error = dependency_plan.blocked.find(m->id);
        if (dependency_error != dependency_plan.blocked.end()) {
            m->status = "failed: " + dependency_error->second;
            core_write_status(m);
            continue;
        }
        bool dependency_failed = false;
        for (const auto& descriptor : dependency_input) if (descriptor.id == m->id) {
            for (const auto& requirement : descriptor.required) {
                auto name = swml_version::requirement(requirement).first;
                auto found = std::find_if(g_mods.begin(), g_mods.end(), [&](const auto* loaded) { return loaded->id == name; });
                if (found == g_mods.end() || !(*found)->operational) {
                    m->status = "failed: required dependency did not initialize: " + name; dependency_failed = true; break;
                }
            }
        }
        if (dependency_failed) { core_write_status(m); continue; }
        wchar_t dll[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"mod", L"dll", L"", dll, MAX_PATH, manifest.c_str());
        if (!dll[0]) { // data-only mod: the launcher installs its game data
            m->operational = true;
            m->status = "OK (game data only)";
            core_write_status(m);
            continue;
        }
        try {ensure_settings(m);} catch(const std::exception& error) {
            m->status=std::string("failed: settings: ")+error.what();core_write_status(m);continue;
        }
        open_mod_log(m, id);
        HMODULE mod = LoadLibraryExW((dir + L"\\" + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            corelog("mod %s: could not load %ls (error %lu)", m->id.c_str(), dll, GetLastError());
            m->status = "failed: DLL could not be loaded";
            core_write_status(m);
            continue;
        }
        m->module = mod;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(mod) + dos->e_lfanew);
        m->image_begin = reinterpret_cast<uintptr_t>(mod);
        m->image_end = m->image_begin + nt->OptionalHeader.SizeOfImage;
        auto init = reinterpret_cast<swml_mod_init_fn>(GetProcAddress(mod, SWML_MOD_INIT_NAME));
        if (!init) {
            m->status = "failed: not a swml mod (no swml_mod_init)";
            core_write_status(m);
            continue;
        }
        m->status = "starting";
        core_v2_attach(m);
        core_v3_attach(m);
        if (!core_v4_attach(m)) { unload(m);core_write_status(m);continue; }
        bool crashed = false;
        int ok = call_init(init, m, &crashed);
        if (crashed) {
            corelog("mod %s: crashed during start-up", m->id.c_str());
            m->status = "failed: crashed during start-up";
        }
        if (!ok) {
            unload(m);
            if (m->status == "starting" || m->status == "OK") m->status = "failed to start (see swml.log)";
            core_write_status(m);
        }
        m->operational = ok != 0;
        corelog("mod %s: %s", m->id.c_str(), m->status.c_str());
    }
}

DWORD WINAPI init_thread(void*)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    g_root = path;
    g_root.resize(g_root.find_last_of(L'\\'));
    g_logs = g_root + L"\\logs";
    g_mods_dir = g_root + L"\\mods";
    auto session = swml_logs::open_session(g_logs);
    g_log = session.file;
    corelog("swml core loaded (built " __DATE__ " " __TIME__ ")");
    corelog("%s", session_line().c_str());
    if (!session.warning.empty()) {
        corelog("log history: %s", session.warning.c_str());
        if (!g_log) OutputDebugStringA(("SWML log history: " + session.warning + "\n").c_str());
    }
    game_init();
    if (MH_STATUS st = MH_Initialize(); st != MH_OK) {
        corelog("MH_Initialize failed: %s", MH_StatusToString(st));
        return 0;
    }
    core_crash_install();
    core_events_install();
    if (!core_status_install()) swml_health::note_fault();
    load_mods();
    uint32_t loaded = 0;
    for (const auto* m : g_mods) {
        loaded += m->operational;
        if ((!m->operational && m->status != "disabled") || swml_health::bad_status(m->status))
            swml_health::note_fault();
    }
    swml_health::loaded.store(loaded, std::memory_order_relaxed);
    core_v2_ready();
    if (MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS); st != MH_OK) {
        swml_health::note_fault();
        corelog("MH_EnableHook failed: %s", MH_StatusToString(st));
    }
    g_enabled_all = true;
    swml_health::ready.store(true, std::memory_order_release);
    int running = 0;
    for (auto* m : g_mods) running += m->module && !m->hooks.empty();
    corelog("start-up done: %zu mods found, %d running with hooks", g_mods.size(), running);
#ifdef SWML_STARTUP_LOADER
    wchar_t event_name[256]{};
    DWORD n=GetEnvironmentVariableW(L"SWML_READY_EVENT",event_name,256);
    if(n&&n<256)if(HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,event_name)){SetEvent(event);CloseHandle(event);}
#endif
    return 0;
}

} // namespace

// --- core-wide helpers -------------------------------------------------------------------------
const std::wstring& core_root() { return g_root; }
bool core_internal_hook(void* target, void* detour, void** original, const char* name) {
    static swml_mod owner = [] { swml_mod m; m.id = "swml.ui"; return m; }();
    return api_hook(&owner, target, detour, original, name) != 0;
}
const std::wstring& core_logs() { return g_logs; }
std::vector<swml_mod*>& core_mods() { return g_mods; }

void core_log(const char* tag, const char* msg)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_log_mu);
    auto own = g_mod_logs.find(tag);
    FILE* out = own != g_mod_logs.end() ? own->second : g_log;
    if (out) {
        fprintf(out, "%02d:%02d:%02d.%03d [%5lu] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), tag, msg);
        fflush(out);
    }
}

void corelog(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    core_log("core", msg);
}

swml_mod* core_mod_at(uintptr_t a)
{
    for (auto* m : g_mods)
        if (a >= m->image_begin && a < m->image_end) return m;
    return nullptr;
}

void core_write_status(swml_mod* m)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t when[64], text[600];
    swprintf(when, 64, L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    MultiByteToWideChar(CP_UTF8, 0, m->status.c_str(), -1, text, 600);
    std::wstring f = m->dir + L"\\status.ini";
    WritePrivateProfileStringW(L"status", L"text", text, f.c_str());
    WritePrivateProfileStringW(L"status", L"time", when, f.c_str());
}

// The core's own code (resolve.cpp from the SDK) logs through these, tagged "core".
void swlog(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    core_log("core", msg);
}
const wchar_t* log_dir() { return g_logs.c_str(); }
const wchar_t* settings_ini()
{
    static std::wstring ini = g_root + L"\\swml.ini";
    return ini.c_str();
}
const wchar_t* mod_dir() { return g_root.c_str(); }
bool game_build_matches() { return g_build_known; }
uintptr_t game_base() { return g_base; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
#ifndef SWML_STARTUP_LOADER
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) CloseHandle(t);
#endif
    }
    return TRUE;
}
#ifdef SWML_STARTUP_LOADER
extern "C" __declspec(dllexport) BOOL WINAPI swml_bootstrap_start_v1(uint32_t version)
{
    static volatile LONG started=0;
    if(version!=1||!g_self)return FALSE;
    if(InterlockedCompareExchange(&started,1,0)!=0)return TRUE;
    HANDLE worker=CreateThread(nullptr,0,init_thread,nullptr,0,nullptr);
    if(!worker){InterlockedExchange(&started,0);return FALSE;}CloseHandle(worker);return TRUE;
}
#endif
