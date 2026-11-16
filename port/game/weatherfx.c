/* weatherfx.c - what the render path draws for rain and snow,
 * weatherfx.h says what is here and what is not;
 * is the reading, function by function.
 *
 * THE ARITHMETIC.  The engine is hand-written x87 run under Direct3D's FPU
 * setting (24-bit precision, round to nearest), so every operation rounds to
 * float and every FISTP rounds to nearest even: C's float arithmetic and
 * lrintf.  Where a function sets the control word to CHOP itself (0x004AC1F6,
 * 0x004AFC18, 0x004B0133) the conversions truncate, and say so.  An integer
 * compare of a float's BITS (the engine's `cmp dword ptr [x], imm` on a
 * float) is `bits()`, kept as the compare it is. */

#include "weatherfx.h"

#include "image.h"
#include "pcx.h"
#include "plate.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* rally.c declares these the same way: port-side, not in plat.h */
int      plat_gfx_clear_depth(void);
plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h);
int      plat_gfx_frame_copy(plat_tex t, int w, int h);

/* ---- helpers ------------------------------------------------------------ */

static int32_t fistp(float v)          /* FISTP, round to nearest even      */
{
    return (int32_t)lrintf(v);
}

static int32_t chop(float v)           /* FISTP under the CHOP control word */
{
    return (int32_t)v;
}

static int32_t bits(float v)
{
    int32_t b;
    memcpy(&b, &v, 4);
    return b;
}

static uint32_t ror32(uint32_t v, unsigned n)
{
    return (v >> n) | (v << (32u - n));
}

/* [0x006CF334]: s = s * 0x9D + 0x47, the engine's shared generator */
static uint32_t lcg9d(uint32_t *s)
{
    *s = *s * 0x9Du + 0x47u;
    return *s;
}

/* the screen modules' own: s = s * 0xC5 + 0x5D */
static uint32_t lcgc5(uint32_t *s)
{
    *s = *s * 0xC5u + 0x5Du;
    return *s;
}

static void norm3(float v[3])          /* 0x00462147: v *= 1 / |v|          */
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float q = 1.0f / l;
    v[0] *= q; v[1] *= q; v[2] *= q;
}

/* 0x004621AE (esi, edi, ebp): ebp = esi x edi */
static void cross3(float o[3], const float s[3], const float e[3])
{
    float x = s[1] * e[2] - s[2] * e[1];
    float y = s[2] * e[0] - s[0] * e[2];
    float z = s[0] * e[1] - s[1] * e[0];
    o[0] = x; o[1] = y; o[2] = z;
}

static void vtx(plat_vertex *p, float x, float y, float z, float rhw,
                uint32_t col, uint32_t spec, float u, float v)
{
    p->sx = x; p->sy = y; p->sz = z; p->rhw = rhw;
    p->diffuse = col; p->specular = spec;
    p->tu = u; p->tv = v;
}

static float far_inv(const stage_view *v)  /* 1 / [0x005D705C]              */
{
    return stage_depth_scale(v->radius, v->detail);
}

/* ---- load ---------------------------------------------------------------- */

static int rd_i32(const ral_image *im, uint32_t va, int32_t *out)
{
    const unsigned char *p = ral_image_at(im, va, 4);
    if (!p) return -1;
    *out = ral_image_i32(p);
    return 0;
}

static int rd_f32(const ral_image *im, uint32_t va, float *out)
{
    const unsigned char *p = ral_image_at(im, va, 4);
    if (!p) return -1;
    *out = ral_image_f32(p);
    return 0;
}

int wfx_load(wfx *w, const struct ral_image *im, const char *files_root,
             const void *grade, int model, int video, char *err,
             size_t errlen)
{
    int c, k, bad = 0;

    (void)model;
    memset(w, 0, sizeof *w);
    /* 0x0045452C..0x0045459F: the windscreen's two points per car */
    for (c = 0; c < WFX_CARS; c++)
        for (k = 0; k < 6; k++)
            bad |= rd_i32(im, 0x0057E144u + 0x12Cu * (uint32_t)c + 4u * (uint32_t)k,
                          &w->wscreen[c][k]);
    bad |= rd_i32(im, 0x005C6CF8u, &w->yoff);
    for (c = 0; c < WFX_CARS; c++) {
        bad |= rd_f32(im, 0x0070D5B0u + 4u * (uint32_t)c, &w->zbias[c]);
        bad |= rd_f32(im, 0x007BA1C7u + 4u * (uint32_t)c, &w->tilt_deg[c]);
    }
    for (k = 0; k < 8; k++)
        bad |= rd_f32(im, 0x007BCB12u + 4u * (uint32_t)k, &w->drop_rad[k]);
    bad |= rd_f32(im, 0x0085CCB1u, &w->deg2rad);
    if (bad) {
        snprintf(err, errlen, "the weather's tables are not in the image");
        return -1;
    }
    w->have_tables = 1;
    if (!video)
        return 0;

    /* Texture id 0xCA, [0x00586B58]: setplatesall_q's plate 900 (files\m,
     * the band FUN_00424672 gives 900..901), its alpha the forced id+1 as
     * the other hard-wired plates'.  Graded: 900 is in neither exemption. */
    {
        plate *p = (plate *)malloc(sizeof *p);
        if (!p) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        if (plate_load(p, files_root, 900u, PLATE_ALPHA_FORCED,
                       (const plate_grade *)grade, err, errlen) != 0) {
            free(p);
            return -1;
        }
        w->atlas = plate_upload(p);
        free(p);
    }
    return 0;
}

/* 0x004B0C61..0x004B0D06: files\m\edgeNN.pcx at 256 x 128, each cell
 * ~(b0|b1|b2|b3) of the decoded texel - byte 3 is 0 (the decoder zeroes it,
 * so white glass is 0 (kept) and black 0xFF
 * (a drop or a flake there dies).  The files are 256 x 127: row 127 is
 * never written by the decode and holds what the buffer held - THE PORT
 * MAKES IT 0xFF, the border the rows above it already are. */
int wfx_load_edge(wfx *w, const char *path, char *err, size_t errlen);
int wfx_load_edge(wfx *w, const char *path, char *err, size_t errlen)
{
    FILE *f;
    long n;
    unsigned char *buf;
    uint32_t *px;
    unsigned dw = 0, dh = 0, x, y;
    int rc;

    memset(w->edge, 0xFF, sizeof w->edge);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: cannot open", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)(n > 0 ? n : 1));
    px = (uint32_t *)malloc(256u * 128u * 4u);
    if (!buf || !px || n <= 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        free(px);
        snprintf(err, errlen, "%s: cannot read", path);
        return -1;
    }
    fclose(f);
    rc = pcx_decode_request(buf, (size_t)n, px, 256u * 128u, 256u, 128u,
                            &dw, &dh, err, errlen);
    free(buf);
    if (rc != 0) {
        free(px);
        return -1;
    }
    for (y = 0; y < dh && y < 128u; y++)
        for (x = 0; x < dw && x < 256u; x++) {
            uint32_t t = px[y * dw + x];
            uint8_t  o = (uint8_t)((t & 0xFFu) | ((t >> 8) & 0xFFu)
                                   | ((t >> 16) & 0xFFu));
            w->edge[y * 256u + x] = (uint8_t)~o;
        }
    free(px);
    return 0;
}

/* ---- the race's start, 0x004C0F44 ---------------------------------------- */

static void rain3d_init(wfx *w)          /* 0x004B1287 */
{
    int i;
    for (i = WFX_RAIN3D_N - 1; i >= 0; i--) {
        uint32_t s = lcg9d(&w->lcg);
        w->r_x[i] = (int32_t)(s & 0x3FFFFu);
        w->r_y[i] = (int32_t)(ror32(s, 10) & 0x3FFFFu);
        w->r_z[i] = (int32_t)(ror32(s, 20) & 0x3FFFFu);
    }
    w->r_primed = 0;
}

/* [0x0087ACE4]: the walk's carry accumulator - never reset, so the table
 * differs from one stage load to the next in the original too. */
static uint32_t s_carry;

static void snow3d_init(wfx *w)          /* 0x004B1E55 */
{
    static int32_t t2[4096];
    int32_t acc = 0, lo = -4096, hi = 4096, step = 2, slope = 0;
    int i, j, pass;

    w->lcg = 0x07FFFFFFu;
    for (i = 0; i < 4096; i++) {
        int32_t e;
        uint32_t s, old;
        w->s_t[i] = fistp((float)(int32_t)((uint32_t)acc << 4) * 8.5f);
        e = slope + step;
        s = lcg9d(&w->lcg);
        old = s_carry;
        s_carry += s;
        if (s_carry < old)                /* ADD sets CF: JAE skips          */
            e -= 2 * step;
        if (e < lo) e = lo;
        if (e > hi) e = hi;
        slope = e;
        acc += e;
        acc = fistp((float)acc * 0.9f);
    }
    for (pass = 0; pass < 160; pass++) {  /* push 0xA0 / dec / jns          */
        memcpy(t2, w->s_t, sizeof t2);
        for (j = 0; j < 0xFFE; j++) {
            if (j == 0 || j == 0xFFF) {
                w->s_t[j] = t2[j];
                continue;
            }
            {
                float a = (float)t2[j - 1] * 0.3f;
                float b = (float)t2[j] * 0.4f;
                float c = (float)t2[j + 1] * 0.3f;
                w->s_t[j] = fistp(a + (b + c));
            }
        }
    }
    /* 0x004B1FF3..0x004B2041: a detrend of the COPY, never read again */
    for (i = WFX_SNOW3D_N - 1; i >= 0; i--) {
        uint32_t s = lcg9d(&w->lcg);
        w->s_x[i] = (int32_t)(s & 0xFFFFFu);
        w->s_y[i] = (int32_t)(ror32(s, 10) & 0xFFFFFu);
        w->s_z[i] = (int32_t)(ror32(s, 20) & 0xFFFFFu);
        w->s_u[i] = (float)(int32_t)(ror32(s, 30) & 7u) * 0.0625f
                  + 0.001953125f;
    }
    for (i = WFX_SNOW3D_N - 1; i >= 0; i--) {
        w->s_ox[i] = (int32_t)(lcg9d(&w->lcg) % 3072u);   /* XOR EDX / IDIV */
        w->s_oz[i] = (int32_t)(lcg9d(&w->lcg) % 3072u);
    }
}

/* 0x004AA663 (its every-call half 0x004AA6D3): the drop pool cleared, the
 * curvature table and the eight hexagons.  The cursor is not reset. */
static void screen_rain_init(wfx *w)
{
    int i, j, s, k;

    for (i = 0; i < WFX_DROPS_N; i++)
        w->drop[i].active = 0;
    for (i = 0, j = 0; i <= 0x7F8; i += 8, j++) {
        float t = ((float)i - 1024.0f) * 0.00054414064f;
        float v = 1.0f - sqrtf(1.0f - t * t);
        if (i < 0x400) v = -v;
        w->curve[j] = v;
    }
    for (s = 0; s < 8; s++) {
        float a = 0.0f;
        float st = 3.14159265358979f / 6.0f * 2.0f; /* FLDPI / FIDIV 6 / FMUL 2.0 */
        for (k = 0; k < 6; k++) {
            w->hex[s][k][0] = sinf(a) * w->drop_rad[s];
            w->hex[s][k][1] = cosf(a) * w->drop_rad[s];
            a += st;
        }
    }
    w->acc_first = w->acc_second = 0;
}

void wfx_race_start(wfx *w, int model)
{
    int i;

    if (model == 6) model = 11;            /* FUN_00454CEC                    */
    if (model < 0) model = 0;
    if (model >= WFX_CARS) model = WFX_CARS - 1;
    w->model = model;
    w->lcg = (uint32_t)model;              /* 0x00454E8B: EAX, the model      */
    {   /* 0x004B0E39's wiper load, its flow field's draws */
        unsigned long i;
        for (i = 0; i < w->wipe_draws; i++)
            w->lcg = w->lcg * 0x9Du + 0x47u;
    }
    rain3d_init(w);
    snow3d_init(w);
    screen_rain_init(w);
    for (i = 0; i < WFX_FLAKES_N; i++)     /* 0x004AF146                      */
        w->flake[i].alive = 0;
    w->last_ms = 0;
    w->dt = -1;                            /* no clock yet: the first tick 0  */
    w->copy_valid = 0;
}

/* ---- the engine's clock, 0x004C27F0 -------------------------------------- */

void wfx_engine_tick(wfx *w, const wfx_frame *f)
{
    int32_t d = (int32_t)(f->now_ms - w->last_ms);
    if (w->dt < 0 || !f->advance) d = 0;
    w->dt = d;
    w->last_ms = f->now_ms;
}

/* ---- the copy, 0x00453C27 / 0x004590B5 ------------------------------------ */

void wfx_scene_copy(wfx *w, const wfx_frame *f)
{
    if (!f->inside || f->screen_w <= 0 || f->screen_h <= 0)
        return;
    if (!w->copy[0]) {
        unsigned tw = 1, th = 1;
        while (tw < (unsigned)f->screen_w) tw <<= 1;
        while (th < (unsigned)f->screen_h) th <<= 1;
        w->copy[0] = plat_gfx_frame_tex_create(tw, th);
        w->copy[1] = plat_gfx_frame_tex_create(tw, th);
        w->copy_w = tw;
        w->copy_h = th;
        if (!w->copy[0] || !w->copy[1])
            return;
    }
    plat_gfx_frame_copy(w->copy[w->copy_cur], f->screen_w, f->screen_h);
    w->copy_valid |= 2;                    /* this frame's: valid next frame */
}

void wfx_frame_end(wfx *w)
{
    if (w->copy_valid & 2) {
        w->copy_cur ^= 1;
        w->copy_valid = 1;
    }
}

/* ---- quad batching ----------------------------------------------------------
 * The engine drew each drop and flake as its own fan; here a quad is a
 * 4-vertex fan {0,1,2,3} = triangles (0,1,2) (0,2,3), appended to one buffer
 * and flushed as one indexed draw.  Same vertices, same order, same blend:
 * 2048 falling flakes were 2048 GL draws (and uploads) a frame.  Callers
 * flush before changing state; a quad batch is never held across a call. */
#define QB_MAX 512
static plat_vertex qb_v[QB_MAX * 4];
static uint16_t    qb_i[QB_MAX * 6];
static int         qb_n;

static void qb_flush(void)
{
    if (qb_n) {
        int k;
        if (qb_i[1] == 0)
            for (k = 0; k < QB_MAX; k++) {
                uint16_t b = (uint16_t)(k * 4);
                qb_i[k * 6 + 0] = b;     qb_i[k * 6 + 1] = (uint16_t)(b + 1);
                qb_i[k * 6 + 2] = (uint16_t)(b + 2); qb_i[k * 6 + 3] = b;
                qb_i[k * 6 + 4] = (uint16_t)(b + 2);
                qb_i[k * 6 + 5] = (uint16_t)(b + 3);
            }
        plat_gfx_draw_indexed(PLAT_PRIM_TRIANGLES, qb_v, (unsigned)qb_n * 4u,
                              qb_i, (unsigned)qb_n * 6u);
    }
    qb_n = 0;
}

static void qb_add(const plat_vertex q4[4])
{
    memcpy(&qb_v[qb_n * 4], q4, 4 * sizeof *q4);
    if (++qb_n == QB_MAX)
        qb_flush();
}

/* ---- rain3d, 0x004B12E9 ---------------------------------------------------- */

static void rain3d_wind(wfx *w, uint32_t clk)     /* 0x004B17A8 */
{
    if ((uint32_t)(clk - w->r_gust_t) >= w->r_gust_p) {
        uint32_t s = lcg9d(&w->lcg);
        w->r_tgt_x = (float)((int32_t)(s & 0xFFFu) - 0x7FF) * 10.0f;
        w->r_tgt_z = (float)((int32_t)((s >> 12) & 0xFFFu) - 0x7FF) * 10.0f;
        w->r_gust_p = (((s >> 24) & 0xFFu) + 0x40u) * 5u;
        w->r_gust_t = clk;
        s = lcg9d(&w->lcg);
        w->r_jit = (int32_t)((s & 0xFFFFu) << 8);
    }
    w->r_wind_x += (w->r_tgt_x - w->r_wind_x) * 0.05f;
    w->r_wind_z += (w->r_tgt_z - w->r_wind_z) * 0.05f;
    w->r_off[0] = fistp((float)w->r_off[0] + w->r_wind_x);
    w->r_off[2] = fistp((float)w->r_off[2] + w->r_wind_z);
}

/* the camera's 3 x 3, block +0x28..+0x48: [0x0059EF94]'s rows, column 3 out */
static void cam3(float m[9], const float *vr)
{
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            m[r * 3 + c] = vr[r * 4 + c];
}

static void rain3d_drop(const wfx *w, const wfx_frame *f, const float m[9],
                        float x, float y, float z, const float vel[3])
{
    const stage_view *v = f->v;
    float hx, hy, hz, qx, qy, qz, ax, ay, az;
    float fa, fh, Ax, Ay, wA, Hx, Hy, wH, dx, dyn, L, q, nx, ny, sz;
    const float rhw = (float)(int32_t)0x43800000;  /* FILD of a float's bits */
    plat_vertex q4[4];

    /* 0x004B19CC: camera space = P * M + (0, 0, 0x20000) */
    hx = x * m[0] + y * m[3] + z * m[6];
    hy = x * m[1] + y * m[4] + z * m[7];
    hz = x * m[2] + y * m[5] + z * m[8] + 131072.0f;
    qx = x - vel[0]; qy = y - vel[1]; qz = z - vel[2];
    ax = qx * m[0] + qy * m[3] + qz * m[6];
    ay = qx * m[1] + qy * m[4] + qz * m[7];
    az = qx * m[2] + qy * m[5] + qz * m[8] + 131072.0f;
    if (bits(hz) <= 0x20000) return;               /* the head in front     */
    if (!w->r_primed) return;
    if (bits(az) <= 0x48000000) return;             /* the tail past 2 m     */
    fa = v->focal / az;
    fh = v->focal / hz;
    Ax = ax * fa + v->centre_x;  Ay = v->centre_y - ay * fa;  wA = fa * 500.0f;
    Hx = hx * fh + v->centre_x;  Hy = v->centre_y - hy * fh;  wH = fh * 500.0f;
    dx = Hx - Ax;
    dyn = -(Hy - Ay);
    L = sqrtf(dx * dx + dyn * dyn);
    q = 1.0f / L;
    nx = dyn * q;
    ny = dx * q;
    sz = az * far_inv(v);
    vtx(&q4[0], Ax, Ay, sz, rhw, 0x00FFFFFFu, 0, 0, 0);
    vtx(&q4[1], Ax - nx * wA, Ay - ny * wA, sz, rhw, 0x00000000u, 0, 0, 0);
    vtx(&q4[2], Hx - nx * wH, Hy - ny * wH, sz, rhw, 0x28000000u, 0, 0, 0);
    vtx(&q4[3], Hx, Hy, sz, rhw, 0x28FFFFFFu, 0, 0, 0);
    /* 0x004A8619: the split-view clip, which accepts in a single view */
    qb_add(q4);
}

static void rain3d(wfx *w, const wfx_frame *f)
{
    float m[9], vel[3], L;
    int32_t n, r[3], negcam[3];
    int i;

    n = f->rain * 2;                               /* block +0x2764          */
    if (n == 0) {
        w->r_primed = 0;
        return;
    }
    if (n > 0x100) n = 0x100;
    cam3(m, f->view_rot);
    negcam[0] = -f->cam[0]; negcam[1] = -f->cam[1]; negcam[2] = -f->cam[2];
    /* 0x004B18C7 */
    r[0] = fistp(131072.0f * m[2]);
    r[1] = fistp(131072.0f * m[5]);
    r[2] = fistp(131072.0f * m[8]);
    w->r_off[0] = negcam[0] - r[0];
    w->r_off[1] = negcam[1] - r[1];
    w->r_off[2] = fistp(131072.0f - (float)r[2] + (float)negcam[2]);

    /* 0x0044C48C and the module's states: no depth at all, untextured */
    plat_gfx_set_state(PLAT_RS_ZTEST, 0u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_tex_bind(0);

    if (f->advance)
        rain3d_wind(w, f->now_ms);
    if (f->advance) {                              /* block +0x0C || +0x10   */
        w->r_vel[0] = (float)(w->r_off[0] - w->r_prev[0]) * 30.0f;
        w->r_vel[1] = (float)(w->r_off[1] + w->r_fall - w->r_prev[1]) * 30.0f;
        w->r_vel[2] = (float)(w->r_off[2] - w->r_prev[2]) * 30.0f;
    }
    vel[0] = w->r_vel[0]; vel[1] = w->r_vel[1]; vel[2] = w->r_vel[2];
    L = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (!(L < 100000.0f)) {
        float s = 1.0f / L;
        vel[0] = vel[0] * s * 100000.0f;
        vel[1] = vel[1] * s * 100000.0f;
        vel[2] = vel[2] * s * 100000.0f;
    }
    w->r_prev[0] = w->r_off[0];
    w->r_prev[1] = fistp((float)w->r_off[1] + (float)w->r_fall);
    w->r_prev[2] = w->r_off[2];
    if (f->advance)
        w->r_fall = fistp((float)(int32_t)(f->now_ms & 0xFFFFFu) * -500.0f);
    for (i = n - 1; i >= 0; i--) {
        int32_t x = (int32_t)(((uint32_t)(w->r_x[i] + w->r_off[0] + w->r_jit))
                              & 0x3FFFFu) - 0x1FFFF;
        int32_t y = (int32_t)(((uint32_t)(w->r_y[i] + w->r_fall + w->r_off[1]))
                              & 0x3FFFFu) - 0x1FFFF;
        int32_t z = (int32_t)(((uint32_t)(w->r_z[i] + w->r_off[2] + w->r_jit))
                              & 0x3FFFFu) - 0x1FFFF;
        rain3d_drop(w, f, m, (float)x, (float)y, (float)z, vel);
    }
    qb_flush();
    w->r_primed = 1;
}

/* ---- snow3d, 0x004B2204 ---------------------------------------------------- */

static void snow3d_lights(wfx *w, uint32_t tint)  /* 0x004B340C */
{
    float ir = (float)(255 - (int32_t)((tint >> 16) & 0xFFu));
    float ig = (float)(255 - (int32_t)((tint >> 8) & 0xFFu));
    float ib = (float)(255 - (int32_t)(tint & 0xFFu));
    float t = 0.0f;
    int i;
    for (i = 0; i < 17; i++) {
        uint32_t r = (uint32_t)fistp(ir * t) & 0xFFu;
        uint32_t g = (uint32_t)fistp(ig * t) & 0xFFu;
        uint32_t b = (uint32_t)fistp(ib * t) & 0xFFu;
        w->s_light[i] = (0xFF000000u | r << 16 | g << 8 | b) + tint;
        t += 0.0625f;
    }
}

static void snow3d(wfx *w, const wfx_frame *f)
{
    const stage_view *v = f->v;
    float m[9], lp[3], ld[3], finv;
    int32_t n, r[3], negcam[3];
    uint32_t col;
    int i;

    /* Part A, every frame: the headlights - a point 2 m ahead of the car
     * and its forward axis, in camera space (block +0x50 picks them up) */
    {
        const float *mv = f->car_mv;
        lp[0] = 131072.0f * mv[8] + mv[12];
        lp[1] = 131072.0f * mv[9] + mv[13];
        lp[2] = 131072.0f * mv[10] + mv[14];
        ld[0] = mv[8]; ld[1] = mv[9]; ld[2] = mv[10];
    }
    snow3d_lights(w, f->tint);                      /* Part B, 0x004B340C   */

    n = f->snow * 8;                                /* block +0x2760        */
    if (n == 0)
        return;
    if (n > 0x800) n = 0x800;
    cam3(m, f->view_rot);
    negcam[0] = -f->cam[0]; negcam[1] = -f->cam[1]; negcam[2] = -f->cam[2];
    /* 0x004B26C2 */
    r[0] = fistp(524288.0f * m[2]);
    r[1] = fistp(524288.0f * m[5]);
    r[2] = fistp(524288.0f * m[8]);
    w->s_off[0] = negcam[0] - r[0];
    w->s_off[1] = negcam[1] - r[1];
    w->s_off[2] = fistp(524288.0f - (float)r[2] + (float)negcam[2]);

    plat_gfx_tex_bind(w->atlas);
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);          /* 0x0044C4BA           */
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);         /* 0x0044C46C           */
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    col = f->tint | 0xFF000000u;
    finv = far_inv(v);
    if (f->advance)
        w->s_drift = fistp((float)(int32_t)(f->now_ms - w->s_last) * -160.0f
                           + (float)w->s_drift);
    w->s_drift &= 0x0FFFFFFF;
    w->s_last = f->now_ms;

    /* 0x004B2A53, the FPU draw */
    for (i = 0; i < n; i++) {
        int32_t ys = w->s_y[i] + w->s_drift;
        int32_t ph = (int32_t)(((uint32_t)ys & 0xFFFFFu) >> 11);
        int32_t x = w->s_x[i] + w->s_t[ph + w->s_ox[i]];
        int32_t z = w->s_z[i] + w->s_t[ph + w->s_oz[i]];
        float fx, fy, fz, cx, cy, cz, s, x0, y0, wd, sz;
        uint32_t c = col;
        plat_vertex q4[4];

        x = (int32_t)((uint32_t)(x + w->s_off[0]) & 0xFFFFFu) - 0x7FFFF;
        ys = (int32_t)((uint32_t)(ys + w->s_off[1]) & 0xFFFFFu) - 0x7FFFF;
        z = (int32_t)((uint32_t)(z + w->s_off[2]) & 0xFFFFFu) - 0x7FFFF;
        fx = (float)x; fy = (float)ys; fz = (float)z;
        cx = fx * m[0] + (fy * m[3] + fz * m[6]);
        cy = fx * m[1] + (fy * m[4] + fz * m[7]);
        cz = fx * m[2] + (fy * m[5] + fz * m[8]) + 524288.0f;
        if (fistp(cz) <= 0x10000)                   /* nearer than 1 m      */
            continue;
        if (f->lights) {
            float d0 = cx - lp[0], d1 = cy - lp[1], d2 = cz - lp[2];
            float q = 1.0f / sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
            float dot = q * (d0 * ld[0] + d1 * ld[1] + d2 * ld[2]);
            int32_t idx = fistp(dot * dot * 16.0f);
            if (bits(dot) < 0) idx = 0;
            if (idx < 0) idx = 0;
            if (idx > 16) idx = 16;
            c = w->s_light[idx];
        }
        s = v->focal / cz;
        x0 = cx * s + v->centre_x;
        y0 = v->centre_y - cy * s;
        wd = 6200.0f * s;
        sz = cz * finv;
        vtx(&q4[0], x0, y0, sz, 256.0f, c, 0, w->s_u[i], 0.001953125f);
        vtx(&q4[1], x0 + wd, y0, sz, 256.0f, c, 0, w->s_u[i] + 0.0625f,
            0.001953125f);
        vtx(&q4[2], x0 + wd, y0 + wd, sz, 256.0f, c, 0, w->s_u[i] + 0.0625f,
            0.064453125f);
        vtx(&q4[3], (x0 + wd) - wd, y0 + wd, sz, 256.0f, c, 0, w->s_u[i],
            0.064453125f);
        qb_add(q4);
    }
    qb_flush();
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);         /* 0x0044C47C           */
}

void wfx_draw_falling(wfx *w, const wfx_frame *f)
{
    if (!w->have_tables)
        return;
    rain3d(w, f);
    snow3d(w, f);
}

/* ---- the windscreen ------------------------------------------------------- */

/* FUN_00454419's four points, 0x00454604..0x0045469A: the car's two
 * windscreen points (mm -> 16.16 by IMUL / IDIV, y less 0.7 m), each also
 * with x negated, through the car's model -> view matrix, FISTP'd. */
static void glass_point(const wfx *w, const float *mv, int k, int neg,
                        int32_t out[3])
{
    const int32_t *r = &w->wscreen[w->model][k * 3];
    int32_t x = (int32_t)(((int64_t)r[0] * 0x10000) / 1000);
    int32_t y = (int32_t)(((int64_t)r[1] * 0x10000) / 1000) - w->yoff;
    int32_t z = (int32_t)(((int64_t)r[2] * 0x10000) / 1000);
    float fx = (float)x, fy = (float)y, fz = (float)z;
    if (neg) fx = -fx;
    out[0] = fistp(fx * mv[0] + fy * mv[4] + fz * mv[8] + mv[12]);
    out[1] = fistp(fx * mv[1] + fy * mv[5] + fz * mv[9] + mv[13]);
    out[2] = fistp(fx * mv[2] + fy * mv[6] + fz * mv[10] + mv[14]);
}

typedef struct {
    float O[3], A[3], B[3], C[3];
    float su, sv;
} glass_basis;

/* 0x004AA895 / 0x004AF22C's common head: block +0x2720 (Pa), +0x272C (Pb),
 * +0x2708 (P1).  Returns 0 when the glass faces away (the sign test RETs). */
static int glass(const wfx *w, const wfx_frame *f, glass_basis *g)
{
    int32_t p1[3], pa[3], pb[3];
    float b0[3], nrm[3], d;
    int k;

    glass_point(w, f->car_mv, 0, 1, p1);     /* +0x2708 */
    glass_point(w, f->car_mv, 1, 1, pa);     /* +0x2720 */
    glass_point(w, f->car_mv, 1, 0, pb);     /* +0x272C */
    for (k = 0; k < 3; k++) {
        g->O[k] = (float)pa[k];
        g->A[k] = (float)pb[k] - (float)pa[k];   /* FILD / FISUB          */
        b0[k] = (float)p1[k] - (float)pa[k];
    }
    g->su = sqrtf(g->A[0] * g->A[0] + g->A[1] * g->A[1] + g->A[2] * g->A[2])
          / 2048.0f;
    norm3(g->A);
    g->sv = sqrtf(b0[0] * b0[0] + b0[1] * b0[1] + b0[2] * b0[2]) / 1024.0f;
    norm3(b0);
    cross3(g->C, b0, g->A);
    norm3(g->C);
    cross3(g->B, g->A, g->C);
    for (k = 0; k < 3; k++) nrm[k] = (float)pa[k];
    norm3(nrm);
    d = nrm[0] * g->C[0] + nrm[1] * g->C[1] + nrm[2] * g->C[2];
    return bits(d) < 0;
}

/* ---- the screen snow, 0x004AF22C ------------------------------------------ */

static void flake_alloc(wfx *w, int32_t x, int32_t y, int32_t k)  /* 0x004AFD4C */
{
    int probe;
    for (probe = 0; probe < 32; probe++) {
        wfx_flake *p = &w->flake[w->flake_cur];
        w->flake_cur = (w->flake_cur + 1) % WFX_FLAKES_N;
        if (p->alive == 0) {
            p->alive = 1;
            p->age = 0;
            p->u = (float)(k & 7) * 0.0625f + 0.001953125f;
            p->v = 0.0f;
            p->x = (float)x;
            p->y = (float)y;
            return;
        }
    }
}

static void flake_spawn(wfx *w, const wfx_frame *f, float snow_s)  /* 0x004B0133 */
{
    float t = 0.0f + (90.0f - w->tilt_deg[w->model]) * w->deg2rad;
    /* [0x007B9D73]: 0x004B0D67's copy of [0x0071167E], unread - 0 here
     * (weatherfx.h, a STAND-IN) */
    float accz = 0.0f;
    float R = sinf(t) * snow_s + (-(snow_s * accz) * cosf(t)) * 0.1f;
    int32_t n, p;
    float fr;
    (void)f;
    if (R < 0.0f)
        return;
    n = chop(R);
    if (n >= 128) n = 128;
    while (n-- > 0) {
        uint32_t s = lcgc5(&w->flake_seed_b);       /* 0x004B024F            */
        flake_alloc(w, (int32_t)(s & 0x7FFu), (int32_t)((s >> 11) & 0x3FFu),
                    (int32_t)((s >> 21) & 7u));
    }
    fr = R - (float)chop(R);
    p = chop(fr * 65536.0f);
    if ((int32_t)((lcgc5(&w->flake_seed_a) >> 8) & 0xFFFFu) <= p) {
        uint32_t s = lcgc5(&w->flake_seed_b);
        flake_alloc(w, (int32_t)(s & 0x7FFu), (int32_t)((s >> 11) & 0x3FFu),
                    (int32_t)((s >> 21) & 7u));
    }
}

static void screen_snow(wfx *w, const wfx_frame *f, const glass_basis *g)
{
    const stage_view *v = f->v;
    float snow_s = (float)(f->inside ? (f->snow >> 3) : 0) * 1.0f;  /* +0x2740 */
    float dA[3], dB[3], hA[3], hB[3], zs;
    float zb = w->zbias[w->model];
    int i, k;

    if (fistp(snow_s * 65536.0f) == 0)
        return;
    for (k = 0; k < 3; k++) {
        dA[k] = g->A[k] * 1264.0f;                   /* [0x0083D782]         */
        dB[k] = g->B[k] * 1264.0f;
        hA[k] = dA[k] * 0.5f;
        hB[k] = dB[k] * 0.5f;
    }
    /* 0x004AFAC9 */
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_tex_bind(w->atlas);
    plat_gfx_set_state(PLAT_RS_TEXMAG, 2u);
    plat_gfx_set_state(PLAT_RS_TEXMIN, 2u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    zs = far_inv(v);

    /* 0x004AFC18 */
    if (f->advance)
        flake_spawn(w, f, snow_s);
    for (i = 0; i < WFX_FLAKES_N; i++) {
        wfx_flake *p = &w->flake[i];
        int32_t cx, cy, a;
        uint32_t col;
        float sx, sy, Q[3], c[4][3];
        plat_vertex q4[4];
        if (p->alive != 1) continue;
        if (f->advance) {
            p->age += w->dt;
            if (p->age >= 4000) { p->alive = 0; continue; }
        }
        cx = (chop(p->x) >> 3) & 0xFF;
        cy = (chop(p->y) >> 3) & 0x7F;
        if (w->edge[cy * 256 + cx]) { p->alive = 0; continue; }
        a = 0xFF - ((p->age / 16) & 0xFF);
        col = ((uint32_t)a << 24) | (f->tint & 0xFFFFFFu);
        /* 0x004AF42C */
        sx = p->x;
        sy = 1024.0f - p->y;
        for (k = 0; k < 3; k++) {
            Q[k] = g->O[k] + g->A[k] * (sx * g->su) + g->B[k] * (sy * g->sv);
            c[0][k] = Q[k] - hA[k] - hB[k];
            c[1][k] = c[0][k] + dB[k];
            c[2][k] = c[1][k] + dA[k];
            c[3][k] = c[2][k] - dB[k];
        }
        if (bits(c[0][2]) <= 0x45800000) continue;   /* c0 past 4096        */
        for (k = 0; k < 4; k++) {
            float s = v->focal / c[k][2];
            float X = v->centre_x + c[k][0] * s;
            float Y = v->centre_y - c[k][1] * s;
            float Z = (c[k][2] + zb) * zs;
            float u = (k == 0 || k == 3) ? p->u + 0.001953125f
                                         : p->u + 0.064453125f;
            float vv = (k <= 1) ? p->v + 0.001953125f : p->v + 0.064453125f;
            vtx(&q4[k], X, Y, Z, 256.0f, col, 0xFF000000u, u, vv);
        }
        if (!w->diag_no_glass_draw)
            qb_add(q4);
    }
    qb_flush();
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);           /* 0x0044C4BA          */
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    /* 0x004AF40B: block +0x2744 (1) and [0x0070D610], advancing - only
     * past the two returns above, so with no snow the cursor stays and the
     * drops' wipe-off takes the steps.  (0x004B0280 writes the
     * wipers' step and stroke as it goes; 0x004ADB8F recomputes both before
     * the next pose, so nothing sees them.) */
    if (f->advance && w->wipe && w->wipe->have && w->wp)
        wipers_snow_wipe(w->wipe, w->wp, &w->wipe_cur, w->flake, WFX_FLAKES_N,
                         sizeof w->flake[0], offsetof(wfx_flake, alive),
                         offsetof(wfx_flake, x), offsetof(wfx_flake, y));
}

/* ---- the screen rain, 0x004AA895 ------------------------------------------ */

/* 0x004AC806: the car's acceleration in the windscreen's frame */
static void drop_accel(wfx *w, const wfx_frame *f)
{
    float P[3], V[3], A[3], M[9], o[9], r[3], c, s;
    float inv = 1.0f / (float)w->dt;                 /* [0x0070D350]: inf at 0 */
    int k;

    w->acc[0] = w->acc[1] = w->acc[2] = 0.0f;
    for (k = 0; k < 3; k++) P[k] = (float)f->car_pos[k];
    if (!w->acc_first) {
        w->acc_first = 1;
        goto store_p;
    }
    if (!w->acc_second) {
        w->acc_second = 1;
        goto store_v;
    }
    for (k = 0; k < 3; k++) {
        V[k] = (P[k] - w->acc_prev_p[k]) * inv;
        A[k] = V[k] - w->acc_prev_v[k];
        A[k] += w->acc_prev_v[k] * 0.02f;
    }
    {
        const float *m = f->car_rot;
        M[0] = m[0]; M[1] = m[4]; M[2] = m[8];
        M[3] = m[1]; M[4] = m[5]; M[5] = m[9];
        M[6] = m[2]; M[7] = m[6]; M[8] = m[10];
    }
    o[0] = M[4] * M[8] - M[5] * M[7];  o[1] = M[2] * M[7] - M[1] * M[8];
    o[2] = M[1] * M[5] - M[2] * M[4];  o[3] = M[5] * M[6] - M[3] * M[8];
    o[4] = M[0] * M[8] - M[2] * M[6];  o[5] = M[2] * M[3] - M[0] * M[5];
    o[6] = M[3] * M[7] - M[4] * M[6];  o[7] = M[1] * M[6] - M[0] * M[7];
    o[8] = M[0] * M[4] - M[1] * M[3];
    r[0] = A[0] * o[0] + A[1] * o[1] + A[2] * o[2];
    r[1] = A[0] * o[3] + A[1] * o[4] + A[2] * o[5];
    r[2] = A[0] * o[6] + A[1] * o[7] + A[2] * o[8];
    /* 0x004623E0 with [0x007D6282] = -45.0f - in RADIANS, as the code has it */
    c = cosf(-45.0f);
    s = sinf(-45.0f);
    w->acc[0] = -r[0] * 0.02f;
    w->acc[1] = (c * r[1] + s * r[2]) * 0.02f;
    w->acc[2] = (-s * r[1] + c * r[2]) * 0.02f;
store_v:
    for (k = 0; k < 3; k++)
        w->acc_prev_v[k] = (P[k] - w->acc_prev_p[k]) * inv;
store_p:
    for (k = 0; k < 3; k++)
        w->acc_prev_p[k] = P[k];
}

static void drop_alloc(wfx *w, int32_t x, int32_t y, int32_t size)  /* 0x004ABE48 */
{
    int probe;
    for (probe = 0; probe < 32; probe++) {
        wfx_drop *d = &w->drop[w->drop_cur];
        w->drop_cur = (w->drop_cur + 1) % WFX_DROPS_N;
        if (d->active == 0) {
            d->active = 1;
            d->x = (float)x;
            d->y = (float)y;
            d->size = size;
            d->vx = d->vy = 0.0f;
            return;
        }
    }
}

static void drop_spawn_one(wfx *w)                 /* 0x004AC30E */
{
    uint32_t a = lcg9d(&w->lcg);
    drop_alloc(w, (int32_t)(a & 0x7FFu), (int32_t)(ror32(a, 11) & 0x3FFu),
               (int32_t)(ror32(a, 22) & 7u));
}

static void drop_spawn(wfx *w, float rate)         /* 0x004AC1F6, CHOP */
{
    float th = 0.0f + (90.0f - w->tilt_deg[w->model]) * w->deg2rad;
    float n = sinf(th) * rate;
    int32_t k, fr;
    n = (-(rate * w->acc[2])) * cosf(th) * 0.1f + n;
    if (n < 0.0f)
        return;
    k = chop(n);
    if (k >= 0x80) k = 0x80;
    while (--k >= 0)
        drop_spawn_one(w);
    fr = chop((n - (float)chop(n)) * 65536.0f);
    if ((int32_t)((lcg9d(&w->lcg) >> 8) & 0xFFFFu) <= fr)
        drop_spawn_one(w);
}

/* 0x004AB55B into a batch of 64; 0x0041A11B draws it */
static plat_vertex drop_v[64 * 6];
static uint16_t    drop_i[64 * 12];
static int         drop_n;

static int drop_nodraw;                 /* wfx.diag_no_glass_draw */

static void drop_flush(void)
{
    if (drop_n && !drop_nodraw)
        plat_gfx_draw_indexed(PLAT_PRIM_TRIANGLES, drop_v, (unsigned)drop_n * 6u,
                              drop_i, (unsigned)drop_n * 12u);
    drop_n = 0;
}

static void screen_rain(wfx *w, const wfx_frame *f, const glass_basis *g)
{
    const stage_view *v = f->v;
    float rate = (float)(f->inside ? f->rain * 4 : 0) * 0.7f;  /* +0x2738 */
    float dtk = 0.003f * (float)w->dt;
    float off[8][6][3], zs, W, H;
    int32_t cnt = 0;
    float zb = w->zbias[w->model];
    int i, s, k;

    for (s = 0; s < 8; s++)
        for (k = 0; k < 6; k++) {
            float x = w->hex[s][k][0] * g->su, y = w->hex[s][k][1] * g->sv;
            off[s][k][0] = x * g->A[0] + y * g->B[0];
            off[s][k][1] = x * g->A[1] + y * g->B[1];
            off[s][k][2] = y * g->B[2] + x * g->A[2];
        }
    if (bits(rate) == 0)
        return;

    /* 0x004ABCCE */
    if (f->advance)
        drop_accel(w, f);
    /* 0x004ABA70 */
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_tex_bind((w->copy_valid & 1) ? w->copy[w->copy_cur ^ 1] : 0);
    plat_gfx_set_state(PLAT_RS_TEXMAG, 1u);
    plat_gfx_set_state(PLAT_RS_TEXMIN, 1u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_set_state(PLAT_RS_TEXADDRESS, 1u);
    zs = far_inv(v);
    W = (float)f->screen_w;
    H = (float)f->screen_h;
    drop_n = 0;
    drop_nodraw = w->diag_no_glass_draw;
    for (i = 0; i < 64; i++) {
        uint16_t b = (uint16_t)(i * 6);
        uint16_t t[12] = { 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5 };
        for (k = 0; k < 12; k++) drop_i[i * 12 + k] = (uint16_t)(b + t[k]);
    }

    for (i = 0; i < WFX_DROPS_N; i++) {
        wfx_drop *d = &w->drop[i];
        int32_t X, Y, idx;
        if (d->active != 1) continue;
        cnt++;
        if (f->advance) {
            int32_t xi = fistp(d->x);
            float c = w->curve[((uint32_t)xi >> 3) & 0xFFu];
            float gx = -(c * w->acc[2]) + w->acc[0];
            float gy = w->acc[1];
            float rad = w->drop_rad[d->size & 7];
            float nvx = (gx * rad - d->vx) * dtk + d->vx;
            float nvy = (gy * rad - d->vy) * dtk + d->vy;
            float nx = d->x + nvx, ny = d->y + nvy;
            d->vx = nvx; d->vy = nvy;
            d->x = nx; d->y = ny;
        }
        X = fistp(d->x);
        Y = fistp(d->y);
        if (((X | (Y + Y)) & 0x800) != 0) { d->active = 0; continue; }
        idx = (int32_t)(((((uint32_t)Y >> 3) & 0x7Fu) << 8)
                        + (((uint32_t)X >> 3) & 0xFFu));
        if (w->edge[idx]) { d->active = 0; continue; }
        /* 0x004AB55B: u = X, v = 1024 - Y */
        {
            float u = (float)X * g->su, vv = (1024.0f - (float)Y) * g->sv;
            float pc[3], kk, sxc, syc, sz;
            plat_vertex *o = &drop_v[drop_n * 6];
            pc[0] = u * g->A[0] + vv * g->B[0] + g->O[0];
            pc[1] = u * g->A[1] + vv * g->B[1] + g->O[1];
            pc[2] = vv * g->B[2] + u * g->A[2] + g->O[2];
            if (bits(pc[2]) < 0x45800000) continue;   /* nearer than 4096    */
            kk = v->focal / pc[2];
            sxc = pc[0] * kk + v->centre_x;
            syc = -(pc[1] * kk) + v->centre_y;
            sz = (pc[2] + zb) * zs;
            for (k = 0; k < 6; k++) {
                const float *of = off[d->size & 7][k];
                float sx = (pc[0] + of[0]) * kk + v->centre_x;
                float sy = -((pc[1] + of[1]) * kk) + v->centre_y;
                /* The lens: the texture is the screen, sampled about the
                 * drop's centre at -16x - (sxc/800 + (sx - sxc) * -0.02) in
                 * the original's 800 x 600, here the frame's own size, and
                 * then into the copy, whose rows run bottom-up. */
                float lx = sxc + (sx - sxc) * -16.0f;
                float ly = syc + (sy - syc) * -16.0f;
                float tu = lx / (float)w->copy_w;
                float tv = (H - ly) / (float)w->copy_h;
                (void)W;
                vtx(&o[k], sx, sy, sz, 256.0f, 0xFFFFFFFFu, 0xFF000000u, tu, tv);
            }
            if (++drop_n == 64)
                drop_flush();
        }
    }
    if (cnt < f->rain * 2 && f->advance)            /* block +0x273C         */
        drop_spawn(w, rate);
    drop_flush();

    /* 0x004AC6ED: the wipe-off (0x004AAB76: block +0x2744 and [0x0070D610]).
     * Per drop the evaporation draw first, then the phases since the cursor,
     * newest first, through the triangle table against either mask.  The
     * cursor is snow's too: on a stage with both, snow took the steps
     * and the drops see none - no stage has both. */
    if (f->advance) {
        int wipe = w->wipe && w->wipe->have && w->wp;
        wipers_rain_wipe(wipe ? w->wipe : NULL, wipe ? w->wp->phase : 0,
                         &w->wipe_cur, &w->evap, w->drop, WFX_DROPS_N,
                         sizeof w->drop[0], offsetof(wfx_drop, active),
                         offsetof(wfx_drop, x), offsetof(wfx_drop, y));
    }
}

void wfx_draw_screen(wfx *w, const wfx_frame *f)
{
    glass_basis g;

    if (!w->have_tables)
        return;
    /* 0x004B7106's own states first */
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    /* 0x004B8BA2, the dirt layer, is the.  0x004AF22C: */
    if (glass(w, f, &g))
        screen_snow(w, f, &g);
    /* 0x004AA895 */
    if (glass(w, f, &g))
        screen_rain(w, f, &g);
    plat_gfx_tex_bind(w->atlas);                     /* 0x004AAB90          */
}
