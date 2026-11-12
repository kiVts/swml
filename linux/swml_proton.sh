#!/bin/sh
# swml_proton.sh - Stormworks mod loader on Linux (Steam + Proton).
#
# Steam launch option (install_linux.sh prints the exact line):
#   sh "<prefix>/drive_c/users/steamuser/AppData/Local/swml/swml_proton.sh" %command%
#
# The mod window is the native Linux launcher (swml, next to this script). When it asks to play
# it re-invokes this script with SWML_PLAY=1: a Windows exe can't go in front of %command% on
# Linux, so the script then edits Proton's command instead: the game exe (.../stormworks64.exe)
# becomes  swml.exe --run Z:\...\stormworks64.exe. Proton starts swml.exe headless in the game's
# prefix, exactly as on Windows: it installs the enabled mods' game data, starts the game with
# the rest of the command line and loads swml_core.dll into it.
#
# SWML_OFF=1 %command%  in the launch options (before this script) starts the game untouched.
SWML=$(cd "$(dirname "$0")" && pwd)
SWML_PROTON_SCRIPT="$SWML/swml_proton.sh"
export SWML_PROTON_SCRIPT
mkdir -p "$SWML/logs"
LOG="$SWML/logs/swml_proton.log"
log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$LOG"; }

# Linux path -> Wine path (Proton maps Z: to /).
winpath() {
    case $1 in
    [A-Za-z]:*) printf '%s' "$1"; return ;;
    /*) p=$1 ;;
    *) p=$PWD/$1 ;;
    esac
    printf 'Z:%s' "$p" | tr '/' '\\'
}

log "steam: $*"
log "environment: session=${XDG_SESSION_TYPE:-unknown} display=${DISPLAY:-none} wayland=${WAYLAND_DISPLAY:-none} prefix=${STEAM_COMPAT_DATA_PATH:-unknown} play=${SWML_PLAY:-0}"
if [ "$#" -eq 0 ]; then
    log "no Steam command supplied"
    exit 64
fi
if [ "${SWML_OFF:-0}" = 1 ]; then
    log "SWML_OFF=1: starting the game without the mod loader"
    exec "$@"
fi

# Play handoff from the native launcher: run the Windows launcher headless inside Proton.
if [ "${SWML_PLAY:-0}" = 1 ]; then
    if [ ! -f "$SWML/swml.exe" ]; then
        log "no swml.exe next to this script: starting the game without the mod loader"
        exec "$@"
    fi
    found=
    for a do
        shift
        if [ -z "$found" ]; then
            case $a in
            */[Ss]tormworks64.exe | [Ss]tormworks64.exe | */[Ss]tormworks.exe | [Ss]tormworks.exe | *\\[Ss]tormworks64.exe)
                found=1
                set -- "$@" "$SWML/swml.exe" --run "$(winpath "$a")"
                continue
                ;;
            esac
        fi
        set -- "$@" "$a"
    done
    if [ -z "$found" ]; then
        log "no stormworks64.exe in the command: starting it unchanged"
    else
        log "run:   $*"
    fi
    exec "$@"
fi

# Normal Steam start: show the native mod window first (Wayland/X11, no Wine UI).
if [ -x "$SWML/swml" ]; then
    log "native launcher: $SWML/swml"
    "$SWML/swml" "$@"
    code=$?
    log "native launcher exited with code $code"
    exit "$code"
fi

# Fallback (no native binary in the kit): the Windows launcher inside Proton, as before.
if [ ! -f "$SWML/swml.exe" ]; then
    log "no swml or swml.exe next to this script: starting the game without the mod loader"
    exec "$@"
fi
found=
for a do
    shift
    if [ -z "$found" ]; then
        case $a in
        */[Ss]tormworks64.exe | [Ss]tormworks64.exe | */[Ss]tormworks.exe | [Ss]tormworks.exe | *\\[Ss]tormworks64.exe)
            found=1
            set -- "$@" "$SWML/swml.exe" "$(winpath "$a")"
            continue
            ;;
        esac
    fi
    set -- "$@" "$a"
done
if [ -z "$found" ]; then
    log "no stormworks64.exe in the command: starting it unchanged"
else
    log "run:   $*"
fi
exec "$@"
