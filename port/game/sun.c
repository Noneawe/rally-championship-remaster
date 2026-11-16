/* sun.c - the level's sun, its polygon test, its flare and glare,
 * 2026-11-04; sun.h has the account. */

#include "sun.h"

#include "plat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LD_YAW = 0x680, LD_AZ = 0x684, LD_EL = 0x688, LD_FOGSUN = 0x65C,
       LD_GLARE = 0x6AC, LD_RGB = 0x18BC };

/* [0x005F4468] 2*pi/256, [0x0057B0D0] 98304.0 (1.5 in 16.16), [0x0057B09C]
 * 4096.0 (the near plane), [0x0057AFE4] 64.0: the constants as the image
 * holds them. */
static const float K_UNIT = 0.024543693f;
static const float K_R    = 98304.0f;
static const float K_NEAR = 4096.0f;
static const float K_QTR  = 64.0f;

/* The flare's 17 sprites, in the order 0x004B4BAF draws them (the counter
 * runs 16 down to 0): the TEXTINFO record, the size in pixels (x 1.5), and
 * where on the line from the sun through the screen centre (t = n / 256 - 1:
 * -1 is the sun itself, +1 its mirror).  The tables are static data at
 * 0x008C0DEA, 0x008C0E72 and 0x008C0EB6 (the last as ints, turned into
 * floats by 0x004B4A74 once). */
static const uint8_t  FL_REC[17]  = { 0x40, 0x41, 0x42, 0x45, 0x43, 0x43, 0x43,
                                      0x44, 0x45, 0x45, 0x46, 0x46, 0x46, 0x47,
                                      0x45, 0x48, 0x49 };
static const uint16_t FL_SIZE[17] = { 0x85, 0xBA, 0x31, 0x50, 0x16, 0x47, 0x26,
                                      0x18, 0x06, 0x08, 0x30, 0x57, 0x16, 0x24,
                                      0x11, 0x7E, 0xFB };
static const uint16_t FL_POS[17]  = { 0x000, 0x009, 0x02B, 0x062, 0x0B4, 0x0BF,
                                      0x0C6, 0x0E7, 0x109, 0x139, 0x151, 0x15C,
                                      0x15E, 0x17A, 0x181, 0x1B2, 0x200 };

void sun_init(sun *s, const ld_file *ld, int have_ld, int option)
{
    memset(s, 0, sizeof *s);
    s->option = option;
    if (!have_ld || !ld) return;
    s->yaw = (int)ld_dw(ld, LD_YAW);
    s->az = (int)ld_dw(ld, LD_AZ);
    s->el = (int)ld_dw(ld, LD_EL);
    s->glare = (int)ld_dw(ld, LD_GLARE);
    s->have = (int)ld_dw(ld, LD_FOGSUN) + s->glare != 0;
    s->glare_rgb = ((ld_dw(ld, LD_RGB) & 0xFFu) << 16)
                 | ((ld_dw(ld, LD_RGB + 4) & 0xFFu) << 8)
                 |  (ld_dw(ld, LD_RGB + 8) & 0xFFu);
}

/* ---- the polygon test: 0x0044E0F3 / 0x0044E162 ------------------------- */

static sun *g_tap_sun;

/* One polygon's edges: every cross product negative, in integers as FISTP
 * makes them (the vertices' x, y rounded to nearest even). */
static int inside_tri(const sun *s, const plat_vertex *a, const plat_vertex *b,
                      const plat_vertex *c)
{
    const plat_vertex *p[4];
    int k;

    p[0] = a; p[1] = b; p[2] = c; p[3] = a;
    for (k = 0; k < 3; k++) {
        int64_t x1 = lrintf(p[k]->sx), y1 = lrintf(p[k]->sy);
        int64_t x2 = lrintf(p[k + 1]->sx), y2 = lrintf(p[k + 1]->sy);
        int64_t cr = ((int64_t)s->ix - x1) * (y2 - y1)
                   - ((int64_t)s->iy - y1) * (x2 - x1);
        if (cr >= 0) return 0;
    }
    return 1;
}

static void tap(plat_prim p, const plat_vertex *v, size_t stride, unsigned nv,
                const uint16_t *idx, unsigned ni)
{
    sun *s = g_tap_sun;
    unsigned n = idx ? ni : nv, i;
    const unsigned char *base = (const unsigned char *)v;
#define V(k) ((const plat_vertex *)(base + stride * (size_t)(idx ? idx[(k)] : (k))))

    if (!s || !s->vis) return;
    if (p == PLAT_PRIM_TRIANGLES) {
        for (i = 0; i + 2 < n && s->vis; i += 3)
            if (inside_tri(s, V(i), V(i + 1), V(i + 2))) { s->vis = 0; s->occluded++; }
    } else if (p == PLAT_PRIM_TRIANGLE_STRIP) {
        for (i = 0; i + 2 < n && s->vis; i++) {
            /* the strip's odd triangles are wound the other way round */
            if (i & 1u ? inside_tri(s, V(i + 1), V(i), V(i + 2))
                       : inside_tri(s, V(i), V(i + 1), V(i + 2)))
            { s->vis = 0; s->occluded++; }
        }
    } else if (p == PLAT_PRIM_TRIANGLE_FAN) {
        for (i = 1; i + 1 < n && s->vis; i++)
            if (inside_tri(s, V(0), V(i), V(i + 1))) { s->vis = 0; s->occluded++; }
    }
#undef V
}

void sun_frame_begin(sun *s)
{
    int a;
    /* 0x0044DBFC, run before this frame's sky: the glare's alpha from the
     * frame before's closeness and level */
    s->glare_alpha = 0;
    if (s->glare) {
        unsigned prod = (unsigned)s->close * (unsigned)s->level;
        a = (int)((((prod >> 4) * (unsigned)s->glare) >> 8) & 0x7FFFFFFFu);
        if (a > 255) a = 255;
        s->glare_alpha = a;
    }
    s->front = s->vis = 0;
    s->close = 0;
    s->occluded = 0;
}

void sun_sky(sun *s, const ral_mat4 *dome, const stage_view *v, int width,
             int height)
{
    double a, e, ca, sa, ce, se, x, y, z, sc, d;
    int32_t vx, vy, vz;
    int yb;

    s->sv = v;
    s->wid = width;
    s->hei = height;
    if (!s->have) return;

    yb = (s->az + s->yaw) & 0xFF;
    a = (-(double)yb + (double)K_QTR) * (double)K_UNIT;
    ca = cos(a); sa = sin(a);
    e = (double)s->el * (double)K_UNIT;
    ce = cos(e); se = sin(e);
    x = (double)(float)(ca * ce * (double)K_R);
    y = (double)(float)(se * (double)K_R);
    z = (double)(float)(ce * sa * (double)K_R);
    /* FUN_0044B1AB: the row vector times the matrix, the translation row
     * added, each result a rounded integer (0x005F4478 / 7C / 80) */
    vx = (int32_t)lrint(x * dome->m[0] + y * dome->m[4] + z * dome->m[8] + dome->m[12]);
    vy = (int32_t)lrint(x * dome->m[1] + y * dome->m[5] + z * dome->m[9] + dome->m[13]);
    vz = (int32_t)lrint(x * dome->m[2] + y * dome->m[6] + z * dome->m[10] + dome->m[14]);
    if ((float)vz < K_NEAR) return;             /* behind: no sun           */

    s->front = 1;
    sc = (double)v->focal / (double)vz;
    s->sx = (float)((double)vx * sc + (double)v->centre_x);
    s->sy = (float)(-((double)vy * sc) + (double)v->centre_y);
    s->ix = (int)lrintf(s->sx);
    s->iy = (int)lrintf(s->sy);
    s->vis = (unsigned)s->ix < (unsigned)width && (unsigned)s->iy < (unsigned)height;
    d = sqrt((double)(s->sy - v->centre_y) * (double)(s->sy - v->centre_y)
           + (double)(s->sx - v->centre_x) * (double)(s->sx - v->centre_x));
    s->close = (int)lrint(-(d * 0.5 - 256.0));
    if (s->close < 0) s->close = 0;

    g_tap_sun = s;
    plat_gfx_set_tap(tap);
}

void sun_world_end(sun *s)
{
    plat_gfx_set_tap(NULL);
    g_tap_sun = NULL;
    s->flag = s->vis;                           /* 0x00453426: car+0xFB8    */
    /* DIAGNOSTIC, not the original's: RALLY_SUN_LOG prints each frame's sun */
    {
        static int log = -1;
        if (log < 0) log = getenv("RALLY_SUN_LOG") != NULL;
        if (log)
            printf("sun: tick %lu front %d at %.1f,%.1f close %d covered %d "
                   "flag %d level %d glare %d\n", s->last_tick, s->front,
                   (double)s->sx, (double)s->sy, s->close, s->occluded, s->flag,
                   s->level, s->glare_alpha);
    }
}

void sun_tick(sun *s, unsigned long tick)
{
    unsigned long n;

    if (!s->have_tick) { s->have_tick = 1; s->last_tick = tick; return; }
    n = tick - s->last_tick;
    s->last_tick = tick;
    if (n > 128ul) n = 128ul;                   /* 0..256 in 64 ticks       */
    while (n--) {
        s->level += s->flag ? 4 : -4;
        if (s->level < 0) s->level = 0;
        if (s->level > 256) s->level = 256;
    }
}

/* ---- the draws ---------------------------------------------------------- */

static void vtx(plat_vertex *p, float x, float y, float z, uint32_t col,
                float u, float v)
{
    p->sx = x; p->sy = y; p->sz = z; p->rhw = 256.0f;
    p->diffuse = col; p->specular = 0u;
    p->tu = u; p->tv = v;
}

void sun_flare_draw(const sun *s, uint32_t atlas, const float *aspect,
                    uint32_t near_bits, float zscale)
{
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    plat_vertex out[17 * 6];
    unsigned n = 0;
    float cx, cy, dx, dy, dist, nearz, z;
    int a255, alpha, i;

    if (!s->have || !s->option || s->level <= 0 || !atlas || !aspect || !s->sv)
        return;
    cx = s->sv->centre_x;
    cy = s->sv->centre_y;
    dx = s->sx - cx;
    dy = -(s->sy - cy);
    dist = sqrtf(dx * dx + dy * dy);
    a255 = (int)lrintf(255.0f - dist * 0.5f);
    if (a255 < 0) a255 = 0;
    alpha = (int)(((unsigned)s->level * (unsigned)a255) >> 8);
    if (alpha <= 0) return;
    memcpy(&nearz, &near_bits, sizeof nearz);
    z = (float)((double)nearz * (double)zscale);

    for (i = 16; i >= 0; i--) {
        float t = (float)FL_POS[i] / 256.0f - 1.0f;
        float px = -dx * t + cx, py = dy * t + cy;
        float size = (float)lrintf((float)FL_SIZE[i] * 1.5f);
        const float *e = aspect + (size_t)FL_REC[i] * 9u;
        float w = size, h = size * e[0], ox = 0.5f * w, oy = 0.5f * h;
        float qx[4] = { -ox, w - ox, w - ox, -ox };
        float qy[4] = { -oy, -oy, h - oy, h - oy };
        plat_vertex q[4];
        int k;
        for (k = 0; k < 4; k++)
            vtx(&q[k], qx[k] + px, qy[k] + py, z, ((uint32_t)alpha << 24) | 0xFFFFFFu,
                e[1 + 2 * k], e[2 + 2 * k]);
        for (k = 0; k < 6; k++) out[n++] = q[tri[k]];
    }
    /* 0x004C1201's DESTBLEND ONE, the spray's atlas, the frame's states
     * as the fireworks draw them (fireworks_draw.c) */
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_ONE);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_tex_bind(atlas);
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    plat_gfx_draw(PLAT_PRIM_TRIANGLES, out, n);
}

void sun_glare_draw(const sun *s, int width, int height)
{
    plat_vertex q[6];
    uint32_t col;
    float w = (float)width, h = (float)height;
    int k;
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    plat_vertex c[4];

    if (!s->have || s->glare_alpha <= 0) return;
    col = ((uint32_t)s->glare_alpha << 24) | (s->glare_rgb & 0xFFFFFFu);
    vtx(&c[0], 0.0f, 0.0f, 0.0f, col, 0.0f, 0.0f);
    vtx(&c[1], w, 0.0f, 0.0f, col, 0.0f, 0.0f);
    vtx(&c[2], w, h, 0.0f, col, 0.0f, 0.0f);
    vtx(&c[3], 0.0f, h, 0.0f, col, 0.0f, 0.0f);
    for (k = 0; k < 6; k++) q[k] = c[tri[k]];
    /* a flat polygon, alpha blended, over everything the scene drew */
    plat_gfx_set_state(PLAT_RS_ZTEST, 0u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_tex_bind(0);
    plat_gfx_draw(PLAT_PRIM_TRIANGLES, q, 6);
}
