#!/bin/bash
# release-macos.sh - build and pack the macOS release.  Run ON a Mac:
# Apple's SDK cannot be used from Linux, so release.sh cannot make this one.
#
#   bash tools/release-macos.sh --version 1.1.0 [--arch x86_64|arm64] [--out DIR]
#
# Needs: Xcode command-line tools, and  brew install cmake ninja sdl2 pkg-config
#
# Writes into --out (default dist/<version>/, git-ignored):
#   rally-<version>-macos-<arch>.tar.gz   rally, lib/libSDL2-2.0.0.dylib, overrides/, licences
#   SHA256SUMS-macos.txt
#
# --arch defaults to x86_64 (an Intel Mac; an Apple Silicon Mac runs it under
# Rosetta 2).  arm64 is NOT a release yet: Apple arm64 has no 80-bit long double, so its
# physics may differ from x86.  The tarball's macOS floor is Homebrew SDL2's
# (printed).  Keep this file LF-only.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
VER=""; ARCH="x86_64"; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --version) VER=$2; shift ;;
    --arch)    ARCH=$2; shift ;;
    --out)     OUT=$2; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "release-macos: unknown option $1 (see --help)"; exit 2 ;;
  esac
  shift
done
[ -n "$VER" ] || { echo "release-macos: give --version"; exit 2; }
[ "$(uname -s)" = Darwin ] || { echo "release-macos: run this on a Mac"; exit 2; }
case "$ARCH" in x86_64|arm64) ;; *) echo "release-macos: --arch x86_64 or arm64"; exit 2 ;; esac
[ "$(uname -m)" = "$ARCH" ] || { echo "release-macos: this Mac is $(uname -m), not $ARCH (build on the matching Mac)"; exit 2; }
pkg-config --exists sdl2 || { echo "release-macos: no SDL2 (brew install sdl2 pkg-config)"; exit 2; }
OUT=${OUT:-$REPO/dist/$VER}
mkdir -p "$OUT"
cd "$REPO"

B=port/build-release-macos-$ARCH
rm -rf "$B"
cmake -S port -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$OUT/$(basename "$B").log" 2>&1
cmake --build "$B" >> "$OUT/$(basename "$B").log" 2>&1 \
  || { echo "release-macos: build failed - see $OUT/$(basename "$B").log"; exit 1; }
echo "release-macos: built $B ($(grep -c 'warning:' "$OUT/$(basename "$B").log" || true) warnings)"

NAME="rally-$VER-macos-$ARCH"
ST="$OUT/stage-macos/$NAME"
rm -rf "$OUT/stage-macos"; mkdir -p "$ST/lib" "$ST/licenses"
cp "$B/game/rally" "$ST/"

# Bundle SDL2 and point the binary at the copy beside it.
SDLREF=$(otool -L "$ST/rally" | awk '/libSDL2-2\.0\.0\.dylib/ {print $1}')
[ -n "$SDLREF" ] || { echo "release-macos: rally does not link libSDL2-2.0.0.dylib"; exit 1; }
SDLREAL=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SDLREF")
cp "$SDLREAL" "$ST/lib/libSDL2-2.0.0.dylib"
chmod 644 "$ST/lib/libSDL2-2.0.0.dylib"
install_name_tool -id "@executable_path/lib/libSDL2-2.0.0.dylib" "$ST/lib/libSDL2-2.0.0.dylib"
install_name_tool -change "$SDLREF" "@executable_path/lib/libSDL2-2.0.0.dylib" "$ST/rally"
# install_name_tool invalidates the ad-hoc signature arm64 requires; re-sign ad hoc.
codesign --force -s - "$ST/lib/libSDL2-2.0.0.dylib" "$ST/rally"
if otool -L "$ST/rally" "$ST/lib/libSDL2-2.0.0.dylib" | grep -E '/(usr/local|opt/homebrew)/'; then
  echo "release-macos: a Homebrew path is still referenced (above)"; exit 1
fi

( find overrides -type f ! -name '*.orig.png' ) | while read -r f; do mkdir -p "$ST/$(dirname "$f")"; cp "$f" "$ST/$f"; done
cp LICENSE                            "$ST/licenses/LICENSE-rally-championship-remaster.txt"
cp overrides/LICENSE                  "$ST/licenses/LICENSE-overrides.txt"
cp port/third_party/miniaudio/LICENSE "$ST/licenses/LICENSE-miniaudio.txt"
sed -n '/^This software is available under 2 licenses/,$p' port/third_party/stb/stb_image.h > "$ST/licenses/LICENSE-stb_image.txt"
SDLLIC=$(find "$(brew --prefix sdl2)/" -maxdepth 1 \( -name 'LICENSE*' -o -name 'COPYING*' \) | head -1)
[ -n "$SDLLIC" ] || { echo "release-macos: SDL2's licence not found under $(brew --prefix sdl2)"; exit 1; }
cp "$SDLLIC" "$ST/licenses/LICENSE-SDL2.txt"

MINOS=$(otool -l "$ST/rally" "$ST/lib/libSDL2-2.0.0.dylib" | awk '/minos/ {print $2}' | sort -V | tail -1)
cat > "$ST/README.txt" <<EOF
Rally Championship Remaster $VER - macOS ($ARCH), macOS $MINOS or later
$( [ "$ARCH" = x86_64 ] && echo "Intel Macs, and Apple Silicon Macs through Rosetta 2." )

You need your own copy of Mobil 1 Rally Championship (1999).

1. Copy rally, the lib folder and the overrides folder into your Rally
   Championship folder, next to RAL.EXE.
2. The download is not signed, so macOS blocks it the first time.  In
   Terminal, in that folder, run:
       xattr -dr com.apple.quarantine rally lib
3. Then run:   ./rally

Alt+Enter switches between fullscreen and a window.  Saves are kept in the
game's var folder, as in the original.  If something goes wrong, rally.log
next to rally has the details: please attach it to an issue at
https://github.com/novocast/rally-championship-remaster/issues
EOF
find "$ST" -type f -exec chmod 644 {} +; chmod 755 "$ST/rally"
tar -C "$OUT/stage-macos" -czf "$OUT/$NAME.tar.gz" "$NAME"
rm -rf "$OUT/stage-macos"
( cd "$OUT" && shasum -a 256 rally-*-macos-*.tar.gz > SHA256SUMS-macos.txt )
echo "release-macos: $OUT/$NAME.tar.gz (macOS $MINOS or later)"
cat "$OUT/SHA256SUMS-macos.txt"
