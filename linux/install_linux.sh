#!/bin/sh
# Installs the Stormworks mod loader into Stormworks' Proton prefix and prints the Steam launch option.
#   sh install_linux.sh               find the prefix in your Steam libraries
#   sh install_linux.sh <prefix>      use this prefix (the folder that contains drive_c)
# Run Stormworks once with Proton first, so its prefix exists. Running it again updates the loader
# but keeps your mods, your swml.ini, each mod's settings.ini and the logs.
# The kit contains the native Linux mod window (swml) plus the Windows loader (swml.exe,
# swml_core.dll) that Proton runs headless to start the game with mods.
set -eu
APPID=573090
KIT=$(cd "$(dirname "$0")" && pwd)
SRC="$KIT/swml"
[ -f "$SRC/swml.exe" ] || SRC="$KIT/runtime"

if [ ! -f "$SRC/swml.exe" ]; then
    echo "No swml/swml.exe next to this script. Unzip the whole kit and run it from there."
    exit 1
fi

find_prefix() {
    for root in "${SWML_STEAM_ROOT:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
        "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$root" ] || continue
        {
            echo "$root"
            vdf="$root/steamapps/libraryfolders.vdf"
            [ -f "$vdf" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf"
        } | while IFS= read -r lib; do
            if [ -d "$lib/steamapps/compatdata/$APPID/pfx/drive_c" ]; then
                echo "$lib/steamapps/compatdata/$APPID/pfx"
            fi
        done
    done | head -n1
}

if [ $# -ge 1 ]; then PREFIX=$1; else PREFIX=$(find_prefix); fi
if [ -z "$PREFIX" ] || [ ! -d "$PREFIX/drive_c" ]; then
    echo "Could not find Stormworks' Proton prefix (steamapps/compatdata/$APPID/pfx)."
    echo "Start Stormworks once with Proton, quit, and run this again, or pass the prefix:"
    echo "  sh install_linux.sh /path/to/steamapps/compatdata/$APPID/pfx"
    exit 1
fi
DEST="$PREFIX/drive_c/users/steamuser/AppData/Local/swml"
mkdir -p "$DEST/logs"

# Copy everything, but never overwrite what belongs to the user.
(cd "$SRC" && find . -type f) | while IFS= read -r f; do
    case $f in
    ./logs/* | ./backups/* | ./staging/* | ./mods/*/data/*) continue ;;
    esac
    case $(basename "$f") in
    swml.ini | settings.ini | settings.overrides.ini | status.ini) [ -e "$DEST/$f" ] && continue ;;
    esac
    mkdir -p "$DEST/$(dirname "$f")"
    # Publish a completed file instead of rewriting an inode mapped by Wine.
    update="$DEST/$f.swml-update-$$"
    cp "$SRC/$f" "$update"
    mv -f "$update" "$DEST/$f"
done
chmod +x "$DEST/swml_proton.sh"
[ -f "$DEST/swml" ] && chmod +x "$DEST/swml"

# First install: only the diagnostics/sync trace mod on, so desync captures aren't changed by the
# other mods. Turn the others on in the mod window when you want them.
if [ ! -e "$DEST/swml.ini" ]; then
    printf '[profile]\r\ndefault_disabled=1\r\n[mods]\r\ndiagnostics=1\r\n' >"$DEST/swml.ini"
fi

# Shortcut to the logs (swml.log, trace_*.csv, crash dumps) next to the kit.
ln -sfn "$DEST/logs" "$KIT/swml_logs" 2>/dev/null || true

echo "Installed to: $DEST"
echo
echo "Steam > Stormworks > Properties > General > Launch Options, paste:"
echo
quote() { printf "'"; printf '%s' "$1" | sed "s/'/'\\\\''/g"; printf "'"; }
printf '  sh '
quote "$DEST/swml_proton.sh"
printf ' %%command%%\n'
echo
echo "Starting the game now shows the native mod window first; Play starts the game with the enabled mods."
echo "Logs: $KIT/swml_logs (swml.log, launcher.log, swml_proton.log, trace_*.csv)."
