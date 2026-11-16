/* surface.c - the surface solver.  See surface.h.
 *
 * SOURCE
 * ------
 * FUN_0043BA75, 1,401 bytes, 305 instructions, Ghidra coverage 1,401 of 1,401
 * read in full from disasm.tsv this session - the
 * head and the tail for the first time, the blend
 * loop against section 5's existing transcription.  FUN_0043BFEE (the class
 * byte's orientation decode, 30 instructions) and initgametexinfbuf_q
 * (0x00437B1D, the descriptor builder) were read the same session.
 *
 * THE ROUNDING RULE, AND WHERE IT IS KNOWN TO BE APPROXIMATE
 * ---------------------------------------------------------
 * physics.h's rule: an x87 run ending in ONE FSTP is ONE rounding to float32,
 * written here as (float)(double expression).  Every expression below names
 * the FSTP it matches, and the function has been read at the instruction, so
 * unlike tyre.c the STORE POINTS are known, not assumed.
 *
 * What (double) does not reproduce is the 64-bit x87 significand itself: an
 * intermediate that is exact in 80 bits and not in 64 (a difference of two
 * floats far apart in exponent, the weight plane's sum when x or z is tiny) is
 * rounded once more here than there, and the final float can differ on a tie.
 * The Python twin uses the same double arithmetic, so gate A cannot see this;
 * only the game can (gate B).  It is the same residual physics.c carries and
 * it is stated rather than hidden.
*/

#include "surface.h"
#include "image.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the table ---------------------------------------------------------- */

#define SURF_TABLE_VA   0x005C66B0u
#define SURF_POOL_VA    0x005C69B0u   /* = SURF_TABLE_VA + 16 * 0x30, exactly */
#define SURF_POOL_N     8u            /* eight distinct curves, s11 */
#define SURF_MODES_VA   0x005F0890u

static int all_ones(const int32_t *c)
{
    int j;
    for (j = 0; j < SIM_SURF_CURVE_LEN; j++)
        if (c[j] != 1) return 0;
    return 1;
}

int sim_surface_table_load(const char *image_path, sim_surface_table *out,
                           char *err, size_t errlen)
{
    ral_image im;
    int rc, i, j;

    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    rc = ral_image_open(&im, image_path, err, errlen);
    if (rc != 0) return rc;

#define NEED(ptr, va, n, what)                                              \
    do {                                                                    \
        (ptr) = ral_image_at(&im, (uint32_t)(va), (n));                     \
        if (!(ptr)) {                                                       \
            if (err) snprintf(err, errlen,                                  \
                "0x%08X (%s) is not backed by raw data", (unsigned)(va),    \
                (what));                                                    \
            ral_image_close(&im);                                           \
            return -3;                                                      \
        }                                                                   \
    } while (0)

    for (i = 0; i < SIM_SURF_RECORDS; i++) {
        const unsigned char *r, *c;
        sim_surf_rec *s = &out->rec[i];
        uint32_t off;

        NEED(r, SURF_TABLE_VA + (uint32_t)i * SIM_SURF_STRIDE, SIM_SURF_STRIDE,
             "a surface record");
        s->curve_va = (uint32_t)ral_image_i32(r);

        /* THE RELOCATION.  The pointer is an absolute VA in a copyrighted
         * image; it is resolved through the image reader, never dereferenced.
         * And it must land on a CURVE boundary - 0x34, not the record's 0x30 -
         * inside the eight-curve pool.  This is the self-check the ticket
         * asked for, in its sharpest form: a reader with the strides swapped
         * fails here, not at the grip. */
        off = s->curve_va - SURF_POOL_VA;
        if (s->curve_va < SURF_POOL_VA
            || off % SIM_SURF_CURVE_STRIDE != 0
            || off / SIM_SURF_CURVE_STRIDE >= SURF_POOL_N) {
            if (err) snprintf(err, errlen,
                "surface record %d's curve pointer 0x%08X is not on a 0x34 "
                "boundary inside the pool at 0x%08X - wrong image, or the "
                "record and curve strides are confused", i,
                (unsigned)s->curve_va, (unsigned)SURF_POOL_VA);
            ral_image_close(&im);
            return -4;
        }
        NEED(c, s->curve_va, SIM_SURF_CURVE_LEN * 4, "a surface curve");
        for (j = 0; j < SIM_SURF_CURVE_LEN; j++)
            s->curve[j] = ral_image_i32(c + 4 * j);

        s->slide = ral_image_f32(r + 0x04);
        s->accel = ral_image_f32(r + 0x08);
        s->brake = ral_image_f32(r + 0x0C);
        s->f10   = ral_image_f32(r + 0x10);
        s->f14   = ral_image_f32(r + 0x14);
        s->f18   = ral_image_f32(r + 0x18);
        s->f1c   = ral_image_f32(r + 0x1C);
        s->f20   = ral_image_f32(r + 0x20);
        s->f24   = ral_image_f32(r + 0x24);
        s->f28   = ral_image_f32(r + 0x28);
        s->f2c   = ral_image_f32(r + 0x2C);
    }

    {
        const unsigned char *m;
        NEED(m, SURF_MODES_VA, SIM_SURF_LAYER_MODES * 4, "the layer-mode table");
        for (i = 0; i < SIM_SURF_LAYER_MODES; i++)
            out->layer_mode[i] = ral_image_i32(m + 4 * i);
    }

    {
        static const uint32_t gva[4] = {
            0x005C6C90u, 0x005C6C94u, 0x005C6C98u, 0x005C6C9Cu };
        const unsigned char *p;
        NEED(p, 0x0057AF80u, 4, "k_pct");  out->k_pct  = ral_image_f32(p);
        NEED(p, 0x0057B0B0u, 4, "k_256");  out->k_256  = ral_image_f32(p);
        NEED(p, 0x005C6D24u, 4, "k_mean"); out->k_mean = ral_image_f32(p);
        NEED(p, 0x0057B01Cu, 4, "k_fix");  out->k_fix  = ral_image_f32(p);
        for (i = 0; i < 4; i++) {
            NEED(p, gva[i], 4, "gripf_k");
            out->gripf_k[i] = ral_image_f32(p);
        }
    }
#undef NEED
    ral_image_close(&im);

    /* THE SELF-CHECK, which can fail.  From the DATA, not the code: records 0
     * and 13..15 are the unused ones and all four point at the all-ones curve;
     * records 1..12 are the twelve real surfaces and none of them does
     * (and's corpus
     * census agrees from the asset side: classes 0..12 and nothing else). */
    for (i = 0; i < SIM_SURF_RECORDS; i++) {
        int unused = (i == 0 || i >= 13);
        if (all_ones(out->rec[i].curve) != unused) {
            if (err) snprintf(err, errlen,
                "surface record %d %s the all-ones curve; records 0 and 13..15 "
                "must and 1..12 must not - this is not the retail 5.25 table",
                i, unused ? "does not resolve to" : "resolves to");
            return -4;
        }
    }
    if (out->k_fix != 65536.0f || out->k_256 != 256.0f) {
        if (err) snprintf(err, errlen,
            "the quadrant scale reads %g and the byte divisor %g, not 65536 "
            "and 256", (double)out->k_fix, (double)out->k_256);
        return -4;
    }
    return 0;
}

/* ---- the level ---------------------------------------------------------- */

int sim_surface_level_build(sim_surface_level *lv,
                            const uint8_t *texinfo,
                            const uint8_t *ti, size_t ti_len,
                            const uint8_t *pols, int32_t npols)
{
    int id, rc = 0;

    memset(lv, 0, sizeof(*lv));
    lv->pols  = pols;
    lv->npols = npols;

    /* initgametexinfbuf_q, 0x00437B1D: 256 slots, record r = texinfo[id], and
     * a raw index * 0x80 into TEXTINFO with no bounds check. */
    for (id = 0; id < SIM_SURF_TEXSLOTS; id++) {
        uint32_t r = (uint32_t)texinfo[4 * id]
                   | ((uint32_t)texinfo[4 * id + 1] << 8)
                   | ((uint32_t)texinfo[4 * id + 2] << 16)
                   | ((uint32_t)texinfo[4 * id + 3] << 24);
        const uint8_t *rec;
        sim_texdesc *d = &lv->desc[id];

        if ((size_t)r >= ti_len / 0x80u) {
            rc = -1;                  /* THE PORT'S refusal - see surface.h */
            continue;
        }
        rec = ti + (size_t)r * 0x80u;
        memcpy(d->cls, rec + 0x20, 4);   /* 0x00437B69 -> desc+0x10 */
        memcpy(d->b14, rec + 0x10, 4);   /* 0x00437B6F -> desc+0x14 */
        memcpy(d->b18, rec + 0x14, 4);   /* 0x00437B75 -> desc+0x18 */
        d->mode = rec[0x24];             /* 0x00437B81 -> desc+0x2C */
    }
    return rc;
}

/* ---- the solver --------------------------------------------------------- */

/* FISTP under the boot's round-to-nearest-even control word.  An out-of-range
 * value stores the x87 "integer indefinite", 0x80000000. */
static int32_t fistp(double v)
{
    double r = nearbyint(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return INT32_MIN;
    return (int32_t)r;
}

/* FUN_0043BFEE.  EAX = the class byte, EDX = the x half-bit, EBX = the z
 * half-bit; the low three bits of the class byte are a symmetry of the square
 * (identity, the two mirrors, the half turn, and four with a transpose), and
 * the return in EDX is the quadrant, EDX*2 + EBX.  Thirty instructions:
 *
 *   0: -            1: EBX^=1          2: EDX^=1          3: EBX^=1 EDX^=1
 *   4: swap EDX^=1  5: swap EBX^=1 EDX^=1   6: swap       7: swap EBX^=1 */
static int32_t quadrant_decode(uint8_t cls, uint32_t edx, uint32_t ebx)
{
    uint32_t t;
    switch (cls & 7u) {
    case 1: ebx ^= 1u; break;
    case 2: edx ^= 1u; break;
    case 3: ebx ^= 1u; edx ^= 1u; break;
    case 4: t = edx; edx = ebx; ebx = t; edx ^= 1u; break;
    case 5: t = edx; edx = ebx; ebx = t; ebx ^= 1u; edx ^= 1u; break;
    case 6: t = edx; edx = ebx; ebx = t; break;
    case 7: t = edx; edx = ebx; ebx = t; ebx ^= 1u; break;
    default: break;
    }
    return (int32_t)(edx + edx + ebx);
}

/* FLD a / FLD b / FSUB ST0,ST1 / FMUL w / FADDP / FSTP  ->  a + (b - a)*w,
 * one rounding.  The shape of every one of the loop's lerps. */
static float lerp1(float a, float b, float w)
{
    return (float)((double)a + ((double)b - (double)a) * (double)w);
}

static int on_road_class(int32_t c)
{
    /* 0x0043BE24..0x0043BE72: four CMP/JZ pairs per layer. */
    return c == 1 || c == 2 || c == 3 || c == 12;
}

/* FLD next / FLD cur / FCOMPP / FNSTSW / SHR AH,1 / JC keep-cur.  C0 is set
 * for cur < next AND for unordered, so the kept value is cur unless
 * cur >= next - which `!(cur >= next)` says exactly, NaN included. */
static float x87_min(float cur, float next)
{
    return !(cur >= next) ? cur : next;
}

int sim_surface_solve_q(const sim_surface_table *t,
                        const sim_surface_level *lv,
                        const sim_contact contact[SIM_CONTACT_POINTS],
                        const car_params *p, float bumpf,
                        car_state *car, sim_surface *o)
{
    int i;
    int32_t k = car->tyretype;
    const float wet = car->wetns;

    if (k < 0) k = 0;                                /* THE PORT'S clamp */
    if (k > SIM_SURF_TYRE_MAX) k = SIM_SURF_TYRE_MAX;

    memset(o, 0, sizeof(*o));

    for (i = 0; i < SIM_CONTACT_POINTS; i++) {
        const sim_contact *c = &contact[i];
        const uint8_t *pol;
        const sim_texdesc *d0, *d1;
        const sim_surf_rec *A, *B;
        uint32_t xb, zb;
        float gA, gB;

        /* ---- the head: which polygon, which quadrant -------------------
         * 0x0043BA7A: EBX = poly; ESI = [0x00581A04] + poly*0x40.  No bounds
         * check in the original. */
        if (c->poly < 0 || c->poly >= lv->npols) return -1;
        pol = lv->pols + (size_t)c->poly * 0x40u;
        d0  = &lv->desc[pol[0x01]];

        /* FLD x / FMUL 65536.0 / FISTP - the product is exact (a power of
         * two), and only bit 15 survives: which half of the polygon.  SHR is
         * logical and the AND discards everything else, so the sign of a
         * negative coordinate cannot reach it. */
        xb = ((uint32_t)fistp((double)c->x * (double)t->k_fix) >> 15) & 1u;
        zb = ((uint32_t)fistp((double)c->z * (double)t->k_fix) >> 15) & 1u;
        if (c->flag == 1) { xb ^= 1u; zb ^= 1u; }   /* CMP [..],1 / JNZ */

        /* PUSH EBX / PUSH EDX around the first call, so both layers decode
         * the SAME half-bits, each through its own class byte. */
        o->quad_a[i] = quadrant_decode(pol[0x00], xb, zb);
        o->quad_b[i] = quadrant_decode(pol[0x20], xb, zb);

        o->cls_a[i]   = d0->cls[o->quad_a[i]];
        o->aux18_a[i] = d0->b18[o->quad_a[i]];
        o->aux14_a[i] = d0->b14[o->quad_a[i]];

        d1 = &lv->desc[pol[0x21]];
        o->cls_b[i]   = d1->cls[o->quad_b[i]];
        o->aux18_b[i] = d1->b18[o->quad_b[i]];
        o->aux14_b[i] = d1->b14[o->quad_b[i]];

        /* ---- the weight: a plane through three corner bytes ------------
         * FLDZ / FSTP first, so every skip below leaves it 0 = "all layer 0".
         * Skipped for second-layer modes 5 and 4 (the multiply-down and the
         * additive arm -), for a zero layer-1 class,
         * and for no second layer at all (pols+0x21 == 0). */
        o->weight[i] = 0.0f;
        {
            int32_t mode = t->layer_mode[d1->mode];
            if (mode != 5 && mode != 4 && o->cls_b[i] != 0 && pol[0x21] != 0) {
                int32_t a, b, cc;
                if (c->flag == 1) { a = pol[0x25]; b = pol[0x24]; cc = pol[0x23]; }
                else              { a = pol[0x22]; b = pol[0x23]; cc = pol[0x24]; }
                /* FILD (b-a) / FMUL z / FILD (c-a) / FMUL x / FILD a / FADDP /
                 * FADDP / FDIV 256.0 / FSTP - the (c-a)x + a pair adds FIRST. */
                o->weight[i] = (float)(((double)(b - a) * (double)c->z
                                        + ((double)(cc - a) * (double)c->x
                                           + (double)a))
                                       / (double)t->k_256);
            }
        }

        /* ---- the override: flags bit 0x1000 --------------------------------
         * TEST word [ESI+2],0x1000.  Both classes forced to 5, both aux18
         * bytes to 0x3C, both aux14 bytes to 0 - and the WEIGHT IS LEFT AS
         * COMPUTED, so a blended polygon stays blended, between two copies of
         * record 5. */
        if (((uint32_t)pol[0x02] | ((uint32_t)pol[0x03] << 8)) & 0x1000u) {
            o->cls_a[i] = o->cls_b[i] = 5;
            o->aux18_a[i] = o->aux18_b[i] = 0x3C;
            o->aux14_a[i] = o->aux14_b[i] = 0;
        }

        /* The class indexes the 16-record table unchecked in the original; a
         * byte past 15 would read past the curve pool.  THE PORT refuses. */
        if (o->cls_a[i] >= SIM_SURF_RECORDS || o->cls_b[i] >= SIM_SURF_RECORDS)
            return -1;
        A = &t->rec[o->cls_a[i]];
        B = &t->rec[o->cls_b[i]];

        /* ---- the blend: in its STORE order ------ */
        o->f28[i] = lerp1(A->f28, B->f28, o->weight[i]);   /* [0x5C6FF4] */
        o->f2c[i] = lerp1(A->f2c, B->f2c, o->weight[i]);   /* [0x6CD5AC] */

        /* The curve.  0x0043BCA2..BCB8 first stores entry[0] x 0.01 to both
         * scratch slots and 0x0043BCDC/BD00 overwrite them unread - two DEAD
         * STORES, not a second branch.  Then, with the tyre type:
         *   FILD c[1+k] / FILD c[7+k] / FSUB ST0,ST1 / FMUL wet / FADDP /
         *   FMUL 0.01 / FSTP  ->  (c1 + (c7 - c1)*wet) * 0.01, one rounding,
         * STORED - so each grip product below multiplies a float32 g. */
        gA = (float)(((double)A->curve[1 + k]
                      + ((double)A->curve[7 + k] - (double)A->curve[1 + k])
                        * (double)wet) * (double)t->k_pct);
        gB = (float)(((double)B->curve[1 + k]
                      + ((double)B->curve[7 + k] - (double)B->curve[1 + k])
                        * (double)wet) * (double)t->k_pct);

        /* FLD A.x / FMUL gA / FLD B.x / FMUL gB / FSUB ST0,ST1 / FMUL w /
         * FADDP / FMUL knob / FSTP - the whole grip is ONE rounding. */
#define GRIP(field, knob)                                                   \
        (float)(((double)A->field * gA                                      \
                 + ((double)B->field * gB - (double)A->field * gA)          \
                   * (double)o->weight[i]) * (double)(knob))
        o->slide[i] = GRIP(slide, p->sidegrip);            /* [0x5C6FB8] */
        o->accel[i] = GRIP(accel, p->acc_grip);            /* [0x5C6FCC] */
        o->brake[i] = GRIP(brake, p->brk_grip);            /* [0x5C6FE0] */
#undef GRIP

        o->f20[i] = lerp1(A->f20, B->f20, o->weight[i]);   /* [0x5C73F4] */
        o->f24[i] = lerp1(A->f24, B->f24, o->weight[i]);   /* [0x5C7408] */
        o->f14[i] = lerp1(A->f14, B->f14, o->weight[i]);   /* [0x5C741C] */
        o->f10[i] = lerp1(A->f10, B->f10, o->weight[i]);   /* [0x5C7430] */
    }

    /* ---- the tail: 0x0043BDF0..0x0043BFED ---------------------------------
     * FLD / FADD / FADD / FADD / FMUL 0.0025 / FSTP - the WHEELS only, 0..3. */
    o->mean_slide = (float)(((((double)o->slide[0] + o->slide[1]) + o->slide[2])
                             + o->slide[3]) * (double)t->k_mean);

    /* A per-point "on a road surface" flag over all five, and a count over
     * the four wheels into car0+0x258.  {1, 2, 3, 12} are tarmac, gravel,
     * mud and the second gravel record. */
    for (i = 0; i < SIM_CONTACT_POINTS; i++)
        o->on_road[i] = on_road_class(o->cls_a[i]) || on_road_class(o->cls_b[i]);
    car->road_wheels_q = o->on_road[0] + o->on_road[1] + o->on_road[2]
                       + o->on_road[3];

    /* The minimum of record +0x28 over all FIVE points, each step stored to
     * car0+0x060 and re-loaded, then x bumpf with one more FSTP. */
    {
        float m = x87_min(o->f28[0], o->f28[1]);
        m = x87_min(m, o->f28[2]);
        m = x87_min(m, o->f28[3]);
        m = x87_min(m, o->f28[4]);
        car->bump_q = (float)((double)m * (double)bumpf);
    }

    /* `gripf`, [0x005C6F64].  Two wetness lerps, each STORED to scratch
     * (0x005D6ED4/ED8), then a lerp between them by set_s_1 / 256:
     *   ed4 = (0x5C6C90 - 0x5C6C98)*wet + 0x5C6C98     1.0 / 0.85
     *   ed8 = (0x5C6C94 - 0x5C6C9C)*wet + 0x5C6C9C     0.9 / 1.0
     *   gripf = (ed8 - ed4) * (set_s_1 / 256) + ed4    one rounding  */
    {
        float ed4 = (float)(((double)t->gripf_k[0] - t->gripf_k[2]) * wet
                            + t->gripf_k[2]);
        float ed8 = (float)(((double)t->gripf_k[1] - t->gripf_k[3]) * wet
                            + t->gripf_k[3]);
        o->gripf = (float)(((double)ed8 - ed4)
                           * ((double)car->set_s_1 / (double)t->k_256)
                           + ed4);
    }

    /* Four FLD/FSTP copies of CONTACT POINT 4's lerps.  The first two are
     * sim_surface's drag pair, so the fifth point - the one
     * section 1 says feeds the yaw ceiling and nothing else - also sets the
     * drag.  That sentence needs widening, and says so. */
    o->drag_k    = o->f20[4];     /* [0x5C7494] <- [0x5C7404] */
    o->drag_vmin = o->f24[4];     /* [0x5C7498] <- [0x5C7418] */
    o->f14_pt4   = o->f14[4];     /* [0x5D6C3C] <- [0x5C742C] */
    o->f10_pt4   = o->f10[4];     /* [0x5D6C40] <- [0x5C7440] */
    return 0;
}
