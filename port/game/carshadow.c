/* carshadow.c - the cars' shadows; carshadow.h says what this is
 * and is not. */

#include "carshadow.h"

#include "plat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define U 65536.0f

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static float smooth(float a, float b, float x)
{
    float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* ---- the pure half ----------------------------------------------------- */

typedef struct { float mx, ax, mz, az, lean, alpha; } layer_params;

int carshadow_build(carshadow_quad *q, const car_state *car,
                    const sim_collision_points *pts,
                    carshadow_height_fn height, void *ctx,
                    const carshadow_sun *sun, int layer)
{
    /* The layers' shape: the outline's multiplier and margin across and along
     * the car, how much of the sun's lean it takes, and its darkness. */
    static const layer_params LP[CARSHADOW_LAYERS] = {
        { 2.10f, 0.40f, 1.70f, 0.50f, 1.0f, CARSHADOW_PEAK * CARSHADOW_CAST_FRAC },
        { 1.12f, 0.10f, 1.08f, 0.12f, 0.0f, CARSHADOW_PEAK },
    };
    const layer_params *lp = &LP[layer == CARSHADOW_CONTACT ? CARSHADOW_CONTACT : CARSHADOW_CAST];
    const float *m = car->matrix;
    float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
    float fx, fz, rx, rz, l, hx, hz, cx, cz, px, py, pz;
    float lean_x = 0.0f, lean_z = 0.0f, gsum = 0.0f, fade, clear;
    int n = pts->n, i, j, nfail = 0;
    int32_t y16s[CARSHADOW_NV];
    char    got[CARSHADOW_NV];

    if (n < 4) return 0;
    if (n > SIM_GROUND_POINTS) n = SIM_GROUND_POINTS;

    /* The footprint is the model's own outline, from every collision point:
     * the wheels (0..3) and the body points around them. */
    for (i = 0; i < n; i++) {
        if (pts->x[i] < xmin) xmin = pts->x[i];
        if (pts->x[i] > xmax) xmax = pts->x[i];
        if (pts->z[i] < zmin) zmin = pts->z[i];
        if (pts->z[i] > zmax) zmax = pts->z[i];
    }

    /* The car's heading on the ground: the model's z axis (row 2 of the
     * matrix, FUN_0044B1AB's m8 / m10) flattened.  A car on its side or its
     * roof has none worth a shadow. */
    fx = m[8]; fz = m[10];
    l = sqrtf(fx * fx + fz * fz);
    if (l < 0.2f) return 0;
    fx /= l; fz /= l;
    rx = m[0]; rz = m[2];
    l = sqrtf(rx * rx + rz * rz);
    if (l < 0.2f) { rx = fz; rz = -fx; }
    else          { rx /= l; rz /= l; }

    /* The quad is the outline plus a margin for the soft edge (the texture's
     * solid part is the inner ~75%). */
    hx = (xmax - xmin) * 0.5f * lp->mx + lp->ax;
    hz = (zmax - zmin) * 0.5f * lp->mz + lp->az;
    cx = (xmax + xmin) * 0.5f;
    cz = (zmax + zmin) * 0.5f;

    px = (float)car->pos[0] / U;
    py = (float)car->pos[1] / U;
    pz = (float)car->pos[2] / U;

    /* The sun's lean, FUN_00455EA4's own angles: the elevation clamped to
     * 0x10 and run through cot, the azimuth turned by -az - 0x40, both in
     * 1/128 turns.  The shadow falls AWAY from the sun.  The original leaned
     * each wheel point by (its height + 0.7) * cot, which at a low sun throws a
     * hull several metres off the car; this takes the lean at the body's
     * height and caps it, so the shadow stays attached. */
    if (sun && sun->have) {
        const float pi = 3.14159265358979f;
        int el = sun->el < 0x10 ? 0x10 : sun->el;
        float cot = cosf(pi * (float)el / 128.0f) / sinf(pi * (float)el / 128.0f);
        float a = pi * (float)(-sun->az - 0x40) / 128.0f;
        float len = clampf(cot * CARSHADOW_BODY_H, 0.0f, CARSHADOW_LEAN_MAX) * lp->lean;
        lean_x = cosf(a) * len;
        lean_z = sinf(a) * len;
    }

    for (j = 0; j < CARSHADOW_N; j++)
    for (i = 0; i < CARSHADOW_N; i++) {
        int k = j * CARSHADOW_N + i;
        float su = (float)i / (float)(CARSHADOW_N - 1);
        float sv = (float)j / (float)(CARSHADOW_N - 1);
        float a = (su * 2.0f - 1.0f) * hx, b = (sv * 2.0f - 1.0f) * hz;
        float ox = rx * a + fx * b, oz = rz * a + fz * b;
        float on = sqrtf(ox * ox + oz * oz);
        float w = 0.5f;
        float wx, wz;
        int32_t y16 = 0;

        /* The corners on the sun's far side take the whole lean, those on
         * the near side none: the shadow stretches away from the sun
         * instead of sliding off. */
        if (on > 1e-6f && (lean_x != 0.0f || lean_z != 0.0f)) {
            float ln = sqrtf(lean_x * lean_x + lean_z * lean_z);
            w = 0.5f + 0.5f * clampf((ox * lean_x + oz * lean_z) / (on * ln),
                                     -1.0f, 1.0f);
        }
        wx = px + rx * cx + fx * cz + ox + lean_x * w;
        wz = pz + rz * cx + fz * cz + oz + lean_z * w;
        got[k] = (char)(height(ctx, (int32_t)lrintf(wx * U), (int32_t)lrintf(wz * U),
                               &y16) != 0);
        if (!got[k]) nfail++;
        y16s[k] = y16;
        q->p[k][0] = wx * U;
        q->p[k][2] = wz * U;
        q->u[k] = su;
        q->v[k] = sv;
    }

    /* A vertex off the level (a shadow over the edge of the map) takes the
     * mean of those that are on it; a car with none on it has no shadow. */
    if (nfail == CARSHADOW_NV) return 0;
    {
        float sum = 0.0f;
        int k, on = 0;
        for (k = 0; k < CARSHADOW_NV; k++)
            if (got[k]) { sum += (float)y16s[k]; on++; }
        for (k = 0; k < CARSHADOW_NV; k++) {
            if (!got[k]) y16s[k] = (int32_t)lrintf(sum / (float)on);
            q->p[k][1] = (float)y16s[k] + (float)CARSHADOW_LIFT;
            gsum += (float)y16s[k] / U;
        }
    }

    /* Clearance: the wheels' contact height (the origin less the ride, along
     * the car's own up) over the ground under the quad.  ~0 on the road. */
    clear = (py - CARSHADOW_RIDE * m[5]) - gsum / (float)CARSHADOW_NV;
    fade = 1.0f - smooth(CARSHADOW_FADE_LO, CARSHADOW_FADE_HI, clear);
    q->alpha = lp->alpha * fade;
    q->layer = layer == CARSHADOW_CONTACT ? CARSHADOW_CONTACT : CARSHADOW_CAST;
    return q->alpha >= 1.0f / 64.0f;
}

/* ---- the drawing half -------------------------------------------------- */

#define TEX_W 64
#define TEX_H 64
#define TEX_LEVELS 7

static plat_tex g_tex[CARSHADOW_LAYERS];
static int      g_tex_tried;
static carshadow_quad g_list[CARSHADOW_MAX];
static int      g_n;

/* The layers' alpha shapes: the rounded box's half-extents and corner radius
 * (in the quad's normalised -1..1), how much narrower it is at the nose, the
 * edge's feather (signed distance from the box, in the same units: solid up to
 * e0, gone by e1) and the darkest the core's centre is above its rim. */
typedef struct { float bx, bz, rad, taper, e0, e1, rim; } shape_params;
static const shape_params SP[CARSHADOW_LAYERS] = {
    /* cast    */ { 0.66f, 0.72f, 0.45f, 0.10f, -0.05f, 0.38f, 0.70f },
    /* contact */ { 0.88f, 0.90f, 0.26f, 0.16f, -0.02f, 0.12f, 0.85f },
};

float carshadow_shape(int layer, float p, float q)
{
    const shape_params *s = &SP[layer == CARSHADOW_CONTACT ? CARSHADOW_CONTACT : CARSHADOW_CAST];
    float taper = 1.0f - s->taper * clampf(q, 0.0f, 1.0f);
    float ax = fabsf(p) / taper - (s->bx - s->rad);
    float az = fabsf(q) - (s->bz - s->rad);
    float ox = ax > 0.0f ? ax : 0.0f, oz = az > 0.0f ? az : 0.0f;
    float inner = (ax > az ? ax : az);
    float sd = sqrtf(ox * ox + oz * oz) + (inner < 0.0f ? inner : 0.0f) - s->rad;
    float edge = 1.0f - smooth(s->e0, s->e1, sd);
    float core = s->rim + (1.0f - s->rim) * smooth(0.0f, 0.55f, -sd);
    return edge * core;
}

static void make_tex(void)
{
    static uint32_t lv[TEX_LEVELS][TEX_W * TEX_H];
    const uint32_t *lp[TEX_LEVELS];
    int l, x, y, layer;

    g_tex_tried = 1;
    for (layer = 0; layer < CARSHADOW_LAYERS; layer++) {
        for (y = 0; y < TEX_H; y++)
            for (x = 0; x < TEX_W; x++) {
                float p = ((float)x + 0.5f) / TEX_W * 2.0f - 1.0f;
                float q = ((float)y + 0.5f) / TEX_H * 2.0f - 1.0f;
                uint32_t a = (uint32_t)lrintf(clampf(carshadow_shape(layer, p, q), 0.0f, 1.0f) * 255.0f);
                lv[0][y * TEX_W + x] = a << 24;          /* RGBA bytes: R G B = 0, A */
            }
        for (l = 1; l < TEX_LEVELS; l++) {
            int w = TEX_W >> l, h = TEX_H >> l, pw = TEX_W >> (l - 1);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const uint32_t *s = &lv[l - 1][(y * 2) * pw + x * 2];
                    uint32_t a = ((s[0] >> 24) + (s[1] >> 24) + (s[pw] >> 24)
                                  + (s[pw + 1] >> 24) + 2) >> 2;
                    lv[l][y * w + x] = a << 24;
                }
        }
        for (l = 0; l < TEX_LEVELS; l++) lp[l] = lv[l];
        g_tex[layer] = plat_gfx_tex_create(TEX_W, TEX_H, lp, TEX_LEVELS);
        if (g_tex[layer]) plat_gfx_tex_smooth(g_tex[layer], TEX_LEVELS);
    }
}

void carshadow_begin(void) { g_n = 0; }

void carshadow_add(const carshadow_quad *q)
{
    if (g_n < CARSHADOW_MAX) g_list[g_n++] = *q;
}

typedef struct { float x, y, z, u, v; } cv;       /* camera space, 16.16 */

/* A triangle clipped to the near plane: 0, 3 or 4 corners. */
static int clip_near(const cv in[3], cv out[4], float zn)
{
    int i, n = 0;
    for (i = 0; i < 3; i++) {
        const cv *a = &in[i], *b = &in[(i + 1) % 3];
        int ain = a->z >= zn, bin = b->z >= zn;
        if (ain) out[n++] = *a;
        if (ain != bin) {
            float t = (zn - a->z) / (b->z - a->z);
            cv c;
            c.x = a->x + (b->x - a->x) * t;
            c.y = a->y + (b->y - a->y) * t;
            c.z = zn;
            c.u = a->u + (b->u - a->u) * t;
            c.v = a->v + (b->v - a->v) * t;
            out[n++] = c;
        }
    }
    return n;
}

void carshadow_flush(const stage_view *v, const float vr[16], const float vt[16])
{
    static plat_vertex buf[CARSHADOW_MAX * CARSHADOW_TRIS * 6];
    float vm[16];
    float dscale;
    int nv = 0, k, i, j, c, r, lay, split = 0;

    if (g_n == 0) return;
    if (!g_tex_tried) make_tex();
    if (!g_tex[0] || !g_tex[1]) { g_n = 0; return; }

    /* world -> camera, as spray_view_matrix builds it */
    memset(vm, 0, sizeof vm);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) vm[r * 4 + c] = vr[r * 4 + c];
    for (c = 0; c < 3; c++)
        vm[12 + c] = vt[12] * vr[c] + vt[13] * vr[4 + c] + vt[14] * vr[8 + c]
                   + vr[12 + c];
    dscale = stage_depth_scale(v->radius, v->detail);

    /* the cast layer's quads first, then the contact's: two draws, one texture each */
    for (lay = 0; lay < CARSHADOW_LAYERS; lay++) {
    for (k = 0; k < g_n; k++) {
        const carshadow_quad *q = &g_list[k];
        cv cam[CARSHADOW_NV];
        if (q->layer != lay) continue;
        uint32_t dif = (uint32_t)lrintf(clampf(q->alpha, 0.0f, 1.0f) * 255.0f) << 24;

        for (i = 0; i < CARSHADOW_NV; i++) {
            const float *p = q->p[i];
            cam[i].x = p[0] * vm[0] + p[1] * vm[4] + p[2] * vm[8]  + vm[12];
            cam[i].y = p[0] * vm[1] + p[1] * vm[5] + p[2] * vm[9]  + vm[13];
            cam[i].z = p[0] * vm[2] + p[1] * vm[6] + p[2] * vm[10] + vm[14];
            cam[i].u = q->u[i];
            cam[i].v = q->v[i];
        }
        /* each grid cell is two triangles; each is clipped to the near plane
         * and its fan goes out as triangles */
        for (j = 0; j + 1 < CARSHADOW_N; j++)
        for (i = 0; i + 1 < CARSHADOW_N; i++) {
            int a = j * CARSHADOW_N + i, b = a + 1;
            int d = a + CARSHADOW_N, e = d + 1;
            const int tri[2][3] = { { a, b, e }, { a, e, d } };
            int t;
            for (t = 0; t < 2; t++) {
                cv in[3], out[4];
                int no, f;
                in[0] = cam[tri[t][0]]; in[1] = cam[tri[t][1]]; in[2] = cam[tri[t][2]];
                no = clip_near(in, out, 0x4000);        /* a quarter of a unit */
                for (f = 2; f < no; f++) {              /* the fan (0, f-1, f) */
                    int g;
                    for (g = 0; g < 3; g++) {
                        const cv *sv = (g == 0) ? &out[0] : (g == 1 ? &out[f - 1] : &out[f]);
                        float z = sv->z, w = z * dscale;
                        plat_vertex *o = &buf[nv++];
                        o->sx = sv->x * (v->focal / z) + v->centre_x;
                        o->sy = v->centre_y - sv->y * (v->focal / z);
                        o->rhw = w > 0.0f ? 0.01f / w : 0.0f;
                        o->sz = clampf(w, 0.0f, 1.0f);
                        o->diffuse = dif;
                        o->specular = v->fog ? (uint32_t)stage_fog_byte(v->fog, z) << 24 : 0u;
                        o->tu = sv->u;
                        o->tv = sv->v;
                    }
                }
            }
        }
    }
    if (lay == 0) split = nv;
    }
    g_n = 0;
    if (nv == 0) return;

    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);            /* MODULATEALPHA   */
    plat_gfx_set_state(PLAT_RS_COLORKEY, 0u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, v->fog ? 1u : 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    if (split > 0) {
        plat_gfx_tex_bind(g_tex[CARSHADOW_CAST]);
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, buf, (unsigned)split);
    }
    if (nv > split) {
        plat_gfx_tex_bind(g_tex[CARSHADOW_CONTACT]);
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, buf + split, (unsigned)(nv - split));
    }
}

void carshadow_free(void)
{
    int layer;
    for (layer = 0; layer < CARSHADOW_LAYERS; layer++) {
        if (g_tex[layer]) plat_gfx_tex_destroy(g_tex[layer]);
        g_tex[layer] = 0;
    }
    g_tex_tried = 0;
    g_n = 0;
}
