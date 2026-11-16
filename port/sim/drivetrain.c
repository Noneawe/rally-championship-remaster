/* drivetrain.c - see drivetrain.h.  A line-for-line translation of
 * tools/drivetrain.py's `Drivetrain`, which already gates
 * against the game's own `initaccels`.,.
 *
 * Read this next to tools/drivetrain.py, not next to the disassembly: the
 * Python is the reference translation and the two are meant to be diffable by
 * eye as well as by the gate.
*/

#include "drivetrain.h"
#include "ground.h"                         /* the x87 model, sim_x87_*      */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- image access ------------------------------------------------------
 * A second, deliberately tiny PE walk rather than a shared one: car_params.c
 * owns the real reader, and exposing it would make the constants table look
 * like part of the handling record.  Kept honest by both of them being
 * checked against the same image in the same test. */

typedef struct { unsigned char *blob; size_t len; uint32_t base;
                 const unsigned char *sec; unsigned nsec; } dtpe;

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static float rd_f32(const unsigned char *p)
{
    union { uint32_t u; float f; } cvt;
    cvt.u = rd_u32(p);
    return cvt.f;
}

static const unsigned char *dt_at(const dtpe *im, uint32_t va, size_t n)
{
    unsigned i;
    for (i = 0; i < im->nsec; i++) {
        const unsigned char *h = im->sec + i * 40;
        uint32_t vsize = rd_u32(h + 8), vaddr = rd_u32(h + 12) + im->base;
        uint32_t rawsize = rd_u32(h + 16), rawptr = rd_u32(h + 20);
        uint32_t span = (vsize > rawsize) ? vsize : rawsize;
        if (va < vaddr || va - vaddr >= span) continue;
        {
            uint32_t d = va - vaddr;
            if (d >= rawsize) return NULL;
            if ((uint64_t)d + n > rawsize) return NULL;
            if ((uint64_t)rawptr + d + n > im->len) return NULL;
            return im->blob + rawptr + d;
        }
    }
    return NULL;
}

int dt_consts_load(const char *image_path, dt_consts *out,
                   char *err, size_t errlen)
{
    /* One row per drivetrain.py CONSTANTS entry, in the same order, so the
     * two tables diff cleanly.  Retail 5.25 addresses - see the header. */
    static const uint32_t VA_DT         = 0x005C65C4u;
    static const uint32_t VA_MPH        = 0x0057AF84u;
    static const uint32_t VA_MPH_DISP   = 0x0057AF98u;
    static const uint32_t VA_K10K       = 0x0057B0B8u;
    static const uint32_t VA_THROTTLEFS = 0x0057B0B0u;
    static const uint32_t VA_DRAG_Q     = 0x005C6D04u;
    static const uint32_t VA_ROLL       = 0x005C6D08u;
    static const uint32_t VA_REVREF     = 0x005C6BF8u;
    static const uint32_t VA_REVS_FLOOR = 0x0057AF9Cu;
    static const uint32_t VA_RATIO_FLR  = 0x0057AFD4u;
    static const uint32_t VA_SETG_STEP  = 0x0057B028u;
    static const uint32_t VA_SHIFT_LOSS = 0x005C6BDCu;
    static const uint32_t VA_NEUTRAL    = 0x005C6D20u;
    static const uint32_t VA_CUT_STEPS  = 0x005C6CD8u;
    static const uint32_t VA_OVERREV    = 0x005C6D1Cu;
    static const uint32_t VA_DOWN_SCALE = 0x005C6BC8u;
    static const uint32_t VA_COAST_MARG = 0x0057B060u;
    static const uint32_t VA_T60       = 0x005806C0u;
    static const uint32_t VA_T100       = 0x005806C4u;
    static const uint32_t VA_N_4WD      = 0x005C6C2Cu;
    static const uint32_t VA_N_2WD      = 0x005C6C30u;
    static const uint32_t VA_F208_DIV   = 0x005C6C34u;
    static const uint32_t VA_XTABLE     = 0x005C6D28u;

    dtpe im;
    FILE *f;
    long sz;
    uint32_t pe, opt;
    uint16_t opt_size, magic;
    int i, rc = 0;

    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));

    f = fopen(image_path, "rb");
    if (!f) {
        if (err) snprintf(err, errlen, "cannot open %s", image_path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x400) {
        if (err) snprintf(err, errlen, "%s: too small to be a PE image", image_path);
        fclose(f);
        return -1;
    }
    rewind(f);
    im.len  = (size_t)sz;
    im.blob = (unsigned char *)malloc(im.len);
    if (!im.blob || fread(im.blob, 1, im.len, f) != im.len) {
        if (err) snprintf(err, errlen, "short read on %s", image_path);
        free(im.blob);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (im.blob[0] != 'M' || im.blob[1] != 'Z') {
        if (err) snprintf(err, errlen, "%s: not an MZ image", image_path);
        free(im.blob);
        return -2;
    }
    pe = rd_u32(im.blob + 0x3C);
    if (pe + 24 > im.len || memcmp(im.blob + pe, "PE\0\0", 4) != 0) {
        if (err) snprintf(err, errlen, "%s: no PE header", image_path);
        free(im.blob);
        return -2;
    }
    im.nsec  = rd_u16(im.blob + pe + 6);
    opt_size = rd_u16(im.blob + pe + 20);
    opt      = pe + 24;
    magic    = rd_u16(im.blob + opt);
    if (magic != 0x10B) {
        if (err) snprintf(err, errlen, "%s: not PE32", image_path);
        free(im.blob);
        return -2;
    }
    im.base = rd_u32(im.blob + opt + 28);
    im.sec  = im.blob + opt + opt_size;

#define DTF(field, va)                                                      \
    do {                                                                    \
        const unsigned char *_p = dt_at(&im, (va), 4);                      \
        if (!_p) {                                                          \
            if (err) snprintf(err, errlen,                                  \
                "0x%08X (%s) is not backed by raw data", (unsigned)(va),    \
                #field);                                                    \
            rc = -3;                                                        \
        } else out->field = rd_f32(_p);                                     \
    } while (0)

#define DTI(field, va)                                                      \
    do {                                                                    \
        const unsigned char *_p = dt_at(&im, (va), 4);                      \
        if (!_p) {                                                          \
            if (err) snprintf(err, errlen,                                  \
                "0x%08X (%s) is not backed by raw data", (unsigned)(va),    \
                #field);                                                    \
            rc = -3;                                                        \
        } else out->field = (int32_t)rd_u32(_p);                            \
    } while (0)

    DTF(dt, VA_DT);                 DTF(mph, VA_MPH);
    DTF(mph_disp, VA_MPH_DISP);     DTF(k10k, VA_K10K);
    DTF(throttle_fs, VA_THROTTLEFS);DTF(drag_q, VA_DRAG_Q);
    DTF(roll, VA_ROLL);             DTF(revref, VA_REVREF);
    DTF(revs_floor, VA_REVS_FLOOR); DTF(ratio_floor, VA_RATIO_FLR);
    DTF(setg_step, VA_SETG_STEP);   DTF(shift_loss, VA_SHIFT_LOSS);
    DTF(neutral, VA_NEUTRAL);       DTF(overrev, VA_OVERREV);
    DTF(down_scale, VA_DOWN_SCALE); DTF(coast_margin, VA_COAST_MARG);
    DTF(t60, VA_T60);
    DTF(t100, VA_T100);

    /* These four are read as INTEGERS by the original.  cut_steps genuinely
     * is one; the other three are FUN_0043F208's type bug (section 8). */
    DTI(cut_steps, VA_CUT_STEPS);
    DTI(class_n_4wd, VA_N_4WD);
    DTI(class_n_2wd, VA_N_2WD);
    DTI(f208_div, VA_F208_DIV);

#undef DTF
#undef DTI

    for (i = 0; i < DT_CURVE_PTS && rc == 0; i++) {
        const unsigned char *p = dt_at(&im, VA_XTABLE + 4u * (unsigned)i, 4);
        if (!p) {
            if (err) snprintf(err, errlen,
                              "the torque curve's breakpoint table is not "
                              "backed by raw data");
            rc = -3;
            break;
        }
        out->xt[i] = rd_f32(p);
    }

    /* A self-check that can fail, for the same reason car_params_load has one:
     * these are absolute retail-5.25 addresses and a differently-built image
     * would read plausible-looking garbage out of them.  dt is the sharpest
     * single test - the whole 100 Hz argument closes on it (s3,
     * where a SECOND route to the same 0.01 is what confirmed it). */
    if (rc == 0 && !(out->dt > 0.0099f && out->dt < 0.0101f)) {
        if (err) snprintf(err, errlen,
                          "[0x005C65C4] reads %g, not the 0.01 timestep - "
                          "these are retail 5.25 addresses and this image is "
                          "not that build", (double)out->dt);
        rc = -4;
    }
    free(im.blob);
    return rc;
}

/* ---- FUN_004206D8: the gear ladder ------------------------------------- */

void dt_build_gear_ratios(const car_params *p, const dt_consts *k,
                          int32_t set_g_1, dt_gears *out)
{
    float b0, b1, b2, b3, top, first;
    int g;

    memset(out, 0, sizeof(*out));
    out->ngears  = p->ngears;              /* already clamped to [5,7] on load */
    out->maxrevs = p->maxrevs;             /* already floored at 1000          */

    /* The four gearing endpoints were floored at 8.0 by car_params_load, which
     * is where sim_load_car_params does it.  Do not floor again here. */
    b0 = (float)p->first_lo;   /* record +0x64, first gear, slider 0 */
    b1 = (float)p->top_lo;     /* record +0x68, top gear,   slider 0 */
    b2 = (float)p->first_hi;   /* record +0x6C, first gear, slider 4 */
    b3 = (float)p->top_hi;     /* record +0x70, top gear,   slider 4 */

    /* 0x004206DE..0x00420739: each is ONE x87 expression, and the slider is
     * applied as ((B3 - B1) * setg_step) FIMUL set_g_1 - not B * (setg_step
     * * set_g_1), which is how this used to read. */
    top   = sim_x87_to_float(sim_x87_div(sim_x87_div(sim_x87_add(sim_x87_mul(sim_x87_mul(
                sim_x87_sub(sim_x87_from_float(b3), sim_x87_from_float(b1)),
                sim_x87_from_float(k->setg_step)), sim_x87_from_i64(set_g_1)),
                sim_x87_from_float(b1)), sim_x87_from_float(k->mph)),
                sim_x87_from_float(out->maxrevs)));
    first = sim_x87_to_float(sim_x87_div(sim_x87_div(sim_x87_add(sim_x87_mul(sim_x87_mul(
                sim_x87_sub(sim_x87_from_float(b2), sim_x87_from_float(b0)),
                sim_x87_from_float(k->setg_step)), sim_x87_from_i64(set_g_1)),
                sim_x87_from_float(b0)), sim_x87_from_float(k->mph)),
                sim_x87_from_float(out->maxrevs)));

    out->ratio[out->ngears] = top;
    out->ratio[1]           = first;

    /* THE ONE PLACE IN THIS FILE WHERE AN INTERMEDIATE MUST NOT BE ROUNDED TO
     * float, and it is not cosmetic.
     *
     * The original is x87: FLD ratio[ngears], FSUB ratio[1], FDIV by (n-1),
     * FSTP to a 32-bit slot.  The subtraction and the division both happen in
     * the 80-bit register and only the RESULT is rounded to float32 - which is
     * exactly the "80-bit values crossing a boundary" hazard
     * section 7 is about, and which this file's own header warns of.
     * tools/drivetrain.py models it the same way: its f32() wraps the whole
     * expression, not each operand.
     *
     * Writing this as plain float arithmetic rounds the SUBTRACTION too, which
     * moves `step` by one ULP.  Measured cost of getting it wrong: record 13
     * (Nissan Micra) came out with ratio[3] and ratio[4] one ULP off while the
     * other four gears re-converged and every other car agreed - i.e. it was
     * wrong in 2 values out of 193 and invisible in any decimal printout.
     * That is precisely why the gate compares hex floats (standing rule 1: a
     * print format can fake a measurement). */
    /* ~~step = (float)(...)~~ - AND THE PARAGRAPH ABOVE IS HALF
     * WRONG.  The subtraction and division are in the register, yes; but the
     * quotient is NEVER STORED until 0x004207A2, after every use.  It stays in
     * st(1) and the running sum in st(0), both at 64 bits, and each gear is
     * an FST of the sum - a rounded COPY:
     *     0042073F  FLD top / FSUB first / FILD ngears / FSUB 1.0 / FDIVP
     *     0042075A  FLD first
     *     00420760  FADD st(1) / FST ratio[2]  ... ratio[5]
     *     00420780  ngears > 5: FADD st(1);  FST ratio[6]
     *     00420791  ngears > 6: FADD st(1);  FSTP ratio[7]
     * The old float reading put gears 1-4 of the capture's car one ULP low;
     * the game's own wheel = revs x ratio identity holds at +1 ULP on
     * 2,266/2,276, 8,175/8,204, 8,736/8,773, 7,243/7,270 (gears 1-4) and at +0
     * on gears 5 and 6.  MEASURED SOLO: the endpoints' single rounding above
     * is what fixes it (fwhlspd -2,742, revs -14,045 when reverted); this
     * block's register step/sum changes nothing on that car alone, and stays
     * because the bytes say so (s4).  The Micra
     * evidence above was agreement with tools/drivetrain.py, which made the
     * same reading. */
    {
        sim_x87 sx = sim_x87_div(sim_x87_sub(sim_x87_from_float(out->ratio[out->ngears]),
                                             sim_x87_from_float(out->ratio[1])),
                                 sim_x87_sub(sim_x87_from_i64(out->ngears),
                                             sim_x87_from_float(1.0f)));
        sim_x87 ax = sim_x87_from_float(first);
        for (g = 2; g <= 5; g++) {
            ax = sim_x87_add(ax, sx);
            out->ratio[g] = sim_x87_to_float(ax);
        }
        if (out->ngears > 5) ax = sim_x87_add(ax, sx);
        out->ratio[6] = sim_x87_to_float(ax);
        if (out->ngears > 6) ax = sim_x87_add(ax, sx);
        out->ratio[7] = sim_x87_to_float(ax);
    }

    out->ratio[0] = k->neutral;            /* 0.001 - s7 */
    out->reverse  = -first;

    /* sim_load_car_params: [0x5806A0] = record +0x78 x [0x005C6BF8] / maxrevs.
     * maxrevs is floored at 1000, so this cannot divide by zero however the
     * image is patched. */
    out->finaldrive = (float)p->engscale_raw * k->revref / out->maxrevs;
}

/* ---- FUN_0043B096: the torque curve ------------------------------------ */

void dt_torque_curve(const car_params *p, const dt_consts *k,
                     float revs, float maxrevs, float engpower,
                     float *y, float *q)
{
    float x = revs / maxrevs;
    int i;
    float t, v;

    *q = x * 0.25f;

    /* BOTH out-of-range branches produce curve[0], not curve[8].  Above the
     * rev limit that is a torque collapse to the idle value - but the rev
     * limiter has already zeroed the output before x can exceed 1, so the
     * high clamp is unreachable through the physics path (it IS reachable
     * through the overlay's copy at 0x0043AF86). */
    if (x >= 1.0f || x < 0.0f) {
        *y = p->torque[0] * engpower;
        return;
    }

    i = (int)(x * 8.0f);                   /* truncating, and in [0,7] by the
                                            * clamps above - in bounds by
                                            * construction, not by luck */
    t = (x - k->xt[i]) / (k->xt[i + 1] - k->xt[i]);
    v = (p->torque[i + 1] - p->torque[i]) * t + p->torque[i];
    *y = v * engpower;                     /* engine damage multiplies the
                                            * curve directly - that is the
                                            * whole of the coupling */
}

/* ---- FUN_0043F208: the low-speed torque scale, type bug included ------- */

float dt_torque_scale(const dt_consts *k, int drive_class, int32_t set_g_1)
{
    int32_t n = (drive_class == 1) ? k->class_n_4wd : k->class_n_2wd;

    /* The original computes, in full:
     *
     *     v = |zcarspd| / (int)[0x005C6C34]       ; FIDIV - THE BUG
     *     if (v > 1.0) v = 1.0
     *     k = (float)(N - set_g_1) / (float)N
     *     scale = (1.0 - k) * v + k
     *
     * [0x005C6C34] holds the BIT PATTERN of 32.0f and FIDIV reads it as the
     * integer 1107296256, so `v` is ~0 for any real road speed, the clamp is
     * unreachable, and the whole expression collapses to the constant `k`.
     * The speed ramp does not run.
     *
     * A PORT MUST COPY THIS, NOT FIX IT.  Repairing the type would change
     * every car's low-speed acceleration; filed
     * against in.
     *
     * It is written as the collapsed form rather than the full expression
     * because the full one would need `zcarspd` threaded in to compute a term
     * that is provably zero - which reads like the ramp works. */
    (void)k->f208_div;
    return (float)(n - set_g_1) / (float)n;
}

/* ---- FUN_0043A5CB: sim_engine_speed ------------------------------------ */

float dt_engine_speed(const dt_gears *g, int drive_class,
                      float fwhlspd, float rwhlspd, int gear)
{
    /* The 4WD arm averages the two axles; the 2WD arm takes the front alone.
     * `initaccels` cannot tell these apart - it slaves rwhlspd to fwhlspd, so
     * (v+v)*0.5 is EXACTLY v and both operations are exact in binary floating
     * point.  The tyre force model is what makes
     * them differ, so this is the code path that closes that blind spot, and
     * it is one function so that a mutation to it is visible to both gates. */
    /* 0x0043A5DA..0x0043A5FC is ONE x87 expression, FLD f [/ FADD r / FMUL
     * 0.5] / FDIV ratio / FSTP.  The float version rounded the
     * class-1 sum before the divide.  The ratio through dt_gear_ratio. */
    sim_x87 w = sim_x87_from_float(fwhlspd);
    if (drive_class == 1)
        w = sim_x87_mul(sim_x87_add(w, sim_x87_from_float(rwhlspd)),
                        sim_x87_from_float(0.5f));
    return sim_x87_to_float(sim_x87_div(w, sim_x87_from_float(dt_gear_ratio(g, gear))));
}

float dt_gear_ratio(const dt_gears *g, int gear)
{
    return (gear < 0) ? g->reverse : g->ratio[gear];
}

/* ---- FUN_0043AFEF: sim_drive_torque ------------------------------------ */

float dt_drive_torque(const dt_consts *k, const dt_gears *g,
                      float y, float q, int32_t throttle, int gear,
                      float *crank_out)
{
    /* transcribed.  The bracket is
     *     crank = ((y + q) * throttle / 256) - q
     * and it cancels exactly at throttle 256, which is why `initaccels` can
     * leave it out and why its omission is independent evidence that
     * car0+0x638 has a 256 full scale.
     *
     * ~~Every line there is its own x87 store, so this is plain float
     * arithmetic throughout~~ -: there are TWO stores, not five.
     * 0x0043AFF4..0x0043B014 (FLD y / FADD q / FILD throttle / FMULP / FDIV 256
     * / FSUB q / FSTP [0x5C7474]) and 0x0043B020..0x0043B039 (FLD crank / FMUL
     * finaldrive / FDIV ratio[gear] / FDIV 10000 / FSTP torqu). */
    float crank = sim_x87_to_float(sim_x87_sub(sim_x87_div(sim_x87_mul(
                      sim_x87_add(sim_x87_from_float(y), sim_x87_from_float(q)),
                      sim_x87_from_i64(throttle)), sim_x87_from_float(k->throttle_fs)),
                      sim_x87_from_float(q)));
    if (crank_out) *crank_out = crank;
    return sim_x87_to_float(sim_x87_div(sim_x87_div(sim_x87_mul(
               sim_x87_from_float(crank), sim_x87_from_float(g->finaldrive)),
               sim_x87_from_float(dt_gear_ratio(g, gear))),
               sim_x87_from_float(k->k10k)));
}

/* ---- FUN_0042053A: initaccels ------------------------------------------ */

static int dt_gear_ok(uint32_t gearbits, int g)
{
    return !((gearbits >> (g + 1)) & 1u);   /* BT gearbits, gear+1 */
}

static int dt_gear_up(uint32_t gearbits, int g, int ngears)
{
    int c;
    for (c = g + 1; c <= ngears; c++)
        if (dt_gear_ok(gearbits, c)) return c;
    return 0;                                /* 0 = "none" (never a target) */
}

static int dt_gear_down(uint32_t gearbits, int g)
{
    int c;
    for (c = g - 1; c != 0; c--)
        if (dt_gear_ok(gearbits, c)) return c;
    return 0;
}

void dt_selftest_run(const car_params *p, const dt_consts *k,
                     int32_t set_g_1, uint32_t gearbits,
                     dt_selftest *out)
{
    dt_gears gr;
    int fourwd = (p->drive_class == 1);
    float finaldrive, upfrac, downfrac, coast, engpower = 1.0f, kscale;
    int32_t n;
    float v = 0.0f, revs = 0.0f;
    int gear = 1, cut = 0, cool = 0, limiter = 0;
    int t60 = 0, t100 = 0, s, g;

    memset(out, 0, sizeof(*out));
    dt_build_gear_ratios(p, k, set_g_1, &gr);

    finaldrive = gr.finaldrive;
    upfrac     = p->upshift;
    downfrac   = upfrac * k->down_scale;
    /* [0x00580694] = f32(+0xA4 + 0.1), 0x0042026A (s2.1) - the raw +0xA4 read here until then was 0.1 low. */
    coast      = p->coast_off_brake + k->coast_margin;

    n      = fourwd ? k->class_n_4wd : k->class_n_2wd;
    kscale = (float)(n - set_g_1) / (float)n;

    for (s = 0; s < DT_SELFTEST_STEPS; s++) {
        float z = v;                         /* zcarspd <- fwhlspd */
        int direction = 0;
        float y, qq, torqu, drag;

        if (cut) cut--;                      /* sim_drivetrain_tick_q */

        if (cool) {
            cool--;                          /* the shift cooldown arm */
        } else if (dt_gear_ok(gearbits, gear)) {
            if (gear >= 1 && gear != gr.ngears) {
                float rf = (z / gr.ratio[gear]) / gr.maxrevs;
                int ng = dt_gear_up(gearbits, gear, gr.ngears);
                if (rf >= upfrac && ng != 0) {
                    v = revs * gr.ratio[gear];
                    gear = ng;
                    revs = v / gr.ratio[gear];
                    cut = k->cut_steps; cool = 0x14; direction = 1;
                }
            }
            if (direction == 0 && gear >= 2) {
                float rf = revs / gr.maxrevs;
                int ng = dt_gear_down(gearbits, gear);
                if (ng != 0) {
                    float tt = (rf * gr.ratio[gear]) / gr.ratio[ng];
                    if (tt < downfrac && rf < coast) {
                        v = revs * gr.ratio[gear];
                        gear = ng;
                        revs = v / gr.ratio[gear];
                        if (revs < gr.maxrevs * k->overrev) {
                            cut = k->cut_steps; cool = 0x14; direction = 1;
                        } else {
                            /* the money-shift refusal: put the gear back up */
                            gear += 1;
                            revs = v / gr.ratio[gear];
                            cool = 0x14; direction = -1;
                        }
                    }
                }
            }
        }

        if (direction == 1) revs = revs * k->shift_loss;

        /* sim_wheel_speed_from_revs, UNCONDITIONAL in the gearbox tail */
        if (gear != 0) v = revs * gr.ratio[gear];

        /* FUN_0043B058, the rev limiter */
        if (revs >= gr.maxrevs) {
            limiter = k->cut_steps;
            y = 0.0f;
        } else if (limiter) {
            limiter--;
            y = 0.0f;
        } else {
            dt_torque_curve(p, k, revs, gr.maxrevs, engpower, &y, &qq);
        }

        torqu = ((y * finaldrive) / gr.ratio[gear]) / k->k10k;
        if (cut) torqu = 0.0f;               /* the gearshift torque cut */
        torqu = torqu * kscale;              /* FUN_0043F208 */

        drag = -(((v * v) * k->drag_q) / k->k10k + k->roll);
        v    = (drag + torqu) * k->dt + v;

        if (gear != 0)                       /* sim_engine_speed */
            revs = dt_engine_speed(&gr, p->drive_class, v, v, gear);

        if (!t60  && v >= k->t60)  t60  = s;
        if (!t100 && v >= k->t100) t100 = s;

        if (s % 20 == 0) {
            out->trace_v[out->trace_len] = v * k->mph;
            out->trace_g[out->trace_len] = gear;
            out->trace_len++;
        }
    }

    memcpy(out->name, p->name, sizeof(out->name));
    out->ngears     = gr.ngears;
    out->maxrevs    = gr.maxrevs;
    for (g = 0; g < 8; g++) out->ratio[g] = gr.ratio[g];
    out->finaldrive = finaldrive;
    out->top        = v * k->mph;
    out->t60        = (double)t60  / (double)k->mph_disp;
    out->t100       = (double)t100 / (double)k->mph_disp;
    out->gear       = gear;
    out->designtop  = (float)p->top_lo;
}
