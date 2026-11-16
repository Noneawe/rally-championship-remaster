#!/bin/bash
# RallyDeviceInfo.sh - the data-gathering checklist, run ON the handheld.
#
# Copy this file into the device's ports folder (on knulli: \\KNULLI\share\roms\ports\)
# and launch it from the Ports menu.  It changes nothing on the device: it reads
# system files and lists libraries.  It finishes in a minute or two and returns
# to the menu by itself; the screen stays on the menu meanwhile (nothing is drawn).
#
# Output: ports/rallychampionship/device-info-<date>.txt - attach it to an issue
# when the game will not start on a new device.
#
# Keep this file LF-only: a Windows editor that saves CRLF makes bash refuse it.

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
GAMEDIR="$SCRIPT_DIR/rallychampionship"
LOGDIR="$GAMEDIR"
mkdir -p "$LOGDIR"
OUT="$LOGDIR/device-info-$(date +%Y%m%d-%H%M%S).txt"

# Everything below goes to the log file.
exec > "$OUT" 2>&1

T() { if command -v timeout >/dev/null 2>&1; then timeout "$@"; else shift; "$@"; fi; }
section() { printf '\n==================== %s\n' "$*"; }
run() { printf '\n$ %s\n' "$*"; T 20 sh -c "$*" 2>&1 | head -n 200; }
show() { for f in "$@"; do [ -e "$f" ] && { printf '\n--- %s\n' "$f"; head -n 100 "$f" 2>&1; }; done; }

echo "RallyDeviceInfo v2  $(date)"
echo "script: $SCRIPT_DIR/$(basename "$0")"
echo "user: $(id 2>&1)"

section "PortMaster (control.txt and what it says about this device)"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
for c in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster /userdata/roms/ports/PortMaster; do
    if [ -f "$c/control.txt" ]; then controlfolder="$c"; break; fi
done
echo "controlfolder=${controlfolder:-NOT FOUND}"
if [ -n "${controlfolder:-}" ]; then
    (
        # shellcheck disable=SC1091
        source "$controlfolder/control.txt" >/dev/null 2>&1
        [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt" >/dev/null 2>&1
        get_controls >/dev/null 2>&1
        for v in CFW_NAME CFW_VERSION DEVICE_NAME DEVICE_ARCH DEVICE_CPU DEVICE_RAM \
                 DISPLAY_WIDTH DISPLAY_HEIGHT DISPLAY_ORIENTATION ASPECT_X ASPECT_Y \
                 ANALOG_STICKS directory ESUDO GPTOKEYB GPTOKEYB2 sdl_controllerconfig; do
            # whole values: sdl_controllerconfig holds several newline-separated
            # mappings, and v1's 300-character cut hid the device's own (2026-10-18)
            printf '%s=%s\n' "$v" "$(eval echo "\"\${$v:-}\"" | head -c 4000)"
        done
        type pm_platform_helper >/dev/null 2>&1 && echo "pm_platform_helper: present"
        type pm_finish          >/dev/null 2>&1 && echo "pm_finish: present"
    )
    run "ls '$controlfolder' | head -n 80"
    run "ls '$controlfolder/libs' 2>/dev/null | head -n 80"
fi

section "OS and distribution"
run "uname -a"
show /etc/os-release /etc/issue /usr/share/batocera/batocera.version /usr/share/knulli/knulli.version
run "ls /usr/share | grep -i -E 'batocera|knulli|recalbox|rocknix|muos|portmaster'"
run "cat /proc/cmdline"

section "CPU"
run "nproc"
show /proc/cpuinfo
run "for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \$p; for f in scaling_governor scaling_available_governors scaling_cur_freq scaling_min_freq scaling_max_freq cpuinfo_max_freq scaling_available_frequencies; do printf '  %s: ' \$f; cat \$p/\$f 2>/dev/null || echo -; done; done"
run "for z in /sys/class/thermal/thermal_zone*; do printf '%s %s %s\n' \$z \$(cat \$z/type 2>/dev/null) \$(cat \$z/temp 2>/dev/null); done"
run "ls /sys/class/devfreq/ 2>/dev/null; for d in /sys/class/devfreq/*; do printf '%s governor=%s cur=%s max=%s\n' \$d \$(cat \$d/governor 2>/dev/null) \$(cat \$d/cur_freq 2>/dev/null) \$(cat \$d/max_freq 2>/dev/null); done"

section "Memory"
run "free -m"
show /proc/meminfo

section "Storage"
run "df -h"
run "mount | grep -E 'userdata|mmc|/sd|share|roms' "

section "libc (decides the build's sysroot)"
run "ldd --version 2>&1 | head -n 3"
run "ls -l /lib/ld-* /lib64/ld-* /usr/lib/ld-* 2>/dev/null"
run "ls -l /lib/libc.so* /usr/lib/libc.so* /lib/aarch64-linux-gnu/libc.so* /usr/lib/aarch64-linux-gnu/libc.so* 2>/dev/null"
for libc in /lib/libc.so.6 /usr/lib/libc.so.6 $(find /lib /usr/lib -maxdepth 2 -name 'libc.so.6' 2>/dev/null); do
    if [ -f "$libc" ]; then
        echo "--- $libc: GLIBC symbol versions (highest last)"
        grep -ao 'GLIBC_2\.[0-9]*' "$libc" | sort -u | sort -t. -k2 -n | tail -n 4
        echo "--- $libc run directly:"
        T 5 "$libc" 2>&1 | head -n 2
        break
    fi
done
run "ls /lib/ld-musl* /usr/lib/libc.musl* 2>/dev/null || echo 'no musl loader'"

section "SDL2 (the port links libSDL2-2.0.so.0)"
run "find /usr/lib /lib /usr/local/lib /opt ${controlfolder:+'$controlfolder'} -maxdepth 4 -name 'libSDL2*' 2>/dev/null | head -n 40"
run "ls -l \$(find /usr/lib /lib -maxdepth 3 -name 'libSDL2-2.0.so*' 2>/dev/null) 2>/dev/null"
run "pkg-config --modversion sdl2 2>/dev/null || echo 'no pkg-config'"
run "find / -xdev -maxdepth 6 -name 'gamecontrollerdb*.txt' 2>/dev/null | head -n 10"

section "GPU, GLES, EGL, display"
run "find /usr/lib /lib /usr/local/lib /opt -maxdepth 4 \\( -name 'libGLESv2*' -o -name 'libEGL*' -o -name 'libmali*' -o -name 'libgbm*' -o -name 'libMali*' \\) 2>/dev/null | head -n 40"
run "for f in \$(find /usr/lib /lib -maxdepth 3 -name 'libGLESv2.so*' 2>/dev/null | head -n 3); do echo \$f; grep -ao -m3 -E 'Mesa [0-9][0-9.]*|panfrost|lima|Mali-[A-Z0-9]+|r[0-9]+p[0-9]+' \$f | sort -u | head -n 8; done"
run "ls -l /dev/dri /dev/mali* /dev/fb* 2>/dev/null"
run "for c in /sys/class/drm/card*-*; do printf '%s status=%s\n' \$c \$(cat \$c/status 2>/dev/null); cat \$c/modes 2>/dev/null; done"
show /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/name
run "dmesg 2>/dev/null | grep -i -E 'mali|panfrost|lima|drm|gpu|display|panel' | tail -n 40"
run "lsmod 2>/dev/null | head -n 60"
run "command -v glmark2-es2 glmark2-es2-drm kmscube eglinfo es2_info 2>/dev/null"

section "Audio"
show /proc/asound/cards /proc/asound/pcm
run "command -v aplay >/dev/null && aplay -l"
run "ps 2>/dev/null | grep -i -E 'pipewire|pulse|wireplumber' | grep -v grep"

section "Input (built-in pad)"
show /proc/bus/input/devices
run "ls -l /dev/input /dev/input/by-id 2>/dev/null"
run "ls -l /dev/uinput 2>/dev/null"

section "Environment and processes"
run "env | sort | grep -v -i -E 'pass|token|secret|key='"
run "ps 2>/dev/null | head -n 60"
run "command -v ssh sshd dropbear python3 gdb strace perf 2>/dev/null"

section "The port (ports/rallychampionship/)"
run "ls -la '$GAMEDIR' 2>/dev/null | head -n 40"
if [ -f "$GAMEDIR/rally.aarch64" ]; then
    run "ldd '$GAMEDIR/rally.aarch64' 2>&1"
fi
show "$GAMEDIR/log.txt"

section "done"
echo "wrote $OUT"
sync
