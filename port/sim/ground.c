/* ground.c - the wheel-to-ground query.  See ground.h.
 *
 * SOURCE
 * ------
 * Read in full from disasm.tsv and, where Ghidra has no function, from the
 * image (tools/ralimg.py disasm) -:
 *
 *   FUN_0045B76A    44 b   the step's caller, and the snapshot
 *   FUN_0045AE5D   982 b   the per-point driver
 *   FUN_00440293     6 b   CALL FUN_00440299 / RET - rule 15's shape exactly
 *   FUN_00440299   308 b   hint, then the 3 x 3 cell search
 *   FUN_00440276    29 b   the hint test
 *   FUN_00440424    31 b   cell key; FUN_00420DA8 (65 b) the bucketed lookup
 *   FUN_00440443    45 b   the polygons of one cell, in index order
 *   FUN_00440470   797 b   one polygon: one triangle or two, first hit wins
 *   FUN_00440A59    92 b   the edge test
 *   FUN_0044078D   638 b   barycentric (u, v), x87 or SSE
 *   FUN_00440A20    57 b   the plane height
 *   FUN_00425AAF   322 b   the collision points, at model load (its tail)
 *
 * Every x87 step is modelled with sim_x87 below rather than double - see
 * ground.h for why this file, alone in port/sim, needs to.
*/

#include "ground.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(__SSE__)
#include <xmmintrin.h>
#define GROUND_HOST_RCPSS 1
#endif

/* GCC and Clang on both targets; __extension__ is how -Wpedantic is told the
 * use is deliberate.  The x87 model needs 128-bit products and nothing else
 * that is not ISO C. */
__extension__ typedef unsigned __int128 u128;

/* ======================================================================== */
/* The x87 model: exact operations, then one round to a 64-bit significand. */
/* ======================================================================== */

#define X87_FIN 0
#define X87_INF 1
#define X87_NAN 2

static int bitlen128(u128 x)
{
    uint64_t hi = (uint64_t)(x >> 64), lo = (uint64_t)x;
    if (hi) return 128 - __builtin_clzll(hi);
    if (lo) return 64 - __builtin_clzll(lo);
    return 0;
}

static sim_x87 x87_special(int neg, int cls)
{
    sim_x87 r;
    r.neg = neg; r.cls = cls; r.e = 0; r.m = 0;
    return r;
}

/* mag * 2^e, plus `sticky` meaning "and a little more, less than one unit of
 * mag's last bit".  Callers only set sticky when mag is at least 2^126, so
 * the rounding point is always well above it. */
static sim_x87 x87_round(int neg, u128 mag, int32_t e, int sticky)
{
    sim_x87 r;
    int L = bitlen128(mag);
    r.neg = neg; r.cls = X87_FIN;
    if (L == 0) { r.e = 0; r.m = 0; return r; }
    if (L <= 64) {
        r.m = (uint64_t)(mag << (64 - L));
        r.e = e - (64 - L);
        return r;
    }
    {
        int sh = L - 64;
        u128 half = (u128)1 << (sh - 1);
        u128 lo   = mag & (((u128)1 << sh) - 1);
        u128 q    = mag >> sh;
        int  up   = lo > half || (lo == half && (sticky || (q & 1)));
        e += sh;
        if (up) {
            q++;
            if (q >> 64) { q >>= 1; e++; }
        }
        r.m = (uint64_t)q;
        r.e = e;
    }
    return r;
}

sim_x87 sim_x87_from_float(float f)
{
    uint32_t b;
    int neg, ex;
    uint32_t mant;
    memcpy(&b, &f, 4);
    neg = (int)(b >> 31);
    ex = (int)((b >> 23) & 0xff);
    mant = b & 0x7fffffu;
    if (ex == 0xff) return x87_special(neg, mant ? X87_NAN : X87_INF);
    if (ex == 0) {
        if (!mant) { sim_x87 z = x87_special(neg, X87_FIN); return z; }
        return x87_round(neg, mant, -149, 0);
    }
    return x87_round(neg, (u128)(mant | 0x800000u), ex - 150, 0);
}

sim_x87 sim_x87_from_i64(int64_t i)
{
    uint64_t mag = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
    return x87_round(i < 0, mag, 0, 0);
}

static int x87_is_zero(sim_x87 a) { return a.cls == X87_FIN && a.m == 0; }

sim_x87 sim_x87_add(sim_x87 a, sim_x87 b)
{
    sim_x87 A, B;
    int32_t E;
    u128 MA, MB, S;
    int sticky = 0;

    if (a.cls == X87_NAN || b.cls == X87_NAN) return x87_special(1, X87_NAN);
    if (a.cls == X87_INF || b.cls == X87_INF) {
        if (a.cls == X87_INF && b.cls == X87_INF && a.neg != b.neg)
            return x87_special(1, X87_NAN);
        return a.cls == X87_INF ? a : b;
    }
    if (x87_is_zero(a) && x87_is_zero(b))
        return x87_special(a.neg && b.neg, X87_FIN);   /* round to nearest   */
    if (x87_is_zero(a)) return b;
    if (x87_is_zero(b)) return a;

    /* Both normalised (top bit 63), so the exponent orders the magnitudes. */
    if (a.e > b.e || (a.e == b.e && a.m >= b.m)) { A = a; B = b; }
    else                                         { A = b; B = a; }

    E  = A.e - 63;
    MA = (u128)A.m << 63;
    if (B.e >= E) {
        MB = (u128)B.m << (B.e - E);           /* B.e - E <= 63              */
    } else {
        int32_t sh = E - B.e;
        if (sh >= 128) { MB = 0; sticky = 1; }
        else {
            MB = (u128)B.m >> sh;
            sticky = ((u128)B.m & (((u128)1 << sh) - 1)) != 0;
        }
    }
    if (A.neg == B.neg) {
        S = MA + MB;
    } else {
        /* true value = MA - MB - eps; written as (MA - MB - 1) + (1 - eps) */
        S = MA - MB - (u128)(sticky ? 1 : 0);
        if (S == 0 && !sticky) return x87_special(0, X87_FIN);   /* x-x = +0 */
    }
    return x87_round(A.neg, S, E, sticky);
}

sim_x87 sim_x87_sub(sim_x87 a, sim_x87 b)
{
    b.neg = !b.neg;
    return sim_x87_add(a, b);
}

sim_x87 sim_x87_mul(sim_x87 a, sim_x87 b)
{
    int neg = a.neg ^ b.neg;
    if (a.cls == X87_NAN || b.cls == X87_NAN) return x87_special(1, X87_NAN);
    if (a.cls == X87_INF || b.cls == X87_INF) {
        if (x87_is_zero(a) || x87_is_zero(b)) return x87_special(1, X87_NAN);
        return x87_special(neg, X87_INF);
    }
    if (x87_is_zero(a) || x87_is_zero(b)) return x87_special(neg, X87_FIN);
    return x87_round(neg, (u128)a.m * (u128)b.m, a.e + b.e, 0);
}

/* FDIV.  FUN_0044078D's x87 branch, the bump scale, and sim_yaw_model_q use
 * it.  The quotient is carried to 66 bits and the remainder is the sticky. */
sim_x87 sim_x87_div(sim_x87 a, sim_x87 b)
{
    int neg = a.neg ^ b.neg;
    u128 q, r;
    int k;
    if (a.cls == X87_NAN || b.cls == X87_NAN) return x87_special(1, X87_NAN);
    if (a.cls == X87_INF) {
        if (b.cls == X87_INF) return x87_special(1, X87_NAN);
        return x87_special(neg, X87_INF);
    }
    if (b.cls == X87_INF) return x87_special(neg, X87_FIN);
    if (x87_is_zero(b)) {
        if (x87_is_zero(a)) return x87_special(1, X87_NAN);
        return x87_special(neg, X87_INF);   /* ZE masked: signed infinity   */
    }
    if (x87_is_zero(a)) return x87_special(neg, X87_FIN);

    q = ((u128)a.m << 64) / b.m;
    r = ((u128)a.m << 64) % b.m;
    for (k = 0; k < 2; k++) {
        r <<= 1;
        q <<= 1;
        if (r >= b.m) { r -= b.m; q |= 1; }
    }
    return x87_round(neg, q, a.e - b.e - 66, r != 0);
}

/* Round to nearest-even at bit position `lsb` (value = m * 2^e). */
static u128 round_at(uint64_t m, int32_t shift)
{
    u128 q, lo, half;
    if (shift <= 0) return (u128)m << (-shift);
    if (shift >= 128) return 0;
    q    = (u128)m >> shift;
    lo   = (u128)m & (((u128)1 << shift) - 1);
    half = (u128)1 << (shift - 1);
    if (lo > half || (lo == half && (q & 1))) q++;
    return q;
}

/* FST / FSTP to a float. */
float sim_x87_to_float(sim_x87 a)
{
    int32_t top, lsb;
    u128 q;
    float f;
    if (a.cls == X87_NAN) {
        uint32_t ind = 0xFFC00000u;          /* the x87 "real indefinite"   */
        memcpy(&f, &ind, 4);
        return f;
    }
    if (a.cls == X87_INF) return a.neg ? -INFINITY : INFINITY;
    if (a.m == 0) return a.neg ? -0.0f : 0.0f;

    top = a.e + 63;                          /* the value's leading bit      */
    lsb = top - 23;
    if (lsb < -149) lsb = -149;              /* the denormal floor           */
    q = round_at(a.m, lsb - a.e);
    f = ldexpf((float)(uint32_t)q, (int)lsb); /* q <= 2^24: exact, or inf   */
    return a.neg ? -f : f;
}

/* FISTP under round-to-nearest.  Out of range, infinity and NaN all store
 * the "integer indefinite", 0x80000000. */
int32_t sim_x87_fistp(sim_x87 a)
{
    u128 q;
    if (a.cls != X87_FIN) return (int32_t)0x80000000u;
    if (a.m == 0) return 0;
    if (a.e + 63 >= 64) return (int32_t)0x80000000u;
    q = round_at(a.m, -a.e);
    if (a.neg) {
        if (q > ((u128)1 << 31)) return (int32_t)0x80000000u;
        return (int32_t)(uint32_t)(0u - (uint32_t)q);
    }
    if (q >= ((u128)1 << 31)) return (int32_t)0x80000000u;
    return (int32_t)(uint32_t)q;
}

#define XF(f)  sim_x87_from_float(f)
#define XI(i)  sim_x87_from_i64((int64_t)(i))

/* ======================================================================== */
/* The host's RCPSS.                                                        */
/* ======================================================================== */

#ifdef GROUND_HOST_RCPSS
static float host_rcpss(float x)
{
    return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
}
sim_rcp_fn sim_ground_host_rcpss(void) { return host_rcpss; }
#else
sim_rcp_fn sim_ground_host_rcpss(void) { return NULL; }
#endif

/* ======================================================================== */
/* The geometry.                                                            */
/* ======================================================================== */

#define ZUBE_KEY    0x00
#define ZUBE_PFIRST 0x08
#define ZUBE_PLAST  0x0C
#define KEY_END     0xFFFFFFFFu

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct { uint32_t key; int32_t rec; } keyrec;

static int keyrec_cmp(const void *a, const void *b)
{
    uint32_t x = ((const keyrec *)a)->key, y = ((const keyrec *)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}

int sim_ground_geom_build(sim_ground_geom *g,
                          const uint8_t *zube, int32_t nzube,
                          const uint8_t *pols, int32_t npols,
                          const uint8_t *pnts, int32_t npnts,
                          char *err, size_t errlen)
{
    keyrec *kr;
    int32_t i, n = 0;

    if (err && errlen) err[0] = '\0';
    memset(g, 0, sizeof(*g));
    g->zube = zube; g->nzube = nzube;
    g->pols = pols; g->npols = npols;
    g->pnts = pnts; g->npnts = npnts;
    g->branch = SIM_GROUND_X87;

    kr = (keyrec *)malloc(sizeof(keyrec) * (size_t)(nzube > 0 ? nzube : 1));
    if (!kr) return -2;
    for (i = 0; i < nzube; i++) {
        uint32_t k = rd32(zube + (size_t)i * 0x20 + ZUBE_KEY);
        /* The terminator is bucketed too, but no query key reaches it: a key
         * is at most 0x7FF | 0x7FF << 11. */
        if (k == KEY_END) continue;
        kr[n].key = k; kr[n].rec = i; n++;
    }
    qsort(kr, (size_t)n, sizeof(keyrec), keyrec_cmp);
    for (i = 1; i < n; i++)
        if (kr[i].key == kr[i - 1].key) {
            if (err) snprintf(err, errlen,
                "zube records %d and %d share cell key 0x%06X - the bucket "
                "order would decide which the game finds, and the port does "
                "not reproduce FUN_00420B34's sort", (int)kr[i - 1].rec,
                (int)kr[i].rec, (unsigned)kr[i].key);
            free(kr);
            return -1;
        }

    g->key = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    g->rec = (int32_t *)malloc(sizeof(int32_t) * (size_t)(n ? n : 1));
    if (!g->key || !g->rec) { free(kr); sim_ground_geom_free(g); return -2; }
    for (i = 0; i < n; i++) { g->key[i] = kr[i].key; g->rec[i] = kr[i].rec; }
    g->nkey = n;
    free(kr);
    return 0;
}

void sim_ground_geom_free(sim_ground_geom *g)
{
    free(g->key);
    free(g->rec);
    g->key = NULL; g->rec = NULL; g->nkey = 0;
}

/* FUN_00420DA8: CF = 1 is NOT found - the one inversion inside the family. */
static int32_t cell_lookup(const sim_ground_geom *g, uint32_t key)
{
    int32_t lo = 0, hi = g->nkey - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (g->key[mid] == key) return g->rec[mid];
        if (g->key[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int32_t sim_ground_cell_lookup(const sim_ground_geom *g, uint32_t key)
{
    if (!g || !g->key || !g->rec) return -1;
    return cell_lookup(g, key);
}

/* ======================================================================== */
/* The query.                                                               */
/* ======================================================================== */

typedef struct { int32_t x, y, z; } pnt;

typedef struct query {
    const sim_ground_geom *g;
    int32_t qx, qz;          /* [0x005EC6E4], [0x005EC6EC]                  */
    uint32_t mask;           /* [0x005D31A0]                                */
    int refused;
} query;

static int get_pnt(query *q, uint32_t idx, pnt *out)
{
    const uint8_t *p;
    if (idx >= (uint32_t)q->g->npnts) { q->refused = 1; return -1; }
    p = q->g->pnts + (size_t)idx * 0x0C;
    out->x = (int32_t)rd32(p);
    out->y = (int32_t)rd32(p + 4);
    out->z = (int32_t)rd32(p + 8);
    return 0;
}

/* FUN_00440A59.  EBP = a, EDI = b.  Returns 1 inside-or-on (ZF=1, CF=0),
 * 0 outside (CF=1), -1 degenerate - a and b coincide in x/z (ZF=0), which the
 * callers treat like outside.  Every subtraction is a 32-bit SUB; the two
 * products are IMUL's full 64 bits and the difference is SUB/SBB. */
static int edge(const query *q, const pnt *a, const pnt *b)
{
    int64_t t1, t2;
    uint64_t d;
    if (((uint32_t)a->x ^ (uint32_t)b->x) == 0
        && ((uint32_t)a->z ^ (uint32_t)b->z) == 0)
        return -1;
    t1 = (int64_t)(int32_t)((uint32_t)q->qz - (uint32_t)a->z)
       * (int64_t)(int32_t)((uint32_t)b->x - (uint32_t)a->x);
    t2 = (int64_t)(int32_t)((uint32_t)q->qx - (uint32_t)a->x)
       * (int64_t)(int32_t)((uint32_t)b->z - (uint32_t)a->z);
    d = (uint64_t)t2 - (uint64_t)t1;
    return (d >> 63) ? 0 : 1;              /* TEST EDX,EDX / JS              */
}

/* FUN_0044078D and FUN_00440A20: everything the query leaves once it knows
 * the triangle.  p[0..2] are [0x005EC5CC], [0x005EC5D8], [0x005EC5E4]. */
static int barycentric(query *q, const pnt p[3], sim_ground_hit *h)
{
    const sim_ground_geom *g = q->g;
    int32_t dx  = (int32_t)((uint32_t)q->qx   - (uint32_t)p[0].x);   /* 5fc */
    int32_t e1x = (int32_t)((uint32_t)p[1].x - (uint32_t)p[0].x);    /* 600 */
    int32_t e2x = (int32_t)((uint32_t)p[2].x - (uint32_t)p[0].x);    /* 604 */
    int32_t dz  = (int32_t)((uint32_t)q->qz   - (uint32_t)p[0].z);   /* 614 */
    int32_t e1z = (int32_t)((uint32_t)p[1].z - (uint32_t)p[0].z);    /* 618 */
    int32_t e2z = (int32_t)((uint32_t)p[2].z - (uint32_t)p[0].z);    /* 61c */
    sim_x87 det = sim_x87_sub(sim_x87_mul(XI(e1x), XI(e2z)),
                              sim_x87_mul(XI(e2x), XI(e1z)));
    sim_x87 nu  = sim_x87_sub(sim_x87_mul(XI(dx),  XI(e2z)),
                              sim_x87_mul(XI(e2x), XI(dz)));
    sim_x87 nv  = sim_x87_sub(sim_x87_mul(XI(e1x), XI(dz)),
                              sim_x87_mul(XI(dx),  XI(e1z)));
    float det_f = sim_x87_to_float(det);                     /* FSTP [628] */
    const sim_x87 k65536 = XF(65536.0f);                     /* [0x57B01C] */
    sim_x87 ux, vx;

    if (g->branch == SIM_GROUND_X87) {
        /* 0x004408B5: each numerator is STORED, reloaded as a float and
         * FDIVed by the stored det; the FST keeps the 64-bit quotient in ST0
         * for the x 65536 / FISTP. */
        ux = sim_x87_div(XF(sim_x87_to_float(nu)), XF(det_f));
        vx = sim_x87_div(XF(sim_x87_to_float(nv)), XF(det_f));
    } else {
        /* 0x00440982: RCPSS of the stored det, then the numerator is
         * multiplied WITHOUT being reloaded - FST [620] stores it, and the
         * FMUL uses the exact integer still in ST0. */
        float r;
        if (!g->rcp) { q->refused = 1; return -1; }
        r  = g->rcp(det_f);                                   /* [0x5EC62C] */
        ux = sim_x87_mul(nu, XF(r));
        vx = sim_x87_mul(nv, XF(r));
    }
    h->u   = sim_x87_to_float(ux);                            /* [0x5EC6D0] */
    h->u16 = sim_x87_fistp(sim_x87_mul(ux, k65536));          /* [0x5EC6C0] */
    h->v   = sim_x87_to_float(vx);                            /* [0x5EC6D4] */
    h->v16 = sim_x87_fistp(sim_x87_mul(vx, k65536));          /* [0x5EC6C4] */

    /* FUN_00440A20 - the height, from the STORED u and v:
     *   FILD P1y / FILD P0y / FSUBP / FMUL u          A
     *   FILD P2y / FILD P0y / FSUBP / FMUL v          B
     *   FILD P0y / FADDP / FADDP / FISTP              A + (B + P0y)        */
    {
        sim_x87 A = sim_x87_mul(sim_x87_sub(XI(p[1].y), XI(p[0].y)), XF(h->u));
        sim_x87 B = sim_x87_mul(sim_x87_sub(XI(p[2].y), XI(p[0].y)), XF(h->v));
        h->height = sim_x87_fistp(sim_x87_add(A, sim_x87_add(B, XI(p[0].y))));
    }
    return 0;
}

/* FUN_00440470.  CF = 1 is FOUND.  Returns 1 found (h filled), 0 not. */
static int try_polygon(query *q, uint32_t pidx, sim_ground_hit *h)
{
    const uint8_t *r;
    uint32_t c4, c8, cc, c10;
    pnt v4, v8, vc, v10, tri[3];
    int e, t;

    if (pidx >= (uint32_t)q->g->npols) { q->refused = 1; return 0; }
    r = q->g->pols + (size_t)pidx * 0x40;
    if (r[0] >= 0x10) return 0;                                /* 0x00440483 */
    if (((uint32_t)r[2] | ((uint32_t)r[3] << 8)) & q->mask) return 0;
    c4 = rd32(r + 0x04); c8 = rd32(r + 0x08);
    cc = rd32(r + 0x0C); c10 = rd32(r + 0x10);
    if (get_pnt(q, c4, &v4) || get_pnt(q, c8, &v8) || get_pnt(q, cc, &vc))
        return 0;

    if (cc == c10) {                                           /* a triangle */
        if (edge(q, &v4, &v8) != 1 || edge(q, &v8, &vc) != 1
            || edge(q, &vc, &v4) != 1)
            return 0;
        t = 0;
    } else {                                                   /* a quad     */
        /* The +8 -> +C edge splits it; negative goes to the far triangle,
         * which is the only path that loads corner +0x10 (0x0044061F). */
        e = edge(q, &v8, &vc);
        if (e < 0) return 0;
        if (e == 1) {
            if (edge(q, &vc, &v4) != 1 || edge(q, &v4, &v8) != 1) return 0;
            t = 0;
        } else {
            if (get_pnt(q, c10, &v10)) return 0;
            if (edge(q, &v10, &vc) != 1 || edge(q, &v8, &v10) != 1) return 0;
            t = 1;
        }
    }

    h->poly = (int32_t)pidx;                                   /* [0x5819BC] */
    h->tri  = t;                                               /* [0x5819B4] */
    if (t == 0) {
        h->vert[0] = (int32_t)c4; h->vert[1] = (int32_t)c8; h->vert[2] = (int32_t)cc;
        tri[0] = v4; tri[1] = v8; tri[2] = vc;
    } else {
        h->vert[0] = (int32_t)c10; h->vert[1] = (int32_t)cc; h->vert[2] = (int32_t)c8;
        tri[0] = v10; tri[1] = vc; tri[2] = v8;
    }
    /* [0x005EC6E0] = 1 at 0x0044077B: a flag nothing on this path reads. */
    if (barycentric(q, tri, h) != 0) return 0;
    return 1;
}

/* FUN_00440424 + FUN_00440443: one cell.  CF = 1 is FOUND. */
static int try_cell(query *q, uint32_t x, uint32_t z, sim_ground_hit *h)
{
    uint32_t key = (x >> 21) + ((z >> 21) << 11);
    int32_t rec = cell_lookup(q->g, key);
    const uint8_t *c;
    uint32_t first, last, p;
    if (rec < 0) return 0;
    c = q->g->zube + (size_t)rec * 0x20;
    first = rd32(c + ZUBE_PFIRST);
    last  = rd32(c + ZUBE_PLAST);
    if (first > last || first == 0xFFFFFFFFu) return 0;        /* JA, unsigned */
    for (p = first; ; p++) {
        if (try_polygon(q, p, h)) return 1;
        if (q->refused) return 0;
        if (p >= last) break;                                  /* JBE          */
    }
    return 0;
}

int sim_ground_query(const sim_ground_geom *g, int32_t x, int32_t z,
                     int32_t hint, uint32_t mask, sim_ground_hit *out)
{
    /* FUN_00440299's nine offsets, in its order: ( 0, 0) first, then
     * clockwise from (-1,-1) in (x, z).  0xFFE00000 is -0x200000, one cell. */
    static const int32_t off[9][2] = {
        { 0, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 },
        { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }
    };
    query q;
    int i;

    q.g = g; q.qx = x; q.qz = z; q.mask = mask; q.refused = 0;
    out->found = 0;

    /* FUN_00440276: the hint, if it is not -1 and below [0x00581A64]. */
    if (hint != -1 && (uint32_t)hint < (uint32_t)g->npols) {
        if (try_polygon(&q, (uint32_t)hint, out)) { out->found = 1; return 1; }
        if (q.refused) return -1;
    }
    out->poly = -1;                                            /* 0x004402A5 */
    for (i = 0; i < 9; i++) {
        uint32_t cx = (uint32_t)x + (uint32_t)(off[i][0] * 0x200000);
        uint32_t cz = (uint32_t)z + (uint32_t)(off[i][1] * 0x200000);
        if (try_cell(&q, cx, cz, out)) { out->found = 1; return 1; }
        if (q.refused) return -1;
    }
    return 0;
}

/* ======================================================================== */
/* The collision points and the per-point driver.                           */
/* ======================================================================== */

/* 0x004257F6 (key y, ascending), 0x004258B2 (z, ascending), 0x0042596E and
 * 0x00425A26 (x, descending) - four copies of one routine differing only in
 * the key offset and in which of JA/JC each scan uses.  Transcribed as it
 * runs, including its one-step left scan, rather than as "a quicksort": equal
 * keys decide which point lands where, and a textbook partition would place
 * them differently. */
static int select_qsort(int32_t (*a)[3], int32_t lo, int32_t hi, int key,
                        int ascending)
{
    int32_t i, j;
    uint32_t pivot;
    if (lo >= hi) return 0;                                    /* 0x004257F8 */
    i = lo;
    j = hi;
    /* SHR (hi-lo)*12 by 1, DIV 12, MUL 12: the record (hi - lo) / 2 in. */
    pivot = (uint32_t)a[lo + (hi - lo) / 2][key];              /* 0x0042581A */
    for (;;) {
        uint32_t b;
        if (i > hi) return -1;
        b = (uint32_t)a[i][key];                               /* 0x0042581E */
        i++;
        if (!(ascending ? pivot > b : pivot < b)) i--;         /* JA / JC    */
        do {                                                   /* 0x0042582B */
            if (j < lo) return -1;
            b = (uint32_t)a[j][key];
            j--;
        } while (ascending ? pivot < b : pivot > b);           /* JC / JA    */
        j++;
        if (i == j) break;                                     /* 0x0042583A */
        if (i < lo || i > hi) return -1;
        {
            int32_t t0 = a[i][0], t1 = a[i][1], t2 = a[i][2];
            a[i][0] = a[j][0]; a[i][1] = a[j][1]; a[i][2] = a[j][2];
            a[j][0] = t0;      a[j][1] = t1;      a[j][2] = t2;
        }
        if (a[i][key] == a[j][key]) i++;                       /* 0x0042585F */
    }
    /* 0x00425869: EDI = i - 1, recurse if lo < EDI; ESI = EDI + 0x18. */
    if (lo < i - 1 && select_qsort(a, lo, i - 1, key, ascending) != 0)
        return -1;
    if (i + 1 < hi && select_qsort(a, i + 1, hi, key, ascending) != 0)
        return -1;
    return 0;
}

int sim_collision_points_select(int32_t (*pts)[3], int32_t count)
{
    int32_t n, i;
    int rc = 0;
    if (!pts || count < 4) return -1;
    n = count > 0x3D ? 0x3D : count;                           /* 0x004257A8 */
    for (i = 0; i < n; i++)                                    /* 0x004257BF */
        pts[i][1] = (int32_t)((uint32_t)pts[i][1] + 0x50000u);
    if (select_qsort(pts, 0, n - 1, 1, 1) != 0) rc = -1;       /* 0x004257EB */
    else if (select_qsort(pts, 0, 3, 2, 1) != 0) rc = -1;      /* 0x004258A7 */
    else if (select_qsort(pts, 0, 1, 0, 0) != 0) rc = -1;      /* 0x00425963 */
    else if (select_qsort(pts + 2, 0, 1, 0, 0) != 0) rc = -1;  /* 0x00425A1B */
    for (i = 0; i < n; i++)                                    /* 0x00425ABD */
        pts[i][1] = (int32_t)((uint32_t)pts[i][1] - 0x50000u);
    return rc;
}

int sim_collision_points_build(sim_collision_points *out,
                               const int32_t (*src)[3], int32_t nsrc)
{
    int32_t i;
    memset(out, 0, sizeof(*out));
    if (nsrc < 0 || nsrc + 2 > SIM_GROUND_POINTS) return -1;
    /* 0x00425AEF: FILD / FDIV [0x0057B01C] / FSTP - one rounding.  The
     * quotient of an int by 2^16 is exact in double. */
    for (i = 0; i < nsrc; i++) {
        out->x[i] = (float)((double)src[i][0] / 65536.0);
        out->y[i] = (float)((double)src[i][1] / 65536.0);
        out->z[i] = (float)((double)src[i][2] / 65536.0);
    }
    /* 0x00425B44: for ECX = 0x3D down to 4, a[ECX + 2] = a[ECX] - over the
     * whole 64-entry arrays, whatever nsrc was. */
    for (i = 0x3D; i != 3; i--) {
        out->x[i + 2] = out->x[i];
        out->y[i + 2] = out->y[i];
        out->z[i + 2] = out->z[i];
    }
    out->x[4] = 0.0f; out->y[4] = 0.0f; out->z[4] = 0.0f;      /* 0x00425B79 */
    out->x[5] = 0.0f; out->y[5] = out->y[6]; out->z[5] = 0.0f;
    out->n = nsrc + 2;                                         /* 0x00425BAD */
    return 0;
}

void sim_ground_points_init(sim_ground_points *gp)
{
    int i;
    memset(gp, 0, sizeof(*gp));
    for (i = 0; i < SIM_GROUND_POINTS; i++) gp->poly[i] = -1;
    gp->last_miss = -1;
}

/* SHRD EAX,EDX,14 after IMUL: bits 14..45 of the signed 64-bit product. */
static int32_t lerp14(int32_t a, int32_t b, int32_t f)
{
    int64_t p = (int64_t)(int32_t)((uint32_t)b - (uint32_t)a) * (int64_t)f;
    return (int32_t)((uint32_t)a + (uint32_t)((uint64_t)p >> 14));
}

static int sim_ground_contacts_impl(const sim_ground_geom *g,
                                    const car_state *car,
                                    const sim_ground_inputs *in,
                                    sim_ground_points *gp, int snapshot)
{
    const float *m = car->matrix;
    const sim_collision_points *pts = in->pts;
    uint32_t n = (uint32_t)car->numcollpts;                   /* car+0x260   */
    uint32_t i;
    const sim_x87 k65536 = XF(65536.0f);

    if (n < 1) goto snapshot;                                 /* CMP / JNC   */
    if (n > SIM_GROUND_POINTS) return -1;     /* the original overruns; port's */
    if (n > (uint32_t)pts->n) return -1;      /* fewer points than numcollpts:
                                                 port's guard, not the game's */
    gp->last_miss = -1;

    for (i = 0; i < n; i++) {
        sim_x87 x = XF(pts->x[i]), y, z = XF(pts->z[i]);
        sim_x87 ox, oy, oz;
        sim_ground_hit h;
        int rc;

        if (i < 4) {
            /* 0x0045AE89: the wheel's own y is FSTPed away and replaced by
             * y[6] - susph (+ wheel_drop unless BOTH CPLEFT bits i+5 and i+1
             * are set) - susp_half * suspt.  Never stored: it enters the
             * transform at 64 bits. */
            uint32_t cp = (uint32_t)car->carpartsleft;
            y = sim_x87_sub(XF(pts->y[6]), XF(in->susph));
            if (!(((cp >> (i + 5)) & 1u) && ((cp >> (i + 1)) & 1u)))
                y = sim_x87_add(y, XF(in->wheel_drop));
            y = sim_x87_sub(y, sim_x87_mul(XF(in->susp_half), XF(in->suspt)));
        } else {
            y = XF(pts->y[i]);
        }

        /* FUN_0044B1AB. */
        ox = sim_x87_add(sim_x87_add(sim_x87_mul(XF(m[0]), x),
                                     sim_x87_add(sim_x87_mul(XF(m[4]), y),
                                                 sim_x87_mul(XF(m[8]), z))),
                         XF(m[12]));
        oy = sim_x87_add(sim_x87_add(sim_x87_mul(XF(m[1]), x),
                                     sim_x87_add(sim_x87_mul(XF(m[5]), y),
                                                 sim_x87_mul(XF(m[9]), z))),
                         XF(m[13]));
        oz = sim_x87_add(sim_x87_add(sim_x87_mul(XF(m[2]), x),
                                     sim_x87_add(sim_x87_mul(XF(m[10]), z),
                                                 sim_x87_mul(XF(m[6]), y))),
                         XF(m[14]));
        gp->wx[i] = sim_x87_to_float(ox);
        gp->wy[i] = sim_x87_to_float(oy);
        gp->wz[i] = sim_x87_to_float(oz);
        gp->last_wx = gp->wx[i];
        gp->last_wz = gp->wz[i];

        /* 0x0045AF06: from the STORED floats, x 65536, FISTP, + car pos. */
        gp->ix[i] = (int32_t)((uint32_t)sim_x87_fistp(sim_x87_mul(XF(gp->wx[i]), k65536))
                              + (uint32_t)car->pos[0]);
        gp->iy[i] = (int32_t)((uint32_t)sim_x87_fistp(sim_x87_mul(XF(gp->wy[i]), k65536))
                              + (uint32_t)car->pos[1]);
        gp->iz[i] = (int32_t)((uint32_t)sim_x87_fistp(sim_x87_mul(XF(gp->wz[i]), k65536))
                              + (uint32_t)car->pos[2]);

        rc = sim_ground_query(g, gp->ix[i], gp->iz[i], gp->poly[i],
                              SIM_GROUND_MASK_CONTACT, &h);
        if (rc < 0) return -1;
        gp->ground[i] = 0;
        if (!h.found) {
            gp->last_miss = (int32_t)i;                        /* 0x0045AFAF */
            gp->found[i] = 0;
            continue;              /* poly, tri, u, v, clear: all left as they were */
        }
        gp->found[i]  = 1;
        gp->ground[i] = h.height;

        /* 0x0045AFDC: the bump map, bilinear, 14-bit fractions. */
        {
            uint32_t row = ((uint32_t)gp->iz[i] >> 14) & 0x3f;
            uint32_t col = ((uint32_t)gp->ix[i] >> 14) & 0x3f;
            uint32_t base = (row << 6) & 0xfc0, base1 = (base + 0x40) & 0xfc0;
            uint32_t col1 = (col + 1) & 0x3f;
            int32_t h00 = in->bump_map[base + col],  h01 = in->bump_map[base + col1];
            int32_t h10 = in->bump_map[base1 + col], h11 = in->bump_map[base1 + col1];
            int32_t fx = gp->ix[i] & 0x3fff, fz = gp->iz[i] & 0x3fff;
            int32_t far_  = lerp14(h10, h11, fx);              /* [0x685D7C]  */
            int32_t near_ = lerp14(h00, h01, fx);              /* [0x685D78]  */
            int32_t b     = lerp14(near_, far_, fz);           /* [0x685D80]  */
            /* FILD / FMUL car+0x060 / FMUL 256.0 / FDIV 100.0 / FISTP */
            sim_x87 s = sim_x87_mul(sim_x87_mul(XI(b), XF(car->bump_q)),
                                    XF(in->bump_scale));
            gp->bump[i] = b;
            gp->ground[i] = (int32_t)((uint32_t)gp->ground[i]
                            + (uint32_t)sim_x87_fistp(sim_x87_div(s, XF(in->bump_div))));
        }

        gp->tri[i] = h.tri;
        gp->u[i]   = h.u;
        gp->v[i]   = h.v;
        gp->clear[i] = (int32_t)((uint32_t)gp->iy[i] - (uint32_t)gp->ground[i]);

        if (i < 4) {
            /* 0x0045B140: s = matrix[5], replaced by [0x0057B014] (0.0) if
             * it is below it or unordered (FCOMPP C0 -> CF). */
            float s = m[5];
            if (!(s >= 0.0f)) s = 0.0f;
            gp->clear_raw[i] = gp->clear[i];
            gp->clear[i] = sim_x87_fistp(sim_x87_mul(XI(gp->clear[i]), XF(s)));
        }
        gp->poly[i] = h.poly;                                  /* 0x0045B18E */
    }

    for (i = 0; i < 4; i++) {                                  /* 0x0045B1A2 */
        gp->wheel_ix[i]     = gp->ix[i];
        gp->wheel_ground[i] = gp->ground[i];
        gp->wheel_iz[i]     = gp->iz[i];
    }

snapshot:
    if (snapshot) {
        /* FUN_0045B76A's two REP MOVSDs. */
        memcpy(gp->prev_ixyz[0], gp->ix, sizeof(gp->ix));
        memcpy(gp->prev_ixyz[1], gp->iy, sizeof(gp->iy));
        memcpy(gp->prev_ixyz[2], gp->iz, sizeof(gp->iz));
        memcpy(gp->prev_poly, gp->poly, sizeof(gp->poly));
    }
    return 0;
}

int sim_ground_contacts(const sim_ground_geom *g, const car_state *car,
                        const sim_ground_inputs *in, sim_ground_points *gp)
{
    return sim_ground_contacts_impl(g, car, in, gp, 1);
}

int sim_ground_query_contacts(const sim_ground_geom *g, const car_state *car,
                              const sim_ground_inputs *in,
                              sim_ground_points *gp)
{
    return sim_ground_contacts_impl(g, car, in, gp, 0);
}

void sim_ground_to_contacts(const sim_ground_points *gp,
                            sim_contact out[SIM_CONTACT_POINTS])
{
    int i;
    for (i = 0; i < SIM_CONTACT_POINTS; i++) {
        out[i].poly = gp->poly[i];
        out[i].x    = gp->v[i];      /* [0x00685838] <- [0x005EC6D4] */
        out[i].z    = gp->u[i];      /* [0x00685738] <- [0x005EC6D0] */
        out[i].flag = gp->tri[i];    /* [0x00685638] <- [0x005819B4] */
    }
}

/* ------------------------------------------------------------------------ */
/* The car's ground light - FUN_0044CD28 (ground.h).                        */

static void gl_ramp(uint32_t out[256], const int32_t lo[3], const int32_t hi[3])
{
    int32_t i, k;
    for (i = 0; i < 256; i++) {
        uint32_t c = 0xFF000000u;
        for (k = 0; k < 3; k++) {
            int32_t a = lo ? lo[k] : 0;
            int32_t x = a + (hi[k] - a) * i / 255;          /* IDIV */
            c |= ((uint32_t)x & 0xFFu) << (16 - 8 * k);
        }
        out[i] = c;
    }
}

void sim_ground_light_build(sim_ground_light *gl, const int32_t lo[3],
                            const int32_t hi[3], const int32_t c1[3],
                            const int32_t c2[3])
{
    gl_ramp(gl->lut, lo, hi);                               /* FUN_0044D1AD */
    gl_ramp(gl->c1, NULL, c1);                              /* FUN_0044D200 */
    gl_ramp(gl->c2, NULL, c2);
}

/* IMUL / SHRD EAX,EDX,0x10 / ADD: a + ((b - a) * t >> 16), signed. */
static int32_t gl_lerp(int32_t a, int32_t b, int32_t t)
{
    return (int32_t)(((int64_t)(b - a) * t) >> 16) + a;
}

static int32_t gl_bilinear(const uint8_t *q, int32_t u, int32_t v)
{
    int32_t ec = gl_lerp(q[0], q[1], u);                    /* [0x5819EC] */
    int32_t e8 = gl_lerp(q[2], q[3], u);                    /* [0x5819E8] */
    return gl_lerp(ec, e8, v);
}

/* The MMX arm's PADDUSB (0x0044CFEC); the other arm's three ADD/JAE are the
 * same on the colour bytes and leave alpha 0xFF either way. */
static uint32_t gl_addsat(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    int k;
    for (k = 0; k < 32; k += 8) {
        uint32_t s = ((a >> k) & 0xFFu) + ((b >> k) & 0xFFu);
        r |= (s > 0xFFu ? 0xFFu : s) << k;
    }
    return r;
}

void sim_car_ground_light_q(car_state *car, const sim_ground_geom *geom,
                            const sim_surface_level *level,
                            const sim_ground_light *gl, int32_t hint,
                            uint32_t mask, int32_t reset_window)
{
    sim_ground_hit h;
    const uint8_t *p;
    uint8_t  s[4];
    int32_t  u, v, t, k;
    uint32_t col;

    if (!geom || !gl) return;
    if (sim_ground_query(geom, car->pos[0], car->pos[2], hint, mask, &h) != 1)
        return;                                             /* 0x0044CD78 */
    u = h.u16;
    v = h.v16;
    if (h.tri == 1) {                                       /* 0x0044CD7E */
        u = 0x10000 - u;
        v = 0x10000 - v;
    }
    p = geom->pols + (size_t)h.poly * 0x40u;
    for (k = 0; k < 4; k++) s[k] = p[0x14 + k];
    /* 0x0044CDEE: the slot's +0x2C mode 1 darkens by ((b >> 1) ^ 0x7F) +
     * 0x80, i.e. 255 - b/2, over 256 (MUL, unsigned). */
    if (level && level->desc[p[0x21]].mode == 1) {
        for (k = 0; k < 4; k++) {
            uint32_t f = (((uint32_t)p[0x22 + k] >> 1) ^ 0x7Fu) + 0x80u;
            s[k] = (uint8_t)((f * s[k]) >> 8);
        }
    }
    t = gl_bilinear(s, u, v);
    /* 0x0044CED4: while car+0xFB4 is non-zero the index moves at most 10
     * from the one the car came in with (car+0x244 now).  d == 10 is
     * already inside (JB on |d| < 10 skips; the clamp then equals t).
     * [0x005D64B4] (the pause / retire flag) is
     * never set inside a step - no step runs while paused - so its
     * "keep the old index" arm (0x0044CEDD) cannot be taken. */
    if (reset_window != 0) {
        int32_t prev = car->ground_shade;
        int32_t d = t - prev;
        if ((uint32_t)(d < 0 ? -d : d) >= 10u)
            t = prev + (d < 0 ? -10 : 10);
    }
    car->ground_shade = t;                                  /* car+0x244 */
    col = gl->lut[t & 0xFF];                                /* 0x0044CF33 */
    if (p[0x02] & 4u) {                                     /* 0x0044CF44 */
        col = gl_addsat(gl->c1[gl_bilinear(p + 0x28, u, v) & 0xFF], col);
        col = gl_addsat(gl->c2[gl_bilinear(p + 0x2C, u, v) & 0xFF], col);
    }
    car->ground_col = col;                                  /* car+0x240 */
}
