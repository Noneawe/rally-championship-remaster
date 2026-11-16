# Rally Championship Remaster

<p align="center">
  <strong>A modern, playable reimplementation of Mobil 1 Rally Championship (1999),<br>
  for Windows, Linux and Linux handhelds</strong>
</p>

Mobil 1 Rally Championship (1999) is a rally game with long point-to-point stages and
cars that feel like they weigh something. This project rebuilds it so it runs
on modern hardware, including a handheld, while keeping the original's
handling, physics, damage and race rules as faithful as measurement allows.

> **You need your own copy of the game.** This project ships the new engine
> only. The original's data files are copyrighted and are not included, so you
> must supply `RAL.EXE`, `files\` and `var\` from your own copy of Mobil 1 Rally
> Championship.

## Background

Rally Championship was developed by **Magnetic Fields (Software Design) Ltd**
and published by **Actualize Ltd** in 1999. This repository is an independent
preservation and reimplementation effort and is not affiliated with, or
endorsed by, either company or any other rights holder.

From my perspective, I want to thank Magnetic Fields for the unbelievable game
they made. As a child I would spend hours rallying on this game using only the
keyboard on my old family-PC. I always loved the long rally courses and the
handling of the cars, and I hope that I've done justice to one of my all-time
favourites.

## Download

Grab the latest build from the
[**Releases page**](https://github.com/novocast/rally-championship-remaster/releases/latest).
Each release is a set of files, one per platform. Download the one for your
device (the "Source code" files GitHub adds automatically are not what you want).

| Platform | File | Install |
|---|---|---|
| Windows 10/11, 64-bit | `rally-<version>-windows-x64.zip` | [Windows](#windows) |
| Linux desktop, x86-64 (Ubuntu 24.04 or later) | `rally-<version>-linux-x64.tar.gz` | [Linux](#linux-desktop) |
| Linux handhelds with PortMaster (Knulli, muOS, ROCKNIX, ArkOS, ...) | `rally-<version>-portmaster-aarch64.zip` | [Handheld](#handhelds-portmaster) |

Each release also lists a `SHA256SUMS.txt` so you can check your download.

## What's been tested

**Tested** means someone has run a release build on that device and played a
race. **Plays well** means it ran at a comfortable frame rate. An empty box
means nobody has checked yet, not "probably fine".

| Platform | Build | Tested | Plays well |
|---|---|---|---|
| Windows 10/11 x64 | Windows zip | ☑ | ☑ |
| Ubuntu 24.04+ desktop, x86-64 | Linux tarball | ☐ | ☐ |
| Lenovo Legion Go and other Windows handhelds | Windows zip | ☐ | ☐ |
| Anbernic RG40XX-H (Knulli) | Handheld zip | ☑ | ☐ ~48 fps in a race at the default 800x600 |
| Anbernic RG35XX-H / Plus / SP / Pro, RG28XX, RG34XX (Knulli, muOS) | Handheld zip, same chip as the RG40XX-H | ☐ | ☐ |
| Anbernic RG353 / RG503 / RG405 (Knulli, ROCKNIX) | Handheld zip | ☐ | ☐ |
| Other aarch64 Linux handhelds (ArkOS and similar) | Handheld zip | ☐ | ☐ |
| Raspberry Pi 4 / 5 (64-bit Linux) | Handheld zip | ☐ | ☐ |
| Steam Deck and other x86-64 Linux PCs | Linux tarball | ☐ | ☐ |
| Windows on Arm, macOS | Not built | ☐ | ☐ |

The other aarch64 rows use the same binary as the RG40XX-H and should work if
the device has SDL2, an OpenGL ES 2 driver and glibc 2.29 or newer, but that is
a guess until someone confirms it. Screens that are not 4:3 show the game with
black bars at the sides for now.

Known issues: On the handheld the analogue throttle and brake act as on/off
buttons, because Knulli accurately reports the triggers as buttons.

**Got it running on something not ticked here?** Please open an issue with
your device, the release version, and (if possible) the log file (see
[Reporting a problem](#reporting-a-problem)).

---

## Install and play

### Windows

1. Download the Windows zip from the Releases page and unzip it.
2. Copy `rally.exe`, `SDL2.dll` and the `overrides` folder into your Rally
   Championship folder, next to `RAL.EXE`.
3. Double-click `rally.exe`. It finds the game beside itself and opens the
   menus fullscreen.

Alt+Enter switches between fullscreen and a window. Your saves are kept in the
game folder's `var\` folder, as in the original. If something goes wrong, look
at `rally.log` next to `rally.exe`.

**Default keys** (the original's; any you redefined in the original carry over
with your saves):

| Key | Action |
|---|---|
| Up / Down | Throttle / brake |
| Left / Right | Steer |
| `#` (key left of Return) / Right Shift | Gear up / down (gear up after the countdown) |
| `'` | Handbrake |
| `;` (hold) | Reset the car to the last checkpoint |
| Return or F1 | Change camera view |
| P | Pause |
| Esc | Retire (Y / N) |
| F3 / F4 | Volume down / up |
| F12 | Screenshot |

### Linux desktop

1. Download the Linux tarball from the Releases page and unpack it
   (`tar xzf rally-<version>-linux-x64.tar.gz`).
2. Copy `rally`, the `lib` folder and the `overrides` folder into your Rally
   Championship folder, next to `RAL.EXE`.
3. In a terminal, in that folder, run `./rally`. It finds the game beside
   itself and opens the menus fullscreen.

It needs glibc 2.38 or newer (Ubuntu 24.04, Fedora 39, or anything of the
same age or newer) and brings its own SDL2. Keys, saves and `rally.log` work
as on Windows.

### Handhelds (PortMaster)

The handheld build is packaged for [PortMaster](https://portmaster.games/),
which runs on most Linux handheld firmware (Knulli, muOS, ROCKNIX, ArkOS and
others). PortMaster must already be installed on the device.

1. Unzip `rally-<version>-portmaster-aarch64.zip`. It contains
   `Rally Championship.sh` and a `rallychampionship` folder.
2. Copy both into `roms/ports/` on the SD card (on Knulli the share is usually
   `\\KNULLI\share`).
3. Copy your own game files into `roms/ports/rallychampionship/game/`:
   `RAL.EXE`, `files/` and `var/`. That's about 600 MB. Copying your `var/`
   also carries your saves across.
4. On the handheld, refresh the game list (or reboot). Then open
   **Ports → Rally Championship**.

**Controls on the handheld:**

| Button | Menus | Race |
|---|---|---|
| D-pad / left stick | Move | Left stick: steer |
| R2 / L2 | | Throttle / brake |
| A | Select | Handbrake |
| B | Back | Gear up (manual gearbox) |
| X | | Gear down (manual gearbox) |
| Y | | Next camera view |
| Start | Select | Pause |
| Select | Back | Retire |
| L1 (hold) | | Reset car |
| **Select + Start** | **Quit** | **Quit** |

### Reporting a problem

Every run writes a log: `rally.log` next to `rally.exe` or `rally` on Windows
and Linux, `ports/rallychampionship/log.txt` on a handheld. Attach it to a
[GitHub issue](https://github.com/novocast/rally-championship-remaster/issues)
along with the release version, your device, and what you saw. Photos of
anything that looks wrong on screen help a lot.

---

## For developers and the curious

### The approach

This is an evidence-led reverse-engineering project, not a byte-matching
decompilation. The original's logic is reconstructed from measurements of the
running game, while platform-specific systems (DirectDraw, DirectSound,
DirectInput) are replaced with portable equivalents:

- **Replace** legacy Win32 and DirectX services with portable platform layers
  (SDL2, OpenGL / OpenGL ES 2, miniaudio).
- **Rewrite** the file-format parsers as clean, testable C.
- **Reconstruct faithfully** the handling model, physics, damage, AI, race
  rules and replay behaviour that make the game what it is. The original's
  quirks are kept, not "fixed".
- **Keep original assets external**, so players provide their own copy of the
  game data.

### What is in the repository

- `port/`: the game and platform code in C: the renderer, simulation, menus,
  HUD, audio and the `rally` executable, built with CMake.
- `tools/formats/`: the parsers for the original's file formats (`.ro3`
  geometry, `.bun` bundles, levels, textures), used by the game and usable as
  standalone command-line tools.
- `tools/portmaster/`: the PortMaster package for Linux handhelds, and the
  script that builds it.
- `tools/device/`: a diagnostic script for handhelds.
- `tools/release.sh`: builds every release file (Windows, Linux, handheld);
  `tools/release-macos.sh` is the macOS one, run on a Mac (not released yet).
- `overrides/`: replacement art packs.

No part of the original game is included.

### Building from source

See [`port/README.md`](port/README.md). In short, on Linux (or WSL) with
CMake, Ninja, a C compiler and SDL2:

```sh
cmake -S port -B port/build -G Ninja
cmake --build port/build --target rally
```

Windows and aarch64 handheld builds are cross-compiled from Linux; the guide
covers both.

## Licence

- **Code:** [PolyForm Noncommercial 1.0.0](LICENSE). Use it, change it and
  share it for any non-commercial purpose, keeping the credit notice.
- **Replacement art** in `overrides/`: [CC BY-NC 4.0](overrides/LICENSE).
- **Third-party code** in `port/third_party/` keeps its own licence
  (miniaudio and stb_image: public domain or MIT).

"Mobil 1" is a trademark of its owner, and "Rally Championship" and the
original game belong to their respective rights holders. They are used here
only to identify the game this project is compatible with.
