// Game launch + core loading, Windows / Wine / Proton only. Extracted from the old Win32
// launcher so the new UI (and the headless --run mode used by swml_proton.sh) share one code
// path. Two load methods:
//  - SWML_STARTUP_LOADER: the game loads our opengl32 proxy, which starts the core in-process
//    after wglMakeCurrent (no remote process access).
//  - otherwise: wait for the game's renderer (opengl32.dll), then inject swml_core.dll.
#include "runner.h"
#include "platform.h"
#include "mod_manifest.h"
#ifdef SWML_STARTUP_LOADER
#include "../bootstrap/launch.h"
#endif

#include <windows.h>
#include <share.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwchar>
#include <filesystem>

namespace swml_runner {
namespace {
namespace fs = std::filesystem;

std::wstring stormworks_mods_dir()
{
    wchar_t* roaming = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) r = std::wstring(roaming) + L"\\Stormworks\\data\\mods";
    CoTaskMemFree(roaming);
    return r;
}

// Each mod's Stormworks data mod is installed as <Stormworks mods>\<id> while it is enabled.
// Saves remember the full path of every data mod they use (scene.xml <mod_path>), so the folder
// name must stay the same forever, or worlds that use the mod silently fall back to vanilla.
void sync_game_data(bool with_mods, const std::vector<GameMod>& mods)
{
    std::wstring sw = stormworks_mods_dir();
    if (sw.empty()) return;
    SHCreateDirectoryExW(nullptr, sw.c_str(), nullptr);
    for (const GameMod& m : mods) {
        if (m.game_data.empty()) continue;
        std::wstring target = sw + L"\\" + m.id, src = m.dir + L"\\" + m.game_data;
        std::wstring legacy = sw + L"\\swml_" + m.id; // first loader version used this name
        if (swml_platform::path_exists(legacy)) swml_platform::remove_tree(legacy, false);
        if (swml_platform::path_exists(target)) swml_platform::remove_tree(target, false);
        if (with_mods && m.enabled && m.problem.empty() && swml_platform::path_exists(src)) swml_platform::copy_tree(src, target);
    }
}

#ifndef SWML_STARTUP_LOADER
bool has_module(DWORD pid, const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) found = _wcsicmp(me.szModule, name) == 0;
    CloseHandle(snap);
    return found;
}

bool inject(HANDLE proc, const std::wstring& dll)
{
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);
    if (ok) {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE t = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr);
        ok = t != nullptr;
        if (t) {
            WaitForSingleObject(t, 30000);
            DWORD code = 0;
            GetExitCodeThread(t, &code);
            ok = code != 0;
            CloseHandle(t);
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}
#endif

// First token of a command line (the game exe) -> its folder.
std::wstring exe_dir_of(const std::wstring& cmd)
{
    std::wstring exe;
    size_t i = 0;
    while (i < cmd.size() && cmd[i] == L' ') i++;
    if (i < cmd.size() && cmd[i] == L'"') {
        size_t e = cmd.find(L'"', i + 1);
        exe = cmd.substr(i + 1, e == std::wstring::npos ? std::wstring::npos : e - i - 1);
    } else {
        size_t e = cmd.find(L' ', i);
        exe = cmd.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i);
    }
    size_t s = exe.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : exe.substr(0, s);
}
} // namespace

void log(const std::wstring& root, const std::wstring& message)
{
    CreateDirectoryW((root + L"\\logs").c_str(), nullptr);
    FILE* f = _wfsopen((root + L"\\logs\\launcher.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, message.c_str());
    fclose(f);
}

Result run_game(const std::wstring& root, const std::wstring& game_cmd, bool with_mods,
                const std::vector<GameMod>& mods)
{
    Result result;
#ifdef SWML_STARTUP_LOADER
    with_mods = with_mods && swml_bootstrap::environment(L"SWML_OFF") != L"1";
    int game_argc = 0;
    auto game_argv = CommandLineToArgvW(game_cmd.c_str(), &game_argc);
    bool supported_client = game_argv && game_argc > 0 && !_wcsicmp(fs::path(game_argv[0]).filename().c_str(), L"stormworks64.exe");
    if (game_argv) LocalFree(game_argv);
    if (with_mods && !supported_client) { result.error = L"Choose the Stormworks 64-bit client."; return result; }
    const auto game_dir = exe_dir_of(game_cmd);
    if (with_mods && (!swml_bootstrap::exported(game_dir + L"\\opengl32.dll", "swml_bootstrap_version") ||
                      !swml_bootstrap::exported(root + L"\\swml_core.dll", "swml_bootstrap_start_v1"))) {
        result.error = L"The matching startup loader is not installed. Run install.bat from the extracted SWML release and select this game's folder.";
        return result;
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return result;
    wchar_t guid_text[64]{};
    StringFromGUID2(guid, guid_text, 64);
    std::wstring event_name = L"Local\\SWML-startup-" + std::wstring(guid_text);
    struct Event { HANDLE handle{}; ~Event() { if (handle) CloseHandle(handle); } } ready_event;
    if (with_mods) { ready_event.handle = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()); if (!ready_event.handle) return result; }
    std::vector<wchar_t> child;
    try { child = swml_bootstrap::child_environment(root, with_mods, event_name); }
    catch (...) { result.error = L"Could not prepare the game's startup settings."; return result; }
#endif
    sync_game_data(with_mods, mods);
    std::wstring cmd = game_cmd, dir = exe_dir_of(game_cmd);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    log(root, std::wstring(L"starting (") + (with_mods ? L"with mods" : L"without mods") + L"): " + cmd);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
#ifdef SWML_STARTUP_LOADER
        CREATE_UNICODE_ENVIRONMENT, child.data(),
#else
        0, nullptr,
#endif
        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        log(root, L"could not start the game: error " + std::to_wstring(e));
        result.error = L"Could not start the game (error " + std::to_wstring(e) + L").\n\n" + cmd;
        return result;
    }
    if (with_mods) {
#ifdef SWML_STARTUP_LOADER
        HANDLE waiting[] = {ready_event.handle, pi.hProcess};
        DWORD wait = WaitForMultipleObjects(2, waiting, FALSE, 30000);
        if (wait == WAIT_OBJECT_0) log(root, L"startup core ready (in-process bootstrap)");
        else if (wait == WAIT_OBJECT_0 + 1) log(root, L"game exited before startup core became ready");
        else log(root, L"startup core did not report ready; inspect bootstrap.log and swml.log");
#else
        std::wstring core = root + L"\\swml_core.dll";
        // Wait for the renderer, like swinject --launch: early enough for every mod, after the
        // CRT, Steam and the window exist.
        bool ready = false;
        for (int i = 0; i < 480 && !ready; i++) {
            if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) break;
            ready = has_module(pi.dwProcessId, L"opengl32.dll");
        }
        if (ready) {
            bool ok = inject(pi.hProcess, core);
            log(root, ok ? L"core loaded" : L"core FAILED to load");
        } else {
            log(root, L"game exited or never initialised OpenGL; mods not loaded");
        }
#endif
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    log(root, L"game exited with code 0x" + std::to_wstring(code));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    result.code = int(code);
    return result;
}
} // namespace swml_runner
