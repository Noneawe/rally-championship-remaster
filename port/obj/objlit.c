/* objlit.c - see objlit.h.
 *
 * Where the original keeps a value on the x87 stack this keeps it in a long
 * double; where it stores one to memory (FSTP dword) this stores a float.
 * The rounding points are the original's, which is the only reason the types
 * below are not all `float`. */
#include "objlit.h"

#include <math.h>
#include <string.h>

/* The x87 stack's type.  On x86 `long double` IS the x87's 80-bit extended,
 * so the rounding points are the original's.  Elsewhere `long double` is no
 * nearer the x87 (aarch64: IEEE 128-bit quad, done in SOFTWARE), only slower:
 * there it is `double`, the hardware's widest (car select
 * drew at 7 fps on the RG40XX-H). */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
typedef long double lit_ld;
#define LIT_C(x)    x##L
#define LIT_RINT    nearbyintl
#define LIT_SQRT    sqrtl
#else
typedef double lit_ld;
#define LIT_C(x)    x
#define LIT_RINT    nearbyint
#define LIT_SQRT    sqrt
#endif

/* ---- the table square root, 0x004201A4 and FUN_0045A06B ----------------- */

static float sq_mant[256];       /* [0x0057C378]: sqrt((i + 256) / 256.0f)    */
static float sq_exp[256];        /* [0x0057C778]: sqrt of a float whose bits
                                  * are e << 23, i.e. 2^(e - 127)              */
static int   sq_ready;

static void sq_init(void)
{
    int i;
    if (sq_ready) return;
    for (i = 0; i < 256; i++) {
        /* FILD i+0x100 / FDIV [0x0057B0B0] (256.0f) / FSQRT / FSTP dword */
        sq_mant[i] = (float)LIT_SQRT((lit_ld)(i + 0x100) / LIT_C(256.0));
    }
    for (i = 0; i < 256; i++) {
        /* SHL EAX,0x17 / FLD dword / FSQRT / FSTP dword */
        uint32_t b = (uint32_t)i << 23;
        float f;
        memcpy(&f, &b, 4);
        sq_exp[i] = (float)LIT_SQRT((lit_ld)f);
    }
    sq_ready = 1;
}

/* 0x0045A089..0x0045A0AB: the bits of the float sum, mantissa's top eight
 * (SHR 15, AND 0xFF) and the exponent (AND 0x7F800000, SHR 23). */
static lit_ld sq_lookup(float x)
{
    uint32_t b;
    memcpy(&b, &x, 4);
    return (lit_ld)sq_mant[(b >> 15) & 0xFFu] * (lit_ld)sq_exp[(b >> 23) & 0xFFu];
}

float objlit_sqrt(float x)
{
    sq_init();
    return (float)sq_lookup(x);
}

/* FUN_0045A06B, the scalar arm ([0x005D4788] is 0 -).  Normalises v in
 * place; a zero length (FCOM against [0x0057B014], 0.0f) leaves it alone. */
static void lit_normalise(float v[3])
{
    lit_ld s, r;
    float sum;
    int i;

    sq_init();
    sum = (float)((lit_ld)v[0] * v[0]
                + ((lit_ld)v[1] * v[1] + (lit_ld)v[2] * v[2]));
    s = sq_lookup(sum);
    if ((float)s == 0.0f) return;                 /* FST [0x0068461C] / FCOM */
    r = LIT_C(1.0) / s;
    for (i = 0; i < 3; i++) {
        uint32_t b;
        v[i] = (float)((lit_ld)v[i] * r);
        memcpy(&b, &v[i], 4);
        if (b == 0x80000000u) v[i] = 0.0f;        /* 0x0045A0FC: -0 -> 0      */
    }
}

/* ---- the setup ----------------------------------------------------------- */

void objlit_ramp(uint32_t lut[256], const int32_t lo[3], const int32_t hi[3])
{
    int32_t i, c;
    for (i = 0; i < 256; i++) {
        uint32_t px = 0;
        for (c = 0; c < 3; c++) {
            /* (hi - lo) * i / 255 + lo, IMUL / IDIV 0xFF, low byte kept */
            int32_t v = (int32_t)(((int64_t)(hi[c] - lo[c]) * i) / 255) + lo[c];
            px = (px << 8) | ((uint32_t)v & 0xFFu);
        }
        lut[i] = px | 0xFF000000u;
    }
}

void objlit_light(float out[3], int32_t yaw, int32_t pitch,
                  const ral_mat4 *view_rot)
{
    ral_mat4 rx, ry, a, b, id;
    ral_obj_rot_x16(&rx, (unsigned)(-(pitch << 8)));      /* 0x00425F42 */
    ral_obj_rot_y16(&ry, (unsigned)(-(yaw << 8)));        /* 0x00425F52 */
    ral_mat4_mul(&a, &rx, &ry);                           /* 0x00425F69 */
    if (!view_rot) {
        memset(&id, 0, sizeof id);
        id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
        view_rot = &id;
    }
    ral_mat4_mul(&b, &a, view_rot);                       /* 0x00425F80 */
    /* FLD1 / FLDZ / FLDZ, then 0x0044B1AB: the row vector (0, 0, 1) */
    out[0] = (float)((lit_ld)b.m[8]  + b.m[12]);
    out[1] = (float)((lit_ld)b.m[9]  + b.m[13]);
    out[2] = (float)((lit_ld)b.m[10] + b.m[14]);
}

void objlit_matrix(ral_mat4 *out, const objlit_place *p)
{
    ral_mat4 ry, rx, t, m;
    float s;
    int i;
    ral_obj_rot_y16(&ry, (unsigned)(-(int32_t)p->yaw));
    ral_obj_rot_x16(&rx, (unsigned)(-(int32_t)p->pitch));
    ral_obj_translate_i(&t, p->x, p->y, p->z);
    ral_mat4_mul(&m, &ry, &rx);
    /* FILD scale / FLD1, FADD ST0 x4 (16.0) / FDIVP / FSTP dword */
    s = (float)((lit_ld)p->scale16 / LIT_C(16.0));
    for (i = 0; i < 16; i++) m.m[i] = (float)((lit_ld)m.m[i] * s);
    ral_mat4_copy_add_t(out, &m, &t);
}

void objlit_stats_init(objlit_stats *st)
{
    memset(st, 0, sizeof *st);
    st->min_sx = st->min_sy = 1e30f;
    st->max_sx = st->max_sy = -1e30f;
}

/* ---- the draw ------------------------------------------------------------ */

typedef struct {
    float    vx, vy, vz, sx, sy;
    float    tu, tv;              /* layer 0: the corner's own */
    float    eu, ev;              /* layer 1: FUN_0044F92E's    */
    uint32_t c0, c1;              /* +0x24 and +0x2C            */
    int      behind;
} litvtx;

#define LIT_MAXV 65535
static plat_vertex lit_buf0[LIT_MAXV], lit_buf1[LIT_MAXV];
static litvtx      lit_tv[RO3_MAX_CORNERS];

/* (a * b) >> 8 per byte, the arm's modulate (0x0044A7C0..0x0044A85B) */
static uint32_t lit_mod(uint32_t c, uint32_t g)
{
    uint32_t r = 0;
    int k;
    for (k = 0; k < 24; k += 8)
        r |= ((((c >> k) & 0xFFu) * ((g >> k) & 0xFFu)) >> 8) << k;
    return r | 0xFF000000u;
}

/* FUN_0044F193 for one corner: n is its view normal ([0x005E0938..40]). */
static uint32_t lit_corner(const objlit_env *e, const float n[3],
                           objlit_stats *st)
{
    int32_t i = 0;
    if (e->diffuse != 0) {
        /* FUN_0044CC5C: x*Lx, y*Ly, z*Lz summed z + (y + x)... in that stack
         * order, times (float)[0x005E7F20], FISTP */
        lit_ld d = (lit_ld)n[2] * e->light[2]
                      + ((lit_ld)n[1] * e->light[1]
                       + (lit_ld)n[0] * e->light[0]);
        i = (int32_t)LIT_RINT(d * (lit_ld)(float)e->diffuse);
    }
    i += e->ambient;
    if (i - e->lo < 0)  { i = e->lo; if (st) st->clamp_lo++; }    /* JNS */
    if (!(i - e->hi < 0)) { if (i > e->hi && st) st->clamp_hi++; i = e->hi; }
    return e->lut[i & 0xFF];
}

/* FUN_0044F92E: the reflection uv of one corner, `n` its view normal and `p`
 * its view position.  `rinv` is [0x0059EF54], the view's inverse rotation. */
void objlit_env_uv(const float n[3], const float p[3], const ral_mat4 *rinv,
                       float *u, float *v)
{
    float N[3], E[3], R[3], Q[3];
    lit_ld d;
    int i;

    E[0] = -p[0]; E[1] = -p[1]; E[2] = -p[2];          /* FCHS, 0x0044F981   */
    lit_normalise(E);                                  /* 0x0044F99F          */
    /* FUN_0044FA3F: d = N.E; N *= d (each stored); R = 2N - E */
    d = (lit_ld)n[2] * E[2] + ((lit_ld)n[1] * E[1] + (lit_ld)n[0] * E[0]);
    for (i = 0; i < 3; i++) N[i] = (float)((lit_ld)n[i] * d);
    for (i = 0; i < 3; i++) R[i] = (float)((lit_ld)N[i] + N[i] - E[i]);
    /* ...through [0x0059EF54] (0x0044FAEB, FUN_0044B1AB) */
    for (i = 0; i < 3; i++)
        Q[i] = (float)((lit_ld)R[0] * rinv->m[0 + i] + (lit_ld)R[1] * rinv->m[4 + i]
                     + (lit_ld)R[2] * rinv->m[8 + i] + rinv->m[12 + i]);
    /* 0x0044F9C7: (x, y + 1.0, z), normalised */
    Q[1] = (float)((lit_ld)Q[1] + LIT_C(1.0));
    lit_normalise(Q);
    /* (x * 0.5 + 0.5) * 65536.0 * 256.0, FISTP: 16.16 texels of a 256 page */
    *u = (float)LIT_RINT(((lit_ld)Q[0] * LIT_C(0.5) + LIT_C(0.5)) * LIT_C(65536.0) * LIT_C(256.0)) / 16777216.0f;
    *v = (float)LIT_RINT(((lit_ld)Q[2] * LIT_C(0.5) + LIT_C(0.5)) * LIT_C(65536.0) * LIT_C(256.0)) / 16777216.0f;
}

static void lit_flush(plat_tex t0, unsigned n0, plat_tex t1, unsigned n1)
{
    /* 0x0044AD28: TEXTUREADDRESS 3 (clamp) when the layer's flag
     * [0x005F1028] / [0x005F102C] is 1, else 1 (wrap).  Both are 0 from boot
     * and only race code writes them (0x004511D0.., 0x00452F56.., 0x004586A0),
     * so the menus wrap - until a race has left 0x004586A0's 1 behind, which
     * the port does not carry over. */
    plat_gfx_set_state(PLAT_RS_TEXADDRESS, 1u);
    if (n0) {
        /* 0x0044A8C3..0x0044A90E: layer 0 */
        plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
        plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_INVSRCALPHA);
        plat_gfx_set_state(PLAT_RS_BLEND, 1u);
        plat_gfx_tex_bind(t0);
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, lit_buf0, n0);
    }
    if (n1) {
        /* 0x0044A940..0x0044A99A: layer 1, ZWRITE off around it, ADDITIVE */
        plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);           /* 0x0044C46C */
        plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
        plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_ONE);
        plat_gfx_set_state(PLAT_RS_BLEND, 1u);
        plat_gfx_tex_bind(t1);
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, lit_buf1, n1);
        plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);           /* 0x0044C47C */
    }
}

void objlit_draw(objscene *os, int type, const ral_mat4 *mv,
                 const stage_view *v, const objlit_env *e, objlit_stats *st)
{
    objscene_type *ty;
    const ral_mat4 *m = mv;
    ral_mat4 rinv;
    unsigned n0 = 0, n1 = 0;
    plat_tex cur = 0;
    int have_cur = 0;
    float dscale;
    size_t pi;

    if (type < 0 || type >= OBJ_TYPES) return;
    ty = &os->type[type];
    if (!ty->have || ty->m.npolys == 0) return;           /* 0x0044FDF2 */

    memset(&rinv, 0, sizeof rinv);                        /* the menus' view */
    rinv.m[0] = rinv.m[5] = rinv.m[10] = rinv.m[15] = 1.0f;
    dscale = stage_depth_scale(v->radius, v->detail);

    for (pi = 0; pi < ty->m.npolys; pi++) {
        const ro3_poly *p = &ty->m.polys[pi];
        int32_t  i, n = p->corners;
        uint32_t sel = (uint32_t)p->selector;
        int      b6 = ro3_sel_a(p), bb = ro3_sel_b(p);
        int      b9 = (int)((sel >> 8) & 0xFFu), ba = (int)((sel >> 16) & 0xFFu);
        uint16_t texid = (uint16_t)ro3_texture_id(p);
        uint32_t g, w = 0;
        int      lit, env;
        plat_tex ptex = 0;
        unsigned k;

        if (n < 3 || n > RO3_MAX_CORNERS) continue;       /* 0x0044FEF1 */

        /* FUN_0044E663: byte +6 == 0x1C draws only inside a level, and then
         * by the car's own part record (not ported: the menus are not a
         * level); any other non-zero +6 draws only when +0x0B equals that
         * part's state */
        if (b6 == 0x1C) {
            if (!e->in_level) { if (st) st->gated++; continue; }
        } else if (b6 != 0) {
            int32_t want = e->part_state ? e->part_state[b6] : 0;
            if (bb != want) { if (st) st->gated++; continue; }
        }
        if (st) st->polys++;

        for (k = 0u; k < ty->ntexid; k++)
            if (ty->texid[k] == texid) { ptex = ty->tex[k]; break; }
        if (st && !ptex) st->tex_none++;

        /* 0x0044EA57: the global colour from byte +0x0A */
        g = ba ? (uint32_t)ba * 0x10101u : 0x00FFFFFFu;

        /* 0x0044EE2F's gate: which polygons are lit at all */
        if (e->context == 4)        lit = 0;
        else if (b6 == 0x1C)        lit = 1;
        else if (b6 == 5)           lit = 0;
        else if ((unsigned)b6 < 10) lit = 1;
        else                        lit = bb == 0;

        /* 0x0044EBC2: [0x005D4230], then 0x0044F862's weight - the same for
         * every corner, because it reads corner 0's fog byte by address */
        env = e->strength != 0 && e->reflect && e->context != 4 && b9 != 0;
        if (env) {
            uint32_t x = (uint32_t)((uint32_t)b9 * e->strength * e->fog0) >> 12;
            w = (int32_t)x < 0 ? 0 : x > 0xFF ? 0xFF : x;
            env = w != 0;                                 /* [0x005D4234]     */
        }

        if ((n0 || n1) && (!have_cur || cur != ptex)) {
            lit_flush(cur, n0, e->tex87, n1);
            n0 = n1 = 0;
        }
        cur = ptex;
        have_cur = 1;

        for (i = 0; i < n; i++) {
            size_t   c  = p->first_corner + (size_t)i;
            int32_t  vi = ty->m.corner_vert[c], ni = ty->m.corner_norm[c];
            litvtx  *o  = &lit_tv[i];
            float    pt[3] = { 0, 0, 0 }, nv[3] = { 0, 0, 0 }, pv[3];
            uint32_t col;

            if (vi >= 0 && (size_t)vi < ty->m.npoints) {
                /* 0x00459F16: FILD - the 16.16 ints the parser divided */
                pt[0] = (float)(ty->m.points[vi].x * RO3_FIXED_ONE);
                pt[1] = (float)(ty->m.points[vi].y * RO3_FIXED_ONE);
                pt[2] = (float)(ty->m.points[vi].z * RO3_FIXED_ONE);
            }
            pv[0] = (float)((lit_ld)pt[0] * m->m[0] + (lit_ld)pt[1] * m->m[4]
                          + (lit_ld)pt[2] * m->m[8] + m->m[12]);
            pv[1] = (float)((lit_ld)pt[0] * m->m[1] + (lit_ld)pt[1] * m->m[5]
                          + (lit_ld)pt[2] * m->m[9] + m->m[13]);
            pv[2] = (float)((lit_ld)pt[0] * m->m[2] + (lit_ld)pt[1] * m->m[6]
                          + (lit_ld)pt[2] * m->m[10] + m->m[14]);
            o->vx = pv[0]; o->vy = pv[1]; o->vz = pv[2];

            /* 0x00459F6F: the normal through the 3x3 - the scale with it.
             * The table RAW: the turn at 0x0042566A moves points only, and the
             * files\b normals are stored already in the turned frame. */
            if (ni >= 0 && (size_t)ni < ty->m.nnormals) {
                float a = (float)ty->m.normals[ni].x, b = (float)ty->m.normals[ni].y,
                      cz = (float)ty->m.normals[ni].z;
                nv[0] = (float)((lit_ld)a * m->m[0] + (lit_ld)b * m->m[4] + (lit_ld)cz * m->m[8]);
                nv[1] = (float)((lit_ld)a * m->m[1] + (lit_ld)b * m->m[5] + (lit_ld)cz * m->m[9]);
                nv[2] = (float)((lit_ld)a * m->m[2] + (lit_ld)b * m->m[6] + (lit_ld)cz * m->m[10]);
            }

            /* 0x00459FBF: the projection, then 0x0044AA76's offset */
            o->behind = !(o->vz >= v->near_z);
            o->sx = o->sy = 0.0f;
            if (!o->behind) {
                lit_ld k2 = (lit_ld)v->focal / o->vz;
                o->sx = (float)((float)((lit_ld)o->vx * k2 + v->centre_x) + e->off_x);
                o->sy = (float)((float)(-(lit_ld)o->vy * k2 + v->centre_y) + e->off_y);
            }
            o->tu = (float)(ty->m.corner_u[c] / RO3_TEXTURE_PAGE);
            o->tv = (float)(ty->m.corner_v[c] / RO3_TEXTURE_PAGE);

            if (lit) { col = lit_corner(e, nv, st); if (st) st->lit++; }
            else     { col = 0x00FFFFFFu; if (st) st->white++; }
            o->c0 = lit_mod(col, g);                                  /* +0x24 */
            o->c1 = (o->c0 & 0x00FFFFFFu) | (w << 24);                /* +0x2C */
            if (env) objlit_env_uv(nv, pv, &rinv, &o->eu, &o->ev);
        }
        if (st && env) st->reflect++;

        /* The fan: (0, i, i+1). */
        for (i = 1; i + 1 < n; i++) {
            const litvtx *t3[3];
            int j;
            t3[0] = &lit_tv[0]; t3[1] = &lit_tv[i]; t3[2] = &lit_tv[i + 1];
            if (st) st->tris++;
            if (t3[0]->behind || t3[1]->behind || t3[2]->behind) {
                if (st) st->behind++;
                continue;
            }
            if (n0 + 3 > LIT_MAXV || n1 + 3 > LIT_MAXV) continue;
            for (j = 0; j < 3; j++) {
                const litvtx *s = t3[j];
                plat_vertex q;
                float sz = s->vz * dscale;
                q.sx = (s->sx + e->bias) * e->scale_x;
                q.sy = (s->sy + e->bias) * e->scale_y;
                q.rhw = sz > 0.0f ? 0.01f / sz : 0.0f;
                if (sz < 0.0f) sz = 0.0f;
                if (sz > 1.0f) sz = 1.0f;
                q.sz = sz;
                q.specular = 0xFF000000u;   /* fog byte 255 (+0x28, +0x30) */
                q.diffuse = s->c0;
                q.tu = s->tu; q.tv = s->tv;
                lit_buf0[n0++] = q;
                if (env) {
                    q.diffuse = s->c1;
                    q.tu = s->eu; q.tv = s->ev;
                    lit_buf1[n1++] = q;
                }
                if (st) {
                    if (s->sx < st->min_sx) st->min_sx = s->sx;
                    if (s->sx > st->max_sx) st->max_sx = s->sx;
                    if (s->sy < st->min_sy) st->min_sy = s->sy;
                    if (s->sy > st->max_sy) st->max_sy = s->sy;
                }
            }
            if (st) st->drawn++;
        }
    }
    if (n0 || n1) lit_flush(cur, n0, e->tex87, n1);
}
