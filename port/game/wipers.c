/* wipers.c - the wipers' motion; wipers.h says
 * what is the original's and what is not.  Every address is retail's. */

#include "wipers.h"
#include "x87geom.h"

#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static float rd_f32(const unsigned char *p)
{
    float f;
    uint32_t u = ral_image_i32(p);
    memcpy(&f, &u, 4);
    return f;
}

static int32_t fix(double v)             /* the port's double back to 16.16 */
{
    return (int32_t)lrint(v * 65536.0);
}

int wipers_load(wipers *w, const ral_image *im, const ro3_model *m,
                int32_t carnum1, char *err, size_t errlen)
{
    static const struct { uint32_t mat, sweep, lo; } T[2] = {
        { 0x0083D094u, 0x0083D1A8u, 0x0083D22Cu },
        { 0x0083D0F0u, 0x0083D14Cu, 0x0083D234u } };
    const unsigned char *p;
    float deg[2], rx[9];
    size_t i;
    int k, c, seeded = w->seeded;
    wipers keep = *w;

    memset(w, 0, sizeof *w);
    c = carnum1 == 6 ? 11 : carnum1;                        /* FUN_00454CEC */
    if (c < 0 || c >= 23 || !m) {
        snprintf(err, errlen, "car %d has no wiper row", (int)carnum1);
        return -1;
    }
    w->car = c;
#define RD(va, n) do { p = ral_image_at(im, (va), (n)); \
        if (!p) { snprintf(err, errlen, "0x%08X is not in the image", \
                           (unsigned)(va)); return -1; } } while (0)
    RD(0x007EC560u, 256 * 4);
    for (i = 0; i < 256; i++) w->table[i] = ral_image_i32(p + 4 * i);
    RD(0x007ECD84u, 8 * 4);
    for (i = 0; i < 8; i++) w->mask[i] = (uint32_t)ral_image_i32(p + 4 * i);
    RD(0x0083D254u, 8);
    w->rate_park = rd_f32(p);
    w->rate_run = rd_f32(p + 4);
    RD(0x0083D278u, 12);
    w->half = rd_f32(p);
    w->half_div = ral_image_i32(p + 4);
    w->step_div = ral_image_i32(p + 8);
    RD(0x007ECD7Cu, 8);                                     /* the image's  */
    w->running = ral_image_i32(p);
    w->next_ms = ral_image_i32(p + 4);
    RD(0x0083D24Cu, 8);
    w->lcg[0] = (uint32_t)ral_image_i32(p);
    w->lcg[1] = (uint32_t)ral_image_i32(p + 4);
    if (seeded) {        /* globals: nothing at the load resets them */
        w->running = keep.running;
        w->next_ms = keep.next_ms;
        w->lcg[0] = keep.lcg[0];
        w->lcg[1] = keep.lcg[1];
        w->acc = keep.acc;
        w->phase = keep.phase;
        memcpy(w->ends, keep.ends, sizeof w->ends);
    }
    w->seeded = 1;
    /* 0x004B0D0A: the tilt, degrees by [0x0085CCB1], of the table entry
     * as 0x004B0B5A leaves it once a process (0x004B0C01: FSUBR, 90 - t) */
    {
        float t, r, k;
        RD(0x007BA1C7u + 4u * (uint32_t)c, 4); t = rd_f32(p);
        RD(0x0085CC9Au, 4);                    k = rd_f32(p);
        RD(0x0085CCB1u, 4);                    r = rd_f32(p);
        t = (float)((ext)k - (ext)t);
        w->tilt = (float)((ext)t * r);
    }
    for (k = 0; k < 2; k++) {
        float r;
        RD(T[k].mat + 4u * (uint32_t)c, 4);
        w->b[k].n = 0;
        deg[k] = 0.0f;
        {
            int32_t mat = ral_image_i32(p) & 0xFF;          /* CMP [EDI+7],AL */
            /* 0x004ACE9C: the model's polygons in order; mat 1 first */
            for (i = 0; i < m->npolys; i++) {
                const ro3_poly *q = &m->polys[i];
                int32_t pm = (int32_t)(((uint32_t)q->flags >> 24) & 0xFFu);
                int32_t j, other;
                if (pm != mat) continue;
                if (k == 1) {               /* 0x004ACEB6: only if not mat 1 */
                    const unsigned char *p0 = ral_image_at(im, T[0].mat + 4u * (uint32_t)c, 4);
                    other = p0 ? (ral_image_i32(p0) & 0xFF) : -1;
                    if (pm == other) continue;
                }
                for (j = 0; j < q->corners; j++) {          /* 0x004AD16E */
                    int32_t v = m->corner_vert[q->first_corner + (size_t)j], e;
                    if (w->b[k].n == WIPER_VERTS) break;
                    for (e = 0; e < w->b[k].n; e++)
                        if (w->b[k].id[e] == v) break;
                    if (e == w->b[k].n) {                   /* 0x004AD198 */
                        w->b[k].group[w->b[k].n] = (int32_t)i;
                        w->b[k].id[w->b[k].n++] = v;
                    }
                }
            }
        }
        RD(T[k].sweep + 4u * (uint32_t)c, 4); deg[k] = rd_f32(p);
        RD(0x0083D23Cu + 4u * (uint32_t)k, 4); r = rd_f32(p);  /* 0x004ACE23 */
        w->b[k].sweep = (float)((ext)deg[k] * r);
        RD(T[k].lo, 4); w->b[k].lo = rd_f32(p);
    }
    /* 0x004ACF97..0x004AD136: the pivot (lowest y, the loop runs down, so
     * the last of equals), the rest less it, then un-tilted */
    rot_x(rx, -(ext)w->tilt);
    for (k = 0; k < 2; k++) {
        wiper_blade *b = &w->b[k];
        int32_t v[WIPER_VERTS][3], lo = 0x7FFFFFFF;
        int e;
        for (e = 0; e < b->n; e++) {
            const ro3_vec *q = &m->points[b->id[e]];
            v[e][0] = fix(q->x); v[e][1] = fix(q->y); v[e][2] = fix(q->z);
        }
        for (e = b->n - 1; e >= 0; e--)
            if (v[e][1] < lo) {
                lo = v[e][1];
                memcpy(b->pivot, v[e], sizeof b->pivot);
            }
        for (e = 0; e < b->n; e++) {
            int32_t d[3];
            d[0] = v[e][0] - b->pivot[0];
            d[1] = v[e][1] - b->pivot[1];
            d[2] = v[e][2] - b->pivot[2];
            xform(b->rest[e], rx, d);
        }
    }
#undef RD
    w->have = w->b[0].n > 0 || w->b[1].n > 0;
    return 0;
}

/* 0x004AE545: the eased angle at step k (and k + 1) */
static float ease(const wipers *w, const wiper_blade *b, int32_t k, int div)
{
    ext h = (ext)(float)(((ext)b->sweep - (ext)b->lo) * w->half);
    ext pi = EXT_C(3.14159265358979323846264338327950288);
    return (float)((-ext_cos(pi / (ext)div * (ext)k) * h + (ext)b->lo) + h);
}

static float blade_angle(const wipers *w, int k, int32_t step, int32_t stroke)
{
    const wiper_blade *b = &w->b[k];
    /* ECX = the angle at step + 1 ([0x0083D27C]), EBX at step
     * ([0x0083D280]); EBX is pushed, and the return stroke takes ECX */
    return stroke ? ease(w, b, step, w->step_div)
                  : ease(w, b, step + 1, w->half_div);
}

static void steps(wipers *w)                         /* 0x004AE4E0 */
{
    int k;
    for (k = 0; k < 2; k++) {
        int32_t t = w->table[w->phase & 0xFF];
        w->stroke[k] = t >= 0 ? 2 : 0;
        w->step[k] = t >= 0 ? t : -t;
    }
}

static void pose_one(const wipers *w, int k, float angle, int32_t out[][3])
{
    const wiper_blade *b = &w->b[k];
    float rx[9], rz[9], mm[9];
    int e;
    rot_x(rx, (ext)w->tilt);                          /* 0x004AD216 */
    rot_z(rz, (ext)angle);
    mat_mul(mm, rx, rz);                              /* 0x004AD261 */
    for (e = 0; e < b->n; e++) {
        int32_t t[3];
        xform(t, mm, b->rest[e]);
        out[e][0] = t[0] + b->pivot[0];               /* FILD, FIADD, FISTP */
        out[e][1] = t[1] + b->pivot[1];
        out[e][2] = t[2] + b->pivot[2];
    }
}

int wipers_pose_blade(const wipers *w, int k, int32_t phase, int32_t out[][3])
{
    int32_t t = w->table[phase & 0xFF];
    if (k < 0 || k > 1) return 0;
    pose_one(w, k, blade_angle(w, k, t >= 0 ? t : -t, t >= 0 ? 2 : 0), out);
    return w->b[k].n;
}

void wipers_update(wipers *w, ro3_model *m, int32_t clock_ms, int32_t dt,
                   int run)
{
    int k, e;
    if (!w->have || !m) return;
    steps(w);
    for (k = 0; k < 2; k++) {                         /* 0x004ACD3F */
        int32_t out[WIPER_VERTS][3];
        pose_one(w, k, blade_angle(w, k, w->step[k], w->stroke[k]), out);
        for (e = 0; e < w->b[k].n; e++) {
            ro3_vec *q = &m->points[w->b[k].id[e]];
            q->x = out[e][0] / 65536.0;
            q->y = out[e][1] / 65536.0;
            q->z = out[e][2] / 65536.0;
        }
    }
    w->poses++;
    if (clock_ms >= w->next_ms) {                     /* 0x004ADBA9 */
        uint32_t b;
        w->lcg[0] = w->lcg[0] * 0xC5u + 0x5Du;
        w->next_ms = (int32_t)(w->lcg[0] & 0xFFu) + clock_ms;
        w->lcg[1] = w->lcg[1] * 0xC5u + 0x5Du;
        b = ((w->lcg[1] >> 16) | (w->lcg[1] << 16)) + (uint32_t)clock_ms;
        b &= w->mask[0];                              /* block +0x2748 = 0 */
        w->running = b != 0;
        if (!w->running) return;
    }
    if (!w->running) return;
    if (!run) {                                       /* block +0x274C == 1 */
        if ((w->phase >> 3) == 0) { steps(w); return; }
        w->acc = fistp((ext)dt * w->rate_park + (ext)w->acc);
    } else {
        w->acc = fistp((ext)dt * w->rate_run + (ext)w->acc);
    }
    w->phase = (int32_t)(((uint32_t)w->acc >> 16) & 0xFFu);
    steps(w);
}

/* ---- the wipe-off's load, 0x004AD93A .. 0x004AE36F ------- */

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* FIST / FISTP under the current rounding mode; NaN or out of range, the
 * integer indefinite (the exceptions are masked) */
static int32_t fist(ext x)
{
    ext r;
    if (x != x) return INT32_MIN;
    r = ext_rint(x);
    if (r < -EXT_C(2147483648.0) || r > 2147483647.0L) return INT32_MIN;
    return (int32_t)r;
}

/* 0x004AD93A: the farthest pair of rest vertices that the same polygon
 * added; both loops run down and `JG` skips only a shorter pair, so the
 * LAST found of equals wins: [0x0082CF04] the inner loop's, [+4] the
 * outer's. */
static void end_points(const wiper_blade *b, int32_t e[2])
{
    int32_t best = 0;
    int i, j, a;
    for (i = b->n - 1; i >= 0; i--)
        for (j = b->n - 1; j >= 0; j--) {
            int32_t d[3], len;
            if (j == i || b->group[j] != b->group[i]) continue;
            for (a = 0; a < 3; a++)
                d[a] = (int32_t)((uint32_t)b->rest[j][a] - (uint32_t)b->rest[i][a]);
            len = fist(ext_sqrt((ext)d[0] * d[0]
                             + ((ext)d[1] * d[1] + (ext)d[2] * d[2])));
            if (best > len) continue;
            best = len;
            e[0] = j;
            e[1] = i;
        }
}

typedef struct { int32_t y0, rows, first; } wscan_tri;

typedef struct {
    int32_t   (*sp)[2];                  /* {x, len}: the 0x80000 heap     */
    int         n, cap;
    wscan_tri   tri[2][2 * (WIPER_POSES - 1)];
    float       kc[3];                   /* [0x0083D25C..64]: 1.0          */
} wscan;

/* 0x004ADF32: one trapezoid from ya to yb, the edges l0 -> l1 and r0 ->
 * r1, sampled at the next row line; a row is {trunc l, trunc(r - x)} */
static int trap(wscan *s, float ya, float yb, float l0, float r0, float l1,
                float r1)
{
    float dy = (float)((ext)yb - ya);
    float sl = (float)(((ext)l1 - l0) / dy);
    float sr = (float)(((ext)r1 - r0) / dy);
    int32_t it = fist(ya);                                  /* FIST, chop */
    float xl = (float)(((ext)s->kc[0] - ((ext)ya - (ext)it)) * sl + l0);
    float xr = (float)(((ext)s->kc[1] - ((ext)ya - (ext)it)) * sr + r0);
    float row = (float)ext_rint((ext)ya);                 /* FRNDINT    */
    for (;;) {
        int32_t a = fist(row), b = fist(yb);
        float l, r;
        row = (float)((ext)row + s->kc[2]);
        if (!(a < b)) return 0;
        l = xl; xl = (float)((ext)xl + sl);
        r = xr; xr = (float)((ext)xr + sr);
        if ((ext)l > (ext)r) { float t = l; l = r; r = t; }
        if (s->n == s->cap) return -1;
        s->sp[s->n][0] = fist(l);
        s->sp[s->n][1] = fist((ext)r - (ext)s->sp[s->n][0]);
        s->n++;
    }
}

/* 0x004ADD6A: a triangle, sorted by y (a bubble sort, C0 alone swaps), cut
 * at the middle vertex; the record is {trunc y0, rows, first span} */
static int scan_tri(wscan *s, wscan_tri *t, float p[3][2])
{
    float xm;
    int i, again;
    do {
        again = 0;
        for (i = 0; i < 2; i++)
            if ((ext)p[i + 1][1] < (ext)p[i][1]) {
                float q[2];
                memcpy(q, p[i], sizeof q);
                memcpy(p[i], p[i + 1], sizeof q);
                memcpy(p[i + 1], q, sizeof q);
                again = 1;
            }
    } while (again);
    xm = (float)(((ext)EXT_C(1.0) / ((ext)p[2][1] - p[0][1]))
                 * ((ext)p[1][1] - p[0][1]) * ((ext)p[2][0] - p[0][0]) + p[0][0]);
    t->first = s->n;
    if (trap(s, p[0][1], p[1][1], p[0][0], p[0][0], p[1][0], xm) != 0 ||
        trap(s, p[1][1], p[2][1], p[1][0], xm, p[2][0], p[2][0]) != 0)
        return -1;
    t->y0 = fist(p[0][1]);
    t->rows = s->n - t->first;
    return 0;
}

/* 0x004AF04F + 0x004AF0B5: the frame of the edge a -> b, its rows scaled
 * to [0x0083D2DC] / [0x0083D2E0]; 0x004AF026 then takes x' = x m0 + y m1 */
static void frame(const float a[2], const float b[2], const float kn[2],
                  float m[4])
{
    ext f;
    m[0] = (float)((ext)b[0] - a[0]);
    m[2] = (float)(-((ext)b[1] - a[1]));
    m[1] = (float)(-(ext)m[2]);
    m[3] = m[0];
    f = (ext)kn[0] / ext_sqrt((ext)m[0] * m[0] + ((ext)m[1] * m[1] + EXT_C(0.0)));
    m[0] = (float)(f * m[0]);
    m[1] = (float)(f * m[1]);
    f = (ext)kn[1] / ext_sqrt((ext)m[2] * m[2] + ((ext)m[3] * m[3] + EXT_C(0.0)));
    m[2] = (float)(f * m[2]);
    m[3] = (float)(f * m[3]);
}

static float frame_x(const float m[4], ext x, ext y)
{
    return (float)(x * m[0] + y * m[1]);
}

/* One half of 0x004AEBEF: the cell in the frame of the edge a -> b, against
 * the far edge u -> w; one draw, and a second that replaces it unless the
 * cell lies short of the far edge - |w - u| taken by the SIGN BIT, as the
 * code tests it */
static int half_draws(int32_t cx, int32_t cy, const float a[2],
                      const float b[2], const float u[2], const float w[2],
                      const float kn[2])
{
    float m[4], ux, wx, x, d;
    ext v;
    frame(a, b, kn, m);
    ux = frame_x(m, (ext)u[0] - a[0], (ext)u[1] - a[1]);
    wx = frame_x(m, (ext)w[0] - a[0], (ext)w[1] - a[1]);
    x = frame_x(m, (ext)cx - a[0], (ext)cy - a[1]);
    d = (float)((ext)wx - ux);
    v = (ext)x - ux;
    if (signbit(d)) { d = -d; v = -v; }
    return (v < (ext)d) ? 1 : 2;
}

int wipers_masks_build(wipers *w, const ral_image *im, wiper_masks *mk,
                       uint32_t *gen, char *err, size_t errlen)
{
    static const uint32_t ebase[2] = { 0x007D7BAAu, 0x007D7BCEu };
    static int32_t pt[2][WIPER_POSES][2][3];
    static float   fl[2][WIPER_POSES][2][2];
    static wscan   s;
    int32_t   tab[12], div[2], flip[2];
    float     lo[2], qm[2], qd[2], kn[2];
    ext       kscale;
    const unsigned char *p;
    int       b, k, j, mode = fegetround(), rc = -1;
    unsigned long draws = 0;

    memset(mk, 0, sizeof *mk);
    memset(&s, 0, sizeof s);
#define RD(va, n) do { p = ral_image_at(im, (va), (n)); \
        if (!p) { snprintf(err, errlen, "0x%08X is not in the image", \
                           (unsigned)(va)); goto out; } } while (0)
    if (!w->have) {
        snprintf(err, errlen, "no blades");
        return -1;
    }
    for (b = 0; b < 2; b++) {
        RD(ebase[b], 4);
        mk->steps[b] = ral_image_i32(p);
        if (mk->steps[b] < 1 || mk->steps[b] >= WIPER_POSES) {
            snprintf(err, errlen, "blade %d: %d steps", b, (int)mk->steps[b]);
            return -1;
        }
        if (w->b[b].n < 2) {    /* the original reads the last load's ends */
            snprintf(err, errlen, "blade %d has %d vertices", b, w->b[b].n);
            return -1;
        }
    }
    RD(0x0083D284u, 4); lo[0] = rd_f32(p);
    RD(0x0083D29Cu, 4); lo[1] = rd_f32(p);
    RD(0x0083D2A4u, 16);
    div[0] = ral_image_i32(p);      div[1] = ral_image_i32(p + 4);
    flip[0] = ral_image_i32(p + 8); flip[1] = ral_image_i32(p + 12);
    RD(0x0083D2B4u, 16);
    qm[0] = rd_f32(p);     qm[1] = rd_f32(p + 4);
    qd[0] = rd_f32(p + 8); qd[1] = rd_f32(p + 12);
    RD(0x0083D25Cu, 12);
    s.kc[0] = rd_f32(p); s.kc[1] = rd_f32(p + 4); s.kc[2] = rd_f32(p + 8);
    RD(0x0083D2DCu, 8);
    kn[0] = rd_f32(p); kn[1] = rd_f32(p + 4);
    RD(0x007EC960u, 256 * 4);
    for (j = 0; j < 256; j++) mk->tri[j] = ral_image_i32(p + 4 * j);
    RD(0x007BC664u, 4);
    mk->cur0 = ral_image_i32(p);
    /* 0x004B0B84: the per-car table, +0xC and +0x24 negated, then each
     * field FISTP(f x 65536 / 1000) - the factor stays on the stack */
    RD(0x0085CC92u, 8);
    kscale = (ext)rd_f32(p) / (ext)rd_f32(p + 4);
    RD(0x007B9D77u + 0x30u * (uint32_t)w->car, 0x30);
    for (j = 0; j < 12; j++) {
        float f = rd_f32(p + 4 * j);
        if (j == 3 || j == 9) f = (float)-(ext)f;
        tab[j] = fist((ext)f * kscale);
    }
#undef RD
    /* 0x004AD93A */
    for (b = 0; b < 2; b++)
        end_points(&w->b[b], w->ends[b]);
    /* 0x004AE742: per step the angle (0x004AE545, its k half), the pose
     * without the pivot (0x004AD20A), the end points +0xB2A7 on y */
    for (b = 0; b < 2; b++) {
        const wiper_blade *bl = &w->b[b];
        ext h = (ext)(float)(((ext)bl->sweep - lo[b]) * w->half);
        for (k = 0; k < WIPER_POSES; k++) {
            ext pi = EXT_C(3.14159265358979323846264338327950288);
            float a = (float)((-ext_cos(pi / (ext)w->step_div * (ext)k) * h
                               + lo[b]) + h);
            float rx[9], rz[9], mm[9];
            rot_x(rx, (ext)w->tilt);
            rot_z(rz, (ext)a);
            mat_mul(mm, rx, rz);
            for (j = 0; j < 2; j++) {
                xform(pt[b][k][j], mm, bl->rest[w->ends[b][j]]);
                pt[b][k][j][1] = (int32_t)((uint32_t)pt[b][k][j][1] + 0xB2A7u);
            }
        }
    }
    for (b = 0; b < 2; b++)
        for (k = 0; k < WIPER_POSES; k++) {
            int32_t (*q)[3] = pt[b][k];
            /* 0x004AE8C8: the inner one first - x and y swap, z stays */
            ext s0 = (ext)q[0][0] * q[0][0] + (ext)q[0][1] * q[0][1];
            ext s1 = (ext)q[1][0] * q[1][0] + (ext)q[1][1] * q[1][1];
            if (!(s1 > s0)) {
                int32_t t;
                t = q[0][0]; q[0][0] = q[1][0]; q[1][0] = t;
                t = q[0][1]; q[0][1] = q[1][1]; q[1][1] = t;
            }
        }
    {
        /* 0x004AE9B6: into the 256 x 128 grid, y up; 0x004AEB3F: to 1/256 */
        float sx = (float)((ext)(int32_t)((uint32_t)tab[9] - (uint32_t)tab[6]) / (ext)div[0]);
        float sy = (float)((ext)(int32_t)((uint32_t)tab[1] - (uint32_t)tab[7]) / (ext)div[1]);
        for (b = 0; b < 2; b++)
            for (k = 0; k < WIPER_POSES; k++)
                for (j = 0; j < 2; j++) {
                    int32_t *q = pt[b][k][j], xi, yi;
                    int32_t x = (int32_t)((uint32_t)q[0] + (uint32_t)w->b[b].pivot[0] - (uint32_t)tab[6]);
                    int32_t y = (int32_t)((uint32_t)q[1] + (uint32_t)w->b[b].pivot[1] - (uint32_t)tab[7]);
                    float X = (float)((ext)x / sx), Y = (float)((ext)y / sy);
                    Y = (float)((ext)flip[b] - Y);
                    yi = fist((ext)Y * qm[0]);
                    xi = fist((ext)X * qm[1]);
                    if (xi < 0) xi = 0;
                    if (xi > 0xFFFF) xi = 0xFFFF;
                    if (yi < 0) yi = 0;
                    if (yi > 0x7FFF) yi = 0x7FFF;
                    fl[b][k][j][0] = (float)((ext)xi / qd[0]);
                    fl[b][k][j][1] = (float)((ext)yi / qd[1]);
                    mk->end[b][k][3 * j]     = fbits(fl[b][k][j][0]);
                    mk->end[b][k][3 * j + 1] = fbits(fl[b][k][j][1]);
                    mk->end[b][k][3 * j + 2] = (uint32_t)q[2];
                }
    }
    /* 0x004AE086 per blade: step k's quad (p0k, p0k+1, p1k+1, p1k) as two
     * triangles, scan-converted under CHOP */
    s.cap = 0x80000 / 8;
    s.sp = (int32_t (*)[2])malloc((size_t)s.cap * sizeof *s.sp);
    if (!s.sp) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    fesetround(FE_TOWARDZERO);
    for (b = 0; b < 2; b++)
        for (k = 0; k < mk->steps[b]; k++) {
            float t1[3][2], t2[3][2];
            memcpy(t1[0], fl[b][k][0], 8);     memcpy(t1[1], fl[b][k + 1][0], 8);
            memcpy(t1[2], fl[b][k + 1][1], 8);
            memcpy(t2[0], fl[b][k + 1][1], 8); memcpy(t2[1], fl[b][k][1], 8);
            memcpy(t2[2], fl[b][k][0], 8);
            if (scan_tri(&s, &s.tri[b][2 * k], t1) != 0 ||
                scan_tri(&s, &s.tri[b][2 * k + 1], t2) != 0) {
                fesetround(mode);
                snprintf(err, errlen, "blade %d step %d: the scan overran", b, k);
                goto out;
            }
        }
    fesetround(mode);
    /* 0x004AE2DF: 0xFF, then each step's spans set to the step */
    for (b = 0; b < 2; b++) {
        uint8_t *m = mk->mask[b];
        memset(m, 0xFF, sizeof mk->mask[b]);
        for (k = 0; k < 2 * mk->steps[b]; k++) {
            const wscan_tri *t = &s.tri[b][k];
            int r;
            for (r = 0; r < t->rows; r++) {
                const int32_t *sp = s.sp[t->first + r];
                int64_t at = ((int64_t)t->y0 + r) * WIPER_MASK_W + sp[0];
                if (sp[1] == 0) continue;
                if (sp[1] < 0 || at < 0 || at + sp[1] > (int64_t)sizeof mk->mask[b]) {
                    mk->clipped += (unsigned long)sp[1];
                    continue;
                }
                memset(m + at, (uint8_t)(k / 2), (size_t)sp[1]);
            }
        }
    }
    /* 0x004AE36F per blade, under CHOP: every cell of a step's spans draws
     * 2 to 4 times (0x004AEBEF; DEC / JNS on the length: len cells) */
    fesetround(FE_TOWARDZERO);
    for (b = 0; b < 2; b++)
        for (k = 0; k < 2 * mk->steps[b]; k++) {
            const wscan_tri *t = &s.tri[b][k];
            const float *p0 = fl[b][k / 2][0], *p1 = fl[b][k / 2][1];
            const float *q0 = fl[b][k / 2 + 1][0], *q1 = fl[b][k / 2 + 1][1];
            int r;
            for (r = 0; r < t->rows; r++) {
                const int32_t *sp = s.sp[t->first + r];
                int32_t cy = t->y0 + r, cx, e;
                for (cx = sp[0], e = sp[1]; --e >= 0; cx++)
                    draws += (unsigned long)(half_draws(cx, cy, p0, p1, q0, q1, kn)
                                             + half_draws(cx, cy, q0, q1, p0, p1, kn));
            }
        }
    fesetround(mode);
    {
        unsigned long i;
        for (i = 0; i < draws; i++)
            *gen = *gen * 0x9Du + 0x47u;
    }
    mk->draws = draws;
    mk->have = 1;
    rc = 0;
out:
    free(s.sp);
    s.sp = NULL;
    return rc;
}

/* ---- the wipe-offs, every frame ------------------------- */

/* The cell of a point on the glass: FISTP each coordinate, the 256 x 128
 * grid of 8-unit cells (0x004B02F0, 0x004AC75B) */
static uint32_t glass_cell(const unsigned char *r, size_t ox, size_t oy)
{
    float x, y;
    uint32_t cx, cy;
    memcpy(&x, r + ox, 4);
    memcpy(&y, r + oy, 4);
    cx = (uint32_t)fistp((ext)x);
    cy = (uint32_t)fistp((ext)y);
    return ((cy >> 3) & 0x7Fu) * 256u + ((cx >> 3) & 0xFFu);
}

static int32_t rd_i32(const unsigned char *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr_i32(unsigned char *p, int32_t v)
{
    memcpy(p, &v, 4);
}

void wipers_snow_wipe(const wiper_masks *mk, const wipers *w, int32_t *cur,
                      void *rec, size_t n, size_t stride, size_t o_alive,
                      size_t o_x, size_t o_y)
{
    int32_t p = *cur, k = (int32_t)((uint32_t)(w->phase - p) & 0xFFu);
    size_t i;
    for (; k > 0; k--, p = (p + 1) & 0xFF) {
        int32_t t = w->table[p & 0xFF];
        uint8_t st = (uint8_t)(t >= 0 ? t : -t);         /* 0x004AE4E0     */
        for (i = 0; i < n; i++) {
            unsigned char *r = (unsigned char *)rec + i * stride;
            uint32_t cell;
            if (rd_i32(r + o_alive) != 1) continue;
            cell = glass_cell(r, o_x, o_y);
            if (mk->mask[0][cell] == st) wr_i32(r + o_alive, 0);
            if (mk->mask[1][cell] == st) wr_i32(r + o_alive, 0);  
        }
    }
    *cur = p;
}

void wipers_rain_wipe(const wiper_masks *mk, int32_t phase, int32_t *cur,
                      uint32_t *evap, void *rec, size_t n, size_t stride,
                      size_t o_alive, size_t o_x, size_t o_y)
{
    int32_t c = *cur, k = mk ? (int32_t)((uint32_t)(phase - c) & 0xFFu) : 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char *r = (unsigned char *)rec + i * stride;
        uint32_t cell = 0, s;
        int32_t j;
        if (rd_i32(r + o_alive) != 1) continue;
        if (mk) cell = glass_cell(r, o_x, o_y);
        *evap = *evap * 0xC5u + 0x5Du;                   /* [0x007D6206]   */
        s = (*evap >> 16) | (*evap << 16);
        if ((s & 0x7FFu) == 0) wr_i32(r + o_alive, 0);
        for (j = k - 1; j >= 0; j--) {
            uint8_t st = (uint8_t)mk->tri[(uint32_t)(c + j) & 0xFFu];
            if (mk->mask[0][cell] == st || mk->mask[1][cell] == st) {
                wr_i32(r + o_alive, 0);
                break;
            }
        }
    }
    if (mk) *cur = (int32_t)((uint32_t)(c + k) & 0xFFu);
}
