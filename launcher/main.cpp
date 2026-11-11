// swml.exe - Stormworks mod loader: mod window + game launcher.
//
// Steam launch option: "<install dir>\swml.exe" %command%
// Steam then starts swml.exe with the game's own command line after ours. The window lists the
// mods in <install dir>\mods\<id>\ (mod.ini, README.md, settings.ini, status.ini); Play installs
// the enabled mods' Stormworks game data, starts the game with exactly Steam's command line, waits
// for its renderer and loads swml_core.dll into it, then stays in the background until the game
// exits so Steam sees the session end correctly. Started without a game command line it is a mod
// manager only.

#include "zip.h"
#include "management.h"
#include "field_dialog.h"
#include "settings_editor.h"
#include "settings_panel.h"
#include "mod_manifest.h"
#include "updates.h"
#include "swml_v2.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <share.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwchar>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "         \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// --- data ----------------------------------------------------------------------------------------
struct Mod {
    std::wstring id, dir, name, version, author, summary, multiplayer, game_data, dll, status, status_time, readme;
    std::wstring problem;
    bool enabled = true;
};

std::wstring g_root, g_mods_dir, g_ini, g_game_cmd;
std::vector<Mod> g_mods;

std::wstring ini_str(const std::wstring& file, const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"")
{
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec, key, def, buf, 2048, file.c_str());
    return buf;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring read_utf8(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return L"";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s = s.substr(3);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void load_mods()
{
    g_mods.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_mods_dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        Mod m;
        m.id = fd.cFileName;
        m.dir = g_mods_dir + L"\\" + m.id;
        std::wstring man = m.dir + L"\\mod.ini";
        if (!exists(man)) continue;
        m.name = ini_str(man, L"mod", L"name", m.id.c_str());
        m.version = ini_str(man, L"mod", L"version");
        m.author = ini_str(man, L"mod", L"author");
        m.summary = ini_str(man, L"mod", L"summary");
        m.multiplayer = ini_str(man, L"mod", L"multiplayer", L"local");
        m.game_data = ini_str(man, L"mod", L"game_data");
        m.dll = ini_str(man, L"mod", L"dll");
        int def = GetPrivateProfileIntW(L"mod", L"default_enabled", 1, man.c_str());
        m.enabled = GetPrivateProfileIntW(L"mods", m.id.c_str(), def, g_ini.c_str()) != 0;
        std::wstring st = m.dir + L"\\status.ini";
        m.status = ini_str(st, L"status", L"text", L"not run yet");
        m.status_time = ini_str(st, L"status", L"time");
        m.readme = read_utf8(m.dir + L"\\README.md");
        g_mods.push_back(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::vector<swml_dependencies::Mod> descriptors;
    for (auto& m : g_mods) {
        try { descriptors.push_back(swml_manifest::inspect(m.dir,m.id,g_ini)); }
        catch (const std::exception& e) { swml_dependencies::Mod bad{swml_manifest::utf8(m.id)}; bad.problem=e.what(); descriptors.push_back(std::move(bad)); }
    }
    auto plan=swml_dependencies::plan(descriptors);
    for (auto& m : g_mods) {
        const auto& descriptor=*std::find_if(descriptors.begin(),descriptors.end(),[&](const auto& d) { return d.id==swml_manifest::utf8(m.id); });
        m.enabled=descriptor.enabled;
        auto problem=plan.blocked.find(swml_manifest::utf8(m.id));
        if (problem!=plan.blocked.end()) m.problem=swml_manifest::wide(problem->second);
    }
}

void set_enabled(Mod& m, bool on)
{
    m.enabled = on;
    WritePrivateProfileStringW(L"mods", m.id.c_str(), on ? L"1" : L"0", g_ini.c_str());
}

// --- file helpers --------------------------------------------------------------------------------
bool shell_op(UINT op, const std::wstring& from, const std::wstring& to, FILEOP_FLAGS flags)
{
    std::wstring f = from + L'\0', t = to + L'\0';
    SHFILEOPSTRUCTW s{};
    s.wFunc = op;
    s.pFrom = f.c_str();
    s.pTo = to.empty() ? nullptr : t.c_str();
    s.fFlags = flags | FOF_NOCONFIRMMKDIR | FOF_SILENT;
    return SHFileOperationW(&s) == 0 && !s.fAnyOperationsAborted;
}

std::wstring stormworks_mods_dir()
{
    wchar_t* roaming = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) r = std::wstring(roaming) + L"\\Stormworks\\data\\mods";
    CoTaskMemFree(roaming);
    return r;
}

// Each mod's Stormworks data mod is installed as <Stormworks mods>\<id> while it is enabled. Saves
// remember the full path of every data mod they use (scene.xml <mod_path>), so the folder name must
// stay the same forever, or worlds that use the mod silently fall back to the vanilla definitions.
std::wstring game_data_target(const std::wstring& sw, const Mod& m) { return sw + L"\\" + m.id; }

void sync_game_data(bool with_mods)
{
    std::wstring sw = stormworks_mods_dir();
    if (sw.empty()) return;
    SHCreateDirectoryExW(nullptr, sw.c_str(), nullptr);
    for (const Mod& m : g_mods) {
        if (m.game_data.empty()) continue;
        std::wstring target = game_data_target(sw, m), src = m.dir + L"\\" + m.game_data;
        std::wstring legacy = sw + L"\\swml_" + m.id; // first loader version used this name
        if (exists(legacy)) shell_op(FO_DELETE, legacy, L"", FOF_NOCONFIRMATION | FOF_NOERRORUI);
        if (exists(target)) shell_op(FO_DELETE, target, L"", FOF_NOCONFIRMATION | FOF_NOERRORUI);
        if (with_mods && m.enabled && m.problem.empty() && exists(src)) shell_op(FO_COPY, src, target, FOF_NOCONFIRMATION | FOF_NOERRORUI);
    }
}

// --- game launch -----------------------------------------------------------------------------------
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

void log_launch(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list a;
    va_start(a, fmt);
    vswprintf(msg, 1024, fmt, a);
    va_end(a);
    CreateDirectoryW((g_root + L"\\logs").c_str(), nullptr);
    FILE* f = _wfsopen((g_root + L"\\logs\\launcher.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
    fclose(f);
}

// Returns the process exit code, or -1 if the game couldn't be started.
int run_game(bool with_mods, HWND wnd)
{
    sync_game_data(with_mods);
    std::wstring cmd = g_game_cmd, dir = exe_dir_of(g_game_cmd);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    log_launch(L"starting (%ls): %ls", with_mods ? L"with mods" : L"without mods", cmd.c_str());
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        log_launch(L"could not start the game: error %lu", e);
        wchar_t msg[512];
        swprintf(msg, 512, L"Could not start the game (error %lu).\n\n%ls", e, cmd.c_str());
        MessageBoxW(wnd, msg, L"Stormworks Mod Loader", MB_ICONERROR);
        return -1;
    }
    if (wnd) ShowWindow(wnd, SW_HIDE);
    if (with_mods) {
        std::wstring core = g_root + L"\\swml_core.dll";
        // Wait for the renderer, like swinject --launch: early enough for every mod, after the CRT,
        // Steam and the window exist.
        bool ready = false;
        for (int i = 0; i < 480 && !ready; i++) {
            if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) break;
            ready = has_module(pi.dwProcessId, L"opengl32.dll");
        }
        if (ready) {
            bool ok = inject(pi.hProcess, core);
            log_launch(L"core %ls", ok ? L"loaded" : L"FAILED to load");
        } else {
            log_launch(L"game exited or never initialised OpenGL; mods not loaded");
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    log_launch(L"game exited with code 0x%lx", code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// --- window ----------------------------------------------------------------------------------------
enum {
    IDC_LIST = 100, IDC_DETAILS, IDC_ADD, IDC_REMOVE, IDC_FOLDER, IDC_SETTINGS, IDC_LOGS, IDC_PLAY, IDC_PLAY_VANILLA, IDC_COPY,
    IDC_HINT,
    IDC_DOCTOR, IDC_ROLLBACK,
    IDC_CHECK_UPDATES, IDC_UPDATE, IDC_UPDATE_ALL, IDC_LOADER_NOTICE, IDC_UPDATE_STATUS,
};

constexpr UINT WM_SWML_WORK_DONE = WM_APP + 1;
struct WorkResult {
    bool check = false, have_index = false;
    swml_updates::Index index;
    std::wstring error, failures;
    int updated = 0;
};

swml_updates::Index g_index;
bool g_have_index = false, g_checking = false, g_installing = false;
HWND g_update_status;

HWND g_wnd, g_list, g_details, g_hint,g_detail_tabs,g_settings_panel;
std::wstring g_panel_directory,g_panel_version;
HWND g_buttons[9];
HFONT g_font, g_bold;
int g_dpi = 96;
bool g_filling = false;
bool g_launch_requested = false, g_launch_with_mods = true;

int S(int px) { return MulDiv(px, g_dpi, 96); }

const wchar_t* mp_text(const std::wstring& mp)
{
    if (mp == L"host") return L"the host needs it (other players don't)";
    if (mp == L"everyone") return L"every player in the session needs it";
    return L"only affects you (other players don't need it)";
}

int selected()
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return -1;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = i;
    ListView_GetItem(g_list, &it);
    return static_cast<int>(it.lParam);
}

const swml_updates::Entry* offered_update(const Mod& m)
{
    if (!g_have_index) return nullptr;
    try {
        auto found = g_index.mods.find(swml_manifest::utf8(m.id));
        if (found == g_index.mods.end() || !swml_updates::newer(found->second.version, swml_manifest::utf8(m.version))) return nullptr;
        return &found->second;
    } catch (const std::exception&) { return nullptr; }
}

bool can_update(const Mod& m)
{
    auto* entry = offered_update(m);
    return entry && entry->api <= SWML_API_VERSION;
}

void refresh_buttons()
{
    int i = selected();
    bool any = std::any_of(g_mods.begin(), g_mods.end(), can_update), idle = !g_installing;
    auto enable = [](int id, bool on) { EnableWindow(GetDlgItem(g_wnd, id), on); };
    for (int id : {IDC_REMOVE, IDC_SETTINGS}) enable(id, i >= 0 && idle);
    enable(IDC_FOLDER, i >= 0);
    for (int id : {IDC_ADD, IDC_ROLLBACK}) enable(id, idle);
    enable(IDC_CHECK_UPDATES, idle && !g_checking);
    enable(IDC_UPDATE, idle && i >= 0 && can_update(g_mods[i]));
    enable(IDC_UPDATE_ALL, idle && any);
    bool playable = idle && !g_game_cmd.empty();
    for (int id : {IDC_PLAY, IDC_PLAY_VANILLA}) enable(id, playable);
}

void show_details()
{
    int i = selected();
    std::wstring t;
    if (i < 0) {
        t = L"Select a mod to see what it does.";
    } else {
        const Mod& m = g_mods[i];
        t = m.name + L"   v" + m.version + (m.author.empty() ? L"" : L"   by " + m.author) + L"\r\n";
        t += L"Multiplayer: " + std::wstring(mp_text(m.multiplayer)) + L"\r\n";
        t += L"Last run: " + m.status + (m.status_time.empty() ? L"" : L"  (" + m.status_time + L")") + L"\r\n";
        if (!m.problem.empty()) t += L"Cannot load: " + m.problem + L"\r\n";
        if (auto* entry = offered_update(m)) {
            t += L"Update available: v" + swml_manifest::wide(entry->version);
            t += entry->api > SWML_API_VERSION ? L" (needs a newer SWML)\r\n" : L"\r\n";
        } else if (g_have_index && !g_index.mods.contains(swml_manifest::utf8(m.id))) {
            t += L"Updates: not in the SWML mod index, so it isn't checked for updates\r\n";
        }
        t += L"Folder: " + m.dir + L"\r\n\r\n";
        t += m.summary + L"\r\n\r\n";
        std::wstring r = m.readme;
        if (r.rfind(L"# ", 0) == 0) r = r.substr(r.find(L'\n') == std::wstring::npos ? r.size() : r.find(L'\n') + 1);
        std::wstring out;
        for (wchar_t c : r) {
            if (c == L'\r') continue;
            if (c == L'\n') out += L"\r\n";
            else out += c;
        }
        t += out;
    }
    SetWindowTextW(g_details, t.c_str());
    auto panel_directory=selected()>=0?g_mods[selected()].dir:std::wstring{};
    auto panel_version=selected()>=0?g_mods[selected()].version:std::wstring{};
    if(panel_directory!=g_panel_directory||panel_version!=g_panel_version){g_panel_directory=panel_directory;g_panel_version=panel_version;select_settings_panel(g_settings_panel,panel_directory);}
    refresh_buttons();
}

std::wstring version_text(const Mod& m)
{
    auto* entry = offered_update(m);
    return entry ? m.version + L" → " + swml_manifest::wide(entry->version) : m.version;
}

std::wstring status_word(const Mod& m)
{
    if (!m.enabled) return L"off";
    if (!m.problem.empty()) return L"blocked";
    if (m.status.rfind(L"OK", 0) == 0) return L"OK";
    if (m.status.rfind(L"crashed", 0) == 0) return L"crashed!";
    if (m.status == L"not run yet" || m.status == L"disabled") return L"";
    return L"problem";
}

void fill_list(const std::wstring& select_id = L"")
{
    g_filling = true;
    ListView_DeleteAllItems(g_list);
    int sel = -1;
    for (size_t i = 0; i < g_mods.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(g_mods[i].name.c_str());
        it.lParam = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(g_list, &it);
        std::wstring version = version_text(g_mods[i]), st = status_word(g_mods[i]);
        ListView_SetItemText(g_list, row, 1, const_cast<wchar_t*>(version.c_str()));
        ListView_SetItemText(g_list, row, 2, const_cast<wchar_t*>(st.c_str()));
        ListView_SetCheckState(g_list, row, g_mods[i].enabled);
        if (g_mods[i].id == select_id) sel = row;
    }
    if (sel < 0 && !g_mods.empty()) sel = 0;
    if (sel >= 0) ListView_SetItemState(g_list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g_filling = false;
    show_details();
}

void layout()
{
    if (!g_wnd || !g_list || !g_details) return;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int w = rc.right, h = rc.bottom, m = S(10), bh = S(28), list_w = S(380);
    int bottom = h - m - bh;
    const int top=S(48);
    MoveWindow(g_list, m, top, list_w, bottom - m - bh - top, TRUE);
    int right_x=2*m+list_w,right_width=w-3*m-list_w,header=S(30),right_height=bottom-m-bh-top;
    MoveWindow(g_detail_tabs,right_x,top,right_width,header,TRUE);
    MoveWindow(g_details,right_x,top+header,right_width,std::max(1,right_height-header),TRUE);
    MoveWindow(g_settings_panel,right_x,top+header,right_width,std::max(1,right_height-header),TRUE);
    int tool_x=m;
    const std::pair<int,int> tools[]={{IDC_DOCTOR,108},{IDC_ROLLBACK,108},{IDC_CHECK_UPDATES,130},{IDC_UPDATE,100},{IDC_UPDATE_ALL,90}};
    for (auto [control,width] : tools) {
        MoveWindow(GetDlgItem(g_wnd,control),tool_x,m,S(width),bh,TRUE); tool_x+=S(width+6);
    }
    MoveWindow(GetDlgItem(g_wnd,IDC_LOADER_NOTICE),w-m-S(170),m,S(170),bh,TRUE);
    // middle row: mod buttons
    int x = m, y = bottom - m - bh;
    int ids[] = {IDC_ADD, IDC_REMOVE, IDC_FOLDER, IDC_SETTINGS, IDC_LOGS};
    int widths[] = {S(100), S(90), S(120), S(90), S(100)};
    for (int i = 0; i < 5; i++) {
        MoveWindow(GetDlgItem(g_wnd, ids[i]), x, y, widths[i], bh, TRUE);
        x += widths[i] + S(6);
    }
    MoveWindow(g_update_status, x + S(6), y + S(6), w - m - x - S(6), bh - S(6), TRUE);
    // bottom row: play
    MoveWindow(GetDlgItem(g_wnd, IDC_PLAY), w - m - S(120), bottom, S(120), bh, TRUE);
    MoveWindow(GetDlgItem(g_wnd, IDC_PLAY_VANILLA), w - m - S(120) - S(6) - S(150), bottom, S(150), bh, TRUE);
    MoveWindow(GetDlgItem(g_wnd, IDC_COPY), m, bottom, S(190), bh, TRUE);
    MoveWindow(g_hint, m + S(200), bottom + S(6), w - 2 * m - S(200) - S(290), bh, TRUE);
}

HWND button(const wchar_t* text, int id, DWORD extra = 0)
{
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_PUSHBUTTON | extra, 0, 0, 10, 10, g_wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    auto choice=ini_str(g_root+L"\\swml.ini",L"ui",L"classic_controls",L"auto");
    if (choice==L"1" || (choice==L"auto" && swml_manifest::wine())) {
        using Theme=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);
        static HMODULE themes=LoadLibraryW(L"uxtheme.dll");
        static auto theme=themes?reinterpret_cast<Theme>(GetProcAddress(themes,"SetWindowTheme")):nullptr;
        if (theme) theme(b,L"",L"");
    }
    return b;
}

// --- add / remove ------------------------------------------------------------------------------------
std::wstring pick_mod_source(HWND owner)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    COMDLG_FILTERSPEC types[] = {{L"Mod (mod.ini or .zip)", L"mod.ini;*.zip"}};
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(L"Add a mod: pick its mod.ini, or a .zip");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void add_mod()
{
    std::wstring src = pick_mod_source(g_wnd);
    if (src.empty()) return;
    std::wstring folder, temp;
    if (src.size() > 4 && _wcsicmp(src.c_str() + src.size() - 4, L".zip") == 0) {
        wchar_t tmp[MAX_PATH];
        DWORD length=GetTempPathW(MAX_PATH,tmp);GUID unique{};wchar_t suffix[64]{};
        if(!length||length>=MAX_PATH||FAILED(CoCreateGuid(&unique))||!StringFromGUID2(unique,suffix,int(std::size(suffix)))){
            MessageBoxW(g_wnd,L"Could not allocate a temporary extraction path.",L"Add mod",MB_ICONERROR);return;
        }
        temp=std::wstring(tmp)+L"swml_add_"+suffix;
        if(!CreateDirectoryW(temp.c_str(),nullptr)){
            MessageBoxW(g_wnd,L"Could not create a private extraction directory.",L"Add mod",MB_ICONERROR);return;
        }
        // Our own reader (zip.cpp): tar.exe isn't there under Wine / Proton.
        std::wstring why;
        if (!zip_extract(src, temp, why)) {
            MessageBoxW(g_wnd, (L"Could not extract the .zip: " + why + L".\n\nExtract it yourself and pick its mod.ini.").c_str(),
                        L"Add mod", MB_ICONERROR);
            shell_op(FO_DELETE, temp, L"", FOF_NOCONFIRMATION | FOF_NOERRORUI);
            return;
        }
        folder = swml_updates::find_mod_root(temp);
    } else {
        folder = src.substr(0, src.find_last_of(L'\\'));
    }
    if (folder.empty() || !exists(folder + L"\\mod.ini")) {
        MessageBoxW(g_wnd, L"That isn't a mod: no mod.ini found.", L"Add mod", MB_ICONERROR);
    } else {
        std::wstring id = ini_str(folder + L"\\mod.ini", L"mod", L"id", folder.substr(folder.find_last_of(L'\\') + 1).c_str());
        std::wstring target = g_mods_dir + L"\\" + id;
        bool go = true;
        if (exists(target)) {
            go = MessageBoxW(g_wnd, (L"Update mod '" + id + L"'? Settings and saved mod data are preserved. The previous installation remains available through Roll back.").c_str(),
                             L"Add mod", MB_YESNO | MB_ICONQUESTION) == IDYES;
        }
        std::wstring dll = ini_str(folder + L"\\mod.ini", L"mod", L"dll");
        if (go && !dll.empty()) {
            go = MessageBoxW(g_wnd, (L"'" + id + L"' contains a program (" + dll + L") that will run inside the game with full access to your account and files.\n\n"
                                     L"Mods added from a file aren't checked by SWML. Only continue if you trust where this one came from.\n\nInstall it?").c_str(),
                             L"Add mod", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        }
        if (go && _wcsicmp(folder.c_str(), target.c_str()) != 0) {
            std::wstring why;
            if (!swml_manager::install(g_root,folder,id,why)) MessageBoxW(g_wnd,why.c_str(),L"Mod update failed",MB_ICONERROR);
        }
        load_mods();
        fill_list(id);
    }
    if (!temp.empty()) shell_op(FO_DELETE, temp, L"", FOF_NOCONFIRMATION | FOF_NOERRORUI);
}

void remove_mod()
{
    int i = selected();
    if (i < 0) return;
    Mod m = g_mods[i];
    std::wstring q = L"Remove \"" + m.name + L"\"?\n\nIts folder (with its settings) goes to the Recycle Bin.";
    if (MessageBoxW(g_wnd, q.c_str(), L"Remove mod", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!m.game_data.empty()) {
        std::wstring sw = game_data_target(stormworks_mods_dir(), m);
        if (exists(sw)) shell_op(FO_DELETE, sw, L"", FOF_NOCONFIRMATION | FOF_NOERRORUI);
    }
    if (!shell_op(FO_DELETE, m.dir, L"", FOF_ALLOWUNDO | FOF_NOCONFIRMATION)) {
        MessageBoxW(g_wnd, L"Could not move the mod folder to the Recycle Bin (is a file in it open?).", L"Remove mod", MB_ICONERROR);
    }
    WritePrivateProfileStringW(L"mods", m.id.c_str(), nullptr, g_ini.c_str());
    load_mods();
    fill_list();
}

void open_settings()
{
    int i = selected();
    if (i < 0) return;
    select_settings_panel(g_settings_panel,g_mods[i].dir);
    TabCtrl_SetCurSel(g_detail_tabs,1);ShowWindow(g_details,SW_HIDE);ShowWindow(g_settings_panel,SW_SHOW);
}

void tool_action(int action) {
    std::wstring error;
    if (action==IDC_DOCTOR) {
        auto path=g_root+L"\\logs\\doctor.json";
        if (swml_manager::doctor(g_root,path,error)) ShellExecuteW(g_wnd,L"open",L"notepad.exe",(L"\""+path+L"\"").c_str(),nullptr,SW_SHOWNORMAL);
    } else if (action==IDC_ROLLBACK) {
        auto index=selected(); if (index<0) return;
        if (MessageBoxW(g_wnd,L"Restore the previous mod installation? Current settings and saved mod data will be preserved.",L"Roll back mod",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        swml_manager::rollback(g_root,g_mods[index].id,error);
    }
    if (!error.empty()) MessageBoxW(g_wnd,error.c_str(),L"SWML",MB_ICONERROR);
    load_mods(); fill_list();
}

void set_update_status(const std::wstring& text) { SetWindowTextW(g_update_status, text.c_str()); }

void post_result(HWND window, std::unique_ptr<WorkResult> result)
{
    if (PostMessageW(window, WM_SWML_WORK_DONE, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
}

void start_update_check()
{
    if (g_checking || g_installing) return;
    g_checking = true;
    refresh_buttons();
    set_update_status(L"Checking for updates...");
    std::thread([window = g_wnd] {
        auto result = std::make_unique<WorkResult>();
        result->check = true;
        result->have_index = swml_updates::fetch_index(result->index, result->error);
        post_result(window, std::move(result));
    }).detach();
}

void start_updates(std::vector<swml_updates::Entry> entries)
{
    if (g_installing || entries.empty()) return;
    g_installing = true;
    refresh_buttons();
    set_update_status(L"Updating " + std::to_wstring(entries.size()) + (entries.size() == 1 ? L" mod..." : L" mods..."));
    std::thread([window = g_wnd, root = g_root, entries = std::move(entries)] {
        auto result = std::make_unique<WorkResult>();
        for (const auto& entry : entries) {
            std::wstring error;
            if (swml_updates::update_mod(root, entry, error)) ++result->updated;
            else result->failures += swml_manifest::wide(entry.id) + L": " + error + L"\n";
        }
        post_result(window, std::move(result));
    }).detach();
}

void selected_update()
{
    int i = selected();
    if (i < 0 || !can_update(g_mods[i])) return;
    start_updates({*offered_update(g_mods[i])});
}

void all_updates()
{
    std::vector<swml_updates::Entry> entries;
    for (const Mod& m : g_mods) if (can_update(m)) entries.push_back(*offered_update(m));
    start_updates(std::move(entries));
}

void show_loader_notice()
{
    bool show = g_have_index && swml_updates::newer(g_index.loader_version, SWML_SDK_VERSION);
    HWND notice = GetDlgItem(g_wnd, IDC_LOADER_NOTICE);
    if (show) SetWindowTextW(notice, (L"SWML " + swml_manifest::wide(g_index.loader_version) + L" available").c_str());
    ShowWindow(notice, show ? SW_SHOW : SW_HIDE);
}

void work_finished(std::unique_ptr<WorkResult> result)
{
    int i = selected();
    std::wstring keep = i >= 0 ? g_mods[i].id : L"";
    if (result->check) {
        g_checking = false;
        if (!result->have_index) {
            log_launch(L"update check failed: %ls", result->error.c_str());
            set_update_status(L"Could not check for updates: " + result->error);
            refresh_buttons();
            return;
        }
        g_index = std::move(result->index);
        g_have_index = true;
        show_loader_notice();
        fill_list(keep);
        auto count = std::count_if(g_mods.begin(), g_mods.end(), can_update);
        set_update_status(count ? std::to_wstring(count) + (count == 1 ? L" update available." : L" updates available.") : L"Mods are up to date.");
        return;
    }
    g_installing = false;
    load_mods();
    fill_list(keep);
    set_update_status(L"Updated " + std::to_wstring(result->updated) + (result->updated == 1 ? L" mod." : L" mods."));
    if (!result->failures.empty()) {
        log_launch(L"update failed: %ls", result->failures.c_str());
        MessageBoxW(g_wnd, (L"These mods were not updated and are unchanged:\n\n" + result->failures).c_str(), L"Mod update failed", MB_ICONERROR);
    }
}

void copy_launch_option()
{
    std::wstring opt = L"\"" + g_root + L"\\swml.exe\" %command%";
    if (swml_manifest::wine()) {
        wchar_t script[32768]{};
        auto count=GetEnvironmentVariableW(L"SWML_PROTON_SCRIPT",script,DWORD(std::size(script)));
        if (!count || count>=std::size(script)) { MessageBoxW(g_wnd,L"Run install_linux.sh to obtain the Steam launch option, then start SWML through that wrapper.",L"Linux launch option",MB_ICONINFORMATION); return; }
        std::wstring quoted=L"'";
        for (wchar_t c : std::wstring(script)) quoted+=c==L'\''?L"'\\''":std::wstring(1,c);
        opt=L"sh "+quoted+L"' %command%";
    }
    if (OpenClipboard(g_wnd)) {
        EmptyClipboard();
        size_t bytes = (opt.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            memcpy(GlobalLock(mem), opt.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    MessageBoxW(g_wnd,
                (L"Copied:\n\n" + opt +
                 L"\n\nIn Steam: right-click Stormworks > Properties > General > Launch Options, paste it there. From then on "
                 L"this window opens every time you start the game from Steam.")
                    .c_str(),
                L"Steam launch option", MB_ICONINFORMATION);
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        if (g_list) RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_THEMECHANGED:
        RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {S(820), S(480)};
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if(hdr->hwndFrom==g_detail_tabs && hdr->code==TCN_SELCHANGE){bool settings=TabCtrl_GetCurSel(g_detail_tabs)==1;ShowWindow(g_details,settings?SW_HIDE:SW_SHOW);ShowWindow(g_settings_panel,settings?SW_SHOW:SW_HIDE);return 0;}
        if (hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED && !g_filling) {
            auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
                Mod& m = g_mods[nm->lParam];
                bool on = ListView_GetCheckState(g_list, nm->iItem) != 0;
                if (on != m.enabled) {
                    set_enabled(m, on);
                    std::wstring st = status_word(m);
                    ListView_SetItemText(g_list, nm->iItem, 2, const_cast<wchar_t*>(st.c_str()));
                }
            }
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)) show_details();
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DOCTOR: case IDC_ROLLBACK:
            tool_action(LOWORD(wp)); break;
        case IDC_CHECK_UPDATES: start_update_check(); break;
        case IDC_UPDATE: selected_update(); break;
        case IDC_UPDATE_ALL: all_updates(); break;
        case IDC_LOADER_NOTICE: ShellExecuteW(w, L"open", swml_manifest::wide(g_index.loader_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDC_ADD: add_mod(); break;
        case IDC_REMOVE: remove_mod(); break;
        case IDC_FOLDER: {
            int i = selected();
            if (i >= 0) ShellExecuteW(w, L"open", g_mods[i].dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDC_SETTINGS: open_settings(); break;
        case IDC_LOGS: {
            std::wstring logs = g_root + L"\\logs";
            CreateDirectoryW(logs.c_str(), nullptr);
            ShellExecuteW(w, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDC_COPY: copy_launch_option(); break;
        case IDC_PLAY:
        case IDC_PLAY_VANILLA:
            if (LOWORD(wp)==IDC_PLAY) {
                load_mods();
                auto blocked=std::find_if(g_mods.begin(),g_mods.end(),[](const auto& m) { return m.enabled && !m.problem.empty(); });
                if (blocked!=g_mods.end()) { MessageBoxW(w,(blocked->name+L": "+blocked->problem+L"\n\nFix the enabled mod set, or use Play without mods.").c_str(),L"Mod preflight failed",MB_ICONERROR); fill_list(blocked->id); break; }
            }
            g_launch_requested = true;
            g_launch_with_mods = LOWORD(wp) == IDC_PLAY;
            DestroyWindow(w);
            break;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lp) == g_details) {
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_SWML_WORK_DONE:
        work_finished(std::unique_ptr<WorkResult>(reinterpret_cast<WorkResult*>(lp)));
        return 0;
    case WM_CLOSE:
        if (g_installing) { set_update_status(L"Wait for the update to finish."); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void create_window(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"swml_window";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    g_wnd = CreateWindowW(L"swml_window", L"Stormworks Mod Loader", WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(960), S(600),
                          nullptr, nullptr, inst, nullptr);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 10, 10, g_wnd, reinterpret_cast<HMENU>(IDC_LIST), inst, nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Mod");
    col.cx = S(190);
    ListView_InsertColumn(g_list, 0, &col);
    col.pszText = const_cast<wchar_t*>(L"Version");
    col.cx = S(110);
    ListView_InsertColumn(g_list, 1, &col);
    col.pszText = const_cast<wchar_t*>(L"Last run");
    col.cx = S(70);
    ListView_InsertColumn(g_list, 2, &col);

    g_detail_tabs=CreateWindowW(WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,10,10,g_wnd,nullptr,inst,nullptr);
    SendMessageW(g_detail_tabs,WM_SETFONT,reinterpret_cast<WPARAM>(g_font),TRUE);
    TCITEMW detail_tab{};detail_tab.mask=TCIF_TEXT;detail_tab.pszText=const_cast<wchar_t*>(L"About");TabCtrl_InsertItem(g_detail_tabs,0,&detail_tab);detail_tab.pszText=const_cast<wchar_t*>(L"Settings");TabCtrl_InsertItem(g_detail_tabs,1,&detail_tab);
    g_settings_panel=create_settings_panel(g_wnd,g_font);
    g_details = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, g_wnd, reinterpret_cast<HMENU>(IDC_DETAILS), inst, nullptr);
    SendMessageW(g_details, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

    button(L"Diagnostics",IDC_DOCTOR); button(L"Roll back mod",IDC_ROLLBACK);
    button(L"Check for updates",IDC_CHECK_UPDATES); button(L"Update mod",IDC_UPDATE); button(L"Update all",IDC_UPDATE_ALL);
    ShowWindow(button(L"",IDC_LOADER_NOTICE),SW_HIDE);
    g_update_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 10, 10, g_wnd, reinterpret_cast<HMENU>(IDC_UPDATE_STATUS), inst, nullptr);
    SendMessageW(g_update_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

    button(L"Add mod...", IDC_ADD);
    button(L"Remove", IDC_REMOVE);
    button(L"Open mod folder", IDC_FOLDER);
    button(L"Settings", IDC_SETTINGS);
    button(L"Open logs", IDC_LOGS);
    button(L"Copy Steam launch option", IDC_COPY);
    HWND play = button(L"Play", IDC_PLAY, BS_DEFPUSHBUTTON);
    HWND vanilla = button(L"Play without mods", IDC_PLAY_VANILLA);
    g_hint = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_wnd, reinterpret_cast<HMENU>(IDC_HINT), inst, nullptr);
    SendMessageW(g_hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    if (g_game_cmd.empty()) {
        EnableWindow(play, FALSE);
        EnableWindow(vanilla, FALSE);
        SetWindowTextW(g_hint, L"Manager only: start Stormworks from Steam to play (set the launch option first).");
    } else {
        SetWindowTextW(g_hint, L"Changes apply the next time the game starts.");
    }
    layout();
    fill_list();
    ShowWindow(g_wnd, SW_SHOW);
    RedrawWindow(g_wnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    SetForegroundWindow(g_wnd);
    SetFocus(play);
    if (GetPrivateProfileIntW(L"updates", L"check", 1, g_ini.c_str())) start_update_check();
}

// Everything after our own exe path on the command line = the game's command line from Steam.
std::wstring game_command_line()
{
    const wchar_t* c = GetCommandLineW();
    if (*c == L'"') {
        c++;
        while (*c && *c != L'"') c++;
        if (*c) c++;
    } else {
        while (*c && *c != L' ' && *c != L'\t') c++;
    }
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_root = self;
    g_root.resize(g_root.find_last_of(L'\\'));
    g_mods_dir = g_root + L"\\mods";
    g_ini = g_root + L"\\swml.ini";
    int count=0; auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if (arguments && count>=2 && std::wstring(arguments[1]).starts_with(L"--")) {
        std::wstring action=arguments[1], error; bool handled=true, okay=false;
        if (action==L"--doctor") okay=swml_manager::doctor(g_root,count>2?arguments[2]:g_root+L"\\logs\\doctor.json",error);
        else if (action==L"--rollback" && count==3) okay=swml_manager::rollback(g_root,arguments[2],error);
        else { error=L"Unknown command or invalid arguments"; handled=true; }
        LocalFree(arguments); arguments=nullptr;
        if (handled) { if (!okay) log_launch(L"command failed: %ls",error.c_str()); CoUninitialize(); return okay?0:2; }
    }
    if (arguments) LocalFree(arguments);
    g_game_cmd = game_command_line();

    load_mods();
    create_window(inst);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    int code = 0;
    if (g_launch_requested && !g_game_cmd.empty()) code = run_game(g_launch_with_mods, nullptr); // the window is gone by now
    CoUninitialize();
    return code < 0 ? 1 : code;
}
