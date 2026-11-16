/* camera.h - the original's in-race camera, its STEP half.
 *
 * is the reading; every function below names the
 * original's address.  The camera is stepped at 100 Hz INSIDE the car step
 * (0x00439E83 -> FUN_0044DEE4 -> FUN_004364A5 -> FUN_00437827) for a car with
 * an input device, and its whole state is the car record's +0x684..+0x6C8
 * (plus the query hints +0xEF0..+0xF00).  All of that is ABOVE the replayed
 * 1,668-byte prefix, so it is not a car_state member: it lives in sim_camera,
 * one per car, beside the car.
 *
 * WHY IT IS IN THE SIMULATION.  It is the original's order, and it touches
 * the simulation three ways: views 1-3 draw rnd_next
 * (the shake), the camera's ground queries set the polygon mask, and
 * +0x6F0 (view 2 or 3) gates the yaw-angle re-derivation (FUN_0043A3DE).  A
 * caller that does not model the camera leaves sim_world.camera NULL and the
 * step is exactly what it was before - trace_check relies on that.
 *
 * NOT PORTED: the replay's
 * trackside views 7-10 (FUN_004364A5's branch at 0x00436AE2) - retail cannot
 * select them in a race, and a view >= 7 here takes the ordinary path; the
 * external camera 0x00437150; the debug Alt+numpad selector 0x00439A88.
 *
 * The render half (0x00436D04 / 0x00436E4C: the view matrix, the lean, the
 * mirror) is port/game/camview.c. */
#ifndef RALLY_SIM_CAMERA_H
#define RALLY_SIM_CAMERA_H

#include "car.h"
#include "image.h"

#include <stdint.h>

struct sim_ground_geom;
struct sim_route;

#define SIM_CAMERA_VIEWS     16
#define SIM_CAMERA_CARS      23      /* [0x0057E10C] rows, 0x12C each        */
#define SIM_CAMERA_BLOCK     14      /* floats per view block, 0x38 bytes   */
#define SIM_CAMERA_NAMES     7       /* [0x005C660C]: the views F1 cycles   */

/* Everything the camera reads from the image (read at run time, never
 * transcribed).  `block` is the WORKING copy of the view table: the original
 * rewrites parts of it every tick (FUN_004364A5 views 1-3 from the car's row,
 * FUN_00437602 views 4-6 from the screen layout's table), and a write in one
 * tick is read by the next, so the copy is state - sim_camera_ctx owns one. */
typedef struct sim_camera_consts {
    float   block[SIM_CAMERA_VIEWS][SIM_CAMERA_BLOCK];  /* 0x005BA730         */
    float   chase[2][7];      /* 0x005BAAB0 one screen, 0x005BAACC split      */
    int32_t family[SIM_CAMERA_VIEWS];                    /* 0x005BA6AC         */
    int32_t eye[SIM_CAMERA_CARS][9];   /* 0x0057E10C + id*0x12C, +0x00..+0x20 */
    int32_t name[SIM_CAMERA_NAMES];    /* 0x005C660C: lexicon ids (268..274)  */
    int32_t nviews;           /* [0x005BA668] = 7 in the image and at all
                               * seven of its writers                        */
    float   inv65536;         /* [0x0057B024] 2^-16                           */
    float   eye_drop;         /* [0x005C6CF4] 0.7                             */
    float   shake_k;          /* [0x005C6DD0] 0.05                            */
    float   shake_max;        /* [0x005C6DCC] 0.02                            */
    float   shake_h;          /* [0x0057AFC4] 3.0                             */
    float   fish_min;         /* [0x005BA620] 128.0                           */
    float   one;              /* [0x0057AFBC] 1.0                             */
    float   k65536;           /* [0x0057B01C] 65536.0                         */
    float   k4096;            /* [0x0057B0AC] 4096.0                          */
    float   half;             /* [0x0057B0C4] 0.5                             */
    float   mirror_fish;      /* [0x0057B0B0] 256.0 (the mirror's pass)       */
    float   mirror_fish2;     /* [0x005BA5D4] 256.0 (the mirror's camera)     */
    float   mirror_rot[16];   /* [0x0059EC14] diag(-1, 1, -1, 1)              */
    int32_t mirror_at[12][2]; /* [0x005F44BC]: the inset's x, y at 800 x 600,
                               * by layout + 3 x player (+6 without the
                               * progress bar) - 0x00453E06                   */
    float   focal_k;          /* [0x005D6C60] 1/163840 (FUN_0044C516)         */
    float   inv2pi;           /* [0x0057B058] 1/(2 pi): binary angle units    */
    float   lean_div;         /* [0x0057AFD8] 10.0: the lean's speed scale    */
    float   lean_k;           /* [0x0057B054] 0.1:  the lean's steer gain     */
} sim_camera_consts;

/* 0; -1 if an address is not in the image. */
int sim_camera_consts_load(const ral_image *im, sim_camera_consts *k,
                           char *err, size_t errlen);

/* The car record's camera words.  Names are the overlay's where it has one
 * else the reading's. */
typedef struct sim_camera {
    int32_t view;         /* +0x684  p1 view                                  */
    int32_t nview;        /* +0x688  p1 nview: the requested view             */
    int32_t restore;      /* +0x68C  p1 cscnt: view + 1 to restore            */
    int32_t start;        /* +0x690  the blend's start (a copy of +0x008)     */
    int32_t blending;     /* +0x694  a blend is running                       */
    int32_t length;       /* +0x698  its length in ticks: 40, or 10           */
    float   t;            /* +0x69C  its fraction, [0, 1]                     */
    float   lean;         /* +0x6A0  views 1-2's lean (RENDER side)           */
    int32_t pos[3];       /* +0x6A4  xcampos, ycampos, zcampos (16.16)        */
    int32_t yaw;          /* +0x6B0  the lagged angles, 2^32 per turn         */
    int32_t roll;         /* +0x6B4                                           */
    int32_t pitch;        /* +0x6B8                                           */
    int32_t vpitch;       /* +0x6BC  the view's own angles                    */
    int32_t vyaw;         /* +0x6C0                                           */
    int32_t vroll;        /* +0x6C4                                           */
    float   fishval;      /* +0x6C8                                           */
    int32_t interior;     /* +0x6F0  view is 2 or 3 (FUN_0043D644)            */
    int32_t hint[5];      /* +0xEF0..+0xF00  the queries' polygon hints       */
} sim_camera;

/* The camera's world: what FUN_004364A5 reads that is not the car's.  One per
 * car that has a camera; sim_world.camera points at it. */
typedef struct sim_camera_ctx {
    const sim_camera_consts *k;
    sim_camera   cam;
    float        block[SIM_CAMERA_VIEWS][SIM_CAMERA_BLOCK];  /* working copy */
    int32_t      layout;      /* [0x005D6758] shaunsplitmode (2 = split)      */
    int32_t      numpl;       /* [0x005F0A54]                                 */
    int32_t      snap;        /* [0x006A24F0]: k = 1 (a replay seek)          */
    const struct sim_route *route;          /* [0x00983B38], may be NULL      */
    /* What the last step left in the process globals, for the caller. */
    uint32_t     mask;        /* [0x005D31A0]: 0x40 after any camera query,
                               * else left as it was                  */
    long         queries;     /* FUN_004375CA calls this step                 */
    float        shake[4];    /* [0x005EA4FC..508]                            */
    int          search;      /* this step's yaw search ran (diagnostic)      */
} sim_camera_ctx;

/* Fresh camera on a fresh car: the view `view` (every capture starts in 4,
 * CHASE 1 - what writes it before the race is unread), the working table
 * from the image. */
void sim_camera_init(sim_camera_ctx *c, const sim_camera_consts *k,
                     int32_t view);

/* FUN_0044DEE4 -> FUN_004364A5, the whole step half, for `car` (its record
 * after the step's physics), its binary angles +0x9C0..+0x9C8 and the step's
 * vertical velocity delta +0x90C.  `rnd` is rnd_next's state [0x004DB13C]
 * (views 1-3 draw it); `geom` the level's ground for the queries; `sin_table`
 * FUN_0041E368's.  Returns 0; -1 if a ground query refuses. */
int  sim_camera_step_q(sim_camera_ctx *c, const car_state *car,
                       const int32_t binary_angle[3], float yvel_delta,
                       uint32_t *rnd, const struct sim_ground_geom *geom,
                       const float *sin_table);

/* FUN_0043E00B's camera half: the lagged angles take the car's binary
 * angles, and a view saved by the forced switch is restored. */
void sim_camera_rest_q(sim_camera_ctx *c, const int32_t binary_angle[3]);

/* FUN_00439024 (F1, binding slot 9): the next view, then FUN_00438FB3.  Does
 * nothing during a blend.  Returns the lexicon id of the view's name to show
 * (FUN_00438C42's 0x00438F58), or -1 when nothing changed. */
int32_t sim_camera_next_view_q(sim_camera_ctx *c, int32_t clock);

/* FUN_00438FB3 alone: go to c->cam.nview - a cut across families, a 40-tick
 * blend within one. */
void sim_camera_change_q(sim_camera_ctx *c, int32_t clock);

#endif /* RALLY_SIM_CAMERA_H */
