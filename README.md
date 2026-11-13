# SWML

[![Join Discord](https://img.shields.io/badge/Join-Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/bX4RzunsxC)

Stormworks Mod Loader: `swml.exe` (the mod window and launcher) and `swml_core.dll` (loaded into the game, it loads the mods).
On Linux the mod window is a **native app** (`swml`, Dear ImGui + SDL3, Wayland or X11); the game itself still runs through Proton with the Windows loader.

| Folder | What's in it |
|---|---|
| `core/` | `swml_core.dll`: logging, crash capture, shared hooks, mod loading |
| `launcher/` | `swml.exe` / `swml`: the mod list, settings, starting the game |
| `sdk/` | The headers and helpers mods are built with |
| `linux/` | Installer, Proton wrapper and static release build for Linux |
| `tests/` | Portable unit tests for the launcher logic |


## Install

Extract the release zip and run `install.bat`. It copies the loader to your appdata/local and opens it. After that copy the launch options & add them to stormworks properties.
After that start stormworks from steam normally, you can then click "Add Mods..." and select a zipfile for a mod. Be careful what you install.

### Linux (Steam + Proton)

Run Stormworks once with Proton so its prefix exists, then from the extracted kit:

```
sh linux/install_linux.sh
```

It finds the prefix, installs the native mod window plus the Windows loader into it, and prints the Steam launch option to paste
(`sh ".../swml_proton.sh" %command%`). Starting the game from Steam then opens the native mod window first; **Play** hands the
game back to Proton, which runs `swml.exe --run` headless to install the enabled mods' game data and load `swml_core.dll`.
`SWML_OFF=1 %command%` starts the game untouched. Re-running the installer updates the loader but keeps your mods and settings.

The native window runs on Wayland and X11, scales with the display, and uses the desktop's file picker (zenity/kdialog) and trash.
The release binary is fully static (only libc/libm), so it also runs on SteamOS/Steam Deck without installing anything.

**Dedicated servers are not supported!** They're an entirely different binary and need two different versions of... well, everything. They might get supported later.

## Build

The launcher (mod window) builds on Windows, Linux and macOS, and cross-builds with mingw. The in-game core and startup bootstrap
are Windows-only and need Visual Studio Build Tools 2022 plus generated per-game-build headers: `port_table.h`, `port_sigs.h` and
`native_ui_signatures.h` aren't in this repository, so point the build at them:

```
cmake -B build -A x64 -DSWML_GAME_DATA_DIR=path/to/generated/headers
cmake --build build --config Release
```

Without `SWML_GAME_DATA_DIR` (or on Linux) only the launcher is built:

```
cmake -B build -DSWML_BUILD_CORE=OFF
cmake --build build
ctest --test-dir build
```

Linux dependencies: a C++20 compiler, CMake, libcurl + OpenSSL dev packages; for Wayland support also
`libwayland-dev`, `libxkbcommon-dev` and `wayland-protocols` (SDL dlopens the Wayland client libs at runtime).
For the single-file static release: `sh linux/build-release.sh` (builds a minimal HTTPS-only static libcurl, then links everything statically).

To build a mod, see `sdk/README.md`.
