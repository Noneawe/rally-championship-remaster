#!/bin/bash
# Rally Championship Remaster - PortMaster launcher.
#
#   ports/Rally Championship.sh       this script
#   ports/rallychampionship/
#       rally.aarch64                 the game
#       overrides/                    replacement art packs
#       licenses/
#       game/                         YOUR copy of the game: RAL.EXE, files/, var/
#       args.txt     (optional)       extra options, one line, e.g. "--nosound"
#       window.txt   (optional)       the render size, e.g. "640x480"
#       log.txt                       this run's output (attach it to bug reports)
#
# The game reads the pad itself through SDL's GameController API (with
# PortMaster's controller config), so gptokeyb is not used: an empty gptk
# still applies gptokeyb's built-in defaults, which sent L2 as HOME.
# Select+Start quits (the game's own handling).  Keep this file LF-only.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/rallychampionship"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

echo "Rally Championship Remaster  $(date)"
echo "CFW_NAME=${CFW_NAME:-}  DEVICE_NAME=${DEVICE_NAME:-}  display=${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?}"

DEVICE_ARCH="${DEVICE_ARCH:-aarch64}"

# The original game's files are the player's own and are not shipped.
if [ ! -f "$GAMEDIR/game/RAL.EXE" ] || [ ! -d "$GAMEDIR/game/files" ]; then
  MSG="Game files not found. Copy RAL.EXE, files/ and var/ from your own copy of Rally Championship into ports/rallychampionship/game/"
  echo "$MSG"
  if type pm_message >/dev/null 2>&1; then pm_message "$MSG"; sleep 8; fi
  pm_finish
  exit 1
fi

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"

# The render size.  The game's own 800x600 by default, not a 640x480 panel's:
# measured on an RG40XX-H, the driver scales 800x600 down at 48 fps, while
# 640x480 is paced at two refreshes (30 fps).  window.txt overrides it.
WIN="800x600"
# R36S (RK3326, Mali-G31, 640x480 panel): 640x480 draws directly, no scaling
# pass.  GL calls and the swap run on their own thread, so the simulation and
# the scene walk overlap the driver and the GPU (42 -> 60 fps, same picture;
# costs about one frame of input latency).  RALLY_GL_THREAD=0 turns it off, and
# RALLY_ASYNC_SWAP=1 (swap thread only) is the milder variant.  Other devices:
# untested, left off.
if grep -q "R36S" /proc/cpuinfo 2>/dev/null; then
  WIN="640x480"
  export RALLY_GL_THREAD="${RALLY_GL_THREAD:-1}"
  # the CPU's top operating point (1296 MHz; ArkOS caps at 1248): +4 % fps.
  # Needs root; skipped silently without it, restored on exit.
  CPUF=/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
  CPUF_OLD="$(cat $CPUF 2>/dev/null)"
  CPUF_TOP="$(tr ' ' '\n' < ${CPUF%max_freq}available_frequencies 2>/dev/null | sort -n | tail -n1)"
  [ -n "$CPUF_OLD" ] && [ -n "$CPUF_TOP" ] && echo "$CPUF_TOP" | sudo -n tee $CPUF >/dev/null 2>&1
fi
[ -f "$GAMEDIR/window.txt" ] && WIN="$(head -n1 "$GAMEDIR/window.txt" | tr -d '\r\n ')"

EXTRA=""
[ -f "$GAMEDIR/args.txt" ] && EXTRA="$(head -n1 "$GAMEDIR/args.txt" | tr -d '\r')"

# Anbernic's pads are Nintendo-labelled (A on the right, B at the bottom);
# SDL names buttons by their Xbox position, so tell the game.
case "${DEVICE_NAME:-}" in
  RG*|rg*) case " $EXTRA " in *" --pad-nintendo "*) ;; *) EXTRA="--pad-nintendo $EXTRA" ;; esac ;;
esac

chmod +x "$GAMEDIR/rally.${DEVICE_ARCH}" 2>/dev/null
pm_platform_helper "$GAMEDIR/rally.${DEVICE_ARCH}" >/dev/null

echo "\$ ./rally.${DEVICE_ARCH} --assets game --window $WIN $EXTRA"
# shellcheck disable=SC2086
./rally.${DEVICE_ARCH} --assets game --window "$WIN" $EXTRA
echo "rally exited with $?"
[ -n "${CPUF_OLD:-}" ] && echo "$CPUF_OLD" | sudo -n tee $CPUF >/dev/null 2>&1

pm_finish
