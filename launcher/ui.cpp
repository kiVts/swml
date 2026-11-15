// The mod window: Dear ImGui + SDL3 (native Wayland/X11 on Linux, Win32 on Windows).
// Replaces the old Win32/comctl32 UI; all logic (mods, updates, settings, game launch) lives in
// the portable model/manager/updates modules and the platform layer.
#include "ui.h"
#include "platform.h"
#include "launcher_model.h"
#include "management.h"
#include "updates.h"
#include "loader_version.h"
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
#include "swml_brand_data.h"

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
    if (mp == L"both_or_host" || mp == L"Both or host only") return "Both or host only";
    if (mp == L"both_or_client" || mp == L"Both or client only") return "Both or client only";
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
    bool adding = false;
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
    bool catalogue_open = false;
    char catalogue_filter[128]{};

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

    void start_updates(std::vector<swml_updates::Entry> entries, bool adding = false)
    {
        if (checking || installing || entries.empty()) return;
        installing = true;
        update_status = (adding ? L"Installing " : L"Updating ") + std::to_wstring(entries.size()) + (entries.size() == 1 ? L" mod..." : L" mods...");
        std::thread([this, root = model.root, entries = std::move(entries), adding] {
            auto result = std::make_unique<WorkResult>();
            result->adding = adding;
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
            show_notice = swml_updates::newer(index.loader_version, SWML_LOADER_VERSION);
            if (show_notice) notice_text = L"SWML " + wide(index.loader_version) + L" available";
            reload_mods();
            auto count = std::count_if(model.mods.begin(), model.mods.end(), [&](const Mod& m) { return model.can_update(m, index, have_index); });
            update_status = count ? std::to_wstring(count) + (count == 1 ? L" update available." : L" updates available.") : L"Mods are up to date.";
            return;
        }
        installing = false;
        reload_mods();
        update_status = (result->adding ? L"Installed " : L"Updated ") + std::to_wstring(result->updated) + (result->updated == 1 ? L" mod." : L" mods.");
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
        if (picked) {
            if (!picked->empty()) add_mod_from(*picked); // empty: the user closed the picker
            return;
        }
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

// --- brand ---------------------------------------------------------------------------------------
// The site's palette and the logo lockup (tools/make_brand.py draws the same thing for the SVG/PNG
// logo files; keep the geometry in step).
namespace brand {
constexpr ImVec4 rgb(int hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a);
}
constexpr ImVec4 bg = rgb(0x0b1016), bg2 = rgb(0x111923), panel = rgb(0x151f2b), line = rgb(0x243244),
                 line_hi = rgb(0x2e4057), text = rgb(0xdbe4ee), muted = rgb(0x8a9bb0), accent = rgb(0xffb020),
                 accent_hi = rgb(0xffc24f), accent_ink = rgb(0x1a1200), stripe_dim = rgb(0x1d2a3a),
                 sea = rgb(0x3aa7c9), sea_hi = rgb(0x6cc4e0);
const float text_skew = std::tan(10.0f * 3.14159265f / 180.0f); // on top of the font's own italic
const float bar_skew = std::tan(18.0f * 3.14159265f / 180.0f);
constexpr int bars = 16;
constexpr float bar_ramp = 0.16f;
// ImGui sizes a font by ascender-to-descender height; Chakra Petch's is 1.3 em (992 + 308 of 1000).
// The logo geometry is in em, like the SVG.
constexpr float em_px = 1.3f;
ImFont* font = nullptr; // Chakra Petch Bold Italic, embedded

// Slant everything drawn since vtx_start right going up, pivoting on baseline_y.
void shear(ImDrawList* dl, int vtx_start, float baseline_y, float skew)
{
    for (int i = vtx_start; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos.x += (baseline_y - v.pos.y) * skew;
    }
}

// count slanted bars filling [a, b], widest first when reverse; alpha(t) fades them along x.
template <class Alpha>
void bar_run(ImDrawList* dl, ImVec2 a, ImVec2 b, float gap, bool reverse, ImVec4 color, Alpha alpha)
{
    const float lean = (b.y - a.y) * bar_skew;
    float weights = 0;
    for (int i = 0; i < bars; i++) weights += 1 + i * bar_ramp;
    const float unit = (b.x - a.x - lean - gap * (bars - 1)) / weights;
    if (unit <= 0) return;
    float x = a.x;
    for (int i = 0; i < bars; i++) {
        int k = reverse ? bars - 1 - i : i;
        float w = (1 + k * bar_ramp) * unit;
        float t = (x + w * 0.5f + lean * 0.5f - a.x) / (b.x - a.x);
        ImVec4 c = color;
        c.w *= alpha(t);
        if (c.w > 0.004f)
            dl->AddQuadFilled(ImVec2(x + lean, a.y), ImVec2(x + lean + w, a.y), ImVec2(x + w, b.y), ImVec2(x, b.y), ImGui::GetColorU32(c));
        x += w + gap;
    }
}

// Ink extent of text drawn at x = 0 in the current font: (left, right).
ImVec2 ink(const char* str, float tracking = 0)
{
    ImFontBaked* baked = ImGui::GetFontBaked();
    float pen = 0, left = 0, right = 0;
    for (const char* p = str; *p; p++) {
        const ImFontGlyph* g = baked->FindGlyph(ImWchar(*p));
        if (p == str) left = pen + g->X0;
        right = pen + g->X1;
        pen += g->AdvanceX + (p[1] ? tracking : 0);
    }
    return ImVec2(left, right);
}

// Draws str with extra tracking between glyphs. One AddText run, then each glyph's quad is moved
// along: drawing glyph by glyph would snap each one to a pixel and space them unevenly.
void tracked(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* str, float tracking)
{
    ImFontBaked* baked = ImGui::GetFontBaked();
    int vtx = dl->VtxBuffer.Size;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, color, str);
    int i = 0;
    for (const char* p = str; *p; p++, i++) {
        const ImFontGlyph* g = baked->FindGlyph(ImWchar(*p));
        if (!g->Visible) continue;
        for (int k = 0; k < 4 && vtx < dl->VtxBuffer.Size; k++) dl->VtxBuffer[vtx++].pos.x += i * tracking;
    }
}

// The SWML lockup: dim bars, the wordmark F px tall (font size), the subtitle tracked to its
// width. top_left is the top-left of the bars; returns the lockup's size.
ImVec2 logo(ImDrawList* dl, ImVec2 top_left, float F, bool measure_only = false)
{
    const float pad_y = 0.10f * F, bar_left = 0.40f * F, bar_right = 1.10f * F;
    ImGui::PushFont(font, F * em_px);
    ImFontBaked* big = ImGui::GetFontBaked();
    const float cap = F * 0.70f; // Chakra Petch cap height is 700/1000
    ImVec2 word = ink("SWML");
    ImGui::PopFont();
    const float sub_size = 0.205f * F;
    ImGui::PushFont(font, sub_size * em_px);
    ImVec2 sub_plain = ink("STORMWORKS MOD LOADER");
    const float tracking = ((word.y - word.x) - (sub_plain.y - sub_plain.x)) / 20.0f;
    ImVec2 sub = ink("STORMWORKS MOD LOADER", tracking);
    ImFontBaked* small = ImGui::GetFontBaked();
    ImGui::PopFont();

    const float width = bar_left + (word.y - word.x) + bar_right;
    const float baseline = top_left.y + pad_y + cap;
    const float sub_baseline = baseline + 0.13f * F + 0.70f * sub_size;
    const float height = sub_baseline - top_left.y + 0.04f * F;
    if (measure_only) return ImVec2(width, height);

    bar_run(dl, top_left, ImVec2(top_left.x + width, baseline + pad_y), 0.10f * F, true, stripe_dim,
            [](float t) { return t <= 0.55f ? 1.0f : std::max(0.0f, 1.0f - (t - 0.55f) / 0.45f); });

    const float word_x = top_left.x + bar_left - word.x;
    int start = dl->VtxBuffer.Size;
    dl->AddText(font, F * em_px, ImVec2(word_x, baseline - big->Ascent), ImGui::GetColorU32(accent), "SWML");
    shear(dl, start, baseline, text_skew);

    ImGui::PushFont(font, sub_size * em_px);
    start = dl->VtxBuffer.Size;
    tracked(dl, ImVec2(top_left.x + bar_left - sub.x, sub_baseline - small->Ascent), ImGui::GetColorU32(accent),
            "STORMWORKS MOD LOADER", tracking);
    shear(dl, start, sub_baseline, text_skew);
    ImGui::PopFont();
    return ImVec2(width, height);
}

// The header's yellow speed stripes: widening to the right, fading in from the left.
void stripes(ImDrawList* dl, ImVec2 a, ImVec2 b, float gap)
{
    bar_run(dl, a, b, gap, false, accent, [](float t) { return std::min(1.0f, t / 0.45f); });
}

// The usual "opens elsewhere" icon: a box with an arrow leaving its top-right corner. pos is the
// top-left of an s x s square.
void link_icon(ImDrawList* dl, ImVec2 pos, float s, ImU32 color)
{
    const float t = std::max(1.0f, s * 0.12f);
    auto at = [&](float x, float y) { return ImVec2(pos.x + x * s, pos.y + y * s); };
    ImVec2 box[5] = {at(0.42f, 0.18f), at(0.04f, 0.18f), at(0.04f, 0.96f), at(0.82f, 0.96f), at(0.82f, 0.58f)};
    dl->AddPolyline(box, 5, color, ImDrawFlags_None, t);
    dl->AddLine(at(0.40f, 0.60f), at(0.96f, 0.04f), color, t);
    ImVec2 head[3] = {at(0.58f, 0.04f), at(0.96f, 0.04f), at(0.96f, 0.42f)};
    dl->AddPolyline(head, 3, color, ImDrawFlags_None, t);
}

struct ButtonLook {
    float font_px = 17;  // Chakra Petch size, before ui scale
    float cut = 0;
    bool link = false;   // trailing link icon
};

// The site's .btn: a parallelogram with italic Chakra Petch; primary is the yellow one.
bool button(const char* label, ImVec2 size, bool primary, float scale, ButtonLook look = {})
{
    ImGui::PushFont(font, look.font_px * scale);
    ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const float icon = look.link ? label_size.y * 0.62f : 0.0f, icon_gap = look.link ? label_size.y * 0.35f : 0.0f;
    if (size.x <= 0) size.x = label_size.x + icon_gap + icon + 44 * scale;
    if (size.y <= 0) size.y = label_size.y + 16 * scale;
    const float cut = look.cut > 0 ? look.cut * scale : 0;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImVec4 fill = primary ? (hovered ? accent_hi : accent) : (held ? line_hi : hovered ? line : panel);
    if (primary && held) fill = accent;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q[4] = {ImVec2(p.x + cut, p.y), ImVec2(p.x + size.x, p.y), ImVec2(p.x + size.x - cut, p.y + size.y), ImVec2(p.x, p.y + size.y)};
    dl->AddConvexPolyFilled(q, 4, ImGui::GetColorU32(fill));
    if (!primary) dl->AddPolyline(q, 4, ImGui::GetColorU32(line_hi), ImDrawFlags_Closed, 1.0f);
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        dl->AddPolyline(q, 4, ImGui::GetColorU32(accent_hi), ImDrawFlags_Closed, 2.0f);
    const ImU32 ink_color = ImGui::GetColorU32(primary ? accent_ink : text);
    const float group = label_size.x + icon_gap + icon;
    ImVec2 at(p.x + (size.x - group) * 0.5f, p.y + (size.y - label_size.y) * 0.5f);
    dl->AddText(at, ink_color, label, std::strstr(label, "##")); // text after ## is the id
    if (look.link) link_icon(dl, ImVec2(at.x + label_size.x + icon_gap, p.y + (size.y - icon) * 0.5f), icon, ink_color);
    ImGui::PopFont();
    return pressed;
}

// A text link in the site's link colour, underlined on hover, with the link icon after it.
bool link(const char* label)
{
    ImVec2 label_size = ImGui::CalcTextSize(label);
    const float icon = label_size.y * 0.62f, gap = label_size.y * 0.3f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, ImVec2(label_size.x + gap + icon, label_size.y));
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImU32 color = ImGui::GetColorU32(hovered ? sea_hi : sea);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, color, label);
    if (hovered) dl->AddLine(ImVec2(p.x, p.y + label_size.y), ImVec2(p.x + label_size.x, p.y + label_size.y), color, 1.0f);
    link_icon(dl, ImVec2(p.x + label_size.x + gap, p.y + (label_size.y - icon) * 0.5f), icon, color);
    return pressed;
}

void apply_style(ImGuiStyle& style)
{
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 0.0f;
    style.ScrollbarRounding = 2.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f; // text boxes and buttons stay visible on any panel
    style.PopupBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg2;
    c[ImGuiCol_PopupBg] = bg2;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = line;
    c[ImGuiCol_FrameBgActive] = line_hi;
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_TitleBgCollapsed] = panel;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x05080b, 0.70f);
    c[ImGuiCol_ScrollbarBg] = bg2;
    c[ImGuiCol_ScrollbarGrab] = line;
    c[ImGuiCol_ScrollbarGrabHovered] = line_hi;
    c[ImGuiCol_ScrollbarGrabActive] = muted;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = line;
    c[ImGuiCol_ButtonActive] = line_hi;
    c[ImGuiCol_Header] = rgb(0xffb020, 0.18f);   // selected mod row
    c[ImGuiCol_HeaderHovered] = line;
    c[ImGuiCol_HeaderActive] = rgb(0xffb020, 0.26f);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = line_hi;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = line;
    c[ImGuiCol_ResizeGripHovered] = line_hi;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = bg2;
    c[ImGuiCol_TabHovered] = line;
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg2;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = accent;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = line;
    c[ImGuiCol_TableRowBg] = rgb(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xffffff, 0.02f);
    c[ImGuiCol_TextSelectedBg] = rgb(0xffb020, 0.30f);
    c[ImGuiCol_NavCursor] = accent;
}
} // namespace brand

void load_brand_font(float scale)
{
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // static data
    std::snprintf(config.Name, sizeof(config.Name), "Chakra Petch Bold Italic");
    brand::font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(swml_brand::font),
                                                             int(sizeof(swml_brand::font)), 16.0f * scale, &config);
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
    brand::apply_style(style);

#ifndef _WIN32
    // Windows takes the window icon from the exe's icon resource; elsewhere hand SDL the pixels.
    if (SDL_Surface* icon = SDL_CreateSurfaceFrom(swml_brand::icon64_size, swml_brand::icon64_size, SDL_PIXELFORMAT_RGBA32,
                                                  const_cast<unsigned char*>(swml_brand::icon64), swml_brand::icon64_size * 4)) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
#endif

    float scale = SDL_GetWindowDisplayScale(window);
    if (!(scale > 0.0f)) scale = 1.0f;
    app.ui_scale = scale;
    load_fonts(scale);
    load_brand_font(scale);
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

        // --- header: the logo, the update buttons, then the site's yellow speed stripes ----------------
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float F = 46 * scale;               // wordmark size (em)
            const float pad = style.WindowPadding.x;  // the bars run off the window's left edge
            ImVec2 origin = ImGui::GetCursorScreenPos();
            // The window clips its padding; the bars and stripes are meant to touch the window edges.
            dl->PushClipRect(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y));
            ImVec2 logo = brand::logo(dl, ImVec2(origin.x - pad, origin.y), F);
            const float band_top = origin.y, band_bottom = origin.y + 0.9f * F; // the bars' height
            // Update controls beside the wordmark; catalogue action at the far end of the banner.
            const int rows = 3;
            const float row_gap = 3 * scale;
            const float row_h = std::floor((logo.y - row_gap * (rows - 1)) / rows);
            const brand::ButtonLook small{row_h * 0.92f / scale, -1, false};
            const brand::ButtonLook small_link{small.font_px, -1, true};
            const float step = 0;
            const float stack_left = origin.x - pad + logo.x - 0.55f * F;
            std::string first = app.show_notice ? u8(app.notice_text) : app.checking ? "Checking..." : "Check for updates";
            ImGui::PushFont(brand::font, small.font_px * scale);
            const float stack_w = std::max({ImGui::CalcTextSize(first.c_str()).x + (app.show_notice ? row_h : 0.0f),
                                            ImGui::CalcTextSize("Check for updates").x,
                                            ImGui::CalcTextSize("Website").x + row_h}) + 30 * scale;
            ImGui::PopFont();
            auto row = [&](int i) { ImGui::SetCursorScreenPos(ImVec2(stack_left + (rows - 1 - i) * step, origin.y + i * (row_h + row_gap))); };

            row(0);
            if (app.show_notice) {
                // A newer loader is out: this row becomes the yellow link to it.
                if (brand::button((first + "##notice").c_str(), ImVec2(stack_w, row_h), true, scale, small_link))
                    swml_platform::open_external(wide(app.index.loader_url));
            } else {
                ImGui::BeginDisabled(!idle || app.checking);
                if (brand::button((first + "##check").c_str(), ImVec2(stack_w, row_h), false, scale, small)) app.start_update_check();
                ImGui::EndDisabled();
            }
            row(1);
            bool any_update = std::any_of(app.model.mods.begin(), app.model.mods.end(), [&](const Mod& m) { return app.model.can_update(m, app.index, app.have_index); });
            ImGui::BeginDisabled(!idle || !any_update);
            if (brand::button("Update all", ImVec2(stack_w, row_h), false, scale, small)) {
                std::vector<swml_updates::Entry> entries;
                for (const Mod& m : app.model.mods) if (app.model.can_update(m, app.index, app.have_index)) entries.push_back(*app.model.offered_update(m, app.index, app.have_index));
                app.start_updates(std::move(entries));
            }
            ImGui::EndDisabled();
            row(2);
            if (brand::button("Website", ImVec2(stack_w, row_h), false, scale, small_link))
                swml_platform::open_external(L"https://swml.kivts.net");

            const float stripes_left = stack_left + (rows - 1) * step + stack_w + 18 * scale;
            const float browse_width = 178 * scale, browse_height = band_bottom - band_top;
            const float browse_lean = browse_height * brand::bar_skew;
            const float browse_left = viewport->WorkPos.x + viewport->WorkSize.x - browse_width - pad;
            const float stripes_right = browse_left + browse_lean - 0.10f * F;
            if (stripes_right - stripes_left > 80 * scale)
                brand::stripes(dl, ImVec2(stripes_left, band_top), ImVec2(stripes_right, band_bottom), 0.10f * F);
            ImGui::SetCursorScreenPos(ImVec2(browse_left, band_top));
            if (brand::button("BROWSE MODS", ImVec2(browse_width, browse_height), true, scale, {17, browse_lean / scale, false})) {
                app.catalogue_open = true;
                if (!app.have_index) app.start_update_check();
            }
            dl->PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + logo.y));
            ImGui::Dummy(ImVec2(0, 2 * scale));
        }

        ImGui::Separator();

        // --- main area: mod list + details -----------------------------------------------------------
        const float play_height = 38 * scale;
        float footer_height = ImGui::GetFrameHeightWithSpacing() + play_height + ImGui::GetTextLineHeight() +
                              3 * style.ItemSpacing.y + 4 * scale;
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

        // --- footer: actions on the selected mod, troubleshooting on the right, then Play -----------------
        ImGui::BeginDisabled(!idle);
        if (ImGui::Button("Add mod...")) app.add_mod();
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!idle || !sel);
        if (ImGui::Button("Remove")) app.remove_mod();
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!idle || !sel || !app.model.can_update(*sel, app.index, app.have_index));
        if (ImGui::Button("Update mod")) app.start_updates({*app.model.offered_update(*sel, app.index, app.have_index)});
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!sel);
        if (ImGui::Button("Open mod folder")) swml_platform::open_external(sel->dir);
        ImGui::EndDisabled();
        ImGui::SameLine(0, spacing);
        ImGui::BeginDisabled(!sel);
        if (ImGui::Button("Settings")) app.focus_settings = true;
        ImGui::EndDisabled();
        {
            const float right = ImGui::CalcTextSize("Diagnostics").x + ImGui::CalcTextSize("Open logs").x +
                                4 * style.FramePadding.x + spacing;
            ImGui::SameLine(0, spacing);
            float room = ImGui::GetContentRegionAvail().x - right;
            if (room > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
        }
        if (ImGui::Button("Diagnostics")) {
            std::wstring error;
            auto path = (fs::path(app.model.root) / "logs" / "doctor.json").wstring();
            if (swml_manager::doctor(app.model.root, path, error)) swml_platform::open_text_file(path);
            else app.open_popup(L"SWML", error);
            app.reload_mods();
        }
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Open logs")) {
            auto logs = (fs::path(app.model.root) / "logs").wstring();
            std::error_code ec;
            fs::create_directories(fs::path(logs), ec);
            swml_platform::open_external(logs);
        }

        ImGui::Dummy(ImVec2(0, 2 * scale));
        bool playable = idle && !app.game_cmd.empty();
        const float play_y = ImGui::GetCursorPosY();
        ImGui::BeginDisabled(!playable);
        if (brand::button("PLAY", ImVec2(150 * scale, play_height), true, scale)) app.request_play(true);
        ImGui::SameLine(0, spacing);
        if (brand::button("PLAY WITHOUT MODS", ImVec2(0, play_height), false, scale)) app.request_play(false);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 14 * scale);
        ImGui::SetCursorPosY(play_y + (play_height - ImGui::GetFrameHeight()) * 0.5f);
        if (ImGui::Button("Copy Steam launch option")) app.copy_launch_option();
        ImGui::SetCursorPosY(play_y + play_height + style.ItemSpacing.y);
        ImGui::TextDisabled("%s", app.game_cmd.empty()
            ? "Manager only: start Stormworks from Steam to play (set the launch option first)."
            : "Changes apply the next time the game starts.");
        if (!app.update_status.empty()) {
            ImGui::SameLine(0, 18 * scale);
            ImGui::PushStyleColor(ImGuiCol_Text, brand::accent);
            ImGui::TextUnformatted(u8(app.update_status).c_str());
            ImGui::PopStyleColor();
        }
        {
            const char* discord = "SWML Discord";
            const float need = ImGui::CalcTextSize(discord).x + ImGui::GetTextLineHeight();
            ImGui::SameLine(0, 18 * scale);
            float room = ImGui::GetContentRegionAvail().x - need;
            if (room > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
            if (brand::link(discord)) swml_platform::open_external(L"https://discord.gg/bX4RzunsxC");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("discord.gg/bX4RzunsxC");
        }

        ImGui::End();

        // --- popups --------------------------------------------------------------------------------------
        if (app.catalogue_open) ImGui::OpenPopup("Browse mods##catalogue");
        ImGui::SetNextWindowSize(ImVec2(std::min(820 * scale, viewport->WorkSize.x - 24 * scale),
                                      std::min(600 * scale, viewport->WorkSize.y - 24 * scale)), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Browse mods##catalogue", &app.catalogue_open)) {
            ImGui::SetNextItemWidth(std::max(120 * scale, ImGui::GetContentRegionAvail().x - 130 * scale));
            ImGui::InputTextWithHint("##cataloguesearch", "Search mods...", app.catalogue_filter, sizeof(app.catalogue_filter));
            ImGui::SameLine();
            ImGui::BeginDisabled(app.checking || app.installing);
            if (brand::button("REFRESH", ImVec2(0, 28 * scale), false, scale)) app.start_update_check();
            ImGui::EndDisabled();
            ImGui::TextWrapped("%s", app.update_status.empty() ? "Choose a mod to download and install." : u8(app.update_status).c_str());
            ImGui::Separator();
            ImGui::BeginChild("##cataloguerows", ImVec2(0, -40 * scale));
            if (!app.have_index) ImGui::TextWrapped("%s", app.checking ? "Loading available mods..." : "Catalogue unavailable. Use Refresh to try again.");
            ImGuiTextFilter filter;
            std::strncpy(filter.InputBuf, app.catalogue_filter, sizeof(filter.InputBuf) - 1);
            filter.Build();
            int visible = 0;
            for (const auto& [id, entry] : app.index.mods) {
                std::string searchable = entry.name + " " + id + " " + entry.summary;
                if (!filter.PassFilter(searchable.c_str())) continue;
                ++visible;
                ImGui::PushID(id.c_str());
                auto installed = std::find_if(app.model.mods.begin(), app.model.mods.end(), [&](const Mod& m) { return u8(m.id) == id; });
                bool present = installed != app.model.mods.end();
                bool newer = present && swml_updates::newer(entry.version, u8(installed->version));
                ImGui::Spacing();
                ImGui::TextColored(brand::accent, "%s", entry.name.c_str());
                ImGui::TextDisabled("Available: v%s%s", entry.version.c_str(), entry.obsolete ? " - OBSOLETE" : "");
                ImGui::TextWrapped("%s", entry.summary.empty() ? "Description not supplied by the catalogue. See the GitHub page for details." : entry.summary.c_str());
                if (!entry.multiplayer.empty()) ImGui::TextDisabled("Multiplayer: %s", mp_text(wide(entry.multiplayer)));
                if (entry.api > SWML_API_VERSION) ImGui::TextDisabled("Requires a newer SWML");
                if (entry.obsolete) ImGui::TextWrapped("Obsolete: this functionality is included in SWML. No separate install is needed.");
                ImGui::BeginDisabled(app.checking || app.installing || entry.api > SWML_API_VERSION || entry.obsolete || (present && !newer));
                if (ImGui::Button(entry.obsolete ? "Obsolete" : !present ? "Install" : newer ? "Update" : "Installed", ImVec2(130 * scale, 30 * scale)))
                    app.start_updates({entry}, !present);
                ImGui::EndDisabled();
                if (!entry.homepage.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("GitHub", ImVec2(90 * scale, 30 * scale))) swml_platform::open_external(wide(entry.homepage));
                }
                if (present) { ImGui::SameLine(); ImGui::TextDisabled("Installed v%s", u8(installed->version).c_str()); }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::PopID();
            }
            if (app.have_index && !visible) ImGui::TextDisabled("No mods match your search.");
            ImGui::EndChild();
            if (brand::button("CLOSE", ImVec2(110 * scale, 30 * scale), false, scale)) {
                app.catalogue_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
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
        SDL_SetRenderDrawColor(renderer, 11, 16, 22, 255); // brand::bg
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
