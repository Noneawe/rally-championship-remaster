/* camera.c - the original's in-race camera, its step half.
 *
 * is the reading, and tools/camera.py its first model;
 * camera_check (this directory) steps THIS code over the hook's dumps.  The
 * x87 rule is physics.c's: a run of x87 arithmetic that ends in one store is
 * computed in double and rounded once.  Where the original stores a float32
 * in between (FUN_0041EC71's operands and results, the lerped block values),
 * this stores one too. */

#include "camera.h"
#include "ground.h"
#include "physics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI_D 3.14159265358979323846

/* FISTP, round to nearest even; out of range or NaN gives the x87's integer
 * indefinite.  physics.c's fistp_rne, which is static there. */
static int32_t fistp(double v)
{
    double f, d, r;
    if (!(v > -2147483648.5 && v < 2147483647.5)) return INT32_MIN;
    f = floor(v);
    d = v - f;
    if (d > 0.5)      r = f + 1.0;
    else if (d < 0.5) r = f;
    else              r = (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
    return (int32_t)r;
}

static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static float   f32(double v) { return (float)v; }

/* ---- the image ---------------------------------------------------------- */

static int rd_f32s(const ral_image *im, uint32_t va, float *out, int n)
{
    const unsigned char *p = ral_image_at(im, va, (size_t)n * 4u);
    int i;
    if (!p) return -1;
    for (i = 0; i < n; i++) out[i] = ral_image_f32(p + 4 * i);
    return 0;
}

static int rd_i32s(const ral_image *im, uint32_t va, int32_t *out, int n)
{
    const unsigned char *p = ral_image_at(im, va, (size_t)n * 4u);
    int i;
    if (!p) return -1;
    for (i = 0; i < n; i++) out[i] = ral_image_i32(p + 4 * i);
    return 0;
}

int sim_camera_consts_load(const ral_image *im, sim_camera_consts *k,
                           char *err, size_t errlen)
{
    int v, bad = 0;
    memset(k, 0, sizeof *k);
    for (v = 0; v < SIM_CAMERA_VIEWS; v++)
        bad |= rd_f32s(im, 0x005BA730u + 0x38u * (uint32_t)v, k->block[v],
                       SIM_CAMERA_BLOCK);
    bad |= rd_f32s(im, 0x005BAAB0u, k->chase[0], 7);
    bad |= rd_f32s(im, 0x005BAACCu, k->chase[1], 7);
    bad |= rd_i32s(im, 0x005BA6ACu, k->family, SIM_CAMERA_VIEWS);
    for (v = 0; v < SIM_CAMERA_CARS; v++)
        bad |= rd_i32s(im, 0x0057E10Cu + 0x12Cu * (uint32_t)v, k->eye[v], 9);
    bad |= rd_i32s(im, 0x005C660Cu, k->name, SIM_CAMERA_NAMES);
    bad |= rd_i32s(im, 0x005BA668u, &k->nviews, 1);
    bad |= rd_f32s(im, 0x0057B024u, &k->inv65536, 1);
    bad |= rd_f32s(im, 0x005C6CF4u, &k->eye_drop, 1);
    bad |= rd_f32s(im, 0x005C6DD0u, &k->shake_k, 1);
    bad |= rd_f32s(im, 0x005C6DCCu, &k->shake_max, 1);
    bad |= rd_f32s(im, 0x0057AFC4u, &k->shake_h, 1);
    bad |= rd_f32s(im, 0x005BA620u, &k->fish_min, 1);
    bad |= rd_f32s(im, 0x0057AFBCu, &k->one, 1);
    bad |= rd_f32s(im, 0x0057B01Cu, &k->k65536, 1);
    bad |= rd_f32s(im, 0x0057B0ACu, &k->k4096, 1);
    bad |= rd_f32s(im, 0x0057B0C4u, &k->half, 1);
    bad |= rd_f32s(im, 0x0057B0B0u, &k->mirror_fish, 1);
    bad |= rd_f32s(im, 0x005BA5D4u, &k->mirror_fish2, 1);
    bad |= rd_f32s(im, 0x0059EC14u, k->mirror_rot, 16);
    bad |= rd_i32s(im, 0x005F44BCu, &k->mirror_at[0][0], 24);
    bad |= rd_f32s(im, 0x005D6C60u, &k->focal_k, 1);
    bad |= rd_f32s(im, 0x0057B058u, &k->inv2pi, 1);
    bad |= rd_f32s(im, 0x0057AFD8u, &k->lean_div, 1);
    bad |= rd_f32s(im, 0x0057B054u, &k->lean_k, 1);
    if (bad) {
        if (err && errlen) snprintf(err, errlen, "camera: a table is not in the image");
        return -1;
    }
    /* All seven writers of [0x005BA668] store 7, as the image does; a
     * different image value would be a different build. */
    if (k->nviews < 1 || k->nviews > SIM_CAMERA_VIEWS) k->nviews = 7;
    return 0;
}

void sim_camera_init(sim_camera_ctx *c, const sim_camera_consts *k,
                     int32_t view)
{
    memset(c, 0, sizeof *c);
    c->k = k;
    memcpy(c->block, k->block, sizeof c->block);
    c->numpl = 1;
    c->cam.view = c->cam.nview = view;
    c->cam.fishval = k->block[view & 15][13];
    c->cam.interior = (view == 2 || view == 3);
    c->cam.hint[0] = c->cam.hint[1] = c->cam.hint[2] = -1;
    c->cam.hint[3] = c->cam.hint[4] = -1;
    c->mask = 0;
}

/* ---- the view keys ------------------------------------------------------ */

void sim_camera_change_q(sim_camera_ctx *c, int32_t clock)
{
    sim_camera *m = &c->cam;
    /* FUN_00438FB3: [0x005BA6AC + view*4] against the new view's. */
    if (c->k->family[m->view & 15] != c->k->family[m->nview & 15]) {
        m->view = m->nview;
        m->blending = 0;
        m->t = 0.0f;                      /* +0x69C = 0 (the dword)          */
        m->length = 0;
        return;
    }
    m->blending = 1;
    m->t = 0.0f;
    m->length = 0x28;
    m->start = clock;
}

int32_t sim_camera_next_view_q(sim_camera_ctx *c, int32_t clock)
{
    sim_camera *m = &c->cam;
    if (m->blending == 0) {               /* FUN_00439024                     */
        uint32_t n = (uint32_t)m->view + 1u;
        if (n > 1u && n < 4u && c->numpl == 2) n = 4u;
        if ((uint32_t)c->k->nviews <= n) n = 0u;
        m->nview = (int32_t)n;
        sim_camera_change_q(c, clock);
    }
    /* 0x00438F4D: the message is +0x688's name, whether or not it changed. */
    if ((uint32_t)m->nview < (uint32_t)SIM_CAMERA_NAMES)
        return c->k->name[m->nview];
    return -1;
}

void sim_camera_rest_q(sim_camera_ctx *c, const int32_t binary_angle[3])
{
    sim_camera *m = &c->cam;
    /* FUN_0043E00B: heading, pitch, roll. */
    m->yaw   = binary_angle[0];
    m->pitch = binary_angle[1];
    m->roll  = binary_angle[2];
    if (m->restore != 0) {
        m->view = m->nview = m->restore - 1;
        m->restore = 0;
    }
}

/* ---- the step ----------------------------------------------------------- */

/* One step's process globals, FUN_004364A5 to FUN_00437827. */
typedef struct cam_step {
    sim_camera_ctx *c;
    const car_state *car;
    const sim_ground_geom *geom;
    const float *sin;
    int32_t carpos[3];        /* [0x005F45A8..B0]                           */
    int32_t off[3];           /* [0x005BB7EC..F4]  the view offset, 16.16   */
    int32_t local[3];         /* [0x005BB830..38]  the car-frame offset     */
    int32_t tgt_yaw;          /* [0x005BB80C]                               */
    int32_t tgt_roll;         /* [0x005BB810]                               */
    int32_t tgt_pitch;        /* [0x005BB814]                               */
    float   k_pitch;          /* [0x005BB8A0]                               */
    float   k_yaw;            /* [0x005BB8A4]                               */
    float   k_roll;           /* [0x005BB8A8]                               */
    int32_t height;           /* [0x005EC6E8] as the last query left it     */
    int     error;
} cam_step;

/* FUN_0041EC71 on the pair (a, b): FILD -> float32, the products in the x87,
 * each result stored float32, then FISTP. */
static void rot2(const float *sin, int32_t *a, int32_t *b, int32_t ang)
{
    uint32_t i = ((uint32_t)ang + 0x80000u) >> 20;
    double s = sin[i], c = sin[i + 1024u];
    double A = (float)*a, B = (float)*b;
    float na = f32(A * c + B * s), nb = f32(B * c - A * s);
    *a = fistp(na);
    *b = fistp(nb);
}
/* The three entry points: 0x0041EB77 (y, x), 0x0041EB01 (z, y), 0x0041EA8B
 * (x, z). */
static void rot_z(const float *s, int32_t v[3], int32_t a) { rot2(s, &v[1], &v[0], a); }
static void rot_x(const float *s, int32_t v[3], int32_t a) { rot2(s, &v[2], &v[1], a); }
static void rot_y(const float *s, int32_t v[3], int32_t a) { rot2(s, &v[0], &v[2], a); }

/* FUN_004375CA: [0x005D31A0] = 0x40, then the ground query.  1 found. */
static int query(cam_step *s, int32_t x, int32_t z, int32_t hint,
                 int32_t *poly)
{
    sim_ground_hit h;
    int rc;
    s->c->mask = 0x40u;
    s->c->queries++;
    rc = sim_ground_query(s->geom, x, z, hint, 0x40u, &h);
    if (rc < 0) { s->error = 1; return 0; }
    if (rc == 1) {
        *poly = h.poly;
        s->height = h.height;
        return 1;
    }
    return 0;
}

/* FUN_00437602: the position from the angles. */
static void position(cam_step *s)
{
    sim_camera_ctx *c = s->c;
    sim_camera *m = &c->cam;
    const float *ch = c->k->chase[c->layout == 2 ? 1 : 0];
    const float *mt = s->car->matrix;
    int32_t v[3];
    double lx, ly, lz;
    int32_t w[3];
    int i;

    /* 0x00437602: views 4-6's height, distance and pitch, every call. */
    c->block[4][1] = ch[0]; c->block[4][2] = ch[1];
    c->block[5][1] = ch[2]; c->block[5][2] = ch[3];
    c->block[6][1] = ch[4]; c->block[6][2] = ch[5];
    c->block[4][6] = c->block[5][6] = c->block[6][6] = ch[6];

    v[0] = s->off[0]; v[1] = s->off[1]; v[2] = s->off[2];
    rot_z(s->sin, v, m->vroll);
    rot_x(s->sin, v, m->vpitch);
    rot_y(s->sin, v, m->vyaw);
    rot_z(s->sin, v, m->roll);
    rot_x(s->sin, v, m->pitch);
    rot_y(s->sin, v, m->yaw);

    /* FUN_0044B1AB with the car's matrix (+0xB4), row vector, + row 3. */
    lx = (double)s->local[0] / 65536.0;
    ly = (double)s->local[1] / 65536.0;
    lz = (double)s->local[2] / 65536.0;
    w[0] = fistp((mt[0] * lx + (mt[4] * ly + mt[8]  * lz) + mt[12]) * 65536.0);
    w[1] = fistp((mt[1] * lx + (mt[5] * ly + mt[9]  * lz) + mt[13]) * 65536.0);
    w[2] = fistp((mt[2] * lx + (mt[6] * ly + mt[10] * lz) + mt[14]) * 65536.0);
    for (i = 0; i < 3; i++)
        m->pos[i] = wadd(v[i], wadd(w[i], s->carpos[i]));
}

/* FUN_00437494, the footprint: CF = 1 (returned 1) when the ground under
 * the camera, or 0.5 m to either side of it, is missing. */
static int footprint_fails(cam_step *s)
{
    sim_camera *m = &s->c->cam;
    int32_t d[3];
    int32_t poly;

    if (!query(s, m->pos[0], m->pos[2], m->hint[0], &poly)) return 1;
    m->hint[0] = poly;
    d[0] = (int32_t)0xFFFF8000u; d[1] = 0; d[2] = 0;
    rot_y(s->sin, d, m->yaw);
    if (!query(s, wadd(d[0], m->pos[0]), wadd(d[2], m->pos[2]), m->hint[1],
               &poly)) return 1;
    m->hint[1] = poly;
    d[0] = 0x8000; d[1] = 0; d[2] = 0;
    rot_y(s->sin, d, m->yaw);
    if (!query(s, wadd(d[0], m->pos[0]), wadd(d[2], m->pos[2]), m->hint[2],
               &poly)) return 1;
    m->hint[2] = poly;
    return 0;
}

/* FUN_0043757A, the height: CF = 1 unless the camera is at least 1.0 m above
 * the ground under it. */
static int height_fails(cam_step *s)
{
    sim_camera *m = &s->c->cam;
    int32_t poly;
    if (!query(s, m->pos[0], m->pos[2], m->hint[0], &poly)) return 1;
    m->hint[0] = poly;
    return wsub(wsub(m->pos[1], s->height), 0x10000) < 0;
}

/* FUN_0044E1CF: the 16-bit heading from (px, pz) to (cx, cz). */
static int32_t heading16(int32_t px, int32_t pz, int32_t cx, int32_t cz)
{
    double dx = (double)wsub(cx, px), dz = (double)wsub(cz, pz);
    double a = atan2(dx, dz) / PI_D * 0.5 * 65536.0;
    return fistp(a) & 0xFFFF;
}

/* 0x0043785C / 0x004378B9: aim along route strip car+0x98 - back.  0 when
 * there is no such strip (JS taken) - the port also refuses a strip past
 * the route, where the original would read on. */
static int route_yaw(cam_step *s, int back, int32_t *yaw)
{
    const sim_route *r = s->c->route;
    int32_t i = wsub(s->car->nearestb, back), pol, pt;
    const uint8_t *rec, *p;
    if (i < 0 || !r || i >= r->n || !s->geom) return 0;
    pol = r->pol[i];
    if (pol < 0 || pol >= s->geom->npols) return 0;
    rec = s->geom->pols + (size_t)pol * 0x40u;
    memcpy(&pt, rec + 4, 4);
    if (pt < 0 || pt >= s->geom->npnts) return 0;
    p = s->geom->pnts + (size_t)pt * 0x0Cu;
    {
        int32_t px, pz;
        memcpy(&px, p, 4);
        memcpy(&pz, p + 8, 4);
        *yaw = (int32_t)((uint32_t)heading16(px, pz, s->car->pos[0],
                                             s->car->pos[2]) << 16);
    }
    return 1;
}

/* FUN_004372F7, the lag. */
static void lag(cam_step *s)
{
    sim_camera *m = &s->c->cam;
    if (s->c->snap) s->k_pitch = s->k_yaw = s->k_roll = 1.0f;
    m->yaw   = wadd(m->yaw,   fistp((double)wsub(s->tgt_yaw,   m->yaw)   * s->k_yaw));
    m->pitch = wadd(m->pitch, fistp((double)wsub(s->tgt_pitch, m->pitch) * s->k_pitch));
    m->roll  = wadd(m->roll,  fistp((double)wsub(s->tgt_roll,  m->roll)  * s->k_roll));
}

/* FUN_00437827: lag, position, the yaw search, the height bisection. */
static void place(cam_step *s)
{
    sim_camera *m = &s->c->cam;
    int32_t y0, yaw;
    uint32_t step;
    int ok;

    lag(s);
    y0 = m->yaw;                                   /* [0x005D6E6C]           */
    position(s);
    if (!footprint_fails(s)) goto height;
    s->c->search = 1;
    /* 0x0043785C: along the route, one strip back, then two. */
    ok = route_yaw(s, 1, &yaw);
    if (!ok) { m->yaw = y0; goto height; }
    m->yaw = yaw;
    position(s);
    if (footprint_fails(s)) {
        ok = route_yaw(s, 2, &yaw);
        if (!ok) { m->yaw = y0; goto height; }
        m->yaw = yaw;
        position(s);
        if (footprint_fails(s)) { m->yaw = y0; goto height; }
    }
    /* 0x0043791A: walk back toward y0 in steps that shrink by 8. */
    step = 0x4000000u;
    for (;;) {
        int32_t sy = m->yaw, sh[5], d;
        uint32_t ad;
        memcpy(sh, m->hint, sizeof sh);
        d = wsub(y0, m->yaw);
        ad = d < 0 ? (uint32_t)0 - (uint32_t)d : (uint32_t)d;
        if (ad >= step) {
            m->yaw = wadd(m->yaw, d < 0 ? (int32_t)((uint32_t)0 - step)
                                        : (int32_t)step);
            position(s);
            if (!footprint_fails(s)) continue;     /* JNC 0x00437924         */
            m->yaw = sy;
            memcpy(m->hint, sh, sizeof sh);
        }
        step >>= 3;                                /* 0x004379F9             */
        if (step <= 0x1000u) break;
    }

height:
    /* 0x00437A20 */
    position(s);
    if (!height_fails(s)) return;
    {
        int32_t hi = 0x40000000, lo, half;
        for (;;) {
            /* 0x00437A40: raise by half the gap to straight down. */
            lo = m->pitch;
            half = (int32_t)((uint32_t)wsub(hi, m->pitch) >> 1);
            m->pitch = wadd(m->pitch, half);
            if (half == 0) return;
            position(s);
            if (height_fails(s)) continue;
            /* 0x00437A66: lower by half the gap to the last failure. */
            for (;;) {
                hi = m->pitch;
                half = (int32_t)((uint32_t)wsub(m->pitch, lo) >> 1);
                m->pitch = wsub(m->pitch, half);
                if (half == 0) return;
                position(s);
                if (height_fails(s)) break;
            }
        }
    }
}

/* FUN_0043739C: the bumper views' check for an upside-down car. */
static void bumper_check(cam_step *s)
{
    sim_camera *m = &s->c->cam;
    int32_t poly;
    /* FLD car+0xC8 ; FCOMP 0.0 ; JAE skip: proceed on less-than or NaN. */
    if (s->car->matrix[5] >= 0.0f) return;
    if (m->blending != 0) return;
    if (m->view != 1 && m->view != 0) return;
    if (!query(s, m->pos[0], m->pos[2], m->hint[0], &poly)) return;
    m->hint[0] = poly;
    if (wsub(wsub(m->pos[1], s->height), 0x800) >= 0) return;
    m->restore = m->nview + 1;
    m->nview = 4;
    m->blending = 1;
    m->t = 0.0f;
    m->length = 10;
    m->start = s->car->alivet;
}

/* FUN_0043695A / FUN_00436A1E's targets: halved unless the view is 1-3. */
static int32_t tgt(int32_t a, int32_t view)
{
    return (view == 1 || view == 2 || view == 3) ? a : (a >> 1);
}

int sim_camera_step_q(sim_camera_ctx *c, const car_state *car,
                      const int32_t binary_angle[3], float yvel_delta,
                      uint32_t *rnd, const sim_ground_geom *geom,
                      const float *sin_table)
{
    const sim_camera_consts *k = c->k;
    sim_camera *m = &c->cam;
    cam_step s;
    const float *fb, *tb;
    float fo[3], to[3];
    double t;
    int32_t id;
    int i;

    memset(&s, 0, sizeof s);
    s.c = c; s.car = car; s.geom = geom; s.sin = sin_table;
    c->queries = 0;
    c->search = 0;

    /* FUN_0044DEE4: the shake, views 1-3 only, when car+0x254 == 0. */
    c->shake[0] = c->shake[1] = c->shake[2] = c->shake[3] = 0.0f;
    if ((m->view == 1 || m->view == 2 || m->view == 3)
        && car->yaw_couple_gate == 0) {
        float sy = f32((double)yvel_delta * k->shake_k);
        float mx = k->shake_max;
        if (!(sy < mx)) sy = mx;                   /* FCOMPP, JC skips       */
        if (sy < -mx) sy = -mx;                    /* JNC skips              */
        c->shake[1] = sy;
        if (rnd) {
            int32_t r = (int32_t)sim_rnd_next_q(rnd);
            c->shake[3] = f32((double)r / k->k65536 / k->k4096
                              * k->shake_h * sy);
        }
    }

    /* FUN_004364A5 */
    bumper_check(&s);
    for (i = 0; i < 3; i++) s.carpos[i] = car->pos[i];
    id = car->carnum1;
    if (id >= 0 && id < SIM_CAMERA_CARS) {
        const int32_t *e = k->eye[id];
        double inv = k->inv65536;
        c->block[2][3] = f32(e[0] * inv);
        c->block[2][4] = f32(e[1] * inv - k->eye_drop);
        c->block[2][5] = f32(e[2] * inv);
        c->block[3][3] = f32(e[4] * inv);
        c->block[3][4] = f32(e[5] * inv - k->eye_drop);
        c->block[3][5] = f32(e[6] * inv);
        memcpy(&c->block[3][6], &e[7], 4);         /* raw, 0x0043654A        */
        c->block[1][5] = f32(e[8] * inv);
    }
    /* Views 7-10 with no blend take the trackside branch (0x00436AE2),
     * which is not ported: retail cannot select them in a race. */

    /* 0x0043659B */
    for (i = 0; i < 3; i++) m->pos[i] = car->pos[i];
    if (m->blending != 0) {
        if (wsub(wsub(car->alivet, m->start), m->length) < 0) {
            float tt = f32((double)wsub(car->alivet, m->start)
                           / (double)m->length);
            if (!(tt < k->one)) tt = k->one;
            if (tt < 0.0f || tt != tt) tt = 0.0f;
            m->t = tt;
        } else {
            m->blending = 0;
            m->t = 0.0f;
            m->view = m->nview;
            if ((uint32_t)k->nviews <= (uint32_t)m->view) m->view = 0;
        }
    }
    fb = c->block[m->view & 15];
    tb = c->block[m->nview & 15];
    /* FUN_0043695A / FUN_00436A1E: the offsets carry the shake. */
    fo[0] = f32((double)fb[0] + c->shake[0]);
    fo[1] = f32((double)fb[1] + c->shake[1]);
    fo[2] = f32((double)fb[2] + c->shake[2]);
    to[0] = f32((double)tb[0] + c->shake[0]);
    to[1] = f32((double)tb[1] + c->shake[1]);
    to[2] = f32((double)tb[2] + c->shake[2]);
    t = m->t;
    {
        int32_t fy = binary_angle[0], ty = binary_angle[0];
        int32_t fp = tgt(binary_angle[1], m->view), tp = tgt(binary_angle[1], m->nview);
        int32_t fr = tgt(binary_angle[2], m->view), tr = tgt(binary_angle[2], m->nview);
        s.tgt_pitch = fistp(((double)tp - fp) * t + fp);
        s.tgt_yaw   = fistp(((double)ty - fy) * t + fy);
        s.tgt_roll  = fistp(((double)tr - fr) * t + fr);
    }
#define LERP(j) (((double)tb[j] - fb[j]) * t + fb[j])
    m->fishval = f32(LERP(13));
    s.k_pitch = f32(LERP(9));
    s.k_yaw   = f32(LERP(10));
    s.k_roll  = f32(LERP(11));
    for (i = 0; i < 3; i++) {
        s.off[i] = fistp((((double)to[i] - fo[i]) * t + fo[i]) * k->k65536);
        s.local[i] = fistp(LERP(3 + i) * k->k65536);
    }
    m->vpitch = (int32_t)((uint32_t)fistp(LERP(6) * k->k65536) << 16);
    m->vyaw   = (int32_t)((uint32_t)fistp(LERP(7) * k->k65536) << 16);
    m->vroll  = (int32_t)((uint32_t)fistp(LERP(8) * k->k65536) << 16);
#undef LERP
    if (m->fishval < k->fish_min) m->fishval = k->fish_min;

    place(&s);
    m->interior = (m->view == 2 || m->view == 3);
    return s.error ? -1 : 0;
}
