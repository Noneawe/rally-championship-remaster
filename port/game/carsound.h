/* carsound.h - the audio thread's half of the car's sounds,
 * sections 1, 2, 4-9.
 *
 * THE GAME'S LOGIC, TRANSCRIBED: on the thread's 20 ms gate (0x0040F031,
 * sound.c's snd_cd_tick) the original runs, per open player record,
 *
 *   0x0040D724  0x0040D773 the per-record tick (extrapolated revs, the
 *               throttle's smoothing, the state, the shift cooldown, the
 *               engine's own one-shot flags, distance, Doppler, 1/d), then
 *   0x0040DB8C / 0x0040DC09 / 0x0040DC7C  the engine's three voices
 *   0x0040DD0E  the surface: roll, then skid
 *   0x0040DF19  the car's one-shots (four of them in the inside views only)
 *   0x0040DFA5  an impact            0x0040E008  the scrape
 *   0x0040E072  blow-off, pops, bang
 *
 * each of which ends in the 2D poster 0x0040EF3F (volume x scale x 1/d, the
 * pan from the bearing, the rate x Doppler) or the stop 0x0040EFF4, which
 * post command 1 (0x00409A0D) to the sample player 0x0040A89E.  The player
 * is transcribed too (snd_car_post): a voice per sound id, started or
 * updated, volume 0 stops it, a one-shot that is still playing is dropped,
 * loop type 3 rewinds.
 *
 * NOT TRANSCRIBED: the channel limit and its steal (8 hardware channels,
 * 0x0040A8EF) - the port's mixer has 32 voices and never steals; weather and
 * ambience (0x0040E0CC,
 * 0x0040E434:); the fireworks; DS3D (bit 20 of [0x004F327C], off in a
 * stock install).
 *
 * Every request the half makes is a snd_car_req: the gate (carsound_check)
 * compares them, line by line, with tools/carsound.py's second transcription
 * driven by the same feed. */
#ifndef RALLY_GAME_CARSOUND_H
#define RALLY_GAME_CARSOUND_H

#include <stdint.h>
#include <stdio.h>

#include "soundfeed.h"

struct ral_image;
struct snd;

#define SND_CAR_IDS     0x1A1     /* the sound table's 417 records           */
#define SND_CAR_AI      3         /* the arcade's records 1..3       */
#define SND_CAR_MODELS  24        /* [0x0050F304 + car*4], car 0..0x17        */

/* A record of the sound table 0x0050571C + id*0x40, the fields the race's
 * sounds read (carsound.py table). */
typedef struct snd_car_sound {
    char     file[48];   /* +0x18 -> "files\d\snd\...", "" for none           */
    int32_t  loop;       /* +0x20  1 loops, 3 rewinds, 0 overlapping, 2 once  */
    uint32_t slider;     /* +0x2C  the volume global's address                */
    float    scale;      /* +0x30  the poster's volume factor                 */
    int32_t  volume;     /* +0x34  the default volume (EBX = -0x6F)           */
    int32_t  rate;       /* +0x3C  the default rate; -0x70 = the file's own   */
} snd_car_sound;

/* An engine voice of a model's block, as 0x0040D4C7 leaves it (floats, the
 * ranges as slopes). */
typedef struct snd_car_voice {
    int32_t id;
    float   f_lo, f_slope;   /* +0x10, +0x18 */
    float   v_lo, v_slope;   /* +0x1C, +0x24 */
} snd_car_voice;

typedef struct snd_car_model {
    uint32_t      flags;     /* +0  0x2000 bang; 0x1000 + id pops, blow-off  */
    float         lo, k;     /* +4 the rev range's low end, +0xC 1/(hi - lo) */
    snd_car_voice v[3];      /* +0x10, +0x38, +0x60                          */
} snd_car_model;

/* The image's tables, read once. */
typedef struct snd_car_tables {
    snd_car_sound snd[SND_CAR_IDS];
    snd_car_model model[SND_CAR_MODELS];
    snd_car_voice other;                 /* 0x0050FFC8: othercar.wav          */
    int32_t       surf_class[16];        /* [0x005102F4]                      */
    int32_t       roll_type[8], roll[8][8];   /* [0x00510134 + class*8]       */
    int32_t       skid_type[8], skid[8][8];   /* [0x00510138 + class*8]       */
    int32_t       slot_id[10];           /* record 0's .data, carsound.py events */
    float         k_whl;                 /* [0x00510108] 0.0140625            */
    float         k_inside_eng;          /* [0x00510118] 0.714                */
    float         k_inside_whl;          /* [0x00510114] 1.4                  */
    float         k_sum;                 /* [0x005100FC] 0.4                  */
    float         k_64;                  /* [0x00510100] 64.0                 */
    float         k_tenth;               /* [0x005100F4] 0.1                  */
    float         k_surf;                /* [0x0051011C] 0.625                */
    float         k_c, k_inv_c;          /* [0x0050C86C] 337.52, [0x0050C870] */
    float         k_dop;                 /* [0x005100F8] 0.1                  */
    float         k_pan0, k_pan;         /* [0x0050C874] 64, [0x0050C878]     */
    float         k_bang;                /* 0x3C888889 (1/60), at 0x0040D9C9  */
} snd_car_tables;

/* A one-shot slot (0x0040E3C1): {id, trigger, playing, end, volume, rate}. */
typedef struct snd_car_slot {
    int32_t  id, trig, playing;
    uint32_t end;
    int32_t  vol, rate;
} snd_car_slot;

enum {  /* the slots, in record order */
    SND_SLOT_WIPER, SND_SLOT_SWITCH, SND_SLOT_GEARCHNG, SND_SLOT_GEARGRND,
    SND_SLOT_IGNITION, SND_SLOT_BLOWOFF, SND_SLOT_POPS, SND_SLOT_BANG,
    SND_SLOT_BLOWOUT, SND_SLOT_SHATTER, SND_SLOTS
};

/* The audio thread's half of a feedback record, named by its offsets. */
typedef struct snd_car_rec {
    int32_t  player;          /* +0x00  0 / 1; negative an AI car            */
    int32_t  car;             /* +0x04  the car (the engine model)           */
    int32_t  on;              /* +0x08                                       */
    int32_t  reset;           /* +0x20  state 3 once; its writer is unfound  */
    int32_t  gear0;           /* +0x24  "gear == 0" (but see 0x0040D9EF)     */
    int32_t  revs;            /* +0x14  as the thread leaves it              */
    uint32_t t0, t1;          /* +0x94, +0x98  the last two updates' times   */
    int32_t  v0, v1;          /* +0x9C, +0xA0  ... and sound revs            */
    int32_t  lastgear;        /* +0xEC                                       */
    int32_t  bang_set;        /* +0xF0  written, read by nothing here        */
    int32_t  cool;            /* +0xF4  the whine's cooldown                 */
    int32_t  blow_latch;      /* +0xF8                                       */
    int32_t  lastdir;         /* +0xFC                                       */
    float    x, thr, whl;     /* +0x100, +0x104, +0x108                      */
    int32_t  state, pstate;   /* +0x10C, +0x110                              */
    int32_t  smooth, diff;    /* +0x118, +0x11C                              */
    int32_t  rises, dir, falls; /* +0x128, +0x12C, +0x130                    */
    float    d, dd, doppler, atten;  /* +0x134 .. +0x140                     */
    int32_t  roll_id, skid_id;       /* +0x144, +0x148                       */
    int32_t  w284, w28c, w290, mat;  /* +0x284/+0x28C/+0x290/+0x294: written,
                                      * read by nothing in the module        */
    snd_car_slot slot[SND_SLOTS];
    int32_t  pops_wait;       /* +0x1F4                                      */
    float    bang_pitch;      /* +0x210                                      */
} snd_car_rec;

/* One command-1 post: the gate's unit. */
typedef struct snd_car_req {
    int32_t id, vol, pan, rate;
} snd_car_req;

/* 64 for the car; the world adds rain, wind, the thunder and one post per
 * playing emitter of the 72 (worldsound.h). */
#define SND_CAR_LOG 160

typedef struct snd_car {
    snd_car_tables t;
    snd_car_rec    rec;           /* record 0 - the one player              */
    sim_sound_feed *feed;         /* its step half (the race's snd[0])      */
    /* the arcade's AI records, 1..SND_CAR_AI (0x0040D5EB / 0x0040D63C).
     * ai_feed[k] is roster slot k+1's snd[]; NULL = no such record.
     * ai_driver[k] is that car's +0x25C, which the caller refreshes each
     * tick. ai_on turns the arcade arm on ([0x0050F300] == 1). */
    snd_car_rec    ai[SND_CAR_AI];
    sim_sound_feed *ai_feed[SND_CAR_AI];
    int32_t        ai_driver[SND_CAR_AI];
    int            ai_on;
    int32_t        near_a, near_b;    /* [0x0050F2F4] / [0x0050F2F8]        */
    uint32_t       clock;         /* [0x0050C85C] at the tick               */
    float          tps;           /* [0x0050C860] ticks per second          */
    /* the requests this tick made, in order */
    snd_car_req    req[SND_CAR_LOG];
    int            nreq;
    /* the sample player's per-id voice state (0x0040A89E): playing, and
     * what was last applied, so an update changes only what differs */
    struct {
        int      open, on;
        int32_t  vol, pan, rate;
        uint32_t frames, file_rate;
    } v[SND_CAR_IDS];
    struct snd    *s;             /* the slider values, the backend         */
    FILE          *log;           /* --sound-log: one line per request      */
} snd_car;

/* The tables out of the image; 0, or -1 if an address is not in it. */
int  snd_car_tables_load(snd_car_tables *t, const struct ral_image *im);

/* Record 0 as the image's .data holds it, for `car`, fed by `feed`. */
void snd_car_init(snd_car *c, const snd_car_tables *t, struct snd *s,
                  int32_t car, sim_sound_feed *feed);

/* the arcade's rivals.  Hands record k+1 (k = 0..2) its step half
 * `feed` (the race's snd[k+1]) and switches 0x0040D5EB's arm on; call before
 * snd_car_race.  The driver words are set per tick in c->ai_driver. */
void snd_car_arcade(snd_car *c, sim_sound_feed *const feed[SND_CAR_AI]);

/* 0x0040D213 for the record: the slots' triggers and playing flags cleared,
 * the revs, throttle and speeds 0; and 0x0040D423: opened.  Every file the
 * race can ask for loaded now, before the start (0x0040EE51's set). */
void snd_car_race(snd_car *c);

/* One tick of the thread at `clock` (ms) with [0x0050C860] = `tps`:
 * 0x0040D63C for the player's record.  Records this tick's requests in
 * c->req and hands each to the sample player. */
void snd_car_tick(snd_car *c, uint32_t clock, float tps);

/* 0x0040D213 for the record, as `stop_sounds_q` (0x0045ADE6) runs it
 * at PAUSED and RETIRE?: command 7 (every voice stopped - snd_car_hush), the
 * triggers and the step's words cleared, the record CLOSED (+0x08 = 0), so
 * the tick does nothing until 0x0045ADAD sets +0x08 = 1 again. */
void snd_car_close(snd_car *c);

/* Every voice of the race's sounds stopped, the record left as it is: the
 * command 7 half of snd_car_close.  STAND-IN where the port calls it alone -
 * the end of a replay and after the finish, while no step runs (the thread
 * would otherwise extrapolate the revs from the last two updates without
 * end); what the original does at the finish is unread.  The pause's was
 * this until read it (snd_car_close).. */
void snd_car_hush(snd_car *c);

/* The sample player, 0x0040A89E, for a race sound: EAX id, EBX volume,
 * ECX pan, EDX rate. */
void snd_car_post(snd_car *c, int32_t id, int32_t vol, int32_t pan,
                  int32_t rate);

/* ---- the module's shared machinery, for worldsound.c ---------- */

/* The context 0x0050C830: +0 id, +4 rate, +8 volume, +0x0C..+0x14 the
 * position in the camera's frame, +0x1C the bearing, +0x20 the pitch factor,
 * +0x24 the attenuation. */
typedef struct snd_car_ctx {
    int32_t id;
    float   rate, vol, pos[3], ang, pitch, atten;
} snd_car_ctx;

void     snd_car_post2d(snd_car *c, snd_car_ctx *x);           /* 0x0040EF3F */
void     snd_car_stop(snd_car *c, int32_t id);                 /* 0x0040EFF4 */
void     snd_car_defaults(snd_car *c, int32_t id, int32_t *vol,
                          int32_t *rate);                      /* 0x0040B918 */
uint32_t snd_car_length(snd_car *c, int32_t id, int32_t rate); /* 0x0040B8B2 */
void     snd_car_slot_service(snd_car *c, snd_car_slot *p, int32_t *trig,
                      snd_car_ctx *x);                         /* 0x0040E3C1 */
/* The loop type of an id (the sound record's +0x20), -1 if out of range. */
int32_t  snd_car_loop(const snd_car *c, int32_t id);

#endif
