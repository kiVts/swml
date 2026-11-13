// The mod window: Dear ImGui + SDL3 (native Wayland/X11 on Linux, Win32 on Windows).
// Replaces the old Win32/comctl32 UI; all logic (mods, updates, settings, game launch) lives in
// the portable model/manager/updates modules and the platform layer.
#include "ui.h"
#include "platform.h"
#include "launcher_model.h"
#include "management.h"
#include "updates.h"
#include "zip.h"
#include "settings_store.h"
#include "settings_schema.h"
#include "mod_manifest.h"
#include "swml_v2.h"
#ifdef _WIN32
#include "runner.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using swml_launcher::Mod;

namespace swml_ui {
namespace {

// --- small helpers ---------------------------------------------------------------------------------
std::string u8(const std::wstring& text)
{
    try { return swml_platform::utf8(text); } catch (...) { return {}; }
}
std::wstring wide(const std::string& text)
{
    try { return swml_platform::wide(text); } catch (...) { return {}; }
}
std::wstring trim(std::wstring s)
{
    auto begin = s.find_first_not_of(L" \t\r\n");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(L" \t\r\n") - begin + 1);
}

const char* mp_text(const std::wstring& mp)
{
    if (mp == L"host") return "the host needs it (other players don't)";
    if (mp == L"everyone") return "every player in the session needs it";
    return "only affects you (other players don't need it)";
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

// --- settings panel --------------------------------------------------------------------------------
// Same field logic as the old inline panel: schema fields first, INI-comment descriptions for
// schema-free mods, unit-suffixed keys stay number fields, values saved through settings_store.
struct SettingsItem {
    std::wstring section, key, initial, description;
    bool checkbox = false, heading = false;
    int schema_index = -1; // index into SettingsPanel::schema, -1 = schema-free
    std::string buffer; // edit buffer for text fields
    bool checked = false;
};

struct SettingsPanel {
    std::wstring dir;
    std::vector<SettingsItem> items;
    std::vector<swml_settings::Field> schema;
    std::wstring error;

    void load(const std::wstring& directory)
    {
        dir = directory;
        items.clear();
        schema.clear();
        error.clear();
        if (dir.empty()) return;
        try {
            swml_settings_store::prepare(dir);
            auto schema_path = fs::path(dir) / "settings.schema.json";
            if (fs::is_regular_file(schema_path)) {
                auto schema_bytes = swml_platform::read_bytes(schema_path, 65535);
                if (!schema_bytes.empty()) schema = swml_settings::parse(swml_json::parse(schema_bytes));
            }
            // INI comments supply descriptions for existing schema-free mods.
            std::map<swml_settings_store::Key, std::wstring> descriptions;
            auto source = swml_platform::read_text((fs::path(dir) / "settings.default.ini").wstring());
            std::wstring section, description;
            size_t start = 0;
            while (start < source.size()) {
                auto end = source.find(L'\n', start);
                if (end == source.npos) end = source.size();
                auto line = trim(source.substr(start, end - start));
                start = end + 1;
                if (line.starts_with(L";") || line.starts_with(L"#")) { if (!description.empty()) description += L" "; description += trim(line.substr(1)); }
                else if (line.starts_with(L"[")) { auto close = line.find(L']'); section = swml_settings_store::lower(line.substr(1, close - 1)); description.clear(); }
                else if (auto equal = line.find(L'='); equal != line.npos) { descriptions[{section, swml_settings_store::lower(trim(line.substr(0, equal)))}] = description; description.clear(); }
            }
            auto base = swml_settings_store::defaults(dir), current = swml_settings_store::merged(dir);
            for (auto& field : schema) {
                auto id = swml_settings_store::Key{swml_settings_store::lower(wide(field.section)), swml_settings_store::lower(wide(field.key))};
                if (!base.contains(id)) base[id] = wide(swml_settings::encode(field, field.initial));
            }

            std::wstring previous_section;
            for (const auto& [id, value] : base) {
                if (id.first != previous_section) {
                    SettingsItem heading;
                    heading.heading = true;
                    heading.key = L"[" + id.first + L"]";
                    items.push_back(std::move(heading));
                    previous_section = id.first;
                }
                SettingsItem item;
                item.section = id.first;
                item.key = id.second;
                item.initial = value;
                item.description = descriptions[id];
                for (size_t f = 0; f < schema.size(); f++)
                    if (swml_settings_store::lower(wide(schema[f].section)) == id.first && swml_settings_store::lower(wide(schema[f].key)) == id.second) {
                        item.schema_index = int(f);
                        item.description = wide(schema[f].description);
                        break;
                    }
                // Ambiguous numeric 0/1 settings stay editable rather than becoming switches.
                auto lower = swml_settings_store::lower(item.description);
                // A key ending in a unit (isolate_s, send_rate_kb, ...) is a quantity even when
                // its default is 0 or 1: as a checkbox it could only ever be saved as 1.
                auto unit_suffix = [](const std::wstring& key) {
                    for (auto u : {L"_s", L"_ms", L"_m", L"_cm", L"_mm", L"_km", L"_kb", L"_mb", L"_gb", L"_hz", L"_n", L"_pct", L"_deg", L"_px"}) {
                        std::wstring_view s(u);
                        if (key.size() > s.size() && key.compare(key.size() - s.size(), s.size(), s) == 0) return true;
                    }
                    return false;
                };
                item.checkbox = item.schema_index >= 0 ? schema[size_t(item.schema_index)].type == "bool"
                    : (value == L"0" || value == L"1") &&
                        (lower.find(L"1 =") != lower.npos || lower.find(L"1=") != lower.npos || lower.find(L"0 = off") != lower.npos || item.key == L"enabled") &&
                        lower.find(L"0 = auto") == lower.npos &&
                        id.second.find(L"threads") == id.second.npos && id.second.find(L"ticks") == id.second.npos && !unit_suffix(id.second) &&
                        lower.find(L"2 =") == lower.npos && lower.find(L"2=") == lower.npos;
                auto found = current.find(id);
                auto effective = found == current.end() ? value : found->second;
                if (item.checkbox) item.checked = effective == L"1" || effective == L"true";
                else item.buffer = u8(effective);
                items.push_back(std::move(item));
            }
            if (items.empty()) {
                SettingsItem empty;
                empty.heading = true;
                empty.key = L"This mod has no settings.";
                items.push_back(std::move(empty));
            }
        } catch (const std::exception& e) {
            error = wide(e.what());
        }
    }

    void save(SettingsItem& item, const std::wstring& value)
    {
        try {
            std::wstring encoded = value;
            if (item.schema_index >= 0) {
                const auto& field = schema[size_t(item.schema_index)];
                auto decoded = swml_settings::decode(field, u8(value));
                encoded = wide(swml_settings::encode(field, decoded));
            } else if (!item.checkbox) {
                wchar_t* end = nullptr;
                // Only checks whether the current value is numeric; the number itself is not needed.
                [[maybe_unused]] const double initial_number = std::wcstod(item.initial.c_str(), &end);
                if (end != item.initial.c_str() && !*end) {
                    double number = std::wcstod(value.c_str(), &end);
                    if (end == value.c_str() || *end || !std::isfinite(number)) throw std::runtime_error("enter a finite numeric value");
                }
            }
            swml_settings_store::set(dir, item.section, item.key, encoded, &item.initial);
        } catch (const std::exception& e) {
            error = wide(e.what());
            // Revert the control to the stored value.
            try {
                auto values = swml_settings_store::merged(dir);
                auto found = values.find({item.section, item.key});
                auto stored = found == values.end() ? item.initial : found->second;
                if (item.checkbox) item.checked = stored == L"1" || stored == L"true";
                else item.buffer = u8(stored);
            } catch (...) {}
        }
    }
};

// --- background work -------------------------------------------------------------------------------
struct WorkResult {
    bool check = false;
    bool have_index = false;
    swml_updates::Index index;
    std::wstring error, failures;
    int updated = 0;
};

// --- application state -----------------------------------------------------------------------------
struct Popup {
    std::string id, title, message;
    bool yes_no = false;
    int action = 0; // 0 = info, 1 = remove mod, 2 = overwrite mod, 3 = trust DLL mod
    std::wstring payload;
};

enum Action { ACT_NONE, ACT_REMOVE, ACT_OVERWRITE, ACT_TRUST_DLL };

struct App {
    swml_launcher::Model model;
    std::wstring game_cmd;
    int selected = -1;
    bool focus_settings = false;
    SettingsPanel settings;
    std::wstring settings_dir;

    swml_updates::Index index;
    bool have_index = false, checking = false, installing = false;
    std::wstring update_status;

    std::mutex work_mutex;
    std::unique_ptr<WorkResult> pending_work;

    std::vector<Popup> popups;
    bool quit_requested = false;
    bool launch_requested = false, launch_with_mods = true;
    int exit_code = 0;

    // pending "add mod" flow (confirmations between steps)
    std::wstring add_folder, add_id, add_dll, add_temp;

    std::wstring notice_text; // loader update notice
    bool show_notice = false;

    float ui_scale = 1.0f;
    std::string path_prompt_buffer;
    bool path_prompt_open = false;

    Mod* selected_mod() { return selected >= 0 && selected < int(model.mods.size()) ? &model.mods[selected] : nullptr; }

    void open_popup(const std::wstring& title, const std::wstring& message, bool yes_no = false, int action = ACT_NONE, const std::wstring& payload = L"")
    {
        Popup p;
        p.title = u8(title);
        p.message = u8(message);
        p.yes_no = yes_no;
        p.action = action;
        p.payload = payload;
        p.id = p.title + "##" + std::to_string(popups.size());
        popups.push_back(std::move(p));
    }

    void refresh_selection()
    {
        if (selected >= int(model.mods.size())) selected = int(model.mods.size()) - 1;
        auto* m = selected_mod();
        std::wstring dir = m ? m->dir : L"";
        if (dir != settings_dir) {
            settings_dir = dir;
            settings.load(dir);
        }
    }

    void reload_mods(const std::wstring& keep_id = L"")
    {
        std::wstring keep = keep_id;
        if (keep.empty()) { if (auto* m = selected_mod()) keep = m->id; }
        model.reload();
        selected = -1;
        for (size_t i = 0; i < model.mods.size(); i++) if (model.mods[i].id == keep) { selected = int(i); break; }
        if (selected < 0 && !model.mods.empty()) selected = 0;
        settings_dir.clear(); // force settings reload for the new selection
        refresh_selection();
    }

    // --- updates -----------------------------------------------------------------------------------
    void start_update_check()
    {
        if (checking || installing) return;
        checking = true;
        update_status = L"Checking for updates...";
        std::thread([this] {
            auto result = std::make_unique<WorkResult>();
            result->check = true;
            result->have_index = swml_updates::fetch_index(result->index, result->error);
            std::lock_guard lock(work_mutex);
            pending_work = std::move(result);
        }).detach();
    }

    void start_updates(std::vector<swml_updates::Entry> entries)
    {
        if (installing || entries.empty()) return;
        installing = true;
        update_status = L"Updating " + std::to_wstring(entries.size()) + (entries.size() == 1 ? L" mod..." : L" mods...");
        std::thread([this, root = model.root, entries = std::move(entries)] {
            auto result = std::make_unique<WorkResult>();
            for (const auto& entry : entries) {
                std::wstring error;
                if (swml_updates::update_mod(root, entry, error)) ++result->updated;
                else result->failures += wide(entry.id) + L": " + error + L"\n";
            }
            std::lock_guard lock(work_mutex);
            pending_work = std::move(result);
        }).detach();
    }

    void poll_work()
    {
        std::unique_ptr<WorkResult> result;
        {
            std::lock_guard lock(work_mutex);
            result = std::move(pending_work);
        }
        if (!result) return;
        if (result->check) {
            checking = false;
            if (!result->have_index) {
                update_status = L"Could not check for updates: " + result->error;
                return;
            }
            index = std::move(result->index);
            have_index = true;
            show_notice = swml_updates::newer(index.loader_version, SWML_SDK_VERSION);
            if (show_notice) notice_text = L"SWML " + wide(index.loader_version) + L" available";
            reload_mods();
            auto count = std::count_if(model.mods.begin(), model.mods.end(), [&](const Mod& m) { return model.can_update(m, index, have_index); });
            update_status = count ? std::to_wstring(count) + (count == 1 ? L" update available." : L" updates available.") : L"Mods are up to date.";
            return;
        }
        installing = false;
        reload_mods();
        update_status = L"Updated " + std::to_wstring(result->updated) + (result->updated == 1 ? L" mod." : L" mods.");
        if (!result->failures.empty()) open_popup(L"Mod update failed", L"These mods were not updated and are unchanged:\n\n" + result->failures);
    }

    // --- add / remove ------------------------------------------------------------------------------
    void finish_add()
    {
        std::wstring target = (fs::path(model.mods_dir) / add_id).wstring();
        if (swml_platform::compare_icase(add_folder, target) != 0) {
            std::wstring why;
            if (!swml_manager::install(model.root, add_folder, add_id, why)) open_popup(L"Mod update failed", why);
        }
        if (!add_temp.empty()) swml_platform::remove_tree(add_temp, false);
        add_temp.clear();
        reload_mods(add_id);
    }

    void add_mod_from(const std::wstring& src)
    {
        if (src.empty()) return;
        std::wstring folder, temp;
        if (src.size() > 4 && swml_platform::compare_icase(src.substr(src.size() - 4), L".zip") == 0) {
            temp = (fs::path(swml_platform::temp_dir()) / (L"swml_add_" + swml_platform::make_unique_id())).wstring();
            std::error_code ec;
            fs::create_directories(fs::path(temp), ec);
            if (ec) { open_popup(L"Add mod", L"Could not create a private extraction directory."); return; }
            std::wstring why;
            if (!zip_extract(src, temp, why)) {
                open_popup(L"Add mod", L"Could not extract the .zip: " + why + L".\n\nExtract it yourself and pick its mod.ini.");
                swml_platform::remove_tree(temp, false);
                return;
            }
            folder = swml_updates::find_mod_root(temp);
        } else {
            folder = fs::path(src).parent_path().wstring();
        }
        if (folder.empty() || !swml_platform::path_exists((fs::path(folder) / "mod.ini").wstring())) {
            open_popup(L"Add mod", L"That isn't a mod: no mod.ini found.");
            if (!temp.empty()) swml_platform::remove_tree(temp, false);
            return;
        }
        add_folder = folder;
        add_temp = temp;
        add_id = swml_platform::ini_read((fs::path(folder) / "mod.ini").wstring(), L"mod", L"id", fs::path(folder).filename().wstring().c_str());
        add_dll = swml_platform::ini_read((fs::path(folder) / "mod.ini").wstring(), L"mod", L"dll");
        std::wstring target = (fs::path(model.mods_dir) / add_id).wstring();
        if (swml_platform::path_exists(target)) {
            open_popup(L"Add mod", L"Update mod '" + add_id + L"'? Settings and saved mod data are preserved.", true, ACT_OVERWRITE);
            return;
        }
        if (!add_dll.empty()) {
            open_popup(L"Add mod",
                       L"'" + add_id + L"' contains a program (" + add_dll + L") that will run inside the game with full access to your account and files.\n\n"
                       L"Mods added from a file aren't checked by SWML. Only continue if you trust where this one came from.\n\nInstall it?",
                       true, ACT_TRUST_DLL);
            return;
        }
        finish_add();
    }

    void add_mod()
    {
        auto picked = swml_platform::pick_open_file(L"Add a mod: pick its mod.ini, or a .zip",
                                                    {{L"Mod (mod.ini or .zip)", L"mod.ini;*.zip"}});
        if (!picked.empty()) { add_mod_from(picked); return; }
        // No desktop picker available (e.g. gamescope session): ask for a path.
        path_prompt_buffer.clear();
        path_prompt_open = true;
    }

    void remove_mod()
    {
        auto* m = selected_mod();
        if (!m) return;
        open_popup(L"Remove mod", L"Remove \"" + m->name + L"\"?\n\nIts folder (with its settings) goes to the Recycle Bin.", true, ACT_REMOVE, m->id);
    }

    void do_remove(const std::wstring& id)
    {
        auto found = std::find_if(model.mods.begin(), model.mods.end(), [&](const Mod& m) { return m.id == id; });
        if (found == model.mods.end()) return;
        Mod m = *found;
        if (!m.game_data.empty()) {
            auto sw = fs::path(swml_platform::roaming_appdata()) / "Stormworks" / "data" / "mods" / m.id;
            if (swml_platform::path_exists(sw.wstring())) swml_platform::remove_tree(sw.wstring(), false);
        }
        if (!swml_platform::remove_tree(m.dir, true))
            open_popup(L"Remove mod", L"Could not move the mod folder to the Recycle Bin (is a file in it open?).");
        swml_platform::ini_write(model.ini, L"mods", m.id, nullptr);
        reload_mods();
    }

    void popup_yes(int action, const std::wstring& payload)
    {
        switch (action) {
        case ACT_REMOVE: do_remove(payload); break;
        case ACT_OVERWRITE:
            if (!add_dll.empty()) {
                open_popup(L"Add mod",
                           L"'" + add_id + L"' contains a program (" + add_dll + L") that will run inside the game with full access to your account and files.\n\n"
                           L"Mods added from a file aren't checked by SWML. Only continue if you trust where this one came from.\n\nInstall it?",
                           true, ACT_TRUST_DLL);
            } else finish_add();
            break;
        case ACT_TRUST_DLL: finish_add(); break;
        default: break;
        }
    }

    // --- launch --------------------------------------------------------------------------------------
    std::wstring steam_launch_option()
    {
#ifdef _WIN32
        if (!swml_platform::is_wine()) return L"\"" + model.root + L"\\swml.exe\" %command%";
#endif
        auto script = swml_platform::env(L"SWML_PROTON_SCRIPT");
        if (script.empty()) script = (fs::path(model.root) / "swml_proton.sh").wstring();
        std::wstring quoted = L"'";
        for (wchar_t c : script) quoted += c == L'\'' ? L"'\\''" : std::wstring(1, c);
        return L"sh " + quoted + L"' %command%";
    }

    void copy_launch_option()
    {
        auto option = steam_launch_option();
        SDL_SetClipboardText(u8(option).c_str());
        open_popup(L"Steam launch option",
                   L"Copied:\n\n" + option +
                   L"\n\nIn Steam: right-click Stormworks > Properties > General > Launch Options, paste it there. From then on "
                   L"this window opens every time you start the game from Steam.");
    }

    void request_play(bool with_mods)
    {
        if (installing) { update_status = L"Wait for the update to finish."; return; }
        if (with_mods) {
            reload_mods();
            auto blocked = std::find_if(model.mods.begin(), model.mods.end(), [](const Mod& m) { return m.enabled && !m.problem.empty(); });
            if (blocked != model.mods.end()) {
                open_popup(L"Mod preflight failed", blocked->name + L": " + blocked->problem + L"\n\nFix the enabled mod set, or use Play without mods.");
                for (size_t i = 0; i < model.mods.size(); i++) if (model.mods[i].id == blocked->id) { selected = int(i); break; }
                settings_dir.clear();
                refresh_selection();
                return;
            }
        }
        launch_requested = true;
        launch_with_mods = with_mods;
        quit_requested = true;
    }

    int launch_game()
    {
#ifdef _WIN32
        std::vector<swml_runner::GameMod> mods;
        for (const auto& m : model.mods) mods.push_back({m.id, m.dir, m.game_data, m.enabled, m.problem});
        auto result = swml_runner::run_game(model.root, game_cmd, launch_with_mods, mods);
        if (!result.error.empty()) {
            swml_runner::log(model.root, L"launch failed: " + result.error);
            fprintf(stderr, "%s\n", u8(result.error).c_str());
            return 1;
        }
        return result.code < 0 ? 1 : result.code;
#else
        // Native Linux: the game must start inside Proton. Hand Steam's command line to the
        // wrapper, which runs swml.exe --run in the game's prefix and loads the core there.
        auto script = swml_platform::env(L"SWML_PROTON_SCRIPT");
        if (script.empty()) script = (fs::path(model.root) / "swml_proton.sh").wstring();
        if (!swml_platform::path_exists(script)) {
            fprintf(stderr, "swml: swml_proton.sh not found next to the launcher; run install_linux.sh.\n");
            return 1;
        }
        std::vector<std::wstring> arguments{script};
        // Split the stored command line back into arguments (it was quoted by us).
        const auto& cmd = game_cmd;
        size_t i = 0;
        while (i < cmd.size()) {
            while (i < cmd.size() && cmd[i] == L' ') i++;
            if (i >= cmd.size()) break;
            std::wstring arg;
            if (cmd[i] == L'"') {
                i++;
                while (i < cmd.size()) {
                    if (cmd[i] == L'\\' && i + 1 < cmd.size() && cmd[i + 1] == L'"') { arg += L'"'; i += 2; continue; }
                    if (cmd[i] == L'"') { i++; break; }
                    arg += cmd[i++];
                }
            } else {
                while (i < cmd.size() && cmd[i] != L' ') arg += cmd[i++];
            }
            arguments.push_back(arg);
        }
        // SWML_PLAY tells the wrapper to run the headless in-Proton launcher; SWML_OFF is read
        // by the wrapper and the in-Proton launcher.
        std::vector<std::wstring> extra_env{L"SWML_PLAY=1", L"SWML_OFF=" + std::wstring(launch_with_mods ? L"0" : L"1")};
        auto result = swml_platform::run_process(L"sh", arguments, L"", 0, extra_env);
        return result.started ? result.exit_code : 1;
#endif
    }
};

// --- fonts -------------------------------------------------------------------------------------------
void load_fonts(float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    const float size = 16.0f * scale;
#ifdef _WIN32
    wchar_t windows[MAX_PATH];
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring dir(windows, n && n < MAX_PATH ? n : 0);
    for (const wchar_t* name : {L"\\fonts\\segoeui.ttf", L"\\fonts\\tahoma.ttf", L"\\fonts\\arial.ttf"}) {
        auto path = dir + name;
        if (swml_platform::path_exists(path)) {
            if (io.Fonts->AddFontFromFileTTF(u8(path).c_str(), size, &config)) return;
        }
    }
#else
    const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    };
    for (const char* path : candidates)
        if (swml_platform::path_exists(wide(path)))
            if (io.Fonts->AddFontFromFileTTF(path, size, &config)) return;
#endif
    io.Fonts->AddFontDefault();
}

} // namespace

int run(const std::wstring& root, const std::wstring& game_cmd)
{
    App app;
    app.model.init(root);
    app.game_cmd = game_cmd;
    app.reload_mods();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "swml: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Native Wayland when available (SDL picks it from the environment), X11 otherwise.
    SDL_Window* window = SDL_CreateWindow("Stormworks Mod Loader", 1000, 640,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "swml: could not create a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 820, 480);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        fprintf(stderr, "swml: could not create a renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't litter the install dir with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.22f, 0.36f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.44f, 0.66f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.36f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.36f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.47f, 0.71f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.30f, 0.47f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.78f, 1.0f, 1.0f);

    float scale = SDL_GetWindowDisplayScale(window);
    if (!(scale > 0.0f)) scale = 1.0f;
    app.ui_scale = scale;
    load_fonts(scale);
    style.ScaleAllSizes(scale);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    if (swml_platform::ini_read_int(app.model.ini, L"updates", L"check", 1)) app.start_update_check();

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                if (app.installing) app.update_status = L"Wait for the update to finish.";
                else running = false;
            }
        }
        app.poll_work();
        if (app.quit_requested) running = false;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale, 10 * scale));
        ImGui::Begin("##swml", nullptr, flags);
        ImGui::PopStyleVar();

        const float spacing = 6 * scale;
        bool idle = !app.installing;
        auto* sel = app.selected_mod();

        // --- top toolbar ---------------------------------------------------------------------------
        if (ImGui::Button("Diagnostics")) {
            std::wstring error;
            auto path = (fs::path(app.model.root) / "logs" / "doctor.json").wstring();
            if (swml_manager::doctor(app.model.root, path, error)) swml_platform::open_text_file(path);
            else app.open_popup(L"SWML", error);
            app.reload_mods();
        }
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!idle || app.checking);
        if (ImGui::Button("Check for updates")) app.start_update_check();
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!idle || !sel || !app.model.can_update(*sel, app.index, app.have_index));
        if (ImGui::Button("Update mod")) app.start_updates({*app.model.offered_update(*sel, app.index, app.have_index)});
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        bool any_update = std::any_of(app.model.mods.begin(), app.model.mods.end(), [&](const Mod& m) { return app.model.can_update(m, app.index, app.have_index); });
        ImGui::BeginDisabled(!idle || !any_update);
        if (ImGui::Button("Update all")) {
            std::vector<swml_updates::Entry> entries;
            for (const Mod& m : app.model.mods) if (app.model.can_update(m, app.index, app.have_index)) entries.push_back(*app.model.offered_update(m, app.index, app.have_index));
            app.start_updates(std::move(entries));
        }
        ImGui::EndDisabled();
        if (app.show_notice) {
            ImGui::SameLine(0, spacing);
            if (ImGui::Button(u8(app.notice_text).c_str())) swml_platform::open_external(wide(app.index.loader_url));
        }

        ImGui::Separator();

        // --- main area: mod list + details -----------------------------------------------------------
        float footer_height = (28 + 28 + 28 + 24) * scale;
        ImVec2 area = ImGui::GetContentRegionAvail();
        float list_width = std::min(400 * scale, area.x * 0.45f);
        float body_height = area.y - footer_height;

        if (ImGui::BeginChild("##mods", ImVec2(list_width, body_height), ImGuiChildFlags_Borders)) {
            if (ImGui::BeginTable("##modtable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * scale);
                ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 100 * scale);
                ImGui::TableSetupColumn("Last run", ImGuiTableColumnFlags_WidthFixed, 70 * scale);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < app.model.mods.size(); i++) {
                    Mod& m = app.model.mods[i];
                    ImGui::PushID(int(i));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool enabled = m.enabled;
                    if (ImGui::Checkbox("##enable", &enabled) && enabled != m.enabled) {
                        app.model.set_enabled(m, enabled);
                    }
                    ImGui::TableSetColumnIndex(1);
                    bool is_selected = app.selected == int(i);
                    if (ImGui::Selectable(u8(m.name).c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        app.selected = int(i);
                        app.settings_dir.clear();
                        app.refresh_selection();
                    }
                    ImGui::TableSetColumnIndex(2);
                    auto version = m.version;
                    if (auto* entry = app.model.offered_update(m, app.index, app.have_index)) version += L" -> " + wide(entry->version);
                    ImGui::TextUnformatted(u8(version).c_str());
                    ImGui::TableSetColumnIndex(3);
                    auto word = status_word(m);
                    if (word == L"crashed!" || word == L"blocked") ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                    else if (word == L"OK") ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
                    else if (word == L"problem") ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
                    else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted(u8(word).c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0, 10 * scale);

        if (ImGui::BeginChild("##details", ImVec2(0, body_height), ImGuiChildFlags_Borders)) {
            if (ImGui::BeginTabBar("##detailtabs")) {
                if (ImGui::BeginTabItem("About")) {
                    if (!sel) {
                        ImGui::TextDisabled("Select a mod to see what it does.");
                    } else {
                        const Mod& m = *sel;
                        ImGui::Text("%s   v%s%s", u8(m.name).c_str(), u8(m.version).c_str(),
                                    m.author.empty() ? "" : ("   by " + u8(m.author)).c_str());
                        ImGui::Text("Multiplayer: %s", mp_text(m.multiplayer));
                        ImGui::Text("Last run: %s%s", u8(m.status).c_str(),
                                    m.status_time.empty() ? "" : ("  (" + u8(m.status_time) + ")").c_str());
                        if (!m.problem.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                            ImGui::TextWrapped("Cannot load: %s", u8(m.problem).c_str());
                            ImGui::PopStyleColor();
                        }
                        if (auto* entry = app.model.offered_update(m, app.index, app.have_index)) {
                            ImGui::Text("Update available: v%s%s", entry->version.c_str(),
                                        entry->api > SWML_API_VERSION ? " (needs a newer SWML)" : "");
                        } else if (app.have_index && !app.index.mods.contains(u8(m.id))) {
                            ImGui::TextDisabled("Updates: not in the SWML mod index, so it isn't checked for updates");
                        }
                        ImGui::TextDisabled("Folder: %s", u8(m.dir).c_str());
                        ImGui::Separator();
                        if (!m.summary.empty()) ImGui::TextWrapped("%s", u8(m.summary).c_str());
                        ImGui::Separator();
                        ImGui::BeginChild("##readme", ImVec2(0, 0), ImGuiChildFlags_None);
                        auto readme = m.readme;
                        if (readme.rfind(L"# ", 0) == 0) readme = readme.substr(readme.find(L'\n') == std::wstring::npos ? readme.size() : readme.find(L'\n') + 1);
                        ImGui::TextWrapped("%s", u8(readme).c_str());
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings", nullptr, app.focus_settings ? ImGuiTabItemFlags_SetSelected : 0)) {
                    app.focus_settings = false;
                    if (!sel) {
                        ImGui::TextDisabled("Select a mod to edit its settings.");
                    } else {
                        auto& panel = app.settings;
                        if (!panel.error.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                            ImGui::TextWrapped("%s", u8(panel.error).c_str());
                            ImGui::PopStyleColor();
                            panel.error.clear();
                        }
                        ImGui::TextDisabled("Saved immediately; takes effect when the game next starts.");
                        ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_None);
                        for (size_t i = 0; i < panel.items.size(); i++) {
                            auto& item = panel.items[i];
                            ImGui::PushID(int(i));
                            if (item.heading) {
                                ImGui::SeparatorText(u8(item.key).c_str());
                            } else if (item.checkbox) {
                                bool checked = item.checked;
                                if (ImGui::Checkbox(u8(item.key).c_str(), &checked) && checked != item.checked) {
                                    item.checked = checked;
                                    panel.save(item, checked ? L"1" : L"0");
                                }
                                if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", u8(item.description).c_str());
                            } else {
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(u8(item.key).c_str());
                                if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", u8(item.description).c_str());
                                ImGui::SameLine(180 * scale);
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                char buffer[4096];
                                std::strncpy(buffer, item.buffer.c_str(), sizeof(buffer) - 1);
                                buffer[sizeof(buffer) - 1] = 0;
                                if (ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    item.buffer = buffer;
                                    panel.save(item, wide(buffer));
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    item.buffer = buffer;
                                    panel.save(item, wide(buffer));
                                }
                                if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", u8(item.description).c_str());
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        // --- footer ------------------------------------------------------------------------------------
        ImGui::BeginDisabled(!idle);
        if (ImGui::Button("Add mod...")) app.add_mod();
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!idle || !sel);
        if (ImGui::Button("Remove")) app.remove_mod();
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!sel);
        if (ImGui::Button("Open mod folder")) swml_platform::open_external(sel->dir);
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!sel);
        if (ImGui::Button("Settings")) app.focus_settings = true;
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Open logs")) {
            auto logs = (fs::path(app.model.root) / "logs").wstring();
            std::error_code ec;
            fs::create_directories(fs::path(logs), ec);
            swml_platform::open_external(logs);
        }
        if (!app.update_status.empty()) {
            ImGui::SameLine(0, 12 * scale);
            ImGui::TextDisabled("%s", u8(app.update_status).c_str());
        }

        ImGui::Spacing();
        bool playable = idle && !app.game_cmd.empty();
        ImGui::BeginDisabled(!playable);
        if (ImGui::Button("Play", ImVec2(120 * scale, 0))) app.request_play(true);
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Play without mods", ImVec2(150 * scale, 0))) app.request_play(false);
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Copy Steam launch option")) app.copy_launch_option();
        ImGui::TextDisabled("%s", app.game_cmd.empty()
            ? "Manager only: start Stormworks from Steam to play (set the launch option first)."
            : "Changes apply the next time the game starts.");

        ImGui::End();

        // --- popups --------------------------------------------------------------------------------------
        for (size_t i = 0; i < app.popups.size(); i++) ImGui::OpenPopup(app.popups[i].id.c_str());
        for (size_t i = 0; i < app.popups.size();) {
            auto& popup = app.popups[i];
            bool open = true;
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(std::min(560 * scale, viewport->WorkSize.x - 40 * scale), 0));
            bool done = false;
            if (ImGui::BeginPopupModal(popup.id.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("%s", popup.message.c_str());
                ImGui::Spacing();
                if (popup.yes_no) {
                    if (ImGui::Button("Yes", ImVec2(90 * scale, 0))) { app.popup_yes(popup.action, popup.payload); done = true; }
                    ImGui::SameLine(0, spacing);
                    if (ImGui::Button("No", ImVec2(90 * scale, 0))) {
                        if (popup.action == ACT_OVERWRITE || popup.action == ACT_TRUST_DLL) {
                            if (!app.add_temp.empty()) swml_platform::remove_tree(app.add_temp, false);
                            app.add_temp.clear();
                        }
                        done = true;
                    }
                } else if (ImGui::Button("OK", ImVec2(90 * scale, 0))) done = true;
                if (done || !open) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (done || !open) app.popups.erase(app.popups.begin() + long(i));
            else i++;
        }

        // Path prompt fallback when no desktop file picker exists.
        if (app.path_prompt_open) ImGui::OpenPopup("Add a mod##path");
        {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(std::min(600 * scale, viewport->WorkSize.x - 40 * scale), 0));
            if (ImGui::BeginPopupModal("Add a mod##path", &app.path_prompt_open, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("No file picker is available. Enter the path of a mod's mod.ini or a .zip:");
                char buffer[4096];
                std::strncpy(buffer, app.path_prompt_buffer.c_str(), sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool enter = ImGui::InputText("##path", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
                app.path_prompt_buffer = buffer;
                if (ImGui::Button("Add", ImVec2(90 * scale, 0)) || enter) {
                    app.path_prompt_open = false;
                    ImGui::CloseCurrentPopup();
                    app.add_mod_from(trim(wide(app.path_prompt_buffer)));
                }
                ImGui::SameLine(0, spacing);
                if (ImGui::Button("Cancel", ImVec2(90 * scale, 0))) { app.path_prompt_open = false; ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        }

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (app.launch_requested && !app.game_cmd.empty()) return app.launch_game();
    return app.exit_code;
}

} // namespace swml_ui
