/* spray.c - the wheels' spray; spray.h says what is the
 * original's and what is not, is the reading.
 * Every address is retail's.  Each function names the original's address;
 * the arithmetic follows its x87 sequence (an FSTP is a float store, the rest
 * long double), its integer stores (FISTP) round to nearest even. */

#include "spray.h"

#include "image.h"
#include "physics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext.h"

/* ---- the memory -------------------------------------------------------- */

uint8_t *spray_at(spray *s, uint32_t va)
{
    if (va >= SPRAY_D0 && va + 4 <= SPRAY_D1) return s->d + (va - SPRAY_D0);
    if (va >= SPRAY_K0 && va + 4 <= SPRAY_K1) return s->k + (va - SPRAY_K0);
    if (va >= SPRAY_BLOCK && va + 4 <= SPRAY_BLOCK + SPRAY_BLOCK_LEN)
        return s->block + (va - SPRAY_BLOCK);
    if (va >= SPRAY_POOL_VA && va + 4 <= SPRAY_POOL_VA + sizeof s->pool)
        return s->pool + (va - SPRAY_POOL_VA);
    if (va >= SPRAY_MAT_VA && va + 4 <= SPRAY_MAT_VA + sizeof s->mats)
        return s->mats + (va - SPRAY_MAT_VA);
    if (va >= SPRAY_T0 && va + 4 <= SPRAY_T1) return s->t + (va - SPRAY_T0);
    if (va >= SPRAY_VIEW_VA && va + 4 <= SPRAY_VIEW_VA + sizeof s->view)
        return (uint8_t *)s->view + (va - SPRAY_VIEW_VA);
    if (va == 0x006CF334u) return (uint8_t *)s->lcg;
    if (va == 0x008C38C4u) return (uint8_t *)&s->jit;
    return NULL;
}

static uint8_t dummy[8];

static uint8_t *A(spray *s, uint32_t va)
{
    uint8_t *p = spray_at(s, va);
    return p ? p : dummy;                /* an address outside the module:
                                          * the gate checks none is reached */
}

static int32_t I(spray *s, uint32_t va)
{
    int32_t v;
    memcpy(&v, A(s, va), 4);
    return v;
}

static uint32_t U(spray *s, uint32_t va) { return (uint32_t)I(s, va); }

static float F(spray *s, uint32_t va)
{
    float v;
    memcpy(&v, A(s, va), 4);
    return v;
}

static void WI(spray *s, uint32_t va, int32_t v) { memcpy(A(s, va), &v, 4); }
static void WU(spray *s, uint32_t va, uint32_t v) { memcpy(A(s, va), &v, 4); }
static void WF(spray *s, uint32_t va, ext v)
{
    float f = (float)v;                  /* FSTP dword                      */
    memcpy(A(s, va), &f, 4);
}

static int32_t fistp(ext x)              /* FISTP, round to nearest even    */
{
    if (!(x >= -EXT_C(2147483648.0) && x < EXT_C(2147483648.0)))
        return INT32_MIN;                /* the integer indefinite          */
    return (int32_t)ext_llrint(x);
}

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t ror(uint32_t v, unsigned n)
{
    return (v >> n) | (v << (32u - n));
}

/* the per-site generators: [a] = [a] * 0xC5 + 0x5D */
static uint32_t R(spray *s, uint32_t va)
{
    uint32_t r = U(s, va) * 0xC5u + 0x5Du;
    WU(s, va, r);
    return r;
}

/* the shared one, [0x006CF334] = [0x006CF334] * 0x9D + 0x47 */
static uint32_t L(spray *s)
{
    *s->lcg = *s->lcg * 0x9Du + 0x47u;
    return *s->lcg;
}

/* x87 FCOM / FCOMP of st0 against m: C0 (st0 < m), the only bit the module
 * tests through "and 0x700 / cmp 0x100" (unordered sets C0 C2 C3, != 0x100) */
static int below(ext a, ext m) { return a < m; }

/* ---- the addresses ----------------------------------------------------- */

enum {
    G_WHEEL   = 0x008D54BEu,     /* the wheel's index 0..3 (2, 3 the body)   */
    G_SIDE    = 0x008D54DAu,     /* grass / snow: -1 or 1                    */
    G_S       = 0x008D54DEu,     /* the car's S                              */
    G_FWD     = 0x008D54E2u,     /* the wheel's backward axis (3 floats)     */
    G_UP      = 0x008D54EEu,     /* the constant the fronts cross (3)        */
    G_CR      = 0x008D54FAu,     /* the cross, normalised (3)                */
    G_SIDEAX  = 0x008D5506u,     /* the wheel's sideways axis (3)            */
    G_SPARK0  = 0x008D5512u,     /* type 0x12's velocity (3)                 */
    G_FOOT_X  = 0x008D551Eu,     /* the footprint, half x, 16.16 int         */
    G_FOOT_Z  = 0x008D5522u,     /* half z                                   */
    G_CAROFF  = 0x008D5526u,     /* car * 0x4C8, each particle's +8          */
    G_SNAPLEN = 0x008D5586u,
    G_POOL    = 0x008D5700u,     /* the head                                 */
    G_SIZE0   = 0x008D5718u,     /* dry smoke's alpha                        */
    G_LIFE0   = 0x008D571Cu,     /* dry smoke's life                         */
    G_FX      = 0x008D5720u,     /* |local x|, |local z| (the footprint)     */
    G_FZ      = 0x008D5724u,
    G_V       = 0x008D5728u,     /* the throw's velocity (3)                 */
    G_LAT     = 0x008D5734u,     /* the sideways throw's s                   */
    G_BACK    = 0x008D5738u,     /* the backward throw's s                   */
    G_SLIP    = 0x008D574Cu,
    G_ACC     = 0x008D5750u,
    G_T       = 0x008D5760u,     /* the module's scratch                     */
    G_COUNT   = 0x008D5768u,     /* the live count                           */
    G_REL     = 0x008D57B8u,     /* a particle relative to the car (3)       */
    G_RELV    = 0x008D57C4u,
    G_JX      = 0x008D55F2u,     /* the jitters                              */
    G_JZ      = 0x008D55F6u,
    G_TMP     = 0x008D5606u,
    G_CELL    = 0x008D9C4Eu,     /* the wheel's marks byte                   */
    G_SPRAYB  = 0x008D9C4Au,     /* the wheel's spray byte                   */
    G_RAIN    = 0x008DA276u,     /* [0x0085CDC5], the rain                   */
    G_SMOKE   = 0x008DA26Eu,     /* the smoke level 0..7                     */
    G_CARV    = 0x008DA262u,     /* the car's velocity (3)                   */
    K_DT      = 0x0070D340u,     /* ms, 0 while paused                       */
    K_SPIN    = 0x0070D344u,
    K_INVDT   = 0x0070D350u,
    K_ONE     = 0x0070D4DCu,
    K_TMP     = 0x0070D4A8u
};

#define BLK(s, off)  (SPRAY_BLOCK + (off))

/* ---- the pool, 0x004BAE77 / 0x004BB29A ----------------------------------- */

void spray_race_start(spray *s)
{
    uint32_t i;
    WU(s, 0x008D5708u, SPRAY_POOL_VA);
    WU(s, G_POOL, SPRAY_POOL_VA);
    WI(s, G_COUNT, 0);
    for (i = 0; i < SPRAY_POOL_N; i++) {
        uint32_t r = SPRAY_POOL_VA + i * SPRAY_REC;
        WU(s, r, SPRAY_POOL_VA + ((i - 1u) & 0x3FFu) * SPRAY_REC);
        WU(s, r + 4, SPRAY_POOL_VA + ((i + 1u) & 0x3FFu) * SPRAY_REC);
    }
}

/* every emitter's opening: EAX = S+0x1C & 0xFFFFFF, the count against 0x3FE,
 * the record before the head becomes the head.  0 when the pool is full. */
static uint32_t take(spray *s, uint32_t S, int32_t type)
{
    uint32_t col = U(s, S + 0x1C) & 0xFFFFFFu, p;
    if (I(s, G_COUNT) == 0x3FE) {
        s->full++;
        return 0;
    }
    p = U(s, U(s, G_POOL));
    WU(s, G_POOL, p);
    WI(s, G_COUNT, I(s, G_COUNT) + 1);
    WI(s, p + 8, I(s, G_CAROFF));
    WU(s, p + 0x90, col);
    WI(s, p + 0xC, type);
    s->emitted[type & 0x1F]++;
    return p;
}

/* ---- the helpers --------------------------------------------------------- */

/* 0x0044B1AB: (x, y, z) through the 4x4 at m, row-vector, translated */
static void xform(spray *s, uint32_t m, ext x, ext y, ext z, ext o[3])
{
    o[0] = (F(s, m) * x + (F(s, m + 0x10) * y + F(s, m + 0x20) * z))
         + F(s, m + 0x30);
    o[1] = (F(s, m + 4) * x + (F(s, m + 0x14) * y + F(s, m + 0x24) * z))
         + F(s, m + 0x34);
    o[2] = (F(s, m + 8) * x + (F(s, m + 0x18) * y + F(s, m + 0x28) * z))
         + F(s, m + 0x38);
}

/* 0x004B5D32: each of a, b, c (stored as floats) plus FPREM(L >> 12, k),
 * k stored as a float too.  Returns EBX as it leaves it - the last draw
 * >> 12, which grass and snow then take for their size */
static uint32_t jitter_rem(spray *s, ext a, ext b, ext c, ext k, ext o[3])
{
    float fa = (float)a, fb = (float)b, fc = (float)c, fk = (float)k;
    int32_t r;
    r = (int32_t)L(s) >> 12;
    fa = (float)(ext_fmod((ext)r, (ext)fk) + fa);
    r = (int32_t)L(s) >> 12;
    fb = (float)(ext_fmod((ext)r, (ext)fk) + fb);
    r = (int32_t)L(s) >> 12;
    fc = (float)(ext_fmod((ext)r, (ext)fk) + fc);
    o[0] = fa; o[1] = fb; o[2] = fc;
    return (uint32_t)r;
}

/* 0x004B5DD7: each of a, b, c (floats) times 1 + j k, j from its own
 * generator [0x008C38C4] (rotated by 12 between draws) times [0x0070D58C] */
static void jitter_mul(spray *s, ext a, ext b, ext c, ext k, ext o[3])
{
    float fa = (float)a, fb = (float)b, fc = (float)c, fk = (float)k;
    double dk;
    uint32_t r;
    ext j0, j1, j2;
    memcpy(&dk, A(s, 0x0070D58Cu), 8);
    r = s->jit * 0x9Du + 0x47u;
    j0 = (ext)(int32_t)r * (ext)dk;
    r = ror(r, 12);
    j1 = (ext)(int32_t)r * (ext)dk;
    r = ror(r, 12);
    j2 = (ext)(int32_t)r * (ext)dk;
    s->jit = r;
    o[0] = (float)(j2 * ((ext)fa * fk) + fa);
    o[1] = (float)(j1 * ((ext)fb * fk) + fb);
    o[2] = (float)(j0 * ((ext)fc * fk) + fc);
}

/* 0x004ACB88: S+0x94, the body's cofactors and its negated position */
static void frame(spray *s, uint32_t B, uint32_t D)
{
#define M(o) ((ext)F(s, B + (o)))
    WF(s, D + 0x00, M(0x14) * M(0x28) - M(0x24) * M(0x18));
    WF(s, D + 0x10, M(0x20) * M(0x18) - M(0x10) * M(0x28));
    WF(s, D + 0x20, M(0x10) * M(0x24) - M(0x20) * M(0x14));
    WF(s, D + 0x04, M(0x24) * M(0x08) - M(0x04) * M(0x28));
    WF(s, D + 0x14, M(0x00) * M(0x28) - M(0x20) * M(0x08));
    WF(s, D + 0x24, M(0x20) * M(0x04) - M(0x00) * M(0x24));
    WF(s, D + 0x08, M(0x04) * M(0x18) - M(0x14) * M(0x08));
    WF(s, D + 0x18, M(0x10) * M(0x08) - M(0x00) * M(0x18));
    WF(s, D + 0x28, M(0x00) * M(0x14) - M(0x10) * M(0x04));
    WF(s, D + 0x30, -M(0x30));
    WF(s, D + 0x34, -M(0x34));
    WF(s, D + 0x38, -M(0x38));
#undef M
}

/* ---- the emitters -------------------------------------------------------- */

/* 0x004BBDAA grass (type 5, life 5000) / 0x004BBFA7 snow (0xB, 2000): one a
 * wheel a frame.  `base` is the constant block: 0x008DA2DA / 0x008DA302. */
static void emit_ground(spray *s, uint32_t S, uint32_t W, int snow)
{
    uint32_t base = snow ? 0x008DA302u : 0x008DA2DAu;
    uint32_t B, p, r, rx, rz;
    ext fwd = ext_fabs((ext)F(s, W + 0x18)), x[3], v[3];
    float t;
    int32_t side;

    t = (float)fwd;
    WF(s, G_TMP, t);                     /* FSTP before the test            */
    if (snow) {
        if (fistp(fwd) <= 1) return;     /* FIST / CMP 1 / JLE              */
    } else if (fbits(t) == 0) {
        return;                          /* the float's dword against 0     */
    }
    p = take(s, S, snow ? 0xB : 5);
    if (!p) return;
    if ((U(s, G_WHEEL) & 1) == 0) {
        side = -1;
        t = (float)((ext)t - F(s, W + 0x10));
    } else {
        side = 1;
        t = (float)((ext)t + F(s, W + 0x10));
    }
    WI(s, G_SIDE, side);
    WF(s, G_TMP, t);
    B = U(s, U(s, G_S) + 0x24);
    r = R(s, base);
    rx = (r & 0x3FFFu) - 0x2000u;
    rz = r & 0x3FFFu;
    WU(s, G_JX, rx);
    WU(s, G_JZ, rz);
    x[0] = (((ext)(int32_t)rx * F(s, B) + (ext)(int32_t)rz * F(s, B + 0x20))
            + F(s, W + 0x3C)) - F(s, B + 0x30);
    x[1] = (((ext)(int32_t)rx * F(s, B + 4) + (ext)(int32_t)rz * F(s, B + 0x24))
            + F(s, W + 0x40)) - F(s, B + 0x34);
    x[2] = (((ext)(int32_t)rx * F(s, B + 8) + (ext)(int32_t)rz * F(s, B + 0x28))
            + F(s, W + 0x44)) - F(s, B + 0x38);
    WF(s, p + 0x18, x[2]);
    WF(s, p + 0x28, x[1]);
    WF(s, p + 0x14, x[1]);
    WF(s, p + 0x10, x[0]);
    rz = jitter_rem(s,
               (F(s, B) * (ext)F(s, base + 4)) * side * t,
               ((F(s, B + 4) * (ext)F(s, base + 8)) * side + F(s, base + 0xC)) * t,
               (F(s, B + 8) * (ext)F(s, base + 0x10)) * side * t,
               F(s, base + 0x14), v);
    WF(s, p + 0x34, v[2] + (F(s, B + 0x28) * (ext)F(s, base + 0x18)) * F(s, W + 0x18));
    WF(s, p + 0x30, v[1] + (F(s, B + 0x24) * (ext)F(s, base + 0x1C)) * F(s, W + 0x18));
    WF(s, p + 0x2C, v[0] + (F(s, B + 0x20) * (ext)F(s, base + 0x20)) * F(s, W + 0x18));
    WI(s, p + 0x54, fistp((ext)F(s, base + 0x24) * side));
    WI(s, p + 0x58, I(s, W + 0x20) & 0x3F);
    WI(s, p + 0x60, (int32_t)((rz >> 2) & 0xFFFu) + 0x7800);  /* EBX,  */
    WU(s, p + 0x74, 0xFF000000u);
    WI(s, p + 0x84, snow ? 0x7D0 : 0x1388);
    WU(s, p + 0x68, U(s, G_S));
}

/* the per-wheel rate accumulator shared by the wet arm and type 6:
 * 0x004BC6A5..0x004BC716 / 0x004BD8DA..0x004BD94F.  `k` the four constants;
 * returns 1 when a particle is due */
static int rate(spray *s, uint32_t S, uint32_t W, ext in, uint32_t k)
{
    ext t = in * F(s, k) * F(s, k + 4) + F(s, k + 8);
    int32_t a, b;
    t = (ext)F(s, K_ONE) / t;
    t = (ext)F(s, K_ONE) - t;
    t = t * F(s, k + 0xC);
    WI(s, G_ACC, fistp(t));
    if (I(s, S + 0x90) + I(s, G_ACC) < 0x1000) return 0;
    a = I(s, W + 4) + I(s, S + 0x88);
    WI(s, W + 4, a);
    b = I(s, S + 0x90) + I(s, G_ACC);
    b = b + b - (int32_t)((uint32_t)b >> 1);
    if (a < b) return 0;
    WI(s, W + 4, a % b);                 /* XOR EDX / IDIV: a >= b > 0      */
    if (I(s, W + 0x10) == 0 && I(s, W + 0x18) == 0) return 0;
    return 1;
}

/* the wet arm 0x004BC6A5 (type 0x15) and type 6 0x004BD8C1: the same body
 * with its own constants and axes.  `c` its constant block (0x008DA3B2 wet,
 * 0x008DA4BA type 6), `g1` / `g2` its generators. */
static void emit_wet(spray *s, uint32_t S, uint32_t W, int six)
{
    uint32_t c = six ? 0x008DA4BAu : 0x008DA3B2u;
    uint32_t B, p, r, rx, rz;
    ext in, x[3], cx, cy, cz;
    int32_t cell;

    if (six) {
        WU(s, G_CELL, U(s, W + 0x20));
        if ((U(s, G_CELL) & 0xC0u) == 0) return;
        in = ext_fabs((ext)F(s, W + 0xC) - F(s, W + 0x18));
    } else {
        in = F(s, W + 0xC);
    }
    if (!rate(s, S, W, in, c)) return;
    p = take(s, S, six ? 6 : 0x15);
    if (!p) return;
    B = U(s, U(s, G_S) + 0x24);
    r = R(s, c + 0x10);
    rx = (r & 1u) - 1u;
    rz = r & 1u;
    WU(s, G_JX, rx);
    WU(s, G_JZ, rz);
    x[0] = ((ext)(int32_t)rx * F(s, B) + (ext)(int32_t)rz * F(s, B + 0x20))
         + F(s, W + 0x3C);
    x[1] = ((ext)(int32_t)rx * F(s, B + 4) + (ext)(int32_t)rz * F(s, B + 0x24))
         + F(s, W + 0x40);
    x[2] = ((ext)(int32_t)rx * F(s, B + 8) + (ext)(int32_t)rz * F(s, B + 0x28))
         + F(s, W + 0x44);
    WF(s, p + 0x18, x[2]);
    WF(s, p + 0x14, x[1]);
    WF(s, p + 0x28, x[1] - F(s, c + 0x14));
    WF(s, p + 0x10, x[0]);
    if (six) {                           /* the wheel's own axes             */
        cx = ((ext)F(s, G_SIDEAX) * F(s, W + 0x10) * F(s, c + 0x18)
              + (ext)F(s, G_FWD) * F(s, W + 0x18) * F(s, c + 0x1C))
           + (ext)F(s, G_FWD) * F(s, W + 0xC) * F(s, c + 0x20);
        cy = ((ext)F(s, G_SIDEAX + 4) * F(s, W + 0x10) * F(s, c + 0x24)
              + (ext)F(s, G_FWD + 4) * F(s, W + 0x18) * F(s, c + 0x28))
           + (ext)F(s, G_FWD + 4) * F(s, W + 0xC) * F(s, c + 0x2C);
        cz = ((ext)F(s, G_SIDEAX + 8) * F(s, W + 0x10) * F(s, c + 0x30)
              + (ext)F(s, G_FWD + 8) * F(s, W + 0x18) * F(s, c + 0x34))
           + (ext)F(s, G_FWD + 8) * F(s, W + 0xC) * F(s, c + 0x38);
    } else {                             /* the body's                       */
        cx = ((ext)F(s, B) * F(s, W + 0x10) * F(s, c + 0x18)
              + (ext)F(s, B + 0x20) * F(s, W + 0x18) * F(s, c + 0x1C))
           + (ext)F(s, B + 0x20) * F(s, W + 0xC) * F(s, c + 0x20);
        cy = ((ext)F(s, B + 4) * F(s, W + 0x10) * F(s, c + 0x24)
              + (ext)F(s, B + 0x24) * F(s, W + 0x18) * F(s, c + 0x28))
           + (ext)F(s, B + 0x24) * F(s, W + 0xC) * F(s, c + 0x2C);
        cz = ((ext)F(s, B + 8) * F(s, W + 0x10) * F(s, c + 0x30)
              + (ext)F(s, B + 0x28) * F(s, W + 0x18) * F(s, c + 0x34))
           + (ext)F(s, B + 0x28) * F(s, W + 0xC) * F(s, c + 0x38);
    }
    WF(s, p + 0x34, cz);
    WF(s, p + 0x30, cy);
    WF(s, p + 0x2C, cx);
    r = R(s, c + 0x3C);
    WI(s, p + 0x54, (int32_t)(r & 0x3Fu) - 0x20);
    WU(s, p + 0x50, ror(r, 6) & 0xFFFu);
    if (six) {
        cell = I(s, G_CELL) & 0x3F;
    } else {
        (void)(I(s, W + 0x20) & 0x3F);   /* read, then overwritten           */
        cell = 0x34;
    }
    WI(s, p + 0x58, cell);
    WF(s, p + 0x60, (ext)(int32_t)((L(s) & 0x3FFFu) + 0x4000u));
    if (six)
        WI(s, p + 0x84, I(s, 0x008D9C16u + (U(s, G_CELL) >> 6) * 4));
    else
        WI(s, p + 0x84, 0x3E8);
    WF(s, p + 0x78, (ext)F(s, 0x0070D514u) / I(s, p + 0x84));
    WU(s, p + 0x74, U(s, 0x0070D50Cu));
    WU(s, p + 0x68, U(s, G_S));
    WI(s, p + 0x70, I(s, U(s, G_S) + 0x90) + I(s, G_ACC));
    WI(s, p + 0x6C, I(s, U(s, G_S) + 0x88));
    if (six)
        WF(s, p + 0x60, (ext)I(s, p + 0x70) * F(s, c + 0x40) + F(s, c + 0x44));
    else
        WF(s, p + 0x60, (ext)I(s, p + 0x70) * F(s, c + 0x40));
}

/* the dry arm 0x004BCBBE: smoke (type 1), a float accumulator in W+4 */
static void emit_dry(spray *s, uint32_t S, uint32_t W)
{
    uint32_t B, p, r;
    ext acc, t, x;
    int32_t n;

    WU(s, G_CELL, U(s, W + 0x20));
    acc = ((ext_fabs((ext)F(s, W + 0x18)) + ext_fabs((ext)F(s, W + 0x10)))
           + ext_fabs((ext)F(s, W + 0x18) - F(s, W + 0xC))) + F(s, W + 4);
    n = fistp(acc);
    WI(s, G_T, n);
    WF(s, W + 4, acc);
    if (n < 0x18) return;
    n = n % 0x18;                        /* XOR EDX / IDIV 0x18             */
    WI(s, G_T, n);
    WF(s, W + 4, (ext)n);
    t = (ext_fabs((ext)F(s, W + 0x10)) * F(s, 0x0070D4D8u)
         + ext_fabs((ext)F(s, W + 0xC) - F(s, W + 0x18))) * F(s, 0x0070D540u);
    WF(s, G_T, t);                       /* FST: the compare is the ext's   */
    if (below(t, F(s, 0x0070D4D4u))) return;
    if (!below((ext)F(s, G_T), F(s, K_ONE)))
        WU(s, G_T, U(s, K_ONE));
    WI(s, G_LIFE0, fistp((ext)F(s, G_T) * F(s, 0x0070D560u) + F(s, 0x0070D558u)));
    WF(s, G_SIZE0, (ext)F(s, G_T) * F(s, 0x0070D50Cu));
    p = take(s, S, 1);
    if (!p) return;
    B = U(s, U(s, G_S) + 0x24);
    (void)B;
    r = R(s, 0x008DA43Au);
    WI(s, K_TMP, (int32_t)(r & 0x3FFFu) - 0x2000);
    WF(s, p + 0x10, F(s, W + 0x3C) + (ext)F(s, G_FWD) * I(s, K_TMP));
    x = F(s, W + 0x40) + (ext)F(s, G_FWD + 4) * I(s, K_TMP);
    WF(s, p + 0x14, x);
    WF(s, p + 0x28, x - F(s, 0x0070D54Cu));
    WF(s, p + 0x18, F(s, W + 0x44) + (ext)F(s, G_FWD + 8) * I(s, K_TMP));
    {
        ext a, b, c, v[3];
        B = U(s, U(s, G_S) + 0x24);
        a = ((ext)F(s, B) * F(s, W + 0x10) * F(s, 0x008DA43Eu)
             + (ext)F(s, B + 0x20) * F(s, W + 0x18) * F(s, 0x008DA442u))
          + (ext)F(s, G_FWD) * F(s, W + 0xC) * F(s, 0x008DA446u);
        b = ((ext)F(s, B + 4) * F(s, W + 0x10) * F(s, 0x008DA44Au)
             + (ext)F(s, B + 0x24) * F(s, W + 0x18) * F(s, 0x008DA44Eu))
          + (ext)F(s, G_FWD + 4) * F(s, W + 0xC) * F(s, 0x008DA452u);
        c = ((ext)F(s, B + 8) * F(s, W + 0x10) * F(s, 0x008DA456u)
             + (ext)F(s, B + 0x28) * F(s, W + 0x18) * F(s, 0x008DA45Au))
          + (ext)F(s, G_FWD + 8) * F(s, W + 0xC) * F(s, 0x008DA45Eu);
        jitter_rem(s, a, b, c, F(s, 0x008DA462u), v);
        WF(s, p + 0x34, v[2]);
        WF(s, p + 0x30, v[1] + F(s, 0x008DA466u));
        WF(s, p + 0x2C, v[0]);
    }
    r = R(s, 0x008DA46Au);
    WI(s, p + 0x54, (int32_t)(r & 0x3Fu) - 0x20);
    WU(s, p + 0x50, ror(r, 6) & 0xFFFu);
    WI(s, p + 0x58, I(s, G_CELL) & 0x3F);
    WI(s, p + 0x84, I(s, G_LIFE0));
    WF(s, p + 0x78, (ext)F(s, G_SIZE0) / I(s, p + 0x84));
    WU(s, p + 0x74, U(s, G_SIZE0));
    WU(s, p + 0x68, U(s, G_S));
    WI(s, p + 0x70, 0x10000);
    WI(s, p + 0x6C, I(s, U(s, G_S) + 0x88));
    WF(s, p + 0x60, (ext)0x10000 * F(s, 0x008DA46Eu));
    WI(s, p + 0x64, 0);
}

/* the spray 0x004BCE67 and mud 0x004BD394 - instruction for instruction the
 * same, each with its own constants and generators (`c`): the sideways throw
 * (type 0x11) then the backward one (0x10).  0 = the pool filled mid-throw
 * (spray.h: the original's misaligned return). */
static int throw1(spray *s, uint32_t S, uint32_t W, uint32_t c, int back)
{
    uint32_t r, B;
    int32_t n, i;
    ext sv, d = 0;

    if (!back) {
        sv = (ext)F(s, W + 0x10) * F(s, c);
        WF(s, G_LAT, sv);
    } else {
        sv = ((ext)F(s, W + 0xC) - F(s, W + 0x18)) * F(s, c + 0x10)
           + (ext)F(s, W + 0x18) * F(s, c + 0x14);
        WF(s, G_BACK, sv);
    }
    WI(s, G_T, fistp(ext_fabs(sv) * F(s, 0x0070D500u)));   /* FST, the ext on */
    sv = back ? F(s, G_BACK) : F(s, G_LAT);             /* then from memory */
    r = R(s, c + (back ? 0x18u : 4u));
    if ((int32_t)(ror(r, 16) & 0xFFu) >= I(s, G_T)) return 1;
    B = U(s, U(s, G_S) + 0x24);
    (void)B;
    {
        uint32_t ax = back ? G_FWD : G_SIDEAX;
        ext k = F(s, 0x0070D4BCu);
        if (!back) {
            WF(s, G_V, (ext)F(s, ax) * sv * k);
            d = (ext)F(s, ax + 4) * sv - ext_fabs(sv);
            WF(s, G_V + 4, d * k);
        } else {
            WF(s, G_V, (ext)F(s, ax) * sv * k);
            d = (ext)F(s, ax + 4) * sv - ext_fabs(sv) * F(s, 0x0070D4C8u);
            WF(s, G_V + 4, d * k);
        }
        WF(s, G_V + 8, (ext)F(s, ax + 8) * sv * k);
    }
    r = R(s, c + (back ? 0x1Cu : 8u));
    n = (int32_t)((r & 0xFFFu) % (uint32_t)((U(s, G_SPRAYB) >> 6) << 2));
    for (i = n - 1; i >= 0; i--) {
        uint32_t p, q, sz, ax2;
        int32_t off;
        ext v[3];
        p = take(s, S, back ? 0x10 : 0x11);
        if (!p) return 0;
        jitter_mul(s, F(s, G_V), F(s, G_V + 4), F(s, G_V + 8),
                   F(s, 0x0070D4C8u), v);
        WF(s, p + 0x34, v[2]);
        WF(s, p + 0x30, v[1]);
        WF(s, p + 0x2C, v[0]);
        q = R(s, c + (back ? 0x20u : 0xCu));
        if (back) sz = (ror(q, 8) & 0x3FFu) + 0x600u;
        else      sz = (ror(q, 8) & 0x7FFu) + 0x400u;
        WI(s, K_TMP, (int32_t)sz);
        WF(s, p + 0x60, (ext)(int32_t)sz);
        WU(s, p + 0x58, ((U(s, G_SPRAYB) & 0x3Fu) << 4) + (ror(q, 17) & 3u));
        WF(s, p + 0x40, (ext)F(s, S + 0x54) * F(s, 0x0070D4C0u));
        WF(s, p + 0x3C, (ext)F(s, S + 0x50) * F(s, 0x0070D4C0u));
        WF(s, p + 0x38, (ext)F(s, S + 0x4C) * F(s, 0x0070D4C0u));
        WI(s, p + 0x44, 0);
        WI(s, p + 0x48, 0);
        WI(s, p + 0x4C, 0);
        B = U(s, S + 0x24);
        off = (int32_t)(ror(q, 8) & 0x3FFFu) - 0x2000;
        WI(s, K_TMP, off);
        ax2 = back ? G_SIDEAX : G_FWD;
        WF(s, p + 0x10, ((ext)F(s, W + 0x3C) - F(s, B + 0x30))
                        + (ext)F(s, ax2) * off);
        {
            ext y = ((ext)F(s, W + 0x40) - F(s, B + 0x34))
                  + (ext)F(s, ax2 + 4) * off;
            WF(s, p + 0x14, y);
            WF(s, p + 0x28, y);
        }
        WF(s, p + 0x18, ((ext)F(s, W + 0x44) - F(s, B + 0x38))
                        + (ext)F(s, ax2 + 8) * off);
        WI(s, G_T, (int32_t)(q & 0x7FFFFu));
        WF(s, p + 0x5C, (ext)I(s, G_T) * F(s, 0x0070D584u) + F(s, K_ONE));
        WI(s, p + 0x84, 0x7D0);
        WI(s, p + 0x7C, I(s, G_FOOT_X));
        WI(s, p + 0x80, I(s, G_FOOT_Z));
        WU(s, p + 0x68, U(s, G_S));
    }
    return 1;
}

static void emit_spray(spray *s, uint32_t S, uint32_t W, uint32_t c)
{
    WU(s, G_SPRAYB, U(s, W + 0x24));
    if ((U(s, G_SPRAYB) >> 6) == 0) return;
    if (!throw1(s, S, W, c, 0)) return;
    (void)throw1(s, S, W, c, 1);
}

/* 0x004BDB87, the body contact's class 1: sparks (type 0x12) */
static void emit_sparks(spray *s, uint32_t S, uint32_t W)
{
    ext t;
    int32_t n, i;
    WU(s, G_CELL, U(s, W + 0x20));
    t = (ext)F(s, W + 0x38) * F(s, 0x0070D540u);
    if (!below(t, F(s, K_ONE))) t = EXT_C(1.0);           /* FFREE / FLD1        */
    WI(s, G_T, (int32_t)((U(s, G_CELL) >> 6) << 2));
    n = fistp(t * I(s, G_T));
    WI(s, G_T, n);
    for (i = n - 1; i >= 0; i--) {
        /* ESI is not restored round the loop: the colour of every spark
         * after the first is the previous spark's +0x1C, zeroed */
        uint32_t p = take(s, S, 0x12);
        ext v[3];
        if (!p) return;                  /* the exit at 0x4BDCA5 pops the
                                          * counter: balanced               */
        S = p;
        jitter_rem(s, F(s, G_SPARK0), F(s, G_SPARK0 + 4), F(s, G_SPARK0 + 8),
                   F(s, 0x0070D508u), v);
        WF(s, p + 0x34, v[2]);
        WF(s, p + 0x30, ext_fabs(v[1]));
        WF(s, p + 0x2C, v[0]);
        WU(s, p + 0x10, U(s, W + 0x3C));
        WF(s, p + 0x14, F(s, W + 0x40));
        WF(s, p + 0x28, (ext)F(s, W + 0x40) - F(s, 0x0070D54Cu));
        WU(s, p + 0x18, U(s, W + 0x44));
        WI(s, p + 0x1C, 0);
        WI(s, p + 0x20, 0);
        WI(s, p + 0x24, 0);
        WI(s, p + 0x84, 0x12C);
        WU(s, p + 0x68, U(s, G_S));
        WF(s, p + 0x78, (ext)F(s, 0x0070D514u) / I(s, p + 0x84));
    }
}

/* 0x004BDCAB, every frame a car: the exhaust (type 0xD, life 250) */
static void emit_exhaust(spray *s, uint32_t S)
{
    uint32_t B, p, e, r;
    ext x, y, z, o[3], k;
    e = 0x00711682u + (uint32_t)(I(s, S + 4) * 0xC);
    p = take(s, S, 0xD);
    if (!p) return;
    B = U(s, U(s, G_S) + 0x24);
    k = F(s, 0x0070D544u);
    x = -(ext)F(s, e) * k;
    y = ((ext)F(s, e + 4) - F(s, 0x0070D554u)) * k;
    z = (ext)F(s, e + 8) * k;
    xform(s, B, z, y, x, o);             /* loaded x, y, z: z on top        */
    WF(s, p + 0x10, o[0] - F(s, B + 0x30));
    WF(s, p + 0x14, o[1] - F(s, B + 0x34));
    WF(s, p + 0x28, (o[1] - F(s, B + 0x34)) - F(s, 0x008DA502u));
    WF(s, p + 0x18, o[2] - F(s, B + 0x38));
    WF(s, G_T, (ext)F(s, K_ONE)
               - (ext)F(s, K_ONE) / ((ext)F(s, S + 8) * F(s, 0x008DA506u) + F(s, K_ONE)));
    WF(s, p + 0x34, (ext)F(s, B + 0x28) * F(s, G_T) * F(s, 0x008DA512u));
    WF(s, p + 0x30, (ext)F(s, B + 0x24) * F(s, G_T) * F(s, 0x008DA50Eu));
    WF(s, p + 0x2C, (ext)F(s, B + 0x20) * F(s, G_T) * F(s, 0x008DA50Au));
    r = R(s, 0x008DA516u);
    {
        ext f = (ext)(int32_t)(r & 0xFFFFu) * F(s, 0x0070D584u);
        WF(s, p + 0x10, f * F(s, p + 0x2C) * I(s, K_DT) + F(s, p + 0x10));
        WF(s, p + 0x14, f * F(s, p + 0x30) * I(s, K_DT) + F(s, p + 0x14));
        WF(s, p + 0x18, f * F(s, p + 0x34) * I(s, K_DT) + F(s, p + 0x18));
    }
    r = R(s, 0x008DA51Au);
    WI(s, p + 0x54, (int32_t)(r & 0x3Fu) - 0x20);
    WU(s, p + 0x50, ror(r, 6) & 0xFFFu);
    WI(s, p + 0x58, 0x3F);
    WI(s, p + 0x84, 0xFA);
    WF(s, p + 0x78, (ext)F(s, G_T) * F(s, 0x0070D514u) / I(s, p + 0x84));
    WF(s, p + 0x74, (ext)F(s, G_T) * F(s, 0x0070D510u));
    WU(s, p + 0x68, U(s, G_S));
    WI(s, p + 0x70, fistp((ext)F(s, 0x008DA51Eu) * F(s, G_T) + F(s, 0x008DA522u)));
    WI(s, p + 0x6C, I(s, U(s, G_S) + 0x88));
    WF(s, p + 0x60, (ext)I(s, p + 0x70) * F(s, 0x008DA526u));
}

/* 0x004BDED0 (type 0x13, S+0x4C0, cell 0x3E) / 0x004BE0CF (0x16, S+0x4C4,
 * cell 0x33): the damage smoke, a chance level/8 a frame */
static void emit_smoke(spray *s, uint32_t S, int second)
{
    uint32_t c = second ? 0x008DA552u : 0x008DA52Au;
    uint32_t B, p, r;
    ext o[3], v[3];
    WI(s, G_SMOKE, I(s, S + (second ? 0x4C4u : 0x4C0u)));
    r = R(s, c);
    if ((int32_t)(ror(r, 16) & 7u) >= I(s, G_SMOKE)) return;
    WI(s, G_CELL, second ? 0x33 : 0x3E);
    WU(s, G_CARV, U(s, S + 0x4C));
    WU(s, G_CARV + 4, U(s, S + 0x50));
    WU(s, G_CARV + 8, U(s, S + 0x54));
    p = take(s, S, second ? 0x16 : 0x13);
    if (!p) return;
    B = U(s, U(s, G_S) + 0x24);
    xform(s, B, F(s, 0x008DA256u), F(s, 0x008DA25Au), F(s, 0x008DA25Eu), o);
    WF(s, p + 0x10, o[0]);
    WF(s, p + 0x14, o[1]);
    WF(s, p + 0x28, o[1] - F(s, c + 4));
    WF(s, p + 0x18, o[2]);
    /* pushed row 1's z, y, x scaled: the last (x) comes back on top */
    jitter_rem(s, (ext)F(s, B + 0x18) * F(s, c + 8),
               (ext)F(s, B + 0x14) * F(s, c + 0xC),
               (ext)F(s, B + 0x10) * F(s, c + 0x10), F(s, c + 0x14), v);
    WF(s, p + 0x2C, v[2] + (ext)F(s, G_CARV) * F(s, K_INVDT) * F(s, c + 0x18));
    WF(s, p + 0x30, v[1] + (ext)F(s, G_CARV + 4) * F(s, K_INVDT) * F(s, c + 0x1C));
    WF(s, p + 0x34, v[0] + (ext)F(s, G_CARV + 8) * F(s, K_INVDT) * F(s, c + 0x20));
    r = R(s, c + 0x24);
    WI(s, p + 0x54, (int32_t)(r & 0x3Fu) - 0x20);
    WU(s, p + 0x50, ror(r, 6) & 0xFFFu);
    WI(s, p + 0x58, I(s, G_CELL) & 0x3F);
    WI(s, G_T, (int32_t)((ror(r, 20) & 0x4000u) + 0x4000u));
    WF(s, p + 0x60, (ext)I(s, G_T));
    WI(s, p + 0x84, I(s, 0x008D9C26u + (uint32_t)I(s, G_SMOKE) * 4));
    WF(s, p + 0x78, (ext)F(s, 0x0070D514u) / I(s, p + 0x84));
    WU(s, p + 0x74, U(s, 0x0070D50Cu));
    WU(s, p + 0x68, U(s, G_S));
}

/* the dispatch 0x004B9CC3 (wheels) and 0x004B9D79's copy (the body).  With
 * chance weight the second layer W+0x28..0x30 replaces the first - but no
 * code writes a wheel's W+0x28 (FUN_004547E1 copies the layer's bytes, not
 * its class), so on a blend a wheel emits nothing that frame; the
 * body's is copied (car+0x944) */
static void dispatch(spray *s, uint32_t S, uint32_t W, int body)
{
    uint32_t gen = body ? 0x008D55CEu : 0x008D55CAu, r;
    int32_t cls;
    WI(s, G_T, fistp((ext)F(s, W + 0x34) * F(s, 0x0070D54Cu)));
    r = R(s, gen);
    if ((int32_t)(r & 0xFFFFu) < I(s, G_T)) {
        s->swaps++;
        WU(s, W + 0x1C, U(s, W + 0x28));
        WU(s, W + 0x20, U(s, W + 0x2C));
        WU(s, W + 0x24, U(s, W + 0x30));
    }
    cls = I(s, W + 0x1C);
    s->arm[body][cls & 15]++;
    switch (cls) {
    case 1:
        if (body) emit_sparks(s, S, W);
        else if (I(s, G_RAIN) != 0) emit_wet(s, S, W, 0);
        else emit_dry(s, S, W);
        break;
    case 2: case 4: case 6: case 7: case 8: case 9: case 12:
        emit_spray(s, S, W, 0x008DA472u);
        emit_wet(s, S, W, 1);
        break;
    case 3:                              /* the body's mud arm is unreachable:
                                          * its duplicated cmp 3 sends 3 to
                                          * the spray                  */
        if (body) emit_spray(s, S, W, 0x008DA472u);
        else      emit_spray(s, S, W, 0x008DA496u);
        emit_wet(s, S, W, 1);
        break;
    case 5:  emit_ground(s, S, W, 0); break;
    case 11: emit_ground(s, S, W, 1); break;
    default: break;
    }
}

/* 0x004B9A6C, when S+0x30 is 0: the wheels' positions and the dispatches */
static void wheels(spray *s, uint32_t S)
{
    static const uint32_t w[4] = { 0xD4, 0x11C, 0x164, 0x1AC };
    uint32_t B = U(s, S + 0x24);
    ext t, o[3];
    int i, k;

    t = (ext)I(s, S + 0xC) * F(s, 0x008D5596u);
    WF(s, S + 0x118, t); WF(s, S + 0x160, t);
    t = (ext)I(s, S + 0x10) * F(s, 0x008D559Au);
    WF(s, S + 0x1A8, t); WF(s, S + 0x1F0, t);
    t = (ext)I(s, S + 0x14) * F(s, 0x008D559Eu) + F(s, 0x008D55A2u);
    WF(s, S + 0x158, t); WF(s, S + 0x1E8, t);
    WF(s, S + 0x110, -t); WF(s, S + 0x1A0, -t);
    t = (ext)I(s, S + 0x18) * F(s, 0x008D55A6u) - F(s, 0x008D55AAu)
        - F(s, 0x008D55AEu);
    WF(s, S + 0x15C, t); WF(s, S + 0x1EC, t);
    WF(s, S + 0x114, t); WF(s, S + 0x1A4, t);
    WI(s, G_FOOT_X, fistp(ext_fabs((ext)F(s, S + 0x110))));
    WI(s, G_FOOT_Z, fistp((ext_fabs((ext)F(s, S + 0x118)) + ext_fabs((ext)F(s, S + 0x1A8)))
                          * F(s, 0x0070D4C8u)));
    for (i = 0; i < 4; i++) {
        uint32_t q = S + w[i] + 0x3C;
        xform(s, B, F(s, q), F(s, q + 4), F(s, q + 8), o);
        for (k = 0; k < 3; k++) WF(s, q + 4 * (uint32_t)k, o[k]);
    }
    WI(s, G_WHEEL, 0); dispatch(s, S, S + 0xD4, 0);
    WI(s, G_WHEEL, 1); dispatch(s, S, S + 0x11C, 0);
    /* the rears: the body's own axes, x and -z */
    WU(s, G_SIDEAX, U(s, B)); WU(s, G_SIDEAX + 4, U(s, B + 4));
    WU(s, G_SIDEAX + 8, U(s, B + 8));
    WU(s, G_FWD, U(s, B + 0x20) ^ 0x80000000u);
    WU(s, G_FWD + 4, U(s, B + 0x24) ^ 0x80000000u);
    WU(s, G_FWD + 8, U(s, B + 0x28) ^ 0x80000000u);
    WI(s, G_WHEEL, 2); dispatch(s, S, S + 0x164, 0);
    WI(s, G_WHEEL, 3); dispatch(s, S, S + 0x1AC, 0);
    /* the body: two constant axes */
    for (k = 0; k < 3; k++) {
        WU(s, G_SIDEAX + 4 * (uint32_t)k, U(s, 0x008D55B2u + 4 * (uint32_t)k));
        WU(s, G_FWD + 4 * (uint32_t)k, U(s, 0x008D55BEu + 4 * (uint32_t)k));
    }
    /* 0x004B9D79 */
    {
        uint32_t W = S + 0x1F4;
        WI(s, S + 0x90, fistp((ext)I(s, S + 0x90) * F(s, 0x0070D4E4u)));
        WI(s, W + 4, I(s, W + 4) + 0x386A0);
        WI(s, G_WHEEL, 2);
        WU(s, W, U(s, W) ^ 1u);
        if ((U(s, W) & 1u) == 0) WI(s, G_WHEEL, 3);
        WI(s, W + 0xC, 0);
        dispatch(s, S, W, 1);
    }
}

void spray_emit(spray *s, int ncars, int advance)
{
    int car;
    if (!s->have || !advance) return;    /* block +0x10 == 1                */
    for (car = ncars - 1; car >= 0; car--) {
        uint32_t S = BLK(s, 0x60u + (uint32_t)car * SPRAY_SNAP), B, M;
        ext v, t;
        WI(s, G_CAROFF, fistp((ext)car * I(s, G_SNAPLEN)));
        WU(s, G_S, S);
        B = U(s, S + 0x24);
        frame(s, B, S + 0x94);
        memmove(A(s, S + 0x7C), A(s, S + 0x70), 12);
        memmove(A(s, S + 0x70), A(s, S + 0x64), 12);
        memmove(A(s, S + 0x64), A(s, S + 0x58), 12);
        WF(s, S + 0x58, (ext)F(s, B + 0x30) - F(s, S + 0x40));
        WF(s, S + 0x5C, (ext)F(s, B + 0x34) - F(s, S + 0x44));
        WF(s, S + 0x60, (ext)F(s, B + 0x38) - F(s, S + 0x48));
        WF(s, S + 0x4C, ((((ext)F(s, S + 0x58) + F(s, S + 0x64)) + F(s, S + 0x70))
                         + F(s, S + 0x7C)) * F(s, 0x0070D52Cu));
        WF(s, S + 0x50, ((((ext)F(s, S + 0x5C) + F(s, S + 0x68)) + F(s, S + 0x74))
                         + F(s, S + 0x80)) * F(s, 0x0070D52Cu));
        WF(s, S + 0x54, ((((ext)F(s, S + 0x60) + F(s, S + 0x6C)) + F(s, S + 0x78))
                         + F(s, S + 0x84)) * F(s, 0x0070D52Cu));
        memcpy(A(s, S + 0x40), A(s, B + 0x30), 12);
        v = ext_sqrt((ext)F(s, S + 0x4C) * F(s, S + 0x4C)
                  + ((ext)F(s, S + 0x50) * F(s, S + 0x50)
                     + (ext)F(s, S + 0x54) * F(s, S + 0x54)));
        WI(s, S + 0x88, fistp(v));
        t = v * F(s, 0x008D558Au) + F(s, 0x008D558Eu);
        t = (ext)F(s, K_ONE) / t;
        t = (ext)F(s, K_ONE) - t;
        WI(s, S + 0x90, fistp(t * F(s, 0x008D5592u)));
        if (I(s, S + 0x30) != 0) {
            WI(s, S + 0x4C, 0); WI(s, S + 0x50, 0); WI(s, S + 0x54, 0);
            WI(s, S + 0x88, 0);
        }
        /* the fronts' axes: row 0 of car+0x740 crossed with a constant,
         * normalised, crossed again, through the body */
        M = U(s, S + 0x28);
        {
            ext cx, cy, cz, n, r, a, b, c, X, Y, Z;
            WF(s, G_CR, (ext)F(s, G_UP + 4) * F(s, M + 8) - (ext)F(s, G_UP + 8) * F(s, M + 4));
            WF(s, G_CR + 4, (ext)F(s, G_UP + 8) * F(s, M) - (ext)F(s, G_UP) * F(s, M + 8));
            WF(s, G_CR + 8, (ext)F(s, G_UP) * F(s, M + 4) - (ext)F(s, G_UP + 4) * F(s, M));
            cx = F(s, G_CR); cy = F(s, G_CR + 4); cz = F(s, G_CR + 8);
            n = ext_sqrt(cx * cx + (cy * cy + cz * cz));
            r = (ext)F(s, K_ONE) / n;
            WF(s, G_CR, r * cx);
            WF(s, G_CR + 4, r * cy);
            WF(s, G_CR + 8, r * cz);
            cx = F(s, G_CR); cy = F(s, G_CR + 4); cz = F(s, G_CR + 8);
            a = cx * F(s, M + 4) - cy * F(s, M);
            b = cz * F(s, M) - cx * F(s, M + 8);
            c = cy * F(s, M + 8) - cz * F(s, M + 4);
            X = (ext)F(s, B) * c + ((ext)F(s, B + 0x10) * b + (ext)F(s, B + 0x20) * a);
            Y = (ext)F(s, B + 4) * c + ((ext)F(s, B + 0x14) * b + (ext)F(s, B + 0x24) * a);
            Z = (ext)F(s, B + 8) * c + ((ext)F(s, B + 0x18) * b + (ext)F(s, B + 0x28) * a);
            WF(s, G_FWD, X);
            WF(s, G_FWD + 4, Y);
            WF(s, G_FWD + 8, Z);
            WF(s, G_SIDEAX, (ext)F(s, G_FWD + 4) * F(s, G_CR + 8) - (ext)F(s, G_FWD + 8) * F(s, G_CR + 4));
            WF(s, G_SIDEAX + 4, (ext)F(s, G_FWD + 8) * F(s, G_CR) - (ext)F(s, G_FWD) * F(s, G_CR + 8));
            WF(s, G_SIDEAX + 8, (ext)F(s, G_FWD) * F(s, G_CR + 4) - (ext)F(s, G_FWD + 4) * F(s, G_CR));
        }
        WU(s, G_FOOT_X, U(s, 0x00711796u + (uint32_t)I(s, S + 4) * 8));
        WU(s, G_FOOT_Z, U(s, 0x0071179Eu + (uint32_t)I(s, S + 4) * 8));
        if (I(s, S + 0x30) != 0) WI(s, S + 0x30, I(s, S + 0x30) - 1);
        else wheels(s, S);
        emit_exhaust(s, S);
        emit_smoke(s, S, 0);
        emit_smoke(s, S, 1);
    }
}

/* ---- the update, 0x004BB1F7 ---------------------------------------------- */

/* a particle's car: the object block +0x60 + its +8, the draw's gate */
static int car_live(spray *s, uint32_t p)
{
    return I(s, BLK(s, 0x60u + U(s, p + 8))) != 0;
}

/* the draws' shared opening, 0x004BB3E3 (rotated, the 64 cells) and
 * 0x004BBB94 (screen-aligned, the 1,024 sub-cells): the point through
 * [0x0059F014], [0x008D5760] = its z rounded, and nothing nearer than
 * [0x0088C221] (compared as integers).  The quad itself is spray_draw.c's. */
static void draw(spray *s, const spray_view *v, int sprite, uint32_t p,
                 ext x, ext y, ext z, uint32_t cell, uint32_t angle,
                 uint32_t ecx, uint32_t edx)
{
    ext c[3];
    float cf[3], size;
    xform(s, SPRAY_VIEW_VA, x, y, z, c);
    cf[0] = (float)c[0]; cf[1] = (float)c[1]; cf[2] = (float)c[2];
    WI(s, G_T, fistp(c[2]));             /* FIST before the near test       */
    if ((int32_t)fbits(cf[2]) <= (int32_t)s->near_bits) return;
    s->drawn++;
    if (!v || !s->quad) return;
    if (sprite) {
        memcpy(&size, &edx, 4);          /* a float's bits, FLD             */
        s->quad(s, v, 1, cf, cell & 0x3FFu, 0, U(s, p + 0x90) | 0xFF000000u,
                size, 1.0f);
    } else {
        /* the rotated quad's height: [0x008C0FF4 + cell*16], another
         * module's 0x24-byte table read at stride 16 */
        s->quad(s, v, 0, cf, cell & 0x3Fu, angle & 0xFFFu,
                (ecx & 0xFF000000u) | U(s, p + 0x90), (float)(int32_t)edx,
                s->aspect[(cell & 0x3Fu) * 4u]);
    }
}

/* P+0x84 -= dt (dead on the borrow), then dead below the spawn height */
static int life(spray *s, uint32_t p)
{
    uint32_t l = U(s, p + 0x84), dt = U(s, K_DT);
    WU(s, p + 0x84, l - dt);
    if (l < dt) return 0;
    return !below(F(s, p + 0x14), F(s, p + 0x28));
}

static void spin(spray *s, uint32_t p)
{
    int32_t a = fistp((ext)I(s, p + 0x54) * F(s, K_SPIN));
    WU(s, p + 0x50, (U(s, p + 0x50) + (uint32_t)a) & 0xFFFu);
}

static void move(spray *s, uint32_t p)
{
    int32_t dt = I(s, K_DT);
    WF(s, p + 0x10, (ext)F(s, p + 0x2C) * dt + F(s, p + 0x10));
    WF(s, p + 0x14, (ext)F(s, p + 0x30) * dt + F(s, p + 0x14));
    WF(s, p + 0x18, (ext)F(s, p + 0x34) * dt + F(s, p + 0x18));
}

static void fall(spray *s, uint32_t p, uint32_t k)
{
    WF(s, p + 0x30, (ext)F(s, k) * I(s, K_DT) + F(s, p + 0x30));
}

/* v -= v k dt, and 0 when that changed its sign */
static void damp(spray *s, uint32_t p, uint32_t off, uint32_t k)
{
    float old = F(s, p + off);
    WF(s, G_T, old);
    WF(s, p + off, (ext)old - (ext)old * F(s, k) * I(s, K_DT));
    if ((int32_t)(U(s, G_T) ^ U(s, p + off)) < 0) WI(s, p + off, 0);
}

/* the size toward P+0x70, never past it */
static void grow(spray *s, uint32_t p, uint32_t k)
{
    ext t = ((ext)I(s, p + 0x70) - F(s, p + 0x60)) * F(s, k) * I(s, K_DT)
          + F(s, p + 0x60);
    WI(s, G_T, fistp(t));
    WF(s, p + 0x60, t);
    if (I(s, G_T) > I(s, p + 0x70)) WF(s, p + 0x60, (ext)I(s, p + 0x70));
}

static void fade(spray *s, uint32_t p)
{
    WF(s, p + 0x74, (ext)F(s, p + 0x74) - (ext)F(s, p + 0x78) * I(s, K_DT));
}

static void alpha_floor(spray *s, uint32_t p)
{
    if (I(s, p + 0x74) < 0) WI(s, p + 0x74, 0);
}

/* |x| and |z| of a car-relative point in the car's own frame (S+0x94) */
static void foot(spray *s, uint32_t p, ext x, ext y, ext z)
{
    uint32_t m = U(s, p + 0x68) + 0x94;
    ext X = F(s, m) * x + (F(s, m + 0x10) * y + F(s, m + 0x20) * z);
    ext Z = F(s, m + 8) * x + (F(s, m + 0x18) * y + F(s, m + 0x28) * z);
    WI(s, G_FX, fistp(ext_fabs(X)));
    WI(s, G_FZ, fistp(ext_fabs(Z)));
}

static void rel(spray *s, uint32_t p)
{
    uint32_t B = U(s, U(s, p + 0x68) + 0x24);
    WF(s, G_REL, (ext)F(s, p + 0x10) - F(s, B + 0x30));
    WF(s, G_REL + 4, (ext)F(s, p + 0x14) - F(s, B + 0x34));
    WF(s, G_REL + 8, (ext)F(s, p + 0x18) - F(s, B + 0x38));
}

/* the draw's alpha (P+0x74, a float) and size (P+0x60) as integers */
static void draw_abs(spray *s, const spray_view *v, uint32_t p, uint32_t sz)
{
    uint32_t ecx;
    WI(s, G_T, fistp(F(s, p + 0x74)));
    ecx = U(s, G_T) << 24;
    WU(s, G_T, sz);
    draw(s, v, 0, p, F(s, p + 0x10), F(s, p + 0x14), F(s, p + 0x18),
         U(s, p + 0x58), U(s, p + 0x50), ecx, sz);
}

static void draw_rel(spray *s, const spray_view *v, uint32_t p)
{
    uint32_t B = U(s, U(s, p + 0x68) + 0x24), ecx, sz;
    WI(s, G_T, fistp(F(s, p + 0x74)));
    ecx = U(s, G_T) << 24;
    WI(s, G_T, fistp(F(s, p + 0x60)));
    sz = U(s, G_T);
    draw(s, v, 0, p, (ext)F(s, p + 0x10) + F(s, B + 0x30),
         (ext)F(s, p + 0x14) + F(s, B + 0x34),
         (ext)F(s, p + 0x18) + F(s, B + 0x38), U(s, p + 0x58), U(s, p + 0x50),
         ecx, sz);
}

/* 0x004BE2CE grass (5) / 0x004BE3B8 snow (0xB) */
static int upd_ground(spray *s, const spray_view *v, uint32_t p, uint32_t k)
{
    if (!life(s, p)) return -1;
    if (car_live(s, p)) {
        uint32_t B = U(s, U(s, p + 0x68) + 0x24);
        draw(s, v, 0, p, (ext)F(s, p + 0x10) + F(s, B + 0x30),
             (ext)F(s, p + 0x14) + F(s, B + 0x34),
             (ext)F(s, p + 0x18) + F(s, B + 0x38), U(s, p + 0x58),
             U(s, p + 0x50), U(s, p + 0x74), U(s, p + 0x60));
    }
    spin(s, p);
    move(s, p);
    fall(s, p, k);
    WI(s, p + 0x60, fistp((ext)F(s, k + 4) * I(s, K_DT) + I(s, p + 0x60)));
    WI(s, p + 0x74, fistp((ext)I(s, k + 8) * I(s, K_DT) + I(s, p + 0x74)));
    return 0;
}

/* 0x004BE4A2 type 6 / 0x004BE76A wet (0x15): grows while the car moves away */
static int upd_mark(spray *s, const spray_view *v, uint32_t p, uint32_t k)
{
    ext x, y, z, n, r;
    if (!life(s, p)) return -1;
    alpha_floor(s, p);
    if (car_live(s, p)) {
        WI(s, G_T, fistp(F(s, p + 0x60)));
        {
            uint32_t sz = U(s, G_T);
            draw_abs(s, v, p, sz);
        }
    }
    spin(s, p);
    move(s, p);
    damp(s, p, 0x2C, k);
    damp(s, p, 0x30, k + 4);
    damp(s, p, 0x34, k + 8);
    rel(s, p);
    foot(s, p, F(s, G_REL), F(s, G_REL + 4), F(s, G_REL + 8));
    if (I(s, p + 0x7C) >= I(s, G_FX) && I(s, p + 0x80) >= I(s, G_FZ))
        goto out;
    x = F(s, G_REL); y = F(s, G_REL + 4); z = F(s, G_REL + 8);
    n = ext_sqrt(x * x + (y * y + z * z));
    r = (ext)F(s, K_ONE) / n;
    WF(s, G_REL, F(s, G_REL) * r);
    WF(s, G_REL + 4, F(s, G_REL + 4) * r);
    WF(s, G_REL + 8, F(s, G_REL + 8) * r);
    {
        uint32_t S = U(s, p + 0x68);
        WF(s, 0x008D5754u, (ext)F(s, G_REL) * F(s, S + 0x4C)
                           + ((ext)F(s, G_REL + 4) * F(s, S + 0x50)
                              + (ext)F(s, G_REL + 8) * F(s, S + 0x54)));
    }
    if (I(s, 0x008D5754u) > 0) goto out;
    grow(s, p, k + 0xC);
out:
    fade(s, p);
    return 0;
}

/* 0x004BEA32: the dry tarmac's smoke (1) */
static int upd_dry(spray *s, const spray_view *v, uint32_t p)
{
    ext x, y, z, d, r, q;
    uint32_t S;
    if (!life(s, p)) return -1;
    alpha_floor(s, p);
    if (car_live(s, p)) {
        WI(s, G_T, fistp((ext)F(s, p + 0x60) + (ext)F(s, p + 0x64) * F(s, 0x008DA5B2u)));
        draw_abs(s, v, p, U(s, G_T));
    }
    spin(s, p);
    move(s, p);
    damp(s, p, 0x2C, 0x008DA5B6u);
    damp(s, p, 0x30, 0x008DA5BAu);
    damp(s, p, 0x34, 0x008DA5BEu);
    grow(s, p, 0x008DA5C2u);
    fade(s, p);
    rel(s, p);
    x = F(s, G_REL); y = F(s, G_REL + 4); z = F(s, G_REL + 8);
    d = ext_sqrt(x * x + (y * y + z * z));
    WF(s, 0x008D575Cu, d - F(s, 0x008DA5C6u));
    if (I(s, 0x008D575Cu) < 0) return 0;
    r = (ext)F(s, K_ONE) / d;
    WF(s, G_REL, F(s, G_REL) * r);
    WF(s, G_REL + 4, F(s, G_REL + 4) * r);
    WF(s, G_REL + 8, F(s, G_REL + 8) * r);
    S = U(s, p + 0x68);
    q = (ext)F(s, K_ONE) / (ext)I(s, S + 0x88);
    WF(s, G_RELV, F(s, S + 0x4C) * q);
    WF(s, G_RELV + 4, F(s, S + 0x50) * q);
    WF(s, G_RELV + 8, F(s, S + 0x54) * q);
    WF(s, 0x008D5754u, -((ext)F(s, G_REL) * F(s, G_RELV)
                         + ((ext)F(s, G_REL + 4) * F(s, G_RELV + 4)
                            + (ext)F(s, G_REL + 8) * F(s, G_RELV + 8))));
    if (I(s, 0x008D5754u) <= 0) return 0;
    WF(s, G_T, (ext)F(s, K_ONE) - (ext)F(s, K_ONE)
               / ((ext)I(s, S + 0x88) * F(s, 0x008DA5CAu) + F(s, K_ONE)));
    {
        ext t = ((ext)F(s, 0x0070D54Cu) - F(s, p + 0x64))
              * ((ext)F(s, 0x008DA5CEu) * F(s, G_T)) * I(s, K_DT)
              + F(s, p + 0x64);
        WI(s, G_T, fistp(t));
        WF(s, p + 0x64, t);
        if (I(s, G_T) > I(s, 0x0070D4B0u)) WU(s, p + 0x64, U(s, 0x0070D54Cu));
    }
    return 0;
}

/* 0x004BED3E: the exhaust (0xD) */
static int upd_exhaust(spray *s, const spray_view *v, uint32_t p)
{
    if (!life(s, p)) return -1;
    alpha_floor(s, p);
    if (car_live(s, p)) draw_rel(s, v, p);
    spin(s, p);
    move(s, p);
    fall(s, p, 0x008DA5D2u);
    damp(s, p, 0x2C, 0x008DA5D6u);
    damp(s, p, 0x30, 0x008DA5DAu);
    damp(s, p, 0x34, 0x008DA5DEu);
    grow(s, p, 0x008DA5E2u);
    fade(s, p);
    return 0;
}

/* 0x004BEEF3 (0x13) / 0x004BF06E (0x16): the damage smoke */
static int upd_smoke(spray *s, const spray_view *v, uint32_t p, uint32_t k)
{
    if (!life(s, p)) return -1;
    alpha_floor(s, p);
    if (car_live(s, p)) {
        WI(s, G_T, fistp(F(s, p + 0x60)));
        draw_abs(s, v, p, U(s, G_T));
    }
    spin(s, p);
    move(s, p);
    damp(s, p, 0x2C, k);
    damp(s, p, 0x30, k + 4);
    damp(s, p, 0x34, k + 8);
    fade(s, p);
    WF(s, p + 0x60, (ext)F(s, k + 0xC) * I(s, K_DT) + F(s, p + 0x60));
    return 0;
}

/* 0x004BF1E9 (0x10, backward) / 0x004BF351 (0x11, sideways): the spray.
 * Dead inside the car's footprint above [0x008D5710] - 0x11 tests only
 * the footprint's x */
static int upd_spray(spray *s, const spray_view *v, uint32_t p, int side)
{
    if (!life(s, p)) return -1;
    move(s, p);
    fall(s, p, side ? 0x008DA60Au : 0x008DA606u);
    if (car_live(s, p)) {
        uint32_t B = U(s, U(s, p + 0x68) + 0x24), fr;
        WI(s, G_T, fistp((ext)I(s, p + 0x84) * F(s, 0x0070D4CCu) * F(s, p + 0x5C)));
        fr = (U(s, G_T) & 3u) + U(s, p + 0x58);
        draw(s, v, 1, p, (ext)F(s, p + 0x10) + F(s, B + 0x30),
             (ext)F(s, p + 0x14) + F(s, B + 0x34),
             (ext)F(s, p + 0x18) + F(s, B + 0x38), fr, 0, 0, U(s, p + 0x60));
    }
    foot(s, p, F(s, p + 0x10), F(s, p + 0x14), F(s, p + 0x18));
    if (I(s, p + 0x7C) >= I(s, G_FX) && (side || I(s, p + 0x80) >= I(s, G_FZ))
        && (ext)F(s, p + 0x14) > (ext)F(s, 0x008D5710u))
        return -1;
    if (below(F(s, p + 0x14), F(s, p + 0x28))) return -1;
    return 0;
}

/* 0x004B5A8F, the sparks' streak, entered from 0x004BF6F0 with EAX 0x20 (the
 * cell), EDX 0x800 (twice the half width 0x400), ECX the alpha index and ESI
 * its colour.  The previous and current camera points go through F / z to
 * the screen, the perpendicular of the segment between them is scaled by the
 * half width and each end's own scale, and the four corners, with the cell's
 * UVs, are the polygon [0x009089A0] hands the clip 0x004A8619.  Nothing is
 * drawn when either end is not past the near plane; a segment of no length
 * divides by zero (the original's clip is handed NaN: the port's draw drops
 * it). */
static void streak(spray *s, const void *v, uint32_t p, const float prev[3],
                   const float cur[3])
{
    spray_streak o;
    ext sc1, sc0, q, inv;
    float x1, y1, x0, y0, dx, dy, px, py, s1, s0, w1x, w1y, w0x, w0y;
    float z1, z0, uv[4];
    uint32_t tab, col, esi = 0, e = 0x008D5806u + 0x20u * 16u;
    int k;

    if ((int32_t)fbits(cur[2]) <= (int32_t)s->near_bits) return;
    if ((int32_t)fbits(prev[2]) <= (int32_t)s->near_bits) return;
    sc1 = (ext)s->proj.focal / cur[2];
    x1 = (float)(sc1 * cur[0] + s->proj.cx);
    y1 = (float)(-(sc1 * cur[1]) + s->proj.cy);
    s1 = (float)sc1;
    sc0 = (ext)s->proj.focal / prev[2];
    x0 = (float)(sc0 * prev[0] + s->proj.cx);
    y0 = (float)(-(sc0 * prev[1]) + s->proj.cy);
    s0 = (float)sc0;
    dx = (float)((ext)x0 - x1);
    dy = (float)((ext)y0 - y1);
    q = (ext)dx * dx + (ext)dy * dy;
    inv = EXT_C(1.0) / ext_sqrt(q);               /* FLD1 / FSQRT: inf at 0          */
    px = (float)((ext)dy * inv);
    py = (float)(-((ext)dx * inv));
    w1x = (float)((ext)px * 0x400 * s1);
    w1y = (float)((ext)py * 0x400 * s1);
    w0x = (float)((ext)px * 0x400 * s0);
    w0y = (float)((ext)py * 0x400 * s0);
    z1 = (float)((ext)cur[2] * s->proj.zscale);
    z0 = (float)((ext)prev[2] * s->proj.zscale);

    /* ESI = the alpha table's entry, [ECX * 4 + 0x008D9E52] (0x008D9C52 under
     * [0x0070D5A8]); ECX = [0x008D5760], the caller's FISTP */
    tab = (I(s, 0x0070D5A8u) != 0 ? 0x008D9C52u : 0x008D9E52u)
        + (uint32_t)I(s, G_T) * 4u;
    if (spray_at(s, tab)) esi = U(s, tab);
    else s->streak_oob++;
    col = (U(s, G_T) << 24) | esi;
    for (k = 0; k < 4; k++) uv[k] = F(s, e + 4u * (uint32_t)k);

    o.x[0] = (float)((ext)x1 - w1x);  o.y[0] = (float)((ext)y1 - w1y);
    o.x[1] = (float)((ext)x1 + w1x);  o.y[1] = (float)((ext)y1 + w1y);
    o.x[2] = (float)((ext)x0 + w0x);  o.y[2] = (float)((ext)y0 + w0y);
    o.x[3] = (float)((ext)x0 - w0x);  o.y[3] = (float)((ext)y0 - w0y);
    o.z[0] = o.z[1] = z1;
    o.z[2] = o.z[3] = z0;
    for (k = 0; k < 4; k++) o.col[k] = col;
    o.u[0] = uv[0]; o.v[0] = uv[1];      /* u0 v0, u1 v0, u1 v1, u0 v1      */
    o.u[1] = uv[2]; o.v[1] = uv[1];
    o.u[2] = uv[2]; o.v[2] = uv[3];
    o.u[3] = uv[0]; o.v[3] = uv[3];
    o.fogz = U(s, p + 0x24);
    o.px = px; o.py = py; o.w1x = w1x; o.w1y = w1y; o.w0x = w0x; o.w0y = w0y;
    o.s1 = s1; o.s0 = s0;
    s->streaks++;
    s->streak(s, v, &o);
}

/* 0x004BF4AB, the second pass: the sparks (0x12), their streak 0x004B5A8F
 * after the step. */
static int upd_spark(spray *s, const void *v, uint32_t p)
{
    ext c[3];
    float prev[3], cur[3];
    int k;
    if (!life(s, p)) return -1;
    if (I(s, p + 0x10) == 0) {
        xform(s, SPRAY_VIEW_VA, F(s, p + 0x10), F(s, p + 0x14), F(s, p + 0x18), c);
        for (k = 0; k < 3; k++) WF(s, p + 0x1C + 4u * (uint32_t)k, c[k]);
        move(s, p);
        fall(s, p, 0x008DA60Eu);
    }
    move(s, p);
    fall(s, p, 0x008DA612u);
    for (k = 0; k < 3; k++) prev[k] = F(s, p + 0x1C + 4u * (uint32_t)k);
    xform(s, SPRAY_VIEW_VA, F(s, p + 0x10), F(s, p + 0x14), F(s, p + 0x18), c);
    for (k = 0; k < 3; k++) cur[k] = (float)c[k];
    if (I(s, BLK(s, 0xC)) != 0 || I(s, BLK(s, 0x10)) == 1)
        for (k = 0; k < 3; k++) WF(s, p + 0x1C + 4u * (uint32_t)k, c[k]);
    WI(s, G_T, fistp((ext)I(s, p + 0x84) * F(s, p + 0x78)));
    if (s->streak) streak(s, v, p, prev, cur);
    return 0;
}

/* 0x004BB29A / 0x004BB367: the live list, a fixed count of steps from the
 * head; a dead record is moved to just before the head */
static void pass(spray *s, const spray_view *v, int second)
{
    uint32_t p = U(s, G_POOL);
    int32_t i;
    for (i = I(s, G_COUNT) - 1; i >= 0; i--) {
        uint32_t next = U(s, p + 4);
        int dead = 0;
        if (!second) {
            switch (I(s, p + 0xC)) {
            case 0x05: dead = upd_ground(s, v, p, 0x008DA57Au); break;
            case 0x0B: dead = upd_ground(s, v, p, 0x008DA586u); break;
            case 0x06: dead = upd_mark(s, v, p, 0x008DA592u); break;
            case 0x15: dead = upd_mark(s, v, p, 0x008DA5A2u); break;
            case 0x01: dead = upd_dry(s, v, p); break;
            case 0x0D: dead = upd_exhaust(s, v, p); break;
            case 0x10: dead = upd_spray(s, v, p, 0); break;
            case 0x11: dead = upd_spray(s, v, p, 1); break;
            case 0x13: dead = upd_smoke(s, v, p, 0x008DA5E6u); break;
            case 0x16: dead = upd_smoke(s, v, p, 0x008DA5F6u); break;
            default:   break;            /* never dies                       */
            }
        } else if (I(s, BLK(s, 0x60)) != 0 && I(s, p + 0xC) == 0x12) {
            /* gated on CAR 0's object, block +0x60, whatever the spark's
             * car */
            dead = upd_spark(s, v, p);
        }
        if (dead) {
            s->died++;
            WI(s, G_COUNT, I(s, G_COUNT) - 1);
            if (p == U(s, G_POOL)) {
                WU(s, G_POOL, U(s, p + 4));
            } else {
                uint32_t pr = U(s, p), nx = U(s, p + 4), h = U(s, G_POOL);
                uint32_t hp = U(s, h);
                WU(s, pr + 4, nx);
                WU(s, nx, pr);
                WU(s, h, p);
                WU(s, hp + 4, p);
                WU(s, p, hp);
                WU(s, p + 4, h);
            }
        }
        p = next;
    }
}

void spray_update(spray *s, const spray_view *v)
{
    if (!s->have) return;
    WU(s, 0x008D9C46u, U(s, BLK(s, 0x7C)) & 0xFFFFFFu);
    WI(s, G_RAIN, s->rain85);            /* 0x004BB27B                      */
    pass(s, v, 0);
    if (v && s->second_pass) s->second_pass(s);
    pass(s, v, 1);
}

/* ---- the clock and the snapshot ------------------------------------------ */

void spray_clock(spray *s, int32_t dt, int32_t rain, int advance)
{
    ext inv;
    WI(s, BLK(s, 0x10), advance ? 1 : 0);
    WI(s, K_DT, dt);
    WU(s, 0x0070D34Cu, (uint32_t)dt >> 2);
    WU(s, 0x0070D348u, (uint32_t)dt >> 4);
    inv = EXT_C(1.0) / (ext)dt;                /* FLD1 / FIDIV: inf at 0          */
    WF(s, K_INVDT, inv);
    WF(s, 0x0070D354u, inv * F(s, 0x0070D52Cu));
    /* 0x004C284D: FLD the integer dt's bits as a float - a denormal, so the
     * particles' spin is all but 0 */
    WF(s, K_SPIN, (ext)F(s, K_DT) * s->k8dccb4);
    rain *= 2;
    s->rain85 = rain > 0x100 ? 0x100 : rain;
}

static int32_t smoke_level(const spray *s, float x)
{
    int32_t v;
    if (!(x >= s->smoke_k0)) return 0;   /* C0: below or unordered          */
    v = fistp(((ext)x - s->smoke_k0) / s->smoke_k1);
    if (v < 0) v = 0;
    if (v >= 7) v = 7;
    return v;
}

void spray_car(spray *s, int car, const spray_car_in *in)
{
    uint32_t S, mb;
    int i, m;
    if (car < 0 || car >= SPRAY_CARS) return;
    S = BLK(s, 0x60u + (uint32_t)car * SPRAY_SNAP);
    mb = SPRAY_MAT_VA + (uint32_t)car * 0x80u;
    WI(s, S, in->live);                  /* the rest is copied for every car
                                          * the snapshot loop visits        */
    memcpy(A(s, mb), in->body, 64);
    memcpy(A(s, mb + 0x40), in->front, 64);
    /* FUN_004547E1 */
    if (in->reset != 0) WI(s, S + 0x30, 1);
    WI(s, S + 0x4C0, smoke_level(s, in->smoke_a));
    WI(s, S + 0x4C4, smoke_level(s, in->smoke_b));
    WU(s, S + 0x1C, in->ground_col);
    m = in->model < 0 ? 0 : in->model > 31 ? 31 : in->model;
    WI(s, S + 0x14, s->geom[m][2]);
    WI(s, S + 0x18, s->geom[m][3]);
    WI(s, S + 0xC, s->geom[m][0]);
    WI(s, S + 0x10, s->geom[m][1]);
    WU(s, S + 0x24, mb);
    WU(s, S + 0x28, mb + 0x40);
    WF(s, S + 8, in->revs);
    WI(s, S + 4, in->model);
    for (i = 0; i < 4; i++) {
        uint32_t W = S + 0xD4 + (uint32_t)i * 0x48;
        WF(s, W + 0xC, i < 2 ? in->fwhlspd : in->rwhlspd);
        WF(s, W + 0x10, in->cxspd);
        WI(s, W + 0x14, 0);
        WF(s, W + 0x18, in->czspd);
        WI(s, W + 0x1C, in->cls_a[i]);
        if (fbits(in->susp[i]) == 0
            && ((uint32_t)in->partsleft >> (5 + i) & 1u))
            WI(s, W + 0x1C, 0);
        WI(s, W + 0x20, in->aux18_a[i]);
        WI(s, W + 0x24, in->aux14_a[i]);
        WI(s, W + 0x2C, in->aux18_b[i]);
        WI(s, W + 0x30, in->aux14_b[i]);
        WF(s, W + 0x34, in->weight[i]);
    }
    {
        uint32_t W = S + 0x1F4;
        WF(s, W + 0x10, in->vel_x);
        WF(s, W + 0x18, in->vel_z);
        WI(s, W + 0x1C, 0);
        WI(s, W + 0x28, 0);
        if (in->body_contact) {
            WI(s, W + 0x20, in->aux18_a[4]);
            WI(s, W + 0x24, in->aux14_a[4]);
            WI(s, W + 0x2C, in->aux18_b[4]);
            WI(s, W + 0x30, in->aux14_b[4]);
            WF(s, W + 0x34, in->weight[4]);
            WI(s, W + 0x1C, in->cls_a[4]);
            WI(s, W + 0x28, in->cls_b[4]);
            WF(s, W + 0x3C, (ext)in->contact[0]);
            WF(s, W + 0x40, (ext)in->contact[1]);
            WF(s, W + 0x44, (ext)in->contact[2]);
            WF(s, W + 0x38, in->contact_vt);
        }
    }
}

/* ---- the load ------------------------------------------------------------ */

static void copy_region(const ral_image *im, uint8_t *dst, uint32_t va0,
                        uint32_t va1)
{
    uint32_t va;
    for (va = va0; va < va1; va++) {
        const unsigned char *b = ral_image_at(im, va, 1);
        dst[va - va0] = b ? *b : 0;
    }
}

static int img_f32(const ral_image *im, uint32_t va, float *out)
{
    const unsigned char *b = ral_image_at(im, va, 4);
    if (!b) return -1;
    *out = ral_image_f32(b);
    return 0;
}

int spray_load(spray *s, const ral_image *im, const uint8_t *textinfo,
               size_t textinfo_len, char *err, size_t errlen)
{
    int i, m;
    const unsigned char *b;
    uint32_t keep = s->atlas;
    void (*quad)(struct spray_s *, const void *, int, const float *, uint32_t,
                 uint32_t, uint32_t, float, float) = s->quad;
    void (*streak_cb)(struct spray_s *, const void *, const spray_streak *)
        = s->streak;
    void (*second_pass)(struct spray_s *) = s->second_pass;
    spray_proj proj = s->proj;

    memset(s, 0, sizeof *s);
    s->atlas = keep;
    s->quad = quad;
    s->streak = streak_cb;
    s->second_pass = second_pass;
    s->proj = proj;
    s->lcg = &s->own_lcg;
    copy_region(im, s->d, SPRAY_D0, SPRAY_D1);
    copy_region(im, s->k, SPRAY_K0, SPRAY_K1);
    copy_region(im, s->t, SPRAY_T0, SPRAY_T1);
    copy_region(im, s->block, SPRAY_BLOCK, SPRAY_BLOCK + SPRAY_BLOCK_LEN);
    sim_sin_build(s->sin_t);             /* 0x009C3EE4, the draw's angles   */
    b = ral_image_at(im, 0x0088C221u, 4);
    s->near_bits = b ? (uint32_t)ral_image_i32(b) : 0;
    b = ral_image_at(im, 0x008C38C4u, 4);
    s->jit = b ? (uint32_t)ral_image_i32(b) : 0;
    if (img_f32(im, 0x008DCCB4u, &s->k8dccb4) != 0) s->k8dccb4 = 0.0f;
    if (img_f32(im, 0x005F0A1Cu, &s->smoke_k0) != 0
        || img_f32(im, 0x0057AFC8u, &s->smoke_k1) != 0) {
        snprintf(err, errlen, "the spray's smoke constants are not in the image");
        return -1;
    }
    for (m = 0; m < 32; m++)
        for (i = 0; i < 4; i++) {
            b = ral_image_at(im, 0x0057E10Cu + 0x12Cu * (uint32_t)m + 0x24u
                                 + 4u * (uint32_t)i, 4);
            s->geom[m][i] = b ? ral_image_i32(b) : 0;
        }
    /* 0x004B604C: TEXTINFO records 14,400.. (0x88C231 = textinfo +0x1C2000,
     * 0x004B3AC3), per record its height over its width, then its four
     * corners x [0x0088C229] */
    if (!textinfo || textinfo_len < 0x1C2000u + 0x8000u) {
        snprintf(err, errlen, "the spray needs TEXTINFO");
        return -1;
    }
    {
        float sc;
        if (img_f32(im, 0x0088C229u, &sc) != 0) sc = 0.0f;
        for (i = 0xFF; i >= 0; i--) {
            const uint8_t *r = textinfo + 0x1C2000u + (uint32_t)i * 0x80u;
#define W16(o) ((uint32_t)r[o] | (uint32_t)r[(o) + 1] << 8)
            uint32_t dx = (W16(4) - W16(0)) & 0xFFFFu, dy = (W16(0xA) - W16(2)) & 0xFFFFu;
            float *e = s->aspect + i * 9;
            if (dx & 0x8000u) dx = (0u - dx) & 0xFFFFu;
            if (dy & 0x8000u) dy = (0u - dy) & 0xFFFFu;
            e[0] = (float)((ext)(int32_t)dy / (ext)(int32_t)dx);
            e[1] = (float)((ext)W16(0) * sc);   e[2] = (float)((ext)W16(2) * sc);
            e[3] = (float)((ext)W16(4) * sc);   e[4] = (float)((ext)W16(6) * sc);
            e[5] = (float)((ext)W16(0xC) * sc); e[6] = (float)((ext)W16(0xE) * sc);
            e[7] = (float)((ext)W16(8) * sc);   e[8] = (float)((ext)W16(0xA) * sc);
#undef W16
        }
    }
    /* 0x004BAEF7: the 64 cells' UVs, then each cell's 4 x 4 sub-cells */
    {
        ext k530 = F(s, 0x0070D530u), k538 = F(s, 0x0070D538u);
        ext k53c = F(s, 0x0070D53Cu), k540 = F(s, 0x0070D540u);
        for (i = 63; i >= 0; i--) {
            uint32_t e = 0x008D5806u + (uint32_t)i * 16u;
            ext u = (ext)(i & 7) * k530 + k538, v = (ext)(i >> 3) * k530 + k538;
            WF(s, e, u);                 /* FST: the sum is the ext's       */
            WF(s, e + 8, u + k530 - k53c);
            WF(s, e + 4, v);
            WF(s, e + 0xC, v + k530 - k53c);
        }
        for (i = 63; i >= 0; i--) {
            uint32_t e = 0x008D5C06u + (uint32_t)i * 0x100u;
            ext u = (ext)(i & 7) * k530 + k538, v = (ext)(i >> 3) * k530 + k538;
            int row, col;
            WF(s, e, u);
            WF(s, e + 8, u + k540 - k53c);
            WF(s, e + 4, v);
            WF(s, e + 0xC, v + k540 - k53c);
            for (row = 0; row < 4; row++)
                for (col = 0; col < 4; col++) {
                    uint32_t q;
                    if (row == 0 && col == 0) continue;
                    q = e + (uint32_t)(row * 4 + col) * 16u;
                    if (col == 0) {      /* the next row: back 3, down 1    */
                        WF(s, q, (ext)F(s, q - 16) - k540 - k540 - k540);
                        WF(s, q + 4, (ext)F(s, q - 12) + k540);
                        WF(s, q + 8, (ext)F(s, q - 8) - k540 - k540 - k540);
                        WF(s, q + 0xC, (ext)F(s, q - 4) + k540);
                    } else {
                        WF(s, q, (ext)F(s, q - 16) + k540);
                        WU(s, q + 4, U(s, q - 12));
                        WF(s, q + 8, (ext)F(s, q - 8) + k540);
                        WU(s, q + 0xC, U(s, q - 4));
                    }
                }
        }
    }
    s->have = 1;
    spray_race_start(s);
    return 0;
}
