# Building Rally Championship Remaster

The port is C99, built with CMake. All three targets are built from Linux (a
native Linux machine, or WSL on Windows): Linux natively, and Windows and the
aarch64 handheld by cross-compiling.

| Target | Output | Notes |
|---|---|---|
| Linux x86-64 | `port/build/game/rally` | OpenGL via SDL2 |
| Windows x86-64 | `port/build-win64/bin/rally.exe` + `SDL2.dll` | cross-built with MinGW-w64 |
| aarch64 Linux handheld | `port/build-aarch64-sdl/game/rally` | OpenGL ES 2 via SDL2, glibc 2.29+ |

CMake builds out of source and refuses an in-source build.

## Linux

```sh
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev
cmake -S port -B port/build -G Ninja
cmake --build port/build
```

That builds the game, `port/build/game/rally`, and the six format tools in
`port/build/formats/` (see [`tools/formats/README.md`](../tools/formats/README.md)).

To run it, point it at your own copy of the game:

```sh
port/build/game/rally --assets /path/to/your/RallyChampionship --windowed
```

## Windows (cross-built from Linux)

**Once:** install MinGW-w64 with the posix thread model, and unpack the
official SDL2 MinGW development package (`SDL2-devel-2.30.0-mingw.tar.gz` from
<https://github.com/libsdl-org/SDL/releases>) somewhere outside the repository.

```sh
sudo apt install mingw-w64
sudo update-alternatives --config x86_64-w64-mingw32-gcc   # choose posix
sudo update-alternatives --config x86_64-w64-mingw32-g++   # choose posix
```

**Build:**

```sh
cmake -S port -B port/build-win64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake \
      -DRALLY_SDL2_MINGW=/path/to/SDL2-2.30.0
cmake --build port/build-win64
```

Every executable lands in `port/build-win64/bin/`, beside the `SDL2.dll` that
configure copies there. No MinGW runtime DLL is needed. To play, copy
`rally.exe` and `SDL2.dll` into your game folder beside `RAL.EXE` and
double-click `rally.exe`. Its output goes to `rally.log` beside it.

## aarch64 handheld (cross-built from Linux)

Handheld firmware ships an older glibc than a current Ubuntu, so build against
a **Debian bullseye sysroot**. The result needs only glibc 2.29 and
`libSDL2-2.0.so.0` (it builds against SDL2 2.0.14), which covers older firmware
such as ArkOS as well as Knulli, muOS and ROCKNIX.

**Once:** the cross compiler, and a bullseye arm64 sysroot with SDL2 in it.

```sh
sudo apt install gcc-aarch64-linux-gnu debootstrap debian-archive-keyring
sudo debootstrap --arch=arm64 --foreign --variant=minbase \
     --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
     --include=libsdl2-dev \
     bullseye /opt/sysroots/bullseye-arm64 http://deb.debian.org/debian
# --foreign only downloads; unpack everything it fetched:
cd /opt/sysroots/bullseye-arm64
for d in var/cache/apt/archives/*.deb; do sudo dpkg-deb -x "$d" .; done
# make the absolute library links relative, so they resolve inside the sysroot:
cd usr/lib/aarch64-linux-gnu
for l in $(find . -maxdepth 1 -type l -lname '/*'); do sudo ln -sfn "../../..$(readlink "$l")" "$l"; done
```

**Build:**

```sh
cmake -S port -B port/build-aarch64-sdl -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
      -DRALLY_SYSROOT=/opt/sysroots/bullseye-arm64 -DRALLY_TUNE=cortex-a53
cmake --build port/build-aarch64-sdl
```

**A whole release** (the Windows zip, the PortMaster zip and folder, and
checksums), built from scratch:

```sh
bash tools/release.sh --version 1.1.0 --sdl2-mingw /path/to/SDL2-2.30.0
# -> dist/1.1.0/
```

**Package it for PortMaster:** `tools/portmaster/package.sh` checks the binary
(aarch64, links `libSDL2-2.0.so.0`), records its glibc floor in `port.json`,
and writes the PortMaster folder plus a zip to unpack into a device's
`roms/ports/`:

```sh
bash tools/portmaster/package.sh --build port/build-aarch64-sdl --screenshot /path/to/screenshot-640x480.png
# -> dist/rallychampionship.zip, dist/portmaster/rallychampionship/, dist/SHA256SUMS.txt
```

Then copy your own game files into the device's
`roms/ports/rallychampionship/game/`. `tools/device/RallyDeviceInfo.sh`, run as
a port, writes a report of a device's firmware, libraries and display, which
helps when a new device does not start the game.

## Command-line options

`rally` with no arguments opens the menus, fullscreen, finding the game beside
itself. The most useful options:

| Option | |
|---|---|
| `--assets DIR` | the folder holding `RAL.EXE`, `files/` and `var/` (default: `$RALLY_ASSETS`, then the executable's folder, then `.`) |
| `--windowed`, `--fullscreen` | start in a window, or fullscreen; Alt+Enter switches |
| `--window WxH` | the game's resolution (default 800x600) |
| `--no-vsync` | don't pace the frame rate to the display |
| `--nomusic`, `--nosound` | no music; no sound at all |
| `--no-save` | don't write anything to `var/` (for testing) |

The full list, including the developer options, is in `port/game/rally.c`.
