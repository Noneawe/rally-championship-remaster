# miniaudio, vendored

The port's mixer library. `port/plat/plat_audio_ma.c`
implements `plat.h` §6 on it, and SDL2's audio device pulls the mix
(`plat_sdl2.c`).

| | |
|---|---|
| upstream | <https://github.com/mackron/miniaudio> |
| release | **0.11.25** (tag `0.11.25`, published 2026-03-03) |
| files | `miniaudio.h`, `miniaudio.c`, `LICENSE`, fetched from the tag on 2026-10-10 |
| licence | public domain (Unlicense) **or** MIT No Attribution, the user's choice (`LICENSE`). No attribution clause, so it ships with the repo as is |
| configuration | `MA_NO_DEVICE_IO MA_NO_FLAC MA_NO_MP3 MA_NO_ENCODING`, PUBLIC on the `rally_miniaudio` target (`port/plat/CMakeLists.txt`). Built with `-w`: it is third-party code, not held to the tree's warning flags |

sha256 of the files as fetched from the tag:

```
ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89  miniaudio.h   (0.11.25, unpatched)
ab1984bb9804ffd7b0303813595d0b345a8a86c34da1daffc353a14b34102a65  miniaudio.c
457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb  LICENSE
```

## Two local patches

`miniaudio.h` here is **0.11.25 plus two patches**, kept as `.patch` files
next to it.

| patch | what was wrong, measured | status upstream |
|---|---|---|
| `0001-end-of-sound-cutoff-5bf32c4.patch` | **The end of every one-shot sound was cut off.** On the non-f32 path, the final read's frames were cached, the sound was marked at its end, and those frames were never mixed. A 1 s 16-bit sample played 47,926 of 48,000 frames: the last 34 source frames (1.5 ms) were dropped at full amplitude, a click. | **Fixed upstream** in commit `5bf32c4` (2026-07-12, issue #1125), unreleased at 0.11.25. This is that fix, verbatim. **Drop the patch when updating to a release that contains it** |
| `0002-resampler-ratio-precision.patch` | **Every pitched voice was flat, by up to 0.1%, and moved in steps.** `ma_resampler_set_rate_ratio` truncated the ratio to thousandths (`d = 1000`). `set_freq(44100)` on a 22,050 Hz sample gave 881.28 Hz, not 882. At low ratios the error and the step grow: 0.16% at 5,000 Hz out of 48,000, so an engine sweep would be quantised. The patch uses `d = 1000000`, the linear resampler's own setter's precision. | **Not fixed upstream** (still `1000` on `dev`, 2026-10-10). A local patch: re-apply on update, or report it upstream |

Updating: fetch the new tag's three files, check the sha256, re-apply 0002,
drop 0001 if the release has `5bf32c4`, then rebuild.
