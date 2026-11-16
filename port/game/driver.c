/* driver.c - the cockpit's articulated parts; driver.h
 * says what is the original's and what is not.  Every address is retail's. */

#include "driver.h"
#include "x87geom.h"

#include <math.h>
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

int driver_load_table(driver *d, const char *path, char *err, size_t errlen)
{
    unsigned char b[256 * 12];
    FILE *f = fopen(path, "rb");
    size_t i;

    d->have_table = 0;
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    if (fread(b, 1, sizeof b, f) != sizeof b) {
        fclose(f);
        snprintf(err, errlen, "%s is shorter than 0xC00 bytes", path);
        return -1;
    }
    fclose(f);
    for (i = 0; i < 256 * 3; i++) {
        uint32_t u = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8
                     | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
        memcpy(&d->w[i / 3][i % 3], &u, 4);
    }
    d->have_table = 1;
    return 0;
}

/* 0x004A996D / 0x004A9A37: the vertices of the polygons of material `mat`,
 * in polygon order, each corner's vertex once, at most 128; the rest points
 * are the model's own, 16.16 */
static void collect(driver_part *r, const ro3_model *m, uint32_t mat)
{
    size_t i;
    int j, e;

    memset(r, 0, sizeof *r);
    for (i = 0; i < m->npolys; i++) {
        const ro3_poly *q = &m->polys[i];
        if ((((uint32_t)q->flags >> 24) & 0x1Fu) != mat) continue;
        for (j = 0; j < q->corners; j++) {
            int32_t v = m->corner_vert[q->first_corner + (size_t)j];
            if (r->n == DRIVER_VERTS) break;
            for (e = 0; e < r->n; e++)
                if (r->id[e] == v) break;
            if (e == r->n) r->id[r->n++] = v;
        }
    }
    for (e = 0; e < r->n; e++) {
        const ro3_vec *q = &m->points[r->id[e]];
        r->rest[e][0] = fix(q->x);
        r->rest[e][1] = fix(q->y);
        r->rest[e][2] = fix(q->z);
    }
}

/* 0x00462147: a vector to length one, float32 components */
static void normalise(float v[3])
{
    ext s = (ext)v[0] * v[0] + ((ext)v[1] * v[1] + (ext)v[2] * v[2]);
    ext inv = EXT_C(1.0) / ext_sqrt(s);
    ext x = (ext)v[0] * inv, y = (ext)v[1] * inv, z = (ext)v[2] * inv;
    v[0] = (float)x;
    v[1] = (float)y;
    v[2] = (float)z;
}

/* 0x0046252C: the arc-cosine of a . b by 16 bisections from pi / 2, float32
 * state, the step halved each time ([0x006CF49E], [0x006CF4A2], [0x006CF4A6]) */
static float arccos_bisect(const float a_[3], const float b_[3],
                           float start, float step, float half)
{
    float a[3], b[3], acc = start, st = step, dot;
    int i;

    memcpy(a, a_, sizeof a);
    memcpy(b, b_, sizeof b);
    normalise(b);                                    /* edi first, then esi */
    normalise(a);
    dot = (float)((ext)a[0] * b[0] + ((ext)a[1] * b[1] + (ext)a[2] * b[2]));
    for (i = 0; i < 16; i++) {
        ext c = ext_cos((ext)acc);
        if (c < (ext)dot)                            /* C0: the sub branch   */
            acc = (float)((ext)acc - (ext)st);
        else                                         /* above, equal or NaN  */
            acc = (float)((ext)acc + (ext)st);
        st = (float)((ext)st * (ext)half);
    }
    return acc;
}

/* 0x004A9A8B: the bounding box's middle, and the angle at +0x20 between the
 * box's (0, dy, dz) and the axis at [0x007124AA] */
static void origin_of(driver_part *r, const float axis[3], float bx,
                      float s0, float s1, float s2)
{
    uint32_t mn[3] = { 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu };
    uint32_t mx[3] = { 0x80000001u, 0x80000001u, 0x80000001u };
    float b[3];
    int i, k;

    for (i = 0; i < r->n; i++)
        for (k = 0; k < 3; k++) {
            if (r->rest[i][k] < (int32_t)mn[k]) mn[k] = (uint32_t)r->rest[i][k];
            if (r->rest[i][k] > (int32_t)mx[k]) mx[k] = (uint32_t)r->rest[i][k];
        }
    b[0] = bx;
    b[1] = (float)(int32_t)(mx[1] - mn[1]);
    b[2] = (float)(int32_t)(mx[2] - mn[2]);
    for (k = 0; k < 3; k++)
        r->origin[k] = (int32_t)(mn[k] + (uint32_t)((int32_t)(mx[k] - mn[k]) >> 1));
    r->ang[0] = arccos_bisect(axis, b, s0, s1, s2);
}

/* 0x004A9BA3 */
static void to_origin(driver_part *r)
{
    int i, k;
    for (i = 0; i < r->n; i++)
        for (k = 0; k < 3; k++)
            r->rest[i][k] = (int32_t)((uint32_t)r->rest[i][k]
                                      - (uint32_t)r->origin[k]);
}

/* the matrix of the three angles, as 0x004A9BF4 (negated, the load) and
 * 0x004A9CBC (as they are) build it: (Rx Ry) Rz */
static void euler(float m[9], ext ax, ext ay, ext az)
{
    float rx[9], ry[9], rz[9], t[9];
    rot_x(rx, ax);
    rot_y(ry, ay);
    rot_z(rz, az);
    mat_mul(t, rx, ry);
    mat_mul(m, t, rz);
}

/* 0x004A9BF4: the rest points turned by the negated angles, in place */
static void turn_in(driver_part *r)
{
    float m[9];
    int i;
    euler(m, -(ext)r->ang[0], -(ext)r->ang[1], -(ext)r->ang[2]);
    for (i = 0; i < r->n; i++) {
        int32_t t[3];
        xform(t, m, r->rest[i]);
        memcpy(r->rest[i], t, sizeof t);
    }
}

/* The record's memory as 0x004AA0FF reads it: the dword at byte
 * `off`, from the arrays the load has made - the rest points at +0x654, the
 * origin at +0xC54, the classes at +0xC90 - and zero elsewhere (a census of
 * the image: the only initialised bytes of a record are +0x38..+0x4B, and
 * nothing here reads them) */
static int32_t rec_word(const driver_part *r, uint32_t off)
{
    if (off >= 0x654u && off < 0x654u + 12u * (uint32_t)r->n)
        return r->rest[(off - 0x654u) / 12u][((off - 0x654u) % 12u) / 4u];
    if (off >= 0xC54u && off < 0xC60u)
        return r->origin[(off - 0xC54u) / 4u];
    if (off >= 0xC90u && off < 0xC90u + 4u * (uint32_t)r->n)
        return r->cls[(off - 0xC90u) / 4u];
    return 0;
}

/* 0x004A9F24 (the classes by height), 0x004A9F96 (the vertices sorted into
 * them, each class from the last vertex down), 0x004AA0FF (the highest and
 * lowest z of the first two, the last of equals) */
static void split_hand(driver_part *r)
{
    int32_t sid[DRIVER_VERTS], srest[DRIVER_VERTS][3];
    int32_t best, bi;
    int i, g, at = 0;

    for (i = r->n - 1; i >= 0; i--) {
        int32_t y = r->rest[i][1];
        r->cls[i] = y >= 0x3830 ? (y >= 0x6720 ? 2 : 1) : 0;
    }
    for (g = 0; g < 3; g++) {
        r->start[g] = at;
        for (i = r->n - 1; i >= 0; i--)
            if (r->cls[i] == g) {
                sid[at] = r->id[i];
                memcpy(srest[at], r->rest[i], sizeof srest[at]);
                at++;
            }
        r->count[g] = at - r->start[g];
    }
    memcpy(r->id, sid, (size_t)r->n * sizeof r->id[0]);
    memcpy(r->rest, srest, (size_t)r->n * sizeof r->rest[0]);
    /* the element is addressed [ebp + (i + start) * 12 * 4 + 0x65C],
     * FOUR vertices on, not one: the dword is the z of vertex 4 (i + start),
     * and past the 128 the next car's record (zeros) */
    best = (int32_t)0x80000001u;
    bi = 0;
    for (i = 0; i < r->count[0]; i++) {
        int32_t z = rec_word(r, 0x65Cu + 48u * (uint32_t)(i + r->start[0]));
        if (!(best > z)) { bi = i; best = z; }
    }
    r->hi = bi + r->start[0];
    best = 0x7FFFFFFF;
    bi = 0;
    for (i = 0; i < r->count[1]; i++) {
        int32_t z = rec_word(r, 0x65Cu + 48u * (uint32_t)(i + r->start[1]));
        if (!(best < z)) { bi = i; best = z; }
    }
    r->lo = bi + r->start[1];
}

int driver_load(driver *d, const ral_image *im, const ro3_model *m,
                int32_t carnum1, char *err, size_t errlen)
{
    /* the records' material words [0x00730E4A / 4E / 52 / 56] come from the
     * per-car tables at 0x00730EB6 / 0x00730F12 / 0x00730F6E / 0x00730FCA */
    static const uint32_t MAT[DRIVER_PARTS] = { 0x00730EB6u, 0x00730F12u,
                                                0x00730F6Eu, 0x00730FCAu };
    static const uint32_t BASE[DRIVER_PARTS] = { 0x00713C46u, 0x0071B0C6u,
                                                 0x00722546u, 0x007299C6u };
    /* the angles each record is given: +0x24, +0x2C, +0x28 */
    static const uint32_t ANG[DRIVER_PARTS] = { 0x007310C5u, 0x007310D1u,
                                                0x007310DDu, 0x007310E9u };
    const unsigned char *p;
    driver keep = *d;
    float axis[3], bx, s0, s1, s2;
    int c, i, g;

    memset(d, 0, sizeof *d);
    memcpy(d->w, keep.w, sizeof d->w);       /* a process loads them once   */
    d->have_table = keep.have_table;
    c = carnum1 == 6 ? 11 : carnum1;                 /* FUN_00454CEC        */
    if (c < 0 || c >= 23 || !m) {
        snprintf(err, errlen, "car %d has no cockpit-part row", (int)carnum1);
        return -1;
    }
    d->car = c;
#define RD(va_, n_) do { p = ral_image_at(im, (va_), (n_)); \
        if (!p) { snprintf(err, errlen, "0x%08X is not in the image", \
                           (unsigned)(va_)); return -1; } } while (0)
    RD(0x00730E5Au + 4u * (uint32_t)c, 4);
    d->swap = ral_image_i32(p);
    RD(0x00731099u, 4 * 11);
    for (i = 0; i < 11; i++) d->k[i] = rd_f32(p + 4 * i);
    RD(0x0071184Au, 4);
    d->walk = keep.seeded ? keep.walk : rd_f32(p);   /* the image's, then carried */
    d->seeded = 1;
    RD(0x007124AAu, 12);
    for (i = 0; i < 3; i++) axis[i] = rd_f32(p + 4 * i);
    RD(0x007124B6u, 4);
    bx = rd_f32(p);
    RD(0x006CF49Eu, 12);
    s0 = rd_f32(p); s1 = rd_f32(p + 4); s2 = rd_f32(p + 8);
    for (g = 0; g < DRIVER_PARTS; g++) {
        driver_part *r = &d->p[g];
        RD(MAT[g] + 4u * (uint32_t)c, 4);
        collect(r, m, (uint32_t)ral_image_i32(p));
        origin_of(r, axis, bx, s0, s1, s2);
        if (g == 1 || g == 2) {          /* the hands: the wheel's origin, +0x20 */
            memcpy(r->origin, d->p[0].origin, sizeof r->origin);
            r->ang[0] = d->p[0].ang[0];
        }
        to_origin(r);
        RD(ANG[g], 12);
        r->ang[1] = rd_f32(p);                       /* +0x24                 */
        r->ang2 = rd_f32(p + 4);                     /* +0x2C                 */
        r->ang[2] = rd_f32(p + 8);                   /* +0x28                 */
        turn_in(r);
        if (g == 1 || g == 2) split_hand(r);
        if (g == 1 || g == 2) {          /* +0x44..+0x4C: the image's (a record's
                                          * static initial words) */
            RD(BASE[g] + 0x44u, 12);
            for (i = 0; i < 3; i++) r->c[i] = ral_image_i32(p + 4 * i);
        }
        if (g == 2) {                                /* 0x004A989A            */
            int32_t cc[3];
            RD(0x00711852u, 12);
            for (i = 0; i < 3; i++) cc[i] = ral_image_i32(p + 4 * i);
            for (i = 0; i < 3; i++)
                r->c[i] = (int32_t)((uint32_t)cc[i]
                                    + (uint32_t)r->rest[r->hi][i]);
        }
    }
#undef RD
    d->anim = 0;
    d->have = d->p[0].n > 0 || d->p[1].n > 0 || d->p[2].n > 0 || d->p[3].n > 0;
    return 0;
}

void driver_snapshot(driver *d, int32_t steer_raw, float shake_h, int hold,
                     int32_t *car_latch)
{
    if (*car_latch) d->blk.latch = *car_latch;       /* 0x00454779            */
    *car_latch = 0;
    d->blk.hold = hold != 0;                         /* 0x00454787            */
    d->blk.steer = (float)((ext)steer_raw / (ext)0x20000000 + (ext)shake_h);
}

static void put(ro3_model *m, int32_t id, const int32_t t[3], const int32_t o[3])
{
    ro3_vec *q = &m->points[id];
    q->x = (int32_t)((uint32_t)t[0] + (uint32_t)o[0]) / 65536.0;
    q->y = (int32_t)((uint32_t)t[1] + (uint32_t)o[1]) / 65536.0;
    q->z = (int32_t)((uint32_t)t[2] + (uint32_t)o[2]) / 65536.0;
}

/* 0x004A9CBC: every vertex through the part's matrix, plus the origin */
static void pose_whole(const driver_part *r, ro3_model *m)
{
    float mm[9];
    int i;
    euler(mm, (ext)r->ang[0], (ext)r->ang[1], (ext)r->ang[2]);
    for (i = 0; i < r->n; i++) {
        int32_t t[3];
        xform(t, mm, r->rest[i]);
        put(m, r->id[i], t, r->origin);
    }
}

/* 0x004AA254 on the forearm's posed points: the three points (the hand's
 * highest vertex, the same with the target's y, the target) blended by the
 * animation's weights, the points moved by what that is from the first */
static void blend(const driver *d, const driver_part *r, int32_t t[][3],
                  int count)
{
    int32_t pt[3], bt[3], ct[3], delta[3];
    const float *w = d->w[d->anim & 0xFF];
    int k, i;

    if (d->hand == d->swap) return;
    for (k = 0; k < 3; k++) pt[k] = r->rest[r->hi][k];
    ct[0] = d->hand == 1 ? 0x2EE0 : -0x3E80;
    ct[1] = r->c[1];
    ct[2] = r->c[2];
    bt[0] = ct[0];
    bt[1] = pt[1];
    bt[2] = ct[2];
    for (k = 0; k < 3; k++)                          /* 0x004AA1F0            */
        delta[k] = fistp((ext)pt[k] * w[2]
                         + ((ext)bt[k] * w[1] + (ext)ct[k] * w[0]));
    for (i = 0; i < count; i++)
        for (k = 0; k < 3; k++)
            t[i][k] = (int32_t)((uint32_t)t[i][k] - (uint32_t)pt[k]
                                + (uint32_t)delta[k]);
}

/* 0x004A9D7A: the forearm by the first angle (and the lever), the wrist by
 * the second */
static void pose_hand(const driver *d, const driver_part *r, ro3_model *m)
{
    int32_t t[DRIVER_VERTS][3];
    float mm[9];
    int s, i;

    for (s = 0; s < 2; s++) {
        euler(mm, (ext)r->ang[0], (ext)(s ? r->ang2 : r->ang[1]),
              (ext)r->ang[2]);
        for (i = 0; i < r->count[s]; i++)
            xform(t[i], mm, r->rest[r->start[s] + i]);
        if (s == 0) blend(d, r, t, r->count[0]);
        for (i = 0; i < r->count[s]; i++)
            put(m, r->id[r->start[s] + i], t[i], r->origin);
    }
}

void driver_step(driver *d, ro3_model *m, int32_t dt, uint32_t *gen)
{
    driver_block *b = &d->blk;
    int32_t h = (int32_t)((uint32_t)dt >> 1);
    ext walk, a;
    int32_t iv;
    float s;

    if (!d->have || !m || !d->have_table) return;
    /* 0x004A9398: the gear hand's animation */
    if (b->hold) {
        b->latch = 0;
        if (d->anim < 0x80) {
            d->anim += h;
            if (d->anim > 0x80) d->anim = 0x80;
        } else if (d->anim > 0x80) {
            d->anim -= h;
            if (d->anim < 0x80) d->anim = 0x80;
        }
    } else if (b->latch) {
        d->anim += h;
        if (d->anim > 0xFF) { b->latch = 0; d->anim = 0; }
    } else if (d->anim != 0) {
        if (d->anim >= 0x80) {                       /* equal falls into add  */
            d->anim += h;
            if (d->anim > 0xFF) d->anim = 0;
        } else {
            d->anim -= h;
            if (d->anim < 1) d->anim = 0;
        }
    }
    /* the random walk: one draw from the shared generator */
    *gen = *gen * 0x9Du + 0x47u;
    iv = (int32_t)(*gen & 0xFFu) - 0x80;
    walk = (ext)d->walk + (ext)iv / (ext)d->k[0] * (ext)d->k[1];
    d->walk = (float)walk;
    d->wobble = (float)(ext_sin(walk) * (ext)d->k[2]);
    /* 0x004A950B: the angles, then the poses */
    a = (ext)b->steer * (ext)d->k[3];
    d->p[0].ang[1] = (float)a;
    d->p[1].ang[1] = (float)a;
    d->p[2].ang[1] = (float)a;
    d->p[3].ang[1] = (float)(a * (ext)d->k[4]);
    pose_whole(&d->p[0], m);
    pose_whole(&d->p[3], m);
    s = d->p[1].ang[1];
    d->p[1].ang2 = (float)((ext)s * (s < d->k[5] ? d->k[7] : d->k[6]));
    d->hand = 0;
    pose_hand(d, &d->p[1], m);
    s = d->p[2].ang[1];
    d->p[2].ang2 = (float)((ext)s * (s > d->k[8] ? d->k[10] : d->k[9]));
    d->hand = 1;
    pose_hand(d, &d->p[2], m);
    d->steps++;
}
