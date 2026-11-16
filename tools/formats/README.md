# `tools/formats` - the file-format parsers

Parsers for the original game's file formats, in plain C99. The game links
them as the `rally_formats` library, and each format also has a small
command-line tool for inspecting your own copy of the game's files.

They are built by the port's CMake build (see [`port/README.md`](../../port/README.md)):

```sh
cmake -S port -B port/build -G Ninja
cmake --build port/build
ls port/build/formats/          # ro3  bun  pcx  sq  cdat  localdat
```

## What is here

| Files | Format |
|---|---|
| `ro3.h` / `ro3.c` | `.ro3` 3D models (cars, objects, scenery) |
| `lwo.h` / `lwo.c` | just enough LightWave Object to read the paired `.lwo` files |
| `bun.h` / `bun.c` | `.bun` sprite bundles: the container and its run-length sprite data |
| `pcx.h` / `pcx.c` | `.pcx` images (texture sheets, menu art) |
| `png.h` / `png.c` | a PNG decoder, for the menus' background screens |
| `sq.h` / `sq.c` | `.sq` containers, in both flavours (SQSL bitstream and SQSM tile grid) |
| `cdat.h` / `cdat.c` | `cdat00c` stage geometry: the cell partition, polygons and points |
| `localdat.h` / `localdat.c` | `LOCALDAT`, a stage's texture set and asset manifest |
| `pyrandom.h` / `pyrandom.c` | a Mersenne Twister matching Python's `random`, used by `ro3 normals` |
| `corpus.h` / `corpus.c` | walking a folder for files of one type (tools only) |
| `*_tool.c` | the command-line tools |

## The tools

Run any tool with no arguments for its usage. For example:

```sh
ro3 info      path/to/OBJ0001.RO3
ro3 obj       path/to/OBJ0001.RO3 -o car.obj       # export to Wavefront .obj
ro3 export    path/to/game -o /tmp/ro3-tree        # every model, mirroring the tree
bun list      path/to/file.bun
pcx decodefile path/to/sheet.pcx
sq info       path/to/LEVEL01.SQ
cdat info     path/to/cdat00c
localdat info path/to/LOCALDAT
```

`export` mirrors the source folders rather than naming output by basename:
different folders hold different models with the same file name.

Anything you export is derived from the original game's data, so keep it out
of this repository.

## House rules

- **A parser takes a buffer**, never a path or a `FILE*`. Everything that
  knows about the file system is in `corpus.c` and the tools.
- **Layout constants are named**, in the headers.
- **The accessors in `cdat.h` do not bounds-check**, deliberately: the caller
  checks `p->fits` once. Read `cdat.h`'s contract before using them.
- `_POSIX_C_SOURCE=200809L` is load-bearing: without it `strdup` is implicitly
  declared under `-std=c99` and its pointer is truncated on a 64-bit host.
