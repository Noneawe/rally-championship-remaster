/* carsound.c - the audio thread's half of the car's sounds: carsound.h.
 * Every function names the original's it transcribes;
 * the reading is.  x87 chains are held in double and
 * rounded where the original stores (FSTP float, FISTP int). */

#include "carsound.h"
#include "sound.h"
#include "image.h"

#include <math.h>
#include <string.h>

/* ---- the image's tables ------------------------------------------------ */

static int rd32(const ral_image *im, uint32_t va, int32_t *out)
{
    const unsigned char *p = ral_image_at(im, va, 4);
    if (!p) return -1;
    *out = ral_image_i32(p);
    return 0;
}

static float bits_float(int32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* 0x0040D597: a word whose top byte is 0x00 or 0xFF is an integer, FILD /
 * FSTP; anything else is already a float. */
static float num(int32_t v)
{
    uint32_t top = (uint32_t)v & 0xFF000000u;
    return (top == 0 || top == 0xFF000000u) ? (float)v : bits_float(v);
}

static int load_voice(const ral_image *im, uint32_t at, snd_car_voice *v)
{
    int32_t w[10];
    int i;
    for (i = 0; i < 10; i++)
        if (rd32(im, at + 4u * (uint32_t)i, &w[i]) != 0) return -1;
    v->id = w[0];
    /* 0x0040D5C2 -> 0x0040D5AC: +0x18 = +0x14 - +0x10, +0x24 = +0x20 - +0x1C,
     * FSUB / FSTP of the converted floats */
    v->f_lo = num(w[4]);
    v->f_slope = (float)((double)num(w[5]) - num(w[4]));
    v->v_lo = num(w[7]);
    v->v_slope = (float)((double)num(w[8]) - num(w[7]));
    return 0;
}

int snd_car_tables_load(snd_car_tables *t, const ral_image *im)
{
    static const uint32_t slot_off[SND_SLOTS] = {
        0x14C, 0x164, 0x17C, 0x194, 0x1AC, 0x1C4, 0x1DC, 0x1F8, 0x220, 0x260
    };
    int32_t w, rec0;
    int i, j, m;

    memset(t, 0, sizeof *t);
    for (i = 0; i < SND_CAR_IDS; i++) {
        uint32_t b = 0x0050571Cu + 0x40u * (uint32_t)i;
        int32_t f, sc;
        if (rd32(im, b + 0x18, &f) || rd32(im, b + 0x20, &t->snd[i].loop)
            || rd32(im, b + 0x2C, &w) || rd32(im, b + 0x30, &sc)
            || rd32(im, b + 0x34, &t->snd[i].volume)
            || rd32(im, b + 0x3C, &t->snd[i].rate))
            return -1;
        t->snd[i].slider = (uint32_t)w;
        t->snd[i].scale = bits_float(sc);
        if (f) {
            size_t n = 0;
            const unsigned char *c;
            while ((c = ral_image_at(im, (uint32_t)f + (uint32_t)n, 1)) && *c
                   && n + 1 < sizeof t->snd[i].file) {
                t->snd[i].file[n] = (char)*c;
                n++;
            }
            t->snd[i].file[n] = '\0';
        }
    }
    /* [0x0050F304 + car*4] -> 0x88-byte blocks; 0x0040D4C7's conversion. */
    for (m = 0; m < SND_CAR_MODELS; m++) {
        int32_t p, fl, lo, hi;
        if (rd32(im, 0x0050F304u + 4u * (uint32_t)m, &p)
            || rd32(im, (uint32_t)p, &fl) || rd32(im, (uint32_t)p + 4, &lo)
            || rd32(im, (uint32_t)p + 8, &hi))
            return -1;
        t->model[m].flags = (uint32_t)fl;
        t->model[m].lo = num(lo);
        /* +0xC = +8 - +4 (FSUB / FSTP), then FLD1 / FDIV / FSTP */
        t->model[m].k = (float)(1.0 / (double)(float)((double)num(hi) - num(lo)));
        for (j = 0; j < 3; j++)
            if (load_voice(im, (uint32_t)p + 0x10u + 0x28u * (uint32_t)j,
                           &t->model[m].v[j]) != 0)
                return -1;
    }
    if (load_voice(im, 0x0050FFC8u, &t->other) != 0) return -1;
    /* the surfaces: [0x005102F4 + material*4] -> class; [0x00510134 +
     * class*8] the roll set, +4 the skid set: {type, ids...} */
    for (m = 0; m < 16; m++)
        if (rd32(im, 0x005102F4u + 4u * (uint32_t)m, &t->surf_class[m])) return -1;
    /* classes 0..6: the class table's values (carsound.py surfaces) */
    for (i = 0; i < 7; i++) {
        int32_t roll, skid;
        if (rd32(im, 0x00510134u + 8u * (uint32_t)i, &roll)
            || rd32(im, 0x00510138u + 8u * (uint32_t)i, &skid))
            return -1;
        if (rd32(im, (uint32_t)roll, &t->roll_type[i])
            || rd32(im, (uint32_t)skid, &t->skid_type[i]))
            return -1;
        for (j = 0; j < 8; j++)
            if (rd32(im, (uint32_t)roll + 4u + 4u * (uint32_t)j, &t->roll[i][j])) return -1;
        for (j = 0; j < 8; j++)
            if (rd32(im, (uint32_t)skid + 4u + 4u * (uint32_t)j, &t->skid[i][j])) return -1;
    }
    if (rd32(im, 0x0050CD84u, &rec0)) return -1;
    for (i = 0; i < SND_SLOTS; i++)
        if (rd32(im, (uint32_t)rec0 + slot_off[i], &t->slot_id[i])) return -1;
    {
        static const uint32_t ka[] = {
            0x00510108, 0x00510118, 0x00510114, 0x005100FC, 0x00510100,
            0x005100F4, 0x0051011C, 0x0050C86C, 0x0050C870, 0x005100F8,
            0x0050C874, 0x0050C878
        };
        float *kp[] = {
            &t->k_whl, &t->k_inside_eng, &t->k_inside_whl, &t->k_sum, &t->k_64,
            &t->k_tenth, &t->k_surf, &t->k_c, &t->k_inv_c, &t->k_dop,
            &t->k_pan0, &t->k_pan
        };
        for (i = 0; i < (int)(sizeof ka / sizeof ka[0]); i++) {
            if (rd32(im, ka[i], &w)) return -1;
            *kp[i] = bits_float(w);
        }
        /* MOV [0x0051215C], 0x3C888889 at 0x0040D9C9: an immediate */
        if (rd32(im, 0x0040D9CFu, &w)) return -1;
        t->k_bang = bits_float(w);
    }
    return 0;
}

/* ---- the record ---------------------------------------------------------- */

/* A record as the image's .data holds it (carsound.py events; the dump in
 */
static void rec_init(snd_car_rec *r, const snd_car_tables *t, int32_t player,
                     int32_t car)
{
    int i;
    memset(r, 0, sizeof *r);
    r->revs = 1000;                     /* +0x14                           */
    r->blow_latch = 1;                  /* +0xF8                           */
    r->doppler = 1.0f;                  /* +0x13C                          */
    r->roll_id = r->skid_id = -2;       /* +0x144 / +0x148                 */
    r->bang_pitch = 1.0f;               /* +0x210                          */
    for (i = 0; i < SND_SLOTS; i++) {
        r->slot[i].id = t->slot_id[i];
        r->slot[i].vol = r->slot[i].rate = -0x6F;
    }
    r->player = player;
    r->car = car;
}

void snd_car_init(snd_car *c, const snd_car_tables *t, struct snd *s,
                  int32_t car, sim_sound_feed *feed)
{
    int k;
    memset(c, 0, sizeof *c);
    c->t = *t;
    c->s = s;
    c->feed = feed;
    c->tps = 50.0f;                     /* 0x0040F00C: [0x0050C860] = 50   */
    /* 0x004158B6: the launcher's +0x00 (player 1) and +0x04 (the car) */
    rec_init(&c->rec, t, 0, car);
    /* records 1..3 as .data holds them: +0x04 = 0x11 (car 17; the
     * launcher sets +0x04 of the players' records only) and +0x00 = ~slot
     * (0x00415861..) until 0x0040D5EB writes it. */
    for (k = 0; k < SND_CAR_AI; k++) rec_init(&c->ai[k], t, ~(k + 1), 0x11);
}

/* ---- the sample player, 0x0040A89E ----------------------------------- */

static int32_t slider_of(const snd_car *c, uint32_t addr)
{
    const snd *s = c->s;
    if (!s) return 64;
    switch (addr) {
    case 0x00707FFCu: return s->set.fx_race;     /* in-game effects          */
    case 0x00708008u: return s->set.engine;      /* car engine               */
    case 0x00708004u: return s->set.codriver;    /* co-driver speech         */
    case 0x00501918u: return s->set.fx_menu;
    default:          return 64;
    }
}

static int open_id(snd_car *c, int32_t id)
{
    const snd_car_sound *r = &c->t.snd[id];
    snd *s = c->s;
    if (c->v[id].open) return c->v[id].open > 0;
    c->v[id].open = -1;
    if (!r->file[0] || !s || !s->be.id_load) return 0;
    if (s->be.id_load(s->be.ud, id, r->file, &c->v[id].frames,
                      &c->v[id].file_rate) != 0)
        return 0;
    c->v[id].open = 1;
    c->v[id].vol = c->v[id].pan = c->v[id].rate = -1000;
    return 1;
}

static int playing(snd_car *c, int32_t id)
{
    snd *s = c->s;
    return s && s->be.id_playing && s->be.id_playing(s->be.ud, id);
}

void snd_car_post(snd_car *c, int32_t id, int32_t vol, int32_t pan, int32_t rate)
{
    const snd_car_sound *r;
    snd *s = c->s;
    int on, looping, upd;

    /* EAX bit 31: update only (the emitters, 0x0040EA3F) - the drain
     * hands the player the id without it, and 0x0040A9AB tests the posted
     * word: a voice that is not playing is not started. */
    upd = (id & (int32_t)0x80000000u) != 0;
    if (c->nreq < SND_CAR_LOG) {               /* the posted word, bit 31 too */
        snd_car_req *q = &c->req[c->nreq++];
        q->id = id; q->vol = vol; q->pan = pan; q->rate = rate;
    }
    if (c->log)
        fprintf(c->log, "%u car %d %d %d %d\n", (unsigned)c->clock, (int)id,
                (int)vol, (int)pan, (int)rate);
    id &= 0x7FFFFFFF;
    if (id < 0 || id >= SND_CAR_IDS) return;           /* JS                 */
    if (s && s->nosound) return;
    if (!open_id(c, id)) return;                       /* no voice / unloaded */
    r = &c->t.snd[id];
    on = playing(c, id);
    if (vol == 0 || r->loop != 0) {                    /* 0x0040A988         */
        if (on) {
            if (vol == 0) {                            /* 0x0040A9BD: stop   */
                if (s->be.id_stop) s->be.id_stop(s->be.ud, id);
                return;
            }
        } else if (vol == 0) {
            return;
        } else if (upd) {
            return;                                    /* 0x0040A9AB          */
        }
    } else if (on) {
        return;                       /* a one-shot with no free voice: dropped */
    }
    /* 0x0040AA09: the voice's words, each applied only when it changed */
    if (vol == -0x6F) vol = r->volume;
    if ((uint32_t)vol > 0x40u) vol = 0x40;             /* JBE, unsigned      */
    if (pan == -0x6F) pan = 0;                         /* the record's +0x38 */
    pan = (int32_t)(int8_t)(uint8_t)pan;               /* MOVSX AL           */
    if (pan < -0x40) pan = -0x40;
    if (pan > 0x40) pan = 0x40;
    if (rate == -0x6F) rate = r->rate;
    if (rate == -0x70) rate = (int32_t)c->v[id].file_rate;
    if (rate < 100) rate = 100;
    if (rate > 200000) rate = 200000;
    if (s->be.id_set && (vol != c->v[id].vol || pan != c->v[id].pan
                         || rate != c->v[id].rate)) {
        /* SetVolume: the table's entry for vol x slider >> 6; SetPan: the
         * far channel's (0x0040B86D); SetFrequency: Hz */
        int32_t ix = (vol * slider_of(c, r->slider)) >> 6;
        int32_t pmb = pan < 0 ? s->mb[64 + pan] : -s->mb[64 - pan];
        if (ix < 0) ix = 0;
        if (ix > 64) ix = 64;
        s->be.id_set(s->be.ud, id, s->mb[ix], pmb, (uint32_t)rate);
    }
    c->v[id].vol = vol; c->v[id].pan = pan; c->v[id].rate = rate;
    /* bit 0x10 always (Play), 0x20 for loop type 3 (rewind first);
     * DSBPLAY_LOOPING for loop type 1 (0x0040B7D8) */
    looping = r->loop == 1;
    if (s->be.id_start && (!on || r->loop == 3 || !looping))
        s->be.id_start(s->be.ud, id, looping, r->loop == 3);
}

/* 0x0040B918: the defaults (EBX -0x6F the record's volume; EDX -0x6F the
 * record's rate, -0x70 the file's), clamped; an id with no voice gives 0
 * and 44,100. */
static void defaults(snd_car *c, int32_t id, int32_t *vol, int32_t *rate)
{
    const snd_car_sound *r;
    if (id < 0 || id >= SND_CAR_IDS || !open_id(c, id)) {
        *vol = 0; *rate = 0xAC44;
        return;
    }
    r = &c->t.snd[id];
    if (*vol == -0x6F) *vol = r->volume;
    if (*vol < 0) *vol = 0;
    if (*vol > 0x40) *vol = 0x40;
    if (*rate == -0x6F) *rate = r->rate;
    if (*rate == -0x70) *rate = (int32_t)c->v[id].file_rate;
    if (*rate < 100) *rate = 100;
    if (*rate > 200000) *rate = 200000;
}

/* 0x0040B8B2: the sample's length at `rate` - 0 with no voice, 0x7FFFFFFF
 * for a looping record; frames x 1000 / rate, 32-bit, unsigned. */
static uint32_t length_ms(snd_car *c, int32_t id, int32_t rate)
{
    if (id < 0 || id >= SND_CAR_IDS || !open_id(c, id)) return 0;
    if (c->t.snd[id].loop == 1) return 0x7FFFFFFFu;
    if (rate == -0x6F) rate = c->t.snd[id].rate;
    if (rate == -0x70) rate = (int32_t)c->v[id].file_rate;
    if (rate < 100) rate = 100;
    if (rate > 200000) rate = 200000;
    return (uint32_t)(c->v[id].frames * 1000u) / (uint32_t)rate;
}

/* ---- the poster ------------------------------------------------------- */

/* The context 0x0050C830 (carsound.h). */
typedef snd_car_ctx ctx_t;

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* 0x0040EF3F, the 2D path ([0x004F327C] bit 20 clear). */
static void post2d(snd_car *c, ctx_t *x)
{
    const snd_car_sound *r;
    int32_t vol, pan, rate;
    double a;
    if (x->id & 0x8000) return;
    /* MOVSX EDI,AX: the record by the id's low 16 bits */
    if ((int16_t)x->id < 0 || (int16_t)x->id >= SND_CAR_IDS) return;
    r = &c->t.snd[(int16_t)x->id];
    vol = (int32_t)lrint((double)x->vol * r->scale * x->atten);
    if (vol < 0) vol = 0;
    if (vol - 0x40 >= 0) vol = 0x40;
    a = atan2((double)x->pos[2], (double)x->pos[0]);     /* FPATAN           */
    x->ang = (float)a;
    pan = (int32_t)lrint((double)c->t.k_pan0 - fabs(a) * c->t.k_pan);
    if ((fbits(x->pos[0]) & 0x7FFFFFFFu) == 0) pan = 0;
    rate = (int32_t)lrint((double)x->rate * x->pitch);
    snd_car_post(c, x->id, vol, pan, rate);
}

/* 0x0040EFF4: command 1 at volume 0, unless the id has bit 15. */
static void stop(snd_car *c, int32_t id)
{
    if (id & 0x8000) return;
    snd_car_post(c, id, 0, 0, 0);
}

/* ---- 0x0040D773, the per-record tick ----------------------------------- */

static void record_tick(snd_car *c, const snd_car_model *m)
{
    snd_car_rec *r = &c->rec;
    sim_sound_feed *f = c->feed;
    int32_t ecx, eax, old;
    double sq;
    float d0;

    if (f->fresh == 1) {
        r->t0 = r->t1; r->t1 = c->clock;
        r->v0 = r->v1; r->v1 = f->revs;
        r->revs = f->revs;
        f->fresh = 0;
    } else {
        int32_t dt = (int32_t)(r->t1 - r->t0);
        if (dt != 0) {
            int64_t p;
            if (dt > 1000) dt = 1000;
            p = (int64_t)(int32_t)(c->clock - r->t1) * (int64_t)(r->v1 - r->v0);
            r->revs = (int32_t)(p / dt) + r->v1;       /* IDIV, toward zero   */
        }
    }
    r->x = (float)(((double)r->revs - m->lo) * m->k);
    r->thr = (float)f->throttle / 256.0f;  /* the exponent less 8; 0 stays 0 */
    r->whl = (float)(fabs((double)f->whl) * c->t.k_whl);
    r->pstate = r->state;
    if (r->revs == 0) r->state = 0;
    else if ((uint32_t)r->revs <= 1000u) r->state = 1;
    else r->state = 2;
    if (r->reset != 0) { r->reset = 0; r->state = 3; }

    /* the throttle's smoothing */
    ecx = f->throttle;
    eax = ecx - r->smooth;
    r->diff = eax;
    if (eax == 0) {
        r->dir = 0;
        if (r->rises == 1) r->rises++;
        if (r->falls == 1) r->falls++;
    } else if (eax < 0) {
        r->falls++; r->rises = 0; r->dir = 2;
        r->smooth -= (int32_t)((uint32_t)r->smooth >> 7);
        r->smooth--;
        if (r->smooth < ecx) r->smooth = ecx;
    } else {
        r->rises++; r->falls = 0; r->dir = 1;
        r->smooth += (int32_t)((uint32_t)(0x100 - r->smooth) >> 6);
        r->smooth++;
        if (r->smooth > ecx) r->smooth = ecx;
    }
    old = r->lastdir;
    r->lastdir = r->dir;
    if (old + r->dir == 3) r->cool += 5;                /* a flip 1 <-> 2      */

    /* a gear change, and the bang */
    eax = f->gear;
    if (eax != r->lastgear) {
        r->lastgear = eax;
        r->cool += 7;
        if ((m->flags & 0x2000u) && eax > 1 && r->diff >= 0x32) {
            r->slot[SND_SLOT_BANG].trig = 1;
            r->bang_set = 1;
            eax = (int32_t)(c->clock & 15u);
            r->bang_pitch = (float)((double)eax * c->t.k_bang + 0.75);
        }
    }
    r->gear0 = eax == 0;                 /* EAX: the gear, or now & 15     */

    if (m->flags & 0x1000u) {
        snd_car_slot *p = &r->slot[SND_SLOT_POPS];
        if (r->dir == 1) {
            p->trig = 0; p->playing = 0; r->pops_wait = 0; p->vol = 0;
        } else if ((uint32_t)r->smooth <= 0xF0u && --r->pops_wait < 0) {
            int32_t e = r->revs - 2000;
            int32_t v;
            if (e < 0) e = 0;
            r->pops_wait = (int32_t)(c->clock & 3u)
                           + (int32_t)((uint32_t)e >> 9) + 1;
            p->trig = 1;
            v = p->vol + 8;
            if (v > 0x40) v = 0x40;
            p->vol = v;
        }
        if (r->dir == 1) {
            r->blow_latch = 0;
        } else if (r->blow_latch != 1 && r->dir == 2) {
            if ((uint32_t)r->revs >= 4000u) {
                r->slot[SND_SLOT_BLOWOFF].id = (int32_t)(m->flags & 0xFFFu);
                r->slot[SND_SLOT_BLOWOFF].trig = 1;
            }
            r->blow_latch = 1;
        }
    }
    if (r->cool != 0) r->cool--;

    /* distance, Doppler, attenuation */
    /* FLD x / FMUL, y, z, then FADDP twice: x^2 + (y^2 + z^2) */
    sq = sqrt((double)f->pos[0] * f->pos[0]
              + ((double)f->pos[1] * f->pos[1] + (double)f->pos[2] * f->pos[2]));
    d0 = r->d;
    r->d = (float)sq;                                   /* FST            */
    r->dd = (float)(((double)d0 - sq) * c->tps);        /* FSUBP on the
                                                          * unrounded root */
    r->doppler = (float)((((double)r->dd + c->t.k_c) * c->t.k_inv_c
                          - r->doppler) * c->t.k_dop + r->doppler);
    r->atten = 1.0f;
    if ((int32_t)fbits(r->d) > (int32_t)fbits(1.0f))    /* JLE on the bits */
        r->atten = (float)(1.0 / (double)r->d);
}

/* ---- the voices ------------------------------------------------------- */

static void engine_voice(snd_car *c, const snd_car_model *m, int k, ctx_t *x)
{
    snd_car_rec *r = &c->rec;
    const sim_sound_feed *f = c->feed;
    const snd_car_voice *v;
    int32_t eax = r->player;
    double vol;
    int i;

    if (eax < 0) {
        if (k != 0) return;                    /* B and C: RET for a rival   */
        eax &= 1;
        v = &c->t.other;
    } else {
        v = &m->v[k];
    }
    x->id = eax + v->id;
    if (r->state == 0) { stop(c, x->id); return; }
    if (k < 2) {
        if (r->state == 3) { stop(c, x->id); return; }
        x->rate = (float)((double)r->x * v->f_slope + v->f_lo);
        vol = (double)(k == 0 ? r->x : r->thr) * v->v_slope + v->v_lo;
        if (f->outside == 0) vol *= c->t.k_inside_eng;
        x->vol = (float)vol;
        for (i = 0; i < 3; i++)
            x->pos[i] = (float)(k == 0 ? (double)f->pos[i] + f->fwd[i]
                                       : (double)f->pos[i] - f->fwd[i]);
    } else {
        if (r->state == 1 || r->gear0 == 1 || r->cool != 0 || f->airborne != 0) {
            stop(c, x->id);
            return;
        }
        x->rate = (float)((double)r->whl * v->f_slope + v->f_lo);
        vol = (double)r->whl * v->v_slope + v->v_lo;
        if (f->outside == 0) vol *= c->t.k_inside_whl;
        x->vol = (float)vol;
        for (i = 0; i < 3; i++) x->pos[i] = f->pos[i];
    }
    post2d(c, x);
}

/* 0x0040DE85: one set's voice - the id by player (and, type 1, the view),
 * the old id stopped when it changes, the record's rate, `vol` x 0.625. */
static void surface_set(snd_car *c, int32_t type, const int32_t *ids,
                        float vol, int32_t *cur, ctx_t *x)
{
    snd_car_rec *r = &c->rec;
    const sim_sound_feed *f = c->feed;
    int32_t e = r->player & 3, id, v = -0x6F, rate = -0x6F;
    int i;
    if (type == 1) e += f->outside * 4;
    id = ids[e & 7];
    if (id != *cur) {
        int32_t was = *cur;
        *cur = id;
        stop(c, was);
    }
    x->id = id;
    defaults(c, id, &v, &rate);
    x->rate = (float)rate;
    x->vol = (float)((double)vol * c->t.k_surf);
    for (i = 0; i < 3; i++) x->pos[i] = f->pos[i];
    post2d(c, x);
}

/* The wheel 0x0040DD0E reads the material from: by the signs of two sums. */
static int32_t pick_material(const sim_sound_feed *f, const float g[4])
{
    float a = (float)((((0.0 - g[0]) + g[1]) - g[2]) + g[3]);
    float b = (float)((((0.0 + g[0]) + g[1]) - g[2]) - g[3]);
    int w0 = 0, w2 = 2;
    if (!(fbits(a) & 0x80000000u)) { w0 = 1; w2 = 3; }  /* JS on the bits     */
    if (fbits(b) & 0x80000000u) w0 = w2;
    return f->w[w0].material;
}

static int32_t class_of(const snd_car *c, int32_t material)
{
    /* the lookup is not masked in the original; the port keeps it in the
     * table (the captures' materials are 0, 1, 2, 7, 10) */
    return c->t.surf_class[material & 15] & 7;
}

static void surfaces(snd_car *c, ctx_t *x)
{
    snd_car_rec *r = &c->rec;
    const sim_sound_feed *f = c->feed;
    float g[4], k[4];
    double vr, vs;
    int32_t cls;
    int i;

    for (i = 0; i < 4; i++) { g[i] = f->w[i].gate; k[i] = f->w[i].skid; }
    r->w284 = (int32_t)lrint(((((double)f->w[0].speed + f->w[1].speed)
                                + f->w[2].speed) + f->w[3].speed) * c->t.k_sum);
    r->w28c = (int32_t)lrint(((((double)g[0] + g[1]) + g[2]) + g[3]) * c->t.k_64);
    vr = 0.0;
    for (i = 0; i < 4; i++) vr += (double)f->w[i].speed * g[i];
    r->mat = pick_material(f, g);
    cls = class_of(c, r->mat);
    surface_set(c, c->t.roll_type[cls], c->t.roll[cls],
                (float)(vr * c->t.k_tenth), &r->roll_id, x);

    r->w290 = (int32_t)lrint(((((double)k[0] + k[1]) + k[2]) + k[3]) * c->t.k_64);
    vs = (((double)k[0] + k[1]) + k[2]) + k[3];
    cls = class_of(c, pick_material(f, k));
    surface_set(c, c->t.skid_type[cls], c->t.skid[cls],
                (float)(vs * c->t.k_tenth), &r->skid_id, x);
}

/* 0x0040E3C1: a one-shot slot, its trigger at `trig`. */
static void slot_service(snd_car *c, snd_car_slot *p, int32_t *trig, ctx_t *x)
{
    int32_t v, rate;
    if (*trig == 1) {
        *trig = 0;
        if (p->id == 0xBD || p->playing != 1) {
            x->id = p->id;
            v = p->vol; rate = p->rate;
            defaults(c, p->id, &v, &rate);
            x->vol = (float)v;
            x->rate = (float)rate;
            p->end = c->clock + length_ms(c, p->id, rate);
            p->playing = 1;
            post2d(c, x);
            return;
        }
    } else if (p->playing != 1) {
        return;
    }
    if ((int32_t)(c->clock - p->end) < 0) return;          /* JS          */
    p->playing = 0;
}

static void one_shots(snd_car *c, ctx_t *x)                /* 0x0040DF19  */
{
    snd_car_rec *r = &c->rec;
    sim_sound_feed *f = c->feed;
    int i;
    if (r->player < 0) return;
    for (i = 0; i < 3; i++) x->pos[i] = f->pos[i];
    if (f->outside == 0) {
        slot_service(c, &r->slot[SND_SLOT_SWITCH], &r->slot[SND_SLOT_SWITCH].trig, x);
        slot_service(c, &r->slot[SND_SLOT_WIPER], &r->slot[SND_SLOT_WIPER].trig, x);
        slot_service(c, &r->slot[SND_SLOT_GEARCHNG], &f->gearchng, x);
        slot_service(c, &r->slot[SND_SLOT_GEARGRND], &f->geargrnd, x);
    }
    slot_service(c, &r->slot[SND_SLOT_SHATTER], &f->shatter, x);
    slot_service(c, &r->slot[SND_SLOT_IGNITION], &r->slot[SND_SLOT_IGNITION].trig, x);
    for (i = 0; i < 3; i++) x->pos[i] = f->blowout_pos[i];
    slot_service(c, &r->slot[SND_SLOT_BLOWOUT], &f->blowout, x);
}

static void impacts(snd_car *c, ctx_t *x)                  /* 0x0040DFA5  */
{
    sim_sound_feed *f = c->feed;
    int32_t id = f->impact_id, v, rate = -0x6F;
    int i;
    if (id == SIM_SOUND_NONE) return;
    f->impact_id = SIM_SOUND_NONE;
    x->id = id;
    v = f->impact_vol;
    defaults(c, id, &v, &rate);
    if ((uint32_t)v < 4u) return;                          /* JB          */
    x->vol = (float)v;
    x->rate = (float)rate;
    for (i = 0; i < 3; i++) x->pos[i] = f->impact_pos[i];
    post2d(c, x);
}

static void scrape(snd_car *c, ctx_t *x)                   /* 0x0040E008  */
{
    sim_sound_feed *f = c->feed;
    int32_t id = f->scrape_id, v, rate = -0x6F;
    int i;
    if (id == SIM_SOUND_NONE) return;
    f->scrape_id = SIM_SOUND_NONE;
    x->id = id;
    v = f->scrape_vol;
    defaults(c, id, &v, &rate);
    if ((uint32_t)v < 1u) { stop(c, id); return; }
    x->vol = (float)v;
    x->rate = (float)rate;
    for (i = 0; i < 3; i++) x->pos[i] = f->scrape_pos[i];
    post2d(c, x);
}

static void engine_shots(snd_car *c, ctx_t *x)             /* 0x0040E072  */
{
    snd_car_rec *r = &c->rec;
    const sim_sound_feed *f = c->feed;
    float pitch;
    int i;
    if (r->state == 3) return;
    for (i = 0; i < 3; i++)
        x->pos[i] = (float)((double)f->pos[i] - f->fwd[i]);
    slot_service(c, &r->slot[SND_SLOT_BLOWOFF], &r->slot[SND_SLOT_BLOWOFF].trig, x);
    slot_service(c, &r->slot[SND_SLOT_POPS], &r->slot[SND_SLOT_POPS].trig, x);
    pitch = x->pitch;
    x->pitch = (float)((double)x->pitch * r->bang_pitch);
    slot_service(c, &r->slot[SND_SLOT_BANG], &r->slot[SND_SLOT_BANG].trig, x);
    x->pitch = pitch;
}

/* 0x0040D724 for c->rec / c->feed. */
static void record_run(snd_car *c)
{
    snd_car_rec *r = &c->rec;
    const snd_car_model *m =
        &c->t.model[(uint32_t)r->car < SND_CAR_MODELS ? r->car : 0];
    ctx_t x;

    memset(&x, 0, sizeof x);
    record_tick(c, m);                                     /* 0x0040D773  */
    x.pitch = r->doppler;
    x.atten = r->atten;
    engine_voice(c, m, 0, &x);
    engine_voice(c, m, 1, &x);
    engine_voice(c, m, 2, &x);
    surfaces(c, &x);
    one_shots(c, &x);
    impacts(c, &x);
    scrape(c, &x);
    engine_shots(c, &x);
}

/* 0x0040D724 for an AI record, `player` -2 or -1 for the call (the
 * original's push / MOV / CALL / pop on +0x00): the engine's context
 * (c->rec, c->feed) is swapped for the record's. */
static void ai_run(snd_car *c, int k, int32_t player)
{
    snd_car_rec save = c->rec;
    sim_sound_feed *sf = c->feed;
    int32_t keep = c->ai[k].player;

    c->rec = c->ai[k];
    c->rec.player = player;
    c->feed = c->ai_feed[k];
    record_run(c);
    c->ai[k] = c->rec;
    c->ai[k].player = keep;
    c->rec = save;
    c->feed = sf;
}

/* 0x0040D5EB: every tick in the arcade, each record is closed, then opened
 * if its car has a driver, and an AI car's +0x00 is ~slot.  Record 0 is the
 * port's own (snd_car_race / snd_car_close); the AI records open only while
 * it is, so the port's pause stays silent. */
static void ai_open(snd_car *c)
{
    int k;
    for (k = 0; k < SND_CAR_AI; k++) {
        snd_car_rec *r = &c->ai[k];
        r->on = 0;
        if (!c->ai_feed[k] || c->rec.on != 1 || c->ai_driver[k] == 0) continue;
        r->on = 1;
        r->player = 0;
        if (c->ai_driver[k] != 1) r->player = ~(k + 1);
    }
}

void snd_car_tick(snd_car *c, uint32_t clock, float tps)
{
    int32_t eax = -1, ebx = -1, ecx = 0x7F800000, edx = 0x7F800000;
    int i;

    c->clock = clock;
    c->tps = tps;
    c->nreq = 0;
    if (c->ai_on) ai_open(c);                              /* 0x0040D5EB  */
    if (!c->ai_on) {
        if (!c->feed || c->rec.on != 1 || c->rec.player < 0) return;
        record_run(c);                                     /* 0x0040D63C  */
        return;
    }
    /* 0x0040D63C over the launcher's four records: a player's through
     * 0x0040D724 at once, an open AI record into the nearest two (the
     * squared distance's float bits, compared as integers). */
    for (i = 0; i < 1 + SND_CAR_AI; i++) {
        snd_car_rec *r = i == 0 ? &c->rec : &c->ai[i - 1];
        int32_t d;
        double sq;
        const sim_sound_feed *f = i == 0 ? c->feed : c->ai_feed[i - 1];
        if (r->on != 1 || !f) continue;
        if (r->player >= 0) {
            if (i == 0) record_run(c);
            else ai_run(c, i - 1, r->player);
            continue;
        }
        sq = (double)f->pos[0] * f->pos[0]
             + ((double)f->pos[1] * f->pos[1] + (double)f->pos[2] * f->pos[2]);
        {
            float fl = (float)sq;
            memcpy(&d, &fl, 4);
        }
        if (ecx > d) {
            edx = ecx; ebx = eax; ecx = d; eax = i;
        } else if (edx > d) {
            edx = d; ebx = i;
        }
    }
    if (eax == c->near_b || ebx == c->near_a) {            /* 0x0040D6C8  */
        int32_t t = eax; eax = ebx; ebx = t;
    }
    c->near_a = eax;
    c->near_b = ebx;
    if (c->near_a > 0 && c->ai_feed[c->near_a - 1])
        ai_run(c, c->near_a - 1, -2);
    if (c->near_b > 0 && c->ai_feed[c->near_b - 1])
        ai_run(c, c->near_b - 1, -1);
}

void snd_car_arcade(snd_car *c, sim_sound_feed *const feed[SND_CAR_AI])
{
    int k;
    for (k = 0; k < SND_CAR_AI; k++) c->ai_feed[k] = feed[k];
    c->ai_on = 1;
}

/* ---- for worldsound.c ---------------------------------------------------- */

void snd_car_post2d(snd_car *c, snd_car_ctx *x) { post2d(c, x); }
void snd_car_stop(snd_car *c, int32_t id) { stop(c, id); }
void snd_car_defaults(snd_car *c, int32_t id, int32_t *vol, int32_t *rate)
{
    defaults(c, id, vol, rate);
}
uint32_t snd_car_length(snd_car *c, int32_t id, int32_t rate)
{
    return length_ms(c, id, rate);
}
void snd_car_slot_service(snd_car *c, snd_car_slot *p, int32_t *trig, snd_car_ctx *x)
{
    slot_service(c, p, trig, x);
}
int32_t snd_car_loop(const snd_car *c, int32_t id)
{
    return (id < 0 || id >= SND_CAR_IDS) ? -1 : c->t.snd[id].loop;
}

void snd_car_hush(snd_car *c)
{
    snd *s = c->s;
    int i;
    if (!s || !s->be.id_stop) return;
    for (i = 0; i < SND_CAR_IDS; i++)
        if (c->v[i].open > 0 && playing(c, i)) s->be.id_stop(s->be.ud, i);
}

/* 0x0040D213 for one record and its step half. */
static void rec_reset(snd_car_rec *r, sim_sound_feed *feed)
{
    int i, k;

    /* every slot's trigger and playing flag (four of the triggers are the
     * feed's, cleared below), the step's words */
    for (i = 0; i < SND_SLOTS; i++) {
        r->slot[i].trig = 0;
        r->slot[i].playing = 0;
    }
    r->revs = 0;                                           /* +0x14       */
    if (feed) {
        feed->revs = 0; feed->throttle = 0; feed->whl = 0.0f;
        for (k = 0; k < 4; k++) feed->w[k].speed = 0.0f;
        feed->gearchng = feed->geargrnd = 0;
        feed->blowout = feed->shatter = 0;
        feed->impact_id = feed->scrape_id = SIM_SOUND_NONE;
    }
    r->on = 0;                                             /* +0x08       */
}

static void car_reset(snd_car *c)
{
    int k;
    rec_reset(&c->rec, c->feed);
    for (k = 0; k < SND_CAR_AI; k++) rec_reset(&c->ai[k], c->ai_feed[k]);
}

void snd_car_close(snd_car *c)
{
    snd_car_hush(c);              /* 0x0040D219: command 7, every voice   */
    car_reset(c);
}

void snd_car_race(snd_car *c)
{
    snd_car_rec *r = &c->rec;
    const snd_car_model *m;
    int i, k;

    car_reset(c);                                          /* 0x0040D213  */
    /* 0x0040D423: opened */
    r->on = 1;
    /* 0x0040EE51's set, loaded now: the car's engine, the whine, every
     * surface, the car's and the impacts' one-shots */
    m = &c->t.model[(uint32_t)r->car < SND_CAR_MODELS ? r->car : 0];
    for (k = 0; k < 3; k++) (void)open_id(c, m->v[k].id + (r->player & 1));
    if (c->ai_on) {            /* the rivals' othercar.wav, ids 0x5E / 0x5F */
        (void)open_id(c, c->t.other.id);
        (void)open_id(c, c->t.other.id + 1);
    }
    for (i = 0x60; i <= 0xC0; i++) (void)open_id(c, i);
}
