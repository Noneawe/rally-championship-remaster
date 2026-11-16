## Notes

This port needs your own copy of the original PC game, Mobil 1 Rally
Championship (1999). Copy these from your installation into
`ports/rallychampionship/game/`:

- `RAL.EXE`
- the `files` folder
- the `var` folder (it also holds your saves; copying an existing `var`
  carries your progress across)

That is about 600 MB. Nothing from the original game is included.

The game renders at 800x600 and is scaled to the screen, which measured
faster than rendering at a 640x480 panel's own size. To change it, write a
size such as `640x480` into `ports/rallychampionship/window.txt`. Extra
options can go on one line in `ports/rallychampionship/args.txt` (for
example `--nosound`).

Screens that are not 4:3 show the game with black bars at the sides for now.

If something goes wrong, `ports/rallychampionship/log.txt` has the last run's
output: please attach it to an issue at
https://github.com/novocast/rally-championship-remaster/issues

Thanks to Magnetic Fields for the original game, which this reimplements.
Port and packaging by novocast.

## Controls

| Button | Menus | Race |
|--|--|--|
| D-pad / left stick | Move | Left stick: steer |
| R2 / L2 | | Throttle / brake |
| A | Select | Handbrake |
| B | Back | Gear up (manual gearbox) |
| X | | Gear down (manual gearbox) |
| Y | | Next camera view |
| Start | Select | Pause |
| Select | Back | Retire |
| L1 (hold) | | Reset car |
| Select + Start | Quit | Quit |

## Compile

The source and build instructions are at
https://github.com/novocast/rally-championship-remaster (see `port/README.md`,
"aarch64 handheld"). The package is assembled with
`tools/portmaster/package.sh`.
