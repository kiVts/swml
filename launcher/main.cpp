// swml - Stormworks mod loader: mod window + game launcher.
//
// Windows / Wine / Proton: Steam launch option "<install dir>\swml.exe" %command%. Steam starts
// swml.exe with the game's own command line after ours; the window lists the mods in
// <install dir>\mods\<id>\, Play installs the enabled mods' game data, starts the game with
// exactly Steam's command line and loads swml_core.dll into it, then stays in the background
// until the game exits so Steam sees the session end correctly. Without a game command line it
// is a mod manager only.
//
// Native Linux: the same UI runs natively (SDL3, Wayland or X11). Play hands the game command to
// swml_proton.sh, which runs "swml.exe --run <game command>" inside the game's Proton prefix.
//
// Headless modes (no window): --run [game command], --doctor [path], --rollback <id>.
#include "platform.h"
#include "launcher_model.h"
#include "management.h"
#include "ui.h"
#ifdef _WIN32
#include "runner.h"
#endif
#include "swml_v2.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {
// Headless game launch: what swml_proton.sh invokes inside the Proton prefix.
int run_headless(const std::wstring& root, const std::wstring& game_cmd, bool with_mods)
{
    swml_launcher::Model model;
    model.init(root);
    std::vector<swml_runner::GameMod> mods;
    for (const auto& m : model.mods) mods.push_back({m.id, m.dir, m.game_data, m.enabled, m.problem});
    auto result = swml_runner::run_game(root, game_cmd, with_mods, mods);
    if (!result.error.empty()) {
        swml_runner::log(root, L"run failed: " + result.error);
        fprintf(stderr, "%s\n", swml_platform::utf8(result.error).c_str());
        return 1;
    }
    return result.code < 0 ? 1 : result.code;
}
}
#endif

int main(int argc, char** argv)
{
    swml_platform::init(argc, argv);
    auto args = swml_platform::args();
    std::wstring root = fs::path(swml_platform::self_path()).parent_path().wstring();

    // Headless commands first (used by scripts and the Proton wrapper).
    if (args.size() >= 2 && args[1].starts_with(L"--")) {
        const auto& action = args[1];
        if (action == L"--run") {
#ifdef _WIN32
            std::wstring game_cmd = swml_launcher::game_command_line(std::vector<std::wstring>(args.begin() + 1, args.end()));
            bool with_mods = swml_platform::env(L"SWML_OFF") != L"1";
            return run_headless(root, game_cmd, with_mods);
#else
            fprintf(stderr, "swml: --run is only available in the Windows/Proton build.\n");
            return 2;
#endif
        }
        bool okay = false;
        std::wstring error;
        if (action == L"--doctor") {
            std::wstring path = args.size() > 2 ? args[2] : (fs::path(root) / "logs" / "doctor.json").wstring();
            okay = swml_manager::doctor(root, path, error);
        } else if (action == L"--rollback" && args.size() == 3) {
            okay = swml_manager::rollback(root, args[2], error);
        } else {
            error = L"Unknown command or invalid arguments";
        }
        if (!okay) {
            fprintf(stderr, "swml: %s\n", swml_platform::utf8(error).c_str());
            return 2;
        }
        return 0;
    }

    std::wstring game_cmd = swml_launcher::game_command_line(args);
    return swml_ui::run(root, game_cmd);
}
