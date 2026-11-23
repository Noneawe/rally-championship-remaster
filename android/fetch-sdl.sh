#!/bin/sh
# Fetches SDL2 into android/SDL2 (not kept in the repo).
set -e
V=2.32.10
cd "$(dirname "$0")"
[ -d SDL2 ] && exit 0
curl -fL -o SDL2.tgz "https://github.com/libsdl-org/SDL/releases/download/release-$V/SDL2-$V.tar.gz"
tar xf SDL2.tgz && mv "SDL2-$V" SDL2 && rm SDL2.tgz
