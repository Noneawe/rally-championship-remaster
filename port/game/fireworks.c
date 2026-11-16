/* fireworks.c - the championship finish's fireworks,
 * 2026-11-04; fireworks.h says what is the original's and what is not,
 * is the reading.  Every address is retail's.  The
 * arithmetic follows the x87 sequences: an FSTP is a float store, the rest
 * `ext`; the integer stores (FISTP) round to nearest even and give the
 * integer indefinite out of range. */

#include "fireworks.h"

#include "image.h"
#include "physics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext.h"

/* ---- the pool ---------------------------------------------------------- */

uint8_t *fireworks_at(fireworks *w, uint32_t va)
{
    if (va >= FW_POOL_VA && va + 4 <= FW_POOL_VA + sizeof w->pool)
        return w->pool + (va - FW_POOL_VA);
    return NULL;
}

static uint8_t dummy[8];

static uint8_t *A(fireworks *w, uint32_t va)
{
    uint8_t *p = fireworks_at(w, va);
    return p ? p : dummy;                /* a link outside the pool: the gate
                                          * checks none is followed          */
}

static uint32_t U(fireworks *w, uint32_t va)
{
    uint32_t v;
    memcpy(&v, A(w, va), 4);
    return v;
}

static int32_t I(fireworks *w, uint32_t va) { return (int32_t)U(w, va); }

static float F(fireworks *w, uint32_t va)
{
    float v;
    memcpy(&v, A(w, va), 4);
    return v;
}

static void WU(fireworks *w, uint32_t va, uint32_t v) { memcpy(A(w, va), &v, 4); }
static void WI(fireworks *w, uint32_t va, int32_t v) { memcpy(A(w, va), &v, 4); }
static void WF(fireworks *w, uint32_t va, ext v)
{
    float f = (float)v;                  /* FSTP dword                       */
    memcpy(A(w, va), &f, 4);
}

static int32_t fistp(ext x)              /* FISTP, round to nearest even     */
{
    if (!(x >= -EXT_C(2147483648.0) && x < EXT_C(2147483648.0)))
        return INT32_MIN;                /* the integer indefinite           */
    return (int32_t)ext_llrint(x);
}

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t ror(uint32_t v, unsigned n)
{
    return (v >> n) | (v << (32u - n));
}

/* the shared generator, [0x006CF334] = [0x006CF334] * 0x9D + 0x47 */
static uint32_t L(fireworks *w)
{
    *w->lcg = *w->lcg * 0x9Du + 0x47u;
    return *w->lcg;
}

/* the constants 0x008DC6B8 + 4 * i */
static float K(const fireworks *w, uint32_t va) { return w->k[(va - 0x008DC6B8u) >> 2]; }
static int32_t KI(const fireworks *w, uint32_t va)
{
    int32_t v;
    memcpy(&v, &w->k[(va - 0x008DC6B8u) >> 2], 4);
    return v;
}

/* ---- the load ---------------------------------------------------------- */

static int take(const ral_image *im, uint32_t va, void *dst, size_t n)
{
    const unsigned char *p = ral_image_at(im, va, n);
    if (!p) return -1;
    memcpy(dst, p, n);
    return 0;
}

int fireworks_load(fireworks *w, const ral_image *im, char *err, size_t errlen)
{
    void (*q)(void *, const fw_quad *) = w->quad;
    void *ud = w->ud;
    memset(w, 0, sizeof *w);
    w->quad = q;
    w->ud = ud;
    w->lcg = &w->own_lcg;
    if (take(im, 0x008DC6B8u, w->k, sizeof w->k) != 0
        || take(im, 0x0088C221u, &w->near_bits, 4) != 0
        || take(im, 0x0088C229u, &w->inv65536, 4) != 0
        || take(im, 0x008DA6B8u, w->ramp, sizeof w->ramp) != 0
        || take(im, 0x008C33F8u, w->spark, sizeof w->spark) != 0) {
        snprintf(err, errlen, "the fireworks' tables are not in the image");
        return -1;
    }
    sim_sin_build(w->sin_t);             /* 0x009C3EE4: built at run time    */
    w->have = 1;
    fireworks_reset(w);
    return 0;
}

/* 0x004BF712 (the ring) and 0x004C0FC5 (the frame counter) */
void fireworks_reset(fireworks *w)
{
    uint32_t i;
    for (i = 0; i < FW_N; i++) {
        uint32_t b = FW_POOL_VA + ((i - 1u) & 0xFFFu) * FW_REC;
        uint32_t n = FW_POOL_VA + ((i + 1u) & 0xFFFu) * FW_REC;
        memcpy(&w->pool[i * FW_REC + FWR_PREV], &b, 4);
        memcpy(&w->pool[i * FW_REC + FWR_NEXT], &n, 4);
    }
    w->head = FW_POOL_VA;
    w->count = 0;
    w->frame = 0;
    w->whoosh = w->boom = 0;
}

/* ---- the pool's list --------------------------------------------------- */

/* 0x004BF7EC: the particle out; a record not at the head goes to just before
 * it, where the next spawn finds it */
static void kill(fireworks *w, uint32_t esi)
{
    uint32_t ebx, ecx;
    w->count--;
    if (esi == w->head) {
        w->head = U(w, esi + FWR_NEXT);
        return;
    }
    ebx = U(w, esi + FWR_PREV);
    ecx = U(w, esi + FWR_NEXT);
    WU(w, ebx + FWR_NEXT, ecx);
    WU(w, ecx + FWR_PREV, ebx);
    ebx = w->head;
    ecx = U(w, ebx + FWR_PREV);
    WU(w, ebx + FWR_PREV, esi);
    WU(w, ecx + FWR_NEXT, esi);
    WU(w, esi + FWR_PREV, ecx);
    WU(w, esi + FWR_NEXT, ebx);
}

/* 0x004BFBBF / 0x004BFC4A / 0x004BFE14: the next record, or 0 when 0xFFF
 * are live */
static uint32_t alloc(fireworks *w)
{
    uint32_t esi;
    if (w->count == 0xFFF) { w->full++; return 0; }
    esi = U(w, w->head + FWR_PREV);
    w->head = esi;
    w->count++;
    return esi;
}

/* ---- the quad: 0x004BFA22 ---------------------------------------------- */

static void point(fireworks *w, const fireworks_in *in, ext x, ext y, ext z,
                  uint32_t col, int32_t size)
{
    float Xf, Yf, Zf, hf, zz, sy_c[2], sx_c[2];
    ext sc, sy, sx;
    fw_quad q;
    const float *m = in->view;

    /* the row vector times the 4 x 4: each output (a + (b + c)) + m[12+c] */
    Xf = (float)(((ext)m[0] * x + ((ext)m[4] * y + (ext)m[8] * z)) + (ext)m[12]);
    Yf = (float)(((ext)m[1] * x + ((ext)m[5] * y + (ext)m[9] * z)) + (ext)m[13]);
    Zf = (float)(((ext)m[2] * x + ((ext)m[6] * y + (ext)m[10] * z)) + (ext)m[14]);
    if ((int32_t)fbits(Zf) <= (int32_t)w->near_bits) { w->behind++; return; }
    sc = (ext)in->focal / (ext)Zf;
    zz = (float)((ext)Zf * (ext)in->zscale);
    hf = (float)((ext)size * sc);
    sy = -(sc * (ext)Yf) + (ext)in->cy;
    sx = sc * (ext)Xf;
    sy_c[0] = (float)(sy - (ext)hf);
    sy_c[1] = (float)(sy + (ext)hf);
    sx = sx + (ext)in->cx;
    sx_c[0] = (float)(sx - (ext)hf);
    sx_c[1] = (float)(sx + (ext)hf);
    q.x[0] = sx_c[0]; q.y[0] = sy_c[0];
    q.x[1] = sx_c[1]; q.y[1] = sy_c[0];
    q.x[2] = sx_c[1]; q.y[2] = sy_c[1];
    q.x[3] = sx_c[0]; q.y[3] = sy_c[1];
    q.z = zz;
    q.col = col;
    memcpy(q.uv, w->uv, sizeof q.uv);
    w->drawn++;
    if (w->quad) w->quad(w->ud, &q);
}

/* ---- the spawns -------------------------------------------------------- */

/* 0x004B5D32: the three words each + (a signed 20-bit random % k), the
 * remainder FPREM's, so with the dividend's sign */
static void jitter(fireworks *w, float v[3], float k)
{
    int c;
    for (c = 0; c < 3; c++) {
        int32_t r = (int32_t)L(w) >> 12;
        ext rem = ext_fmod((ext)r, (ext)k);
        v[c] = (float)(rem + (ext)v[c]);
    }
}

/* 0x004BFBA4: a rocket, EAX = 0 */
static void spawn_rocket(fireworks *w, const fireworks_in *in)
{
    uint32_t r = L(w), esi;
    ext ang;
    float X0, Y0, Z0, vx, vy, vz;
    ext e = (ext)(r & 0xFFFFu);

    ang = ((e * (ext)w->inv65536) * (ext)K(w, 0x008DC6B8u)) * (ext)K(w, 0x008DC6BCu);
    X0 = (float)(ext_sin(ang) * (ext)K(w, 0x008DC6C0u) + (ext)in->car[0]);
    Y0 = in->car[1];
    ang = ((e * (ext)w->inv65536) * (ext)K(w, 0x008DC6C4u)) * (ext)K(w, 0x008DC6C8u);
    Z0 = (float)(ext_cos(ang) * (ext)K(w, 0x008DC6CCu) + (ext)in->car[2]);
    r = L(w);
    vy = (float)((((ext)(r & 0xFFFFu) * (ext)w->inv65536 + (ext)K(w, 0x008DC6D0u))
                  * (ext)K(w, 0x008DC6D4u)) + (ext)K(w, 0x008DC6D8u));
    vx = (float)(((ext)in->car[0] - (ext)X0) * (ext)K(w, 0x008DC6DCu));
    vz = (float)(((ext)in->car[2] - (ext)Z0) * (ext)K(w, 0x008DC6E0u));

    esi = alloc(w);
    if (!esi) return;
    WI(w, esi + FWR_TYPE, 0);
    WF(w, esi + FWR_VZ, vz);
    WF(w, esi + FWR_VY, vy);
    WF(w, esi + FWR_VX, vx);
    WF(w, esi + FWR_Z, Z0);
    WF(w, esi + FWR_PZ, Z0);
    WF(w, esi + FWR_Y, Y0);
    WF(w, esi + FWR_PY, (ext)Y0 - (ext)K(w, 0x008DC6E4u));
    WF(w, esi + FWR_X, X0);
    WF(w, esi + FWR_PX, X0);
    WI(w, esi + FWR_SIZE, 0x8000);
    WU(w, esi + FWR_COL, 0xFF000000u);
    WI(w, esi + FWR_LIFE, 0xC350);
    WF(w, esi + FWR_CEIL, in->car[1]);
    w->rockets++;
    w->born[0]++;
}

/* 0x004BFC30: eight trail sparks from the parent EDI */
static void spawn_sparks(fireworks *w, const fireworks_in *in, uint32_t edi)
{
    int n;
    for (n = 0; n < 8; n++) {
        uint32_t esi = alloc(w), r;
        float u, v[3];
        ext px, py, pz, x, y, z;
        if (!esi) continue;
        WI(w, esi + FWR_TYPE, 4);
        r = L(w);
        u = (float)(((ext)(r & 0xFFFFu) * (ext)w->inv65536) * (ext)K(w, 0x008DC6E8u));
        x = (ext)F(w, edi + FWR_X);
        y = (ext)F(w, edi + FWR_Y);
        z = (ext)F(w, edi + FWR_Z);
        px = (ext)F(w, edi + FWR_PX);
        py = (ext)F(w, edi + FWR_PY);
        pz = (ext)F(w, edi + FWR_PZ);
        WF(w, esi + FWR_X, x + (px - x) * (ext)u);
        WF(w, esi + FWR_Y, y + (py - y) * (ext)u);
        WF(w, esi + FWR_Z, z + (pz - z) * (ext)u);
        WF(w, esi + FWR_VX, (px - x) * (ext)K(w, 0x008DC6ECu)
                            + (ext)F(w, edi + FWR_VX) * (ext)K(w, 0x008DC6F0u));
        WF(w, esi + FWR_VY, (py - y) * (ext)K(w, 0x008DC6F4u)
                            + (ext)F(w, edi + FWR_VY) * (ext)K(w, 0x008DC6F8u));
        WF(w, esi + FWR_VZ, (pz - z) * (ext)K(w, 0x008DC6FCu)
                            + (ext)F(w, edi + FWR_VZ) * (ext)K(w, 0x008DC700u));
        v[0] = F(w, esi + FWR_VX);
        v[1] = F(w, esi + FWR_VY);
        v[2] = F(w, esi + FWR_VZ);
        jitter(w, v, K(w, 0x008DC704u));
        WF(w, esi + FWR_VZ, v[2]);
        WF(w, esi + FWR_VY, v[1]);
        WF(w, esi + FWR_VX, v[0]);
        WI(w, esi + FWR_SIZE, fistp((ext)K(w, 0x008DC708u) * (ext)F(w, edi + FWR_VY)
                                    + (ext)K(w, 0x008DC70Cu)));
        WU(w, esi + FWR_COL, 0xFF000000u);
        WI(w, esi + FWR_LIFE, 0x12C);
        WF(w, esi + FWR_RATE, (ext)K(w, 0x008DC710u) / (ext)I(w, esi + FWR_LIFE));
        WF(w, esi + FWR_CEIL, in->car[1]);
        w->sparks++;
        w->born[4]++;
    }
}

/* 0x004BFD93: the burst, 256 particles from the parent EDI */
static void burst(fireworks *w, uint32_t edi)
{
    uint32_t r = L(w), ramp_off, type;
    float scale;
    int n;

    w->boom = 1;
    scale = (float)((((ext)(r & 0xFFFFu) * (ext)w->inv65536) + (ext)K(w, 0x008DC714u))
                    * (ext)K(w, 0x008DC718u));
    ramp_off = (r & 7u) * 0x400u;
    type = ((ror(r, 16) & 1u) + 8u);
    w->bursts++;
    for (n = 0; n < 0x100; n++) {
        uint32_t esi = alloc(w), s, a, b;
        ext sa, ca, sb, cb;
        if (!esi) break;                 /* je 0x004BFEF2: popal, next       */
        WU(w, esi + FWR_TYPE, type);
        WU(w, esi + FWR_RAMP, ramp_off);
        WF(w, esi + FWR_X, (ext)F(w, edi + FWR_X));
        WF(w, esi + FWR_Y, (ext)F(w, edi + FWR_Y));
        WF(w, esi + FWR_Z, (ext)F(w, edi + FWR_Z));
        s = L(w);
        a = ror(s, 16) & 0xFFFu;
        b = s & 0xFFFu;
        sa = (ext)w->sin_t[a];
        ca = (ext)w->sin_t[a + 0x400u];
        sb = (ext)w->sin_t[b];
        cb = (ext)w->sin_t[b + 0x400u];
        WF(w, esi + FWR_VZ, sa * (ext)scale);
        WF(w, esi + FWR_VX, (ca * sb + (ext)0 * cb) * (ext)scale);
        WF(w, esi + FWR_VY, (ca * cb + (-(ext)0) * sb) * (ext)scale);
        WI(w, esi + FWR_AY, 0);
        WI(w, esi + FWR_SIZE, 0x10000);
        WU(w, esi + FWR_COL, 0xFF000000u);
        WI(w, esi + FWR_LIFE, 0xBB8);
        WF(w, esi + FWR_RATE, (ext)K(w, 0x008DC71Cu) / (ext)I(w, esi + FWR_LIFE));
        w->burst_particles++;
        w->born[type]++;
    }
}

/* ---- the types' steps -------------------------------------------------- */

/* x += v * dt, as FLD v / FIMUL dt / FADD x / FSTP x */
static void adv(fireworks *w, uint32_t esi, int vo, int po, int32_t dt)
{
    WF(w, esi + (uint32_t)po, (ext)F(w, esi + (uint32_t)vo) * (ext)dt
                               + (ext)F(w, esi + (uint32_t)po));
}

/* the colour's alpha: FILD k / FIMUL dt / FIADD [+0x3C] / FISTP */
static void fade(fireworks *w, uint32_t esi, int32_t k, int32_t dt)
{
    WI(w, esi + FWR_COL, fistp((ext)k * (ext)dt + (ext)I(w, esi + FWR_COL)));
}

/* 0x004BFF01: a rocket; 1 when it burst (the caller kills it) */
static int step_rocket(fireworks *w, const fireworks_in *in, uint32_t esi)
{
    int32_t dt = in->dt;
    adv(w, esi, FWR_VX, FWR_X, dt);
    adv(w, esi, FWR_VY, FWR_Y, dt);
    adv(w, esi, FWR_VZ, FWR_Z, dt);
    WF(w, esi + FWR_VY, (ext)K(w, 0x008DC720u) * (ext)dt + (ext)F(w, esi + FWR_VY));
    fade(w, esi, KI(w, 0x008DC724u), dt);
    if (!((ext)K(w, 0x008DC728u) < (ext)F(w, esi + FWR_VY))) {
        burst(w, esi);
        return 1;
    }
    if (!(U(w, esi + FWR_VY) & 0x80000000u))
        spawn_sparks(w, in, esi);
    WU(w, esi + FWR_PX, U(w, esi + FWR_X));
    WU(w, esi + FWR_PY, U(w, esi + FWR_Y));
    WU(w, esi + FWR_PZ, U(w, esi + FWR_Z));
    return 0;
}

/* 0x004BFFA9: a trail spark (its colour from 0x008C33F8) */
static int step_spark(fireworks *w, const fireworks_in *in, uint32_t esi)
{
    int32_t dt = in->dt, idx;
    uint32_t col;
    adv(w, esi, FWR_VX, FWR_X, dt);
    adv(w, esi, FWR_VY, FWR_Y, dt);
    adv(w, esi, FWR_VZ, FWR_Z, dt);
    WF(w, esi + FWR_VY, (ext)K(w, 0x008DC72Cu) * (ext)dt + (ext)F(w, esi + FWR_VY));
    fade(w, esi, KI(w, 0x008DC730u), dt);
    idx = fistp((ext)I(w, esi + FWR_LIFE) * (ext)F(w, esi + FWR_RATE));
    col = (idx >= 0 && idx < 0x400) ? w->spark[idx] : 0u;
    col = (col & 0xFFFFFFu) | (U(w, esi + FWR_COL) & 0xFF000000u);
    point(w, in, (ext)F(w, esi + FWR_X), (ext)F(w, esi + FWR_Y), (ext)F(w, esi + FWR_Z),
          col, I(w, esi + FWR_SIZE));
    WU(w, esi + FWR_PX, U(w, esi + FWR_X));
    WU(w, esi + FWR_PY, U(w, esi + FWR_Y));
    WU(w, esi + FWR_PZ, U(w, esi + FWR_Z));
    return 0;
}

/* 0x004C0051 (type 8) and 0x004C011C (type 9): a burst particle; 1 when its
 * ramp has run out.  Type 9 flares: a 1 in 128 draw adds 0x386A0 to its size */
static int step_burst(fireworks *w, const fireworks_in *in, uint32_t esi, int t9)
{
    int32_t dt = in->dt, idx, size;
    uint32_t col, ramp, k;
    ext vy;
    adv(w, esi, FWR_VX, FWR_X, dt);
    vy = (ext)F(w, esi + FWR_VY) + (ext)F(w, esi + FWR_AY);
    WF(w, esi + FWR_Y, vy * (ext)dt + (ext)F(w, esi + FWR_Y));
    adv(w, esi, FWR_VZ, FWR_Z, dt);
    k = t9 ? 0x008DC748u : 0x008DC734u;
    WF(w, esi + FWR_AY, (ext)K(w, k) * (ext)dt + (ext)F(w, esi + FWR_AY));
    k = t9 ? 0x008DC74Cu : 0x008DC738u;
    WF(w, esi + FWR_VX, (ext)F(w, esi + FWR_VX) - (ext)F(w, esi + FWR_VX) * (ext)K(w, k));
    k = t9 ? 0x008DC750u : 0x008DC73Cu;
    WF(w, esi + FWR_VY, (ext)F(w, esi + FWR_VY) - (ext)F(w, esi + FWR_VY) * (ext)K(w, k));
    k = t9 ? 0x008DC754u : 0x008DC740u;
    WF(w, esi + FWR_VZ, (ext)F(w, esi + FWR_VZ) - (ext)F(w, esi + FWR_VZ) * (ext)K(w, k));
    fade(w, esi, KI(w, t9 ? 0x008DC758u : 0x008DC744u), dt);
    idx = fistp((ext)I(w, esi + FWR_LIFE) * (ext)F(w, esi + FWR_RATE));
    ramp = U(w, esi + FWR_RAMP);
    {
        int64_t at = (int64_t)(ramp >> 2) + (int64_t)idx;      /* dwords    */
        col = (at >= 0 && at < 0x900) ? w->ramp[at] : 0u;
    }
    col &= 0xFFFFFFu;
    if (!col) return 1;                  /* 0x004C0116 / 0x004C0204          */
    col |= U(w, esi + FWR_COL) & 0xFF000000u;
    size = I(w, esi + FWR_SIZE);
    if (t9) {
        uint32_t r = ror(L(w), 16) & 0x7Fu;
        if (!r) size += 0x386A0;
    }
    point(w, in, (ext)F(w, esi + FWR_X), (ext)F(w, esi + FWR_Y), (ext)F(w, esi + FWR_Z),
          col, size);
    return 0;
}

/* ---- the frame --------------------------------------------------------- */

void fireworks_frame(fireworks *w, const fireworks_in *in)
{
    uint32_t esi;
    int32_t n;

    if (!w->have) return;
    w->frame++;                          /* 0x004C10D3, before the module    */
    if (!in->on) return;                 /* 0x004BF7BF: [0x005787E4] == 0    */
    esi = w->head;
    for (n = w->count; n > 0; n--) {
        uint32_t next = U(w, esi + FWR_NEXT), life = U(w, esi + FWR_LIFE);
        int dead = 0, type;

        WU(w, esi + FWR_LIFE, life - (uint32_t)in->dt);
        if (life < (uint32_t)in->dt) dead = 1;           /* SUB: the borrow  */
        else if ((ext)F(w, esi + FWR_Y) < (ext)F(w, esi + FWR_CEIL))
            dead = 1;                                    /* below the eye    */
        else {
            type = I(w, esi + FWR_TYPE);
            if (type == 0) dead = step_rocket(w, in, esi);
            else if (type == 4) dead = step_spark(w, in, esi);
            else if (type == 8) dead = step_burst(w, in, esi, 0);
            else if (type == 9) dead = step_burst(w, in, esi, 1);
            w->by_type[type & 15]++;
        }
        if (dead) { kill(w, esi); w->died++; }
        esi = next;
    }
    if (w->frame & 7u) return;           /* 0x004BF889                       */
    w->whoosh = 1;                       /* 0x004BF899                       */
    spawn_rocket(w, in);
}
