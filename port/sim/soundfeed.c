/* soundfeed.c - the step's half of the car's sounds: soundfeed.h.
 * Each function names the original's it transcribes;
 * the reading is. */

#include "soundfeed.h"

#include <math.h>
#include <string.h>

/* [0x006847E0], `python tools/carsound.py events`: impact1..9, impacta,
 * impactb for materials 0..11, impact1 for 12..15. */
const int32_t sim_sound_impact_by_material[16] = {
    0xAB, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
    0xB2, 0xB3, 0xB4, 0xB5, 0xAB, 0xAB, 0xAB, 0xAB
};

void sim_sound_feed_init(sim_sound_feed *f)
{
    memset(f, 0, sizeof *f);
    f->impact_id = SIM_SOUND_NONE;           /* 0x0040D2FF */
    f->scrape_id = SIM_SOUND_NONE;           /* 0x0040D309 */
}

/* FISTP under the default control word: round half to even. */
static int32_t fistp(double x)
{
    return (int32_t)lrint(x);
}

/* FILD a - FISUB b on three 16.16 pairs, FUN_0044B1AB with the view rotation
 * (row vector x M + row 3), each component x [0x0057B024] (2^-16), FSTP.
 * The differences are exact in the x87; the products and sums are held in
 * double here, one rounding at the store. */
static void to_view(const sim_sound_view *v, const int32_t a[3],
                    const int32_t off[3], float out[3])
{
    double d[3];
    int i;
    for (i = 0; i < 3; i++)
        d[i] = (double)a[i] + (off ? (double)off[i] : 0.0) - (double)v->pos[i];
    for (i = 0; i < 3; i++)
        out[i] = (float)((d[0] * v->rot[i] + d[1] * v->rot[4 + i]
                          + d[2] * v->rot[8 + i] + v->rot[12 + i])
                         * (1.0 / 65536.0));
}

/* The same product on a float vector, no scaling: the forward axis. */
static void turn(const sim_sound_view *v, const float a[3], float out[3])
{
    int i;
    for (i = 0; i < 3; i++)
        out[i] = (float)((double)a[0] * v->rot[i] + (double)a[1] * v->rot[4 + i]
                         + (double)a[2] * v->rot[8 + i] + v->rot[12 + i]);
}

/* 0x0045A2D9: the shift result car+0x238 sets a trigger (and "new data"). */
static void shift_triggers(const car_state *car, sim_sound_feed *f)
{
    if (car->shift_result == 1) { f->gearchng = 1; f->fresh = 1; }
    if (car->shift_result == -1) { f->geargrnd = 1; f->fresh = 1; }
}

void sim_sound_step_q(car_state *car, float idle, int32_t material,
                      int32_t camera_view_2_3, sim_sound_feed *f,
                      const sim_sound_view *v)
{
    float r;
    int32_t s, i;

    /* FUN_0043C14D.  0x0043C14D..0x0043C177: the HUD speed car+0xAB0 (not
     * here: hud.c's).  0x0043C17D: |newspd| = FSQRT(z^2 + (x^2 + y^2)) x
     * [0x0057AF84] (2.25), FSTP car+0x294. */
    car->speed_snd = (float)(sqrt((double)car->newspd[2] * car->newspd[2]
                                  + ((double)car->newspd[0] * car->newspd[0]
                                     + (double)car->newspd[1] * car->newspd[1]))
                             * 2.25);
    /* 0x0043C1A7: the revs, floored at [0x005C6E30] (FCOMPP / C0 / JAE:
     * below or unordered takes the idle), FISTP, eased 50/256 by SAR 8,
     * clamped 2000..12000 by JNS / JS on the difference. */
    r = car->revs;
    if (!(r >= idle)) r = idle;
    s = car->sound_revs;
    s += (int32_t)((fistp((double)r) - s) * 50) >> 8;
    if (s - 2000 < 0) s = 2000;
    if (s - 12000 >= 0) s = 12000;
    car->sound_revs = s;

    /* 0x0043C224: 0x0045A626.  Its car-record half first, in its order. */
    car->skid_snd = car->skidamt / 1.0f;        /* FDIV [0x005C6C20] (1.0)   */
    car->roll_snd = 1.0f;                       /* FLD1                      */
    if (car->yaw_couple_gate != 0) {            /* car+0x254: all four up    */
        car->skid_snd = 0.0f;
        car->roll_snd = 0.0f;
        car->speed_snd = 0.0f;
    }
    if (f) {
        int32_t t;
        f->fresh = 1;                                      /* +0x10          */
        f->revs = car->sound_revs;                         /* +0x14          */
        for (i = 0; i < 4; i++) {
            f->w[i].material = material;                   /* +0x54 ..       */
            f->w[i].gate = car->roll_snd;                  /* +0x4C ..       */
            f->w[i].skid = (float)((double)car->skid_snd * 256.0);  /* +0x50 */
            f->w[i].speed = car->speed_snd;                /* +0x44 ..       */
        }
        t = car->throttle;                                 /* JNS / JS       */
        if (t < 0) t = 0;
        if (t - 0x100 >= 0) t = 0x100;
        f->throttle = t;                                   /* +0x18          */
        f->gear = car->gear;                               /* +0xE8          */
        f->whl = car->wheelspd_mean;                       /* +0x1C: the
                                                            * PREVIOUS step's */
        f->airborne = car->front_airborne & car->drag_gate_q;  /* +0xAC      */
        f->outside = camera_view_2_3 ^ 1;                  /* +0x0C          */
        if (v && v->pos) {
            to_view(v, car->pos, NULL, f->pos);            /* +0x2C          */
            turn(v, &car->matrix[8], f->fwd);              /* +0x38: +0xD4.. */
        }
        /* 0x0043C229: 0x0045A2D9. */
        shift_triggers(car, f);
    }
}

void sim_sound_wheel_mean_q(car_state *car)
{
    car->wheelspd_mean = (float)(((double)car->fwhlspd + car->rwhlspd) * 0.5);
}

/* The volume helpers 0x0045ABEF / 0x0045ABBF / 0x0045AC49 / 0x0045AC79:
 * FMUL k / FISTP / SUB d / clamped 0..64 by JNS / JS. */
static int32_t volume(double x, double k, int32_t d)
{
    int32_t b = fistp(x * k) - d;
    if (b < 0) b = 0;
    if (b - 0x40 >= 0) b = 0x40;
    return b;
}

static void impact(const car_state *car, sim_sound_feed *f,
                   const sim_sound_view *v, int32_t id, int32_t vol,
                   const int32_t off[3])
{
    if (vol == 0) return;                                  /* CMP EBX,0 / JZ */
    f->impact_id = id;                                     /* +0x238         */
    f->impact_vol = vol;                                   /* +0x23C         */
    if (v && v->pos) to_view(v, car->pos, off, f->impact_pos);
    f->fresh = 1;
}

void sim_sound_object_hit_q(const car_state *car, sim_sound_feed *f,
                            const sim_sound_view *v, int32_t id,
                            const int32_t off[3])
{
    if (!f) return;
    /* 0x0045ABEF: [0x006CCE28] car+0x1C4 x [0x00684830] (50) - 10 */
    impact(car, f, v, id, volume(car->speed_len, 50.0, 10), off);
}

void sim_sound_body_hit_q(const car_state *car, sim_sound_feed *f,
                          const sim_sound_view *v, float normal)
{
    if (!f) return;
    /* 0x0045ABBF: [0x00685E2C] x [0x00684830] (50) - 1 */
    impact(car, f, v, 0xB5, volume(normal, 50.0, 1), NULL);
}

void sim_sound_scrape_q(const car_state *car, sim_sound_feed *f,
                        const sim_sound_view *v)
{
    if (!f) return;
    f->scrape_id = 0xB9;                                   /* +0x24C         */
    /* 0x0045AC79: car+0x1C4 x [0x00684834] (50) - 1 */
    f->scrape_vol = volume(car->speed_len, 50.0, 1);       /* +0x250         */
    if (v && v->pos) to_view(v, car->pos, NULL, f->scrape_pos);
    f->fresh = 1;
}

void sim_sound_blowout_q(const car_state *car, sim_sound_feed *f,
                         const sim_sound_view *v)
{
    if (!f) return;
    f->blowout = 1;                                        /* +0x224         */
    if (v && v->pos) to_view(v, car->pos, NULL, f->blowout_pos);
    f->fresh = 1;                                          /* +0x10          */
}

void sim_sound_scrape_end_q(sim_sound_feed *f)
{
    if (!f) return;
    f->scrape_id = 0xB9;                                   /* 0x0045ABA2     */
    f->scrape_vol = 0;
    f->fresh = 1;
}
