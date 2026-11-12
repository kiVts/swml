#pragma once
// Launcher model: mod discovery, enable state, game-data sync inputs. Shared by the UI and the
// headless --run mode; no UI toolkit types here.
#include "platform.h"
#include "updates.h"
#include <string>
#include <vector>

namespace swml_launcher {

struct Mod {
    std::wstring id, dir, name, version, author, summary, multiplayer, game_data, dll, status, status_time, readme;
    std::wstring problem;
    bool enabled = true;
};

struct Model {
    std::wstring root, mods_dir, ini;
    std::vector<Mod> mods;

    void init(const std::wstring& root_dir);
    void reload();
    void set_enabled(Mod& mod, bool on);
    // Update-index helpers (index must have been fetched).
    const swml_updates::Entry* offered_update(const Mod& mod, const swml_updates::Index& index, bool have_index) const;
    bool can_update(const Mod& mod, const swml_updates::Index& index, bool have_index) const;
};

// Everything after the executable path on the command line = the game's command line from Steam.
std::wstring game_command_line(const std::vector<std::wstring>& args);

} // namespace swml_launcher
