/* worldsound.c - worldsound.h.  Every function names the
 * original's it transcribes (tools/ralimg.py disasm); the reading is
 * x87 chains are held in double and
 * rounded where the original stores (FSTP float, FISTP int), as carsound.c. */

#include "worldsound.h"
#include "sound.h"
#include "image.h"
#include "physics.h"

#include <math.h>
#include <string.h>

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

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int snd_world_load(snd_world *w, const ral_image *im, snd_car *car)
{
    int32_t v;
    int i;
    const unsigned char *p;

    memset(w, 0, sizeof *w);
    w->car = car;
    for (i = 0; i < SND_WORLD_INDICES; i++)
        if (rd32(im, 0x00511CECu + 4u * (uint32_t)i, &w->ids[i])) return -1;
    for (i = 1; i < SND_WORLD_INDICES; i++) {
        p = ral_image_at(im, 0x00511D44u + 8u * (uint32_t)(i - 1), 8);
        if (!p) return -1;
        w->sched[i - 1].repeats = (uint16_t)(p[2] | (p[3] << 8));
        w->sched[i - 1].lo = (uint16_t)(p[4] | (p[5] << 8));
        w->sched[i - 1].hi = (uint16_t)(p[6] | (p[7] << 8));
    }
    /* 0x00510364: {id, trigger, playing, end, volume, rate} */
    if (rd32(im, 0x00510364u, &w->thunder.id)
        || rd32(im, 0x00510374u, &w->thunder.vol)
        || rd32(im, 0x00510378u, &w->thunder.rate))
        return -1;
    for (i = 0; i < 3; i++) {
        if (rd32(im, 0x00510344u + 4u * (uint32_t)i, &v)) return -1;
        w->thunder_pos[i] = bits_float(v);
    }
    if (rd32(im, 0x00510350u, &w->thunder_delay)) return -1;
    /* MOV [0x0051215C],0x447A0000 at 0x0040E682: an immediate (1000.0) */
    if (rd32(im, 0x0040E688u, &v)) return -1;
    w->k_1000 = bits_float(v);
    if (rd32(im, 0x0057B024u, &v)) return -1;
    w->k_scale = bits_float(v);
    /* slot 0 as .data holds it; the other 71 are checked to be the same */
    p = ral_image_at(im, 0x00510658u, 0x50u * SND_WORLD_SLOTS);
    if (!p) return -1;
    for (i = 1; i < SND_WORLD_SLOTS; i++)
        if (memcmp(p, p + 0x50 * i, 0x50) != 0) return -1;
    {
        snd_world_slot *s = &w->slot0;
        int32_t d[20];
        for (i = 0; i < 20; i++) d[i] = ral_image_i32(p + 4 * i);
        s->key = d[0]; s->alive = d[1];
        s->pos[0] = bits_float(d[2]); s->pos[1] = bits_float(d[3]);
        s->pos[2] = bits_float(d[4]);
        s->id = d[5]; s->index = d[6]; s->bit = d[7]; s->fresh = d[8];
        s->start = d[9]; s->playing = d[10]; s->repeats = d[11];
        s->delay = d[12]; s->end = (uint32_t)d[13];
        s->rate = bits_float(d[14]); s->vol = bits_float(d[15]);
        s->d = bits_float(d[16]); s->dd = bits_float(d[17]);
        s->doppler = bits_float(d[18]); s->atten = bits_float(d[19]);
    }
    for (i = 0; i < SND_WORLD_SLOTS; i++) w->slot[i] = w->slot0;
    w->rnd = 1;
    return 0;
}

/* 0x0040D38B..0x0040D403 */
static void world_reset(snd_world *w)
{
    sim_weather *weather = w->weather_rw;
    int i;
    if (weather) {
        weather->thunder = 0;                          /* [0x00510340]     */
        weather->rain_vol = weather->wind_vol = 0;     /* [0x00510354/58]  */
    }
    w->thunder_wait = 0;                               /* [0x00510360]     */
    w->thunder.trig = 0;                               /* [0x00510368]     */
    w->thunder.playing = 0;                            /* [0x0051036C]     */
    for (i = 0; i < SND_WORLD_SLOTS; i++) {
        w->slot[i].key = SND_WORLD_EMPTY;
        w->slot[i].id = -2;
    }
    w->nlist = 0;                                      /* [0x00510384]     */
}

void snd_world_stop(snd_world *w)
{
    snd_world_hush(w);                                 /* command 7        */
    world_reset(w);
}

void snd_world_race(snd_world *w, sim_weather *weather, uint32_t seed)
{
    int i;
    w->weather = weather;
    w->weather_rw = weather;
    world_reset(w);
    w->rnd = seed;
    /* every file the world can ask for, loaded now (the port's timing) */
    for (i = 0x184; i <= 0x19A; i++) {
        int32_t vol = -0x6F, rate = -0x6F;
        snd_car_defaults(w->car, i, &vol, &rate);
    }
    for (i = 0x17F; i <= 0x181; i++) {          /* the chimes too */
        int32_t vol = -0x6F, rate = -0x6F;
        snd_car_defaults(w->car, i, &vol, &rate);
    }
}

/* ---- the collection, on the render path ------------------------------ */

/* FUN_0044B1AB: the row vector (x, y, z, 1) times the 4 x 4 at ESI, each
 * output m[c]x + (m[4+c]y + m[8+c]z) + m[12+c]; then x [0x0057B024]. */
static void xform(const snd_world *w, const float m[16], int32_t x, int32_t y,
                  int32_t z, float out[3])
{
    int c;
    for (c = 0; c < 3; c++) {
        double v = (double)m[c] * x
                   + ((double)m[4 + c] * y + (double)m[8 + c] * z)
                   + (double)m[12 + c];
        out[c] = (float)(v * w->k_scale);
    }
}

/* 0x0045A475: one record of a cell's run. */
static void take(snd_world *w, int32_t *n, snd_world_item *list,
                 const cdat_payload *cd, int32_t i, const float m[16])
{
    const uint8_t *r = cdat_pol(cd, i);
    snd_world_item *it;
    int32_t key, pt[3];
    uint32_t idx;
    if (r[0] != 0xFF || r[1] != 0x3F) return;           /* bytes FF 3F      */
    if (*n == SND_WORLD_LIST) return;                    /* CMP EAX,0x24 / JE */
    /* MOVZX EAX,[ESI+8] / CMP EAX,0x186 never equals (a byte against 0x186) */
    idx = r[8];
    if (idx == 0 || idx > 0x15u) return;
    key = (int32_t)cdat_u32(r, 4);
    /* the port's bound (the original reads pnts[key] unchecked; every
     * record of the corpus passes FUN_0044FB03's check, cdat.py objects) */
    if ((uint32_t)key >= (uint32_t)cd->npnts) return;
    it = &list[*n];
    it->index = (int32_t)idx;
    it->key = key;
    cdat_pnt(cd, key, pt);
    /* + the record's 16.16 offset +0x30..+0x38, integer ADDs */
    pt[0] = (int32_t)((uint32_t)pt[0] + cdat_u32(r, 0x30));
    pt[1] = (int32_t)((uint32_t)pt[1] + cdat_u32(r, 0x34));
    pt[2] = (int32_t)((uint32_t)pt[2] + cdat_u32(r, 0x38));
    xform(w, m, pt[0], pt[1], pt[2], it->pos);
    (*n)++;
}

void snd_world_collect_grid(snd_world *w, const int32_t *grid, int32_t radius,
                            const cdat_payload *cd, const float view_m[16])
{
    snd_world_item list[SND_WORLD_LIST];
    int32_t n = 0, row, col;
    /* 0x0045A38D: ECX = (R - 2) * 0x21, five cells, + 0x1C, five rows -
     * rows and columns R-2..R+2 of the stride-32 grid */
    int32_t base = (radius - 2) * 0x21;
    for (row = 0; row < 5; row++) {
        for (col = 0; col < 5; col++) {
            int32_t cell = base + row * 0x20 + col;
            uint32_t z;
            int32_t z8[8], first, last, i;
            /* the port's bound: R < 2 would index before the grid */
            if (cell < 0 || cell >= STAGE_GRID_DWORDS) continue;
            z = (uint32_t)grid[cell];
            if (z >= STAGE_GRID_EMPTY) continue;         /* 0x0045A447 JAE  */
            if ((int32_t)z >= cd->nzube) continue;       /* the port's bound */
            cdat_zube(cd, (int32_t)z, z8);
            first = z8[2]; last = z8[3];                 /* zube +8, +0xC   */
            if (first == -1 || (uint32_t)first > (uint32_t)last) continue;
            for (i = first; (uint32_t)i <= (uint32_t)last; i++) {
                if ((uint32_t)i >= (uint32_t)cd->npols) break;   /* bound   */
                take(w, &n, list, cd, i, view_m);
            }
        }
    }
    /* 0x0045A541: [0x00510384] = the count, REP MOVSD the list */
    w->nlist = n;
    memcpy(w->list, list, (size_t)n * sizeof list[0]);
}

void snd_world_collect(snd_world *w, const stage *st, int32_t radius,
                       const float view_m[16])
{
    snd_world_collect_grid(w, st->grid, radius, &st->cdat, view_m);
}

/* ---- the audio thread's half ------------------------------------------ */

/* 0x0040E247..0x0040E3BF: the fireworks' two sounds, each
 * flag cleared and its sound posted with command 1 at a random volume 33..64,
 * a rate of 44,100 +- 6.25 %, and the pan word ECX the float of a third
 * random number x 0.01 (the 2D path reads a position as a pan: TENTATIVE,
 * The generator is the thread's. */
static void fw_post(snd_world *w, int32_t id)
{
    uint32_t e = sim_rnd_next_q(&w->rnd);
    int32_t vol = (int32_t)(e & 0x1Fu) + 0x21, rate, pan;
    float f;
    e >>= 5;
    rate = ((((int32_t)(e & 0x1FFu) - 0x100) * 44100) >> 12) + 44100;
    e >>= 9;
    f = (float)((long double)((int32_t)(e & 0x1FFu) - 0x100) * (long double)0.01f);
    memcpy(&pan, &f, 4);
    snd_car_post(w->car, id, vol, pan, rate);
}

static void fw_flags(snd_world *w)
{
    int k;
    if (!w->fw) return;
    for (k = 0; k < 2; k++)
        if (w->fw[k] == 1) {
            w->fw[k] = 0;
            fw_post(w, 0x182 + k);
        }
}

/* The finish scene's world tick: the fireworks' sounds and nothing else of
 * the thread's ambience (which the port does not arm there). */
void snd_world_fw(snd_world *w) { fw_flags(w); }

/* 0x0040E0CC: the weather.  The chimes (0x0040E0E1..0x0040E183) are the
 * arcade's and the fireworks (0x0040E247..) the championship finish's. */
static void weather(snd_world *w)
{
    snd_car *c = w->car;
    snd_car_ctx x;
    int32_t rain = 0, wind = 0;
    memset(&x, 0, sizeof x);
    x.pitch = 1.0f;                                     /* [ESI+0x20]       */
    x.atten = 1.0f;                                     /* [ESI+0x24]       */
    /* 0x0040E0E1..0x0040E183: the arcade's chimes - CHECKPT 0x17F,
     * CHECKTT 0x180, CHECKOT 0x181 - each flag cleared and its sound posted
     * with command 1 (0x00409A0D): EBX = EDX = -0x6F, the record's own volume
     * and rate, ECX = 0 */
    if (w->chime) {
        int k;
        for (k = 0; k < 3; k++)
            if (w->chime[k] == 1) {
                w->chime[k] = 0;
                snd_car_post(c, 0x17F + k, -0x6F, 0, -0x6F);
            }
    }
    fw_flags(w);
    if (w->weather_rw && w->weather_rw->thunder == 1) {
        w->weather_rw->thunder = 0;
        w->thunder_at = c->clock + (uint32_t)w->thunder_delay;
        w->thunder_wait = 1;
    }
    if (w->thunder_wait == 1) {
        if ((int32_t)(c->clock - w->thunder_at) < 0) goto levels;  /* JS     */
        w->thunder.trig = 1;
        w->thunder_wait = 0;
    }
    memcpy(x.pos, w->thunder_pos, sizeof x.pos);
    snd_car_slot_service(c, &w->thunder, &w->thunder.trig, &x);
levels:
    if (w->weather) { rain = w->weather->rain_vol; wind = w->weather->wind_vol; }
    snd_car_post(c, 0x199, rain >> 1, 0, -0x6F);        /* SAR EBX,1        */
    snd_car_post(c, 0x19A, wind, 0, -0x6F);
}

/* 0x0040E465: the list into the table. */
static void match(snd_world *w)
{
    int32_t k, j;
    for (k = 0; k < w->nlist; k++) {
        const snd_world_item *it = &w->list[k];
        snd_world_slot *s = NULL;
        if ((uint32_t)it->index < 1u || (uint32_t)it->index > 0x15u) continue;
        for (j = 0; j < SND_WORLD_SLOTS; j++)
            if (w->slot[j].key == it->key) { s = &w->slot[j]; break; }
        if (!s) {
            for (j = 0; j < SND_WORLD_SLOTS; j++)
                if (w->slot[j].key == (int32_t)SND_WORLD_EMPTY) {
                    s = &w->slot[j];
                    break;
                }
            if (!s) continue;
            s->key = it->key;
            s->fresh = 1;
            s->id = w->ids[it->index];
            s->bit = (int32_t)(1u << ((uint32_t)it->index & 31u));  /* SHL CL */
            s->index = it->index;
            s->repeats = 0;
            s->playing = 0;
            s->start = 0;
        }
        memcpy(s->pos, it->pos, sizeof s->pos);
        s->alive = 1;
    }
}

/* 0x0040E517: a slot nobody saw is freed - stopped first if it plays. */
static void free_unseen(snd_world *w)
{
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        if (s->alive == 1) continue;
        if (s->playing == 1) {
            if (s->key == (int32_t)SND_WORLD_EMPTY) continue;
            snd_car_stop(w->car, s->id);
        }
        s->key = (int32_t)SND_WORLD_EMPTY;
        s->id = -2;
    }
}

/* 0x0040E556: distance, its rate, Doppler, 1/d. */
static void motion(snd_world *w, float tps)
{
    const snd_car_tables *t = &w->car->t;
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        double sq;
        float d0;
        if (s->alive != 1) continue;
        d0 = s->d;
        sq = sqrt((double)s->pos[0] * s->pos[0]
                  + ((double)s->pos[1] * s->pos[1]
                     + (double)s->pos[2] * s->pos[2]));
        s->d = (float)sq;                               /* FST              */
        s->dd = (float)(((double)d0 - sq) * tps);       /* FSUBP, FMUL      */
        s->atten = 0.0f;
        s->doppler = 1.0f;
        if (s->fresh != 1)
            s->doppler = (float)((((double)s->dd + t->k_c) * t->k_inv_c
                                  - s->doppler) * t->k_dop + s->doppler);
        s->atten = 1.0f;
        if ((int32_t)fbits(s->d) > (int32_t)fbits(s->atten))  /* JLE bits   */
            s->atten = (float)(1.0 / (double)s->d);
        s->fresh = 0;
    }
}

/* FISTP of d x 1000.0 into a dword the callers compare UNSIGNED. */
static uint32_t dist_ms(const snd_world *w, float d)
{
    double v = (double)d * w->k_1000;
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)lrint(v);
}

/* 0x00460D87: the point of segment A-B nearest P (= 0), t on x and z only,
 * clamped to [0, 1]; t = 0 when A and B coincide in the plane. */
static void segment_point(const float a_in[3], const float b_in[3], float out[3])
{
    float ax = (float)((double)a_in[0] - 0.0), az = (float)((double)a_in[2] - 0.0);
    float bx = (float)((double)b_in[0] - 0.0), bz = (float)((double)b_in[2] - 0.0);
    float dx = (float)((double)ax - bx), dz = (float)((double)az - bz);
    double ad = (double)ax * dx + (double)az * dz;
    double bd = (double)bx * dx + (double)bz * dz;
    double dd = ad - bd;
    float t;
    int i;
    /* FCOM / AND AH,0x40: C3 is set for equal AND for unordered */
    if (dd == 0.0 || dd != dd) t = 0.0f;
    else t = (float)(ad / dd);
    /* FCOMPP / SHR AH,1: C0 (below, or unordered) decides each clamp */
    if (!(t >= 0.0f)) t = 0.0f;                         /* [0x0057B014]     */
    if (!(t < 1.0f)) t = 1.0f;                          /* [0x0057AFBC]     */
    for (i = 0; i < 3; i++)
        out[i] = (float)(((double)b_in[i] - a_in[i]) * t + a_in[i]);
}

/* 0x0040E62C: a line source keeps its nearest emitter. */
static void line_source(snd_world *w, int32_t id)
{
    uint32_t d1 = 0xFFFFFFFFu, d2 = 0xFFFFFFFFu;
    snd_world_slot *n1 = NULL, *n2 = NULL;
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        uint32_t dm;
        if (s->alive != 1 || s->id != id) continue;
        dm = dist_ms(w, s->d);
        if (dm < d1) {
            d2 = d1; n2 = n1;
            d1 = dm; n1 = s;
        } else if (dm < d2) {
            d2 = dm; n2 = s;
        }
        s->alive = 0;
        s->playing = 0;
    }
    if (!n1) return;
    n1->start = 1;
    n1->alive = 1;
    if (!n2) return;
    {
        float p[3];
        segment_point(n1->pos, n2->pos, p);
        memcpy(n1->pos, p, sizeof p);
    }
}

/* 0x0040E81B: a point source keeps one of its nearest two. */
static void point_source(snd_world *w, int32_t id)
{
    uint32_t d1 = 0xFFFFFFFFu, d2 = 0xFFFFFFFFu;
    snd_world_slot *n1 = NULL, *n2 = NULL;
    int j;
    uint32_t r;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        uint32_t dm;
        if (s->alive != 1 || s->id != id) continue;
        if (s->playing == 1) return;                    /* JE 0x0040E915    */
        dm = dist_ms(w, s->d);
        if (dm < d1) {
            /* the displaced pair is NOT cleared: 0x0040E8BE is unreachable */
            d2 = d1; n2 = n1;
            d1 = dm; n1 = s;
            continue;
        }
        if (dm < d2) { d2 = dm; n2 = s; }
        s->alive = 0;
    }
    if (!n2) return;
    r = sim_rnd_next_q(&w->rnd) & 1u;                   /* the STAND-IN rnd */
    w->picks++;
    n1->alive = (int32_t)r;
    n2->alive = (int32_t)(r ^ 1u);
}

/* 0x0040E916: one emitter per index, a playing one first. */
static void one_per_index(snd_world *w)
{
    int32_t mask = 0;
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++)
        if (w->slot[j].alive == 1 && w->slot[j].playing == 1)
            mask |= w->slot[j].bit;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        if (s->alive != 1 || s->playing == 1) continue;
        if (s->bit & mask) s->alive = 0;
        mask |= s->bit;
    }
}

/* 0x0040E967: the schedule - a delay drawn in the window, a repeat count. */
static void schedule(snd_world *w)
{
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        const snd_world_sched *e;
        uint32_t ax;
        if (s->alive != 1 || s->playing == 1) continue;
        /* [EDX*8 + 0x00511D44], EDX = index - 1; match() admits 1..21 only */
        e = &w->sched[s->index - 1];
        ax = (uint16_t)(e->hi - e->lo);                 /* SUB AX           */
        if (ax != 0) {                                  /* 0x0040188E       */
            ax = sim_rnd_next_q(&w->rnd) % ax;
            w->delays++;
        }
        ax = (ax & 0xFFFF0000u) | (uint16_t)(ax + e->lo);   /* ADD AX       */
        s->delay = (int32_t)ax;
        if (e->repeats != 0) {
            if ((int32_t)e->repeats == s->repeats) continue;
            s->repeats++;
        }
        s->start = 1;
    }
}

/* 0x0040E9C1: the post. */
static void play(snd_world *w)
{
    snd_car *c = w->car;
    int j;
    for (j = 0; j < SND_WORLD_SLOTS; j++) {
        snd_world_slot *s = &w->slot[j];
        snd_car_ctx x;
        if (s->alive != 1) continue;
        s->alive = 0;
        memset(&x, 0, sizeof x);
        x.id = s->id;
        if (s->start == 1) {
            int32_t vol = -0x6F, rate = -0x6F;
            s->start = 0;
            snd_car_defaults(c, s->id, &vol, &rate);
            s->vol = (float)vol;
            s->rate = (float)rate;
            s->end = c->clock + snd_car_length(c, s->id, rate)
                     + (uint32_t)s->delay;
            s->playing = 1;
        } else if (s->playing == 1) {
            if (snd_car_loop(c, s->id) != 1)
                x.id = (int32_t)((uint32_t)x.id | 0x80000000u);
        } else {
            continue;
        }
        x.vol = s->vol;
        x.rate = s->rate;
        x.pitch = s->doppler;
        x.atten = s->atten;
        memcpy(x.pos, s->pos, sizeof x.pos);
        snd_car_post2d(c, &x);
        if ((int32_t)(c->clock - s->end) >= 0) {        /* JS skips          */
            snd_car_stop(c, s->id);
            s->playing = 0;
        }
    }
}

static const int32_t LINE_IDS[6]  = { 0x18B, 0x18C, 0x18F, 0x190, 0x195, 0x193 };
static const int32_t POINT_IDS[12] = { 0x185, 0x186, 0x187, 0x188, 0x189, 0x18A,
                                       0x18D, 0x18E, 0x184, 0x192, 0x194, 0x197 };

void snd_world_tick(snd_world *w, uint32_t clock, float tps)
{
    int i;
    w->car->clock = clock;
    w->car->tps = tps;
    weather(w);                                          /* 0x0040E0CC      */
    match(w);                                            /* 0x0040E434 ...  */
    free_unseen(w);
    motion(w, tps);
    for (i = 0; i < 6; i++) line_source(w, LINE_IDS[i]);
    for (i = 0; i < 12; i++) point_source(w, POINT_IDS[i]);
    one_per_index(w);
    schedule(w);
    play(w);
}

void snd_world_hush(snd_world *w)
{
    int i;
    snd *s = w->car->s;
    if (!s || !s->be.id_stop || !s->be.id_playing) return;
    for (i = 0x184; i <= 0x19A; i++)
        if (w->car->v[i].open > 0 && s->be.id_playing(s->be.ud, i))
            s->be.id_stop(s->be.ud, i);
}
