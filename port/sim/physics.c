/* physics.c - the constants loader, the vector/matrix primitives, the two
 * integrators and the divergence guard.  See physics.h.
 *
 * EVERY FUNCTION HERE WAS READ FROM THE IMAGE THIS SESSION, instruction by
 * instruction, with tools/ralimg.py disasm. named the
 * three primitives (axis-angle, orthonormalise, the compose) and recorded what
 * they are FOR; it did not read their arithmetic, and one of them turned out
 * not to do what its role says - see sim_axis_angle_matrix_q.
*/

#include "physics.h"
#include "image.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the x87 rule, in one helper --------------------------------------
 *
 * A run of x87 arithmetic ending in ONE FSTP is ONE rounding to float32.
 * Writing that run in C `float` rounds after every operator, which moves the
 * result by an ULP or two - invisible in decimal and exactly the bug session
 * 61 paid for in FUN_004206D8's `step`.  So an expression that the original
 * stores once is computed in `double` here and rounded once, through this.
 *
 * `double` is not `long double`: x87 keeps 64 mantissa bits and IEEE double
 * keeps 53.  For every expression in this file the double result and the
 * 80-bit result round to the SAME float32 - they would have to differ by more
 * than 2^-53 relative for that to fail, which needs a near-tie at the float32
 * boundary.  It is not proved, and where it matters the measurement against
 * the capture is the check that would notice. */
static float st(double v) { return (float)v; }

/* FISTP under the prevailing control word, which the boot leaves at
 * round-to-nearest (and section 7).  Written out
 * rather than delegated to rintf() so it cannot be changed by whatever the
 * host's FP environment happens to be - and because the C cast a port would
 * reach for first is the OTHER rounding, which the capture measures as the
 * difference between 99.89% and 50.50% agreement on the vertical axis. */
static int32_t fistp_rne(double v)
{
    double f, d, r;

    /* A NaN or an out-of-range value: the x87 stores the "integer
     * indefinite", 0x80000000 (with IE masked, FNINIT's default). */
    if (!(v > -2147483648.5 && v < 2147483647.5)) return INT32_MIN;
    f = floor(v);
    d = v - f;
    if (d > 0.5)      r = f + 1.0;
    else if (d < 0.5) r = f;
    else              r = (fmod(f, 2.0) == 0.0) ? f : f + 1.0;   /* ties to even */
    return (int32_t)r;
}

/* ---- the constants ----------------------------------------------------- */

int sim_consts_load(const char *image_path, sim_consts *out,
                    char *err, size_t errlen)
{
    ral_image im;
    int rc;

    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    rc = ral_image_open(&im, image_path, err, errlen);
    if (rc != 0) return rc;

#define PF(field, va)                                                       \
    do {                                                                    \
        const unsigned char *_p = ral_image_at(&im, (uint32_t)(va), 4);     \
        if (!_p) {                                                          \
            if (err) snprintf(err, errlen,                                  \
                "0x%08X (%s) is not backed by raw data", (unsigned)(va),    \
                #field);                                                    \
            rc = -3;                                                        \
        } else out->field = ral_image_f32(_p);                              \
    } while (0)

    PF(dt,            0x005C65C4u);
    PF(pos_scale,     0x0057B01Cu);
    PF(zero,          0x0057B014u);
    PF(wheelbase,     0x005C6C3Cu);
    PF(half_q,        0x005C6C40u);
    PF(grip_k,        0x005C6C48u);
    PF(clamp_ang,     0x005C6BB0u);
    PF(clamp_lin,     0x005C6BB4u);
    PF(clamp_zspddif, 0x005C6BC0u);
    PF(clamp_rotspd,  0x005C6C08u);
    PF(gravity,       0x005C6CFCu);
    PF(trac_num,      0x005C6B88u);
    PF(trac_add,      0x005C6B8Cu);
    PF(trac_sub,      0x005C6B90u);
    PF(inv_inertia[0], 0x005C6CDCu);
    PF(inv_inertia[1], 0x005C6CE0u);
    PF(inv_inertia[2], 0x005C6CE4u);
    PF(contact_normal_floor, 0x0057AF80u);
    PF(contact_restitution,  0x00684B18u);
    PF(contact_push,         0x00684B1Cu);
    PF(body_mu_base,         0x0057AFBCu);
    PF(body_mu_scale,        0x0057AFC0u);
    PF(body_yaw_divisor,     0x0057AFE4u);
    PF(body_contact_friction, 0x005C6FC8u);
    PF(impact_speed_min,     0x005C6B80u);
    PF(impact_retain_ratio,  0x005C6B84u);
    PF(wheel_drop,    0x005C6BCCu);
    PF(susp_half,     0x005C6CC0u);
    PF(wheel_radius,  0x005C6BFCu);
    PF(inv_pi,        0x0057B04Cu);
    PF(wheel_ride,    0x005C6C00u);
    PF(bump_scale,    0x0057B0B0u);
    PF(bump_div,      0x0057AF98u);
    PF(bbal_low,      0x005C6C50u);
    PF(bbal_high,     0x005C6C54u);
    PF(ste_k_low,     0x005C6C58u);
    PF(ste_k_high,    0x005C6C5Cu);
    PF(ste_j_low,     0x005C6C60u);
    PF(ste_j_high,    0x005C6C64u);
    PF(susph_low,     0x005C6C78u);
    PF(susph_high,    0x005C6C7Cu);
    PF(set_cgh_low[0],  0x005C6C80u);
    PF(set_cgh_high[0], 0x005C6C88u);
    PF(set_cgh_low[1],  0x005C6C84u);
    PF(set_cgh_high[1], 0x005C6C8Cu);
    PF(suspt_low,     0x005C6CA0u);
    PF(suspt_high,    0x005C6CA4u);
    PF(suspd_low,     0x005C6CA8u);
    PF(suspd_high,    0x005C6CACu);
    PF(susp_factor_low,  0x005C6CB8u);
    PF(susp_factor_high, 0x005C6CBCu);
    PF(bumpf_low,     0x005C6CB0u);
    PF(bumpf_high,    0x005C6CB4u);
    PF(load_bump_low,  0x0057AFBCu);
    PF(load_bump_high, 0x0057AFCCu);
    PF(load_bump_step, 0x0057B028u);
    PF(drag_speed_scale, 0x005C6CE8u);
    PF(mass_nonzero_scale, 0x005B538Cu);
    PF(body_drag_num, 0x005C6D04u);
    PF(body_drag_den, 0x0057B0B8u);
    PF(suspension_force_scale, 0x005C6CC4u);
    PF(lateral_point_y, 0x005C6F08u);
    PF(longitudinal_point_y, 0x005C6F0Cu);
    PF(slip_grip_scale[0], 0x005C6C24u);
    PF(slip_grip_scale[1], 0x005C6C28u);
    PF(lowspd_grip_ref,  0x005C6BA4u);
    PF(lowspd_grip_add,  0x005C6BA8u);
    PF(lowspd_grip_half, 0x005C6BACu);
    PF(wheel_spin_down[0], 0x005C6C74u);
    PF(wheel_spin_down[1], 0x005C6C70u);
    PF(idle_revs[0],     0x005C6CECu);
    PF(idle_revs[1],     0x005C6CF0u);
    PF(overrev_scale,    0x005C6D1Cu);
    PF(free_decay_scale, 0x005C6D10u);
    PF(free_decay_base,  0x005C6D0Cu);
    PF(gear_reverse_revs, 0x0057B028u);
    PF(gear_select_blend, 0x005C6D18u);
    PF(stall_margin,      0x0057AF98u);
    PF(mode2_select_frac, 0x005C6C4Cu);
    PF(mode2_fwd_speed,   0x005C6BD4u);
    PF(mode2_back_speed,  0x005C6BD8u);
    PF(object_speed_min, 0x0057AFD4u);
    PF(bearing_scale,    0x0057B0B0u);
    PF(inv_two_pi,       0x0057B058u);
    PF(angle_steps,      0x0057B0ACu);
    PF(asin_limit,       0x0057AFBCu);
    PF(pivot_one,        0x0057AFBCu);
    /* FUN_0045D16A -. */
    PF(car_box[0],       0x006868E0u);
    PF(car_box[1],       0x006868E4u);
    PF(car_box[2],       0x006868E8u);
    {
        int c;
        for (c = 0; c < 8; c++) {
            PF(car_corner[0][c], 0x00686904u + 4u * (uint32_t)c);
            PF(car_corner[1][c], 0x00686924u + 4u * (uint32_t)c);
            PF(car_corner[2][c], 0x00686944u + 4u * (uint32_t)c);
        }
    }
    PF(car_exchange_lo,  0x006868D0u);
    PF(car_exchange_hi,  0x006868D4u);
    PF(car_push_half,    0x006868ACu);
    PF(car_push_scale,   0x0057AFC4u);
    /* FUN_0043A2D3 -. */
    PF(spiral_speed_scale, 0x005C6D14u);
    /* FUN_004C66EA's stage 1 -. */
    PF(dmg_pct,        0x008DCDB8u);
    PF(dmg_w[0],       0x008DD0C4u);
    PF(dmg_w[1],       0x008DD0C8u);
    PF(dmg_w[2],       0x008DD0CCu);
    PF(dmg_w[3],       0x008DD0D0u);
    PF(dmg_w[4],       0x008DD0D4u);
    PF(dmg_w[5],       0x008DD0D8u);
    PF(dmg_turbo,      0x008DCFD0u);
    PF(dmg_full,       0x008DCD7Cu);
    PF(dmg_eff_lo,     0x008DCD80u);
    PF(dmg_eff_hi,     0x008DCD78u);
    /* FUN_0043F422, the octant damage. */
    PF(oct_min,        0x005C7994u);
    PF(oct_div,        0x0057AFE4u);
    PF(oct_lo,         0x0057B014u);
    PF(oct_hi,         0x0057AFBCu);
    PF(dmg_ai_one,     0x0057AFBCu);
    PF(dmg_ai_div,     0x0057B0B0u);
    PF(dmg_ai_mul,     0x0057AF98u);
    PF(dmg_half,       0x008DCD90u);
    PF(dmg_ramp_ttmp[0],  0x008DCF18u);
    PF(dmg_ramp_ttmp[1],  0x008DCF1Cu);
    PF(dmg_ramp_ttmp[2],  0x008DCF20u);
    PF(dmg_ramp_ttmp[3],  0x008DCF24u);
    PF(dmg_ramp_tyre[0],  0x008DCF28u);
    PF(dmg_ramp_tyre[1],  0x008DCF2Cu);
    PF(dmg_ramp_tyre[2],  0x008DCF30u);
    PF(dmg_ramp_tyre[3],  0x008DCF34u);
    PF(dmg_ramp_steer[0], 0x008DCF38u);
    PF(dmg_ramp_steer[1], 0x008DCF3Cu);
    PF(dmg_ramp_steer[2], 0x008DCF40u);
    PF(dmg_ramp_steer[3], 0x008DCF44u);
    PF(dmg_roll,       0x008DCD70u);
#undef PF

    {
        const unsigned char *px = ral_image_at(&im, 0x005C6D4Cu, 64);
        const unsigned char *pz = ral_image_at(&im, 0x005C6D8Cu, 64);
        int c;
        if (!px || !pz) {
            if (err) snprintf(err, errlen,
                              "0x005C6D4C/0x005C6D8C (the spiral search's probe "
                              "tables) are not backed by raw data");
            rc = -3;
        } else {
            for (c = 0; c < 16; c++) {
                out->spiral_x[c] = ral_image_i32(px + 4 * c);
                out->spiral_z[c] = ral_image_i32(pz + 4 * c);
            }
        }
    }

    {
        const unsigned char *p = ral_image_at(&im, SIM_DMG_PAGE_BASE,
                                              4u * SIM_DMG_PAGE_WORDS);
        uint32_t i;
        if (!p) {
            if (err) snprintf(err, errlen,
                              "0x008DCD74.. (the damage accumulators' page) is "
                              "not backed by raw data");
            rc = -3;
        } else {
            for (i = 0; i < SIM_DMG_PAGE_WORDS; i++)
                out->dmg_page[i] = (uint32_t)ral_image_i32(p + 4 * i);
        }
    }

    {
        const unsigned char *p = ral_image_at(&im, 0x005C6CF8u, 4);
        if (!p) {
            if (err) snprintf(err, errlen,
                              "0x005C6CF8 (the files\\b model y offset) is not "
                              "backed by raw data");
            rc = -3;
        } else out->car_model_y_q = ral_image_i32(p);
    }

    {
        const unsigned char *p = ral_image_at(&im, 0x005C6E84u, 4);
        if (!p) {
            if (err) snprintf(err, errlen,
                              "0x005C6E84 (the pass count) is not backed by "
                              "raw data");
            rc = -3;
        } else out->passes = ral_image_i32(p);
    }

    {
        const unsigned char *p = ral_image_at(&im, 0x006CEF1Cu, 24 * 4);
        int c;
        if (!p) {
            if (err) snprintf(err, errlen,
                              "0x006CEF1C (the car model table) is not "
                              "backed by raw data");
            rc = -3;
        } else {
            for (c = 0; c < 24; c++)
                out->car_model[c] = ral_image_i32(p + 4 * c);
        }
    }

    {
        const unsigned char *p = ral_image_at(&im, 0x005C6C38u, 4);
        if (!p) {
            if (err) snprintf(err, errlen,
                              "0x005C6C38 (body-contact threshold) is not "
                              "backed by raw data");
            rc = -3;
        } else out->body_contact_threshold = ral_image_i32(p);
    }

    /* A self-check that can fail, exactly as dt_consts_load has one.  These
     * are absolute retail 5.25 addresses and a differently built image reads
     * plausible garbage out of them; the retail/5.30 delta is NOT constant
     * so this is not offsettable.
     *
     * Three independent tests, not one, because a single value can be right by
     * coincidence: the timestep, the fixed-point scale (which is the ONLY
     * value here whose wrongness would be invisible in the physics and fatal
     * in the positions), and the zero the vector normaliser compares against. */
    if (rc == 0) {
        if (!(out->dt > 0.0099f && out->dt < 0.0101f)) {
            if (err) snprintf(err, errlen,
                "[0x005C65C4] reads %g, not the 0.01 timestep - these are "
                "retail 5.25 addresses and this image is not that build",
                (double)out->dt);
            rc = -4;
        } else if (out->pos_scale != 65536.0f) {
            if (err) snprintf(err, errlen,
                "[0x0057B01C] reads %g, not the 65536.0 fixed-point scale",
                (double)out->pos_scale);
            rc = -4;
        } else if (out->zero != 0.0f) {
            if (err) snprintf(err, errlen,
                "[0x0057B014] reads %g, not the 0.0 the vector normaliser "
                "compares lengths against", (double)out->zero);
            rc = -4;
        }
    }

    ral_image_close(&im);
    return rc;
}

/* ---- FUN_0041E368: the sine table -------------------------------------- */

void sim_sin_build(float *table)
{
    int i;
    /* The builder is
     *     [0x5d6ea0] = i ; FILD ; FLDPI ; FMULP        ; i * pi
     *     [0x5d6ea0] = 0x800 ; FILD ; FDIV ST(1),ST(0) ; / 2048
     *     FSIN ; FST [ecx*4 + 0x009C3EE4]
     * for i = 0 .. 0x13FF.  0x1400 = 5,120 = a full turn (4,096) plus a
     * quarter (1,024) of overrun, so the +0x1000 cosine read never needs a
     * bounds check -.
     *
     * FST, not FSTP, and to a dword slot: each entry is a float32. */
    for (i = 0; i < SIM_SIN_ENTRIES; i++)
        table[i] = (float)sin((double)i * 3.14159265358979323846 / 2048.0);
}

float sim_steer_sin(const float *table, int32_t usteerangle)
{
    /* SHR EDX,0x14 - LOGICAL.  A negative steering angle becomes a large
     * index, which lands in the table's second half where the sine is
     * negative.  A port that writes `>> 20` on a signed int gets an
     * arithmetic shift, a negative index, and undefined behaviour. */
    uint32_t idx = (uint32_t)usteerangle >> 20;
    /* A 32-bit value shifted right by 20 is at most 4,095, and the table has
     * 5,120 entries, so the read is in bounds by construction and there is no
     * mask here to hide a wrong shift. */
    return table[idx];
}

/* ---- FUN_0041FC71 / FD06: normalise and cross -------------------------- */

void sim_vec3_normalise(const float in[3], float out[3], const sim_consts *k)
{
    /* len is ONE FSTP over x^2+y^2+z^2 then FSQRT, and is then STORED to a
     * float32 scratch ([0x005D6EA0]) and RELOADED for each of the three
     * divisions.  So the divisor is the rounded length, not the 80-bit one -
     * this is a place the original does store between terms, and copying that
     * matters as much as not copying it elsewhere. */
    float len = st(sqrt((double)in[0] * in[0]
                      + (double)in[1] * in[1]
                      + (double)in[2] * in[2]));
    int i;

    if (len == k->zero) {           /* FCOMP against [0x0057B014] = 0.0 */
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    for (i = 0; i < 3; i++) {
        out[i] = in[i] / len;
        /* CMP dword [edx+n],0x80000000 / MOV 0 - negative zero is rewritten
         * as positive zero, per component.  Three CMP/MOV pairs at
         * 0x0041FCC2, 0x0041FCD0, 0x0041FCE0. */
        if (out[i] == 0.0f) out[i] = 0.0f;   /* -0.0 becomes +0.0 */
    }
}

/* ---- FUN_0041FFFB / 0041E9C6: length and the in-place normaliser -------- */

float sim_vec3_len_q(const float v[3])
{
    /* FLD x FMUL ST0, FLD y FMUL ST0, FLD z FMUL ST0, FADDP, FADDP, FSQRT:
     * the two FADDPs add z^2 onto y^2 first and x^2 last.  The result is left
     * on the FPU; every caller stores it once. */
    return st(sqrt((double)v[0] * v[0]
                   + ((double)v[1] * v[1] + (double)v[2] * v[2])));
}

float sim_normalise_q(float v[3])
{
    /* Same squares and FADDP order as FUN_0041FFFB, then FCOM [0x0057B014]
     * (0.0) / FNSTSW / AND AH,0x40: C3 is set for EQUAL and for UNORDERED,
     * so both an exact zero and a NaN length take the zeroing arm
     * (0x0041EA66).  Otherwise the divisor is the length STILL ON THE FPU
     * (FDIV ST(0),ST(1)), not a stored float - the difference from
     * sim_vec3_normalise, which reloads a float32 length. */
    double len = sqrt((double)v[0] * v[0]
                      + ((double)v[1] * v[1] + (double)v[2] * v[2]));
    int i;

    if (len == 0.0 || len != len) {
        v[0] = v[1] = v[2] = 0.0f;
        return st(len);
    }
    for (i = 0; i < 3; i++) {
        v[i] = st((double)v[i] / len);
        /* CMP dword,0x80000000 / MOV 0, three times from 0x0041EA23. */
        if (v[i] == 0.0f) v[i] = 0.0f;
    }
    return st(len);
}

void sim_speed_len_q(car_state *car)
{
    /* FUN_0043A22F: LEA ESI,[0x006CCDFC] (car+0x198) / CALL 0x0041FFFB /
     * FSTP [0x006CCE28] (car+0x1C4). */
    car->speed_len = sim_vec3_len_q(car->newspd);
}

/* ---- FUN_0043A3DE / FUN_0042003C / FUN_0041FF84: the binary angles ------ */

/* FUN_0041FF84.  The argument arrives on the FPU (so, for the roll, UNROUNDED)
 * and is FST'd to [0x005D6EA0]; that float is clamped to +-[0x0057AFBC] - the
 * constant is negated in place for the lower test and negated back - and the
 * result is FPATAN(argument, sqrt(1 - clamped^2)).  The NUMERATOR is the
 * unclamped argument: for |x| > 1 the root is 0 and the angle is +-pi/2.
 * An unordered compare fails the first test (JB taken, no clamp) and then
 * clamps to -1 (JAE not taken). */
static double asin_q(double x, const sim_consts *k)
{
    float xc = (float)x;
    float lim = k->asin_limit;

    if (!(xc < lim)) xc = lim;
    if (!(xc >= -lim)) xc = -lim;
    return atan2(x, sqrt(1.0 - (double)xc * xc));
}

void sim_binary_angles_q(const car_state *car, const sim_consts *k,
                         sim_world *w)
{
    static const float forward[3] = { 0.0f, 0.0f, 1.0f };  /* FLD1 FLDZ FLDZ */
    static const float side[3]    = { 1.0f, 0.0f, 0.0f };  /* FLDZ FLDZ FLD1 */
    float v[3], heading, pitch, roll;
    const float *angle[3];
    int i;

    /* 0x0043A3DE: CMP [0x006CD354],0 / JNE run ; CMP [0x006CCEB8],0 / JNE
     * skip.  car+0x6F0 is the view flag, car+0x254 the all-airborne gate. */
    if (w->camera_view_2_3 == 0 && car->yaw_couple_gate != 0) return;

    /* FUN_0042003C on the car matrix (LEA ESI,[0x006CCD18] = car+0xB4). */
    sim_vec3_by_matrix(forward, car->matrix, v);            /* FSTP x3 */
    heading = st(atan2((double)v[0], (double)v[2]));        /* 0x00420065 */
    /* The second call pushes the same vector again. */
    sim_vec3_by_matrix(forward, car->matrix, v);
    pitch = st(-asin_q((double)v[1], k));                   /* FCHS, FSTP */
    sim_vec3_by_matrix(side, car->matrix, v);
    /* FLD v.y / FLD pitch / FCOS / FDIVP: the quotient stays on the FPU. */
    roll = st(asin_q((double)v[1] / cos((double)pitch), k));

    /* 0x0043A401..0x0043A469: FLD angle, FMUL 65536, FMUL 4096, FMUL 1/2pi,
     * FISTP, SHL 4.  [0x006CD628] from the pitch, [0x006CD624] from the
     * heading, [0x006CD62C] from the roll. */
    angle[0] = &heading; angle[1] = &pitch; angle[2] = &roll;
    for (i = 0; i < 3; i++)
        w->binary_angle[i] = (int32_t)((uint32_t)fistp_rne(
                                 (double)*angle[i] * (double)k->pos_scale
                                 * (double)k->angle_steps
                                 * (double)k->inv_two_pi) << 4);
}

void sim_vec3_cross(const float b[3], const float c[3], float out[3])
{
    /* out = cross(b, c).  Each component is FLD/FMUL/FLD/FMUL/FSUBP/FSTP -
     * one rounding over the difference of two products. */
    out[0] = st((double)c[2] * b[1] - (double)b[2] * c[1]);
    out[1] = st((double)c[0] * b[2] - (double)b[0] * c[2]);
    out[2] = st((double)c[1] * b[0] - (double)b[1] * c[0]);
}

/* ---- FUN_0041FD92: the axis-angle matrix, sqrt bug and all -------------- */

void sim_axis_angle_matrix_q(const float axis[3], float angle, float m[16])
{
    double a  = (double)angle;
    float  c  = (float)cos(a);          /* FSINCOS, then two FSTPs: cos first */
    float  s  = (float)sin(a);
    float  t  = st(1.0 - (double)c);    /* FLD1 / FSUB / FSTP                 */
    float  txz, txy, tyz, xs, ys, zs;

    /* Six scratch products, each its own FSTP - 0x0057C324..0x0057C338. */
    txz = st((double)t * axis[0] * axis[2]);
    txy = st((double)t * axis[0] * axis[1]);
    tyz = st((double)t * axis[1] * axis[2]);
    xs  = st((double)axis[0] * s);
    ys  = st((double)axis[1] * s);
    zs  = st((double)axis[2] * s);

    memset(m, 0, 16 * sizeof(float));

    /* >>> THE DIAGONAL IS sqrt(|axis|), NOT axis*axis. <<<
     *
     * 0x0041FE32: FLD [0x57CB7C] / FABS / FSQRT / FMUL t / FADD c / FSTP [edi]
     * and the same at 0x0041FE77 and 0x0041FEBD for y and z.  A Rodrigues
     * matrix has  x*x*(1-cos) + cos  there; a square on this FPU is FMUL ST(0)
     * (d8 c8), which is what vec3_len two functions away uses.  This is d9 e1
     * d9 fa - FABS then FSQRT.  It is not a mis-decode and it is not a
     * rotation matrix.
     *
     * Which is WHY the caller orthonormalises the result before composing it:
     * recorded that step as a fact with no reason
     * attached, and this is the reason.  A port that "repairs" the sqrt
     * changes the delta rotation every car gets on every step, and the fix
     * would be invisible in any single frame. */
    m[0]  = st(sqrt(fabs((double)axis[0])) * t + c);
    m[1]  = st((double)txy + zs);
    m[2]  = st((double)txz - ys);

    m[4]  = st((double)txy - zs);
    m[5]  = st(sqrt(fabs((double)axis[1])) * t + c);
    m[6]  = st((double)tyz + xs);

    m[8]  = st((double)txz + ys);
    m[9]  = st((double)tyz - xs);
    m[10] = st(sqrt(fabs((double)axis[2])) * t + c);

    /* FLDZ / FST / FST / FSTP into +0x30, +0x34, +0x38: the translation row.
     * The fourth COLUMN (+0x0C, +0x1C, +0x2C, +0x3C) is never written by this
     * function at all.  Measured over the capture, car0's matrix holds 0.0 in
     * all seven of those slots in every one of 36,754 frames, so this port
     * zeroes them rather than modelling a stale-scratch read. */
}

/* ---- FUN_0041FC0D: orthonormalise -------------------------------------- */

void sim_matrix_orthonormalise(float m[16], const sim_consts *k)
{
    float row2[3], row0[3], row1[3];

    /* Gram-Schmidt outward from row 2 - the +0x20 row, which for this matrix
     * is the car's forward axis.  Note the ORDER: row 2 is the one kept,
     * row 0 is rebuilt from it and the old row 1, and row 1 is rebuilt last
     * from the two new ones, so row 1 carries all of the correction. */
    sim_vec3_normalise(&m[8], row2, k);                  /* normalise(row2)  */
    sim_vec3_cross(&m[4], row2, row0);                   /* cross(row1,row2')*/
    {
        float tmp[3];
        tmp[0] = row0[0]; tmp[1] = row0[1]; tmp[2] = row0[2];
        sim_vec3_normalise(tmp, row0, k);
    }
    sim_vec3_cross(row2, row0, row1);                    /* cross(row2',row0')*/

    m[0] = row0[0]; m[1] = row0[1]; m[2]  = row0[2];
    m[4] = row1[0]; m[5] = row1[1]; m[6]  = row1[2];
    m[8] = row2[0]; m[9] = row2[1]; m[10] = row2[2];

    /* The original's final REP MOVSD copies 16 dwords out of a scratch block
     * whose fourth column and fourth row it never writes.  Measured zero in
     * every captured frame - see sim_axis_angle_matrix_q's tail. */
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = m[13] = m[14] = m[15] = 0.0f;
}

/* ---- FUN_00425E1D: compose --------------------------------------------- */

void sim_matrix_mul(const float a[16], const float b[16], float out[16])
{
    int r, c;

    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            /* THE GROUPING IS THE x87 STACK'S, NOT A CHOICE.  The three
             * products are pushed in order 0,1,2 and then `faddp st(1)` runs
             * twice, so 1 and 2 add first and 0 is added to that sum.  One
             * FSTP over the whole thing. */
            double p0 = (double)a[r * 4 + 0] * b[0 * 4 + c];
            double p1 = (double)a[r * 4 + 1] * b[1 * 4 + c];
            double p2 = (double)a[r * 4 + 2] * b[2 * 4 + c];
            out[r * 4 + c] = st(p0 + (p1 + p2));
        }
        /* Then b's fourth row is added to the output's fourth row, as a
         * translation - and it is added to the ALREADY STORED float32
         * (FLD [edi+0x30] / FADD [ebp-0x10] / FSTP [ebp-0x10]), so this one is
         * a second rounding and is written as one. */
        out[3 * 4 + c] = b[3 * 4 + c] + out[3 * 4 + c];
    }
}

/* ---- FUN_00420010 / 0044B1AB / 0041FA3B - read --------------- */

void sim_vec3_scale(float v[3], float k)
{
    /* FLD [ESI] / FMUL ST(1) / FSTP [ESI], repeated at +4 and +8: three
     * independent roundings, not one over the triple. */
    v[0] = st((double)v[0] * k);
    v[1] = st((double)v[1] * k);
    v[2] = st((double)v[2] * k);
}

void sim_vec3_by_matrix(const float v[3], const float m[16], float out[3])
{
    int c;
    /* v' = v . M.  Same x87 grouping as sim_matrix_mul - three products
     * pushed in order, the last two add first - because it is the same
     * three-multiply/two-faddp shape run on one row instead of four, then
     * m's row 3 (the translation) added by FADD float ptr [ESI+0x30..0x38]
     * onto the UNROUNDED sum, one rounding at the caller's FSTP - ~~added to
     * the ALREADY-STORED result, a second rounding~~ (read from
     * 0x0044B1BE; this code always rounded once). */
    for (c = 0; c < 3; c++) {
        double p0 = (double)v[0] * m[0 * 4 + c];
        double p1 = (double)v[1] * m[1 * 4 + c];
        double p2 = (double)v[2] * m[2 * 4 + c];
        out[c] = st((double)m[3 * 4 + c] + (p0 + (p1 + p2)));
    }
}

void sim_matrix_inverse_orthonormal(const float m[16], float out[16],
                                    const sim_consts *k)
{
    /* The nine cofactors of the 3x3 part, each ONE FSTP over a
     * product-of-products difference - the same shape as sim_vec3_cross.
     * NO division by the determinant: the caller only ever passes a matrix
     * that sim_matrix_orthonormalise has already run over, so det == 1 and
     * the adjugate already IS the inverse. */
    double a0 = m[0], a1 = m[1], a2 = m[2];
    double a3 = m[4], a4 = m[5], a5 = m[6];
    double a6 = m[8], a7 = m[9], a8 = m[10];

    memset(out, 0, 16 * sizeof(float));
    out[0]  = st(a4 * a8 - a5 * a7);
    out[1]  = st(a2 * a7 - a1 * a8);
    out[2]  = st(a1 * a5 - a2 * a4);
    out[4]  = st(a5 * a6 - a3 * a8);
    out[5]  = st(a0 * a8 - a2 * a6);
    out[6]  = st(a2 * a3 - a0 * a5);
    out[8]  = st(a3 * a7 - a4 * a6);
    out[9]  = st(a1 * a6 - a0 * a7);
    out[10] = st(a0 * a4 - a1 * a3);

    /* The original calls sim_matrix_orthonormalise on its own result before
     * returning - not repair, a cleanup pass a pure transpose would not
     * need, and the second piece of evidence (with sim_axis_angle_matrix_q's
     * own orthonormalise call) that this port's "always orthonormal"
     * assumption is the original's too. */
    sim_matrix_orthonormalise(out, k);
}

void sim_refresh_active_frame(sim_world *w, const car_state *car,
                              const sim_consts *k)
{
    sim_matrix_inverse_orthonormal(car->matrix, w->active_frame, k);
}

/* ---- FUN_0043B18B: position -------------------------------------------- */

void sim_integrate_position(car_state *car, const sim_consts *k)
{
    int i;
    /* FLD newspd / FMUL 65536.0 / FMUL dt / FISTP / ADD.  One conversion at
     * the end, under round-to-nearest; explicit (forward) Euler, one term, no
     * substepping.  The x/z/y order in the original is an instruction-schedule
     * detail: the three are independent. */
    for (i = 0; i < 3; i++)
        car->pos[i] += fistp_rne((double)car->newspd[i]
                                 * (double)k->pos_scale * (double)k->dt);
}

/* ---- FUN_0043C22F: orientation ----------------------------------------- */

void sim_integrate_orientation(car_state *car, const sim_consts *k)
{
    float len, axis[3], delta[16], composed[16];

    /* vec3_len over rotspd, ONE FSTP - to car+0x194 (0x0043C23A), which the
     * port did not store until.  The test that follows is
     * CMP dword [0x6CCDF8],0 - an INTEGER compare of the stored float's bit
     * pattern, so it is "not +0.0" exactly.  A length is a square root and
     * never -0.0, so the two readings coincide here.  FUN_0041FFFB adds the
     * last two squares first; this summed (x+y)+z until (no
     * capture step tells the two apart). */
    len = sim_vec3_len_q(car->rotspd);
    car->rotspd_len = len;

    if (len != 0.0f) {
        float angle;
        sim_vec3_normalise(car->rotspd, axis, k);
        angle = st((double)len * k->dt);     /* FLD |w| / FMUL dt / FSTP */

        sim_axis_angle_matrix_q(axis, angle, delta);
        sim_matrix_orthonormalise(delta, k);            /* <- see the sqrt   */

        sim_matrix_mul(car->matrix, delta, composed);
        memcpy(car->matrix, composed, sizeof(composed));
    }

    /* THE FINAL ORTHONORMALISE RUNS ON BOTH ARMS, including when the car is
     * not rotating at all - the `je` at 0x0043C247 jumps to it, not past it.
     * Drift is actively corrected every step, so a port that omits this
     * diverges for a reason that has nothing to do with arithmetic. */
    sim_matrix_orthonormalise(car->matrix, k);
}

/* ---- FUN_0043DC70: the divergence guard -------------------------------- */

static int exp_is_ff(float v)
{
    /* The original tests the INTEGER exponent field, not a floating compare:
     * exact, cheap, and it catches NaN and Inf together.  Reproduced through a
     * memcpy rather than a cast so it does not rely on type punning. */
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return ((u >> 23) & 0xFFu) == 0xFFu;
}

/* 0x0043DD36..0x0043DF74, per component: FLD lim / FLD v / FCOMPP / SHR AH,1
 * / JB skip, then the same against -lim (FCHS on the constant) with JAE.  C0
 * is set for "below" AND for unordered, so the first test skips a NaN and the
 * second STORES -lim over it (2026-09-30: the port's first transcription
 * returned the NaN).  v == lim stores lim, which is v. */
static float clamp_abs(float v, float lim)
{
    if (v >= lim) return lim;
    if (!(v >= -lim)) return -lim;
    return v;
}

int sim_divergence_guard(car_state *car, const sim_consts *k, float speed_cap)
{
    int fired = 0;
    int i;

    if (exp_is_ff(car->newspd[0]) || exp_is_ff(car->newspd[1])
                                  || exp_is_ff(car->newspd[2])) {
        /* The original logs "*** INFINITY" and "[car0.newxspd] =" with the
         * three values, then zeroes three triples: the velocity, the block at
         * car0+0x1A4 (cxspd/.../czspd) and the angular velocity.  The logging
         * is retail's, through the facility describes;
         * the port reports it to the caller instead. */
        car->newspd[0] = car->newspd[1] = car->newspd[2] = 0.0f;
        car->cxspd = 0.0f;
        car->cyspd_q = 0.0f;          /* [0x6CCE0C]; missed until 2026-09-30 */
        car->czspd = 0.0f;
        car->rotspd[0] = car->rotspd[1] = car->rotspd[2] = 0.0f;
        fired = 1;
    }

    for (i = 0; i < 3; i++) car->rotspd[i] = clamp_abs(car->rotspd[i], k->clamp_ang);
    for (i = 0; i < 3; i++) car->newspd[i] = clamp_abs(car->newspd[i], k->clamp_lin);

    /* A RETRIED STEP CANNOT END FASTER THAN IT STARTED THE LOOP.  0x0043DF76:
     * FUN_0041FFFB on newspd - the WHOLE vector's length, x^2 + (y^2 + z^2) -
     * FSTP float [0x5D6E50]; FLD cap [0x5C6E38] / FCOMP len / JAE out, so the
     * cap applies only when cap < len (a NaN length: C0 set, so it applies).
     * Then cap / len stays on the FPU, and FUN_00420010 multiplies BOTH
     * triples by it, newspd [0x6CCDFC..04] and cxspd..czspd [0x6CCE08..10],
     * each product rounded once at its FSTP.  Retail calls this only after a
     * retry or on exhaustion, so a no-retry step is
     * not capped at all.
     *
     * ~~compares and scales newspd.x alone, and cxspd~~ - the
     * transcription, never re-read until 2026-09-30.  Found by
     * twocar_check: after a car-versus-car push the current car's newspd
     * ended at the push's DIRECTION and the pre-loop SPEED on run 20, where
     * the x-only cap left the push whole (s11). */
    {
        float len = sim_vec3_len_q(car->newspd);
        if (!(speed_cap >= len)) {
            sim_x87 ratio = sim_x87_div(sim_x87_from_float(speed_cap),
                                        sim_x87_from_float(len));
            float *tri[2][3];
            int t;
            tri[0][0] = &car->newspd[0]; tri[0][1] = &car->newspd[1];
            tri[0][2] = &car->newspd[2];
            tri[1][0] = &car->cxspd; tri[1][1] = &car->cyspd_q;
            tri[1][2] = &car->czspd;
            for (t = 0; t < 2; t++)
                for (i = 0; i < 3; i++)
                    *tri[t][i] = sim_x87_to_float(
                        sim_x87_mul(sim_x87_from_float(*tri[t][i]), ratio));
        }
    }
    return fired;
}
