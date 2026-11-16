/* weather.h - the step's half of the weather: wetness, rain, snow, wind and
 * the thunder's draw.
 * sections 1-2 (the reading); (FUN_0043A16A, first read
 * for wetns alone).
 *
 *   FUN_0043A16A  in the step, after the surface solver:
 *                 two samples of the fixed noise map at the CAMERA's world
 *                 position, lerped between per-level LOCALDAT bounds -
 *                 car+0x900 rain, +0x904 snow (LD+0x698 picks which of the
 *                 two keeps the value), +0x908 wind, and wetns car+0x280.
 *   0x0045A31E    after the slot loop (0x0045E5D8's third call): car 0's
 *                 words into the audio thread's levels [0x00510354] (rain)
 *                 and [0x00510358] (wind), and the thunder flag
 *                 [0x00510340], one rnd_next() a step while it pours.
 *
 * The audio thread's half (0x0040E0CC) is port/game/worldsound.c. */
#ifndef RALLY_SIM_WEATHER_H
#define RALLY_SIM_WEATHER_H

#include <stddef.h>
#include <stdint.h>

#include "car.h"

/* The level's six LOCALDAT words FUN_0043A16A reads (the file is an image of
 * 0x005BB970..): 0 where the file is too short for them
 * (LEVEL11-16 end at 8,248 bytes - what the original holds there depends on
 * a clear of the block that is not read). */
typedef struct sim_weather_level {
    int32_t rain_lo, rain_hi;   /* LD+0x690 / +0x694  [0x005BC000] / [0x005BC004] */
    int32_t snow;               /* LD+0x698  [0x005BC008]: non-zero = snow       */
    int32_t wet_floor;          /* LD+0x205C [0x005BD9CC]                        */
    int32_t wind_lo, wind_hi;   /* LD+0x2064 / +0x2068 [0x005BD9D4] / [0x005BD9D8] */
    const uint8_t *noise;       /* [0x009CAEE4], 0x1070 bytes (stage_fog_noise) */
} sim_weather_level;

/* The words, car+0x900..+0x908: above the replayed prefix, so in sim_world. */
typedef struct sim_weather_words {
    int32_t rain;               /* +0x900  0..255                                */
    int32_t snow;               /* +0x904  0..255                                */
    int32_t wind;               /* +0x908  0..255                                */
} sim_weather_words;

/* What 0x0045A31E leaves for the audio thread. */
typedef struct sim_weather {
    int32_t rain_vol;           /* [0x00510354]  clamp((rain >> 2) - 4, 0, 64)  */
    int32_t wind_vol;           /* [0x00510358]  clamp((wind^2 >> 12) - 8, 0, 64) */
    int32_t thunder;            /* [0x00510340]  1 = a clap is due; the audio
                                 * thread clears it                              */
    long    draws;              /* rnd_next() calls made (diagnostic)            */
    long    claps;              /* times the flag was set (diagnostic)           */
} sim_weather;

/* The level's words out of a LOCALDAT image `ld` of `len` bytes. */
void sim_weather_level_from_localdat(sim_weather_level *L, const uint8_t *ld,
                                     size_t len, const uint8_t *noise);

/* FUN_0044DD01 on the map at `map` (the caller adds FUN_0043A16A's 0x50 or
 * 0x20): a bilinear sample at the 16.16 world (x, z), shifted left 8. */
int32_t sim_weather_noise_q(const uint8_t *map, int32_t x, int32_t z);

/* FUN_0043A16A: `campos` is car+0x6A4..+0x6AC.  Writes car->wetns and `w`. */
void sim_weather_sample_q(const sim_weather_level *L, const int32_t campos[3],
                          car_state *car, sim_weather_words *w);

/* 0x0045A31E: car 0's words.  `rnd` is rnd_next's state [0x004DB13C] (the
 * sim's: objects.c's s->rnd); NULL skips the draw (and so the thunder). */
void sim_weather_tick_q(sim_weather *s, const sim_weather_words *car0,
                        uint32_t *rnd);

#endif
