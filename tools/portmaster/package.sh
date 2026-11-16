#!/bin/bash
# package.sh - assemble the PortMaster package from an aarch64 build.
#
#   bash tools/portmaster/package.sh [--build DIR] [--out DIR]
#                                    [--screenshot FILE] [--cover FILE]
#
#   --build       the aarch64 build tree (default port/build-aarch64-sdl)
#   --out         where to write (default dist/, git-ignored)
#   --screenshot  a 640x480 gameplay screenshot (PortMaster requires one)
#   --cover       optional cover image
#
# Writes, under --out:
#   portmaster/rallychampionship/   the folder for a PortMaster-New pull request
#   rallychampionship.zip           unzip into the device's roms/ports/ to test
#   SHA256SUMS.txt
#
# Screenshots show the original game's art, so they are kept OUT of this
# repository: pass them from wherever you keep them.
# It REFUSES a binary that is not aarch64 or does not link libSDL2-2.0.so.0,
# and writes the binary's measured glibc floor into port.json's min_glibc.
# Keep this file LF-only.
set -eu
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD="$REPO/port/build-aarch64-sdl"; OUT="$REPO/dist"; SHOT=""; COVER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build)      BUILD=$2; shift ;;
    --out)        OUT=$2; shift ;;
    --screenshot) SHOT=$2; shift ;;
    --cover)      COVER=$2; shift ;;
    -h|--help)    sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "package: unknown option $1 (see --help)"; exit 2 ;;
  esac
  shift
done
die() { echo "package: REFUSED - $*"; exit 1; }

# ---- the binary --------------------------------------------------------------
BIN="$BUILD/game/rally"
[ -f "$BIN" ] || die "no $BIN - build the aarch64 tree first (port/README.md)"
READELF=$(command -v aarch64-linux-gnu-readelf || command -v readelf)
"$READELF" -h "$BIN" | grep -q "AArch64" || die "$BIN is not an aarch64 binary"
"$READELF" -d "$BIN" | grep -q "libSDL2-2.0.so.0" || die "$BIN does not link libSDL2-2.0.so.0"
GLIBC=$("$READELF" -V "$BIN" | grep -o 'GLIBC_2[.0-9]*' | sort -uV | tail -1 | sed 's/GLIBC_//')
echo "package: rally is aarch64, links libSDL2-2.0.so.0, needs glibc $GLIBC"

# ---- the PortMaster folder ---------------------------------------------------
PM="$OUT/portmaster/rallychampionship"
rm -rf "$OUT/portmaster" "$OUT/rallychampionship.zip" "$OUT/SHA256SUMS.txt"
mkdir -p "$PM"
lf() { tr -d '\r' < "$1" > "$2"; }                  # LF-only, whatever the checkout did
SRC="$HERE/rallychampionship"
lf "$SRC/Rally Championship.sh" "$PM/Rally Championship.sh"; chmod +x "$PM/Rally Championship.sh"
lf "$SRC/README.md"     "$PM/README.md"
lf "$SRC/gameinfo.xml"  "$PM/gameinfo.xml"
sed "s/@MIN_GLIBC@/$GLIBC/" "$SRC/port.json" | tr -d '\r' > "$PM/port.json"

G="$PM/rallychampionship"
mkdir -p "$G/game" "$G/licenses"
cp "$BIN" "$G/rally.aarch64"; chmod +x "$G/rally.aarch64"
: > "$G/rally.gptk"                                  # unused: the game reads the pad itself
cat > "$G/game/PUT YOUR GAME FILES HERE.txt" <<'EOF'
Copy these from your own copy of Mobil 1 Rally Championship (1999):

  RAL.EXE
  files/   (the whole folder)
  var/     (the whole folder; it also holds your saves)

so that this folder holds RAL.EXE, files/ and var/.
EOF
# the art packs, without any *.orig.png templates (those are game data)
( cd "$REPO" && find overrides -type f ! -name '*.orig.png' ) | while read -r f; do
  mkdir -p "$G/$(dirname "$f")"; cp "$REPO/$f" "$G/$f"
done
# licences: this project's code and art, and the third-party code it contains
cp "$REPO/LICENSE"                                "$G/licenses/LICENSE-rally-championship-remaster.txt"
cp "$REPO/overrides/LICENSE"                      "$G/licenses/LICENSE-overrides.txt"
cp "$REPO/port/third_party/miniaudio/LICENSE"     "$G/licenses/LICENSE-miniaudio.txt"
sed -n '/^This software is available under 2 licenses/,$p' \
    "$REPO/port/third_party/stb/stb_image.h"      > "$G/licenses/LICENSE-stb_image.txt"
[ -s "$G/licenses/LICENSE-stb_image.txt" ] || die "could not extract stb_image's licence"
# reference copies inside the port folder, as other ports keep them
cp "$PM/README.md" "$PM/gameinfo.xml" "$PM/port.json" "$G/"

if [ -n "$SHOT" ]; then
  [ -f "$SHOT" ] || die "no screenshot $SHOT"
  cp "$SHOT" "$PM/screenshot.${SHOT##*.}"
else
  echo "package: WARNING - no --screenshot: PortMaster requires a 640x480 one"
fi
if [ -n "$COVER" ]; then
  [ -f "$COVER" ] || die "no cover $COVER"
  cp "$COVER" "$PM/cover.${COVER##*.}"; cp "$COVER" "$G/cover.${COVER##*.}"
fi

# ---- the test zip: unzips into roms/ports/ ------------------------------------
( cd "$PM" && zip -qr "$OUT/rallychampionship.zip" "Rally Championship.sh" rallychampionship )
( cd "$OUT" && sha256sum rallychampionship.zip > SHA256SUMS.txt )

echo "package: $PM"
echo "package: $OUT/rallychampionship.zip ($(du -h "$OUT/rallychampionship.zip" | cut -f1))"
cat "$OUT/SHA256SUMS.txt"
