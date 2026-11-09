# SWML

Stormworks Mod Loader: `swml.exe` (the mod window and launcher) and `swml_core.dll` (loaded into the game, it loads the mods).

| Folder | What's in it |
|---|---|
| `core/` | `swml_core.dll`: logging, crash capture, shared hooks, mod loading |
| `launcher/` | `swml.exe`: the mod list, settings, starting the game |
| `sdk/` | The headers and helpers mods are built with |
| `linux/` | Installer and wrapper for Proton |

## Install

Extract the release zip and run `install.bat`. It copies the loader to your appdata/local and opens it. After that copy the launch options & add them to stormworks properties. 
After that start stormworks from steam normally, you can then click "Add Mods..." and select a zipfile for a mod.


**Dedicated servers are not supported!** They're an entirely different binary and need two different versions of... well, everything. They might get supported later.

## Build

Needs Visual Studio Build Tools 2022, CMake and git. `port_table.h`, `port_sigs.h` and `native_ui_signatures.h` are generated per
game build and aren't in this repository, so point the build at them:

```
cmake -B build -A x64 -DSWML_GAME_DATA_DIR=path/to/generated/headers
cmake --build build --config Release
```

To build a mod, see `sdk/README.md`.
