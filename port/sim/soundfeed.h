/* soundfeed.h - the step's half of the car's sounds.
 * sections 3, 5.1 and 8.
 *
 * The original's 100 Hz step writes a snapshot of the car into the player's
 * FEEDBACK RECORD (the table 0x0050CD84, one record per roster slot: the
 * writers index it by [0x006CE084] & 7), and the audio thread's 20 ms tick
 * reads it back (port/game/carsound.c).  sim_sound_feed is the part of that
 * record the step writes, at the record's own offsets; the audio half reads
 * it, and clears the one-shot triggers it consumes, exactly as the thread
 * does through the shared record.
 *
 * What the step writes into the CAR record (and so into the replayed prefix,
 * trace_check's table) is here too: FUN_0043C14D's sound revs car+0x1E4 and
 * speed car+0x294, and 0x0045A626's car+0x28C / +0x290 (and +0x294's zero
 * while all four wheels are airborne).  Those run on every step, with or
 * without a record. */
#ifndef RALLY_SIM_SOUNDFEED_H
#define RALLY_SIM_SOUNDFEED_H

#include <stdint.h>

#include "car.h"

/* One of the record's four "wheels", stride 0x14 from +0x44.  0x0045A626
 * writes the same four copies (the car's, not a wheel's). */
typedef struct sim_sound_wheel {
    float   speed;              /* +0x44  car+0x294                           */
    int32_t _48;                /* +0x48  not written by the step             */
    float   gate;               /* +0x4C  car+0x290, the roll gate            */
    float   skid;               /* +0x50  car+0x28C x [0x0057B0B0] (256.0)    */
    int32_t material;           /* +0x54  car+0x930, the surface class        */
} sim_sound_wheel;

/* "None" in the impact and scrape ids: 0x0040D213's reset and the audio
 * thread's consumption both store -2 (0x0040DFB1, 0x0040E014). */
#define SIM_SOUND_NONE (-2)

typedef struct sim_sound_feed {
    int32_t outside;            /* +0x0C  car+0x6F0 ^ 1: 0 in views 2 and 3   */
    int32_t fresh;              /* +0x10  1 = new data since the last tick    */
    int32_t revs;               /* +0x14  car+0x1E4, the sound revs           */
    int32_t throttle;           /* +0x18  car+0x638 clamped 0..256            */
    float   whl;                /* +0x1C  car+0x1C0, the mean wheel speed     */
    float   pos[3];             /* +0x2C  the car in the camera's frame       */
    float   fwd[3];             /* +0x38  car+0xD4.. turned the same way      */
    sim_sound_wheel w[4];       /* +0x44 .. +0x90                             */
    int32_t airborne;           /* +0xAC  car+0x248 & car+0x24C               */
    int32_t gear;               /* +0xE8  car+0x1D4                           */
    /* the one-shot slots' triggers the step sets */
    int32_t gearchng;           /* +0x180 slot +0x17C: shift result 1         */
    int32_t geargrnd;           /* +0x198 slot +0x194: shift result -1        */
    float   blowout_pos[3];     /* +0x214                                     */
    int32_t blowout;            /* +0x224 slot +0x220                         */
    int32_t impact_id;          /* +0x238 SIM_SOUND_NONE, or a sound id       */
    int32_t impact_vol;         /* +0x23C 0..64                               */
    float   impact_pos[3];      /* +0x240                                     */
    int32_t scrape_id;          /* +0x24C SIM_SOUND_NONE, or 0xB9             */
    int32_t scrape_vol;         /* +0x250 0 ends the scrape                   */
    float   scrape_pos[3];      /* +0x254                                     */
    int32_t shatter;            /* +0x264 slot +0x260 (the render path's)     */
} sim_sound_feed;

/* The camera the feed is heard from: the position is car+0x6A4 of the
 * player's car ([0x005F0A70]'s when [0x005F0A54] numpl == 1, else the stepped
 * car's own), and `rot` the render's view rotation [0x0059EF94], written by
 * the last rendered frame (camview.c) - row vectors, rows of four, a pure
 * rotation.  NULL pos: the snapshot is skipped. */
typedef struct sim_sound_view {
    const int32_t *pos;         /* 16.16                                      */
    float          rot[16];
} sim_sound_view;

/* 0x0040D213's reset of the step's words: everything 0, the impact and the
 * scrape SIM_SOUND_NONE. */
void sim_sound_feed_init(sim_sound_feed *f);

/* FUN_0043C14D (0x00439E07) without its HUD speed (car+0xAB0, which hud.c
 * computes for itself) and 0x0045A626's car-record half: car+0x294, car+0x1E4,
 * car+0x28C, car+0x290, the airborne zeroing.  `idle` is [0x005C6E30], the
 * class's idle revs.  Then, with a record, the snapshot (0x0045A626) and the
 * shift triggers (0x0045A2D9).  `material` is car+0x930; `outside` car+0x6F0
 * ^ 1. */
void sim_sound_step_q(car_state *car, float idle, int32_t material,
                      int32_t camera_view_2_3, sim_sound_feed *f,
                      const sim_sound_view *v);

/* 0x00439E25: car+0x1C0 = (fwhlspd + rwhlspd) * [0x0057B0C4] (0.5). */
void sim_sound_wheel_mean_q(car_state *car);

/* The step's other writers, each only for a car a local player drives
 * ([0x005F4540 + car*4] != -1 - the caller's test):
 *   0x0045A8B8 an object hit: `id` from [0x006847E0 + (material & 15)*4], the
 *              volume round(car+0x1C4 * 50) - 10, heard at car + `off`
 *   0x0045AA51 a body hit (after every FUN_0045CCD3 but the scrape's):
 *              0xB5 at round(normal * 50) - 1, normal = [0x00685E2C]
 *   0x0045AAED the scrape: 0xB9 at round(car+0x1C4 * 50) - 1
 *   0x0045AB80 the scrape's end: 0xB9 at 0
 * A volume of 0 posts nothing (the JZ after each volume helper), except the
 * scrape's, which always posts.  Positions are in the camera's frame.
 * NOT PORTED: 0x0045A98A, the car-car hit (0xB5 at round([0x006869F4] x 200)
 * - 10): the port does not model [0x006869F4], and a one-player rally race
 * has no other car (overlap.c counts the hits). */
void sim_sound_object_hit_q(const car_state *car, sim_sound_feed *f,
                            const sim_sound_view *v, int32_t id,
                            const int32_t off[3]);
void sim_sound_body_hit_q(const car_state *car, sim_sound_feed *f,
                          const sim_sound_view *v, float normal);
void sim_sound_scrape_q(const car_state *car, sim_sound_feed *f,
                        const sim_sound_view *v);
void sim_sound_scrape_end_q(sim_sound_feed *f);

/* 0x0045A7B4, a tyre coming off (parts_left.c): blowout (+0x224) = 1,
 * blowout_pos (+0x214) = the car in the frame of its OWN camera
 * (car+0x6A4, the working block's [0x006CD308] - not the player's) turned
 * by [0x0059EF94], fresh = 1.  Into any slot's record: it does not test for
 * a local player.  v->pos NULL: the position is left as it is. */
void sim_sound_blowout_q(const car_state *car, sim_sound_feed *f,
                         const sim_sound_view *v);

/* [0x006847E0]: the impact sound by OBJSINFO +0x24's low four bits. */
extern const int32_t sim_sound_impact_by_material[16];

#endif
