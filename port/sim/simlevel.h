/* simlevel.h - one place that loads what the simulation needs from the user's
 * own files, and runs one replayed outer tick on it.
 *
 * A TOOL-SIDE helper, like objload.h, not part of rally_sim: it opens files.
 *
 * WHY THIS EXISTS.  trace_check built the simulation's inputs in its main()
 * (the image's constant tables, the level's polygons with their normals filled,
 * the surface level, the ground-query geometry, the placed objects, the car's
 * collision points), and then set up the outer tick in two places: phase B's
 * re-seeded step in run, and the chained step in chain_pass.  the
 * rally executable needs exactly the same, and its gate is that rally's
 * per-step car state equals trace_check's bit for bit.  So the load and the
 * step are here, once, and trace_check and rally both call them.  The code is
 * MOVED from trace_check.c, not rewritten: the order of every call is the
 * order it had there, because the order is part of what the gate compares
 * (sim_object_state_init seeds an LFSR; sim_ground_points_init clears the
 * hints the query chains from).
 *
 * WHAT IT DOES NOT DO.  It prints nothing.  trace_check's report quotes the
 * figures this loader leaves in sim_level (polygon counts, stale normals,
 * object counts, the collision model) in the order it always printed them,
 * which is what keeps its output byte-identical across the move. */
#ifndef RALLY_SIM_SIMLEVEL_H
#define RALLY_SIM_SIMLEVEL_H

#include "physics.h"
#include "objload.h"
#include "car.h"
#include "cdat.h"

#include <stddef.h>
#include <stdint.h>

typedef struct sim_level {
    /* ---- from the game image (RAL.EXE) ---------------------------------- */
    sim_consts        k;
    dt_consts         dk;
    car_params_table  tab;
    sim_surface_table st;
    float             sin_tab[SIM_SIN_ENTRIES];

    /* ---- the level: cdat00c, LOCALDAT, TEXTINFO ------------------------- */
    int               have_level;
    unsigned char    *raw, *ld, *ti;          /* the three files as read       */
    size_t            raw_len, ld_len, ti_len;
    cdat_payload      pl;
    /* The writable copy of the polygons.  The game's load fills each
     * polygon's two triangle normals (store_norms_q); the file holds zeros.
     * Object hits animate instance records in it, so it is live state. */
    uint8_t          *pols;
    /* ... and as it stood after the load, for sim_level_reset_objects. */
    uint8_t          *pols_pristine;
    int               norms_stale;            /* sim_level_store_norms_q's     */
    sim_surface_level lv;
    int               branches;               /* 1 if the host has no RCPSS    */
    sim_ground_geom   geom[2];                /* [0] x87, [1] host RCPSS       */
    sim_ground_inputs in;                     /* in.pts == &pts                */
    /* the route (setbestline_q), built from the pristine pols for
     * the course direction sim_level_build_route was last given (0 at
     * load).  have_route 0: the level's markers would not build one. */
    sim_route         route;
    int               have_route;
    /* FUN_0043A16A's six LOCALDAT words and the noise map (weather.h),
     * for the steps sim_replay_step runs with a camera. */
    sim_weather_level weather;
    /* FUN_0044CD28's three ramps, from LOCALDAT +0x600/+0x60C/+0x1898/
     * +0x18A4 (ground.h).  have_light 0: LOCALDAT too short for them. */
    sim_ground_light  light;
    int               have_light;

    /* ---- the car's collision points (files/b) --------------------------- */
    sim_collision_points pts;
    int               model_points;           /* pts is the car's model's     */
    int32_t           collision_model;        /* FUN_0046102B(carnum) + 100    */

    /* ---- the level's placed objects (files) ----------------------------- */
    int               have_objects;
    sim_objects       objs;
    sim_object_state  ostate;
    simobj_stats      ost;
} sim_level;

/* Nothing loaded.  sim_level is large (the ground geometry); allocate it
 * statically or on the heap. */
void sim_level_init(sim_level *L);

/* The image's four constant tables and the sine table.  0, or -1 with `err`
 * set (trace_check reports that as a SKIP: the image is the user's own). */
int  sim_level_load_image(sim_level *L, const char *image_path,
                          char *err, size_t errlen);

/* The level the car is driven on.  Returns 0; -1 if a file cannot be read
 * (a SKIP); -2 if a file was read and is not a usable level (a FAIL - the
 * parsers are gated over this corpus).  Fills pols with its normals, the
 * surface level, both ground-query branches, the zero collision-point
 * placeholders and the ground inputs. */
int  sim_level_load_stage(sim_level *L, const char *cdat_path,
                          const char *localdat_path, const char *textinfo_path,
                          char *err, size_t errlen);

/* (Re)build L->route for course direction `variant` ([0x006CE08C]).  0, or
 * -1 if the level's markers do not build one. */
int  sim_level_build_route(sim_level *L, int32_t variant);

/* The level's placed objects, `files_dir` being the assets' `files`
 * directory.  0, or the non-zero code simobj_load_level returned.  Leaves the
 * object state initialised over pols with LFSR seed 1 - the game's own seed is
 * clock-derived and unknown. */
int  sim_level_load_objects(sim_level *L, const char *files_dir);

/* The car's collision points from its collision model in `files_b_dir`
 * 0; -1 if the model cannot be read or parsed;
 * -2 if it is refused as a car collision model.  `path_out` gets the path
 * tried, for the caller's message. */
int  sim_level_load_car_points(sim_level *L, const char *files_b_dir,
                               int32_t carnum, char *path_out,
                               size_t path_len);

/* The same for another car (the arcade's rivals): into `out`, with
 * its collision model in `model_out`.  Same returns. */
int  sim_level_read_car_points(sim_level *L, const char *files_b_dir,
                               int32_t carnum, sim_collision_points *out,
                               int32_t *model_out, char *path_out,
                               size_t path_len);

/* Put pols back as the load left it and re-initialise the object state - what
 * the chained pass does before each pass, so that a knocked-over object
 * from an earlier pass is not still lying there. */
void sim_level_reset_objects(sim_level *L);

void sim_level_free(sim_level *L);

/* ---- one replayed outer tick ------------------------------------------- *
 *
 * The capture's car, stepped by sim_tick_run with everything the capture does
 * not carry supplied the way trace_check has always supplied it:
 *
 *  the input device's four words come from the record the step is going
 *     TO, and the tick's input stage (input.c) turns them into
 *     +0x638..+0x650 as the game does.  ~~The stage's OUTPUTS come from
 *     that record, because the stage is not ported~~ - until they did;
 *     now nothing else is copied from it;
 *  [0x005C7478] (rev_q) is the port's own, carried from the last step;
 *  the damage model's process globals are seeded from `damage_from`, the
 *     record the step starts from;
 *  gmode 0, one car, an input device, object_mask 0 on entry.
 *
 * `r` holds the per-car ground hints and the car geometry, so one sim_replay
 * is one continuous run: re-initialise it (sim_replay_init) wherever a run
 * starts over. */
/* the arcade's rivals, slots 1..3 - each its own record, handling,
 * collision points, ground hints and step scratch, as car 0's below. */
#define SIM_REPLAY_RIVALS 3

typedef struct sim_replay_rival {
    car_state            car;          /* the slot's record                  */
    const car_params    *p;
    sim_collision_points pts;
    sim_ground_points    points;
    sim_ground           ground;
    car_geometry         geom;
    /* Above the record and carried from step to step, as the working
     * block's copy-out carries them: +0x9C0..+0x9C8 and +0x9CC, the active
     * frame another car's FUN_0045D16A reads - zeroed, every corner
     * of every other car would test inside it. */
    int32_t              binary_angle[3];
    float                active_frame[16];
    sim_world            w;
} sim_replay_rival;

typedef struct sim_replay {
    sim_level          *L;
    const car_params   *p;
    sim_ground_points   points;
    sim_ground          ground;
    car_geometry        geom;
    float               rev_q;
    sim_input_globals   input;        /* the input stage's process state  */
    int                 settle;       /* sim_replay_start's, while settling */
    /* the race rules (race.c), over L's route as sim_replay_init
     * found it, gmode 0 and one player - the mode sim_replay_step ticks in.
     * have_race 0 (no route) leaves the rules unmodelled. */
    sim_race            race;
    int                 have_race;
    /* car+0x9C0..+0x9C8, the binary angles, carried from step to step like
     * rev_q: FUN_0043A3DE leaves them as they were while all four wheels are
     * airborne, and sim_replay_step zeroes its sim_world.  Until they
     * were 0 on every airborne step; the route tracker's wrong-way test
     * (FUN_0043F913) is what showed it - 139 of run 18's 174 airborne steps. */
    int32_t             binary_angle[3];
    /* the car's camera (camera.h), or NULL - sim_replay_init leaves
     * it NULL, and trace_check never sets it, so its steps are as before.
     * Non-NULL: every step (the settle's too) runs the camera, the start's
     * FUN_0043E00B gives it the car's angles, and the object tick's mask
     * inputs are its view and blend flag. */
    struct sim_camera_ctx *camera;
    /* with rivals, every step ticks four slots (numcars 4, gmode 2's
     * arcade_game_mode_init), the rivals with no device (FUN_00438880's
     * zero words) and the arcade's streamer after the loop.  nrivals 0 is
     * the one-car tick every gate before measured. */
    int                 nrivals;
    sim_replay_rival    rival[SIM_REPLAY_RIVALS];
} sim_replay;

void sim_replay_init(sim_replay *r, sim_level *L, const car_params *p);

/* Give slot `slot` (1..3) a rival car: its handling and collision points,
 * a fresh ground state, its wheel z from the points.  The record is the
 * caller's to load (r->rival[slot - 1].car).  Sets nrivals to 3 - the
 * arcade always has four cars.  0, or -1 for a bad slot. */
int  sim_replay_add_rival(sim_replay *r, int slot, const car_params *p,
                          const sim_collision_points *pts);

/* Step `car` in place.  `inputs` is the capture's record at the step's END
 * (frame n when stepping n-1 -> n); `damage_from` is the record the step
 * starts from.  `w` receives the step's scratch (zeroed first).  Returns
 * sim_tick_run's code. */
int  sim_replay_step(sim_replay *r, car_state *car, const car_state *inputs,
                     const car_state *damage_from, sim_world *w);

/* ---- the race start (place.c) ----------------------------------- *
 *
 * raceinit_q for one car on a FRESH sim_replay (sim_replay_init, and the
 * objects reset): FUN_00444639's placement and leveller, resettimers_q with
 * `seed` as the car's LFSR, then `settle_steps` FUN_0045E3FC steps
 * (SIM_SETTLE_STEPS).  `car` comes in as the car's load left it - the
 * configuration fields, every state field zero (sim_start_record_q's job in
 * the caller) - and goes out as it stands before the race's first tick.
 * The hints, rev_q and the input stage's state are left in `r` as the
 * settle left them, so the first tick continues from them.  Returns 0; -1
 * when the level has no route or the start is refused; a step's error. */
int  sim_replay_start(sim_replay *r, car_state *car, const sim_start_mode *m,
                      uint32_t slot, uint32_t seed, int settle_steps);

/* ---- the state-dump capture, car 0 ------------------------------------- *
 *
 * each record is a u32 frame index and four 1,668-byte car
 * blocks.  This reads car 0's block of every record into memory (61 MB for run
 * 18's 36,754 frames) - what a viewer that seeks needs.  trace_check keeps its
 * own sequential reader, because it also reads the other three cars. */
typedef struct sim_capture {
    long           frames;
    unsigned char *car0;          /* frames * CAR_REPLAY_PREFIX bytes          */
    long           seq_bad;       /* frame indices that were not 0..N-1        */
} sim_capture;

/* 0; -1 unreadable; -2 not a whole number of records; -3 out of memory. */
int  sim_capture_load(sim_capture *c, const char *path);
const car_state *sim_capture_car(const sim_capture *c, long frame);
void sim_capture_free(sim_capture *c);

#endif
