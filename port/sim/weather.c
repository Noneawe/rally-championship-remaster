/* weather.c - weather.h.  Each function names the
 * original's it transcribes (tools/ralimg.py disasm). */

#include "weather.h"
#include "physics.h"

#include <string.h>

static int32_t ld32(const uint8_t *ld, size_t len, size_t off)
{
    uint32_t v;
    if (!ld || off + 4 > len) return 0;
    memcpy(&v, ld + off, 4);
    return (int32_t)v;
}

void sim_weather_level_from_localdat(sim_weather_level *L, const uint8_t *ld,
                                     size_t len, const uint8_t *noise)
{
    L->rain_lo   = ld32(ld, len, 0x690);
    L->rain_hi   = ld32(ld, len, 0x694);
    L->snow      = ld32(ld, len, 0x698);
    L->wet_floor = ld32(ld, len, 0x205C);
    L->wind_lo   = ld32(ld, len, 0x2064);
    L->wind_hi   = ld32(ld, len, 0x2068);
    L->noise     = noise;
}

/* FUN_0044DD01 (stage.c's fog_noise_sample, on a caller's base): the tile
 * from the top byte of each coordinate, the +1 before the AND 0x3F, three
 * lerps on the 0.16 fractions (IMUL / SHRD 16), SHL 8.  The shifts are SHR. */
int32_t sim_weather_noise_q(const uint8_t *map, int32_t x, int32_t z)
{
    uint32_t ix = (uint32_t)x >> 24, iz = (uint32_t)z >> 24;
    int32_t  fx = (int32_t)(((uint32_t)x >> 8) & 0xffffu);
    int32_t  fz = (int32_t)(((uint32_t)z >> 8) & 0xffffu);
    int32_t  t00, t01, t10, t11, a, b;
#define TAP(TX, TZ) ((int32_t)map[((((uint32_t)(TZ)) & 63u) << 6) | (((uint32_t)(TX)) & 63u)])
    t00 = TAP(ix, iz);
    t01 = TAP(ix + 1u, iz);
    t10 = TAP(ix, iz + 1u);
    t11 = TAP(ix + 1u, iz + 1u);
#undef TAP
    a = t00 + (int32_t)(((int64_t)(t01 - t00) * fx) >> 16);
    b = t10 + (int32_t)(((int64_t)(t11 - t10) * fx) >> 16);
    return (a + (int32_t)(((int64_t)(b - a) * fz) >> 16)) << 8;
}

/* lo + (hi - lo) * t: IMUL EBX / SHRD EAX,EDX,16 / ADD - the low dword of
 * the 64-bit product shifted 16 - then CMP 0 / JNS and CMP 0xFF / JS. */
static int32_t lerp_clamp(int32_t lo, int32_t hi, int32_t t)
{
    int64_t p = (int64_t)(int32_t)((uint32_t)hi - (uint32_t)lo) * t;
    int32_t a = (int32_t)((uint32_t)(uint64_t)(p >> 16) + (uint32_t)lo);
    if (a < 0) a = 0;
    if (a - 0xFF >= 0) a = 0xFF;
    return a;
}

void sim_weather_sample_q(const sim_weather_level *L, const int32_t campos[3],
                          car_state *car, sim_weather_words *w)
{
    int32_t a, b, e;
    float   wet;
    /* 0x0043A16A: ESI = 0x009CAF34 = the map + 0x50 */
    a = lerp_clamp(L->rain_lo, L->rain_hi,
                   sim_weather_noise_q(L->noise + 0x50, campos[0], campos[2]));
    w->rain = w->snow = a;                              /* 0x0043A1A3 / A8 */
    /* 0x0043A1AD: the map + 0x20 */
    b = lerp_clamp(L->wind_lo, L->wind_hi,
                   sim_weather_noise_q(L->noise + 0x20, campos[0], campos[2]));
    w->wind = b;                                        /* 0x0043A1E6      */
    e = b;
    if ((uint32_t)e < (uint32_t)L->wet_floor) e = L->wet_floor;  /* JAE    */
    /* FILD / FDIV [0x0057B0B0] (256.0) / FSTP */
    wet = (float)((double)e / 256.0);
    memcpy(&car->wetns, &wet, sizeof wet);
    if (L->snow != 0) w->rain = 0;                      /* 0x0043A218      */
    else              w->snow = 0;                      /* 0x0043A224      */
}

void sim_weather_tick_q(sim_weather *s, const sim_weather_words *car0,
                        uint32_t *rnd)
{
    int32_t v;
    /* [0x006C2A10] SHR 2, SUB 4, clamp 0..64 (JNS / JS) */
    v = (int32_t)(((uint32_t)car0->rain >> 2) - 4u);
    if (v < 0) v = 0;
    if (v - 0x40 >= 0) v = 0x40;
    s->rain_vol = v;
    /* CMP [0x00510354],0x38 / JB: 56 or more, one rnd_next() a step, and
     * IMUL 500 / EDX == 0 - a non-negative draw below 2^32 / 500 */
    if ((uint32_t)v >= 0x38u && rnd) {
        int64_t p = (int64_t)(int32_t)sim_rnd_next_q(rnd) * 500;
        s->draws++;
        if ((int32_t)(p >> 32) == 0) {
            s->thunder = 1;
            s->claps++;
        }
    }
    /* [0x006C2A18]: IMUL EAX (squared), SHR 12, SUB 8, clamp 0..64 */
    v = (int32_t)((uint32_t)(car0->wind * car0->wind) >> 12) - 8;
    if (v < 0) v = 0;
    if (v - 0x40 >= 0) v = 0x40;
    s->wind_vol = v;
}
