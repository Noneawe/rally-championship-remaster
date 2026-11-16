/* worldsound.h - the weather's and the ambience's sounds,
 * is the reading.
 *
 * THE GAME'S LOGIC, TRANSCRIBED.  On the audio thread's 20 ms gate, after
 * the car records and the co-driver (0x0040D484):
 *
 *   0x0040E0CC  the weather: the thunder's slot (scheduled by the step's
 *               flag, sim/weather.c), rain 0x199 and wind 0x19A re-posted
 *               every tick at the step's levels, centred, no 1/d
 *   0x0040E434  the ambience: the render path's list of the stage's sound
 *               objects is matched into a 72-slot emitter table (0x0040E465),
 *               slots not seen are freed (0x0040E517), each gets its
 *               distance, Doppler and 1/d (0x0040E556); the line sources
 *               keep their nearest emitter, moved to the nearest point of
 *               the segment to the second (0x0040E5ED, 0x00460D87); the
 *               point sources keep one of their nearest two (0x0040E7A0);
 *               one emitter per index (0x0040E916); a schedule (0x0040E967);
 *               and the post (0x0040E9C1)
 *
 * and on the render path, once a frame after the main view and the mirror
 * (0x00452658 / 0x0045265D): the collection, 0x0045A38D / 0x0045A541.
 *
 * NOT TRANSCRIBED: the checkpoint chimes (arcade only) and the
 * fireworks (the championship's finish, not rally's path) - both named in
 * the reading; DS3D.
 *
 * A STAND-IN: the ambience's two random draws (the point sources' pick and
 * the schedule's delay) call rnd_next() ON THE AUDIO THREAD in the original,
 * on the generator the simulation's objects and camera draw from - so the
 * original's sim sequence depends on the audio thread's timing.  The port
 * gives this half its OWN copy of the generator (the same LFSR,
 * `rnd`), so the simulation stays a function of its inputs.  The thunder's
 * draw is in the step, and draws the sim's (weather.c). */
#ifndef RALLY_GAME_WORLDSOUND_H
#define RALLY_GAME_WORLDSOUND_H

#include <stdint.h>

#include "carsound.h"
#include "weather.h"
#include "stage.h"

struct ral_image;

#define SND_WORLD_SLOTS   72      /* 0x00510658, 0x50 bytes each             */
#define SND_WORLD_LIST    36      /* 0x00684844 / 0x00510388: CMP EAX,0x24   */
#define SND_WORLD_EMPTY   0x1888FFFF
#define SND_WORLD_INDICES 22      /* [0x00511CEC + i*4], i 0..21             */

/* One entry of the hand-off list, 0x14 bytes. */
typedef struct snd_world_item {
    int32_t key;                  /* +0x00  the object's anchor pnts index   */
    float   pos[3];               /* +0x04  in the view's frame, metres      */
    int32_t index;                /* +0x10  1..21                            */
} snd_world_item;

/* An emitter slot, by its offsets. */
typedef struct snd_world_slot {
    int32_t  key;                 /* +0x00  SND_WORLD_EMPTY when free         */
    int32_t  alive;               /* +0x04  seen / chosen this tick           */
    float    pos[3];              /* +0x08                                    */
    int32_t  id;                  /* +0x14  -2 when free                      */
    int32_t  index;               /* +0x18                                    */
    int32_t  bit;                 /* +0x1C  1 << index                        */
    int32_t  fresh;               /* +0x20                                    */
    int32_t  start;               /* +0x24                                    */
    int32_t  playing;             /* +0x28                                    */
    int32_t  repeats;             /* +0x2C                                    */
    int32_t  delay;               /* +0x30  ms, added to the end              */
    uint32_t end;                 /* +0x34                                    */
    float    rate, vol;           /* +0x38, +0x3C                             */
    float    d, dd, doppler, atten;  /* +0x40 .. +0x4C                       */
} snd_world_slot;

typedef struct snd_world_sched {
    uint16_t repeats, lo, hi;     /* 0x00511D44 + (i-1)*8: +2, +4, +6         */
} snd_world_sched;

typedef struct snd_world {
    snd_car        *car;          /* the sample player and its tables        */
    /* the image's */
    int32_t         ids[SND_WORLD_INDICES];
    snd_world_sched sched[SND_WORLD_INDICES];   /* [i - 1], 1..21            */
    snd_car_slot    thunder;      /* 0x00510364                              */
    float           thunder_pos[3];   /* 0x00510344: (0, 2, 1)               */
    int32_t         thunder_delay;    /* [0x00510350]: 0, never written      */
    float           k_1000;           /* the immediate 0x447A0000            */
    float           k_scale;          /* [0x0057B024] 2^-16                  */
    snd_world_slot  slot0;        /* a slot as .data holds it                */
    /* the thread's */
    const sim_weather *weather;   /* the step's levels (sim_race.weather)    */
    sim_weather    *weather_rw;   /* ... whose thunder flag the thread clears */
    int32_t        *fw;           /* the fireworks' two flags [0x0051037C]
                                   * (whoosh), [0x00510380] (fwboom), which the
                                   * thread clears; NULL: none               */
    int32_t        *chime;        /* the race's three chime flags
                                   * [0x00510334..3C] (sim_race.chime), which
                                   * the thread clears; NULL: none          */
    uint32_t        thunder_at;   /* [0x0051035C]                            */
    int32_t         thunder_wait; /* [0x00510360]                            */
    int32_t         nlist;        /* [0x00510384]                            */
    snd_world_item  list[SND_WORLD_LIST];       /* 0x00510388                */
    snd_world_slot  slot[SND_WORLD_SLOTS];
    uint32_t        rnd;          /* STAND-IN: the ambience's own rnd_next   */
    long            picks, delays;   /* draws made (diagnostic)              */
} snd_world;

/* The tables out of the image; 0, or -1. */
int  snd_world_load(snd_world *w, const struct ral_image *im, snd_car *car);

/* 0x0040D213's part (0x0040D38B..0x0040D403): the thunder, both levels'
 * consumers, every slot's key and id, the list.  `weather` is the race's
 * (NULL: silence); `seed` the stand-in generator's state. */
void snd_world_race(snd_world *w, sim_weather *weather, uint32_t seed);

/* 0x0045A38D + 0x0045A541: the stage's sound objects in the 5 x 5 cells
 * about the eye, from the visible-cell grid as the LAST view drawn left it
 * (the mirror's, when it was drawn:), at
 * the main view's draw radius `radius` ([0x005BD538], restored), through
 * that view's matrix [0x0059F014] (`view_m`, 16 floats). */
void snd_world_collect(snd_world *w, const stage *st, int32_t radius,
                       const float view_m[16]);

/* The same, from a grid the caller holds (the gate). */
void snd_world_collect_grid(snd_world *w, const int32_t *grid, int32_t radius,
                            const cdat_payload *cdat, const float view_m[16]);

/* One tick: 0x0040E0CC then 0x0040E434, at `clock` with [0x0050C860] =
 * `tps`.  Requests go to the car's player (and its req log). */
void snd_world_tick(snd_world *w, uint32_t clock, float tps);

/* 0x0040D213's world part at PAUSED / RETIRE? (stop_sounds_q):
 * command 7 and the reset above - the generator NOT reseeded. */
void snd_world_stop(snd_world *w);

/* STAND-IN, as snd_car_hush: every world voice stopped. */
void snd_world_hush(snd_world *w);

/* the fireworks' whoosh and fwboom (0x0040E247), for the finish
 * scene's world tick: each flag in w->fw cleared and its sound posted. */
void snd_world_fw(snd_world *w);

#endif
