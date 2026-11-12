#pragma once
#include <string>

namespace swml_ui {
// Show the mod window and run the event loop. game_cmd is Steam's command line (empty = manager
// only). Returns the process exit code (the game's exit code when Play was used).
int run(const std::wstring& root, const std::wstring& game_cmd);
}
