/* car_load.c - see car_load.h; the arcade's allowance. */

#include "car_load.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void put32(car_state *car, unsigned off, uint32_t v)
{
    memcpy((unsigned char *)car + off, &v, sizeof(v));
}

int car_load_tables_read(const ral_image *im, car_load_tables *t, char *err,
                         size_t n)
{
    const unsigned char *a = ral_image_at(im, 0x005B8758u, sizeof t->autosetup);
    const unsigned char *l = ral_image_at(im, 0x005B8B14u, sizeof t->ltab);
    const unsigned char *r = ral_image_at(im, 0x005C6C44u, 4);
    const unsigned char *s = ral_image_at(im, 0x005B5794u, 4u * SIM_RACE_ALLOWANCE_N);
    const unsigned char *pc = ral_image_at(im, 0x005B8A28u, 4u * CAR_LOAD_STAGES);
    const unsigned char *cf = ral_image_at(im, 0x005B8AB8u, 4u * 23u);
    int i, k;
    if (!a || !l || !r || !s || !pc || !cf) {
        snprintf(err, n, "the car's load tables (0x005B8758, 0x005B8B14, "
                         "0x005C6C44, 0x005B5794, 0x005B8A28, 0x005B8AB8) are "
                         "not in the image");
        return -1;
    }
    for (i = 0; i < CAR_LOAD_STAGES; i++)
        for (k = 0; k < 4; k++)
            t->autosetup[i][k] = ral_image_i32(a + 16 * i + 4 * k);
    memcpy(t->ltab, l, sizeof t->ltab);
    t->target_revs = ral_image_f32(r);
    for (i = 0; i < SIM_RACE_ALLOWANCE_N; i++)
        t->allow_boot[i] = ral_image_i32(s + 4 * i);
    for (i = 0; i < CAR_LOAD_STAGES; i++) t->pct[i] = ral_image_i32(pc + 4 * i);
    for (i = 0; i < 23; i++) t->carfactor[i] = ral_image_f32(cf + 4 * i);
    return 0;
}

void car_load_in_default(car_load_in *in)
{
    memset(in, 0, sizeof *in);
    in->gmode = 0;
    in->numcars = 1;
    in->gearbox = 1;                  /* the stand-in's, run 18's autom 1 */
    in->sliders.tyre = 3;  in->sliders.bbal = 50; in->sliders.seth = 70;
    in->sliders.sets = 30; in->sliders.stes = 50; in->sliders.setg = 2;
    in->hud_mask = 0xF7;
    in->carparts = -1;
}

/* [0x005B5794..] and [0x005B5FB0] totatime: process state, as in the game -
 * FUN_004363C3 writes only the row it finds, so the rest keep what they
 * held (the image's at boot). */
static int     allow_set;
static int32_t allow[SIM_RACE_ALLOWANCE_N];
static int32_t totatime;

const int32_t *car_load_allowance(void)
{
    return allow_set ? allow : NULL;
}

/* 0x0043643D / 0x00436477: three or two ASCII digits at c; CF (here 0) on a
 * non-digit, EAX = 0. */
static int digits(const car_load_tables *t, uint32_t c, int nd, int32_t *v)
{
    int32_t x = 0;
    int k;
    *v = 0;
    for (k = 0; k < nd; k++) {
        uint32_t d;
        if (c + (uint32_t)k >= sizeof t->ltab) return 0;   /* past the copy */
        d = (uint32_t)(unsigned char)(t->ltab[c + (uint32_t)k] - 0x30);
        if (d >= 10) return 0;                             /* JAE 0x0043649D */
        x = x * 10 + (int32_t)d;
    }
    *v = x;
    return 1;
}

/* FUN_004363C3, 0x004363C3..0x0043643C */
static void parse_row(const car_load_tables *t, int32_t track)
{
    const uint32_t len = 0x708;                       /* [0x005BE004] */
    uint32_t c = 0;
    int32_t v;
    int i = 0;
    totatime = 0;                                     /* [0x005B5FB0] */
    for (;;) {
        while (t->ltab[c] != 0x6C && t->ltab[c] != 0x4C) {   /* 'l', 'L' */
            if (++c >= len) return;
        }
        c++;
        digits(t, c, 2, &v);       /* 0x004363FF: the CF is not tested - a */
        if (v == track) break;     /* failed read compares as 0             */
        if (++c >= len) return;
    }
    for (c += 3; c < len; c += 4) {
        if (!digits(t, c, 3, &v)) break;           /* JC 0x0043643C */
        if (i < SIM_RACE_ALLOWANCE_N) allow[i] = v;   /* [EBP*4 + 0x5B5794] */
        i++;
        totatime += v;
    }
}

/* FUN_00436222's ladder, 0x00436248..0x004362B0: the stage ordinal, or -1
 * (the RET at 0x0043626B).  JB: unsigned.  Bounds ten apart, groups six
 * wide - the championship order. */
static int32_t ladder(int32_t track)
{
    static const struct { uint32_t below, sub; int32_t event; } L[6] = {
        { 0x1D, 0x15, 0 }, { 0x27, 0x1F, 3 }, { 0x31, 0x29, 5 },
        { 0x3B, 0x33, 1 }, { 0x45, 0x3D, 4 }, { 0x4F, 0x47, 2 }
    };
    int k;
    for (k = 0; k < 6; k++)
        if ((uint32_t)track < L[k].below)
            return (int32_t)((uint32_t)track - L[k].sub) + 6 * L[k].event;
    return -1;
}

/* FISTP under the default control word: round to nearest, even. */
static int32_t fistp(long double v)
{
    return (int32_t)llrintl(v);
}

int32_t car_load_extra_time(const car_load_tables *t, int32_t track,
                            int32_t numcheckposits, int32_t tot)
{
    int32_t idx = ladder(track);
    (void)numcheckposits;
    if (idx < 0 || idx >= CAR_LOAD_STAGES) return -1;
    /* FILD pct / FIMUL totatime / FDIV [0x0057AF98] 100.0f / FISTP */
    return fistp((long double)t->pct[idx] * (long double)tot / 100.0L);
}

/* FUN_00436222, 0x00436222..0x004363C2 */
static void arcade_allowance(const car_load_tables *t, int32_t track,
                             int32_t carnum, int32_t numcheckposits)
{
    int32_t c = carnum, extra, i;
    float f;
    if (c < 0) c = 0;                                 /* CMP 0 / JNS      */
    if (c >= 0x16) c = 0x16;                          /* CMP 0x16 / JS    */
    f = t->carfactor[c];                              /* [0x005B5FC4]     */
    extra = car_load_extra_time(t, track, numcheckposits, totatime);
    if (extra < 0) return;                            /* off the ladder   */
    allow[0] += extra;                                /* 0x0043634E       */
    /* 0x00436354..0x004363C0: a do-while over [0, numcheckposits) - with
     * numcheckposits <= 0 the original runs on past the array; the port
     * stops at its end */
    i = 0;
    do {
        allow[i] = fistp((long double)allow[i] * (long double)f);
        i++;
    } while (i != numcheckposits && i < SIM_RACE_ALLOWANCE_N);
}

int car_load_autosetup_index(int32_t track)
{
    /* 0x004359AA: CMP EAX,0x14 / JC; CMP EAX,0x50 / JNC - unsigned */
    uint32_t u = (uint32_t)track, q, r, idx;
    if (u < 0x14u || u >= 0x50u) return -1;
    q = (u - 0x15u) / 10u;                          /* SUB 0x15; DIV 10 */
    r = (u - 0x15u) % 10u;
    idx = q * 6u + r;                               /* IMUL 6; ADD EDX */
    return idx < CAR_LOAD_STAGES ? (int)idx : -1;
}

void car_load(car_state *car, const car_load_in *in, const car_load_tables *t,
              const car_params *p, int32_t numcollpts)
{
    setup_car_words w;
    int idx;

    memset(car, 0, sizeof(*car));
    if (!allow_set) {
        memcpy(allow, t->allow_boot, sizeof allow);
        allow_set = 1;
    }

    /* graham_to_shaun, 0x004451F5..0x004453F8 (setup.c is its scaling,
     * gated by setup_check); 0x0044550B the HUD mask; 0x00445A79
     * carpartsleft.  The tyre goes to [0x5D64C8] (0x0044503D), not the car. */
    setup_car_words_from(&in->sliders, &w);
    car->ste_s_1 = w.ste_s;
    car->b_bal_1 = w.b_bal;
    car->set_h_1 = w.set_h;
    car->set_s_1 = w.set_s;
    car->set_g_1 = w.set_g;
    put32(car, 0x220, in->hud_mask);
    car->carpartsleft = in->carparts;

    /* init_car_setup_q's car load, 0x0042023D..0x0042024C */
    car->maxrevs = p->maxrevs;                     /* FILD [ESI+0x74] */
    memcpy((unsigned char *)car + 0x1E0, &t->target_revs, 4);
    /* the object loader, 0x00425BEA */
    car->numcollpts = numcollpts;

    /* gamemodestuff_init, 0x00434BBE: the mode's routine */
    if (in->gmode == 0 || in->gmode == 2)
        parse_row(t, in->track);                   /* 0x004363C3 */
    if (in->gmode == 2)                            /* 0x00434BF1 */
        arcade_allowance(t, in->track, in->carnum, in->numcheckposits);

    /* FUN_00434FC3, slot 0 */
    car->driver = 0 < in->numcars ? 1 : 0;
    car->autom_1 = 1;
    car->catileft = allow[0] * 100;                 /* 0x004350CF */
    car->carnum1 = in->carnum;                      /* 0x004350E7 */

    /* FUN_00435148 */
    car->carnum1 = in->carnum;
    car->tyretype = in->sliders.tyre;               /* [0x5D64C8], raw */
    car->autom_1 = in->gearbox;

    if (in->gmode == 1)
        car->catitken = in->champ88;                /* 0x00434F1F */

    /* FUN_0043595A, the auto-setup: gmode 0 (0x00434F90) and 2 (0x00434E1B) */
    if (in->gmode == 0 || in->gmode == 2) {
        car->b_bal_1 = 0x80;
        car->ste_s_1 = 0x80;
        car->set_h_1 = 0x80;
        car->set_s_1 = 0x80;
        car->tyretype = -1;
        car->set_g_1 = 2;
        idx = car_load_autosetup_index(in->track);
        if (idx >= 0) {
            car->set_h_1 = t->autosetup[idx][0];
            car->set_s_1 = t->autosetup[idx][1];
            car->set_g_1 = t->autosetup[idx][3] - 1;
            car->tyretype = t->autosetup[idx][2];
        }
    }
}

void car_load_rival(car_state *car, int slot, int32_t type, int32_t gearbox2,
                    const car_load_in *in, const car_load_tables *t,
                    const car_params *p, int32_t numcollpts)
{
    int idx;

    memset(car, 0, sizeof(*car));
    /* init_car_setup_q and the object loader, as for car 0 */
    car->maxrevs = p->maxrevs;
    memcpy((unsigned char *)car + 0x1E0, &t->target_revs, 4);
    car->numcollpts = numcollpts;
    /* FUN_00434FC3 for the slot: driver = slot < numcars (4), autom 1, the
     * clock's catileft, the car type from shauncarnum2..4 (0x004350F3..) */
    car->autom_1 = 1;
    car->catileft = allow[0] * 100;
    car->carnum1 = type;
    /* arcade_game_mode_init 0x00434D35..0x00434D67: +0x3C = 0x80, driver 0 */
    car->ste_s_1 = 0x80;
    car->driver = 0;
    /* FUN_00435148: slot 1 is player 2's - +0x2C [0x5D64CC] (0 from
     * 0x00444BF1) and +0x38 [0x5D6748], player 2's gearbox */
    if (slot == 1) {
        car->tyretype = 0;
        car->autom_1 = gearbox2;
    }
    /* FUN_0043595A on every record (0x00434E15..0x00434E3C) */
    car->b_bal_1 = 0x80;
    car->ste_s_1 = 0x80;
    car->set_h_1 = 0x80;
    car->set_s_1 = 0x80;
    car->tyretype = -1;
    car->set_g_1 = 2;
    idx = car_load_autosetup_index(in->track);
    if (idx >= 0) {
        car->set_h_1 = t->autosetup[idx][0];
        car->set_s_1 = t->autosetup[idx][1];
        car->set_g_1 = t->autosetup[idx][3] - 1;
        car->tyretype = t->autosetup[idx][2];
    }
}
