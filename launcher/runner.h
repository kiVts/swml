#pragma once
// Game launch side of the loader (Windows / Wine / Proton only). The native Linux launcher
// doesn't link this: it hands the game command to swml_proton.sh, which runs swml.exe --run
// inside the game's Proton prefix.
#include <string>
#include <vector>

namespace swml_runner {

struct GameMod {
    std::wstring id, dir, game_data;
    bool enabled = true;
    std::wstring problem;
};

struct Result {
    int code = -1;            // game exit code, or -1 when it couldn't be started
    std::wstring error;       // human-readable failure reason (empty on success)
};

// Install the enabled mods' Stormworks game data, start the game with exactly the given command
// line, load swml_core.dll into it (startup bootstrap or renderer-wait injection) and wait for
// it to exit. Logs to <root>\logs\launcher.log.
Result run_game(const std::wstring& root, const std::wstring& game_cmd, bool with_mods,
                const std::vector<GameMod>& mods);

// Append a timestamped line to <root>\logs\launcher.log.
void log(const std::wstring& root, const std::wstring& message);

} // namespace swml_runner
