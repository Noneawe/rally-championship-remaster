#!/bin/bash
# release.sh - build and pack everything a GitHub release needs.
#
#   bash tools/release.sh --version 1.1.0 --sdl2-mingw /path/to/SDL2-2.30.0 \
#                         [--sysroot /opt/sysroots/bullseye-arm64] [--out DIR]
#                         [--screenshot FILE] [--cover FILE]
#
# Writes into --out (default dist/<version>/, git-ignored):
#   rally-<version>-windows-x64.zip        rally.exe, SDL2.dll, overrides/, licences
#   rally-<version>-linux-x64.tar.gz       rally, lib/libSDL2-2.0.so.0, overrides/, licences
#   rally-<version>-portmaster-aarch64.zip the PortMaster package (unzip into roms/ports/)
#   portmaster/rallychampionship/          the same, as a folder for a PortMaster pull request
#   SHA256SUMS.txt
#
# Builds from scratch, in port/build-release-win64, port/build-release-linux-x64
# and port/build-release-aarch64.  The Linux desktop build is native: it needs
# libsdl2-dev on the build host, and its glibc floor is the build host's (the
# script prints it).  It carries its own SDL2 (RPATH $ORIGIN/lib).
# The handheld build uses the sysroot given (bullseye's glibc 2.31 reaches older
# firmware such as ArkOS).  Keep this file LF-only.
set -eu
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
VER=""; MINGW=""; SYSROOT="/opt/sysroots/bullseye-arm64"; OUT=""; SHOT=""; COVER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --version)    VER=$2; shift ;;
    --sdl2-mingw) MINGW=$2; shift ;;
    --sysroot)    SYSROOT=$2; shift ;;
    --out)        OUT=$2; shift ;;
    --screenshot) SHOT=$2; shift ;;
    --cover)      COVER=$2; shift ;;
    -h|--help)    sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "release: unknown option $1 (see --help)"; exit 2 ;;
  esac
  shift
done
[ -n "$VER" ]   || { echo "release: give --version"; exit 2; }
[ -n "$MINGW" ] || { echo "release: give --sdl2-mingw (the SDL2 MinGW development package)"; exit 2; }
[ -f "$MINGW/COPYING.txt" ] || { echo "release: $MINGW is not the SDL2 MinGW package"; exit 2; }
pkg-config --exists sdl2 || { echo "release: no SDL2 for the Linux build (sudo apt install libsdl2-dev)"; exit 2; }
[ -d "$SYSROOT/usr/include/SDL2" ] || { echo "release: no SDL2 in sysroot $SYSROOT"; exit 2; }
OUT=${OUT:-$REPO/dist/$VER}
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$REPO"

build() {   # build <dir> <cmake args...>
  local dir=$1; shift
  rm -rf "$dir"
  cmake -S port -B "$dir" -G Ninja "$@" > "$OUT/$(basename "$dir").log" 2>&1
  cmake --build "$dir" >> "$OUT/$(basename "$dir").log" 2>&1 \
    || { echo "release: build failed - see $OUT/$(basename "$dir").log"; exit 1; }
  local w; w=$(grep -c 'warning:' "$OUT/$(basename "$dir").log" || true)
  echo "release: built $dir ($w warnings)"
}

# ---- Windows -------------------------------------------------------------------
W=port/build-release-win64
build $W -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake -DRALLY_SDL2_MINGW="$MINGW"
WZ="$OUT/stage-win/rally-$VER-windows-x64"
mkdir -p "$WZ/licenses"
cp "$W/bin/rally.exe" "$W/bin/SDL2.dll" "$WZ/"
( find overrides -type f ! -name '*.orig.png' ) | while read -r f; do mkdir -p "$WZ/$(dirname "$f")"; cp "$f" "$WZ/$f"; done
cp LICENSE                                   "$WZ/licenses/LICENSE-rally-championship-remaster.txt"
cp overrides/LICENSE                         "$WZ/licenses/LICENSE-overrides.txt"
cp port/third_party/miniaudio/LICENSE        "$WZ/licenses/LICENSE-miniaudio.txt"
sed -n '/^This software is available under 2 licenses/,$p' port/third_party/stb/stb_image.h > "$WZ/licenses/LICENSE-stb_image.txt"
cp "$MINGW/COPYING.txt"                      "$WZ/licenses/LICENSE-SDL2.txt"
cat > "$WZ/README.txt" <<EOF
Rally Championship Remaster $VER - Windows 10/11, 64-bit

You need your own copy of Mobil 1 Rally Championship (1999).

1. Copy rally.exe, SDL2.dll and the overrides folder into your Rally
   Championship folder, next to RAL.EXE.
2. Double-click rally.exe.

Alt+Enter switches between fullscreen and a window.  Saves are kept in the
game's var folder, as in the original.  If something goes wrong, rally.log
next to rally.exe has the details: please attach it to an issue at
https://github.com/novocast/rally-championship-remaster/issues
EOF
sed -i 's/$/\r/' "$WZ/README.txt"
( cd "$OUT/stage-win" && zip -qr "$OUT/rally-$VER-windows-x64.zip" "rally-$VER-windows-x64" )

# ---- Linux desktop (x86-64) ------------------------------------------------------
L=port/build-release-linux-x64
build $L '-DCMAKE_BUILD_RPATH=$ORIGIN/lib'
LZ="$OUT/stage-linux/rally-$VER-linux-x64"
mkdir -p "$LZ/lib" "$LZ/licenses"
cp "$L/game/rally" "$LZ/"
readelf -d "$LZ/rally" | grep -q 'R.*PATH.*\$ORIGIN/lib' || { echo "release: rally has no \$ORIGIN/lib RPATH"; exit 1; }
SDLSO=$(ldd "$LZ/rally" | awk '/libSDL2-2\.0\.so\.0/ {print $3}')
[ -f "$SDLSO" ] || { echo "release: rally does not link libSDL2-2.0.so.0"; exit 1; }
cp -L "$SDLSO" "$LZ/lib/libSDL2-2.0.so.0"
( find overrides -type f ! -name '*.orig.png' ) | while read -r f; do mkdir -p "$LZ/$(dirname "$f")"; cp "$f" "$LZ/$f"; done
cp LICENSE                                   "$LZ/licenses/LICENSE-rally-championship-remaster.txt"
cp overrides/LICENSE                         "$LZ/licenses/LICENSE-overrides.txt"
cp port/third_party/miniaudio/LICENSE        "$LZ/licenses/LICENSE-miniaudio.txt"
sed -n '/^This software is available under 2 licenses/,$p' port/third_party/stb/stb_image.h > "$LZ/licenses/LICENSE-stb_image.txt"
SDLPKG=$(dpkg -S "$(readlink -f "$SDLSO")" 2>/dev/null | cut -d: -f1 | head -1)
if [ -n "$SDLPKG" ] && [ -f "/usr/share/doc/$SDLPKG/copyright" ]; then
  cp "/usr/share/doc/$SDLPKG/copyright" "$LZ/licenses/LICENSE-SDL2.txt"
else
  cp "$MINGW/COPYING.txt"                    "$LZ/licenses/LICENSE-SDL2.txt"
fi
GLIBC=$(objdump -T "$LZ/rally" "$LZ/lib/libSDL2-2.0.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
cat > "$LZ/README.txt" <<EOF
Rally Championship Remaster $VER - Linux desktop, x86-64

You need your own copy of Mobil 1 Rally Championship (1999).
Needs ${GLIBC/_/ } or newer (Ubuntu 24.04 or later, or a distro of the same age).

1. Copy rally, the lib folder and the overrides folder into your Rally
   Championship folder, next to RAL.EXE.
2. In a terminal, in that folder:   ./rally
   (or make the file executable and double-click it, if your file manager
   runs programs).

Alt+Enter switches between fullscreen and a window.  Saves are kept in the
game's var folder, as in the original.  If something goes wrong, rally.log
next to rally has the details: please attach it to an issue at
https://github.com/novocast/rally-championship-remaster/issues
EOF
find "$LZ" -type f -exec chmod 644 {} +; chmod 755 "$LZ/rally"
tar -C "$OUT/stage-linux" -czf "$OUT/rally-$VER-linux-x64.tar.gz" --owner=0 --group=0 "rally-$VER-linux-x64"
echo "release: linux needs $GLIBC"

# ---- handheld (PortMaster) -------------------------------------------------------
H=port/build-release-aarch64
build $H -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
         -DRALLY_SYSROOT="$SYSROOT" -DRALLY_TUNE=cortex-a53
PKG=(--build "$H" --out "$OUT/stage-pm")
[ -n "$SHOT" ]  && PKG+=(--screenshot "$SHOT")
[ -n "$COVER" ] && PKG+=(--cover "$COVER")
bash tools/portmaster/package.sh "${PKG[@]}" | grep -v SHA256 | grep -v '^[0-9a-f]\{64\}'
mv "$OUT/stage-pm/rallychampionship.zip" "$OUT/rally-$VER-portmaster-aarch64.zip"
mv "$OUT/stage-pm/portmaster" "$OUT/portmaster"

rm -rf "$OUT/stage-win" "$OUT/stage-linux" "$OUT/stage-pm"
( cd "$OUT" && sha256sum rally-*.zip rally-*.tar.gz > SHA256SUMS.txt )
echo "release: $OUT"
ls -la "$OUT"/*.zip "$OUT"/*.tar.gz "$OUT/SHA256SUMS.txt"
cat "$OUT/SHA256SUMS.txt"
