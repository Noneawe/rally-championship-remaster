/* objects.c - FUN_0045BCE8, car-versus-object collision, and the load-time
 * tables it reads.
 *
 * is the reading and section 12 is the
 * specification this file implements; every block below names the addresses
 * it transcribes.  Read that document before changing anything here: several
 * choices below look wrong and are the original's -
 *
 *  THE CELL-WALK BASE IS OVERWRITTEN MID-WALK.  A solid hit refreshes the
 *     ground (FUN_0043D66C), whose per-point driver FUN_0045AE5D stores every
 *     queried point's world x/z into [0x005EC6E4]/[0x005EC6EC] - the same two
 *     globals FUN_0045BCEE walks its nine cells from.  So after a solid hit
 *     the REMAINING cells are centred on the car's last collision point, not
 *     on the car.  0x0045AF6D/0x0045AF79.
 *  THE HIT BOX IS ASYMMETRIC AND HALF-OPEN: -0.9 <= x < 0.9 and
 *     -1.6 <= z < 1.9 in the heading frame (CMP / JNS / JS on 0xE666,
 *     0xFFFF199A, 0x1E666, 0xFFFE6667).
 *  COLLISION IGNORES OBJSINFO's TRANSFORM MODE.  FUN_0045BE2A always builds
 *     the yaw-and-first-tilt matrix, even for the camera-facing modes the draw
 *     path honours, and never the second angle triple.
 *  C535 SETS NO LATCH FOR CLASSES 5, 6, 7 AND 10 and C65B never removes
 *     them, so every hitting pass appends another list entry; at 256 the list
 *     is full until raceinit_q resets it.
 *
 *
 * FUN_0045C65B, the per-tick animator that consumes the hit list, is at the
 * end of this file.  Two
 * of its choices also look wrong and are the original's -
 *
 *  A REMOVAL SKIPS THE ENTRY IT SWAPS IN.  The last entry moves into the
 *     removed slot and the loop goes straight to INC ECX, so that entry is
 *     not animated this tick (0x0045C8C3 JMP 0x0045C89A).
 *  CLASSES 2, 3 AND 8 LEAVE THEIR LATCH SET WHEN REMOVED.  Only 1/9 clear
 *     +0x11, and class 4 and type 0x3B remove when it counts to zero, so a
 *     tilted or fallen object stays latched and C535 never records it again.
 *
 * WHAT IS NOT HERE: FUN_0045A8B8, the sound/feedback request - counted, not
 * modelled - and FUN_0045FBAF, C51F's second call (the detached parts).
*/

#include "physics.h"
#include "collision_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float st(double v) { return (float)v; }

static int32_t rd32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t ru32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

/* FISTP, round to nearest even; NaN / out of range is the integer
 * indefinite.  physics.c's is static; this is the same rule. */
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

/* FILD a / FMUL ST0 / FILD b / FMUL ST0 / FADDP / FSQRT / FISTP, for int32
 * a and b.  The squares and their sum are exact at the x87's 64 bits (each
 * square < 2^62, the sum < 2^63), so the only rounding before the FISTP is
 * FSQRT's.  Below 2^58 that rounding cannot move the result across a
 * half-integer (the gap from sqrt(s) to the nearest k + 1/2 is at least
 * 1/(8 sqrt(s)), more than the FSQRT's half-ulp), so the answer is exactly
 * the integer nearest sqrt(s) and is computed that way, identically on every
 * host.  Above it the port falls back to long double: x87-exact on x86-64,
 * a closer approximation than the original on aarch64. */
static int32_t fistp_hypot(int32_t a, int32_t b)
{
    uint64_t s = (uint64_t)((int64_t)a * a) + (uint64_t)((int64_t)b * b);
    uint64_t k;

    if (s >= ((uint64_t)1 << 58))
        return fistp((double)sqrtl((long double)s));
    k = (uint64_t)sqrt((double)s);
    while (k * k > s) k--;
    while ((k + 1) * (k + 1) <= s) k++;
    /* sqrt(s) >= k + 1/2  <=>  s >= k^2 + k + 1/4  <=>  s > k^2 + k. */
    if (s > k * k + k) k++;
    return (int32_t)k;
}

/* ======================================================================== */
/* The tables - FUN_004252A5's bracket.       */
/* ======================================================================== */

void sim_objects_init(sim_objects *o)
{
    int t;
    memset(o, 0, sizeof(*o));
    for (t = 0; t < SIM_OBJECT_TYPES; t++) o->type[t].model = -1;
}

void sim_objects_free(sim_objects *o)
{
    free(o->points);
    o->points = NULL;
    o->npoints = o->cap = 0;
}

/* FUN_0042566A.  0x0042566A: CMP [0x005E7E78],0x190 / JB ; CMP ...,0x258 /
 * JAE - unsigned, so exactly the files\b band, 400..599.  Then PUSH ECX /
 * MOV ECX,EAX / NEG ECX / SUB EBX,[0x005C6CF8] / POP EAX:
 * (x, y, z) -> (z, y - 0.7, -x). */
static int model_turns(int32_t model)
{
    return (uint32_t)model >= 0x190u && (uint32_t)model < 0x258u;
}

static void model_point_turn(const sim_consts *k, int32_t *x, int32_t *y,
                             int32_t *z)
{
    int32_t nx = *z, nz = (int32_t)(0u - (uint32_t)*x);
    *y = (int32_t)((uint32_t)*y - (uint32_t)k->car_model_y_q);
    *x = nx;
    *z = nz;
}

int32_t sim_car_collision_model_q(const sim_consts *k, int32_t carnum)
{
    /* FUN_0046102B: clamp to [0, 0x17] (JNS / JS), then the table.  The
     * caller, 0x0042486E, stores that + 100 as type 0x5B + 0x14 * car. */
    if (carnum < 0) carnum = 0;
    if (carnum > 0x17) carnum = 0x17;
    return k->car_model[carnum] + 100;
}

int sim_car_collision_points(sim_collision_points *out, const sim_consts *k,
                             int32_t model, int32_t (*pts)[3], int32_t npts)
{
    int32_t i;
    if (!out || !k || !pts || npts < 0) return -1;
    /* FUN_004252A5 on type 0x5B + 0x14 * car: sub-type 0x0B takes the
     * 0x00424A21 branch (mask all), and 0x00425397 keeps every point, in file
     * order, each through FUN_0042566A. */
    if (model_turns(model))
        for (i = 0; i < npts; i++)
            model_point_turn(k, &pts[i][0], &pts[i][1], &pts[i][2]);
    if (sim_collision_points_select(pts, npts) != 0) return -1;
    return sim_collision_points_build(out, (const int32_t (*)[3])pts, npts);
}

int sim_objects_set_type(sim_objects *o, const sim_consts *k, int type,
                         int32_t model, int32_t cls,
                         const int32_t (*pts)[3], int32_t npts)
{
    sim_object_type *t;
    int32_t i, radius = 0;
    int turn = model_turns(model);

    if (!o || !k || type < 0 || type >= SIM_OBJECT_TYPES || npts < 0
        || (npts > 0 && !pts))
        return -1;
    /* 0x0042538D: below 0x50 every point is kept.  Above it the polygon mask
     * [0x005D4784] decides, and BCE8 never tests such a type. */
    if (type >= 0x50) return -1;

    if (o->npoints + npts > o->cap) {
        int32_t cap = o->cap ? o->cap : 256;
        int32_t (*grown)[3];
        while (cap < o->npoints + npts) cap *= 2;
        grown = (int32_t (*)[3])realloc(o->points, (size_t)cap * sizeof(*grown));
        if (!grown) return -2;
        o->points = grown;
        o->cap = cap;
    }

    t = &o->type[type];
    for (i = 0; i < npts; i++) {
        int32_t x = pts[i][0], y = pts[i][1], z = pts[i][2], r;
        if (x < -(1 << 28) || x > (1 << 28) || z < -(1 << 28) || z > (1 << 28))
            return -1;
        if (turn) model_point_turn(k, &x, &y, &z);
        o->points[o->npoints + i][0] = x;
        o->points[o->npoints + i][1] = y;
        o->points[o->npoints + i][2] = z;
        /* FUN_0042568F on the TRANSFORMED x and z; CMP r,[0x00581B5C] / JS
         * keep: the sign of r - max, so ties store the same value. */
        r = fistp_hypot(x, z);
        if ((int32_t)((uint32_t)r - (uint32_t)radius) >= 0) radius = r;
    }
    t->model = model;
    t->cls = cls;
    t->start = o->npoints;     /* 0x00425430 */
    t->count = npts;           /* 0x004254C4 (and again at 0x004255E4) */
    t->radius = radius;        /* 0x004254B2 */
    o->npoints += npts;
    return 0;
}

void sim_objects_set_unloaded(sim_objects *o, int type, int32_t texinfo_cls)
{
    if (!o || type < 0 || type >= SIM_OBJECT_TYPES) return;
    /* FUN_0042490F: [0x00581FC4] = -1, start and count cleared.  The radius
     * table is NOT cleared by it (two references image-wide); a stale radius
     * only gates C2AD, which with a zero count cannot hit, so zero is exact. */
    o->type[type].model = -1;
    o->type[type].cls = texinfo_cls;
    o->type[type].start = 0;
    o->type[type].count = 0;
    o->type[type].radius = 0;
}

void sim_object_state_init(sim_object_state *s, uint8_t *pols, int32_t npols,
                           uint32_t rnd_seed)
{
    memset(s, 0, sizeof(*s));
    s->pols = pols;
    s->npols = npols;
    s->rnd = rnd_seed;
}

/* rnd_next 0x004018A0 -> rnd_lfsr_step 0x004018B4 with EBX = 0. */
uint32_t sim_rnd_next_q(uint32_t *state)
{
    uint32_t eax = *state;
    uint32_t al = eax & 0xFFu;

    al ^= (eax & 0x400000u) ? 1u : 0u;   /* TEST EAX,0x400000 / SETNZ / XOR */
    al ^= (al & 4u) ? 1u : 0u;           /* TEST AL,4                       */
    al ^= (al & 2u) ? 1u : 0u;           /* TEST AL,2                       */
    eax = (eax & ~0xFFu) | (al & 0xFFu);
    eax = (eax >> 1) | (eax << 31);      /* ROR EAX,1 ; XOR EAX,EBX (0)     */
    *state = eax;
    return eax;
}

/* FUN_0043F400 through FUN_0043F3E0: the same step as rnd_next with
 * EBX = 0x87654321 instead of 0 (0x0043F41B), on the car's own word, car+0x0C.
 * FUN_00439B37 makes this its first call. */
uint32_t sim_car_rnd_next_q(uint32_t *state)
{
    (void)sim_rnd_next_q(state);        /* the step, EBX = 0                 */
    *state ^= 0x87654321u;              /* XOR EAX,0x87654321                */
    return *state;
}

/* ======================================================================== */
/* The walk.                                                                */
/* ======================================================================== */

typedef struct bce8 {
    car_state                    *car;
    const sim_consts             *k;
    const sim_ground             *g;
    const float                  *sin;
    sim_world                    *w;
    const sim_collision_snapshot *snap;
    const sim_objects            *o;
    sim_object_state             *s;

    int32_t  base_x, base_z;     /* [0x005EC6E4], [0x005EC6EC]               */
    int32_t  cur;                /* [0x00685F3C]  the instance                */
    int32_t  type;               /* [0x00685F40]                              */
    int32_t  cls;                /* [0x00686348]                              */
    uint32_t yaw, tilt, tilt_axis, scale; /* [0x5E7ECC/ED0/ED4/EC8]          */
    int32_t  off[3];             /* [0x00686864..6C]  the point's offset      */
    int32_t  p[3];               /* [0x00686870..78]  world-relative, 16.16   */
    float    pf[3];              /* [0x0068689C..A4]  the same / 65536        */
    int32_t  l[3];               /* [0x00685DB4..BC]  heading frame, 16.16    */
    int      hits;
} bce8;

static int is_one_of(int32_t v, const int32_t *set, int n)
{
    int i;
    for (i = 0; i < n; i++) if (v == set[i]) return 1;
    return 0;
}

/* FUN_0043D5FA -> 0x0041EBED -> 0x0041EC71, on floats: EDX = -[0x006CD624];
 * i = (EDX + 0x80000) >>u 20; x' = x c + z s, z' = z c - x s, each one FSTP.
 * The x87 model makes the two products and their sum exact before the
 * rounding, as the FPU does. */
static void heading_rotate(const float *sin_table, int32_t heading,
                           float *x, float *z)
{
    uint32_t i = ((0u - (uint32_t)heading) + 0x80000u) >> 20;
    sim_x87 s = sim_x87_from_float(sin_table[i]);
    sim_x87 c = sim_x87_from_float(sin_table[i + SIM_SIN_QUARTER]);
    sim_x87 xx = sim_x87_from_float(*x), zz = sim_x87_from_float(*z);

    *x = sim_x87_to_float(sim_x87_add(sim_x87_mul(xx, c), sim_x87_mul(zz, s)));
    *z = sim_x87_to_float(sim_x87_sub(sim_x87_mul(zz, c), sim_x87_mul(xx, s)));
}

/* FUN_0045C358: one point against the car.  `pnt` is the anchor. */
static int test_point(bce8 *b, const uint8_t *rec, const uint8_t *pnt)
{
    static const int32_t writes[4] = { 10, 1, 5, 6 };
    car_state *car = b->car;
    float rx, rz;
    int a;

    b->s->tests++;                                   /* [0x005C6E48]++ */
    for (a = 0; a < 3; a++) {
        b->p[a] = (int32_t)((uint32_t)rd32(pnt + 4 * a)
                            + (uint32_t)rd32(rec + 0x30 + 4 * a)
                            + (uint32_t)b->off[a] - (uint32_t)car->pos[a]);
        /* FILD / FDIV [0x0057B01C] / FSTP: exact division, one rounding. */
        b->pf[a] = st((double)b->p[a] / (double)b->k->pos_scale);
    }
    rx = b->pf[0];
    rz = b->pf[2];
    heading_rotate(b->sin, b->w->binary_angle[0], &rx, &rz);
    /* FLD / FMUL 65536.0 / FISTP - exact scale, one rounding. */
    b->l[0] = fistp((double)rx * (double)b->k->pos_scale);
    b->l[1] = fistp((double)b->pf[1] * (double)b->k->pos_scale);
    b->l[2] = fistp((double)rz * (double)b->k->pos_scale);

    /* 0x0045C43C..0x0045C476: CMP then JNS / JS - the SIGN of the
     * difference, so the upper bounds are exclusive and the lower bounds
     * inclusive. */
    if ((int32_t)((uint32_t)b->l[0] - 0x0000E666u) >= 0) return 0;
    if ((int32_t)((uint32_t)b->l[0] - 0xFFFF199Au) < 0)  return 0;
    if ((int32_t)((uint32_t)b->l[2] - 0x0001E666u) >= 0) return 0;
    if ((int32_t)((uint32_t)b->l[2] - 0xFFFE6667u) < 0)  return 0;

    if (is_one_of(b->cls, writes, 4)) {
        float len, mag;
        /* 0x0045C4A2: car+0x224..0x22C = (x, 0, z) / 65536. */
        car->collision_scratch_224[0] = st((double)b->l[0] / (double)b->k->pos_scale);
        car->collision_scratch_224[1] = 0.0f;
        car->collision_scratch_224[2] = st((double)b->l[2] / (double)b->k->pos_scale);
        /* FUN_0041FFFB on newspd, FSTP [0x00685E2C]; FABS / FCOMP peak /
         * JB skip - C0 is below OR unordered. */
        len = sim_vec3_len_q(car->newspd);
        mag = fabsf(len);
        if (mag >= car->collision_peak_normal_speed) {
            car->collision_peak_normal_speed = mag;
            b->w->collision_peak_normal_speed = mag;
        } else {
            b->w->collision_peak_normal_speed = car->collision_peak_normal_speed;
        }
        car->collision_active = 1;
        car->collision_response_timer = 0x32;
        b->w->collision_active = 1;
        b->w->collision_response_timer = 0x32;
    }
    return 1;                                        /* STC */
}

/* FUN_0045BE2A: the object's matrix, into [0x0059F18C]. */
static void object_matrix(const bce8 *b, float m[16])
{
    float r[16], a[16], c[16];

    /* FUN_00425C3A (about y) and FUN_00425BF1 (about x) write four slots of
     * a matrix whose other twelve are the .data identity (objxform.c).  The
     * angle goes through AND 0xFFFF / SHR 4 into the table. */
#define ROT(mat, ang, isx) do {                                             \
        uint32_t _i = ((uint32_t)(ang) & 0xFFFFu) >> 4;                     \
        float _s = b->sin[_i], _c = b->sin[_i + SIM_SIN_QUARTER];            \
        memset((mat), 0, 16 * sizeof(float));                               \
        (mat)[0] = (mat)[5] = (mat)[10] = (mat)[15] = 1.0f;                 \
        if (isx) { (mat)[5] = _c; (mat)[6] = _s; (mat)[9] = -_s; (mat)[10] = _c; } \
        else     { (mat)[0] = _c; (mat)[2] = -_s; (mat)[8] = _s; (mat)[10] = _c; } \
    } while (0)

    ROT(m, 0u - b->yaw, 0);                          /* NEG EDX / 425C3A     */
    if (b->tilt != 0) {                              /* CMP [0x5E7ED0],0     */
        ROT(r, 0u - b->tilt_axis, 0);
        sim_matrix_mul(m, r, a);                     /* -> 0x0059EE94        */
        ROT(r, 0u - b->tilt, 1);
        sim_matrix_mul(a, r, c);                     /* -> 0x0059EE54        */
        ROT(r, b->tilt_axis, 0);                     /* un-negated: closes it */
        sim_matrix_mul(c, r, m);                     /* -> 0x0059F18C        */
    }
#undef ROT
    {
        /* FILD scale / FLD1 FADD ST0 x4 / FDIVP / FSTP, then every one of the
         * sixteen slots FLD / FMUL / FSTP. */
        float s = st((double)b->scale / 16.0);
        int i;
        for (i = 0; i < 16; i++) m[i] = st((double)m[i] * s);
    }
}

/* FUN_0045C2AD: the type's collision points, through the object matrix. */
static int test_points(bce8 *b, const uint8_t *rec, const uint8_t *pnt)
{
    const sim_object_type *t = &b->o->type[b->type];
    float m[16];
    int32_t j;

    object_matrix(b, m);
    if (t->count == 0) return 0;                     /* CMP ECX,0 / JE: CLC  */
    if (t->start < 0 || t->count < 0 || t->start + t->count > b->o->npoints)
        return -1;
    for (j = 0; j < t->count; j++) {
        const int32_t *v = b->o->points[t->start + j];
        sim_x87 x = sim_x87_from_i64(v[0]);          /* FILD z, y, x         */
        sim_x87 y = sim_x87_from_i64(v[1]);
        sim_x87 z = sim_x87_from_i64(v[2]);
        int a;
        /* FUN_0044B1AB leaves (m0x + (m4y + m8z)) + m12 and its two twins
         * on the FPU; C2AD FISTPs them straight from there. */
        for (a = 0; a < 3; a++) {
            sim_x87 e = sim_x87_add(
                sim_x87_add(sim_x87_mul(sim_x87_from_float(m[a]), x),
                            sim_x87_add(sim_x87_mul(sim_x87_from_float(m[4 + a]), y),
                                        sim_x87_mul(sim_x87_from_float(m[8 + a]), z))),
                sim_x87_from_float(m[12 + a]));
            b->off[a] = sim_x87_fistp(e);
        }
        if (test_point(b, rec, pnt)) return 1;       /* STC */
    }
    return 0;                                        /* CLC */
}

/* FUN_0045C535: record the hit. */
static void record_hit(bce8 *b, int32_t bearing, int32_t speed16)
{
    sim_object_state *s = b->s;
    uint8_t *rec = s->pols + (size_t)b->cur * 0x40u;

    if (rec[0x11] != 0) return;                      /* already animating    */
    if (s->count == SIM_OBJECT_HITS) return;         /* the list is full     */
    s->list[s->count] = b->cur;                      /* before the class test */

    switch (b->cls) {
    case 9: case 1:
        /* FLD [car+0x1C4] / FCOMP [0x0057AFD4] (8.0) / C0 -> return, the
         * entry written but NOT counted. */
        if (!(b->car->speed_len >= b->k->object_speed_min)) return;
        rec[0x09] = 0; rec[0x11] = 1; rec[0x13] = 0;
        rec[0x12] = (uint8_t)bearing;
        break;
    case 2: case 3: case 8:
        rec[0x09] = 0; rec[0x11] = 1; rec[0x13] = 0;
        rec[0x12] = (uint8_t)bearing;
        break;
    case 4: {
        /* [0x00686898] >>u 14, clamped at 0x40 (JB) - about 4 x |newspd|. */
        uint32_t v = (uint32_t)speed16 >> 14;
        if (v >= 0x40u) v = 0x40u;
        rec[0x11] = (uint8_t)v; rec[0x13] = 0;
        rec[0x12] = (uint8_t)bearing;
        break;
    }
    case 0xFF:
        /* Type 0x3B: CALL rnd_next / AND 0xFF / CMP 3 / JAE return. */
        if ((sim_rnd_next_q(&s->rnd) & 0xFFu) >= 3u) return;
        rec[0x11] = 0x0F;
        s->count++;                                  /* no feedback request  */
        s->recorded_hits++;
        return;
    default:                                         /* 5, 6, 7, 10: no latch */
        break;
    }
    s->feedback_requests++;                          /* FUN_0045A8B8         */
    {
        /* the impact sound, by OBJSINFO +0x24 of the hit type, at
         * the car plus [0x00686870..78] */
        sim_sound_view sv;
        sim_sound_feed *sf = sim_sound_feed_local(b->w, &sv);
        if (sf && b->type >= 0 && b->type < SIM_OBJECT_TYPES)
            sim_sound_object_hit_q(b->car, sf, &sv,
                sim_sound_impact_by_material[b->o->type[b->type].material & 15],
                b->p);
    }
    s->count++;
    s->recorded_hits++;
}

/* 0x0045C07B: the response to a hit. */
static int respond(bce8 *b)
{
    static const int32_t solid[5] = { 6, 9, 5, 1, 10 };
    static const int32_t record_only[5] = { 2, 7, 3, 8, 4 };
    car_state *car = b->car;
    int32_t bearing, speed16;

    b->hits++;
    /* FILD P.x / FILD P.z / FPATAN / FMUL 256.0 / FMUL 1/2pi / FISTP, then
     * + 0x80 and & 0xFF: the bearing byte, [0x00686894]. */
    bearing = fistp(atan2((double)b->p[0], (double)b->p[2])
                    * (double)b->k->bearing_scale * (double)b->k->inv_two_pi);
    bearing = (int32_t)(((uint32_t)bearing + 0x80u) & 0xFFu);
    /* x^2 + (z^2 + y^2), FSQRT, FMUL 65536.0, FISTP: [0x00686898]. */
    speed16 = fistp(sqrt((double)car->newspd[0] * car->newspd[0]
                         + ((double)car->newspd[2] * car->newspd[2]
                            + (double)car->newspd[1] * car->newspd[1]))
                    * (double)b->k->pos_scale);

    if (is_one_of(b->cls, solid, 5)) {
        float n[3], r[3];
        double len;
        int rc, i;

        b->s->solid_hits++;
        sim_collision_rollback_q(car, b->g->state, b->w, b->snap); /* C100 */
        rc = sim_collision_refresh_ground_q(car, b->g, b->w);       /* D66C */
        if (rc != 0) return -1;
        if (car->numcollpts >= 1) {
            /* FUN_0045AE5D left the last point's world x/z in the walk
             * base - 0x0045AF6D. */
            b->base_x = b->g->state->ix[car->numcollpts - 1];
            b->base_z = b->g->state->iz[car->numcollpts - 1];
        }
        record_hit(b, bearing, speed16);                            /* C535 */

        /* FCHS / FSTP each newspd component; FUN_0041FFFB against 0.0 with
         * C3 (equal or unordered) -> FLDZ into all three; then y = 0 as an
         * integer MOV, and FUN_0041E9C6. */
        for (i = 0; i < 3; i++) n[i] = -car->newspd[i];
        len = sqrt((double)n[0] * n[0] + ((double)n[1] * n[1]
                                          + (double)n[2] * n[2]));
        if (len == 0.0 || len != len) n[0] = n[1] = n[2] = 0.0f;
        n[1] = 0.0f;
        (void)sim_normalise_q(n);
        /* r = [0x0068689C..A4] with y zeroed; mu = 0; [0x005C7874] = record 0. */
        r[0] = b->pf[0]; r[1] = 0.0f; r[2] = b->pf[2];
        if (sim_contact_impulse_q(car, b->k, b->w, n, r, 0.0f,
                                  &b->w->collision_force[0], NULL, NULL) != 0)
            return -1;
    } else if (is_one_of(b->cls, record_only, 5)) {
        record_hit(b, bearing, speed16);
    }
    return 0;
}

/* FUN_0045BF17: one pols record. */
static int test_instance(bce8 *b, uint32_t index)
{
    static const int32_t radius_classes[4] = { 9, 8, 6, 5 };
    const uint8_t *rec, *pnt;
    const sim_object_type *t;
    uint32_t anchor;
    uint8_t flags, variant;
    int hit;

    if (index >= (uint32_t)b->s->npols) return -1;   /* the original reads on */
    rec = b->s->pols + (size_t)index * 0x40u;
    b->cur = (int32_t)index;

    /* FUN_0045023B, on EVERY record's +1 byte: CF = 1 skips models 48 and 78
     * in arcade mode or with the countdown skip. */
    t = &b->o->type[rec[1]];
    if ((b->o->countdown_skip != 0 || b->o->gmode == 2)
        && (t->model == 0x30 || t->model == 0x4E))
        return 0;
    if (rec[0] != 0xFF) return 0;
    if (rec[1] == 0x3B) {                            /* record only, no test */
        b->cls = 0xFF;
        record_hit(b, 0, 0);
        return 0;
    }
    b->type = rec[1];
    b->cls = t->cls;                                 /* OBJSINFO +0x1C        */

    flags = rec[0x25];
    if (flags & 1u) return 0;
    variant = (uint8_t)((flags >> 1) & 3u);
    if (variant != 0 && (int32_t)(variant - 1) != b->o->route_variant) return 0;
    if (rec[1] != 0x3A && rec[1] >= 0x38) return 0;

    b->yaw       = ((uint32_t)rec[0x08] << 8) & 0xFFFFu;
    b->tilt      = ((uint32_t)rec[0x0C] << 8) & 0xFFFFu;
    b->tilt_axis = ((uint32_t)rec[0x10] << 8) & 0xFFFFu;
    b->scale     = rec[0x18];

    /* FUN_0045C317: the anchor point, zero offset.  It leaves ESI on the
     * anchor, which the radius test below reads. */
    anchor = ru32(rec + 0x04);
    if (!b->g->geom->pnts || anchor >= (uint32_t)b->g->geom->npnts) return -1;
    pnt = b->g->geom->pnts + (size_t)anchor * 0x0Cu;
    b->off[0] = b->off[1] = b->off[2] = 0;
    hit = test_point(b, rec, pnt);

    if (!hit) {
        int32_t dx, dz, d;
        uint32_t reach;
        if (!is_one_of(b->cls, radius_classes, 4)) return 0;
        dx = (int32_t)((uint32_t)rd32(pnt) + (uint32_t)rd32(rec + 0x30)
                       - (uint32_t)b->car->pos[0]);
        dz = (int32_t)((uint32_t)rd32(pnt + 8) + (uint32_t)rd32(rec + 0x38)
                       - (uint32_t)b->car->pos[2]);
        d = fistp_hypot(dx, dz);
        /* MUL [scale] / SHRD EAX,EDX,4 / ADD 0x28000 / CMP d / JS skip. */
        reach = (uint32_t)(((uint64_t)(uint32_t)b->o->type[b->type].radius
                            * (uint64_t)b->scale) >> 4) + 0x28000u;
        if ((int32_t)(reach - (uint32_t)d) < 0) return 0;
        hit = test_points(b, rec, pnt);
        if (hit < 0) return -1;
        if (!hit) return 0;
    }
    return respond(b);
}

int sim_object_collision_q(car_state *car, const sim_consts *k,
                           const sim_ground *ground, const float *sin_table,
                           sim_world *w,
                           const sim_collision_snapshot *snapshot)
{
    /* FUN_0045BCEE's nine cells: centre, then clockwise from (-32, -32). */
    static const int32_t cell[9][2] = {
        { 0, 0 }, { -0x200000, -0x200000 }, { 0, -0x200000 },
        { 0x200000, -0x200000 }, { 0x200000, 0 }, { 0x200000, 0x200000 },
        { 0, 0x200000 }, { -0x200000, 0x200000 }, { -0x200000, 0 }
    };
    bce8 b;
    int c;

    if (!car || !k || !ground || !sin_table || !w || !snapshot) return -1;
    if (!ground->objects && !ground->object_state) return 0;
    if (!ground->objects || !ground->object_state || !ground->geom
        || !ground->state || !ground->object_state->pols
        || ground->object_state->npols != ground->geom->npols)
        return -1;

    memset(&b, 0, sizeof(b));
    b.car = car; b.k = k; b.g = ground; b.sin = sin_table; b.w = w;
    b.snap = snapshot; b.o = ground->objects; b.s = ground->object_state;
    b.base_x = car->pos[0];                          /* [0x006CCD98]          */
    b.base_z = car->pos[2];                          /* [0x006CCDA0]          */

    for (c = 0; c < 9; c++) {
        /* FUN_0045BDE9: re-read the base for every cell. */
        uint32_t x = (uint32_t)b.base_x + (uint32_t)cell[c][0];
        uint32_t z = (uint32_t)b.base_z + (uint32_t)cell[c][1];
        uint32_t key = (x >> 21) + ((z >> 21) << 11);
        int32_t rec = sim_ground_cell_lookup(ground->geom, key);
        const uint8_t *zube;
        uint32_t first, last, p;

        if (rec < 0) continue;                       /* JB: CF = 1, no cell   */
        if (rec >= ground->geom->nzube) return -1;
        zube = ground->geom->zube + (size_t)rec * 0x20u;
        first = ru32(zube + 0x08);
        last = ru32(zube + 0x0C);
        if (first > last || first == 0xFFFFFFFFu) continue;  /* JA, unsigned */
        for (p = first; ; p++) {
            if (test_instance(&b, p) < 0) return -1;
            if (p >= last) break;                    /* INC / CMP / JBE       */
        }
    }
    return b.hits;
}

/* ======================================================================== */
/* The tick - FUN_0045C65B.                 */
/* ======================================================================== */

/* [0x00684B20]: 24 signed bytes, one reference (0x0045C787), no writer. */
static const int8_t wobble[24] = {
    -1, -2, -3, -2, -1, 0, 1, 2, 3, 2, 1, 0,
    -1, -2, -1, 0, 1, 2, 1, 0, -1, 0, 1, 0
};

/* 0x0041EA8B with EAX = 0, ECX = z: FILD / FSTP each to float, the table
 * rotation 0x0041EC71 (x' = x c + z s, z' = z c - x s, each FSTP), then
 * FLD / FISTP each back to int.  EBX's round trip is dropped: C65B does not
 * read it. */
static void table_rotate_int(const float *sin_table, uint32_t angle,
                             int32_t z, int32_t *xo, int32_t *zo)
{
    uint32_t i = (angle + 0x80000u) >> 20;
    sim_x87 s = sim_x87_from_float(sin_table[i]);
    sim_x87 c = sim_x87_from_float(sin_table[i + SIM_SIN_QUARTER]);
    sim_x87 xx = sim_x87_from_float(0.0f);
    sim_x87 zz = sim_x87_from_float(st((double)z));
    float x1 = sim_x87_to_float(sim_x87_add(sim_x87_mul(xx, c), sim_x87_mul(zz, s)));
    float z1 = sim_x87_to_float(sim_x87_sub(sim_x87_mul(zz, c), sim_x87_mul(xx, s)));

    *xo = fistp((double)x1);
    *zo = fistp((double)z1);
}

int sim_object_tick_q(const sim_objects *o, sim_object_state *s,
                      const sim_ground_geom *geom, const float *sin_table,
                      const int32_t car0_pos[3], int32_t car0_hint,
                      uint32_t mask)
{
    int32_t i;

    if (!o || !s || !geom || !sin_table || !car0_pos || !s->pols
        || s->npols != geom->npols || !geom->pnts)
        return -1;
    if (s->count < 0 || s->count > SIM_OBJECT_HITS) return -1;
    if (s->count == 0) return 0;                     /* CMP 0 / JE: RET       */

    i = 0;
    do {                                             /* 0x0045C66D            */
        int32_t idx = s->list[i];
        uint8_t *rec;
        int remove = 0;

        if (idx < 0 || idx >= s->npols) return -1;   /* the original reads on */
        rec = s->pols + (size_t)idx * 0x40u;

        if (rec[0x01] == 0x3B) {
            /* DEC byte / JNE: counts the 0x0F C535 set down to zero. */
            if (--rec[0x11] == 0) remove = 1;
        } else {
            uint32_t anchor = ru32(rec + 0x04);
            const uint8_t *pnt;
            int32_t d, cls;
            uint32_t k;

            if (anchor >= (uint32_t)geom->npnts) return -1;
            pnt = geom->pnts + (size_t)anchor * 0x0Cu;
            /* SUB / FILD / FMUL ST0 twice / FADDP / FSQRT / FISTP, to car[0]
             * - the roster slot, not the car that hit it. */
            d = fistp_hypot((int32_t)((uint32_t)rd32(pnt) - (uint32_t)car0_pos[0]),
                            (int32_t)((uint32_t)rd32(pnt + 8) - (uint32_t)car0_pos[2]));
            /* CMP 0x20000 / JA (unsigned): far is 1; else ((0x20000 - d)
             * >>u 13) + 1, 1..17.  [0x00685D64]. */
            if ((uint32_t)d > 0x20000u) k = 1;
            else k = ((0x20000u - (uint32_t)d) >> 13) + 1u;

            /* MOVZX byte OBJSINFO[model] + 0x1C - BF17's read. */
            cls = (int32_t)(uint8_t)o->type[rec[0x01]].cls;
            switch (cls) {
            case 9: case 1:
                /* INC / CMP 0x18 / JB: the wobble; at 0x18 clear and go. */
                rec[0x11]++;
                if (rec[0x11] >= 0x18) {
                    rec[0x11] = 0;
                    rec[0x13] = 0;
                    remove = 1;
                } else {
                    rec[0x13] = (uint8_t)wobble[rec[0x11]];
                }
                break;
            case 2: {
                /* k times: at 0x30 remove, else INC - tested BEFORE each
                 * increment, so reaching 0x30 on the last one removes next
                 * tick. */
                uint32_t n = k;
                do {
                    if (rec[0x13] == 0x30) { remove = 1; break; }
                    rec[0x13]++;
                } while (--n != 0);
                break;
            }
            case 3: case 8: {
                /* SHL EDX,2: 4k turning sub-steps toward (+9 + +8 + +0x12)
                 * & 0x7F == 0; there the object falls, k fresh sub-steps
                 * (MOV EDX,[0x685D64] again) of +0x13 up to 0x40. */
                uint32_t n = k << 2;
                do {
                    uint8_t al = (uint8_t)((rec[0x09] + rec[0x08] + rec[0x12]) & 0x7Fu);
                    if (al == 0) {
                        uint32_t f = k;
                        do {
                            if (rec[0x13] == 0x40) { remove = 1; break; }
                            rec[0x13]++;
                        } while (--f != 0);
                        break;
                    }
                    rec[0x09] = (uint8_t)(rec[0x09] - 1u);   /* SUB 1          */
                    if (al >= 0x40) rec[0x09] = (uint8_t)(rec[0x09] + 2u); /* JB */
                } while (--n != 0);
                break;
            }
            case 4: {
                int32_t dx, dz, sx, sz;
                sim_ground_hit hit;
                int rc;

                if (rec[0x11] == 0) { remove = 1; break; }
                /* ECX = +0x11 << 8, EAX = 0, EDX = (+0x12 + 0x80) << 24. */
                table_rotate_int(sin_table,
                                 ((uint32_t)rec[0x12] + 0x80u) << 24,
                                 (int32_t)((uint32_t)rec[0x11] << 8), &dx, &dz);
                sx = rd32(rec + 0x30);                  /* [0x00685D50]         */
                sz = rd32(rec + 0x38);                  /* [0x00685D54]         */
                wr32(rec + 0x30, (uint32_t)sx + (uint32_t)dx);
                wr32(rec + 0x38, (uint32_t)sz + (uint32_t)dz);
                /* [0x005EC6E4/EC] = the moved anchor; [0x005819C0] = car[0]'s
                 * point-4 polygon; the mask as left; CALL 0x00440293. */
                rc = sim_ground_query(geom,
                                      (int32_t)((uint32_t)rd32(pnt) + (uint32_t)rd32(rec + 0x30)),
                                      (int32_t)((uint32_t)rd32(pnt + 8) + (uint32_t)rd32(rec + 0x38)),
                                      car0_hint, mask, &hit);
                if (rc < 0) return -1;
                if (rc == 1) s->ground_height = hit.height;
                /* +0x34 = [0x005EC6E8] - pnt.y, found or NOT: stale on a miss. */
                wr32(rec + 0x34, (uint32_t)s->ground_height - (uint32_t)rd32(pnt + 4));
                if (rc == 0) {                          /* [0x005819BC] == -1   */
                    wr32(rec + 0x30, (uint32_t)sx);
                    wr32(rec + 0x38, (uint32_t)sz);
                }
                rec[0x11]--;                            /* found or not         */
                break;
            }
            default:                                     /* 5, 6, 7, 10, other  */
                break;
            }
        }

        if (remove) {
            /* 0x0045C8A8: the last entry into this slot, count down, then
             * JMP to the INC - the moved entry waits for the next tick. */
            s->list[i] = s->list[s->count - 1];
            s->count--;
        }
        i++;
    } while (i < s->count);                          /* CMP / JB              */
    return 0;
}

void sim_object_reset_q(sim_object_state *s)
{
    int32_t i;

    if (!s || !s->pols) return;
    for (i = 0; i < s->npols; i++) {
        uint8_t *rec = s->pols + (size_t)i * 0x40u;
        if (rec[0x00] != 0xFF) continue;
        memset(rec + 0x30, 0, 12);                   /* +0x30, +0x34, +0x38   */
        rec[0x11] = rec[0x12] = rec[0x13] = 0;
    }
    s->count = 0;                                    /* 0x0045C52A            */
}
