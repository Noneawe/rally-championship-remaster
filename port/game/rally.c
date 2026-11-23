/* rally.c - the game executable: first as a REPLAY VIEWER.
 * and DRIVEN, with --live.
 *
 * The port's first program, as opposed to its probes and test
 * harnesses: it loads a stage and a car, plays the state-dump capture's race
 * through the ORIGINAL'S main-loop architecture, and draws the car where the
 * simulation puts it.  ~~Live input replaces the replay.~~  With
 * --live the KEYBOARD drives the car instead: the capture supplies
 * only the starting car, at --start, and every step after that is the port's
 * own, from the keys through the ported input stage (port/sim/input.c).
 *
 * and STARTED, with no capture at all.
 *
 *   rally [--assets DIR] [--image RAL.EXE] [--stage LEVEL33]
 *         --replay statehook.bin [--reseed N] [--start FRAME]
 *         [--live [--keys SCRIPT]]
 *   rally [--assets DIR] [--stage LEVELnn] [--car N] [--direction D]
 *         [--seed N] [--keys SCRIPT]
 *
 * WITHOUT --replay the run is live, and the car starts the game's
 * way: raceinit_q's placement on the stage's own start line, the leveller,
 * resettimers and the fifty-step settle (port/sim/place.c, checked against
 * both captures' first frames by start-diff).  The car's LOAD - its setup
 * sliders and tables - is the game's (car_load.h): in a single stage
 * (--gmode 0, the default) the stage's auto-setup.
 *         [--window WxH | --no-window] [--radius R]
 *         [--frame-ms N | --frame-ms jitter] [--exit-at-end] [--steps N]
 *         [--dump FILE] [--shot FRAME:FILE.ppm ...]
 *
 * --LIVE.  The four device words come from car_input.c - FUN_00408D37's
 * keyboard path over car_keys.h, the original's bindings (the
 * image's player 1, or from the menus the chosen player's record) - or from
 * a --keys script, which drives a live run without a human so that it
 * repeats.  A live run is a FREE RUN: there is nothing to re-seed from, and
 * is what to read its stability against.  The start is
 * the capture's frame --start (0, the default, is the grid: the car is held
 * through the countdown until alivet_r reaches 800, and stands in neutral -
 * the automatic needs gear UP to move off, as the capture's driver did on
 * frame 809).
 *
 * WHAT IS THE ORIGINAL'S, AND WHAT IS NOT
 * --------------------------------------
 * The LOOP is the original's: every render
 * frame runs FUN_004436EF's accumulator - GetTickCount, a debt of more than
 * 1,000 ms thrown away, steps = elapsed / 10 (`MOV EBX,0xa` at 0x0044372F),
 * the remainder carried - then all due 10 ms steps, then ONE render.  No
 * sleep and no vsync wait in the loop; pausing resyncs the accumulator, as
 * doplaygame_q's pause path does ([0x5C6578] = GetTickCount()).
 *
 * The STEP is the port's whole per-car tick, sim_tick_run, set up by
 * simlevel.c exactly as trace_check sets it up - which is what lets the gate
 * compare the two bit for bit.  Inputs are latched INSIDE the step: the
 * device words for the frame being stepped TO, turned into the input stage's
 * outputs by the stage itself (until then the capture's outputs were
 * copied in).
 *
 * NOT the original's, and named where they are made: ~~the chase camera
 * (cam_standin.c)~~ (the original's camera now,
 * port/sim/camera.c and camview.c), the viewer's keys (viewer_keys.h), the
 * car's draw matrix (objscene_draw_placed), and RE-SEEDING from the capture,
 * which exists because a free-running port leaves the road within seconds
 * No frontend:
 * section 5 lets everything above the session lifecycle (FUN_0044231C) be
 * replaced wholesale.
 *
 * THE SEEDING MODES (--reseed N)
 *   N = 0    free run: seed once at the start frame, then the port's own car.
 *            The honest picture of the port's physics, and it does not stay on
 *            the road (167 to 1,283 steps to 1 m off).
 *   N = 1    every step starts from the game's record: trace_check's phase B.
 *   N = 50   the DEFAULT: within 0.38 m of the game over the whole stage.
 *   N = 100  the original's own replay cadence, one snapshot per 100 frames.
 * Step n re-seeds when (n - seed - 1) % N == 0, which is trace_check's
 * TRACE_CHAIN_RESEED arithmetic with the seed at the start frame.
 *
 * NO CD CHECK, AND LOCAL PATHS ONLY (Non-goals).  Every asset is
 * resolved by vfs.c against --assets; a missing one is an immediate error
 * naming the path. */

#include "plat.h"
#include "simlevel.h"
#include "stage.h"
#include "objscene.h"
#include "objlit.h"
#include "platecache.h"
#include "cdat.h"
#include "localdat.h"
#include "vfs.h"
#include "sound.h"
#include "carsound.h"
#include "worldsound.h"
#include "weatherfx.h"
#include "wipers.h"
#include "driver.h"
#include "wheels.h"
#include "spray.h"
#include "spray_draw.h"
#include "fireworks.h"
#include "fireworks_draw.h"
#include "sun.h"
#include "wav.h"
#include "camera.h"
#include "camview.h"
#include "lexicon.h"
#include "viewer_keys.h"
#include "car_input.h"
#include "car_keys.h"
#include "car_load.h"
#include "setup.h"
#include "hud.h"
#include "image.h"
#include "frontend.h"
#include "arcade.h"
#include "portcfg.h"
#include "carshadow.h"
#include "rplrec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* plat_gl.c, deliberately not in plat.h (see the comments there). */
int plat_gfx_read_frame(unsigned char *rgba, int w, int h);
int plat_gfx_cull_mode(int mode);
int plat_gfx_inset(int x, int y, int w, int h);    /* the mirror */
int plat_gfx_clear_depth(void);                    /* 0x0044CA08 */
int plat_gfx_pillar(int x, int w);                 /* 4:3 menus on a wider frame */
void plat_touch_context(int race);                 /* which on-screen controls */
typedef struct {
    uint32_t v[PLAT_RS__COUNT];
    int      valid[PLAT_RS__COUNT];
    plat_tex tex;
} plat_gfx_saved;
void plat_gfx_state_save(plat_gfx_saved *s);
void plat_gfx_state_restore(const plat_gfx_saved *s);

#define MAX_SHOTS 16
/* a car row's part words, +0x58..+0x98, indexed by row offset. */
#define CAR_ROW_WORDS 17
#define CAR_ROW(off)  (((off) - 0x58) / 4)

typedef struct {
    const char *assets, *image, *stage, *replay, *dump, *keys;
    long        reseed, start, steps;
    int         live;
    long        car;               /* --car, without a capture       */
    unsigned long seed;            /* --seed, the car's LFSR         */
    long        direction;         /* --direction, [0x006CE08C]      */
    long        gearbox;           /* --gearbox, car+0x38 autom_1; -1 = the
                                    * load's default, 1              */
    int         stage_given;       /* no --stage and no --replay: the menus  */
    int         has_setup;         /* --setup, or the menus'         */
    int32_t     su[6];             /* tyre bbal seth sets stes setg, menu units */
    long        gmode;             /* --gmode, [0x5D6750] for the load: 0
                                    * single (the auto-setup), 1 championship,
                                    * 2 arcade, 3 time trial         */
    int32_t     carparts;          /* player +0x278 -> car +0x298    */
    int32_t     champ88;           /* player +0x288 -> car +0x88, gmode 1    */
    uint32_t    cheats;            /* [0x70AA78], the menus' cheat word      */
    const sim_ghost_rec *ghosts;   /* the menus' [0x70CC38] records,
                                    * SIM_GHOSTS of them; NULL none          */
    const wear_block *wear;        /* the launcher's copy of the
                                    * record's wear; NULL none               */
    /* --arcade L,S[,P]: the arcade's level [0x70CF64] (0..5), stage
     * [0x70CF60] (1..6) and the last stage's position [0x70CF6C] (-1: none,
     * the field's last); the player's race number +0x28 -> [0x70CD88] ->
     * car +0xAA8; player 2's gearbox [0x70CF04] -> [0x5D6748] (slot 1's) */
    long        arc_level, arc_stage, arc_start;
    int32_t     racenum, gearbox2;
    int         no_ral_dat;        /* --no-ral-dat: the image's players only */
    const char *user_dir;          /* --user-dir: var\ and savedata\ */
    int         no_save;           /* --no-save: nothing written     */
    const char *record;            /* --record FILE: the race's replay */
    const char *view;              /* --view FILE: play a replay       */
    long        view_start;        /* --view-start N: begin at chunk N          */
    int         no_record;         /* --no-record: the menus' race writes none  */
    int         unlock_a8;         /* --unlock-a8: [0x70CC34] = 1    */
    int         hd_textures;       /* --[no-]hd-textures: -1 = rally.cfg's  */
    int         no_quit_confirm;   /* --no-quit-confirm: the original's quit */
    int         quit_confirm;      /* --quit-confirm: the guard in a script  */
    int         car_atlas;         /* --car-atlas: the cars' plates in one */
    long        finish;            /* --finish N: the finish scene for
                                    * [0x70D068] = N, 0..5; -1 none  */
    int         fireworks;         /* --fireworks: [0x70D074] = 1, the
                                    * championship won                       */
    int         has_result;        /* --menu-result: RESULTS shows this, not the race's */
    int32_t     result_cs, result_mph;
    const char *menu_keys;         /* --menu-keys: a script for the menus    */
    const char *menu_shot;         /* --menu-shot: write the menu, then stop */
    const char *vm_census;         /* --vm-census LIST: what could reach the interpreter */
    const char *sound;             /* --sound: a .wav played once at start */
    const char *music;             /* --music: a .wav streamed, looping */
    const char *sound_log;         /* --sound-log: every request heard */
    int         nomusic, nosound;  /* the config's `nomusic` / `nosound` */
    int         in_game_music;     /* --in-game-music: AUDIO SETUP's switch ON
                                    * over the screen's (ported it)    */
    int         joy_dz;            /* --joy-dz given: it, not SETUP CONTROLLER's
                                      lower / upper dz */
    int         codriver;          /* --codriver: PLAYER DETAILS' co-driver
                                    * over the record's, -1 the record's
                                    * (the screen ported)      */
    long        menu_ms;           /* --menu-ms: ms since the last key, for
                                    * the shot (the highlight's phase)        */
    long        menu_step;         /* --menu-step: the %d sweep's stride
                                    * (phase i at --menu-ms + i * step); the
                                    * models' yaw turns once in 8,192 ms      */
    long        menu_cost;         /* --menu-cost: N frames 16 ms apart
                                    * on the script's last screen, timed     */
    int         w, h, window, radius;
    int         frame_ms;          /* 0 = the real clock                     */
    int         jitter;            /* the gate's irregular fake clock        */
    int         exit_at_end;
    int         fullscreen;        /* -1 the default (fullscreen for
                                    * the menus, a player's launch), 0
                                    * --windowed, 1 --fullscreen            */
    int         no_vsync;          /* --no-vsync, present unpaced    */
    int         assets_given;      /* --assets or $RALLY_ASSETS             */
    int         nshots;
    long        shot_frame[MAX_SHOTS];
    const char *shot_path[MAX_SHOTS];
} options;

/* ---- the whole program's state ----------------------------------------- */

static sim_level   L;              /* large: static, not on the stack        */
static sim_capture cap;
static sim_replay  rep;
static stage       stg;
static objscene    objs;
static plate_cache pc;
static stage_fog   fog;

static struct {
    const car_params *p;
    int32_t     carnum;
    car_state   car;               /* the port's car, after step `frame`     */
    long        frame;             /* the capture frame the car stands at    */
    long        seed;              /* where the current run was seeded       */
    long        reseed;
    int         at_end;
    long        steps_run, step_errors, reseeds;
    FILE       *dump;
    /* the original's camera (port/sim/camera.h, camview.h), which
     * replaced the stand-in.  The step half runs inside the sim's step
     * (rep.camera); the view survives a restart, as a player's choice. */
    sim_camera_consts camk;
    sim_camera_ctx    camx;
    int32_t     cam_view;          /* the view a restart keeps              */
    /* each car's part words, row +0x58..+0x98 of 0x0057E10C +
     * id*0x12C (car_parts), and LOCALDAT +0x6A0 bit 1 ([0x005E0960]). */
    int32_t     car_row[SIM_CAMERA_CARS][CAR_ROW_WORDS];
    wheel_geom  car_wheel[SIM_CAMERA_CARS];    /* the same rows   */
    int32_t     model_drop;                    /* [0x005C6CF8]            */
    int         car_lit;
    /* The part-state table's inputs out of the image (car_state_table):
     * 0x006CE094 + id*0x30, the tacho needle's plate per 250 revs, and
     * 0x006CE4EC, slot 8's plate per ten tracks; and LOCALDAT +0x69C bit 0
     * ([0x005E095C], 0x00442B02). */
    unsigned char car_tacho[SIM_CAMERA_CARS][0x30];
    int32_t     car_slot8[7];
    int         car_lamp69c;
    int         wet_level;     /* LOCALDAT +0x690 | +0x694 != 0       */
    /* Light mode 3's per-stage inputs (0x0045F6DD..0x0045F716, LOCALDAT):
     * the sun's azimuth +0x684 and elevation +0x688, the diffuse +0x6A8, the
     * lit colour +0x60C..614 and the shadow colour +0x600..608.  `light`
     * is filled per frame (it is in view space). */
    objscene_light car_light;
    plat_tex    car_env_tex;           /* texture id 0x87's plate (2090)       */
    int32_t     sun_az, sun_el;
    int         have_car_light;
    lexicon     lex;               /* the game's text, for the HUD's banner */
    int         have_lex;
    int         ghost;
    int         car_nocull;        /* RALLY_CAR_NOCULL, a diagnostic        */
    /* --live */
    int         live;
    /* No capture: the car is started the game's way, on the
     * stage's own start line - place.c - from the game's load (car_load.h,
     */
    int         have_cap;
    long        gearbox;           /* -1, or the chosen autom_1      */
    int         have_setup;        /* the player's sliders below     */
    setup_sliders su;              /*   player +0x10C..+0x120, menu units    */
    car_load_tables clt;           /* the load's tables, the image's  */
    int32_t     load_gmode;        /* [0x5D6750] as the load sees it         */
    int32_t     track;             /* [0x5D671C] shauntracknum, LEVELnn's nn */
    int32_t     carparts;          /* [0x70CF80] <- player +0x278            */
    int32_t     champ88;           /* [0x70CF70] <- player +0x288 (gmode 1)  */
    /* [0x5D6770]'s records, the menus' (have_ghosts 0: none - the
     * race keeps FUN_0043560F's defaults) */
    int         have_ghosts;
    int         have_pilot;        /* RALLY_AUTOPILOT outside the arcade:
                                    * AK read for the diagnostic driver     */
    sim_ghost_rec ghosts[SIM_GHOSTS];
    /* the wear handed in (wear.h), kept for the hand-back -
     * its six pass-through dwords go back as they came */
    int         have_wear;
    wear_block  wear;
    uint32_t    cheats;            /* [0x70AA78]: bit 16 stops out of time   */
    int         from_menu;         /* ESC and the race's end go back */
    /* doplaygame_q's two modal waits, PAUSED and RETIRE? */
    int         modal;             /* MODAL_*                                */
    int32_t     pause_cool;        /* [0x5D64B8]: 25 at PAUSED, -1 a step     */
    int32_t     pause_bc;          /* [0x5D64BC]: frames before a key counts  */
    int32_t     retire_all;        /* [0x5D6774]: RETIRE? (Y/N/A), 194        */
    int         no_flare;          /* [0x5D675C] == 0, VISUAL OPTIONS  */
    /* VISUAL OPTIONS' other four, [0x5D6760..0x5D676C], from the menus
     * only (a command-line race keeps the port's own: reflections on, every
     * rival's lamps lit, every car whole).  vis_set = 1 when they are valid. */
    int         vis_set;
    int         vis_reflect;       /* [0x5D6760] 0 = no reflection pass        */
    int         vis_shadow;        /* [0x5D6764]: the shadow switch; carshadow.c draws it */
    int         vis_detail;        /* [0x5D6768] 0..2: whose parts are reduced */
    int         vis_headlights;    /* [0x5D676C] 1 = every car's lamps         */
    int         has_hud_mask;      /* car+0x220 from the menus (VISUAL */
    uint32_t    hud_mask;          /*   PREFERENCES), not the default's 0xF7  */
    int         has_menu_view;     /* the race starts in PLAYER 1     */
    int32_t     menu_view;         /*   DETAILS' camera, player+0x0C          */
    int32_t     gmode;             /* [0x5D6750]                             */
    unsigned char fold_down[0x80]; /* the held state, for the just-pressed   */
    uint32_t    seed_lfsr;         /* +0x00C/+0x010 (sim_reset_timers_q)    */
    int         have_script;
    car_script  script;
    car_slots   held;              /* the slots down on the last step        */
    long        live_steps;        /* steps since the live run started      */
    /* what a live run measures (guardrail 4: instability is a finding) */
    long        nan_step, offroad_steps, d4b3_due, d4b3_ran;
    double      max_speed, dist;
    /* a non-finite car value STOPS the run (the port's guard, not
     * the original's: it never drives the set pieces where this happens,
     * Stepping on would index tables
     * with FISTP's 0x80000000 and crash the process. */
    int         halted;
    /* the finish scene (rally --finish).  [0x005FE9D8], the no-route
     * mode: the placement takes route index 0 and no 2 m back, and the frame
     * leaves out the HUD (0x004523BF, 0x0045269A) and the mirror
     * (0x00452715). */
    int         finish;
    int         fw_finish;        /* the finish with the fireworks
                                   * ([0x0070D074] = 1)                      */
    /* the race.  race_over is FUN_004436C1's carry: a live run's
     * batch stops on it.  log_* are the race words as last printed. */
    int         race_over;
    int32_t     log_seg, log_rfn, log_gof;
    hud         hud;               /* hud.h                           */
    int         have_hud;
    int         no_hud;            /* RALLY_NO_HUD, a diagnostic             */
    int         patch_cull;        /* the fix, on unless RALLY_FAITHFUL_CULL */
    int         day_ramp;          /* RALLY_DAY_RAMP: the identity shade ramp */
    int32_t     test_msg, test_msg_at; /* RALLY_MESSAGE=ID:STEP, a diagnostic
                                    * -1 = none / already sent      */
    int         obj_from_stage;    /* RALLY_OBJECTS_FROM_STAGE, a diagnostic */
    /* the windows' crack words, car+0x700..0x714, per drawn car (the
     * own car and the ghost), recomputed only when caroctdamage moves - the
     * crush pass's +0x2A8 latch (FUN_004C052A).  `valid` 0 forces it. */
    struct car_glass {
        int      valid, ok;
        float    oct[8];
        uint32_t w[6];
        ro3_vec *pts;                  /* the dented mesh, npts vertices  */
        size_t   npts;
    } glass_own, glass_ghost, glass_rival[SIM_REPLAY_RIVALS];
    /* the arcade's rivals, slots 1..3 - what the launch handed over
     * and each slot's car as setuparcadecars loaded it */
    int         have_arcade;       /* gmode 2 with no capture: rivals race  */
    FILE       *arc_log;           /* RALLY_ARCADE_LOG, a diagnostic        */
    long        ap_from[4], ap_to[4]; /* RALLY_AUTOPILOT=FROM:TO[,FROM:TO..],
                                    * a diagnostic: the rival driver drives
                                    * car 0 on those live steps
                                    * (sim_arcade_autopilot_q), up to four */
    int         ap_n;
    int32_t     arc_level, arc_stage, arc_start, racenum, gearbox2;
    struct {
        const car_params    *p;
        sim_collision_points pts;
        int32_t              model;     /* the collision model             */
    } rv[SIM_REPLAY_RIVALS];
    /* rendering */
    objscene_backdrop bg;
    ral_obj_camera    ocam;
    objscene_stats    ost;
    int         radius;
    int         w, h, video;
} G;

/* the replay recorder (rplrec.c, the original's write path).  Outside
 * G, which each race clears: the file belongs to the process, and the menus
 * read whether one exists when the race is over. */
static rplrec  rrec;
static char    rec_path[1100];     /* "" = this race records nothing        */
static long    rec_dir;            /* the course direction the race runs (the header's port word) */
static int     rec_made;           /* a recording was finished this process */

/* the replay VIEWER.  The race loop with the car driven by a recording:
 * at the first step of every chunk the snapshot is put back (replay_unpack_chunk,
 * 0x0045DC62, over the car's replayed 1,668 bytes - its first eight are the chunk
 * head's, which the port's car does not use) and every step takes its device
 * words from the chunk's records (replay_frame_read_q, 0x0045DCB7).  Everything
 * else - the camera, the object hits, the ground hints - runs on as it does in a
 * drive, which is the original's own limit. */
static rplrd   vrd;
static int     view_on;
static uint32_t vw[4];             /* the step's four words: ax, bx, cx, dx  */

/* RALLY_WEAR's once-a-process latch (outside G, which each race clears) */
static int wear_diag_done;

/* THE SLOT'S WARNINGS, kept from one race to the next.  The original's
 * car slot is not cleared between races - its load zeroes only up to +0x2B4
 * (FUN_00434FC3, s4) - so the 35 warning states and the
 * queue (+0x368..+0x5FB) a race ends with are there at the next one's
 * settle, whose steps run FUN_004C773F, before raceinit's rebuild
 * (sim_damage_warn_rebuild_q).  The port's car_load clears the whole car,
 * so they are carried here; a new process starts them at 0, as the
 * original's.  Run 34: a worn stage starts quiet (s5). */
enum { SLOT_WARN_AT = 0x368, SLOT_WARN_END = 0x5FC };
static unsigned char slot_warn[SLOT_WARN_END - SLOT_WARN_AT];

static void die_missing(const char *what, const char *path)
{
    fprintf(stderr, "rally: missing asset (%s): %s\n", what, path);
    exit(1);
}

/* ---- the replay ---------------------------------------------------------- */

/* Start a run at `frame`: the car is the game's record there, and everything
 * the record does not carry starts fresh - the object state and pols as the
 * level loaded them, the ground hints, rev_q.  At the start of the program
 * that is exactly trace_check's chained pass from the same seed.  After a
 * SEEK it is NOT what the game had there (the hidden state is not warmed up
 * to the frame), and the viewer says so. */
/* a fresh camera in the view the player last chose, given to the
 * sim's step.  sim_replay_init clears rep.camera, so every (re)start calls
 * this after it. */
static void camera_attach(void)
{
    sim_camera_init(&G.camx, &G.camk, G.cam_view);
    G.camx.route = L.have_route ? &L.route : NULL;
    G.camx.numpl = 1;
    G.camx.layout = 0;                 /* [0x005D6758]: one screen          */
    rep.camera = &G.camx;
}

/* DIAGNOSTICS, not the original's: RALLY_CAMERA_VIEW=N starts in
 * view N, RALLY_MIRROR=1 turns the mirror on at each start - so a scripted
 * run can shoot every view without a key. */
static void camera_diagnostics(void)
{
    const char *e = getenv("RALLY_MIRROR");
    if (e && *e == '1') {
        unsigned char *raw = (unsigned char *)&G.car + 0x220;
        uint32_t mask;
        memcpy(&mask, raw, 4);
        mask |= 0x08u;
        memcpy(raw, &mask, 4);
    }
}

/* the game's sound requests (sound.h).  Output only: nothing here
 * reaches the simulation (the replay gates must not move). */
static snd S;

/* the arcade's tables and process state (port/sim/arcade.h).  ONE
 * for the process, as the game's globals are: the rival driver's reverse
 * count is never cleared.  `arc_rnd` is rnd_next's [0x004DB13C] for
 * FUN_00447C3D's last two draws - the game's has run since boot from a
 * clock-derived seed; the port's starts at --seed (a reading: which rivals
 * stage 1 brings is the clock's in the original). */
static sim_arcade_consts AK;
static sim_arcade        A;
static uint32_t          arc_rnd;

/* ----: the co-driver ------------------------------------------------ */

static sim_pacenotes notes;        /* init_pacenotes_q's, built at load      */
static int           have_notes;
static uint32_t      snd_ms;       /* the race loop's clock, ms              */

/* The speech tables out of the image: each token's sample ids (0x0050C87C,
 * -1-terminated lists of up to six, 113 tokens to the first list at
 * 0x0050CA40) and the two lists of file names 0x0040ECFB chooses between -
 * male at 0x00502BD6, female at 0x005038D0, 95 strings each laid end to end
 * (0x0040227F's length, then the next). */
static void codriver_tables(const ral_image *im)
{
    static const uint32_t list[2] = { 0x00502BD6u, 0x005038D0u };
    int t, k, x;
    for (t = 0; t < SND_CD_TOKENS; t++) {
        const unsigned char *p = ral_image_at(im, 0x0050C87Cu + 4u * (uint32_t)t, 4);
        uint32_t at = p ? (uint32_t)ral_image_i32(p) : 0u;
        for (k = 0; k < 7; k++) S.tok[t][k] = -1;
        for (k = 0; k < 6 && at; k++) {
            const unsigned char *v = ral_image_at(im, at + 4u * (uint32_t)k, 4);
            S.tok[t][k] = v ? ral_image_i32(v) : -1;
            if (S.tok[t][k] == -1) break;
        }
    }
    for (x = 0; x < 2; x++) {
        uint32_t at = list[x];
        for (k = 0; k < SND_CD_PAIRS; k++) {
            size_t n = 0;
            const unsigned char *c;
            while ((c = ral_image_at(im, at + (uint32_t)n, 1)) && *c
                   && n + 1 < sizeof S.cd_file[x][k]) {
                S.cd_file[x][k][n] = (char)*c;
                n++;
            }
            S.cd_file[x][k][n] = '\0';
            while ((c = ral_image_at(im, at + (uint32_t)n, 1)) && *c) n++;
            at += (uint32_t)n + 1u;
        }
    }
}

/* After every sim_replay_init: the race speaks the level's sentences.  The
 * speech set itself is set up once per race, before the race's music, as
 * 0x0040EE51 does (race()); a restart or a seek empties the queues again
 * (0x0040D213, 0x0040D423 for one player). */
static int cd_restart;

/* ----: the car's sounds ------------------------------------------- */

static snd_car_tables CT;
static int            have_ct;
static snd_car        C;
/* the weather and the ambience (worldsound.h), through C's player */
static snd_world      W;
static int            have_world;

/* ----: what the render path draws for rain and snow (weatherfx.h) */
static wfx               WX;
static int               have_wfx;
static sim_weather_words wx_now;     /* car 0 +0x900.., the last step's      */
static uint32_t          wfx_clock;  /* [0x005C6568] for this frame          */
static int               wfx_advance; /* block +0x10: 0 while paused         */
static int               wfx_no_falling;      /* a diagnostic          */
/* the wipers re-pose the car model's blade vertices
 * (wipers.h); RALLY_WIPER_DUMP=<file> writes each update's phase and
 * blade vertices, the gate's view */
static wipers            WP;
static wiper_masks       WM;                 /* the wipe-off */
/* the steering wheel and the hands (driver.h), posed in the car
 * model's own vertices like the blades */
static driver            DP;
/* The wheel and hands take the weather's clock and generator (WX.dt,
 * WX.lcg) when the weather is loaded.  Without it (RALLY_NO_WEATHER, or its
 * picture failing to load) they run on these, seeded as wfx_race_start
 * seeds the weather's, so the cockpit moves whether or not rain is drawn. */
static uint32_t          drv_gen;
static uint32_t          drv_last_ms;
static int32_t           drv_dt = -1;
static FILE             *wiper_dump;
/* the wheels' spray (spray.h), its snapshot fed from the player's
 * last step world (the surface solve, the body contact) and the rivals' */
static spray             SP;
static int               have_spray;
/* the level's sun (sun.h) */
static sun               SUN;
static sim_world         SW0;
static uint32_t          spray_last_ms;
/* the championship finish's fireworks (fireworks.h), run
 * after the spray in the frame (0x004C1224); on from 32 s into the finish
 * ([0x005787E4], set by 0x004572B6), their two sound flags the audio
 * thread's (worldsound.h fw) */
static fireworks         FW;
static int               have_fw;
static int               fw_on;
static uint32_t          fw_last_ms;

static void world_sound_tick(void *ud, uint32_t clock, float tps)
{
    snd_world_tick((snd_world *)ud, clock, tps);
}


static int have_arcade_snd;         /* the rivals' records are on */

/* The 20 ms gate's call (sound.h race_tick): 0x0040D63C for record 0. */
static void car_sound_tick(void *ud, uint32_t clock, float tps)
{
    (void)ud;
    /* 0x0040D5EB reads each car's +0x25C every tick */
    if (C.ai_on && have_arcade_snd) {
        int k;
        for (k = 0; k < SIM_REPLAY_RIVALS; k++)
            C.ai_driver[k] = rep.rival[k].car.driver;
    }
    snd_car_tick(&C, clock, tps);
}

/* 0x0040D213 / 0x0040D423 for the car's record, fed by the race's record 0
 * (car 0 is player 1's).  The car is the launcher's [0x0070CC50]. */
static int car_sound_race(void)
{
    if (!have_ct || !rep.have_race) return 0;
    if (C.feed != &rep.race.snd[0] || C.rec.car != G.carnum) {
        FILE *log = C.log;
        snd_car_init(&C, &CT, &S, G.carnum, &rep.race.snd[0]);
        C.log = log;
    }
    /* the arcade ([0x0050F300] == 1) opens records 1..3 for the
     * rivals' cars, each fed by the race's snd[slot] */
    have_arcade_snd = G.have_arcade && rep.race.arcade != NULL;
    if (have_arcade_snd) {
        sim_sound_feed *fd[SND_CAR_AI];
        int k;
        for (k = 0; k < SND_CAR_AI; k++) fd[k] = &rep.race.snd[k + 1];
        snd_car_arcade(&C, fd);
    } else {
        C.ai_on = 0;
    }
    snd_car_race(&C);
    S.race_tick = car_sound_tick;
    S.race_ud = &C;
    /* 0x0040D213's world part.  The ambience's generator is a
     * STAND-IN (worldsound.h): seeded from the objects' at the start. */
    if (have_world) {
        snd_world_race(&W, &rep.race.weather, L.ostate.rnd);
        W.chime = rep.race.chime;                   
        S.world_tick = world_sound_tick;
        S.world_ud = &W;
    }
    return 1;
}

/* After every sim_replay_init (the race record is new): the first one arms
 * the car's sounds - the race does not exist before it - and every later
 * one is a restart or a seek, 0x0040D213 again. */
static void codriver_attach(void)
{
    if (rep.have_race && have_notes) rep.race.notes = &notes;
    /* 0x004C0F44, the engine's load - the weather's generator seeded
     * with the car's model, its tables and pools (weatherfx.h) */
    if (have_wfx) {
        wfx_race_start(&WX, G.car.carnum1);
    } else {
        unsigned long i;
        int m = (int)G.car.carnum1;
        if (m == 6) m = 11;
        if (m < 0) m = 0;
        if (m > WFX_CARS - 1) m = WFX_CARS - 1;
        drv_gen = (uint32_t)m;
        for (i = 0; i < WM.draws; i++)
            drv_gen = drv_gen * 0x9Du + 0x47u;
        drv_last_ms = 0;
        drv_dt = -1;
    }
    if (have_spray) spray_race_start(&SP);   /* 0x004BAE77          */
    if (cd_restart) {
        snd_cd_race(&S, 1, snd_ms);
        car_sound_race();
    } else if (car_sound_race() && !S.nosound) {
        printf("rally: the car's sounds armed (car %d)\n", (int)G.carnum);
    }
    cd_restart = 1;
}

/* The step's requests to the speech queue, in the order it made them.
 * 0x0045AD2B / 0x0045AD14 / 0x0045ACFD each look up [0x005F4540 + car*4],
 * the car's local player or -1: car 0 is player 1's in a one-player race. */
static void codriver_drain(uint32_t now_ms)
{
    int32_t k;
    if (!rep.have_race) return;
    S.now = now_ms;
    for (k = 0; k < rep.race.nspeech; k++) {
        const sim_speech *q = &rep.race.speech[k];
        int32_t player = q->slot == 0 ? 0 : -1;
        if (player < 0) continue;
        if (q->kind == SIM_SPEECH_SAMPLE) snd_cd_sample(&S, player, q->value);
        else snd_cd_push(&S, player, q->value, q->kind == SIM_SPEECH_FRONT);
    }
    rep.race.nspeech = 0;
}

/* ----: the recording ----------------------------------------------
 *
 * 0x0044231C's start (fmt_replay_start_record, after 0x0045DD9B filled the
 * replayheader) and 0x0041DA74's stop, around the race; 0x0045DB0B's chunk
 * arithmetic inside it (live_step).  What is NOT the original's, named: the
 * detached-parts table (960 bytes, 0x006CE5A4) is zeroes - the port does not
 * model the flying pieces - and slots 1..3 are zeroes outside the
 * arcade, where the original holds the three unused cars' seeded records. */
static void rec_put(uint8_t *h, unsigned off, int32_t v)
{
    h[off] = (uint8_t)v; h[off + 1] = (uint8_t)(v >> 8);
    h[off + 2] = (uint8_t)(v >> 16); h[off + 3] = (uint8_t)(v >> 24);
}

static void rec_stop(void)
{
    if (!rrec.open) return;
    if (rplrec_end(&rrec) == 0) {
        rec_made = 1;
        printf("rally: replay recorded: %s (%u chunks, %d steps)\n", rec_path,
               (unsigned)rrec.nchunks, (int)rrec.frames);
    } else
        fprintf(stderr, "rally: the replay %s could not be finished\n", rec_path);
}

static void rec_start(void)
{
    uint8_t hdr[RPL_HDR_SIZE];
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    const car_state *rv[3];
    int n = G.load_gmode == 2 && rep.nrivals > 0 ? 1 + rep.nrivals : 1, k;

    rec_stop();
    rec_made = 0;
    if (!rec_path[0] || G.have_cap || !G.live) return;
    memset(hdr, 0, sizeof hdr);
    rec_put(hdr, RPLH_TRACK, G.track);                    /* [0x5D671C]  */
    rec_put(hdr, RPLH_GMODE, G.load_gmode);               /* [0x5D6750]  */
    rec_put(hdr, RPLH_NUMPL, 1);                          /* [0x5F0A54]  */
    rec_put(hdr, RPLH_NUMCARS, n);                        /* [0x5F0A50]  */
    rec_put(hdr, RPLH_CAR1, G.car.carnum1);               /* car[k]+0x24 */
    for (k = 0; k < 3 && k < rep.nrivals; k++) {
        rv[k] = &rep.rival[k].car;
        rec_put(hdr, RPLH_CAR1 + 4u * (unsigned)(k + 1), rv[k]->carnum1);
    }
    rec_put(hdr, RPLH_MODE20, 1);                         /* [0x50F2EC]  */
    rec_put(hdr, RPLH_PORT_DIR, (int32_t)rec_dir);        /* the port's word, */
    rec_put(hdr, RPLH_GAMEOVER1, G.car.gof);              /* car[0]+0xB0 */
    if (rep.nrivals > 0) rec_put(hdr, RPLH_GAMEOVER2, rep.rival[0].car.gof);
    if (rplrec_begin(&rrec, rec_path, hdr,
                     lt ? rplrec_stamp(lt->tm_year + 1900, lt->tm_mon + 1,
                                       lt->tm_mday, lt->tm_hour, lt->tm_min)
                        : 0) != 0) {
        fprintf(stderr, "rally: cannot write the replay %s\n", rec_path);
        return;
    }
    printf("rally: recording the race to %s (track %d, gmode %d, %d car%s)\n",
           rec_path, (int)G.track, (int)G.load_gmode, n, n == 1 ? "" : "s");
    if (G.car.alivet % 100 != 0)
        printf("rally: WARNING - the replay starts at step %d, not on a chunk "
               "boundary\n", (int)G.car.alivet);
}

static void seek(long frame)
{
    if (!G.have_cap) {
        /* raceinit_q for car 0 - the game's load, then its
         * placement, leveller, resettimers and fifty-step settle (place.c).
         * One player. */
        sim_start_mode m;
        car_load_in in;
        int rc;
        memset(&m, 0, sizeof(m));
        m.numpl = 1;
        m.no_route_q = G.finish;                   /* [0x005FE9D8],    */
        G.frame = G.seed = 0;
        car_load_in_default(&in);
        in.gmode = G.load_gmode;
        in.track = G.track;
        in.carnum = G.carnum;
        /* the gearbox mode the menus (or --gearbox) chose:
         * player+0x30 -> [0x70CD98] -> [0x5D6744] -> car+0x38 (0x00435166) */
        if (G.gearbox >= 0) in.gearbox = (int32_t)G.gearbox;
        /* the HUD mask graham_to_shaun builds (0x00445479..0x00445510)
         * from VISUAL PREFERENCES' flags */
        if (G.has_hud_mask) in.hud_mask = G.hud_mask;
        /* the player's sliders - which reach the car only in gmodes 1
         * and 3: a single stage and the arcade take the auto-setup's */
        if (G.have_setup) in.sliders = G.su;
        in.carparts = G.carparts;
        in.champ88 = G.champ88;
        /* initcheckpoints_q runs at level load, before gamemodestuff_init:
         * the race first, for numcheckposits (FUN_00436222's count) */
        sim_level_reset_objects(&L);
        sim_replay_init(&rep, &L, G.p);
        in.numcheckposits = rep.have_race ? rep.race.numcheckposits : 0;
        car_load(&G.car, &in, &G.clt, G.p, L.pts.n);
        /* graham_to_shaun's wear (0x004455B1..), which no
         * part of the load clears (FUN_00434FC3's zeroes stop at +0x2B4;
         */
        if (G.have_wear) wear_into_car(&G.car, &G.wear);
        if (getenv("RALLY_WEAR") && !wear_diag_done) {
            /* A DIAGNOSTIC, not the original's: every damage field at N
             * percent from the start of the process's FIRST race - a
             * shortcut to wear the race itself makes slowly (damage_acc.c,
             * for the gate: a later race has it only through the
             * record, wear.c.  "N:a,b,..." sets only the damage slots
             * dam[a], dam[b].. (the gate: the wheels 16..19 and tyres
             * 39..42 at 100 without the engine's 100 retiring the car) */
            const char *ws = getenv("RALLY_WEAR"), *list = strchr(ws, ':');
            float wv = (float)atof(ws);
            int k;
            if (list) {
                for (list++; *list; ) {
                    char *e;
                    long a = strtol(list, &e, 10);
                    if (e == list) break;
                    if (a >= 0 && a < 44) G.car.dam[a] = wv;
                    list = *e == ',' ? e + 1 : e;
                }
            } else {
                for (k = 0; k < 44; k++) G.car.dam[k] = wv;
            }
            wear_diag_done = 1;
            printf("rally: RALLY_WEAR - %s damage field%s start%s at %g%% (a "
                   "DIAGNOSTIC, this first race only)\n",
                   strchr(ws, ':') ? "the listed" : "every",
                   strchr(ws, ':') ? "s" : "", strchr(ws, ':') ? "" : "s",
                   (double)wv);
        }
        if (getenv("RALLY_DENT")) {
            /* A DIAGNOSTIC, not the original's: the body's dents without a
             * crash - "V" sets all eight octants of caroctdamage to V (0..1),
             * "V:a,b.." only octants a, b..  The crush pass then moves the
             * car's vertices (parts.points), which is the one draw
             * path a clean car never takes: the frame-hash gate. */
            const char *ds = getenv("RALLY_DENT"), *dl = strchr(ds, ':');
            float dv = (float)atof(ds);
            int k, listed = dl != NULL;
            if (dl) {
                for (dl++; *dl; ) {
                    char *e;
                    long a = strtol(dl, &e, 10);
                    if (e == dl) break;
                    if (a >= 0 && a < 8) G.car.caroctdamage[a] = dv;
                    dl = *e == ',' ? e + 1 : e;
                }
            } else {
                for (k = 0; k < 8; k++) G.car.caroctdamage[k] = dv;
            }
            printf("rally: RALLY_DENT - %s octants at %g (a DIAGNOSTIC)\n",
                   listed ? "the listed" : "every", (double)dv);
        }
        /* the arcade's four cars.  gamemode_q's arcade arm: the
         * rivals' records (FUN_00434FC3 .. FUN_0043595A), graham_to_shaun's
         * start position (0x00445A91: arcadestartposition and slot 0's
         * arcadepo from [0x70CF6C]), then arcade_game_mode_init's field. */
        if (G.have_arcade && rep.have_race) {
            int s;
            for (s = 1; s <= SIM_REPLAY_RIVALS; s++) {
                sim_replay_add_rival(&rep, s, G.rv[s - 1].p, &G.rv[s - 1].pts);
                car_load_rival(&rep.rival[s - 1].car, s, A.types[s - 1],
                               G.gearbox2, &in, &G.clt, G.rv[s - 1].p,
                               G.rv[s - 1].pts.n);
            }
            /* no position handed over (--arcade L,S): the field's last, as
             * stage 1 starts - a port default; the original would read
             * [0x70CF6C] as whatever the last race left */
            A.start = G.arc_start >= 0 ? G.arc_start
                    : AK.numarccars[G.arc_level < 0 || G.arc_level > 5 ? 0 : G.arc_level];
            G.car.arcadeposition = A.start;
            rep.race.arcade = &A;
            if (sim_arcade_init_q(&A, &AK, G.arc_level, G.arc_stage - 1, 1,
                                  G.racenum, 0u, &G.car) != 0) {
                fprintf(stderr, "rally: the arcade's field will not start "
                                "(level %d stage %d)\n", (int)G.arc_level,
                        (int)G.arc_stage);
                exit(1);
            }
        }
        /* 0x00442ACA, at the level's load - every car's lights' latch
         * car+0x84 = LOCALDAT +0x6A0 bit 0, so a dark stage starts with the
         * lights on (runs 28 and 29: 1 from frame 0; the dry captures 0) */
        G.car.latch_84_q = stg.have_ld ? (int32_t)(ld_dw(&stg.ld, 0x6A0) & 1u) : 0;
        /* the eight stores at 0x00442ACA are every record's
         * ([0x006C2194] .. [0x006C90B4], stride 0xFE0), the rivals' too.
         * After car_load_rival and sim_arcade_init_q above; nothing else
         * but the lights key (human slots only) writes it, and the arcade's
         * spawn / despawn (0x00436065 / 0x00436135) leave it alone. */
        {
            int s;
            for (s = 0; s < rep.nrivals; s++)
                rep.rival[s].car.latch_84_q = G.car.latch_84_q;
        }
        /* the arcade's clock.  The race runs gmode 2's rules for an
         * arcade stage, and gmode 1's for a championship stage - the
         * gearbox in the countdown, so the false start, the neutral counter,
         * one car a marker.  3 still races as 0 (printed at the launch). */
        m.gmode = (G.load_gmode == 1 || G.load_gmode == 2) ? G.load_gmode : 0;
        printf("rally: the race's rules: gmode %d\n", (int)m.gmode);
        if (rep.have_race) {
            rep.race.allowance = car_load_allowance();
            rep.race.cheat_word = G.cheats;
            rep.race.dontrunoutoftime = 0;         /* [0x005B9228]: the */
            /* gmode 1's ninety rivals, graham_to_shaun's [0x5D6770] */
            rep.race.ghost_recs = G.have_ghosts ? G.ghosts : NULL;
            if (m.gmode == 1)
                printf("rally: gmode 1's 90 rivals: %s (the first: name %d, "
                       "car %d, %d cs)\n",
                       G.have_ghosts ? "the menus' records"
                                     : "FUN_0043560F's defaults",
                       (int)(G.have_ghosts ? G.ghosts[0].name : 0x5A5),
                       (int)(G.have_ghosts ? G.ghosts[0].type : 0),
                       (int)(G.have_ghosts ? G.ghosts[0].total : 0x2EE0));
            /* the DIAGNOSTIC autopilot outside the arcade (RALLY_AUTOPILOT):
             * the rival driver's tables, no arcade rule attached */
            if (G.have_pilot) {
                memset(&A, 0, sizeof A);
                A.k = &AK;
                rep.race.pilot_q = &A;
            }
        }
        memcpy((unsigned char *)&G.car + SLOT_WARN_AT, slot_warn, sizeof slot_warn);
        codriver_attach();                         
        /* The start's FUN_0043E00B gives the camera the car's angles, and
         * the settle steps it (it runs in FUN_0045E3FC's car step too). */
        camera_attach();
        rc = sim_replay_start(&rep, &G.car, &m, 0, G.seed_lfsr,
                              SIM_SETTLE_STEPS);
        if (rc != 0) {
            fprintf(stderr, "rally: the car cannot be put on the start line "
                            "(%d) - does the stage have a route?\n", rc);
            exit(1);
        }
        /* after the settle and raceinit's rebuild - a carried wear
         * starts quiet (champ-race reads it) */
        {
            int i, up = 0;
            for (i = 0; i < 35; i++) up += G.car.warn_state[i] != 0;
            printf("rally: the start's warnings: %d states up, %d queued, banner %d\n",
                   up, (int)G.car.warn_count, (int)rep.race.msg[0].topre);
        }
    } else {
    if (frame < 0) frame = 0;
    if (frame > cap.frames - 1) frame = cap.frames - 1;
    G.frame = frame;
    G.seed = frame;
    G.car = *sim_capture_car(&cap, frame);
    G.at_end = frame >= cap.frames - 1;
    sim_level_reset_objects(&L);
    sim_replay_init(&rep, &L, G.p);
    codriver_attach();                             
    /* The capture holds the record's prefix only, not the camera: start it
     * at the car and let the first step SNAP it - [0x006A24F0], what the
     * original's replay seek sets (FUN_0044DEE4 from 0x0045E241). */
    camera_attach();
    G.camx.snap = 1;
    /* Frame 0 of a capture is the race's start: messagesystem_init's
     * message 202.  A seek elsewhere starts the slots empty. */
    if (frame == 0 && rep.have_race) sim_message_system_init_q(&rep.race);
    }
    /* initoverlay_q: the progress bar's sections. */
    if (G.have_hud) hud_race_start(&G.hud, rep.have_race ? &rep.race : NULL);
    camera_diagnostics();
    G.race_over = 0;
    G.log_seg = G.car.currsegment;
    G.log_rfn = G.car.rfn;
    G.log_gof = G.car.gof;
    if (G.live) {
        G.at_end = 0;
        G.live_steps = 0;
        G.held = 0;
        /* The run's measurements start again with it. */
        G.nan_step = -1;
        G.halted = 0;
        G.offroad_steps = G.d4b3_due = G.d4b3_ran = 0;
        G.max_speed = G.dist = 0.0;
        rec_start();                         /* the race (again) */
    }
}

/* the race's events, in trace_check's own words ("race: ..."), so
 * replay_check.sh can compare this run's crossings and finish with the
 * capture's.  Then FUN_004436C1, for a live run. */
static void race_events(void)
{
    const car_state *c = &G.car;
    /* each object hit the step recorded - the instance obj_hook now
     * draws from the sim's own record, so the frame is where to look. */
    static long hits_seen;
    if (L.have_objects && L.ostate.recorded_hits != hits_seen) {
        if (L.ostate.recorded_hits > hits_seen && L.ostate.count > 0)
            printf("rally: object hit at frame %ld: pols record %d, type 0x%02X (hit %ld)\n",
                   G.frame,
                   (int)L.ostate.list[(L.ostate.count - 1) % SIM_OBJECT_HITS],
                   (unsigned)L.ostate.pols[(size_t)L.ostate.list[(L.ostate.count - 1) % SIM_OBJECT_HITS] * 0x40u + 1],
                   L.ostate.recorded_hits);
        hits_seen = L.ostate.recorded_hits;
    }
    if (c->currsegment != G.log_seg)
        printf("rally: race: section %d entered at frame %ld, catitken %d\n",
               (int)c->currsegment, G.frame, (int)c->catitken);
    if (c->rfn != 0 && G.log_rfn == 0)
        printf("rally: race: finished at frame %ld, catitken %d (%d:%02d.%02d)\n",
               G.frame, (int)c->catitken, (int)(c->catitken / 6000),
               (int)(c->catitken / 100 % 60), (int)(c->catitken % 100));
    if (c->gof != G.log_gof)
        printf("rally: race: gof %d at frame %ld\n", (int)c->gof, G.frame);
    G.log_seg = c->currsegment;
    G.log_rfn = c->rfn;
    G.log_gof = c->gof;
    /* FUN_0044368D stops its batch when FUN_004436C1 sets carry.  A replay
     * plays its capture to the end instead - the capture says when it ends. */
    if (G.live && rep.have_race && !G.race_over && !view_on) {   /* a viewer plays to the file's end */
        const car_state *cars[1];
        cars[0] = &G.car;
        if (sim_race_over_q(cars, rep.race.mode.numpl)) {
            G.race_over = 1;
            printf("rally: the race is over (gof %d) at step %ld: stage time "
                   "%d:%02d.%02d - HOME to race again\n", (int)c->gof,
                   G.live_steps, (int)(c->catitken / 6000),
                   (int)(c->catitken / 100 % 60), (int)(c->catitken % 100));
        }
    }
}

/* ONE LIVE STEP: the same tick, with the device's words from the
 * keyboard or the key script instead of the capture.  Never ends. */
static void live_step(void)
{
    car_state start = G.car, stepped = G.car, in;
    sim_world w;
    car_slots down;
    car_dev dev;
    double v;
    uint32_t words[4];
    int rc, i, recfr = -1;

    if (view_on) {                           /* the recording's words */
        down = 0;
        memset(&dev, 0, sizeof dev);
        memcpy(words, vw, sizeof words);
    } else {
    down = G.have_script ? car_script_at(&G.script, G.live_steps)
                         : (G.video ? car_input_keyboard() : 0);
    /* the pad, through FUN_00408D37's axis arms; a script, or no
     * window, has no device - the keyboard's words exactly */
    if (G.have_script || !G.video || !car_input_pad(&dev))
        memset(&dev, 0, sizeof dev);
    car_input_device_words(down, &dev, car_input_joymap(), words);
    }
    memset(&in, 0, sizeof in);
    in.dannyeax = words[0]; in.dannyebx = words[1];
    in.dannyecx = words[2]; in.dannyedx = words[3];
    rep.race.autopilot_q = 0;
    for (i = 0; i < G.ap_n; i++)
        if (G.live_steps >= G.ap_from[i] && G.live_steps < G.ap_to[i])
            rep.race.autopilot_q = 1;
    if (rrec.open) {
        /* 0x0045DB0B: the chunk and the frame from the car's own
         * step counter; a snapshot of the state BEFORE the step at frame 0
         * (0x0045DC0D), the device words of the step at its slot
         * (0x0045DCFD), the commit after the step at frame 99 (0x0045DD82) */
        const uint8_t *snap[4];
        int k;
        recfr = (int)((uint32_t)G.car.alivet % RPL_FRAMES);
        if (recfr == 0) {
            snap[0] = (const uint8_t *)&G.car;
            for (k = 1; k < 4; k++)
                snap[k] = k <= rep.nrivals ? (const uint8_t *)&rep.rival[k - 1].car : NULL;
            rplrec_snapshot(&rrec, snap, NULL, NULL);
        }
        rplrec_frame(&rrec, (unsigned)recfr, 0, in.dannyebx, in.dannyeax,
                     (int32_t)in.dannyecx, in.dannyedx);
    }
    rc = sim_replay_step(&rep, &stepped, &in, &start, &w);
    if (rc != 0) G.step_errors++;
    SW0 = w;                                 /* car+0x920.., +0xA28. */
    /* a wheel (bits 5..8) or a tyre (bits 1..4) came off - for a log
     * and the parts-live gate; the flying piece is not drawn */
    if (w.parts_off)
        printf("rally: step %ld - carpartsleft bits 0x%03X came off (now "
               "0x%08X)\n", G.live_steps + 1, (unsigned)w.parts_off,
               (unsigned)stepped.carpartsleft);
    if (w.input_439024_due) {
        /* binding slot 9 ran FUN_00439024 in the input stage */
        G.cam_view = G.camx.cam.nview;
        printf("rally: step %ld - binding slot 9 (cycle camera views): next "
               "view %d\n", G.live_steps + 1, (int)G.camx.cam.nview);
    }
    if (w.d4b3_due) {
        G.d4b3_due++;
        if (w.d4b3_ran) {
            G.d4b3_ran++;
            printf("rally: step %ld - FUN_0043D4B3: the car is put back on "
                   "the route at marker %d (a held rescue, 7 s airborne, or "
                   "nowhere free to stand)\n", G.live_steps + 1,
                   (int)stepped.currentb);
        } else
            printf("rally: step %ld - the game would call FUN_0043D4B3, the "
                   "car reset, here; this run has no race context to do it "
                   "with: HOME starts the run again\n", G.live_steps + 1);
    }
    G.held = down;
    G.car = stepped;
    rep.race.rec[0] = &G.car;               /* the tick's was a copy */
    if (rrec.open && recfr == (int)RPL_FRAMES - 1 && rplrec_commit(&rrec) != 0) {
        rec_made = rrec.done;
        printf("rally: the replay file is full (%u chunks): the recording ends\n",
               (unsigned)rrec.nchunks);
    }
    if (rep.have_race && rep.race.mode.gmode == 1
        && G.car.nextchck != start.nextchck) {
        /* FUN_00435688 ran at this checkpoint - what it read and
         * what it left (champ_check.py recomputes it from these) */
        const sim_race *r = &rep.race;
        uint32_t k = (uint32_t)start.nextchck + 1u, n = (uint32_t)r->numcheckposits;
        printf("rally: step %ld - gmode 1 checkpoint %d: position %d of 90, "
               "catitken %d, cp %d / %d; ahead %d ms car %d name %d, behind "
               "%d ms car %d name %d\n", G.live_steps + 1, (int)start.nextchck,
               (int)r->currpos[0], (int)G.car.catitken,
               (int)(k < SIM_RACE_CHECKPOINTS ? r->cp[k] : -1),
               (int)(n < SIM_RACE_CHECKPOINTS ? r->cp[n] : -1),
               (int)r->gap_ahead[0], (int)r->type_ahead[0], (int)r->name_ahead[0],
               (int)r->gap_behind[0], (int)r->type_behind[0],
               (int)r->name_behind[0]);
    }
    if (G.arc_log && G.have_arcade) {
        /* DIAGNOSTIC (RALLY_ARCADE_LOG): what's
         * capture would dump, one line a step */
        int s;
        fprintf(G.arc_log, "%ld nb %d P %d W %d %d Wi %d %d pos %d rfn %d",
                G.live_steps + 1, (int)G.car.nearestb, (int)G.car.arc_behind,
                (int)G.car.arc_w0, (int)G.car.arc_w1, (int)G.car.arc_wi0,
                (int)G.car.arc_wi1, (int)G.car.arcadeposition, (int)G.car.rfn);
        for (s = 0; s < rep.nrivals; s++) {
            const car_state *r = &rep.rival[s].car;
            fprintf(G.arc_log, " | %d d%d nb %d sk %.3f f %.3f g %d v %.1f r %.0f c %d",
                    s + 1, (int)r->driver, (int)r->nearestb, (double)r->arc_skill,
                    (double)r->arc_factor, (int)r->gear, (double)r->czspd,
                    (double)r->revs, (int)r->nextchck);
        }
        fprintf(G.arc_log, " | gap %u %u ms %d %d\n",
                (unsigned)G.car.arc_gap_ahead, (unsigned)G.car.arc_gap_behind,
                (int)G.car.arc_gap_ahead_ms, (int)G.car.arc_gap_behind_ms);
    }
    wx_now = w.wx;                          
    if (G.pause_cool != 0) G.pause_cool--;  /* 0x0045E606, */
    G.frame++;
    G.live_steps++;
    G.steps_run++;
    /* The measurements: the first non-finite position or velocity, the top
     * speed, the distance, and the steps with no wheel on a road class. */
    v = sqrt((double)G.car.newspd[0] * G.car.newspd[0]
             + (double)G.car.newspd[1] * G.car.newspd[1]
             + (double)G.car.newspd[2] * G.car.newspd[2]);
    for (i = 0; i < 3; i++)
        if (!isfinite(G.car.newspd[i]) || !isfinite(G.car.rotspd[i]))
            v = -1.0;
    for (i = 0; i < 16; i++)
        if (!isfinite(G.car.matrix[i])) v = -1.0;
    if (!isfinite(G.car.rotspd_len)) v = -1.0;
    if (v < 0.0 && G.nan_step < 0) {
        G.nan_step = G.live_steps;
        G.halted = 1;
        printf("rally: step %ld - the car's state is NON-FINITE (matrix, "
               "rotspd or velocity); the run stops here rather than step on "
               "(the port's guard)\n", G.live_steps);
    }
    if (v > G.max_speed) G.max_speed = v;
    if (v > 0.0) G.dist += v * 0.01;
    if (G.car.road_wheels_q == 0) G.offroad_steps++;
    if (G.dump) fwrite(&G.car, 1, CAR_REPLAY_PREFIX, G.dump);
    G.camx.snap = 0;
    race_events();
}

/* what the next step takes from the recording - the chunk's snapshot
 * first when this is the chunk's first step, then the step's words.  0 at the
 * file's end (the last whole chunk's last step is its last). */
static int view_prepare(void)
{
    uint32_t a = (uint32_t)G.car.alivet, ch = a / RPL_FRAMES, fr = a % RPL_FRAMES;
    int32_t cx;
    if (ch >= vrd.nchunks) { G.at_end = 1; return 0; }
    if (rplrd_chunk(&vrd, ch) != 0) {
        fprintf(stderr, "rally: chunk %u of the replay is damaged: the viewer stops\n", ch);
        G.at_end = 1;
        return 0;
    }
    if (fr == 0)
        memcpy((unsigned char *)&G.car + 8, vrd.raw + 8, CAR_REPLAY_PREFIX - 8);
    rplrd_frame(&vrd, fr, 0, &vw[1], &vw[0], &cx, &vw[3]);
    vw[2] = (uint32_t)cx;
    G.at_end = (a + 1u) / RPL_FRAMES >= vrd.nchunks && ((a + 1u) % RPL_FRAMES) == 0;
    return 1;
}

/* to the start of chunk `c`: the race put back to its start (seek: the
 * level's objects, the car's load, the settle - the hidden state starts fresh,
 * as the original's seek leaves it), then the chunk's snapshot over the car */
static void view_seek(long c)
{
    seek(0);
    G.car.alivet = (int32_t)(c * (long)RPL_FRAMES);
    G.live_steps = G.frame = c * (long)RPL_FRAMES;
    if (rplrd_chunk(&vrd, (uint32_t)c) == 0)
        memcpy((unsigned char *)&G.car + 8, vrd.raw + 8, CAR_REPLAY_PREFIX - 8);
    G.at_end = 0;
    G.camx.snap = 1;
    printf("rally: the viewer goes to chunk %ld (step %ld)\n", c, G.live_steps);
}

/* ONE 10 ms SIMULATION STEP: FUN_00443744 -> FUN_0045E525 for the one car,
 * frame n-1 -> n.  Returns 0 at the end of the capture (nothing stepped). */
static int step(void)
{
    long n = G.frame + 1;
    const car_state *prev, *cur;
    car_state start, stepped;
    sim_world w;
    int from_game, rc;

    if (G.live) {
        if (G.halted) return 0;
        if (view_on && !view_prepare()) return 0;
        live_step();
        return 1;
    }
    if (n >= cap.frames) {
        G.at_end = 1;
        return 0;
    }
    prev = sim_capture_car(&cap, n - 1);
    cur  = sim_capture_car(&cap, n);
    from_game = G.reseed > 0 && ((n - G.seed - 1) % G.reseed) == 0;
    start = from_game ? *prev : G.car;
    stepped = start;
    rc = sim_replay_step(&rep, &stepped, cur, &start, &w);
    if (rc != 0) G.step_errors++;
    if (from_game) G.reseeds++;
    G.car = stepped;
    wx_now = w.wx;                          
    SW0 = w;                                
    G.frame = n;
    G.steps_run++;
    G.at_end = n >= cap.frames - 1;
    if (G.dump) fwrite(&G.car, 1, CAR_REPLAY_PREFIX, G.dump);
    G.camx.snap = 0;
    race_events();
    return 1;
}

/* ---- rendering ----------------------------------------------------------- */

/* The placed objects are drawn from the SIMULATION'S pols, L.pols - the copy
 * FUN_0045BCE8's hits animate (+0x30..+0x38) - not the stage's own parse of
 * the same file, which never moves (the human drive saw knocked
 * objects stand where they stood).  load() checks the two agree, on every
 * byte objscene_draw_record reads, before a single step. */
static void obj_hook(void *ctx, const stage_view *v, int32_t poly)
{
    const uint8_t *pol;
    (void)ctx;
    /* DIAGNOSTIC: RALLY_OBJECTS_FROM_STAGE draws from the stage's copy, as
     * before - the "before" of the knocked-object picture. */
    if (G.obj_from_stage)
        pol = cdat_pol(&stg.cdat, poly);
    else
        pol = (poly >= 0 && poly < L.pl.npols && L.pols)
              ? L.pols + (size_t)poly * 0x40u : NULL;
    if (pol) {
        objscene_draw_record(&objs, &stg.cdat, pol, v, &G.ocam,
                             OBJ_BREAK_NONE, &G.ost);
    }
}

/* FUN_00438C42's two keys this port has, on player 1's car (the
 * array record, before the step batch).  F1: FUN_00439024 and the view's
 * name (0x00438F4D).  F8: car+0x220 ^= 0x08 and REARVIEW MIRROR ON / OFF
 * (281 / 282).  The message goes the way FUN_00438F65 sends it: msgtopre,
 * flash 0, 300 steps. */
static void camera_keys(int view, int mirror, int vol_down, int vol_up)
{
    int32_t id = -1;
    /* the handler's chain takes ONE key a frame: F1, (F2), F3, F4, ..., F8 */
    if (view) {
        id = sim_camera_next_view_q(&G.camx, G.car.alivet);
        G.cam_view = G.camx.cam.nview;
    }
    else if (vol_down || vol_up) {
        /* 0x00438CA9 - F3 first, so both at once is F3 */
        id = snd_race_volume_key(&S, !vol_down, plat_input_key(VK_LCTRL));
    }
    else if (mirror) {
        unsigned char *raw = (unsigned char *)&G.car + 0x220;
        uint32_t mask;
        memcpy(&mask, raw, 4);
        mask ^= 0x08u;
        memcpy(raw, &mask, 4);
        id = (mask & 0x08u) ? 0x11A : 0x119;
    }
    if (id >= 0 && rep.have_race) {
        rep.race.msg[0].topre = id;
        rep.race.msg[0].flash = 0;
        rep.race.msg[0].timer = 0x12C;
    }
}

/* ----: PAUSED and RETIRE? (doplaygame_q's modal waits) -------------
 *
 * The original blocks inside each (its own frame loop, 0x00452103); the port
 * runs them as states of its one loop: no step runs, the frame still draws.
 * Both open with stop_sounds_q (0x0045ADE6 -> 0x0040D213) and close with
 * 0x0045ADAD; [0x5D64B4] = 1 between, its readers unread. */
enum { MODAL_NONE = 0, MODAL_PAUSED, MODAL_RETIRE };

static void race_msg(int32_t id, int32_t timer)       /* player 1's banner */
{
    if (!rep.have_race) return;
    rep.race.msg[0].topre = id;
    rep.race.msg[0].flash = 0;
    rep.race.msg[0].timer = timer;
}

static void race_msg_clear(void)        /* 0x00443630 / 0x004434CF */
{
    if (!rep.have_race) return;
    rep.race.msg[0].w728 = 0;
    rep.race.msg[0].topre = 0;
    rep.race.msg[0].w724 = 0;
}

static void race_sound_stop(void)       /* 0x0045ADE6 -> 0x0040D213 */
{
    if (have_ct) snd_car_close(&C);
    if (have_world) snd_world_stop(&W);
    snd_cd_off(&S);
}

static void race_sound_resume(void)     /* 0x0045ADAD */
{
    /* each player's record +0x08 = 1 unless a replay plays ([0x6CEF7C]) */
    if (have_ct) C.rec.on = 1;
    snd_cd_open(&S);                    /* 0x0040D423 */
}

/* THE PAD AS KEYS - a remaster (what gptokeyb did on the device, in
 * the port).  In a race the pad drives the car through its own arms
 * (car_input_pad); the keys it presses here are only the ones a pad has no
 * binding for: the menus' arrows, RETURN and ESC, RETIRE?'s Y / N / A, and
 * any key for PAUSED.  The PORT'S LAYOUT:
 *   menus   d-pad or left stick: arrows   A, START: RETURN   B, BACK: ESC
 *   race    BACK: ESC (RETIRE?)           A, START once the race is over:
 *                                         RETURN (to RESULTS)
 *   waits   A: Y   B: N   X: A            START, Y: RETURN (PAUSED's any key)
 * A name being typed has no pad path.
 * --pad-nintendo (2026-10-19, the RG40XX-H): SDL names the face buttons by
 * their Xbox POSITION, so a Nintendo-labelled pad's printed A arrives as B.
 * In the MENUS only, A and B swap there, so the printed A selects; the race
 * and its waits keep the positions (the user: the race's buttons were right). */
enum { PADK_MENU, PADK_RACE, PADK_WAIT, PADK_VIEW };
static int pad_ctx;
static void set_pad_ctx(int c) { pad_ctx = c; plat_touch_context(c == PADK_RACE ? 1 : (c == PADK_WAIT ? 2 : 0)); }
static int pad_nintendo;

static int pad_key(int sc)
{
    plat_pad p;
    unsigned b;
    if (!G.video || !plat_pad_read(&p)) return 0;
    b = p.buttons;
    if (pad_ctx == PADK_MENU) {
        if (pad_nintendo) {
            unsigned ab = b & (PLAT_PAD_A | PLAT_PAD_B);
            b &= ~(PLAT_PAD_A | PLAT_PAD_B);
            if (ab & PLAT_PAD_A) b |= PLAT_PAD_B;
            if (ab & PLAT_PAD_B) b |= PLAT_PAD_A;
        }
        switch (sc) {
        case 82: return (b & PLAT_PAD_UP) || p.ly < -16384;
        case 81: return (b & PLAT_PAD_DOWN) || p.ly > 16384;
        case 80: return (b & PLAT_PAD_LEFT) || p.lx < -16384;
        case 79: return (b & PLAT_PAD_RIGHT) || p.lx > 16384;
        case 40: return (b & (PLAT_PAD_A | PLAT_PAD_START)) != 0;
        case 41: return (b & (PLAT_PAD_B | PLAT_PAD_BACK)) != 0;
        }
        return 0;
    }
    if (pad_ctx == PADK_VIEW) {              /* the replay viewer's keys, on the pad */
        switch (sc) {
        case 44: return (b & (PLAT_PAD_A | PLAT_PAD_START)) != 0;      /* SPACE: pause   */
        case 55: return (b & PLAT_PAD_Y) != 0;                         /* PERIOD: a step */
        case 80: return (b & PLAT_PAD_LEFT) || p.lx < -16384;          /* LEFT: -1 chunk */
        case 79: return (b & PLAT_PAD_RIGHT) || p.lx > 16384;          /* RIGHT: +1      */
        case 81: return (b & PLAT_PAD_DOWN) || p.ly > 16384;           /* DOWN: -10      */
        case 82: return (b & PLAT_PAD_UP) || p.ly < -16384;            /* UP: +10        */
        case 58: return (b & PLAT_PAD_X) != 0;                         /* F1: the camera */
        case 74: return (b & PLAT_PAD_BACK) != 0 && (b & PLAT_PAD_X);  /* HOME: BACK + X */
        case 41: return (b & PLAT_PAD_B) != 0 || ((b & PLAT_PAD_BACK) && !(b & PLAT_PAD_X));
        }
        return 0;
    }
    if (pad_ctx == PADK_RACE) {
        if (sc == 41) return (b & PLAT_PAD_BACK) != 0;
        if (sc == 40)
            return G.race_over && (b & (PLAT_PAD_A | PLAT_PAD_START)) != 0;
        return 0;
    }
    /* RETIRE?'s answers are the first letters of the lexicon's 195..197 ("t",
     * "n" in the Polish text): A yes, B no, X all - whatever the letters are */
    if (G.have_lex) {
        int id;
        for (id = 0xC3; id <= 0xC5; id++) {
            int c = (unsigned char)lexicon_text(&G.lex, id)[0] | 0x20;
            if (c >= 'a' && c <= 'z' && sc == 4 + (c - 'a'))
                return (b & (id == 0xC3 ? PLAT_PAD_A : id == 0xC4 ? PLAT_PAD_B
                                                                  : PLAT_PAD_X)) != 0;
        }
    }
    switch (sc) {
    case 28: return (b & PLAT_PAD_A) != 0;                      /* y */
    case 17: return (b & PLAT_PAD_B) != 0;                      /* n */
    case 4:  return (b & PLAT_PAD_X) != 0;                      /* a */
    case 40: return (b & (PLAT_PAD_START | PLAT_PAD_Y)) != 0;
    }
    return 0;
}

/* A key down on the keyboard, or pressed by the pad. */
static int key_down(int sc)
{
    return plat_input_key(sc) || pad_key(sc);
}

/* 0x00441CA4's just-pressed bytes [0x004F1CA4 + code], by FOLDED code
 * (car_keys.h), from the held state: once a frame. */
static void fold_scan(unsigned char jp[0x80])
{
    int i;
    for (i = 0; i < 0x80; i++) {
        int d = G.video && car_key_sdl[i] ? key_down(car_key_sdl[i]) : 0;
        jp[i] = (unsigned char)(d && !G.fold_down[i]);
        G.fold_down[i] = (unsigned char)d;
    }
}

/* FUN_00443531: PAUSED */
static void pause_enter(void)
{
    G.pause_cool = 0x19;
    race_sound_stop();
    race_msg(0xCB, -1);                 /* 203 PAUSED, until replaced */
    G.pause_bc = 0x1E;
    G.modal = MODAL_PAUSED;
    printf("rally: step %ld - PAUSED\n", G.live_steps);
}

/* One frame of 0x004435BC's loop.  [0x00512104] == 0 (the window inactive)
 * holds pause_bc at >= 10: the port's window does not report it. */
static void pause_frame(const unsigned char jp[0x80], const int *pressed)
{
    int i, any = 0;
    if (G.pause_bc != 0) { G.pause_bc--; return; }
    /* 0x004435E5: the F-keys work while paused */
    if (pressed[11] || pressed[12] || pressed[13] || pressed[14])
        camera_keys(pressed[11], pressed[12], pressed[13], pressed[14]);
    /* 0x004435EA: any key 1..0x7F but LCtrl, LAlt, Tab, F1-F10, F11, F12 */
    for (i = 1; i < 0x80; i++) {
        if (i == 0x1D || i == 0x38 || i == 0x0F) continue;
        if ((i >= 0x3B && i < 0x45) || i == 0x57 || i == 0x58) continue;
        any |= jp[i];
    }
    if (!any) return;
    race_msg_clear();
    race_sound_resume();
    G.car.dannyebx = 0;                 /* 0x00443682: player 1's */
    G.modal = MODAL_NONE;
    printf("rally: step %ld - resumed\n", G.live_steps);
}

/* FUN_00443404: RETIRE?  [0x6C210C] (the replay ring's chunk floor) is not
 * ported: the port records no replay. */
static void retire_enter(void)
{
    race_sound_stop();
    race_msg(G.retire_all ? 0xC2 : 0xC1, -1);   /* 194 (Y/N/A) / 193 (Y/N) */
    G.modal = MODAL_RETIRE;
    printf("rally: step %ld - RETIRE? (%s)\n", G.live_steps,
           G.retire_all ? "Y/N/A" : "Y/N");
}

/* One frame of 0x0044348F's loop; returns FUN_00443404's EAX, or 1 while
 * it waits.  0x00407AB8: the mailbox key, through the keyboard's layout, the
 * first letter of 195 / 196 / 197 (`y`, `n`, `a` in ENGLISH.TXT) - the
 * port maps the letter keys as US QWERTY. */
static int retire_frame(const unsigned char jp[0x80])
{
    static const unsigned char letter_code[26] = {      /* a..z, folded */
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
        0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F,
        0x11, 0x2D, 0x15, 0x2C };
    int id, r = 0;
    for (id = 0xC3; id <= 0xC5; id++) {
        const char *t = G.have_lex ? lexicon_text(&G.lex, id) : &"yna"[id - 0xC3];
        int c = (unsigned char)t[0] | 0x20;
        if (c >= 'a' && c <= 'z' && jp[letter_code[c - 'a']]) {
            r = id == 0xC3 ? 1 : id == 0xC4 ? -1 : 2;
            break;
        }
    }
    if (r == 0 || (r == 2 && (G.gmode == 2 || !G.retire_all))) return 1;
    race_msg_clear();
    race_sound_resume();
    G.car.dannyebx = 0;                 /* 0x0044351C: both players' */
    G.modal = MODAL_NONE;
    return r == 1 ? -1 : r == 2 ? -2 : 0;
}

/* MASK 1, [0x005D423C] (0x0045F260..0x0045F3CB) - the car's bodywork that
 * has come off.  Seven words of the car's row become one-bit masks (0 when
 * the word is -1, "none"):
 *     [0x581B7C] <- +0x64   [0x581B88] <- +0x84 (+ 0x6000000 on car 4 or 7,
 *     0x3000000 on car 13: 0x0045F22E..0x0045F267, skipped with the word)
 *     [0x581B8C] <- +0x88   [0x581B80] <- +0x7C   [0x581B84] <- +0x80
 *     [0x581B90] <- +0x78   [0x581B94] <- +0x8C
 * then, from all ones, for each of carpartsleft (car+0x298) bits 9, 12..17
 * that is CLEAR, the matching bit mask is ANDed out: 9 -> B7C, 12 -> B90,
 * 13 -> B88, 14 -> B8C, 15 -> B94, 16 -> B80, 17 -> B84. */
/* The seven words of the comment above, [0x581B7C..0x581B94], by name. */
typedef struct { uint32_t b7c, b80, b84, b88, b8c, b90, b94; } mask_words;

static void car_mask_words(const car_state *car, mask_words *w)
{
    const int32_t *r = G.car_row[0];
    int32_t id = car->carnum1;
#define PBIT(off) (r[CAR_ROW(off)] == -1 ? 0u : 1u << ((uint32_t)r[CAR_ROW(off)] & 31u))

    if (id >= 0 && id < SIM_CAMERA_CARS) {
        r = G.car_row[id];
    }
    w->b7c = PBIT(0x64);
    w->b80 = PBIT(0x7C);
    w->b84 = PBIT(0x80);
    w->b88 = 0;
    if (r[CAR_ROW(0x84)] != -1) {
        w->b88 = PBIT(0x84);
        if (id == 4 || id == 7)  w->b88 |= 0x6000000u;
        if (id == 13)            w->b88 |= 0x3000000u;
    }
    w->b8c = PBIT(0x88);
    w->b90 = PBIT(0x78);
    w->b94 = PBIT(0x8C);
#undef PBIT
}

/* THE DISTANCE CUTS, 0x0045F4C4..0x0045F63A, on the car's mask
 * ([0x005BA5DC], car+0xF04) - for every car but the viewing car in DRIVER /
 * INTERIOR (carstod nibble 0x10 jumps past them), by `depth`, the car's
 * view-space z [0x005F4464] in 16.16:
 *   >= 20:  mask &= row +0x90 | 1 << lit part   (the exterior mask: the
 *           cabin goes, WHATEVER the car's 0x100 / 0x200 nibble says)
 *           and the four pair parts (rows +0x68 +0x6C +0x70 +0x74,
 *           [0x581B98..A4], bit = word & 31: a -1 is bit 31) and B80, B84
 *   >= 40:  and B88, B8C, B7C
 *   >= 60:  and B94, B90
 * The viewing car's chase camera sits 5..8 units away, so only a RIVAL
 * reaches them in an ordinary race. */
static uint32_t car_distance_cuts(uint32_t mask, const car_state *car,
                                  const int32_t *r, int32_t depth)
{
    mask_words w;
    int i;
    static const int pair[4] = { 0x68, 0x6C, 0x70, 0x74 };

    car_mask_words(car, &w);
    if (depth >= 0x140000) {
        mask &= (uint32_t)r[CAR_ROW(0x90)] | (1u << ((uint32_t)r[CAR_ROW(0x64)] & 31u));
        for (i = 0; i < 4; i++) mask &= ~(1u << ((uint32_t)r[CAR_ROW(pair[i])] & 31u));
        mask &= ~w.b80;
        mask &= ~w.b84;
    }
    if (depth >= 0x280000) mask &= ~w.b88 & ~w.b8c & ~w.b7c;
    if (depth >= 0x3C0000) mask &= ~w.b94 & ~w.b90;
    return mask;
}

static uint32_t car_mask1(const car_state *car)
{
    uint32_t m = 0xFFFFFFFFu;
    mask_words w;
    uint32_t cp = (uint32_t)car->carpartsleft;
    uint32_t b7c, b80, b84, b88, b8c, b90, b94;

    car_mask_words(car, &w);
    b7c = w.b7c; b80 = w.b80; b84 = w.b84; b88 = w.b88;
    b8c = w.b8c; b90 = w.b90; b94 = w.b94;
    if (!(cp & (1u << 9)))  m &= ~b7c;
    if (!(cp & (1u << 12))) m &= ~b90;
    if (!(cp & (1u << 13))) m &= ~b88;
    if (!(cp & (1u << 14))) m &= ~b8c;
    if (!(cp & (1u << 15))) m &= ~b94;
    if (!(cp & (1u << 16))) m &= ~b80;
    if (!(cp & (1u << 17))) m &= ~b84;
    return m;
}

/* One pass of the 3D frame (0x004536A9's part this port draws): the sky, the
 * stage, the cars.  `mirror` is the rear-view mirror's pass: the original
 * does not draw the viewing car in it (0x00453AFF). */
/* the viewing car's part gate in DRIVER / INTERIOR (views 2, 3),
 * 0x00453B0C..0x00453B9C then 0x0045F44A..0x0045F6D3.  0x00453AD2 leaves
 * carstod's low nibble at 4 for both views (2 is overwritten unless the
 * view is 1, which cannot be interior), so the mask is row +0x94 with the
 * lit part's bit set; nibble 0x10 skips the distance cuts at 0x0045F4C4,
 * and nibble 4 skips row +0x90 at 0x0045F68B.  A blend running on the
 * viewing car draws every part (0x0045F6CA, [0x005BA5C8] = car+0x69C). */
static void car_parts(objscene_parts *g, const car_state *car,
                      const sim_camera *cam)
{
    const int32_t *r = G.car_row[0];
    int32_t id = car->carnum1;

    if (id >= 0 && id < SIM_CAMERA_CARS) {
        r = G.car_row[id];
    }
    g->lit_part    = r[CAR_ROW(0x64)];
    g->mask        = (uint32_t)r[CAR_ROW(0x94)]
                   | (1u << ((uint32_t)g->lit_part & 31u));    /* BTS */
    g->lit_on      = G.car_lit;
    g->out_part[0] = r[CAR_ROW(0x68)];
    g->out_part[1] = r[CAR_ROW(0x6C)];
    g->in_part[0]  = r[CAR_ROW(0x70)];
    g->in_part[1]  = r[CAR_ROW(0x74)];
    g->inside      = 1;                 /* car+0xAB8, view 2 or 3          */
    g->state       = NULL;
    g->glass       = NULL;
    g->points      = NULL;
    g->light       = NULL;
    g->mask1       = car_mask1(car);
    if (cam->blending || cam->t != 0.0f) {
        g->mask = 0xFFFFFFFFu;          /* 0x0044E5C9 / 0x0045F6D3         */
    }
}

/* The car's part-state table, [0x005823D0] (section 7): what FUN_0044E663 compares a polygon's byte +0x0B against
 * when its byte +6 is non-zero.  0x0045F9E6..0x0045FB44 fills it before
 * each car's draw (called at 0x0045F094) and 0x0045F9D3 zeroes slots 0..63
 * after it (0x0045F873), so every car starts from zeros.  `racenum` is
 * car+0xAA8, ctd_carnum: slot + 1 for the cars 0x00461689 numbers. */
static void car_state_table(int32_t t[256], const car_state *car,
                            int32_t racenum, uint8_t lamp_out[17])
{
    /* 0x0045E8A0..0x0045EC44, the lamp bytes car+0xAC0..0xAD0, and
     * 0x0045FB28's map from byte i to slot. */
    static const int slot[17] = {
        10, 11, 24, 25, 12, 13, 22, 23, 26, 27, 14, 15, 16, 17, 19, 20, 18
    };
    uint8_t  lamp[17];
    int      on = car->latch_84_q & 0xFF, i, id;
    int      brk = car->brake != 0, rev = car->gear == -1;
    int      l0 = !(car->lightoff[0] & 1u), l1 = !(car->lightoff[1] & 1u);
    int      l2 = !(car->lightoff[2] & 1u), l3 = !(car->lightoff[3] & 1u);
    int      lf = !(car->lightoff[4] & 1u), lb = !(car->lightoff[5] & 1u);
    float    tacho;
    int32_t  r, tr;

    memset(t, 0, 256 * sizeof t[0]);

    /* 0x0045F9EB / 0x0045FA17: the wheels' blur plate past 5.0 ([0x5F4600]). */
    t[1] = t[4] = fabsf(car->fwhlspd) >= 5.0f;
    t[2] = t[3] = fabsf(car->rwhlspd) >= 5.0f;

    /* 0x0045FA3E: the tacho needle.  car+0x718 is max(revs, 0)
     * (FUN_0045E872), FISTP'd, clamped [0, 0x2EDF], per 250, through the
     * car's row of plate characters ('0'..'9', 'a'..). */
    id = car->carnum1 < 0 ? 0 : car->carnum1 >= SIM_CAMERA_CARS
       ? SIM_CAMERA_CARS - 1 : car->carnum1;
    tacho = car->revs > 0.0f ? car->revs : 0.0f;
    r = (int32_t)lrintf(tacho);
    if (r < 0) r = 0;
    if (r > 0x2EDF) r = 0x2EDF;
    r = (int32_t)G.car_tacho[id][r / 250] - 0x30;
    if ((uint32_t)r >= 10u) r -= 0x27;
    t[5] = r;

    /* 0x0045FA93: the race number; one digit goes to slot 9, two to 6 and 7,
     * and the unused slots take 999, which no polygon carries. */
    if (racenum / 10 != 0) {
        t[6] = racenum / 10; t[7] = racenum % 10; t[9] = 999;
    } else {
        t[9] = racenum % 10; t[6] = t[7] = 999;
    }

    /* 0x0045FAE0: slot 8 by the track, (shauntracknum - 11) clamped [0, 69]
     * per ten.  What the plate shows is not read. */
    tr = G.track - 11;
    if (tr < 0) tr = 0;
    if (tr > 0x45) tr = 0x45;
    t[8] = G.car_slot8[tr / 10];

    /* The lamps.  car+0x84 is the lights' latch; [0x005E095C] gates 0xAC8/9
     * and [0x005E0960] 0xACA..F; then each is ANDed with !lightNoff. */
    lamp[0] = lamp[1] = lamp[2] = lamp[3] = (uint8_t)on;
    lamp[8] = lamp[9] = (uint8_t)G.car_lamp69c;
    for (i = 10; i <= 15; i++) lamp[i] = (uint8_t)(on & G.car_lit);
    lamp[4] = lamp[5] = lamp[16] = (uint8_t)brk;
    lamp[6] = lamp[7] = (uint8_t)rev;
    lamp[0] &= l0;  lamp[1] &= l1;
    for (i = 10; i <= 15; i++) lamp[i] &= lf;
    lamp[8] &= l2;  lamp[2] &= l2;  lamp[4] &= l2;  lamp[6] &= l2;
    lamp[9] &= l3;  lamp[3] &= l3;  lamp[5] &= l3;  lamp[7] &= l3;
    lamp[16] &= lb;
    /* 0x0045FB0F: copied only while car+0x310 (dam[22]) < 100.0
     * ([0x0057AF98]); past it every lamp stays at 0. */
    if (car->dam[22] < 100.0f)
        for (i = 0; i < 17; i++) t[slot[i]] = lamp[i];
    if (lamp_out) memcpy(lamp_out, lamp, 17);
}

/* car+0xABC, 0x0045EC4A..0x0045ECC5 - the lamps' brightness the
 * snow3d draw tests against 16 (block +0x50, FUN_00454419 at 0x00454454):
 * (0xAC0 + 0xAC1) x 4, plus 0xACA..0xACF when car+0x298 bit 9, as a BYTE,
 * x 0x20; 0 at dam[22] >= 100 ([0x0057AF98], unordered too) or with the
 * lights' latch car+0x84 off. */
static int32_t car_lamp_abc(const car_state *car, const uint8_t lamp[17]);

/* 0x0044D603's roster walk for the stage's headlight pass: a car lights the
 * ground when +0x84 (the latch) is set - 0x0044D65A's first test, and the
 * only per-car one.  +0x25C (driver) is tested once for the whole pass, by
 * the gate FUN_0045161B (stage_lit_all), not here: - a
 * despawned arcade rival (driver 0, its latch left on) still lights the
 * world from where its record was left.  Its frame is +0x9CC, the inverse of
 * the draw matrix (sim_refresh_active_frame's adjugate is that inverse for an
 * orthonormal matrix), so the transpose of +0xB4's 3x3 here. */
static void stage_lit_add(stage_view *v, const car_state *car)
{
    uint8_t lamp[17];
    int32_t state[256];
    int r, c;

    if (v->nlit >= STAGE_MAX_LIT || car->latch_84_q == 0)
        return;
    car_state_table(state, car, 1, lamp);
    for (r = 0; r < 3; r++) v->lit[v->nlit].pos[r] = car->pos[r];
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            v->lit[v->nlit].frame[r * 3 + c] = car->matrix[c * 4 + r];
    v->lit[v->nlit].abc = car_lamp_abc(car, lamp);
    v->nlit++;
}

/* getenv once.  The environment does not change during a run, and
 * these were scanned every frame (or every effect).  `cache` starts at -1. */
static int env_flag(const char *name, signed char *cache)
{
    if (*cache < 0) {
        *cache = getenv(name) != NULL;
    }
    return *cache;
}
static signed char env_sound_timing = -1;
static signed char env_sparks = -1;

static int g_cli_no_reflect;        /* --no-reflect was given */

static void stage_lit_all(stage_view *v)
{
    static signed char env_no_headlights = -1;

    int on, s;

    v->nlit = 0;
    v->nflash = 0;
    /* the impact flash's slots.  First the DIAGNOSTIC
     * RALLY_FORCE_FLASH=<pnt>,<k> - the same slot tools/flashforce.py forces in
     * the original, for a frame-for-frame comparison - then the live ones below.
     * [0x005F0A18] is |= the slot count (FUN_0045161B). */
    {
        const char *e = getenv("RALLY_FORCE_FLASH");
        int pnt, k;

        if (e && sscanf(e, "%d,%d", &pnt, &k) == 2) {
            cdat_pnt(&stg.cdat, pnt, v->flash[0].pos);
            v->flash[0].k = k;
            v->nflash = 1;
        }
    }
    /* FUN_00452D06 (0x00452D06..0x00452D81): one slot per type-0x3B record on the
     * hit list [0x00685F44], at most 16 - the anchor pnt (rec+4) and the fade
     * counter k (rec+0x11, 15 down to 0).  Records fade in sim/objects.c. */
    if (L.have_objects && L.ostate.pols) {
        int i, n = L.ostate.count;

        if (n > SIM_OBJECT_HITS) n = SIM_OBJECT_HITS;
        for (i = 0; i < n && v->nflash < STAGE_MAX_FLASH; i++) {
            int32_t idx = L.ostate.list[i], anchor;
            const uint8_t *pol;

            if (idx < 0 || idx >= L.ostate.npols) continue;
            pol = L.ostate.pols + (size_t)idx * 0x40u;
            if (pol[1] != 0x3B) continue;
            memcpy(&anchor, pol + 4, 4);
            if (anchor < 0 || anchor >= stg.cdat.npnts) continue;
            cdat_pnt(&stg.cdat, anchor, v->flash[v->nflash].pos);
            v->flash[v->nflash].k = pol[0x11];
            v->nflash++;
        }
    }
    if (G.day_ramp || env_flag("RALLY_NO_HEADLIGHTS", &env_no_headlights)) return;
    /* FUN_0045161B, [0x005F0A18] - the pass runs only when some car
     * of the roster has +0x25C and +0x84 both non-zero, whatever HEADLIGHTS
     * says (|= [0x005F0B08], the type-0x3B flash count, once lands).
     * The split-screen gate ([0x5D6750], [0x5F0A68]) has no case here: the
     * port races one human. */
    on = G.car.driver != 0 && G.car.latch_84_q != 0;
    for (s = 0; G.have_arcade && s < rep.nrivals && !on; s++)
        on = rep.rival[s].car.driver != 0 && rep.rival[s].car.latch_84_q != 0;
    if (!on) return;
    stage_lit_add(v, &G.car);
    /* 0x0044D242 / 0x0044D603 light a car's lamps only when
     * [0x5D676C] == 1 or the car is one of the players' ([0x5F0A54]); the
     * rivals are not.  The menus' default is 0 (the image's [0x707FD0]). */
    if (G.have_arcade && !(G.vis_set && !G.vis_headlights)) {
        for (s = 0; s < rep.nrivals; s++)
            stage_lit_add(v, &rep.rival[s].car);
    }
}

static int32_t car_lamp_abc(const car_state *car, const uint8_t lamp[17])
{
    uint8_t a = (uint8_t)(lamp[0] + lamp[1]);
    int32_t v;
    int i;
    a = (uint8_t)(a + a);
    a = (uint8_t)(a + a);
    if ((uint32_t)car->carpartsleft & (1u << 9))
        for (i = 10; i <= 15; i++) a = (uint8_t)(a + lamp[i]);
    v = (int32_t)a * 0x20;
    if (!(car->dam[22] < 100.0f)) v = 0;
    if (car->latch_84_q == 0) v = 0;
    return v;
}

/* [0x005F4464], the car's view-space z in 16.16 (0x00453048, FISTP): its
 * position through the view.  The reflection fade (car_light_now), the
 * distance cuts (car_distance_cuts) and the wheels' 60-unit cut read it. */
static int32_t car_view_depth(const car_state *car)
{
    const ral_obj_camera *c = &G.ocam;
    float x = (float)car->pos[0] + c->view_trans.m[12];
    float y = (float)car->pos[1] + c->view_trans.m[13];
    float z = (float)car->pos[2] + c->view_trans.m[14];
    return (int32_t)lrintf(x * c->view_rot.m[2] + y * c->view_rot.m[6]
                           + z * c->view_rot.m[10] + c->view_rot.m[14]);
}

/* The parts record outside DRIVER / INTERIOR.
 * MEASURED, run 26: the viewing car's carstod is 0x221 in every outside
 * view - bit 0x200, not 0x100 - so 0x0045F68B's row +0x90 never applies
 * and car+0xF04 stays 0xFFFFFFFF.  Every part is drawn, through section
 * 1's lit-part and inside/outside-pair tests.  The distance cuts at
 * 0x0045F4C4 (car_distance_cuts) are `depth`'s; the viewing car
 * never reaches them.  Mask 1 (car_mask1, 0x0045F311) IS ported,
 **/
static void car_parts_outside(objscene_parts *g, const car_state *car,
                              const sim_camera *cam, const int32_t *state,
                              int viewer, int32_t depth)
{
    const int32_t *r = G.car_row[0];
    int32_t id = car->carnum1;

    if (id >= 0 && id < SIM_CAMERA_CARS) {
        r = G.car_row[id];
    }
    g->lit_part    = r[CAR_ROW(0x64)];
    g->mask        = 0xFFFFFFFFu;
    /* CAR DETAIL ([0x5D6768], 0x00453BA6..0x00453BC0): the nibble
     * car+0x6FC & 0xF00 is 0x100 for every car and 0x200 for the viewer
     * (detail 1) or every car (detail 2); with 0x100 and an exterior view
     * 0x0045F68B ANDs the row's +0x90 exterior mask in (cabin parts out).
     * Menu races only. */
    if (G.vis_set && !(viewer ? G.vis_detail != 0 : G.vis_detail == 2))
        g->mask &= (uint32_t)r[CAR_ROW(0x90)] | (1u << ((uint32_t)r[CAR_ROW(0x64)] & 31u));
    /* the distance cuts, 0x0045F4C4 (depth INT32_MIN: not known) */
    if (depth != INT32_MIN) g->mask = car_distance_cuts(g->mask, car, r, depth);
    g->lit_on      = G.car_lit;
    g->out_part[0] = r[CAR_ROW(0x68)];
    g->out_part[1] = r[CAR_ROW(0x6C)];
    g->in_part[0]  = r[CAR_ROW(0x70)];
    g->in_part[1]  = r[CAR_ROW(0x74)];
    g->inside      = 0;
    (void)cam;
    g->state = state;
    g->glass = NULL;                    /* the caller's: car_crush_parts    */
    g->points = NULL;
    g->light = NULL;
    g->mask1 = car_mask1(car);
}

/* car+0x700..0x714 for the draw, the engine crush pass's six words
 * (objscene_car_windows).  The original
 * runs the pass at render rate and copies the words in at the next render
 * snapshot, so they trail the octants by a few steps; here they follow the
 * octants of the step being drawn.  NULL (all zero) if the car's two
 * models do not load - the original's value at every start line.
 * The same pass's live vertex buffer is what the original draws (model A
 * moved toward the crushed B, FUN_004C079B): parts.points, the dented body. */
static void car_crush_parts(objscene_parts *g, struct car_glass *c,
                            const car_state *car, int slot)
{
    if (!c->valid || memcmp(c->oct, car->caroctdamage, sizeof c->oct) != 0) {
        const objscene_type *ta = &objs.type[OBJ_CAR_TYPE(slot)];
        size_t n = ta->have ? ta->m.npoints : 0;
        memcpy(c->oct, car->caroctdamage, sizeof c->oct);
        if (n > c->npts) {
            ro3_vec *p = realloc(c->pts, n * sizeof *p);
            if (p) { c->pts = p; c->npts = n; }
        }
        c->ok = objscene_car_crush(&objs, slot, car->caroctdamage, c->w,
                                   n && n <= c->npts ? c->pts : NULL) == 0;
        c->valid = 1;
    }
    g->glass  = c->ok ? c->w : NULL;
    g->points = c->ok && c->pts ? c->pts : NULL;
}

/* FUN_00425F36 for the car (0x0045F75B): the sun through the view's
 * rotation, [0x5E7F24] = -azimuth, [0x5E7F28] = elevation; and the base
 * [0x005E0964] = car+0x240, the car's ground light (0x00453019, written by
 * the step's FUN_0044CD28 - sim_car_ground_light_q).  A car whose step has
 * not yet found a polygon still holds 0 there; the original's would too. */
static const objscene_light *car_light_now(const stage_view *v, const car_state *car, int interior)
{
    static signed char env_car_shade = -1;

    if (!G.have_car_light || env_flag("RALLY_CAR_SHADE", &env_car_shade))
        return NULL;
    objlit_light(G.car_light.light, -G.sun_az, G.sun_el, &G.ocam.view_rot);
    /* the impact flash, FUN_0044D473 at the car's position, into the base ([0x005E0954]) */
    G.car_light.base = stage_flash_add(v, car->pos, car->ground_col);

    /* The reflection strength [0x00581BBC] for this car's draw
     * 0x0045F053..0x0045F083 REPLACES the
     * level's value with min(0x3FCE0 - [0x005D6C14], 0x40000) * [0x005BD9C8]
     * >> 18; [0x005D6C14] is the weather's slot-0 ripple counter
     * (0x00448357, unported) - 0 in the dry, and [0x005BD9C8] is 0x200 in
     * the image and written nowhere.  Then 0x0045F4C4..0x0045F548, unless
     * the viewing car is in the cockpit (nibble 0x10 jumps past it): x
     * (1 - fade), fade = clamp((depth - 10.0) / 20.0, 0, 1), depth the
     * car's view-space z, [0x005F4464] (0x00453048). */
    {
        const ral_obj_camera *c = &G.ocam;
        uint32_t base = 0x3FCE0u;                         /* - 0: ripple off */
        uint32_t s;
        if (base > 0x40000u) base = 0x40000u;
        s = (uint32_t)(((uint64_t)base * 0x200u) >> 18);
        if (!interior) {
            float x = (float)car->pos[0] + c->view_trans.m[12];
            float y = (float)car->pos[1] + c->view_trans.m[13];
            float z = (float)car->pos[2] + c->view_trans.m[14];
            float d = x * c->view_rot.m[2] + y * c->view_rot.m[6]
                    + z * c->view_rot.m[10] + c->view_rot.m[14];
            int32_t di = (int32_t)lrintf(d);
            float fade = (float)((long double)((int64_t)di - 0xA0000) / 1310720.0L);
            if (fade < 0.0f) fade = 0.0f;
            if (fade > 1.0f) fade = 1.0f;
            s = (uint32_t)lrintf((float)((1.0L - (long double)fade) * (long double)s));
        }
        G.car_light.reflect = s;
        G.car_light.env_tex = G.car_env_tex;
    }
    return &G.car_light;
}

/* one car outside the cockpit, 0x0045F094's way.  The body is the
 * model without the four wheels' polygons (the loader's split, as a part
 * mask on the material); each wheel is drawn after it, with only its own
 * material, under the matrix 0x0045ECC5..0x0045F04D builds: pivot, spin,
 * steer (the front pair), height.  0x0045F7E3 draws no wheel at all beyond
 * 60 m of view depth ([0x005F4464], the car's position through the view),
 * and a wheel only while its CPLEFT bit 5 + i is set (0x0045F7F5..0x0045F849). */
static void draw_car(int slot, const car_state *car,
                     const sim_collision_points *pts, const stage_view *v,
                     const objscene_parts *parts)
{
    const int32_t id = (car->carnum1 >= 0 && car->carnum1 < SIM_CAMERA_CARS)
                     ? car->carnum1 : 0;
    const wheel_geom *wg = &G.car_wheel[id];
    const uint32_t wmask = wheels_mask(wg);
    objscene_parts p = *parts;
    ral_mat4 m[4];
    int32_t sus[4];
    float depth;
    int i;

    /* DIAGNOSTIC, not the original's: RALLY_CAR_YAW=D turns every car D
     * degrees about its own vertical before it is drawn, so the chase camera
     * sees its side or front (the wheels' picture). */
    float yawed[16];
    const float *rot = car->matrix;
    {
        const char *e = getenv("RALLY_CAR_YAW");
        if (e && *e) {
            ral_mat4 ry, cm, o;
            memset(&cm, 0, sizeof cm);
            for (i = 0; i < 12; i++) cm.m[i] = car->matrix[i];
            cm.m[15] = 1.0f;
            ral_obj_rot_y16(&ry, (unsigned)(atof(e) / 360.0 * 65536.0));
            ral_mat4_mul(&o, &ry, &cm);
            memcpy(yawed, o.m, sizeof yawed);
            rot = yawed;
        }
    }
    /* 0x0045304F..0x0045307F: the view-cone cut before anything is drawn
     * (x and depth are the origin through the view, FISTP'd to 16.16) */
    {
        const ral_obj_camera *c = &G.ocam;
        float dx = (float)car->pos[0] + c->view_trans.m[12];
        float dy = (float)car->pos[1] + c->view_trans.m[13];
        float dz = (float)car->pos[2] + c->view_trans.m[14];
        float cx = dx * c->view_rot.m[0] + dy * c->view_rot.m[4]
                 + dz * c->view_rot.m[8] + c->view_rot.m[12];
        float cz = dx * c->view_rot.m[2] + dy * c->view_rot.m[6]
                 + dz * c->view_rot.m[10] + c->view_rot.m[14];
        if (wheels_view_cut((int32_t)lrintf(cx), (int32_t)lrintf(cz))) return;
    }
    p.mask1 = parts->mask1 & ~wmask;
    objscene_draw_placed_parts(&objs, OBJ_CAR_TYPE(slot), rot,
                               car->pos, v, &G.ocam, &G.ost, &p);

    depth = ((float)car->pos[0] + G.ocam.view_trans.m[12]) * G.ocam.view_rot.m[2]
          + ((float)car->pos[1] + G.ocam.view_trans.m[13]) * G.ocam.view_rot.m[6]
          + ((float)car->pos[2] + G.ocam.view_trans.m[14]) * G.ocam.view_rot.m[10];
    if (depth >= (float)0x3C0000) return;
    sim_wheel_heights(car, &L.k, pts->y[6], sus);
    wheels_build(m, wg, G.model_drop, (unsigned)car->usteerangle_v_q >> 16,
                 car->wheel_spin_f, car->wheel_spin_r, sus);
    for (i = 0; i < 4; i++) {
        if (!((uint32_t)car->carpartsleft & (0x20u << i))) continue;
        p.mask1 = parts->mask1 & (1u << ((uint32_t)wg->bit[i] & 31u));
        objscene_draw_placed_pre(&objs, OBJ_CAR_TYPE(slot), &m[i],
                                 rot, car->pos, v, &G.ocam, &G.ost, &p);
    }
}

/* render's time by part, microseconds summed over the race line's
 * second.  rt_mark charges the time since the last mark to a part; the
 * stage's own split (walk / sort / submit) is stage_draw's, and inside
 * RT_STAGE.  RT_CARS is render_scene after the stage: the cars and what
 * else it draws. */
enum { RT_PRE, RT_SKY, RT_STAGE, RT_CARS, RT_ENGINE, RT_MIRROR, RT_HUD,
       RT_WALK, RT_SORT, RT_SUBMIT, RT_N };
static uint64_t rt_us[RT_N], rt_last;
static void rt_mark(int part)
{
    uint64_t t = plat_time_us();
    rt_us[part] += t - rt_last;
    rt_last = t;
}


/* the cars' shadows (carshadow.h: a remaster, one soft car-shaped quad
 * per car, leaning away from the level's sun).  Every car the race has -
 * the original's loop (FUN_00455378) takes each car record with its
 * +0x6FC set - drawn in one batched blended call, depth-tested, no depth
 * write, after the stage's ground and before the cars. */
static carshadow_sun CSUN;                 /* the level's sun, LOCALDAT +0x684 / +0x688 */

static int shadow_height(void *ctx, int32_t x, int32_t z, int32_t *y)
{
    sim_ground_hit h;
    (void)ctx;
    if (sim_ground_query(&L.geom[0], x, z, -1, 0x40u, &h) != 1) return 0;
    *y = h.height;
    return 1;
}

/* DIAGNOSTIC, not the original's: draw_car's RALLY_CAR_YAW turns the DRAWN car
 * about its vertical; the shadow follows it so a side-on review frame shows
 * the shadow under the car it is looking at (and not under the heading the
 * simulation has).  Off without the variable: the shadow is the car's own. */
static const car_state *shadow_car_yawed(const car_state *c, car_state *tmp)
{
    static int deg_set = -1;
    static double deg;
    ral_mat4 ry, cm, o;
    int i;
    if (deg_set < 0) {
        const char *e = getenv("RALLY_CAR_YAW");
        deg_set = (e && *e) ? 1 : 0;
        deg = deg_set ? atof(e) : 0.0;
    }
    if (!deg_set) return c;
    *tmp = *c;
    memset(&cm, 0, sizeof cm);
    for (i = 0; i < 12; i++) cm.m[i] = c->matrix[i];
    cm.m[15] = 1.0f;
    ral_obj_rot_y16(&ry, (unsigned)(deg / 360.0 * 65536.0));
    ral_mat4_mul(&o, &ry, &cm);
    memcpy(tmp->matrix, o.m, sizeof tmp->matrix);
    return tmp;
}

static void shadow_add(const car_state *c0, const sim_collision_points *pts)
{
    carshadow_quad q;
    static car_state tmp;
    const car_state *c = shadow_car_yawed(c0, &tmp);
    const ral_obj_camera *o = &G.ocam;
    float dx = (float)c->pos[0] + o->view_trans.m[12];
    float dy = (float)c->pos[1] + o->view_trans.m[13];
    float dz = (float)c->pos[2] + o->view_trans.m[14];
    float depth = dx * o->view_rot.m[2] + dy * o->view_rot.m[6] + dz * o->view_rot.m[10]
                + o->view_rot.m[14];
    if (depth >= (float)0x3C0000) return;       /* draw_car's own distance cut */
    int layer;
    for (layer = 0; layer < CARSHADOW_LAYERS; layer++)   /* the cast first, the contact over it */
        if (carshadow_build(&q, c, pts, shadow_height, NULL, &CSUN, layer)) carshadow_add(&q);
}

static void shadows_draw(const stage_view *v)
{
    static int off = -1;
    plat_gfx_saved saved;
    if (off < 0) off = getenv("RALLY_NO_SHADOW") != NULL;
    if (off || (G.vis_set && !G.vis_shadow)) return;
    carshadow_begin();
    shadow_add(&G.car, &L.pts);
    if (G.ghost && !G.live) {
        const car_state *g = sim_capture_car(&cap, G.frame);
        shadow_add(g, &L.pts);
    }
    if (G.have_arcade) {
        int s;
        for (s = 0; s < rep.nrivals; s++)
            if (rep.rival[s].car.driver != 0)
                shadow_add(&rep.rival[s].car, &rep.rival[s].pts);
    }
    plat_gfx_state_save(&saved);
    plat_gfx_cull_mode(0);
    carshadow_flush(v, G.ocam.view_rot.m, G.ocam.view_trans.m);
    plat_gfx_cull_mode(2);
    plat_gfx_state_restore(&saved);
}

static void render_scene(const stage_view *v, int mirror)
{
    stage_stats st;
    const sim_camera *cam = &G.camx.cam;
    int own_car = 1;

    objscene_camera(&G.ocam, v);
    /* [0x005BA608] / [0x005BA614]: the camera's own yaws, for the yaw
     * billboards - not recovered from the matrix, which pitches now. */
    G.ocam.cam_yaw = (unsigned)cam->yaw;
    G.ocam.cam_extra_yaw = (unsigned)cam->vyaw;
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    /* THE SKY DRAW IS THE CLEAR: FUN_0045343E draws object type 1
     * first, under ZFUNC = ALWAYS with the depth write on, and 0x0044C4BA
     * puts LESSEQUAL back.  Culling off for it: it is seen from inside. */
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_ALWAYS);
    plat_gfx_cull_mode(0);
    objscene_draw_backdrop(&objs, &G.bg, v, &G.ocam, OBJ_BREAK_NONE, NULL);
    if (!mirror && SUN.have) {         /* 0x00453515, the sun's point */
        ral_mat4 dm;
        objscene_backdrop_matrix(&dm, &G.bg, &G.ocam, OBJ_BREAK_NONE);
        sun_sky(&SUN, &dm, v, G.w, G.h);
    }
    if (!mirror) rt_mark(RT_SKY);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_cull_mode(2);            /* the backend's default: CW kept     */
    stage_draw(&stg, v, &st);
    if (!mirror) {
        rt_mark(RT_STAGE);
        rt_us[RT_WALK] += st.us_walk;
        rt_us[RT_SORT] += st.us_sort;
        rt_us[RT_SUBMIT] += st.us_draw;
    }
    shadows_draw(v);
    /* The car, through the object path (objscene_draw_placed says what is
     * and is not transcribed).  G draws the GAME'S car too, at the capture's
     * pose for the same frame - in free-run mode, where the port's car has
     * gone. */
    /* THE STATE THE CAR IS DRAWN UNDER - the port's choice, and a measured
     * one.  stage_draw ends on its depth-sorted, blended material with
     * ZWRITE 0 and BLEND 1 (stage.c's second-layer and sorted arms), and the
     * car drawn under that state paints its polygons in FILE order: the
     * cabin, after the roof in the file, covered the roof on every frame
     * (the missing roof - neither the texture
     * nor the cull was the cause).  The placed objects never see it because
     * they draw from the cell walk, before any list.  So the car goes out
     * under 0x00419F29's own scene-begin values, ZWRITE 1 and no blend.
     * Which state block the original's car draw runs under is not read. */
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_BLEND, 0u);
    /* the viewing car (0x00453AD2..): not drawn in BUMPER VIEW with
     * no blend running, nor in the mirror's pass.  In DRIVER / INTERIOR it
     * is drawn through the part gate (car_parts): the same model,
     * its cabin parts in and its outside-only parts out. */
    if (mirror || (cam->view == 0 && cam->t == 0.0f))
        own_car = 0;
    /* in DRIVER / INTERIOR the viewing car is carstod nibble 0x10
     * which the world's pass leaves to
     * 0x004539C7: drawn AFTER the engine's pass, so the falling rain and
     * snow go behind the cockpit - render_interior. */
    if (cam->view == 2 || cam->view == 3)
        own_car = 0;
    /* DIAGNOSTIC, not the original's: RALLY_CAR_NOCULL draws the car with
     * back faces kept (the missing roof). */
    if (G.car_nocull) plat_gfx_cull_mode(0);
    /* The player's car's ctd_carnum, car+0xAA8 = [0x70CD88] = player+0x28 (0x00445411;
     * not the menu models' slot + 1, 0x00461689); the command line's is 1. */
    if (own_car) {
        objscene_parts parts;
        int32_t state[256];
        car_state_table(state, &G.car, G.racenum, NULL);   /* [0x70CD88] -> ctd_carnum (0x00445411) */
        car_parts_outside(&parts, &G.car, cam, state, 1, car_view_depth(&G.car));
        car_crush_parts(&parts, &G.glass_own, &G.car, 0);
        parts.light = car_light_now(v, &G.car, cam->view == 2 || cam->view == 3);
        draw_car(0, &G.car, &L.pts, v, &parts);
    }
    if (G.car_nocull) plat_gfx_cull_mode(2);
    if (G.ghost && !G.live) {
        const car_state *g = sim_capture_car(&cap, G.frame);
        objscene_parts parts;
        int32_t state[256];
        car_state_table(state, g, 1, NULL);
        car_parts_outside(&parts, g, NULL, state, 0, INT32_MIN);
        car_crush_parts(&parts, &G.glass_ghost, g, 0);
        parts.light = car_light_now(v, g, 0);
        draw_car(0, g, &L.pts, v, &parts);
    }
    /* the arcade's rivals - 0x00453AA7 draws every slot whose
     * driver is not 0 in gmodes 0 and 2, the mirror's pass included; each
     * with its own race number (+0xAA8) and model, and the distance cuts
     * at 0x0045F4C4 (car_distance_cuts). */
    if (G.have_arcade) {
        int s;
        for (s = 0; s < rep.nrivals; s++) {
            const car_state *rv = &rep.rival[s].car;
            objscene_parts parts;
            int32_t state[256];
            if (rv->driver == 0) continue;
            car_state_table(state, rv, A.car_number[s + 1], NULL);
            car_parts_outside(&parts, rv, NULL, state, 0, car_view_depth(rv));
            car_crush_parts(&parts, &G.glass_rival[s], rv, s + 1);
            parts.light = car_light_now(v, rv, 0);
            draw_car(s + 1, rv, &rep.rival[s].pts, v, &parts);
        }
    }
    /* the impact flash's glow, one per slot (FUN_004530F2), after the cars: the original queues it
     * in the depth-sorted list, behind the solid draws (0x00455226) */
    {
        int fi;
        for (fi = 0; fi < v->nflash; fi++)
            if (v->flash[fi].k != 0)
                objscene_draw_glow(&objs, v->flash[fi].pos, v->flash[fi].k << 3,
                                   v, &G.ocam, NULL);
    }
    /* 0x00453426 - the world is drawn; the sun's flag is what is left */
    if (!mirror && SUN.have) sun_world_end(&SUN);
}

static float last_view_m[16];             /* [0x0059F014]           */

/* the cockpit in DRIVER / INTERIOR, 0x004539C7's 0x0045F053 - the
 * same part gate as before, under the states render_scene gives the
 * car. */
static void draw_cockpit(const stage_view *v)
{
    const sim_camera *cam = &G.camx.cam;
    objscene_parts parts;
    int32_t state[256];

    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    plat_gfx_set_state(PLAT_RS_BLEND, 0u);
    plat_gfx_cull_mode(G.car_nocull ? 0 : 2);
    car_state_table(state, &G.car, G.racenum, NULL);
    car_parts(&parts, &G.car, cam);
    parts.state = state;
    car_crush_parts(&parts, &G.glass_own, &G.car, 0);
    parts.light = car_light_now(v, &G.car, 1);
    objscene_draw_placed_parts(&objs, OBJ_CAR_TYPE(0), G.car.matrix,
                               G.car.pos, v, &G.ocam, &G.ost, &parts);
    plat_gfx_cull_mode(2);
}

/* FUN_00454419's weather inputs (weatherfx.h's wfx_frame) */
static void wfx_frame_of(wfx_frame *f, const stage_view *v, ral_mat4 *mv)
{
    const sim_camera *cam = &G.camx.cam;
    ral_mat4 cur, at, atv, placed;
    int32_t state[256];
    uint8_t lamp[17];
    int r, c;

    memset(f, 0, sizeof *f);
    f->rain = wx_now.rain;
    f->snow = wx_now.snow;
    /* [0x005F44A0], 0x0045228C: player 1's car+0x6F0, when LD+0x690 or
     * +0x694 is set */
    f->inside = (cam->view == 2 || cam->view == 3) && G.wet_level;
    f->advance = wfx_advance;
    f->now_ms = wfx_clock;
    car_state_table(state, &G.car, G.racenum, lamp);
    f->lights = car_lamp_abc(&G.car, lamp) >= 16;   /* 0x00454454 */
    /* [0x005F44A4] = car+0x240 (0x004539FA -> 0x0045380A), saturating-added
     * to [0x005E0954], the local lights - not ported, as for the car's own
     * draw, so 0 */
    f->tint = G.car.ground_col;
    f->v = v;
    f->view_rot = G.ocam.view_rot.m;
    for (r = 0; r < 3; r++)
        f->cam[r] = (int32_t)lrintf(-G.ocam.view_trans.m[12 + r]);
    /* [0x0059F054]: the car's model -> view matrix, built as the car's own
     * draw builds it (objscene_draw_placed_parts) */
    memset(&cur, 0, sizeof cur);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            cur.m[r * 4 + c] = G.car.matrix[r * 4 + c];
    cur.m[15] = 1.0f;
    ral_obj_translate_i(&at, G.car.pos[0], G.car.pos[1], G.car.pos[2]);
    ral_mat4_copy_add_t(&atv, &at, &G.ocam.view_trans);
    ral_mat4_copy_add_t(&placed, &cur, &atv);
    ral_mat4_mul(mv, &placed, &G.ocam.view_rot);
    f->car_mv = mv->m;
    f->car_pos[0] = G.car.pos[0];
    f->car_pos[1] = G.car.pos[1];
    f->car_pos[2] = G.car.pos[2];
    /* [0x0059F114] is the view car's car+0x8C0, unread; its orientation
     * matrix car+0xB4 stands in (TENTATIVE: a render copy of it) */
    f->car_rot = G.car.matrix;
    f->screen_w = G.w;
    f->screen_h = G.h;
}

/* FUN_004547E1's copy of one car into its engine object (spray.h's
 * spray_car_in).  car+0x8C0 is built as the car's draw builds [0x0059F114]
 *its matrix, its 16.16 position as floats (TENTATIVE, as weatherfx's
 * car_rot); car+0x740's row 0, the only one the spray reads, is 0x00425C3A's
 * (cos s, 0, -sin s), s = car+0x650 >> 16 (0x0045ECC5), the spin about x
 * before it leaving row 0 alone.  The reset countdown car+0xFAC is counted
 * down here, as 0x004547F3 does. */
static void spray_in_car(spray_car_in *in, const car_state *c,
                         const sim_world *w, int slot)
{
    uint32_t a = (((uint32_t)c->usteerangle_v_q >> 16) & 0xFFFFu) >> 4;
    int r, k;

    memset(in, 0, sizeof *in);
    in->live = slot == 0 || c->driver != 0;
    in->model = c->carnum1 == 6 ? 11 : c->carnum1;      /* FUN_00454CEC */
    in->revs = c->revs;
    in->ground_col = c->ground_col;
    for (r = 0; r < 12; r++) in->body[r] = c->matrix[r];
    for (k = 0; k < 3; k++) in->body[12 + k] = (float)c->pos[k];
    in->body[15] = 1.0f;
    in->front[0] = L.sin_tab[a + 0x400u];
    in->front[2] = -L.sin_tab[a];
    in->front[5] = in->front[10] = in->front[15] = 1.0f;
    if (rep.have_race && slot < SIM_ROSTER_SLOTS && rep.race.fac_q[slot] != 0) {
        in->reset = rep.race.fac_q[slot];
        rep.race.fac_q[slot]--;
    }
    in->smoke_a = c->temps[5];
    in->smoke_b = c->dam[0];
    in->fwhlspd = c->fwhlspd;
    in->rwhlspd = c->rwhlspd;
    in->cxspd = c->cxspd;
    in->czspd = c->czspd;
    in->vel_x = c->newspd[0];
    in->vel_z = c->newspd[2];
    for (k = 0; k < 4; k++) in->susp[k] = c->susp[k];
    in->partsleft = c->carpartsleft;
    for (k = 0; k < 5; k++) {
        in->cls_a[k] = w->surf.cls_a[k];
        in->cls_b[k] = w->surf.cls_b[k];
        in->aux18_a[k] = w->surf.aux18_a[k];
        in->aux18_b[k] = w->surf.aux18_b[k];
        in->aux14_a[k] = w->surf.aux14_a[k];
        in->aux14_b[k] = w->surf.aux14_b[k];
        in->weight[k] = w->surf.weight[k];
    }
    in->body_contact = w->body_contact != 0;
    for (k = 0; k < 3; k++) in->contact[k] = w->body_contact_16_16[k];
    in->contact_vt = w->body_contact_vt;
    /* RALLY_SPARKS: a DIAGNOSTIC, not the original's - the body
     * scrapes a tarmac class (1) with the sim's contact point replaced by one
     * two metres to the car's side, so the sparks and their streaks can be
     * looked at on any drive.  Car 0 only; it changes nothing the sim reads. */
    if (slot == 0 && env_flag("RALLY_SPARKS", &env_sparks)) {
        in->body_contact = 1;
        in->contact[0] = (int32_t)c->pos[0] + (int32_t)(131072.0f * c->matrix[0]);
        in->contact[1] = (int32_t)c->pos[1] - 0x8000;
        in->contact[2] = (int32_t)c->pos[2] + (int32_t)(131072.0f * c->matrix[2]);
        in->contact_vt = 20.0f;
        in->cls_a[4] = 1;
        in->aux18_a[4] = 0xFF;
        in->weight[4] = 0.0f;
    }
}

/* at exit, what the spray did - per type emitted, the deaths, the
 * draws past the near plane (a run's census, the live gate's evidence) */
static void spray_report(void)
{
    int t;
    if (!have_spray) return;
    printf("rally: the spray: emitted");
    for (t = 0; t < 0x20; t++)
        if (SP.emitted[t]) printf(" 0x%X:%lu", t, SP.emitted[t]);
    printf("; died %lu; drawn %lu; refused full %lu; arms (wheels) by class:",
           SP.died, SP.drawn, SP.full);
    for (t = 0; t < 13; t++)
        if (SP.arm[0][t]) printf(" %d:%lu", t, SP.arm[0][t]);
    printf("\n");
}

/* 0x004B9734 and 0x004BB1F7 in the engine's draw pass, after the
 * falling weather: the clock, every car's snapshot, the emitters, then each
 * particle's step and draw. */
static void spray_frame(const stage_view *v)
{
    spray_car_in in;
    spray_view sv;
    plat_gfx_saved saved;
    int32_t dt = (int32_t)(wfx_clock - spray_last_ms);
    int n = 1, s;

    if (!wfx_advance || dt < 0) dt = 0;
    spray_last_ms = wfx_clock;
    spray_clock(&SP, dt, wx_now.rain, wfx_advance);
    spray_in_car(&in, &G.car, &SW0, 0);
    spray_car(&SP, 0, &in);
    if (G.have_arcade)
        for (s = 0; s < rep.nrivals && s + 1 < SPRAY_CARS; s++, n++) {
            spray_in_car(&in, &rep.rival[s].car, &rep.rival[s].w, s + 1);
            spray_car(&SP, s + 1, &in);
        }
    spray_view_matrix(&SP, G.ocam.view_rot.m, G.ocam.view_trans.m);
    spray_emit(&SP, n, wfx_advance);
    sv.v = v;
    plat_gfx_state_save(&saved);
    spray_draw_frame(&SP, &sv);
    plat_gfx_state_restore(&saved);
}

/* 0x004BF796 in the frame, after the spray - the pool's
 * step and every quad.  The view car is the finish's one car; its position is
 * [0x0059F144..4C], car+0x8C0's translation as floats (as spray_in_car). */
static void fireworks_render(const stage_view *v)
{
    fireworks_in in;
    plat_gfx_saved saved;
    int32_t dt = (int32_t)(wfx_clock - fw_last_ms);
    int k;

    if (!wfx_advance || dt < 0) dt = 0;
    fw_last_ms = wfx_clock;
    memset(&in, 0, sizeof in);
    in.on = fw_on;
    in.dt = dt;
    fireworks_view_matrix(in.view, G.ocam.view_rot.m, G.ocam.view_trans.m);
    for (k = 0; k < 3; k++) in.car[k] = (float)G.car.pos[k];
    plat_gfx_state_save(&saved);
    fireworks_draw_frame(&FW, &in, v, have_spray ? SP.atlas : 0u,
                         have_spray ? &SP.aspect[88 * 9 + 1] : NULL);
    plat_gfx_state_restore(&saved);
}

/* the wheel and the hands, and the one draw from the shared
 * generator.  The pose goes into the dented copy the way the blades' does. */
static void pose_driver(int32_t dt, uint32_t *gen)
{
    driver_snapshot(&DP, G.car.usteerangle_v_q, G.camx.shake[3],
                    G.car.handbrake_ramp != 0, &G.car.gear_changed_q);
    driver_step(&DP, &objs.type[OBJ_CAR_TYPE(0)].m, dt, gen);
    if (DP.have && G.glass_own.pts) {
        const ro3_model *dm = &objs.type[OBJ_CAR_TYPE(0)].m;
        int g, e;
        for (g = 0; g < DRIVER_PARTS; g++)
            for (e = 0; e < DP.p[g].n; e++) {
                size_t id = (size_t)DP.p[g].id[e];
                if (id < G.glass_own.npts && id < dm->npoints)
                    G.glass_own.pts[id] = dm->points[id];
            }
    }
}

/* 0x004B4ACE, the lens flare - in the engine's additive pass, after
 * the spray and before the fireworks; the state is put back as the others do. */
static void sun_flare_pass(const stage_view *v)
{
    plat_gfx_saved saved;

    /* the texture is [0x00586B58] as the pass before leaves it: id 0xCA, the
     * engine's effects atlas, plate 900 (TX0900.PCX), the weather's - the
     * flare's records 0x40..0x49 are its right half's top rows, not the
     * spray's surfdata atlas.  The sizes come from the spray's table of
     * TEXTINFO records (0x004B604C), which is why it is needed too. */
    if (!SUN.have || !SUN.option || SUN.level <= 0 || !have_spray || !have_wfx
        || !WX.atlas) return;
    plat_gfx_state_save(&saved);
    sun_flare_draw(&SUN, WX.atlas, SP.aspect, SP.near_bits,
                   stage_depth_scale(v->radius, v->detail));
    plat_gfx_state_restore(&saved);
}

/* 0x00453905, the glare over the finished scene, the option's
 * switch not applying (sun.h). */
static void sun_glare_pass(void)
{
    plat_gfx_saved saved;

    if (!SUN.have || SUN.glare_alpha <= 0) return;
    plat_gfx_state_save(&saved);
    sun_glare_draw(&SUN, G.w, G.h);
    plat_gfx_state_restore(&saved);
}

/* 0x00453C27 + 0x004538A4 + 0x004539C7, for the main view */
static void render_engine(const stage_view *v)
{
    const sim_camera *cam = &G.camx.cam;
    int interior = cam->view == 2 || cam->view == 3;
    wfx_frame f;
    ral_mat4 mv;

    if (!G.video) {
        if (have_fw) fireworks_render(v);        /* the step and the sounds  */
        return;
    }
    if (have_wfx) {
        plat_gfx_saved saved;
        wfx_frame_of(&f, v, &mv);
        /* THE PORT'S CHOICE: the state the engine leaves is put back, so
         * the cockpit, the mirror and the HUD draw as they did before
         * (the original lets it carry; which of its states the later draws
         * re-state is not read) */
        plat_gfx_state_save(&saved);
        wfx_scene_copy(&WX, &f);                 /* 0x00453C27            */
        wfx_engine_tick(&WX, &f);                /* 0x004C106B            */
        /* 0x004C106B -> 0x004B0D4C -> 0x004ADB8F: block +0x274C is 0 when
         * (rain || snow) && inside */
        wipers_update(&WP, &objs.type[OBJ_CAR_TYPE(0)].m, (int32_t)f.now_ms,
                      WX.dt, (wx_now.rain != 0 || wx_now.snow != 0) && f.inside);
        /* The draw takes the dented copy when there is one, and it
         * is rebuilt only when the damage moves: the blades go into it too.
         * The original has one buffer, [0x0057C224], which the crush pass
         * and the pose (0x004AD2B6) both write, so the pose wins over a dent
         * of a blade vertex as it does here. */
        if (WP.have && G.glass_own.pts) {
            const ro3_model *wm = &objs.type[OBJ_CAR_TYPE(0)].m;
            int k, e;
            for (k = 0; k < 2; k++)
                for (e = 0; e < WP.b[k].n; e++) {
                    size_t id = (size_t)WP.b[k].id[e];
                    if (id < G.glass_own.npts && id < wm->npoints)
                        G.glass_own.pts[id] = wm->points[id];
                }
        }
        /* 0x004B0E7E, right after the wipers' 0x004B0D4C - and before
         * the falling weather's draws, which share the generator */
        pose_driver(WX.dt, &WX.lcg);
        if (wiper_dump && WP.have) {
            int k, e;
            fprintf(wiper_dump, "u %lu clock %u dt %d run %d phase %d acc %d step %d %d stroke %d %d running %d\n",
                    WP.poses, (unsigned)f.now_ms, (int)WX.dt,
                    (wx_now.rain != 0 || wx_now.snow != 0) && f.inside,
                    (int)WP.phase, (int)WP.acc, (int)WP.step[0], (int)WP.step[1],
                    (int)WP.stroke[0], (int)WP.stroke[1], (int)WP.running);
            for (k = 0; k < 2; k++)
                for (e = 0; e < WP.b[k].n; e++) {
                    const ro3_vec *q = &objs.type[OBJ_CAR_TYPE(0)].m.points[WP.b[k].id[e]];
                    fprintf(wiper_dump, "v %d %d %d %ld %ld %ld\n", k, (int)WP.b[k].id[e],
                            e, lrint(q->x * 65536.0), lrint(q->y * 65536.0),
                            lrint(q->z * 65536.0));
                }
        }
        if (!wfx_no_falling)
            wfx_draw_falling(&WX, &f);           /* 0x004C1100            */
        plat_gfx_state_restore(&saved);
    } else {
        /* no weather: the engine's clock (wfx_engine_tick's) is the driver's */
        int32_t d = (int32_t)(wfx_clock - drv_last_ms);
        if (drv_dt < 0 || !wfx_advance) d = 0;
        drv_dt = d;
        drv_last_ms = wfx_clock;
        pose_driver(drv_dt, &drv_gen);
    }
    if (have_spray)
        spray_frame(v);                          /* 0x004C11C0..D1 */
    sun_flare_pass(v);                           /* 0x004C120B     */
    if (have_fw)
        fireworks_render(v);                     /* 0x004C1224     */
    if (!interior) {
        sun_glare_pass();                        /* 0x00453905     */
        return;
    }
    /* 0x004539C7: the depth cleared when car+0x900 / +0x904 is set (or
     * [0x005E7EA4], zeroed every frame at 0x004536B6 and written nowhere
     * by address: 0 here), then the cockpit, then 0x004B7106 */
    if (wx_now.rain != 0 || wx_now.snow != 0)
        plat_gfx_clear_depth();
    draw_cockpit(v);
    if (have_wfx) {
        plat_gfx_saved saved;
        plat_gfx_state_save(&saved);
        wfx_draw_screen(&WX, &f);
        plat_gfx_state_restore(&saved);
    }
    sun_glare_pass();                            /* 0x00453905     */
}

/* THE VIEWER'S CONTROL STRIP - the port's own (the user's call: a strip
 * drawn in the menus' manner, not the original's panel with its editor).  A bar
 * over the picture's foot: the state, the time against the whole, the camera, a
 * progress track, and the keys.  Drawn on a 640 x 480 surface, scaled to the
 * window like the menus' layer. */
static uint32_t strip_px[640 * 480];
static int      view_paused;

static void strip_rect(int x0, int y0, int x1, int y1, uint32_t argb)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) strip_px[y * 640 + x] = argb;
}

static void view_strip(void)
{
    static gfx2d_surface s;
    static int init;
    char l[96], r[48];
    long a = (long)G.car.alivet, total = (long)(vrd.nchunks * RPL_FRAMES);
    int x, w, tw;
    if (!init) { gfx2d_surface_init(&s, strip_px, 640, 480); init = 1; }
    if (a < 0) a = 0;
    if (a > total) a = total;
    gfx2d_clear(&s, 0x00000000u);
    strip_rect(130, 424, 510, 468, 0xB0000000u);                     /* the bar     */
    strip_rect(138, 430, 502, 434, 0xFF303030u);                      /* the track   */
    w = total > 0 ? (int)((364L * a) / total) : 0;
    strip_rect(138, 430, 138 + w, 434, 0xFFE0A000u);                  /* the played  */
    for (x = 1; x < (int)vrd.nchunks && vrd.nchunks <= 120; x++)     /* a tick a second */
        strip_rect(138 + (int)((364L * x) / (long)vrd.nchunks), 430, 139 + (int)((364L * x) / (long)vrd.nchunks), 434, 0xFF707070u);
    snprintf(l, sizeof l, "%s  %ld:%02ld / %ld:%02ld", a >= total ? "END" : view_paused ? "PAUSED" : "PLAYING",
             a / 6000, a / 100 % 60, total / 6000, total / 100 % 60);
    snprintf(r, sizeof r, "VIEW %d", (int)G.camx.cam.nview);
    hud_text(&G.hud, &s, l, 138, 438);
    tw = hud_text_width(&G.hud, &s, r);
    hud_text(&G.hud, &s, r, 502 - tw, 438);
    hud_text(&G.hud, &s, "SPACE PAUSE  ARROWS SEEK  F1 VIEW  ESC", 138, 452);
    gfx2d_present_over(&s, G.w, G.h);
}

static void render(void)
{
    stage_view v;

    rt_last = plat_time_us();
    /* FUN_004521B6's HUD part, before the scene - the message pickup
     * and the HUD block.  It runs with or without a picture: the frame
     * picking a message up is what lets msgtimer count. */
    if (G.have_hud && !G.finish) {     /* 0x004523BF: not in the finish */
        hud_map_frame(&G.hud, &G.car, rep.binary_angle[0]);   
        long before = G.hud.pickups;
        hud_frame(&G.hud, &G.car, rep.have_race ? &rep.race : NULL, 0);
        /* which message reached the banner, for a log and the
         * warn-live gate (the damage warnings are ids 207..267) */
        if (G.hud.pickups != before)
            printf("rally: message %d picked up at step %ld\n",
                   (int)G.hud.shown_id, G.steps_run);
    }

    /* The fog ramp's noise blend is sampled at car 0's world position,
     * [0x006CD308] / [0x006CD310] (stage.h, FUN_004365A6) - so it is rebuilt
     * where the car is.  THE PORT'S READING: when the original rebuilds it
     * has not been read; on 44 of 48 levels the blend changes nothing. */
    stage_fog_build(&fog, &stg.ld, stg.have_ld, G.radius,
                    G.car.pos[0], G.car.pos[2]);
    /* the original's camera, 0x00436D04 / 0x00436E4C (camview.h). */
    camview_main(&v, G.w, G.h, &G.camx.cam, &G.car, &G.camk, NULL, NULL,
                 rep.have_race ? rep.race.snd_view.rot : NULL);
    v.radius = G.radius;
    v.fog = &fog;
    if (!G.day_ramp) stage_view_shade_ld(&v, &stg.ld, stg.have_ld);
    stage_lit_all(&v);
    v.cull_all_corners = G.patch_cull;      /* opt-in */
    {   /* DIAGNOSTIC: RALLY_NO_BACKS=1 draws no two-sided backs */
        static int no_backs = -1;
        if (no_backs < 0) no_backs = getenv("RALLY_NO_BACKS") != NULL;
        v.disable_back_pass = no_backs;
    }
    /* [0x006C2118], the animation time base, is the frame counter - the same
     * global FUN_0045DB0B divides by 100 for the replay's chunk index. */
    v.time_base = (int32_t)G.frame;
    objscene_stats_init(&G.ost);

    plat_video_frame_begin();
    plat_video_scene_begin();
    rt_mark(RT_PRE);
    if (SUN.have) {                    /* car+0xFBC's ramp (the sim tick's) */
        sun_tick(&SUN, (unsigned long)G.frame);
        sun_frame_begin(&SUN);
    }
    render_scene(&v, 0);
    rt_mark(RT_CARS);
    /* the ambience hears from [0x0059F014] as the LAST view left it */
    memcpy(last_view_m, v.m, sizeof last_view_m);
    /* 0x00453C27, then 0x004538A4 - the engine's tick and its draw
     * pass (the falling rain and snow), then 0x004539C7, the cockpit and
     * the windscreen.  All before the mirror, which reaches none of it. */
    render_engine(&v);
    rt_mark(RT_ENGINE);
    /* the rear-view mirror, car+0x220 bit 0x08 (F8) - 0x00452713:
     * a second frame into the inset, draw radius 2, fishval 256. */
    {
        uint32_t mask;
        memcpy(&mask, (const unsigned char *)&G.car + 0x220, 4);
        if ((mask & 0x08u) && !G.finish) {     /* 0x00452715 */
            stage_view mv;
            int r[4];
            /* built (and its rotation left in [0x0059EF94] for the
             * sounds) whether or not there is a picture to draw it in */
            camview_mirror(&mv, G.w, G.h, &G.car, &G.camk, G.camx.layout, 0,
                           (mask & 0x40u) != 0, r,
                           rep.have_race ? rep.race.snd_view.rot : NULL);
            memcpy(last_view_m, mv.m, sizeof last_view_m);
            if (G.video) {
            mv.radius = 2;                 /* [0x005BD538] = 2, 0x00452750 */
            mv.fog = &fog;
            if (!G.day_ramp) stage_view_shade_ld(&mv, &stg.ld, stg.have_ld);
            stage_lit_all(&mv);
            mv.cull_all_corners = G.patch_cull;
            mv.time_base = (int32_t)G.frame;
            plat_gfx_inset(r[0], r[1], r[2], r[3]);
            render_scene(&mv, 1);
            plat_gfx_inset(0, 0, 0, 0);
            }
        }
    }
    rt_mark(RT_MIRROR);
    /* 0x00452658 / 0x0045265D, after both views: the stage's sound
     * objects about the eye, from the grid the last DRAWN view left (the
     * mirror's is drawn only with a picture), at the main view's radius -
     * [0x005BD538] restored. */
    if (have_world && rep.have_race)
        snd_world_collect(&W, &stg, G.radius, last_view_m);
    /* the HUD (hud.h), FUN_00458619, over the scene.  the
     * stand-in text~~.  The render state it sets is put back afterwards,
     * exactly: objects inherit state across frames (section 6), and the HUD must not change the 3D picture.  RALLY_NO_HUD
     * leaves the DRAW out (the frame logic above still runs), which is how
     * that is checked. */
    if (G.video && G.have_hud && !G.no_hud && !G.finish) {   /* 0x0045269A */
        plat_gfx_saved saved;
        plat_gfx_state_save(&saved);
        hud_draw(&G.hud, rep.have_race ? &rep.race : NULL, 0, G.race_over,
                 G.w, G.h);
        plat_gfx_state_restore(&saved);
    }
    if (G.video && view_on && G.have_hud) {   /* the viewer's strip */
        plat_gfx_saved saved;
        plat_gfx_state_save(&saved);
        view_strip();
        plat_gfx_state_restore(&saved);
    }
    if (have_wfx) wfx_frame_end(&WX);      /* 0x004590B5's upload           */
    plat_video_scene_end();
    rt_mark(RT_HUD);
}

static int write_ppm(const char *path)
{
    static unsigned char *rgba, *rgb;
    FILE *f;
    int i;

    if (!G.video) return -1;
    if (!rgba) {
        rgba = (unsigned char *)malloc((size_t)G.w * G.h * 4);
        rgb  = (unsigned char *)malloc((size_t)G.w * G.h * 3);
        if (!rgba || !rgb) return -1;
    }
    if (!plat_gfx_read_frame(rgba, G.w, G.h)) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", G.w, G.h);
    /* GL reads bottom-up; PPM is top-down. */
    for (i = 0; i < G.w * G.h; i++) {
        int x = i % G.w, y = i / G.w;
        const unsigned char *s = &rgba[((G.h - 1 - y) * G.w + x) * 4];
        rgb[i * 3 + 0] = s[0];
        rgb[i * 3 + 1] = s[1];
        rgb[i * 3 + 2] = s[2];
    }
    fwrite(rgb, 1, (size_t)G.w * G.h * 3, f);
    fclose(f);
    printf("rally: frame %ld written to %s\n", G.frame, path);
    return 0;
}

/* ---- loading ------------------------------------------------------------- */

/* THE PORT'S: one car type's plates in one texture, so its ~300
 * texture-change flushes a frame (RG40XX-H, ~30 us each) go.  Cells of 256
 * texels with a 16-texel WRAP gutter (the TEXADDRESS the car draws under is
 * wrap, so a bilinear tap past an edge reads the opposite edge, as it did),
 * 7 x 3 cells in 2048 x 1024.  Levels 0-4 are the plates' own chains, the
 * gutter halving with them; from level 5, where a gutter would be under a
 * texel, the atlas's own 2x2 box - the far mips of a car that is never far.
 *
 * when any of the car's plates is an "hd" override (512, plate.scale
 * 2) the whole atlas is twice the size - 512 cells, a 32-texel gutter, 4096
 * x 2048 - and has one level more of its own (0-5).  A shipped 256 plate on
 * such a car fills its cell doubled, nearest, at level 0 and with its own
 * chain from level 1, so it draws as it did.  The atlas then takes the
 * overrides' filtering (plat_gfx_tex_smooth). */
#define CA_W 2048u
#define CA_H 1024u
#define CA_G 16u
#define CA_CELL (256u + 2u * CA_G)
static void car_atlas_build(int type, const char *files, const plate_grade *g,
                            int tyre)
{
    objscene_type *ty = &objs.type[type];
    static plate pl;
    uint32_t *lv[13];
    uint32_t *chain[21];                 /* each plate's levels 0-5, RGBA   */
    unsigned  cs[21];                    /* ... and its scale, 0 = left out */
    unsigned lw[13], lh[13];
    float u0[OBJ_TEXIDS_MAX], v0[OBJ_TEXIDS_MAX];
    int ok[OBJ_TEXIDS_MAX];
    unsigned k, i, nl = 0, w, h, sc = 1u, own, aw0, ah0;
    char err[256];

    if (!ty->have || ty->ntexid == 0 || ty->ntexid > 21u) return;
    for (k = 0; k < ty->ntexid; k++) {
        unsigned ps, n, off = 0, keep;
        chain[k] = NULL;
        cs[k] = 0;
        /* the gravel override, FUN_00424462 0x004245c0 - a car whose
         * tyre is >= 3 gets its livery sheet with the rally wheels */
        if (plate_load_tyre(&pl, files, ty->texid[k], 1, g, tyre, err,
                            sizeof err) != 0
            || pl.scale < 1u || pl.scale > 2u
            || pl.w != 256u * pl.scale || pl.h != 256u * pl.scale
            || pl.levels < 4u + pl.scale)
            continue;
        ps = pl.w;
        keep = 4u + pl.scale;                   /* down to 16, the gutter's 1 */
        for (i = 0, n = 0; i < keep; i++) n += (ps >> i) * (ps >> i);
        chain[k] = (uint32_t *)malloc((size_t)n * 4u);
        if (!chain[k]) continue;
        for (i = 0; i < keep; i++) {
            unsigned m = (ps >> i) * (ps >> i), j;
            for (j = 0; j < m; j++) {
                uint32_t c = pl.level[i][j];
                chain[k][off + j] = (c & 0xff00ff00u)
                    | ((c & 0x00ff0000u) >> 16) | ((c & 0x000000ffu) << 16);
            }
            off += m;
        }
        cs[k] = pl.scale;
        if (pl.scale > sc) sc = pl.scale;
    }
    own = 4u + sc;                       /* levels copied from the plates */
    aw0 = CA_W * sc;
    ah0 = CA_H * sc;
    for (w = aw0, h = ah0; ; ) {
        lv[nl] = (uint32_t *)calloc((size_t)w * h, 4);
        if (!lv[nl]) {
            while (nl) free(lv[--nl]);
            for (k = 0; k < ty->ntexid; k++) free(chain[k]);
            return;
        }
        lw[nl] = w; lh[nl] = h;
        nl++;
        if (w == 1u && h == 1u) break;
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
    }
    for (k = 0; k < ty->ntexid; k++) {
        unsigned cell = CA_CELL * sc, gut = CA_G * sc;
        unsigned cx = (k % 7u) * cell, cy = (k / 7u) * cell;
        ok[k] = 0;
        u0[k] = (float)(cx + gut) / (float)aw0;
        v0[k] = (float)(cy + gut) / (float)ah0;
        if (!cs[k]) continue;
        ok[k] = 1;
        for (i = 0; i < own; i++) {
            /* the atlas level i is the plate's level i - (sc - cs): a 256
             * plate in a 512 atlas is one level behind, and doubled at 0 */
            unsigned pi = i + cs[k] >= sc ? i + cs[k] - sc : 0u;
            unsigned dup = i + cs[k] >= sc ? 1u : sc / cs[k];
            unsigned off = 0, j;
            unsigned ps = (256u * sc) >> i, gs = gut >> i, x, y;
            unsigned aw = aw0 >> i, ox = cx >> i, oy = cy >> i;
            const uint32_t *src;
            for (j = 0; j < pi; j++)
                off += ((256u * cs[k]) >> j) * ((256u * cs[k]) >> j);
            src = chain[k] + off;
            for (y = 0; y < ps + 2u * gs; y++) {
                unsigned sy = ((y + ps - gs) % ps) / dup;
                for (x = 0; x < ps + 2u * gs; x++) {
                    unsigned sx = ((x + ps - gs) % ps) / dup;
                    lv[i][(oy + y) * aw + ox + x] = src[sy * (ps / dup) + sx];
                }
            }
        }
    }
    for (k = 0; k < ty->ntexid; k++) free(chain[k]);
    for (i = own; i < nl; i++) {                 /* the 2x2 box */
        unsigned pw = lw[i - 1u], ph = lh[i - 1u], aw = lw[i], ah = lh[i];
        unsigned x, y, c;
        for (y = 0; y < ah; y++)
            for (x = 0; x < aw; x++) {
                uint32_t o = 0;
                unsigned x0 = pw > 1u ? 2u * x : 0u, x1 = pw > 1u ? 2u * x + 1u : 0u;
                unsigned y0 = ph > 1u ? 2u * y : 0u, y1 = ph > 1u ? 2u * y + 1u : 0u;
                uint32_t p4[4];
                p4[0] = lv[i - 1u][y0 * pw + x0];
                p4[1] = lv[i - 1u][y0 * pw + x1];
                p4[2] = lv[i - 1u][y1 * pw + x0];
                p4[3] = lv[i - 1u][y1 * pw + x1];
                for (c = 0; c < 32u; c += 8u) {
                    unsigned sum = ((p4[0] >> c) & 0xffu) + ((p4[1] >> c) & 0xffu)
                                 + ((p4[2] >> c) & 0xffu) + ((p4[3] >> c) & 0xffu);
                    o |= (uint32_t)((sum + 2u) / 4u) << c;
                }
                lv[i][y * aw + x] = o;
            }
    }
    {
        plat_tex t = plat_gfx_tex_create(aw0, ah0, (const uint32_t *const *)lv, nl);
        if (sc > 1u) plat_gfx_tex_smooth(t, nl);
        objscene_set_atlas(&objs, type, t, u0, v0, 256.0f / (float)CA_W,
                           256.0f / (float)CA_H, ok);
        printf("rally: car atlas (--no-car-atlas to leave out): type %d, %u plates, %u levels, %ux%u\n",
               type, ty->ntexid, nl, aw0, ah0);
    }
    while (nl) free(lv[--nl]);
}

static void load(const options *o)
{
    static vfs fs;
    char  err[256], p_cdat[1024], p_ld[1024], p_ti[1024], p_b[1024],
          p_files[1024], p_img[1024], rel[256];
    int   rc, k;
    uint32_t model;

    vfs_init(&fs, o->assets, NULL);

    /* ---- the files, every one resolved and checked before anything loads */
    if (o->image) {
        snprintf(p_img, sizeof p_img, "%s", o->image);
        if (!plat_file_exists(p_img)) die_missing("the game image", p_img);
    } else if (vfs_resolve(&fs, "RAL.EXE", p_img, sizeof p_img) != 0) {
        die_missing("the game image (or give --image)", p_img);
    }
    snprintf(rel, sizeof rel, "files\\levels\\%s\\cdat00c.sq", o->stage);
    if (vfs_resolve(&fs, rel, p_cdat, sizeof p_cdat) != 0) {
        snprintf(rel, sizeof rel, "files\\levels\\%s\\cdat00c", o->stage);
        if (vfs_resolve(&fs, rel, p_cdat, sizeof p_cdat) != 0)
            die_missing("the stage payload", p_cdat);
    }
    snprintf(rel, sizeof rel, "files\\levels\\%s\\localdat", o->stage);
    if (vfs_resolve(&fs, rel, p_ld, sizeof p_ld) != 0)
        die_missing("the stage's LOCALDAT", p_ld);
    if (vfs_resolve(&fs, "files\\a\\textinfo", p_ti, sizeof p_ti) != 0)
        die_missing("TEXTINFO", p_ti);
    if (vfs_resolve_dir(&fs, "files\\b", p_b, sizeof p_b) != 0)
        die_missing("the car directory", p_b);
    if (vfs_resolve_dir(&fs, "files", p_files, sizeof p_files) != 0)
        die_missing("the files directory", p_files);
    if (o->replay && !plat_file_exists(o->replay))
        die_missing("the replay capture", o->replay);

    /* ---- the simulation: simlevel.c, shared with trace_check ---------- */
    sim_level_init(&L);
    if (sim_level_load_image(&L, p_img, err, sizeof err) != 0) {
        fprintf(stderr, "rally: %s: %s\n", p_img, err);
        exit(1);
    }
    rc = sim_level_load_stage(&L, p_cdat, p_ld, p_ti, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "rally: the stage will not load (%d): %s\n", rc, err);
        exit(1);
    }
    rc = sim_level_load_objects(&L, p_files);
    if (rc != 0) {
        fprintf(stderr, "rally: the stage's objects will not load from %s (%d)\n",
                p_files, rc);
        exit(1);
    }
    if (o->replay) {
        rc = sim_capture_load(&cap, o->replay);
        if (rc != 0 || cap.frames < 2) {
            fprintf(stderr, "rally: %s is not a state-dump capture (%d)\n",
                    o->replay, rc);
            exit(1);
        }
        if (cap.seq_bad)
            fprintf(stderr, "rally: WARNING - %ld frame indices in %s are not "
                            "0..N-1\n", cap.seq_bad, o->replay);
        G.have_cap = 1;
        /* The car the capture was driven in selects its own handling
         * record, read out of frame 0 as trace_check reads it. */
        G.carnum = sim_capture_car(&cap, 0)->carnum1;
    } else {
        /* no capture - the car the player asked for, on the stage's
         * own route for course direction --direction. */
        G.carnum = (int32_t)o->car;
        G.seed_lfsr = (uint32_t)o->seed;
        /* [0x006CE08C] is also the object filter's route variant. */
        L.objs.route_variant = (int32_t)o->direction;
        if (sim_level_build_route(&L, (int32_t)o->direction) != 0
            || L.route.n < 2) {
            fprintf(stderr, "rally: stage %s has no route for direction %ld "
                            "- nowhere to start a car\n", o->stage,
                    o->direction);
            exit(1);
        }
    }
    G.p = car_params_for(&L.tab, G.carnum);
    rc = sim_level_load_car_points(&L, p_b, G.carnum, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "rally: the car's collision model %s will not load "
                        "(%d)\n", err, rc);
        exit(1);
    }
    if (G.have_cap)
        printf("rally: stage %s, %d polygons; capture %s, %ld frames; car %d "
               "(%s), collision model %d\n", o->stage, (int)L.pl.npols,
               o->replay, cap.frames, (int)G.carnum, G.p->name,
               (int)L.collision_model);
    else
        printf("rally: stage %s, %d polygons; NO capture - the car starts on "
               "the stage's start line (route of %d markers, texture %d); car "
               "%d (%s), collision model %d; the car's setup is %s\n",
               o->stage, (int)L.pl.npols, (int)L.route.n, (int)L.route.tex,
               (int)G.carnum, G.p->name, (int)L.collision_model,
               (G.load_gmode == 0 || G.load_gmode == 2)
                   ? "the stage's AUTO-SETUP (FUN_0043595A, gmode 0/2)"
                   : G.have_setup ? "the player's sliders (the screen's / --setup)"
                                  : "a new career's sliders");

    /* the arcade's rivals.  setuparcadecars (0x00444A87 ->
     * FUN_00447C3D) picks slots 1..3's car types at the level's load, from
     * arcrandom ([0x005B94FC]: 0 in a menus game) and rnd_next; each
     * slot then loads its handling and collision model (FUN_004247CD's
     * loop, numcars of them). */
    if (G.load_gmode == 2 && !G.have_cap && L.have_route) {
        ral_image im;
        int s;
        if (ral_image_open(&im, p_img, err, sizeof err) != 0
            || sim_arcade_consts_read(&im, &AK, err, sizeof err) != 0) {
            fprintf(stderr, "rally: the arcade's tables: %s\n", err);
            exit(1);
        }
        ral_image_close(&im);
        arc_rnd = G.seed_lfsr ? G.seed_lfsr : 1u;
        sim_arcade_rival_types_q(&A, &AK, G.carnum, 0u, &arc_rnd);
        for (s = 0; s < SIM_REPLAY_RIVALS; s++) {
            G.rv[s].p = car_params_for(&L.tab, A.types[s]);
            rc = sim_level_read_car_points(&L, p_b, A.types[s], &G.rv[s].pts,
                                           &G.rv[s].model, err, sizeof err);
            if (rc != 0) {
                fprintf(stderr, "rally: rival %d's collision model %s will not "
                                "load (%d)\n", s + 1, err, rc);
                exit(1);
            }
        }
        G.have_arcade = 1;
        printf("rally: the arcade: level %d stage %d, start position "
               "%d; the rivals' cars %d (%s), %d (%s), %d (%s)\n",
               (int)G.arc_level, (int)G.arc_stage, (int)G.arc_start,
               (int)A.types[0], G.rv[0].p->name, (int)A.types[1], G.rv[1].p->name,
               (int)A.types[2], G.rv[2].p->name);
    }

    /* The DIAGNOSTIC autopilot outside the arcade (RALLY_AUTOPILOT, the
     * gate): the rival driver's tables only - no field, no arcade rule. */
    if (G.load_gmode != 2 && !G.have_cap && L.have_route
        && getenv("RALLY_AUTOPILOT")) {
        ral_image im;
        if (ral_image_open(&im, p_img, err, sizeof err) != 0
            || sim_arcade_consts_read(&im, &AK, err, sizeof err) != 0) {
            fprintf(stderr, "rally: the autopilot's tables: %s\n", err);
            exit(1);
        }
        ral_image_close(&im);
        G.have_pilot = 1;
    }

    /* ---- the picture ---------------------------------------------------- */
    if (stage_load(&stg, L.raw, L.raw_len, L.ld, L.ld_len, err, sizeof err) != 0) {
        fprintf(stderr, "rally: stage_load: %s\n", err);
        exit(1);
    }
    stage_set_textinfo(&stg, L.ti, L.ti_len);
    if (plate_cache_init(&pc) != 0) {
        fprintf(stderr, "rally: out of memory\n");
        exit(1);
    }
    {
        plate_grade g;
        /* LOCALDAT +0x64c / +0x650, the level's grade, on every plate. */
        g.saturation = stg.have_ld ? ld_dw(&stg.ld, LD_GRADE_OFF) : LD_GRADE_NEUTRAL;
        g.brightness = stg.have_ld ? ld_dw(&stg.ld, LD_GRADE_OFF + 4) : LD_GRADE_NEUTRAL;
        /* 0x00442ABB, [0x005E0960] = LOCALDAT +0x6A0 bit 1. */
        G.car_lit = stg.have_ld ? (int)((ld_dw(&stg.ld, 0x6A0) >> 1) & 1u) : 0;
        G.car_lamp69c = stg.have_ld ? (int)(ld_dw(&stg.ld, 0x69C) & 1u) : 0;
        G.wet_level = stg.have_ld
                      && (ld_dw(&stg.ld, 0x690) | ld_dw(&stg.ld, 0x694)) != 0;
        G.have_car_light = stg.have_ld;
        if (stg.have_ld) {
            int k;
            G.sun_az = (int32_t)ld_dw(&stg.ld, 0x684);
            G.sun_el = (int32_t)ld_dw(&stg.ld, 0x688);
            G.car_light.diffuse = (int32_t)ld_dw(&stg.ld, 0x6A8);
            for (k = 0; k < 3; k++) {
                G.car_light.shadow[k] = (int32_t)ld_dw(&stg.ld, 0x600 + 4u * (unsigned)k);
                G.car_light.lit[k]    = (int32_t)ld_dw(&stg.ld, 0x60C + 4u * (unsigned)k);
            }
            /* [0x005E0964] is the drawn car's car+0x240 (0x00453019),
             * its ground light - car_light_now takes it per car. */
        }
        for (k = 0; k < 256; k++) {
            plat_tex t = 0;
            if (plate_cache_get(&pc, p_files, stg.plate[k], &g, &t,
                                err, sizeof err) != 0) {
                fprintf(stderr, "rally: plate %u: %s\n", stg.plate[k], err);
                exit(1);
            }
            stage_set_texture(&stg, (unsigned)k, t);
        }
        objscene_init(&objs);
        objscene_load(&objs, &stg.ld, stg.have_ld, p_files);
        /* [0x006CE08C]: the draw filters pols[+0x25] on the same direction
         * the collision does, or the other direction's flags stand there
         * backwards and cannot be hit. */
        objs.route_variant = L.objs.route_variant;
        /* The car's display model: FUN_0046102B(carnum), the table at
         * 0x006CEF1C (sim_consts.car_model), clamped as the original clamps. */
        k = G.carnum < 0 ? 0 : G.carnum > 0x17 ? 0x17 : G.carnum;
        model = (uint32_t)L.k.car_model[k];
        G.glass_own.valid = G.glass_ghost.valid = 0;      
        if (objscene_load_car(&objs, 0, model, p_files) != 0) {
            char path[1024];
            objscene_model_path(path, sizeof path, p_files, model);
            die_missing("the car's display model", path);
        }
        /* the rivals' display models, type 0x50 + 0x14 x slot */
        if (G.have_arcade) {
            int s;
            for (s = 0; s < SIM_REPLAY_RIVALS; s++) {
                int32_t t = A.types[s];
                uint32_t rm;
                t = t < 0 ? 0 : t > 0x17 ? 0x17 : t;
                rm = (uint32_t)L.k.car_model[t];
                G.glass_rival[s].valid = 0;
                if (objscene_load_car(&objs, s + 1, rm, p_files) != 0) {
                    char path[1024];
                    objscene_model_path(path, sizeof path, p_files, rm);
                    die_missing("a rival's display model", path);
                }
            }
        }
        {
            static uint16_t tid[OBJ_TYPES * OBJ_TEXIDS_MAX];
            unsigned n = objscene_texids(&objs, tid,
                                         (unsigned)(sizeof tid / sizeof tid[0]));
            unsigned i;
            for (i = 0; i < n; i++) {
                plat_tex t = 0;
                if (plate_cache_get(&pc, p_files, tid[i], &g, &t,
                                    err, sizeof err) != 0) {
                    fprintf(stderr, "rally: plate %u: %s\n", tid[i], err);
                    exit(1);
                }
                objscene_set_texture(&objs, tid[i], t);
            }
        }
        if (o->car_atlas) {                     /* the port's */
            int s;
            /* the override reads [0x5D64C8 + car*4], the RAW setup
             * tyre (not the car's, which the auto-setup replaces): car 0's is
             * the player's - 3 until the setup screen says otherwise - and
             * nothing the port runs writes the others (every capture logs
             * `grav 0` for cars 2 and up, and for car 1 outside two players) */
            car_load_in dflt;
            int tyre0;
            car_load_in_default(&dflt);
            tyre0 = G.have_setup ? (int)G.su.tyre : (int)dflt.sliders.tyre;
            for (s = 0; s < 1 + SIM_REPLAY_RIVALS; s++)
                car_atlas_build(OBJ_CAR_TYPE(s), p_files, &g, s == 0 ? tyre0 : 0);
        }
        /* Texture id 0x87, the reflection pass's sphere map: the frontend's
         * manifest entry (0x004717D6) is plate 2090, left in [0x0058C8CC]
         * for the race.  Whether the plate is still resident in a race is
         * not read; it is drawn here. */
        {
            plate_grade eg;
            plat_tex t = 0;
            eg.saturation = PLATE_GRADE_NEUTRAL;
            eg.brightness = PLATE_GRADE_NEUTRAL;
            if (plate_cache_get(&pc, p_files, 2090u, &eg, &t, err, sizeof err) != 0)
                fprintf(stderr, "rally: plate 2090 (the reflection map): %s\n", err);
            G.car_env_tex = t;
        }
        /* DIAGNOSTIC, not the original's behaviour: RALLY_CAR_NOTEX draws
         * the car with no plate bound, so a polygon that vanishes because
         * its texels are keyed out can be told from one that is never
         * drawn.  It is how the missing roof was diagnosed
         **/
        if (getenv("RALLY_CAR_NOTEX")) {
            objscene_type *ct = &objs.type[OBJ_CAR_TYPE(0)];
            unsigned j;
            for (j = 0; j < ct->ntexid; j++) ct->tex[j] = 0;
            printf("rally: RALLY_CAR_NOTEX - the car is drawn UNTEXTURED\n");
        }
        stage_set_object_hook(&stg, obj_hook, NULL);
        /* obj_hook draws from the sim's pols.  Before any step, it
         * must equal the stage's own parse on every byte
         * objscene_draw_record reads from a class-0xFF record: +0x00..+0x1B
         * (type, anchor, angles, the scale at +0x18) and +0x30..+0x3B (the
         * offset the hits move).  A difference would be a change the sim
         * made at load, or two parses that disagree. */
        {
            long recs = 0, agree = 0;
            int32_t i;
            for (i = 0; i < L.pl.npols; i++) {
                const uint8_t *a = cdat_pol(&stg.cdat, i);
                const uint8_t *b = L.pols + (size_t)i * 0x40u;
                if (!a || a[0] != 0xFF) continue;
                recs++;
                if (memcmp(a, b, 0x1C) == 0 && memcmp(a + 0x30, b + 0x30, 12) == 0)
                    agree++;
            }
            printf("rally: objects are drawn from the simulation's records: "
                   "%ld of %ld class-0xFF records agree with the stage's at "
                   "load\n", agree, recs);
            if (agree != recs) {
                fprintf(stderr, "rally: the simulation's records differ from "
                                "the stage's at load - objects would draw "
                                "wrongly\n");
                exit(1);
            }
        }
        printf("rally: car model %u (%lu polygons, %u texture ids); %lu object "
               "types loaded; %u plates, %lu decoded, %lu white fallback\n",
               (unsigned)model,
               (unsigned long)objs.type[OBJ_CAR_TYPE(0)].m.npolys,
               objs.type[OBJ_CAR_TYPE(0)].ntexid, objs.types_loaded,
               pc.n, pc.decoded, pc.white);
    }
    /* The draw radius [0x005BD538]: the level's own, from its detail preset
     * (LOCALDAT +0x1fd0 selects a row of +0x1fd4), clamped to >= 1 as
     * 0x00437ACD clamps it - unless --radius forces one. */
    {
        int pidx = stg.have_ld ? (int)ld_dw(&stg.ld, 0x1fd0) : 0;
        int pr = stg.have_ld ? (int)ld_dw(&stg.ld, 0x1fd4 + 4 * (unsigned)pidx) : 0;
        G.radius = o->radius > 0 ? o->radius : pr;
        if (G.radius < 1) G.radius = 1;
        if (G.radius > STAGE_RADIUS_MAX) G.radius = STAGE_RADIUS_MAX;
        printf("rally: draw radius %d cells (level preset %d: R %d)%s\n",
               G.radius, pidx, pr, o->radius > 0 ? " - forced by --radius" : "");
    }
    objscene_backdrop_params(&G.bg, &stg.ld, stg.have_ld, G.radius, 0);
    /* LOCALDAT +0x600 / +0x60c, the shade ramp's ends: RALLY_DAY_RAMP keeps
     * the black -> white identity ramp the port drew every stage with, and
     * with it the old object shade and no headlights - the A/B. */
    G.day_ramp = getenv("RALLY_DAY_RAMP") != NULL;
    /* [0x005D41B0] = 1 at the race start (0x00442624): the glows' additive
     * arm and the records' level lighting.  RALLY_NO_GLOW is the A/B, the
     * black discs (and diagnostic object shade) the port drew before. */
    objscene_set_race(getenv("RALLY_NO_GLOW") == NULL && !G.day_ramp);
    /* REFLECTIONS ([0x5D6760], read at 0x0044EBCB): off from the menu
     * row or --no-reflect; unload() puts the command line's back. */
    objscene_set_no_reflect(g_cli_no_reflect || (G.vis_set && !G.vis_reflect));
    /* the sun's flare and glare.  The LENS FLARE option
     * ([0x005D675C], VISUAL OPTIONS' 0x70CC6C: the menu's
     * [0x707FB8] reaches it through G.no_flare) is 1 by
     * default in the original (the static [0x005D675C] and the launcher's
     * source [0x00707FB8] are both 1); RALLY_NO_FLARE sets it 0 - no flare
     * sprites, the original's own switch - and RALLY_NO_SUN leaves the whole
     * sun out, the picture the port drew before. */
    sun_init(&SUN, &stg.ld, stg.have_ld,
             getenv("RALLY_NO_FLARE") == NULL && !G.no_flare);
    if (getenv("RALLY_NO_SUN")) SUN.have = 0;
    else if (SUN.have)
        printf("rally: the level has a sun (LOCALDAT +0x6AC %d, azimuth %d, "
               "elevation %d, panorama %d), lens flare %s\n", SUN.glare, SUN.az,
               SUN.el, SUN.yaw, SUN.option ? "on" : "off");
    CSUN.have = stg.have_ld;
    CSUN.az = stg.have_ld ? (int)ld_dw(&stg.ld, 0x684) : 0;
    CSUN.el = stg.have_ld ? (int)ld_dw(&stg.ld, 0x688) : 0;
    if (stg.have_ld)
        objscene_set_level_sun((int32_t)ld_dw(&stg.ld, 0x684), (int32_t)ld_dw(&stg.ld, 0x688),
                               (int32_t)ld_dw(&stg.ld, 0x630), (int32_t)ld_dw(&stg.ld, 0x634));

    G.no_hud = getenv("RALLY_NO_HUD") != NULL;
    /* the port's fix, ON by default (user's call);
     * RALLY_FAITHFUL_CULL brings back the original's one-corner reject. */
    G.patch_cull = getenv("RALLY_FAITHFUL_CULL") == NULL;
    G.test_msg = -1;
    {
        const char *e = getenv("RALLY_MESSAGE");
        long id, at;
        char c;
        if (e && sscanf(e, "%ld:%ld%c", &id, &at, &c) == 2 && id > 0) {
            G.test_msg = (int32_t)id;
            G.test_msg_at = (int32_t)at;
            printf("rally: RALLY_MESSAGE - message %ld on the banner at "
                   "alivet %ld\n", id, at);
        }
    }
    G.obj_from_stage = getenv("RALLY_OBJECTS_FROM_STAGE") != NULL;
    if (G.obj_from_stage)
        printf("rally: RALLY_OBJECTS_FROM_STAGE - objects drawn from the "
               "stage's copy: a knocked object stays where it stood\n");
    /* the HUD - its plates (files\b\tx1798..tx1815), the sprite table
     * out of the image, the banner's FNT22, and JADE.BUN for the
     * port's race-over prompt.  A missing
     * plate is an error: the HUD is the game's, not an extra. */
    {
        ral_image im;
        char p_jade[1024];
        int have_jade = vfs_resolve(&fs, "files\\d\\gfx\\jade.bun", p_jade,
                                    sizeof p_jade) == 0;
        if (ral_image_open(&im, p_img, err, sizeof err) != 0) {
            fprintf(stderr, "rally: %s: %s\n", p_img, err);
            exit(1);
        }
        /* the camera's tables, out of the image. */
        if (sim_camera_consts_load(&im, &G.camk, err, sizeof err) != 0) {
            fprintf(stderr, "rally: the camera will not load: %s\n", err);
            exit(1);
        }
        /* the cars' part words, from the same rows. */
        {
            int c, w;
            for (c = 0; c < SIM_CAMERA_CARS; c++)
                for (w = 0; w < CAR_ROW_WORDS; w++) {
                    const unsigned char *q = ral_image_at(&im,
                        0x0057E10Cu + 0x12Cu * (uint32_t)c + 0x58u
                        + 4u * (uint32_t)w, 4);
                    if (!q) {
                        fprintf(stderr, "rally: the car part table is not in the image\n");
                        exit(1);
                    }
                    G.car_row[c][w] = ral_image_i32(q);
                }
            /* the wheels' rows, 0x0045ECD4's reads and 0x00424997's */
            for (c = 0; c < SIM_CAMERA_CARS; c++) {
                static const uint32_t off[8] = { 0x50, 0x54, 0x58, 0x5C,
                                                 0x24, 0x28, 0x2C, 0x30 };
                int32_t *dst[8];
                wheel_geom *wg = &G.car_wheel[c];
                dst[0] = &wg->bit[0]; dst[1] = &wg->bit[1];
                dst[2] = &wg->bit[2]; dst[3] = &wg->bit[3];
                dst[4] = &wg->front_z; dst[5] = &wg->rear_z;
                dst[6] = &wg->track;   dst[7] = &wg->height;
                for (w = 0; w < 8; w++) {
                    const unsigned char *q = ral_image_at(&im,
                        0x0057E10Cu + 0x12Cu * (uint32_t)c + off[w], 4);
                    if (!q) {
                        fprintf(stderr, "rally: the car wheel table is not in the image\n");
                        exit(1);
                    }
                    *dst[w] = ral_image_i32(q);
                }
            }
            {
                const unsigned char *q = ral_image_at(&im, 0x005C6CF8u, 4);
                if (!q) {
                    fprintf(stderr, "rally: the model turn is not in the image\n");
                    exit(1);
                }
                G.model_drop = ral_image_i32(q);
            }
            for (c = 0; c < SIM_CAMERA_CARS; c++) {
                const unsigned char *q = ral_image_at(&im,
                    0x006CE094u + 0x30u * (uint32_t)c, 0x30);
                if (!q) {
                    fprintf(stderr, "rally: the tacho plate table is not in the image\n");
                    exit(1);
                }
                memcpy(G.car_tacho[c], q, 0x30);
            }
            for (w = 0; w < 7; w++) {
                const unsigned char *q = ral_image_at(&im,
                    0x006CE4ECu + 4u * (uint32_t)w, 4);
                if (!q) {
                    fprintf(stderr, "rally: the slot-8 table is not in the image\n");
                    exit(1);
                }
                G.car_slot8[w] = ral_image_i32(q);
            }
        }
        G.cam_view = 4;                /* CHASE 1, where every capture starts */
        /* [0x6C2794] <- [0x70CD90] <- player+0x0C (0x00445429); the
         * screen offers 0..6, the seven views */
        if (G.has_menu_view && G.menu_view >= 0 && G.menu_view <= 6)
            G.cam_view = G.menu_view;
        {
            const char *e = getenv("RALLY_CAMERA_VIEW");
            if (e && *e >= '0' && *e <= '6') {
                G.cam_view = *e - '0';
                printf("rally: RALLY_CAMERA_VIEW - starting in view %d\n",
                       (int)G.cam_view);
            }
        }
        if (hud_init(&G.hud, p_files, &im, have_jade ? p_jade : NULL, G.p,
                     L.sin_tab, L.have_route ? &L.route : NULL, &L.geom[0],
                     G.carnum, G.video, err, sizeof err) != 0) {
            fprintf(stderr, "rally: the HUD will not load: %s\n", err);
            exit(1);
        }
        /* the banner's font, FNT22 (loadgamefonts_q) */
        {
            char p00[1024], p01[1024];
            int ok00 = vfs_resolve(&fs, "files\\d\\fnt\\fnt22_00.pcx", p00,
                                   sizeof p00) == 0;
            int ok01 = vfs_resolve(&fs, "files\\d\\fnt\\fnt22_01.pcx", p01,
                                   sizeof p01) == 0;
            if (G.video
                && hud_banner_load(&G.hud, &im, ok00 ? p00 : NULL,
                                   ok01 ? p01 : NULL, err, sizeof err) != 0)
                fprintf(stderr, "rally: the banner's font will not load (%s): "
                                "no messages\n", err);
            /* the map disc's MAPnn.SQ, nn the level number */
            if (hud_map_load(&G.hud, p_files,
                             (int)strtol(o->stage + (strncmp(o->stage, "LEVEL", 5) ? 0 : 5),
                                         NULL, 10),
                             err, sizeof err) != 0)
                printf("rally: no map disc (%s)\n", err);
        }
        codriver_tables(&im);                      
        /* the car's load - the auto-setup, the section allowances */
        if (car_load_tables_read(&im, &G.clt, err, sizeof err) != 0) {
            fprintf(stderr, "rally: %s\n", err);
            exit(1);
        }
        /* the car's sounds' tables (carsound.h) */
        if (snd_car_tables_load(&CT, &im) != 0)
            fprintf(stderr, "rally: the car's sound tables will not load: "
                            "the car is silent\n");
        else
            have_ct = 1;
        /* the weather's and the ambience's (worldsound.h) */
        if (have_ct && snd_world_load(&W, &im, &C) == 0)
            have_world = 1;
        else if (have_ct)
            fprintf(stderr, "rally: the world's sound tables will not load: "
                            "no weather or ambience\n");
        /* what the render path draws for rain and snow - the
         * engine's tables, plate 900 and files\m\edgeNN.pcx (NN the model,
         * [0x007115A2] = FUN_00454CEC(car+0x24)) */
        {
            plate_grade wg;
            char p_edge[1024], name[32];
            int m = (int)G.carnum;
            wg.saturation = stg.have_ld ? ld_dw(&stg.ld, LD_GRADE_OFF) : LD_GRADE_NEUTRAL;
            wg.brightness = stg.have_ld ? ld_dw(&stg.ld, LD_GRADE_OFF + 4) : LD_GRADE_NEUTRAL;
            if (m == 6) m = 11;
            if (m < 0) m = 0;
            if (m > WFX_CARS - 1) m = WFX_CARS - 1;
            if (wfx_load(&WX, &im, p_files, &wg, m, G.video, err, sizeof err) != 0)
                fprintf(stderr, "rally: the weather's picture will not load "
                                "(%s): no rain or snow drawn\n", err);
            else if (getenv("RALLY_NO_WEATHER")
                     && strcmp(getenv("RALLY_NO_WEATHER"), "screen") != 0
                     && strcmp(getenv("RALLY_NO_WEATHER"), "falling") != 0)
                printf("rally: RALLY_NO_WEATHER - no rain or snow drawn "
                       "(a DIAGNOSTIC: weather-drawn's control)\n");
            else {
                /* DIAGNOSTIC, weather-drawn's: RALLY_NO_WEATHER=screen draws
                 * nothing on the glass (0x004B7106 still runs: weatherfx.h),
                 * =falling leaves out the falling rain and snow (0x004C1100) */
                const char *e = getenv("RALLY_NO_WEATHER");
                WX.diag_no_glass_draw = e && strcmp(e, "screen") == 0;
                wfx_no_falling = e && strcmp(e, "falling") == 0;
                snprintf(name, sizeof name, "files\\m\\edge%02d.pcx", m);
                if (vfs_resolve(&fs, name, p_edge, sizeof p_edge) != 0
                    || wfx_load_edge(&WX, p_edge, err, sizeof err) != 0)
                    fprintf(stderr, "rally: %s will not load: every drop and "
                                    "flake on the glass dies\n", name);
                have_wfx = 1;
            }
        }
        /* the wheels' spray - the module's tables out of the image,
         * TEXTINFO's sprite records (0x004B604C), files\m\surfdata.pcx and
         * surfmask.pcx (0x004BACFA); its generator is the weather's
         * [0x006CF334].  RALLY_NO_SPRAY: none (a DIAGNOSTIC, the control) */
        if (getenv("RALLY_NO_SPRAY"))
            printf("rally: RALLY_NO_SPRAY - no spray emitted or drawn\n");
        else if (spray_load(&SP, &im, L.ti, L.ti_len, err, sizeof err) != 0)
            fprintf(stderr, "rally: the spray will not load (%s)\n", err);
        else {
            char p_sd[1024], p_sm[1024];
            if (have_wfx) SP.lcg = &WX.lcg;
            if (G.video
                && (vfs_resolve(&fs, "files\\m\\surfdata.pcx", p_sd, sizeof p_sd) != 0
                    || vfs_resolve(&fs, "files\\m\\surfmask.pcx", p_sm, sizeof p_sm) != 0
                    || spray_load_atlas(&SP, p_sd, p_sm, err, sizeof err) != 0))
                fprintf(stderr, "rally: the spray's texture will not load: it "
                                "steps undrawn\n");
            have_spray = 1;
            atexit(spray_report);
        }
        /* the finish's fireworks - their tables out of the
         * image; the generator is the weather's, as the spray's */
        have_fw = 0;
        if (G.fw_finish) {
            if (fireworks_load(&FW, &im, err, sizeof err) != 0)
                fprintf(stderr, "rally: the fireworks will not load (%s)\n", err);
            else {
                if (have_spray) FW.lcg = SP.lcg;
                else if (have_wfx) FW.lcg = &WX.lcg;
                fw_on = 0;
                fw_last_ms = wfx_clock;
                have_fw = 1;
            }
        }
        /* the blades out of the car's model (0x004ACC36),
         * after it is loaded; the phase and the generators carry over from
         * the last race, as the original's globals do */
        if (wipers_load(&WP, &im, &objs.type[OBJ_CAR_TYPE(0)].m, G.carnum,
                        err, sizeof err) != 0)
            printf("rally: the wipers will not move (%s)\n", err);
        else {
            uint32_t gen = 0;
            printf("rally: the wipers: car %d, blades of %d and %d vertices\n",
                   WP.car, WP.b[0].n, WP.b[1].n);
            /* item 2: the masks and the flow field's draws (0x004AD93A ..
             * 0x004AE36F); the draws are made again at every race start,
             * between the weather's seed and rain3d's table */
            if (wipers_masks_build(&WP, &im, &WM, &gen, err, sizeof err) != 0)
                printf("rally: the wipers will not wipe (%s)\n", err);
            else {
                WX.wipe = &WM;
                WX.wp = &WP;
                WX.wipe_draws = WM.draws;
                WX.wipe_cur = WM.cur0;
                printf("rally: the wipe-off: masks of %d and %d steps, "
                       "%lu draws at each race start\n", (int)WM.steps[0],
                       (int)WM.steps[1], WM.draws);
            }
        }
        /* the wheel and the hands out of the car's model
         * (0x004B0E39 -> 0x004A9314), the weights from files\m\spline.dat */
        {
            char p_sp[1024];
            if (vfs_resolve(&fs, "files\\m\\spline.dat", p_sp, sizeof p_sp) != 0
                || driver_load_table(&DP, p_sp, err, sizeof err) != 0)
                printf("rally: the wheel and hands will not move (spline.dat)\n");
            else if (driver_load(&DP, &im, &objs.type[OBJ_CAR_TYPE(0)].m,
                                 G.carnum, err, sizeof err) != 0)
                printf("rally: the wheel and hands will not move (%s)\n", err);
            else
                printf("rally: the wheel and hands: car %d, parts of "
                       "%d, %d, %d and %d vertices\n", DP.car, DP.p[0].n,
                       DP.p[1].n, DP.p[2].n, DP.p[3].n);
        }
        if (!wiper_dump && getenv("RALLY_WIPER_DUMP"))
            wiper_dump = fopen(getenv("RALLY_WIPER_DUMP"), "w");
        ral_image_close(&im);
        G.have_hud = 1;
        /* the game's text for the banner's view and mirror messages
         * (files\locale\ENGLISH.TXT, lexicon.h); without it they are blank. */
        {
            char p_lex[1024];
            if (vfs_resolve(&fs, "files\\locale\\english.txt", p_lex,
                            sizeof p_lex) == 0
                && lexicon_load(&G.lex, p_lex) == 0) {
                G.have_lex = 1;
                G.hud.lex = &G.lex;
            }
        }
        if (G.video)
            printf("rally: the HUD: %s dial; the banner %s\n",
                   G.hud.b.dial2 ? "7,000 rpm" : "9,000 rpm",
                   !G.hud.have_banner ? "is blank (no FNT22)"
                   : G.have_lex ? "in FNT22"
                   : "is blank (no ENGLISH.TXT)");
        if (G.no_hud) printf("rally: RALLY_NO_HUD - the HUD is not drawn\n");
    }
    if (L.have_route) {
        sim_race tmp;
        sim_start_mode m;
        memset(&m, 0, sizeof(m));
        m.numpl = 1;
        sim_race_init(&tmp, &L.route, &L.geom[0], &m);
        printf("rally: the race: %d checkpoints on the route, the last the "
               "finish (numcheckposits %d)\n", (int)tmp.numcheckposits + 1,
               (int)tmp.numcheckposits);
        /* init_pacenotes_q (0x00442CD1, `loadlevel: init
         * pacenotes`) over the route as it stands - the pool zeroed on every
         * load, a port decision (physics.h) */
        sim_pacenotes_build(&notes, &L.route, L.pols_pristine, L.pl.npols,
                            L.pl.buf + L.pl.pnts_off, L.pl.npnts);
        have_notes = 1;
        printf("rally: the co-driver: %d sentences on the route, in the %s "
               "voice%s\n", (int)notes.count,
               S.set.voice[0] ? "female" : "male",
               S.nosound ? " (--nosound: not heard)" : "");
    } else {
        have_notes = 0;
    }
    /* stage, objects, cars and HUD all load above */
    printf("rally: %u plates from graphics overrides\n", plate_overrides_taken());
}

/* ---- the command line ---------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: rally [--assets DIR]                     (the menus)\n"
        "       rally --replay statehook.bin [options]   (watch, or --live)\n"
        "       rally --stage LEVELnn [options]          (drive from the start line)\n"
        "       rally --finish N [--fireworks] [options] (the finish scene, N 0..5)\n"
        "the menus: the original's MAIN MENU -> START RALLY -> single (or time\n"
        "      trial, 5th slot) -> 1 player -> car -> SELECT STAGE -> [time trial:\n"
        "      the SERVICE AREA, the car's setup] -> the race; arrows, RETURN,\n"
        "      ESC.  ESC in the race, or RETURN after it, goes to RESULTS, then\n"
        "      REVIEW OPTIONS, then back to SELECT STAGE.  var\\ral.dat (beside\n"
        "      the working directory, or in the install) is read over the players\n"
        "  --menu-result CS,MPH  RESULTS shows this time (hundredths) and speed,\n"
        "                      not the race's (for a picture)\n"
        "  --menu-keys KEYS    drive the menus from a script: U D L R E(nter) X(esc);\n"
        "                      a slot's name being typed takes a-z 0-9 space - .\n"
        "                      and < (backspace)\n"
        "  --menu-shot FILE    write the menu after --menu-keys as a PPM and stop\n"
        "  --menu-ms N         ms since the last key for that shot (default 0)\n"
        "  --menu-step N       with a %%d shot: 512 shots N ms apart from --menu-ms\n"
        "  --menu-cost N       after the script: N frames 16 ms apart on its last\n"
        "                      screen, timed, with the interpreter's instructions\n"
        "                      (the 3D models' yaw turns once in 8192 ms)\n"
        "  --assets DIR        the game's asset tree (default: $RALLY_ASSETS, else\n"
        "                      the executable's folder if RAL.EXE is there, else .)\n"
        "  --sound PATH        play this .wav once at start (files/d/SND/...)\n"
        "  --music PATH        stream this .wav, looping (files/music/...); the\n"
        "                      game's own music cues then stand back (nomusic)\n");
    fprintf(stderr,
        "  --nomusic          the config's nomusic: no music cue plays\n"
        "  --nosound           the config's nosound: no sound at all\n"
        "  --joy-dz LOWER,UPPER the pad's joylowerdz / joyupperdz, 0-100 percent\n"
        "                      over SETUP CONTROLLER's lower / upper dz;\n"
        "                      without the menus the port's default 10,5\n"
        "  --in-game-music     AUDIO SETUP's in-game music ON, over the screen's\n"
        "                      setting; off, as the game boots, a race plays no\n"
        "                      music\n"
        "  --sound-log FILE    one line per sound request heard (the gate's)\n"
        "                      (test hooks)\n"
        "without --replay: the car is put on the stage's own start line\n"
        "  --car N             the car, 0..23 (default 12)\n"
        "  --direction D       the course direction, 0 or 1 (default 0)\n"
        "  --setup T,B,H,S,E,G the player's setup: tyre 0-5, brake balance,\n"
        "                      suspension height, stiffness, steering (0-100),\n"
        "                      gearing 0-4 (default: a new career's,\n"
        "                      3,50,70,30,50,2).  It reaches the car only in\n"
        "                      gmodes 1 and 3: a single stage and the arcade\n"
        "                      race on the stage's auto-setup, as the game does\n"
        "  --gmode M           the mode the load runs (default 0, a single\n"
        "                      stage; 1 championship, 2 arcade, 3 time trial)\n"
        "  --cheats W          the menus' cheat word [0x70AA78] (bit 16, 0x10000:\n"
        "                      the arcade's clock never runs out)\n"
        "  --arcade L,S[,P]    gmode 2: the arcade's level 0-5 and stage 1-6,\n"
        "                      and the position the last stage finished in\n"
        "                      (default 0,1: stage 1 starts last)\n"
        "  --no-ral-dat        the menus: the image's players, not var\\ral.dat\n"
        "                      (nor var\\hst.dat: the best times are seeded)\n"
        "  --unlock-a8         the menus: open START RALLY's 3rd slot, the A8\n"
        "                      championship ([0x70CC34] = 1; a test hook - it is\n"
        "                      the Mobil 1 title's prize)\n"
        "  --user-dir DIR      where var\\ and savedata\\ are read and written\n"
        "                      (default: the working directory; a file not\n"
        "                      there is read from the install's own var\\)\n"
        "  --hd-textures       HD TEXTURES on for this run (rally.cfg's\n"
        "                      hd_textures=1 keeps it on); --no-hd-textures off\n"
        "  --no-save           write nothing (a --menu-keys script writes only\n"
        "                      with --user-dir)\n"
        "  --no-quit-confirm   the main menu quits at once, as the original's\n"
        "                      (default: the EXIT GAME screen is asked first)\n"
        "  --quit-confirm      ask it in a --menu-keys script too (they never do)\n"
        "  --gl-stream N       vertex upload: 0 a buffer per draw, 1 client-side\n"
        "                      arrays (the default on ARM: 2026-10-19's measure)\n"
        "  --no-car-atlas      each car plate its own texture (the default is one\n"
        "                      atlas: far fewer draws, the same picture)\n"
        "  --no-reflect        leave the cars' reflection layer out (a measurement)\n"
        "  --model-timing      time the model path's phases (the 'models' line);\n"
        "                      off by default - the timers cost a clock read per polygon\n"
        "  --env-two-pass      the cars' reflection as separate draws (the default\n"
        "                      adds it in the paint's own draw: the same picture)\n"
        "  --pad-nintendo      in the menus, A and B by their printed labels on a\n"
        "                      Nintendo-style pad (A right, B bottom: Anbernic);\n"
        "                      the race's buttons are unchanged\n"
        "  --gearbox G         0 manual, 1 semi-automatic, 2 fully automatic\n"
        "                      (default 1)\n"
        "  --seed N            the car's random seed (default 1; the game's is\n"
        "                      the clock's)\n"
        "  --image FILE        RAL.EXE, for the physics tables (default: DIR/RAL.EXE)\n"
        "  --stage NAME        default LEVEL33, the level run 18 was driven on\n"
        "  --reseed N          re-seed from the capture every N steps; 0 = free run\n"
        "                      (default 50)\n"
        "  --start FRAME       start at this capture frame (default 0)\n"
        );
    /* split: C99 promises string literals of 4,095 characters, no more */
    fprintf(stderr,
        "  --window WxH        the game's resolution, default 800x600; --no-window\n"
        "                      draws nothing\n"
        "  --fullscreen        the desktop's whole screen, the picture scaled to\n"
        "                      fit (the default for the menus, a player's launch)\n"
        "  --windowed          a window (the default with --stage or --replay);\n"
        "                      Alt+Enter switches either way, and the window can\n"
        "                      be resized\n"
        "  --no-vsync          present as fast as the loop runs (by default it is\n"
        "                      paced to the display; the 10 ms step never is)\n"
        "  --radius R          force the draw radius in cells (default: the level's)\n"
        "  --frame-ms N|jitter a fake clock: N ms per frame, or the gate's pattern\n"
        "  --steps N           stop after N steps\n"
        "  --exit-at-end       quit when the capture ends (default: hold)\n"
        "  --dump FILE         write each step's 1,668-byte car record\n"
        "  --record FILE       write the live race's replay (the game's\n"
        "                      own format, tools/replay/ral_replay.py reads it);\n"
        "                      the menus' race writes var\\tmprpl on its own when\n"
        "                      the saves are on, --no-record stops it\n"
        "  --view FILE         play a replay: its stage, car and mode from the\n"
        "                      header; SPACE pause, . step, LEFT/RIGHT -/+1 chunk (a\n"
        "                      second), DOWN/UP -/+10, HOME start, F1 camera, ESC leave\n"
        "  --shot FRAME:FILE   write the frame at FRAME as a PPM (repeatable)\n"
        "  --live              DRIVE: the keyboard, from the capture's car at\n"
        "                      --start; --reseed does not apply\n"
        "  --keys SCRIPT       with --live: the keys from a script, not the\n"
        "                      keyboard (lines \"STEP KEYS\", KEYS from LRTBUDHXV-)\n"
        "keys: SPACE pause, . step, LEFT/RIGHT -/+1 s, DOWN/UP -/+10 s,\n"
        "      HOME restart, G the game's car too, F12 screenshot, ESC quit\n"
        "--live keys (the original's, car_keys.h; from the menus the\n"
        "      player record's own, var\\ral.dat included): UP throttle, DOWN\n"
        "      brake, LEFT/RIGHT steer, # gear up (the key left of RETURN on a\n"
        "      UK keyboard, \\ on a US one), RIGHT SHIFT gear down, ' handbrake,\n"
        "      ; reset car (hold 1 s while slow: back on the route, a time\n"
        "      penalty), RETURN camera view, ] lights, P PAUSED; and\n"
        "      the game's F1 view, F8 mirror, F3/F4 volume (left Ctrl: the\n"
        "      music), ESC RETIRE? (y/n/a); HOME start again, F12 screenshot\n"
        "the race: an 8 s countdown, the stage clock, checkpoints and\n"
        "      the finish; going into gear in the countdown is a false start.\n"
        "      The HUD is the original's (hud.h) but\n"
        "      for the map disc and the car picture; RALLY_NO_HUD leaves it out\n");
}

static int parse(int argc, char **argv, options *o)
{
    int i;
    memset(o, 0, sizeof *o);
#if defined(__aarch64__) || defined(__arm__)
    /* the RG40XX-H's Mali measured client-side arrays cheapest of
     * the three uploads; --gl-stream overrides */
    plat_gfx_set_stream(1);
#endif
    /* the cars' plates in one texture - on by default since the
     * RG40XX-H run of 2026-10-19 (car 13.5 -> 10.3 ms, the picture the same
     * to one level on 25-36 of 307,200 pixels); --no-car-atlas for the old */
    o->car_atlas = 1;
    o->assets = getenv("RALLY_ASSETS");
    if (!o->assets || !*o->assets) o->assets = ".";
    else o->assets_given = 1;
    o->fullscreen = -1;
    o->hd_textures = -1;
    o->stage = "LEVEL33";
    o->finish = -1;
    o->carparts = -1;                 /* a new career's: 0x00482FE1 */
    o->reseed = 50;
    o->car = 12;                   /* run 18's: the Honda Civic              */
    o->seed = 1;
    o->gearbox = -1;
    /* the arcade's first level and stage; race number 1, what
     * 0x00461689 numbers slot 0; player 2's gearbox the image's [0x5D6748],
     * 0 (with no menus, nothing sets it) */
    o->arc_stage = 1;
    o->arc_start = -1;
    o->racenum = 1;
    o->gearbox2 = 0;
    o->w = 800; o->h = 600; o->window = 1;
    o->codriver = -1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && v && (++i, 1))
        if (ARG("--assets")) { o->assets = v; o->assets_given = 1; }
        else if (ARG("--image")) o->image = v;
        else if (ARG("--stage")) { o->stage = v; o->stage_given = 1; }
        else if (ARG("--finish")) { o->finish = atol(v); o->stage_given = 1; }
        else if (ARG("--replay")) o->replay = v;
        else if (ARG("--reseed")) o->reseed = atol(v);
        else if (ARG("--start")) o->start = atol(v);
        else if (ARG("--steps")) o->steps = atol(v);
        else if (ARG("--radius")) o->radius = atoi(v);
        else if (ARG("--dump")) o->dump = v;
        else if (ARG("--record")) o->record = v;
        else if (ARG("--view")) o->view = v;
        else if (ARG("--view-start")) o->view_start = atol(v);
        else if (ARG("--keys")) o->keys = v;
        else if (ARG("--car")) o->car = atol(v);
        else if (ARG("--seed")) o->seed = strtoul(v, NULL, 0);
        else if (ARG("--direction")) o->direction = atol(v);
        else if (ARG("--gearbox")) o->gearbox = atol(v);
        else if (ARG("--gmode")) o->gmode = atol(v);     
        else if (ARG("--cheats")) o->cheats = (uint32_t)strtoul(v, NULL, 0);
        else if (ARG("--arcade")) {                      
            long t[3] = { 0, 1, -1 };
            int nf = sscanf(v, "%ld,%ld,%ld", &t[0], &t[1], &t[2]);
            if (nf < 2 || t[0] < 0 || t[0] > 5 || t[1] < 1 || t[1] > 6) return -1;
            o->arc_level = t[0];
            o->arc_stage = t[1];
            o->arc_start = nf == 3 ? t[2] : -1;
        }
        else if (ARG("--setup")) {
            /* T,B,H,S,E,G: tyre, brake balance, suspension height and
             * stiffness, steering sensitivity, gearing - the six numbers
             * of the car setup screen */
            long t[6];
            if (sscanf(v, "%ld,%ld,%ld,%ld,%ld,%ld", &t[0], &t[1], &t[2], &t[3],
                       &t[4], &t[5]) != 6) return -1;
            for (int k = 0; k < 6; k++) o->su[k] = (int32_t)t[k];
            o->has_setup = 1;
        }
        else if (ARG("--menu-result")) {
            /* CS,MPH: what the race "handed back", for a picture of RESULTS */
            long t[2];
            if (sscanf(v, "%ld,%ld", &t[0], &t[1]) != 2) return -1;
            o->result_cs = (int32_t)t[0]; o->result_mph = (int32_t)t[1];
            o->has_result = 1;
        }
        else if (ARG("--menu-keys")) o->menu_keys = v;
        else if (ARG("--user-dir")) o->user_dir = v;
        else if (ARG("--menu-shot")) o->menu_shot = v;
        else if (ARG("--vm-census")) o->vm_census = v;
        else if (ARG("--menu-ms")) o->menu_ms = atol(v);
        else if (ARG("--menu-step")) o->menu_step = atol(v);
        else if (ARG("--menu-cost")) o->menu_cost = atol(v);
        else if (ARG("--sound")) o->sound = v;
        else if (ARG("--music")) o->music = v;
        else if (ARG("--sound-log")) o->sound_log = v;
        else if (ARG("--joy-dz")) {         /* section 12's pair */
            int lo, up;
            if (sscanf(v, "%d,%d", &lo, &up) != 2 || lo < 0 || up < 0
                || lo + up > 99) return -1;
            car_input_set_deadzone(lo, up);
            o->joy_dz = 1;
        }
        else if (ARG("--codriver")) {
            if (strcmp(v, "0") == 0 || strcmp(v, "male") == 0) o->codriver = 0;
            else if (strcmp(v, "1") == 0 || strcmp(v, "female") == 0) o->codriver = 1;
            else return -1;
        }
        else if (ARG("--window")) {
            if (sscanf(v, "%dx%d", &o->w, &o->h) != 2 || o->w < 64 || o->h < 64)
                return -1;
        } else if (ARG("--frame-ms")) {
            if (strcmp(v, "jitter") == 0) o->jitter = 1;
            else if ((o->frame_ms = atoi(v)) <= 0) return -1;
        } else if (ARG("--shot")) {
            const char *c = strchr(v, ':');
            if (!c || o->nshots >= MAX_SHOTS) return -1;
            o->shot_frame[o->nshots] = atol(v);
            o->shot_path[o->nshots] = c + 1;
            o->nshots++;
        } else if (strcmp(a, "--no-window") == 0) o->window = 0;
        else if (strcmp(a, "--exit-at-end") == 0) o->exit_at_end = 1;
        else if (strcmp(a, "--fireworks") == 0) o->fireworks = 1;
        else if (strcmp(a, "--fullscreen") == 0) o->fullscreen = 1;
        else if (strcmp(a, "--windowed") == 0) o->fullscreen = 0;
        else if (strcmp(a, "--no-vsync") == 0) o->no_vsync = 1;
        else if (strcmp(a, "--live") == 0) o->live = 1;
        else if (strcmp(a, "--no-ral-dat") == 0) o->no_ral_dat = 1;
        else if (strcmp(a, "--no-save") == 0) o->no_save = 1;
        else if (strcmp(a, "--no-record") == 0) o->no_record = 1;
        else if (strcmp(a, "--unlock-a8") == 0) o->unlock_a8 = 1;       
        else if (strcmp(a, "--hd-textures") == 0) o->hd_textures = 1;   
        else if (strcmp(a, "--no-hd-textures") == 0) o->hd_textures = 0;
        else if (strcmp(a, "--no-quit-confirm") == 0) o->no_quit_confirm = 1;
        else if (strcmp(a, "--quit-confirm") == 0) o->quit_confirm = 1;
        else if (strcmp(a, "--pad-nintendo") == 0) pad_nintendo = 1;
        else if (ARG("--gl-stream")) {      
            int m = atoi(v);
            plat_gfx_set_stream(m);
            printf("rally: --gl-stream %d (%s)\n", m,
                   m == 1 ? "client-side arrays" : "a store a draw");
        }
        else if (strcmp(a, "--car-atlas") == 0) o->car_atlas = 1;
        else if (strcmp(a, "--no-car-atlas") == 0) o->car_atlas = 0;
        else if (strcmp(a, "--env-two-pass") == 0) objscene_set_env_two_pass(1);
        else if (strcmp(a, "--model-timing") == 0) objscene_set_timing(1);  
        else if (strcmp(a, "--no-reflect") == 0) {    /* a measurement */
            objscene_set_no_reflect(1);
            g_cli_no_reflect = 1;
            printf("rally: --no-reflect: the cars' reflection layer is not drawn\n");
        }
        else if (strcmp(a, "--nomusic") == 0) o->nomusic = 1;
        else if (strcmp(a, "--nosound") == 0) o->nosound = 1;
        else if (strcmp(a, "--in-game-music") == 0) o->in_game_music = 1;
        else return -1;
#undef ARG
    }
    if (o->reseed < 0) return -1;
    /* No capture: nothing to replay, so it is a drive. */
    if (!o->replay) o->live = 1;
    if (o->keys && !o->live) return -1;
    if (o->car < 0 || o->car > 0x17) return -1;
    if (o->gearbox < -1 || o->gearbox > 2) return -1;
    return 0;
}

/* --view FILE: the race the replay's header names - its track (LEVELnn),
 * car, mode and course direction (the port's own header word, 0 in the game's
 * files) - played by the recording.  An arcade replay is refused: the file
 * carries neither the rivals nor the arcade's header words (section 20). */
static int prep_view(options *o)
{
    static char stage[16];
    char why[160];
    int32_t gm, car;
    if (rplrd_open(&vrd, o->view, why, sizeof why) != 0) {
        fprintf(stderr, "rally: %s\n", why);
        return -1;
    }
    gm = rplrd_hdr(&vrd, RPLH_GMODE);
    car = rplrd_hdr(&vrd, RPLH_CAR1);
    if (gm == 2) {
        fprintf(stderr, "rally: %s is an arcade replay, which cannot be played yet\n", o->view);
        return -1;
    }
    if (car < 0 || car > 0x17 || rplrd_hdr(&vrd, RPLH_TRACK) < 0 || rplrd_hdr(&vrd, RPLH_TRACK) > 99) {
        fprintf(stderr, "rally: %s names no car or stage the port knows (car %d, track %d)\n",
                o->view, (int)car, (int)rplrd_hdr(&vrd, RPLH_TRACK));
        return -1;
    }
    snprintf(stage, sizeof stage, "LEVEL%02d", (int)rplrd_hdr(&vrd, RPLH_TRACK));
    o->stage = stage;
    o->stage_given = 1;
    o->car = car;
    o->gmode = gm;
    o->direction = rplrd_hdr(&vrd, RPLH_PORT_DIR) & 1;
    o->live = 1;
    o->record = NULL;
    o->no_record = 1;
    o->ghosts = NULL;                  /* the menus' launch data is a race's, not this one's */
    o->wear = NULL;
    o->carparts = 0;
    o->champ88 = 0;
    o->has_setup = 0;
    view_on = 1;
    printf("rally: viewing %s: %u chunks (%d steps), track %d, car %d, gmode %d, direction %ld\n",
           o->view, vrd.nchunks, (int)(vrd.nchunks * RPL_FRAMES), (int)rplrd_hdr(&vrd, RPLH_TRACK),
           (int)car, (int)gm, o->direction);
    return 0;
}

/* ---- the main loop ------------------------------------------------------- */

/* The gate's fake clock: frame durations that exercise every arm of the
 * accumulator - no step (0, 3 ms), one, several, the remainder carried, and
 * the 1,000 ms clamp (1,500 ms) - in a fixed cycle so a run is repeatable. */
static const int jitter_ms[] = { 0, 3, 10, 17, 40, 250, 1500, 7, 33, 16, 120, 9 };

static int race(options *op);
static int finish(options *op);
static int fin_menus;                  /* the scene runs in a menu's hook */
static int menus(options *op);

/* What the last race handed back, for RESULTS (frontend.h, fe_result): the
 * launcher's copy of car+0x088 and the top speed (0x00481518..). */
static fe_result g_result;

int main(int argc, char **argv)
{
    options o;
    plat_config cfg;
    int rc;

    if (parse(argc, argv, &o) != 0) {
        usage();
        return 2;
    }
    if (o.view && prep_view(&o) != 0) return 1;       
    /* a double-click names no asset tree.  The game copied beside the
     * executable is the next guess, before the working directory. */
    if (!o.assets_given) {
        static char exe[1024];
        char probe[1100];
        if (plat_exe_dir(exe, sizeof exe) == 0) {
            snprintf(probe, sizeof probe, "%sRAL.EXE", exe);
            if (plat_file_exists(probe)) {
                o.assets = exe;
                /* and the saves beside it: a launch through explorer.exe
                 * starts in C:\Windows\system32 (measured), where var\
                 * cannot be written */
                if (!o.user_dir && !o.menu_keys && !o.menu_shot)
                    o.user_dir = exe;
            }
        }
    }
    /* Graphics override packs, the port's (plate.h): $RALLY_OVERRIDES is the
     * root (default ./overrides), $RALLY_GFX_PACK the pack tried before
     * "default" (e.g. hd).  A root that does not exist costs one failed
     * fopen per plate and changes nothing. */
    {
        const char *r = getenv("RALLY_OVERRIDES");
        const char *pack = getenv("RALLY_GFX_PACK");
        /* HD TEXTURES (rally.cfg, default off; the flag wins for
         * the run) picks the "hd" pack.  Off loads what it always did. */
        portcfg pc;
        portcfg_load(&pc, o.user_dir);
        if (o.hd_textures >= 0) pc.hd_textures = o.hd_textures;
        if (!pack && pc.hd_textures) pack = "hd";
        car_input_set_curve(pc.pad_curve);           /* 100 = linear */
        if (pc.pad_curve != 100) printf("rally: pad steering curve %d%%\n", pc.pad_curve);
        if (pack) printf("rally: graphics pack \"%s\"\n", pack);
        plate_set_overrides(r ? r : "overrides", pack);
    }
    /* the menus are a player's launch - fullscreen unless --windowed;
     * --stage and --replay are a developer's, windowed unless --fullscreen.
     * --window WxH is the game's resolution either way. */
    if (o.fullscreen < 0)
        o.fullscreen = (!o.replay && !o.stage_given) ? 1 : 0;
    cfg.app_name = "rally";
    cfg.log_path = NULL;
    cfg.want_fullscreen = o.fullscreen;
    /* U1: a fake-clock run hears on its own clock, not the host's device */
    cfg.audio_clock = (o.frame_ms || o.jitter) ? 1 : 0;
    if (plat_init(&cfg) != 0) {
        fprintf(stderr, "rally: plat_init failed\n");
        return 1;
    }
    G.reseed = o.reseed;
    G.live = o.live;
    G.nan_step = -1;
    if (o.keys) {
        long bad = 0;
        if (car_script_load(&G.script, o.keys, &bad) != 0) {
            fprintf(stderr, "rally: key script %s: unreadable or bad at line "
                            "%ld\n", o.keys, bad);
            return 2;
        }
        G.have_script = 1;
    }
    G.w = o.w; G.h = o.h;
    G.car_nocull = getenv("RALLY_CAR_NOCULL") != NULL;
    /* The surface first: the plates are uploaded as they load, and an upload
     * needs a context.  Without one (--no-window, or no display) every draw
     * degrades to counting, and the simulation is the same either way -
     * which is what the gate relies on. */
    if (o.window) {
        plat_video_set_vsync(!o.no_vsync);
        plat_video_set_mode(o.w, o.h, 32, o.fullscreen);
        G.video = plat_video_ok();
        if (!G.video) fprintf(stderr, "rally: no drawable surface - running "
                                      "without a picture\n");
    }
    G.gearbox = o.gearbox;
    {
        snd_backend be;
        snd_plat_backend(&be, o.assets);
        snd_init(&S, &be);
        /* --music's test stream is the music stream: the game's cues stand
         * back, as with the config's nomusic */
        S.nomusic = o.nomusic || o.music;
        S.nosound = o.nosound;
        S.set.race_music_on = o.in_game_music;
        if (o.codriver >= 0) S.set.voice[0] = o.codriver;   /* the stand-in */
        if (o.sound_log && !(S.log = fopen(o.sound_log, "w")))
            fprintf(stderr, "rally: cannot write %s\n", o.sound_log);
    }
    /* the mixer's test hooks.  Output only: nothing here reaches the
     * simulation (the replay gates must not move). */
    if (o.sound || o.music) {
        vfs afs;
        char host[1100];
        vfs_init(&afs, o.assets, NULL);
        if (o.sound) {
            plat_sample s = wav_load_sample(&afs, o.sound);
            plat_voice v = plat_audio_voice_alloc();
            plat_audio_voice_bind(v, s);
            plat_audio_voice_play(v, 0);
        }
        if (o.music && (vfs_resolve(&afs, o.music, host, sizeof host) != 0 ||
                        plat_audio_music_play(host, 1) != 0))
            fprintf(stderr, "rally: music %s: cannot play (%s)\n", o.music, host);
    }
    /* no --stage and no capture - the original's menus choose. */
    rc = o.finish >= 0 ? finish(&o)
       : (!o.replay && !o.stage_given) ? menus(&o) : race(&o);
    if (S.log) fclose(S.log);
    plat_shutdown();
    return rc;
}

/* ---- the race: everything from the load to the end of the run ----------- */

/* set by the window's close request inside a race, so the menus stop
 * too. */
static int app_quit;

static int race(options *op)
{
    options o = *op;
    uint32_t last_ms, now_ms = 0, fake_ms = 0;
    uint64_t stat_us;
    long frames = 0, stat_frames = 0, stat_steps = 0, jit = 0;
    uint64_t stat_sim = 0, stat_draw = 0, stat_swap = 0;   /* us, a second's */
    int paused = 0, quit = 0, nextshot = 0, i;
    unsigned char keydown[VK_COUNT];
    static const int keys[VK_COUNT] = { VK_PAUSE, VK_STEP, VK_BACK_1S,
        VK_FWD_1S, VK_BACK_10S, VK_FWD_10S, VK_RESTART, VK_GHOST, VK_SHOT,
        VK_QUIT, VK_LIVE_PAUSE, VK_VIEW, VK_MIRROR, VK_VOL_DOWN, VK_VOL_UP };

    if (o.has_setup) {
        G.su.tyre = o.su[0]; G.su.bbal = o.su[1]; G.su.seth = o.su[2];
        G.su.sets = o.su[3]; G.su.stes = o.su[4]; G.su.setg = o.su[5];
        G.have_setup = 1;
    }
    /* what the load reads - the mode, the track (LEVELnn's nn), the
     * player's parts word and carried time */
    G.load_gmode = (int32_t)o.gmode;
    G.track = (int32_t)strtol(o.stage + (strncmp(o.stage, "LEVEL", 5) ? 0 : 5),
                              NULL, 10);
    G.carparts = o.carparts;
    G.champ88 = o.champ88;
    G.cheats = o.cheats;
    G.have_ghosts = o.ghosts != NULL;              
    if (o.ghosts) memcpy(G.ghosts, o.ghosts, sizeof G.ghosts);
    G.have_wear = o.wear != NULL;                  
    if (o.wear) G.wear = *o.wear;
    G.arc_level = (int32_t)o.arc_level;              
    G.arc_stage = (int32_t)o.arc_stage;
    G.arc_start = (int32_t)o.arc_start;
    G.racenum = o.racenum;
    G.gearbox2 = o.gearbox2;
    snprintf(rec_path, sizeof rec_path, "%s", o.record ? o.record : "");   
    rec_dir = o.direction;
    load(&o);
    /* 0x0040EE51's speech set - the co-driver's files by the player's
     * voice (0x0040ECFB) - and the queues opened (0x0040D423) */
    {
        uint64_t t0 = plat_time_us();
        snd_cd_race(&S, 1, snd_ms);
        if (!S.nosound)
            printf("rally: the co-driver's speech loaded before the start, "
                   "%.1f ms\n", (double)(plat_time_us() - t0) / 1000.0);
        /* the car's sounds are armed (and their files loaded) when
         * the race record exists, at the first codriver_attach */
        C.log = S.log;
    }
    cd_restart = 0;
    /* the launcher's sound mode, then shauniniteverything's
     * `doingamemusic` (0x004424E6) - silent unless in-game music is on */
    snd_race_music(&S);     /* stamped with the menus' clock (the log's) */
    if (o.dump) {
        G.dump = fopen(o.dump, "wb");
        if (!G.dump) {
            fprintf(stderr, "rally: cannot write %s\n", o.dump);
            return 1;
        }
    }
    if ((G.have_arcade || G.have_pilot) && getenv("RALLY_AUTOPILOT")) {
        const char *e = getenv("RALLY_AUTOPILOT");
        long a, b;
        int used;
        G.ap_n = 0;
        while (G.ap_n < 4 && sscanf(e, "%ld:%ld%n", &a, &b, &used) == 2) {
            if (b > a) {
                G.ap_from[G.ap_n] = a;
                G.ap_to[G.ap_n] = b;
                printf("rally: RALLY_AUTOPILOT - the rival driver drives the car "
                       "on steps %ld..%ld (a DIAGNOSTIC: 0x00438ADE's factor-1 "
                       "entry)\n", a, b - 1);
                G.ap_n++;
            }
            e += used;
            if (*e != ',') break;
            e++;
        }
    }
    if (G.have_arcade && getenv("RALLY_ARCADE_LOG")) {
        G.arc_log = fopen(getenv("RALLY_ARCADE_LOG"), "w");
        if (G.arc_log)
            printf("rally: RALLY_ARCADE_LOG - the streamer, one line a step, "
                   "to %s\n", getenv("RALLY_ARCADE_LOG"));
    }
    seek(o.start);
    if (view_on && o.view_start > 0) {          /* --view-start, the seek key's code */
        long c = o.view_start;
        if (c > (long)vrd.nchunks - 1) c = (long)vrd.nchunks - 1;
        view_seek(c);
    }
    if (G.live) {
        if (G.have_cap)
            printf("rally: LIVE - %s drives the car from capture frame %ld; a "
                   "free run, the port's own physics, %s clock%s\n",
                   G.have_script ? "the key script" : "the keyboard", G.frame,
                   (o.frame_ms || o.jitter) ? "FAKE" : "real",
                   G.video ? "" : ", no window");
        else
            printf("rally: LIVE - %s drives the car from the start line "
                   "(placed, levelled and settled as the game does it, at "
                   "%.3f %.3f %.3f m); %s clock%s\n",
                   G.have_script ? "the key script" : "the keyboard",
                   G.car.pos[0] / 65536.0, G.car.pos[1] / 65536.0,
                   G.car.pos[2] / 65536.0,
                   (o.frame_ms || o.jitter) ? "FAKE" : "real",
                   G.video ? "" : ", no window");
        if (G.car.alivet_r < 0x320)
            printf("rally: the countdown holds the car for %d steps (%.2f s); "
                   "it stands in neutral - gear up to move off\n",
                   (int)(0x320 - G.car.alivet_r),
                   (0x320 - G.car.alivet_r) / 100.0);
        if (car_input_keys_are_default())
            printf("rally: the original's keys (player 1): UP throttle, "
                   "DOWN brake, LEFT/RIGHT steer, # gear up, RIGHT SHIFT gear "
                   "down, ' handbrake, ; reset car (hold), RETURN camera view; "
                   "P pause, HOME start again, F12 shot, ESC quit\n");
        else
            printf("rally: the player record's own keys (not the image's "
                   "defaults; --no-ral-dat for those); P pause, HOME start "
                   "again, F12 shot, ESC quit\n");
    } else {
        printf("rally: %s, start frame %ld, %s clock%s\n",
               G.reseed == 0 ? "FREE RUN (seeded once)" : "re-seeded from the capture",
               G.frame, (o.frame_ms || o.jitter) ? "FAKE" : "real",
               G.video ? "" : ", no window");
        if (G.reseed > 0)
            printf("rally: re-seed every %ld steps\n", G.reseed);
        printf("rally: SPACE pause, . step, LEFT/RIGHT seek 1 s, DOWN/UP 10 s, "
               "HOME restart, G ghost, F12 shot, ESC quit\n");
    }

    memset(keydown, 0, sizeof keydown);
    memset(G.fold_down, 1, sizeof G.fold_down);    /* no key "pressed" at once */
    G.modal = MODAL_NONE;
    G.pause_cool = 0;
    if (!G.from_menu) {
        /* STAND-IN: no launcher ran, so [0x5D6774] / [0x5D6750] are the
         * port's: RETIRE? offers A, as the menus' single race does */
        G.retire_all = 1;
        G.gmode = 0;
    }
    last_ms = (uint32_t)(plat_time_us() / 1000u);
    if (o.frame_ms || o.jitter) last_ms = fake_ms = 0;
    stat_us = plat_time_us();

    while (!quit) {
        long due, done = 0;
        int pressed[VK_COUNT];
        unsigned char jp[0x80];
        uint64_t ft[6];                         /* TEMPORARY - freeze hunt */

        /* ---- the message pump and the viewer's keys --------------------- */
        ft[0] = plat_time_us();
        plat_input_poll();
        ft[1] = plat_time_us();
        if (plat_quit_requested()) { app_quit = 1; break; }
        snd_tick(&S);                       /* AUDIOTDB's service */
        ft[2] = plat_time_us();
        set_pad_ctx(view_on ? PADK_VIEW                               
                : G.modal != MODAL_NONE ? PADK_WAIT : PADK_RACE);   
        for (i = 0; i < VK_COUNT; i++) {
            int d = G.video ? key_down(keys[i]) : 0;
            pressed[i] = d && !keydown[i];
            keydown[i] = (unsigned char)d;
        }
        fold_scan(jp);
        /* doplaygame_q 0x00443223 - in a live race ESC is RETIRE?,
         * and the held pause bit (slot 10, dannyebx & 0x40, while the
         * cooldown is 0) is PAUSED.  A replay, or a race that is over, keeps
         * the viewer's ESC: quit. */
        if (view_on) {
            if (pressed[9]) quit = 1;                /* ESC leaves the viewer */
        } else if (G.live && !G.race_over && G.modal == MODAL_NONE) {
            if (pressed[9]) retire_enter();
            else if (G.pause_cool == 0 && (G.car.dannyebx & 0x40u))
                pause_enter();
        } else if (pressed[9] && G.modal == MODAL_NONE) quit = 1;
        if (G.modal == MODAL_PAUSED) pause_frame(jp, pressed);
        else if (G.modal == MODAL_RETIRE) {
            int r = retire_frame(jp);
            if (r == -1) {                  /* Y: gof -1 into the race */
                G.car.gof = -1;
                printf("rally: step %ld - retired\n", G.live_steps);
                quit = 1;
            } else if (r == -2) {           /* A: gof -2, the race again */
                printf("rally: step %ld - RETIRE? A: the race starts again\n",
                       G.live_steps);
                seek(o.start);
            }
        }
        /* FUN_00438C42, before the step batch - F1 the view, F8 the
         * mirror, each with its message on player 1's banner. */
        else if (pressed[11] || pressed[12] || pressed[13] || pressed[14])
            camera_keys(pressed[11], pressed[12], pressed[13], pressed[14]);
        /* RALLY_MESSAGE: the id as F1 sends one - 300 steps, no
         * flash - for comparing the banner against a snapshot. */
        if (G.test_msg > 0 && (int32_t)G.car.alivet >= G.test_msg_at) {
            race_msg(G.test_msg, 0x12C);
            G.test_msg = -1;
        }
        /* /: RETURN once the race is over goes to RESULTS, as
         * the original does (frontend.h). */
        if (G.from_menu && G.race_over && G.video && key_down(40))
            quit = 1;
        if (view_on) {
            /* SPACE pauses, '.' steps, the arrows seek by chunk (a second)
             * and by ten, HOME starts again; the camera keys are above */
            long c = (long)((uint32_t)G.car.alivet / RPL_FRAMES), to = -1;
            if (pressed[0]) paused = !paused;
            view_paused = paused;
            if (pressed[6]) to = 0;
            else if (pressed[2]) to = c - 1;
            else if (pressed[3]) to = c + 1;
            else if (pressed[4]) to = c - 10;
            else if (pressed[5]) to = c + 10;
            if (to >= 0 || pressed[2] || pressed[4] || pressed[6]) {
                if (to < 0) to = 0;
                if (to > (long)vrd.nchunks - 1) to = (long)vrd.nchunks - 1;
                view_seek(to);
            }
        } else if (G.live) {
            /* The arrows are the car's, and P is the game's own pause key
             * (slot 10): PAUSED above; no seek, no ghost. */
            if (pressed[6] && G.modal == MODAL_NONE) {
                seek(o.start);
                printf("rally: the live run starts again at frame %ld\n",
                       G.frame);
            }
        } else {
        if (pressed[0]) paused = !paused;
        if (pressed[7]) G.ghost = !G.ghost;
        if (pressed[2] || pressed[3] || pressed[4] || pressed[5] || pressed[6]) {
            long to = pressed[6] ? o.start
                    : G.frame + (pressed[2] ? -100 : pressed[3] ? 100
                               : pressed[4] ? -1000 : 1000);
            seek(to);
            printf("rally: seek to frame %ld - the hidden state (object "
                   "hits, ground hints, rev_q) starts FRESH here, not as the "
                   "game had it\n", G.frame);
        }
        }
        if (paused && pressed[1]) {
            step();
            stat_steps++;
        }

        /* ---- FUN_004436EF, the accumulator ------------------------------ */
        if (o.frame_ms || o.jitter) {
            uint32_t inc = (uint32_t)(o.jitter
                       ? jitter_ms[jit++ % (long)(sizeof jitter_ms / sizeof jitter_ms[0])]
                       : o.frame_ms);
            fake_ms += inc;
            now_ms = fake_ms;
            plat_audio_advance(inc);          /* U1: the streams' clock too */
        } else {
            now_ms = (uint32_t)(plat_time_us() / 1000u);
        }
        snd_ms = now_ms;
        if (paused || G.modal || G.at_end || G.race_over) {
            last_ms = now_ms;                 /* the pause path's resync     */
            due = 0;
        } else {
            if (now_ms - last_ms >= 1000u)     /* more than 1 s of debt:     */
                last_ms = now_ms - 1000u;      /*   thrown away, not repaid  */
            due = (long)((now_ms - last_ms) / 10u);
        }

        /* ---- FUN_0044368D: every due step ------------------------------- */
        while (done < due) {
            /* A pending screenshot or step limit stops the batch AT its
             * frame, so the picture is of exactly that frame. */
            if (nextshot < o.nshots && G.frame >= o.shot_frame[nextshot])
                break;
            if (o.steps && G.steps_run >= o.steps) break;
            if (!step()) break;
            done++;
            /* 0x004436AD: CALL FUN_004436C1 / JB - the race is over. */
            if (G.race_over) break;
        }
        last_ms += (uint32_t)(done * 10);
        stat_steps += done;
        ft[3] = plat_time_us();
        /* the steps' speech requests into the queue, then the audio
         * thread's service of it (0x0040F031's 20 ms gate inside) */
        /* the car's sounds tick inside that gate - but not while no
         * step runs (snd_car_hush, a STAND-IN: the original's pause and its
         * finish are unread; without a step the thread would extrapolate
         * the revs from its last two updates without end). */
        {
            static int car_held;
            /* PAUSED / RETIRE? closed the records themselves */
            int held = paused || G.modal || G.at_end || G.race_over;
            if (held && !car_held && have_ct) snd_car_hush(&C);
            if (held && !car_held && have_world) snd_world_hush(&W);
            car_held = held;
            S.race_tick = (held || !have_ct || !rep.have_race || C.feed == NULL)
                          ? NULL : car_sound_tick;
            S.world_tick = (S.race_tick && have_world) ? world_sound_tick : NULL;
        }
        {
            uint64_t t0 = plat_time_us(), t1;     /* TEMPORARY - freeze hunt */
            codriver_drain(now_ms);
            t1 = plat_time_us();
            snd_cd_tick(&S, now_ms);
            if (env_flag("RALLY_SOUND_TIMING", &env_sound_timing)
                && (t1 - t0 > 20000u || plat_time_us() - t1 > 20000u))
                fprintf(stderr, "sound-timing: frame at %u ms: drain %.1f ms, "
                        "tick %.1f ms\n", (unsigned)now_ms,
                        (double)(t1 - t0) / 1000.0,
                        (double)(plat_time_us() - t1) / 1000.0);
        }

        /* ---- FUN_00452103: render once ---------------------------------- */
        ft[4] = plat_time_us();
        /* the engine's clock and its advance flag (block +0x10,
         * 0 while [0x005D64B4] - paused or the RETIRE? prompt) */
        wfx_clock = now_ms;
        wfx_advance = !(paused || G.modal);
        render();
        ft[5] = plat_time_us();
        frames++;
        stat_frames++;
        while (nextshot < o.nshots && G.frame >= o.shot_frame[nextshot]) {
            if (G.frame == o.shot_frame[nextshot])
                write_ppm(o.shot_path[nextshot]);
            else
                fprintf(stderr, "rally: frame %ld was never shown (seeked "
                                "past it)\n", o.shot_frame[nextshot]);
            nextshot++;
        }
        if (pressed[8]) {
            char path[64];
            snprintf(path, sizeof path, "rally_%06ld.ppm", G.frame);
            write_ppm(path);
        }
        plat_video_present();
        stat_sim  += ft[3] - ft[2];             /* the line's split */
        stat_draw += ft[5] - ft[4];
        stat_swap += plat_time_us() - ft[5];
        if (env_flag("RALLY_SOUND_TIMING", &env_sound_timing)) {  /* TEMPORARY - freeze hunt */
            uint64_t end = plat_time_us();
            if (end - ft[0] > 100000u)
                fprintf(stderr, "frame-timing: step %ld (%ld steps): poll %.1f "
                        "snd %.1f steps %.1f cd %.1f render %.1f present %.1f "
                        "ms\n", G.live_steps, done,
                        (ft[1] - ft[0]) / 1000.0, (ft[2] - ft[1]) / 1000.0,
                        (ft[3] - ft[2]) / 1000.0, (ft[4] - ft[3]) / 1000.0,
                        (ft[5] - ft[4]) / 1000.0, (end - ft[5]) / 1000.0);
        }

        /* ---- the rates, once a second of REAL time ----------------------
         * The interval restarts at the SAME instant it was measured to, so
         * consecutive intervals tile real time.  The first version took the
         * new start after printing, which dropped the time spent writing the
         * line from every interval and showed the accumulator's correct
         * catch-up for it as a spurious 166 steps/s second. */
        {
            uint64_t now_us = plat_time_us();
            if (now_us - stat_us >= 1000000u) {
                double s = (double)(now_us - stat_us) / 1e6;
                if (G.live) {
                    /* newspd is m/s ('s units check). */
                    printf("rally: %7.2f s  %6.1f km/h  gear %2d  revs %6.0f  "
                           "steer %+5.2f  wheels on road %d  %5.1f fps  "
                           "steps %.1f draw %.1f swap %.1f ms/frame%s\n",
                           G.live_steps / 100.0,
                           sqrt((double)G.car.newspd[0] * G.car.newspd[0]
                                + (double)G.car.newspd[2] * G.car.newspd[2])
                               * 3.6,
                           (int)G.car.gear, (double)G.car.revs,
                           G.car.usteerangle / (double)0x20000000,
                           (int)G.car.road_wheels_q, stat_frames / s,
                           stat_frames ? stat_sim / 1000.0 / stat_frames : 0.0,
                           stat_frames ? stat_draw / 1000.0 / stat_frames : 0.0,
                           stat_frames ? stat_swap / 1000.0 / stat_frames : 0.0,
                           paused ? "  (paused)" : "");
                    if (stat_frames) {      /* render's parts */
                        double k = 1000.0 * stat_frames;
                        printf("rally:   render  pre %.1f  sky %.1f  stage %.1f "
                               "(walk %.1f sort %.1f submit %.1f)  cars %.1f  "
                               "engine %.1f  mirror %.1f  hud %.1f  "
                               "ms/frame\n",
                               rt_us[RT_PRE] / k, rt_us[RT_SKY] / k,
                               rt_us[RT_STAGE] / k, rt_us[RT_WALK] / k,
                               rt_us[RT_SORT] / k, rt_us[RT_SUBMIT] / k,
                               rt_us[RT_CARS] / k, rt_us[RT_ENGINE] / k,
                               rt_us[RT_MIRROR] / k, rt_us[RT_HUD] / k);
                    }
                    {                       /* the model path's phases */
                        unsigned long long ou[4];
                        unsigned long od;
                        objscene_prof_take(ou, &od);
                        if (stat_frames && !objscene_timing())
                            printf("rally:   models  (phase timers off: "
                                   "--model-timing)  %.1f draws/frame\n",
                                   (double)od / stat_frames);
                        else if (stat_frames)
                            printf("rally:   models  corners %.1f  reflect %.1f  "
                                   "fan %.1f  gl %.1f ms/frame  %.1f draws/frame\n",
                                   ou[0] / (1000.0 * stat_frames),
                                   ou[1] / (1000.0 * stat_frames),
                                   ou[2] / (1000.0 * stat_frames),
                                   ou[3] / (1000.0 * stat_frames),
                                   (double)od / stat_frames);
                    }
                    memset(rt_us, 0, sizeof rt_us);
                } else
                printf("rally: frame %6ld / %ld  %6.1f fps  %6.1f steps/s%s\n",
                       G.frame, cap.frames - 1, stat_frames / s, stat_steps / s,
                       paused ? "  (paused)" : G.at_end ? "  (end)" : "");
                fflush(stdout);
                stat_us = now_us;
                stat_frames = stat_steps = 0;
                stat_sim = stat_draw = stat_swap = 0;
            }
        }
        if (o.steps && G.steps_run >= o.steps && nextshot >= o.nshots) break;
        if ((G.at_end || G.race_over) && o.exit_at_end && nextshot >= o.nshots)
            break;
        if (G.halted) break;                   /* the guard */
    }

    printf("rally: %ld frames rendered, %ld steps run (%ld re-seeded), %ld "
           "step errors, last frame %ld\n", frames, G.steps_run, G.reseeds,
           G.step_errors, G.frame);
    if (G.live)
        printf("rally: live run %ld steps (%.2f s): top speed %.1f km/h, "
               "%.0f m driven, %ld steps with no wheel on a road class, "
               "first non-finite value %ld (-1 = never), car reset due "
               "%ld times (performed %ld); final position %.2f %.2f %.2f m\n",
               G.live_steps, G.live_steps / 100.0, G.max_speed * 3.6, G.dist,
               G.offroad_steps, G.nan_step, G.d4b3_due, G.d4b3_ran,
               G.car.pos[0] / 65536.0, G.car.pos[1] / 65536.0,
               G.car.pos[2] / 65536.0);
    g_result.finished = G.car.gof == 2 || G.car.rfn;
    g_result.time_cs = G.car.catitken;
    g_result.time_left_cs = G.car.catileft;        /* 0x00447A4C */
    if (G.load_gmode == 2)
        printf("rally: the arcade's clock: %d.%02d s left%s\n",
               (int)(G.car.catileft / 100), (int)(G.car.catileft % 100),
               G.car.oot ? " - OUT OF TIME" : "");
    /* 0x00447A56, slot 0's arcadepo -> [0x70CF6C] */
    g_result.arcadepo = G.have_arcade ? G.car.arcadeposition : -1;
    if (G.have_arcade)
        printf("rally: the arcade's position: %d of %d (this stage's best "
               "%d); %ld spawns, %ld fell back, %ld got ahead, %ld resets "
               "refused\n", (int)G.car.arcadeposition, (int)A.n,
               (int)A.bestpos, A.spawns, A.fell_back, A.got_ahead,
               A.resets_refused);
    g_result.speed_x10 = (int32_t)(G.max_speed * 2.2369363) * 10;   /* 0x0048153A; mph, the unit RESULTS prints (unit 0, [0x517954]) */
    /* the live camera's view (its start is player+0x0C unless F1 cycled it) */
    printf("rally: the race's camera ends in view %d, the HUD mask is 0x%X\n",
           (int)G.camx.cam.nview, (unsigned)*(const uint32_t *)((const unsigned char *)&G.car + 0x220));
    /* shaun_to_graham's hand-back - the wear over the launch's
     * block (its pass-through dwords kept), the parts left, the HUD mask */
    memcpy(slot_warn, (const unsigned char *)&G.car + SLOT_WARN_AT, sizeof slot_warn);
    g_result.have_wear = G.have_wear;
    if (G.have_wear) {
        g_result.wear = G.wear;
        wear_from_car(&g_result.wear, &G.car);
        g_result.carparts = G.car.carpartsleft;
        memcpy(&g_result.hud_mask, (const unsigned char *)&G.car + 0x220, 4);
        printf("rally: the wear handed back: engine %d, cooling %d, tyres %d, "
               "lights %d, body %d%% (what came in, and the race's own wear"
               ")\n", (int)g_result.wear.l[0],
               (int)g_result.wear.l[6], (int)g_result.wear.l[48],
               (int)g_result.wear.l[35], (int)g_result.wear.l[1]);
    }
    if (rep.have_race)
        printf("rally: the race: section %d of %d, stage time %d:%02d.%02d "
               "(penalties included), %s\n",
               G.car.currsegment < rep.race.numcheckposits
                   ? (int)G.car.currsegment + 1 : (int)rep.race.numcheckposits,
               (int)rep.race.numcheckposits,
               (int)(G.car.catitken / 6000), (int)(G.car.catitken / 100 % 60),
               (int)(G.car.catitken % 100),
               G.car.gof == 2 ? "FINISHED" : G.car.rfn ? "past the line"
               : G.car.gof ? "retired" : "not finished");
    if (G.have_hud) {
        if (rep.have_race)
            printf("rally: the HUD: %ld messages picked up; the co-driver: %ld "
                   "countdown calls, %ld sentences, %ld wrong-way / turn-around "
                   "calls, %ld requests lost\n", G.hud.pickups, rep.race.beeps,
                   rep.race.pace_spoken, rep.race.sounds,
                   rep.race.speech_lost);
        hud_free(&G.hud);
    }
    if (G.have_script && !G.from_menu) car_script_free(&G.script);
    if (G.dump) { fclose(G.dump); G.dump = NULL; }
    if (G.arc_log) { fclose(G.arc_log); G.arc_log = NULL; }
    rec_stop();                             /* 0x0044239C's stop */
    g_result.replay = rec_made;             /* [0x00574548]: REVIEW's SAVE VIDEO REPLAY */
    snd_cd_off(&S);                         /* the race's speech done */
    /* and the car's (0x0040D213 closes the record; its voices stop
     * with the race's sound set) */
    if (have_ct) { snd_car_hush(&C); C.rec.on = 0; }
    if (have_world) snd_world_hush(&W);
    S.race_tick = NULL;
    return G.step_errors ? 1 : 0;
}

/* ----: the finish scene ---------------------------------------------
 *
 * 0x00456F42 ("CALLING FINISH ROUTINE"), which all three finish routines
 * reach through 0x004264D7 - is the reading.
 * The level is LEVEL0n, n = the table at 0x005FE9DC indexed by [0x70D068];
 * the race's own init (shauniniteverything) runs with [0x005FE9D8] = 1, the
 * no-route mode, and gmode 3; then 0x0045718C's loop runs NO CAR STEP: the
 * car stands where the placement and the settle left it, lifted 0.6 m, while
 * the camera's view 6 is re-scripted every frame (0x00457249) and stepped
 * once a frame (FUN_0044DEE4).  The loop ends at 52 s, or 99 s with the
 * fireworks ([0x70D074]), or on ESC or RETURN.
 *
 * What the port does NOT do here: the fireworks themselves ([0x005787E4],
 * and the menus' hand-off (phase 5) - so the music after the
 * scene is the callers' tail, stop and track 3, played at the exit. */
typedef struct finish_consts {
    int32_t level[7];        /* 0x005FE9DC: LEVEL0n by [0x70D068]          */
    float   lift;            /* 0x005FE9F8: 0.6 m onto car 1's y           */
    float   dur[2];          /* 0x005FE9FC: the scene's length by [0x70D074] */
    float   warp0, warp1;    /* 0x005FEA04 / 08: the clock eases, then stops */
    float   yaw_k;           /* 0x005FEA0C: view 6's yaw, turns a second   */
    float   z0;              /* 0x005FEA10: its distance at 0              */
    float   y0;              /* 0x005FEA14: its height at 0                */
    float   z_k;             /* 0x005FEA18                                 */
    float   y_k;             /* 0x005FEA1C                                 */
    float   fw0, fw1;        /* 0x005FEA30 / 34: the fireworks' ramp       */
    float   pitch_fw;        /* 0x005FEA38: view 6's pitch at full ramp    */
    float   y_fw;            /* 0x005FEA3C: its height at full ramp        */
    float   k65536;          /* 0x0057B01C                                 */
    float   ms;              /* 0x0057AF9C: 1000                           */
    float   half;            /* 0x0057B0C4: 0.5                            */
} finish_consts;

static int finish_consts_read(const char *p_img, finish_consts *k,
                              char *err, size_t n)
{
    ral_image im;
    const unsigned char *q;
    int i;
    if (ral_image_open(&im, p_img, err, n) != 0) return -1;
    q = ral_image_at(&im, 0x005FE9DCu, 0x64);
    if (!q) {
        snprintf(err, n, "the finish's table is not in the image");
        ral_image_close(&im);
        return -1;
    }
    for (i = 0; i < 7; i++) k->level[i] = ral_image_i32(q + 4 * i);
    q += 0x1C;                                  /* 0x005FE9F8 */
    k->lift = ral_image_f32(q + 0x00);
    k->dur[0] = ral_image_f32(q + 0x04);
    k->dur[1] = ral_image_f32(q + 0x08);
    k->warp0 = ral_image_f32(q + 0x0C);
    k->warp1 = ral_image_f32(q + 0x10);
    k->yaw_k = ral_image_f32(q + 0x14);
    k->z0 = ral_image_f32(q + 0x18);
    k->y0 = ral_image_f32(q + 0x1C);
    k->z_k = ral_image_f32(q + 0x20);
    k->y_k = ral_image_f32(q + 0x24);
    k->fw0 = ral_image_f32(q + 0x38);
    k->fw1 = ral_image_f32(q + 0x3C);
    k->pitch_fw = ral_image_f32(q + 0x40);
    k->y_fw = ral_image_f32(q + 0x44);
    if (!(q = ral_image_at(&im, 0x0057B01Cu, 4))) goto bad;
    k->k65536 = ral_image_f32(q);
    if (!(q = ral_image_at(&im, 0x0057AF9Cu, 4))) goto bad;
    k->ms = ral_image_f32(q);
    if (!(q = ral_image_at(&im, 0x0057B0C4u, 4))) goto bad;
    k->half = ral_image_f32(q);
    ral_image_close(&im);
    return 0;
bad:
    snprintf(err, n, "a finish constant is not in the image");
    ral_image_close(&im);
    return -1;
}

static int finish(options *op)
{
    options o = *op;
    finish_consts k;
    char err[256], p_img[1024], stage[16];
    uint32_t now_ms = 0, t0_ms, fake_ms = 0;
    long frames = 0, i;
    int fw = o.fireworks ? 1 : 0, quit = 0, nextshot = 0, by_key = 0;
    int fireworks_said = 0, cam_bad = 0;
    float t = 0.0f;
    int32_t y_settled;

    if (o.finish < 0 || o.finish > 5) {
        fprintf(stderr, "rally: --finish takes 0..5 ([0x70D068], what "
                        "0x00441488 clamps it to)\n");
        return 2;
    }
    if (o.image) snprintf(p_img, sizeof p_img, "%s", o.image);
    else {
        vfs fs;
        vfs_init(&fs, o.assets, NULL);
        if (vfs_resolve(&fs, "RAL.EXE", p_img, sizeof p_img) != 0)
            die_missing("the game image (or give --image)", p_img);
    }
    if (finish_consts_read(p_img, &k, err, sizeof err) != 0) {
        fprintf(stderr, "rally: %s: %s\n", p_img, err);
        return 1;
    }
    /* 0x00456F42's prologue: [shauntracknum] from the table; one player,
     * one screen, gmode 3, the view CHASE 3 (car 1 +0x684 = 6) */
    snprintf(stage, sizeof stage, "LEVEL%02d", (int)k.level[o.finish]);
    o.stage = stage;
    o.gmode = 3;
    o.direction = 0;
    G.finish = 1;
    G.fw_finish = fw;
    G.load_gmode = 3;
    G.track = k.level[o.finish];
    G.carparts = o.carparts;
    G.cheats = o.cheats;
    G.cam_view = 6;
    printf("rally: the FINISH SCENE (0x00456F42): [0x70D068] = %ld -> "
           "%s (the table at 0x005FE9DC), car %ld, fireworks %s; the scene "
           "runs %.0f s or until ESC / RETURN\n", o.finish, stage, o.car,
           fw ? "ON ([0x70D074] = 1)" : "off",
           (double)k.dur[fw]);
    load(&o);
    /* shauniniteverything: the race's sound mode 0x0040EE51 stops the
     * music, and with [0x005FE9D8] set `doingamemusic` takes 0x0046DBD3's
     * arm - track 6 with the fireworks, else 5, once, at incidental music
     * (0x0040F5EB / 0x0040F5D9).  The car's own sample sets are not loaded
     * (0x0040ED6C), so the car is silent: the port arms none. */
    snd_music_stop(&S);
    snd_cue(&S, fw ? 6 : 5, 0, S.set.music_menu);
    /* raceinit: the placement with no route (index 0, no 2 m back), the
     * leveller, resettimers and the settle.  gmode 3 places and settles
     * exactly as gmode 0 for slot 0 with no input (place.c's branches are on
     * gmode 1, 2 and 4 and slot > 0; the settle's gates on 1 and 2), so the
     * start runs in 0, the mode the port's race start accepts. */
    G.live = 0;
    cd_restart = 0;                    /* no co-driver in the finish (as race()) */
    seek(0);
    y_settled = G.car.pos[1];
    /* 0x0045718C: car 1's y += 0.6 m - FLD, FMUL 65536, FIADD, FISTP */
    {
        long double y = (long double)k.lift * (long double)k.k65536
                      + (long double)G.car.pos[1];
        G.car.pos[1] = (int32_t)llrintl(y);
    }
    printf("rally: the car placed without a route at %.3f %.3f %.3f m, "
           "settled, then lifted %+.4f m (0x0045718C); it is not stepped "
           "again\n", G.car.pos[0] / 65536.0, y_settled / 65536.0,
           G.car.pos[2] / 65536.0, (G.car.pos[1] - y_settled) / 65536.0);

    /* the fireworks' whoosh and fwboom (0x0040E247): the audio thread's two
     * flags, cleared each frame by snd_world_fw.  The finish keeps the car
     * silent (0x0040ED6C) and the port has never run the world's ambience
     * here, so the thread's tick (snd_cd_tick, which would run the car's
     * record and the whole world) stays unarmed.  The thread's generator
     * (0x004018A0) is a STAND-IN seeded from the weather's, never 0 (the
     * stand-in's fixed point) */
    if (fw && have_fw && have_ct && have_world && rep.have_race) {
        W.fw = &FW.whoosh;
        W.rnd = (FW.lcg ? *FW.lcg : 1u) | 1u;
        C.log = S.log;                   /* the --sound-log's request lines  */
    }
    memset(G.fold_down, 1, sizeof G.fold_down);   /* no key "pressed" at once */
    set_pad_ctx(PADK_WAIT);
    t0_ms = (o.frame_ms || o.jitter) ? 0u : (uint32_t)(plat_time_us() / 1000u);
    while (!quit) {
        unsigned char jp[0x80];
        float tw, fr;
        plat_input_poll();
        if (plat_quit_requested()) { app_quit = 1; break; }
        snd_tick(&S);
        /* FUN_0041EE8E: the just-pressed scan codes, [0x004F1CA4] */
        fold_scan(jp);
        /* FUN_004572B6: the clock in seconds since the loop began */
        if (o.frame_ms || o.jitter) {
            uint32_t inc = (uint32_t)(o.jitter
                ? jitter_ms[frames % (long)(sizeof jitter_ms / sizeof jitter_ms[0])]
                : o.frame_ms);
            if (frames > 0) {
                fake_ms += inc;
                plat_audio_advance(inc);
            }
            now_ms = fake_ms;
        } else now_ms = (uint32_t)(plat_time_us() / 1000u);
        snd_ms = now_ms;
        if (W.fw) {                    /* 0x0040E247, the thread's weather */
            C.clock = now_ms - t0_ms;
            snd_world_fw(&W);
        }
        S.now = now_ms - t0_ms;        /* the --sound-log's stamp: the scene's ms */
        t = (float)(int32_t)(now_ms - t0_ms) / k.ms;
        tw = t;
        if (k.warp0 <= t) {
            if (k.warp1 <= t)
                tw = (k.warp1 - k.warp0) * k.half + k.warp0;
            else {
                float f = (t - k.warp0) / (k.warp1 - k.warp0);
                tw = -((k.warp1 - k.warp0) * k.half * f * f) + t;
            }
        }
        fr = 0.0f;
        if (fw && k.fw0 <= t) {
            fr = 1.0f;
            if (t < k.fw1) fr = (t - k.fw0) / (k.fw1 - k.fw0);
        }
        if (fw && k.fw0 <= t) fw_on = 1;                /* 0x004573F1       */
        if (fw && k.fw0 <= t && !fireworks_said) {
            printf("rally: %.2f s - [0x005787E4] = 1, the fireworks start "
                   "here%s\n", (double)t, have_fw ? "" : " (NOT LOADED)");
            fireworks_said = 1;
        }
        /* 0x004571F7: FLD t / FCOMP dur[[0x70D074]] - out unless t < dur
         * (a NaN leaves too); or ESC [0x004F1CA5], RETURN [0x004F1CC0] */
        if (!(t < k.dur[fw]) || jp[0x01] || jp[0x1C]) {
            by_key = t < k.dur[fw];
            break;
        }
        /* 0x00457249: view 6's block, every frame, then FUN_0044DEE4 */
        G.camx.block[6][0] = 0.0f;
        G.camx.block[6][1] = fr * k.y_fw + tw * k.y_k + k.y0;
        G.camx.block[6][2] = tw * k.z_k + k.z0;
        G.camx.block[6][6] = fr * k.pitch_fw;
        G.camx.block[6][7] = tw * k.yaw_k;
        G.camx.cam.view = G.camx.cam.nview = 6;    /* [0x006C2794] = 6 */
        if (sim_camera_step_q(&G.camx, &G.car, rep.binary_angle, 0.0f, NULL,
                              &L.geom[0], L.sin_tab) != 0)
            cam_bad++;
        for (i = 0; i < 3; i++)
            if (G.camx.cam.pos[i] == (int32_t)0x80000000) cam_bad++;
        if (!isfinite(G.camx.cam.fishval)) cam_bad++;
        /* FUN_00452103: one frame */
        wfx_clock = now_ms;
        wfx_advance = 1;
        render();
        while (nextshot < o.nshots && frames >= o.shot_frame[nextshot]) {
            if (frames == o.shot_frame[nextshot])
                write_ppm(o.shot_path[nextshot]);
            nextshot++;
        }
        plat_video_present();
        frames++;
        if (o.steps && frames >= o.steps) break;
    }
    printf("rally: the finish scene ended at %.2f s after %ld frames, %s; the "
           "camera %.2f %.2f %.2f m, yaw %.4f turn; %d camera steps refused "
           "or non-finite\n", (double)t, frames,
           by_key ? "by a key" : o.steps && frames >= o.steps ? "at --steps"
           : "on time", G.camx.cam.pos[0] / 65536.0,
           G.camx.cam.pos[1] / 65536.0, G.camx.cam.pos[2] / 65536.0,
           (double)(uint32_t)G.camx.cam.yaw / 4294967296.0, cam_bad);
    /* the callers' tail (0x004841D9..0x004841DE and the two others): stop,
     * and after the frontend reloads, stop again and track 3, looping */
    if (!fin_menus) {                  /* from the menus the routine's own tail */
        snd_music_stop(&S);            /* (0x0040F639, 0x0040F5A3 hooked in     */
        snd_music_stop(&S);            /* frontend.c) does this, after we return */
        snd_cue_after_race(&S);
    }
    if (G.have_hud) hud_free(&G.hud);
    snd_cd_off(&S);
    if (have_ct) { snd_car_hush(&C); C.rec.on = 0; }
    if (have_world) snd_world_hush(&W);
    S.race_tick = NULL;
    S.world_tick = NULL;
    W.fw = NULL;
    have_fw = 0;
    G.fw_finish = 0;
    return cam_bad ? 1 : 0;
}

/* ----: the menus ---------------------------------------------------- */

/* Everything load() made, so the next race can load another stage in the same
 * process.  race() has already freed the HUD and closed the dump. */
static void unload(void)
{
    unsigned k;
    long reseed = G.reseed, gearbox = G.gearbox;
    int live = G.live, w = G.w, h = G.h, video = G.video,
        nocull = G.car_nocull, have_script = G.have_script;
    car_script script = G.script;

    for (k = 0; k < pc.n; k++)
        if (pc.tex[k]) plat_gfx_tex_destroy(pc.tex[k]);
    plate_cache_free(&pc);
    objscene_free(&objs);
    stage_free(&stg);
    sim_level_free(&L);
    memset(&pc, 0, sizeof pc);
    memset(&objs, 0, sizeof objs);
    memset(&stg, 0, sizeof stg);
    memset(&rep, 0, sizeof rep);
    memset(&G, 0, sizeof G);
    G.reseed = reseed; G.gearbox = gearbox; G.live = live; G.w = w; G.h = h;
    G.video = video; G.car_nocull = nocull; G.have_script = have_script;
    G.script = script;
    G.nan_step = -1;
    objscene_set_no_reflect(g_cli_no_reflect);   /* the menus' own */
}

/* One race from the menus: what they chose, onto the command line's own
 * path - so a race from the menus and `--stage --car --direction --gearbox`
 * with the same values start from the same car record (the Done-when). */
static const char *pending_view;           /* view_from_menus' file */

static int race_from_menus(fe *F, const options *op)
{
    fe_launch c;
    options o = *op;
    int rc;
    fe_launch_get(F, &c);
    o.stage = c.level;
    o.stage_given = 1;
    o.car = c.carnum;
    o.direction = c.direction;
    o.gearbox = c.gearbox;
    G.gearbox = c.gearbox;
    o.gmode = c.gmode;                     /* the load's mode */
    o.carparts = c.carparts;
    o.champ88 = c.champ88;
    o.cheats = c.cheats;
    {                                      /* [0x70CC38]'s records */
        static sim_ghost_rec gh[SIM_GHOSTS];
        int k;
        for (k = 0; k < SIM_GHOSTS; k++) {
            gh[k].name = c.ghosts[k].name;
            gh[k].type = c.ghosts[k].type;
            gh[k].total = c.ghosts[k].total;
        }
        o.ghosts = c.have_ghosts ? gh : NULL;
    }
    {                                      /* the wear      */
        static wear_block wb;
        wb = c.wear;
        o.wear = c.have_wear ? &wb : NULL;
    }
    if (c.gmode == 2) {                    /* graham_to_shaun's */
        o.arc_level = c.rally;             /* [0x70CF64]               */
        o.arc_stage = c.stage;             /* [0x70CF60]               */
        o.arc_start = c.arcadepo;          /* [0x70CF6C]               */
    }
    o.racenum = c.racenum;                 /* [0x70CD88]               */
    o.gearbox2 = c.gearbox2;               /* [0x70CF04]               */
    if (c.has_setup) {
        o.su[0] = c.tyre; o.su[1] = c.bbal; o.su[2] = c.seth;
        o.su[3] = c.sets; o.su[4] = c.stes; o.su[5] = c.setg;
        o.has_setup = 1;
    }
    G.from_menu = 1;
    /* the original records every race (fmt_replay_start_record) to
     * var\tmprpl.  Here only where the menus' saves are on (the same test as
     * fe_set_user's): a scripted walk without --user-dir must not write a
     * half-megabyte file into the working directory. */
    if (!o.record && !o.no_record && !o.no_save &&
        (o.user_dir || !(o.menu_keys || o.menu_shot))) {
        static char rp[1200], dir[1100];
        snprintf(dir, sizeof dir, "%s%svar", o.user_dir ? o.user_dir : "",
                 o.user_dir ? "/" : "");
        plat_dir_create(dir);
        snprintf(rp, sizeof rp, "%s/tmprpl", dir);
        o.record = rp;
    }
    G.retire_all = c.retire_all;           /* [0x5D6774] */
    G.no_flare = !c.lens_flare;            /* [0x5D675C]        */
    G.vis_set = 1;                         /* [0x5D6760..0x5D676C] */
    G.vis_reflect = c.reflections != 0;
    G.vis_shadow = c.car_shadows != 0;
    G.vis_detail = c.car_detail;
    G.vis_headlights = c.headlights == 1;  /* 0x0044D242: == 1, not != 0 */
    printf("rally: VISUAL OPTIONS: reflections %s, car shadows %s, "
           "car detail %d, headlights %s\n", G.vis_reflect ? "on" : "off",
           G.vis_shadow ? "on" : "off", G.vis_detail,
           G.vis_headlights ? "every car" : "players' cars only");
    G.hud_mask = c.hud_mask;               /* car+0x220 */
    G.has_hud_mask = 1;
    G.menu_view = c.camera;                /* player+0x0C       */
    G.has_menu_view = 1;
    car_input_set_axis_mode(c.axismode);   /* player+0x34 = jstype */
    G.gmode = c.gmode;                     /* [0x5D6750] */
    car_input_set_keys(c.keys);            /* 0x00480DA7: the player's keys */
    car_input_set_joymap(c.joymap);        /* 0x00480D4F: and pad mapping */
    /* the player's co-driver (player+0x154), unless --codriver */
    S.set.voice[0] = op->codriver >= 0 ? op->codriver : c.codriver;
    /* SETUP CONTROLLER's dead zone (player +0x44 / +0x40), unless
     * --joy-dz; AUDIO SETUP's in-game music, unless --in-game-music */
    if (!op->joy_dz) car_input_set_deadzone(c.lowerdz, c.upperdz);
    S.set.race_music_on = op->in_game_music ? 1 : c.race_music_on;
    printf("rally: the menus launch gmode %d: rally %d stage %d -> track %d "
           "(%s), direction %d, car %d, gearbox %d, setup %d,%d,%d,%d,%d,%d - "
           "the same race as --stage %s --car %d --direction %d --gearbox %d "
           "--setup %d,%d,%d,%d,%d,%d\n",
           (int)c.gmode, (int)c.rally, (int)c.stage, (int)c.track, c.level,
           (int)c.direction, (int)c.carnum, (int)c.gearbox,
           (int)c.tyre, (int)c.bbal, (int)c.seth, (int)c.sets, (int)c.stes,
           (int)c.setg, c.level, (int)c.carnum, (int)c.direction, (int)c.gearbox,
           (int)c.tyre, (int)c.bbal, (int)c.seth, (int)c.sets, (int)c.stes,
           (int)c.setg);
    if (c.gmode == 1)                      /* the service's penalty */
        printf("rally: the service's penalty handed to the race: player+0x288 = %d "
               "-> car+0x88 catitken (%d s on the stage clock)\n",
               (int)c.champ88, (int)(c.champ88 / 100));
    {
        int lo, up;
        car_input_get_deadzone(&lo, &up);
        printf("rally: the menus' options: dead zone %d,%d (%s), HUD mask "
               "0x%X (VISUAL PREFERENCES), in-game music %s, co-driver %s\n", lo, up,
               op->joy_dz ? "--joy-dz" : "SETUP CONTROLLER", (unsigned)c.hud_mask,
               S.set.race_music_on ? "on" : "off", S.set.voice[0] ? "female" : "male");
    }
    printf("rally: PLAYER 1 DETAILS / SETUP CONTROLLER: starting view %d, "
           "car number %d, steering sensitivity %d (the slider's start; the "
           "race's ste s %d), axis mode %d (%s), device %d (the pad stays live: "
           "the device list is a stand-in), shifter %d (not read)\n",
           (int)c.camera, (int)c.racenum, (int)c.sens, (int)c.stes,
           (int)c.axismode, car_input_jstype(c.axismode) == 1 ? "digital" : "analogue",
           (int)c.device, (int)c.shifter);
    if (c.gmode == 3)
        printf("rally: gmode 3 (time trial) races as a single stage here: its ten "
               "rivals and per-checkpoint splits are not ported\n");
    if (c.gmode == 1)
        printf("rally: gmode 1 (championship) races under its own rules (the "
               "false start and the position against its 90 rivals as ghosts "
               "included); the standings are the menus' own, the "
               "original's code\n");
    if (c.gmode == 2)
        printf("rally: gmode 2 (arcade): level %d stage %d, the last stage's "
               "position %d - against the clock and two streamed "
               "rivals\n", (int)c.rally, (int)c.stage, (int)c.arcadepo);
    fflush(stdout);
    if (pending_view) {                    /* the menus' viewer, not a race */
        o.view = pending_view;
        pending_view = NULL;
        if (prep_view(&o) != 0) { unload(); return 1; }
    }
    rc = race(&o);
    if (view_on) { view_on = 0; rplrd_close(&vrd); }
    unload();
    return rc;
}

/* FE_VIEW - the same race setup as a launch (the menus' visual options,
 * sound and window), with the replay's own stage, car and mode over it */
static int view_from_menus(fe *F, const options *op)
{
    pending_view = fe_view_path(F);
    return race_from_menus(F, op);
}

/* 0x004264D7, the finish routines' scene, from the menus'
 * interpreter (frontend.c): the scene as `--finish N` runs it, over the same
 * options, then everything it loaded freed as a race's is - the routine's own
 * tail goes on from here (the music's stop and track 3) */
static void finish_scene_cb(void *ud, long scene, long car, long racenum,
                            int fireworks)
{
    options o = *(const options *)ud;
    int live = G.live;                 /* finish() runs in mode 0, and unload()
                                        * keeps G.live: the next race must not */
    if (scene < 0 || scene > 5) {
        printf("rally: the finish scene [0x70D068] = %ld is not 0..5: left out\n",
               scene);
        return;
    }
    o.finish = scene;
    o.fireworks = fireworks;
    o.car = car;
    o.racenum = racenum;
    o.stage_given = 0;
    fin_menus = 1;
    (void)finish(&o);
    fin_menus = 0;
    unload();
    G.live = live;
}

static unsigned menu_key(char c)
{
    switch (c) {
    case 'U': return FE_UP;    case 'D': return FE_DOWN;
    case 'L': return FE_LEFT;  case 'R': return FE_RIGHT;
    case 'E': return FE_ENTER; case 'X': return FE_ESC;
    }
    return 0;
}

/* a typed key's DirectInput scan code (DIK_*), from a script's
 * character or from an SDL scancode; 0 for none */
static int dik_of_char(char c)
{
    static const unsigned char az[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21,
        0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
        0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (c >= 'a' && c <= 'z') return az[c - 'a'];
    if (c >= '1' && c <= '9') return 0x02 + (c - '1');
    if (c == '0') return 0x0B;
    if (c == ' ') return 0x39;
    if (c == '-') return 0x0C;
    if (c == '.') return 0x34;
    if (c == '<') return 0x0E;                       /* backspace */
    return 0;
}

/* a name typed with no keyboard - the pad's letter picker.  While a
 * name is being typed the original reads only RETURN, ESC and typed keys
 * (su_typing), so the arrows are free: UP / DOWN change the letter being
 * picked (typed as BACKSPACE + the new letter, the original's own 0x0E and
 * character table doing the rest), RIGHT keeps it and moves on, LEFT is
 * BACKSPACE.  A / START are RETURN and B is ESC as everywhere in the menus.
 * The platform layer's addition (the original had a keyboard); the name is
 * still the original's code's. */
static const char pick_set[] = "abcdefghijklmnopqrstuvwxyz0123456789 -.";
static int pick_at = -1;                /* the letter on trial, or -1        */

static unsigned typing_pick(fe *F, unsigned keys)
{
    int n = (int)sizeof pick_set - 1;
    if (keys & (FE_UP | FE_DOWN)) {
        int up = (keys & FE_UP) != 0;
        if (pick_at < 0) pick_at = up ? 0 : n - 1;
        else {
            fe_type(F, 0x0E, 0);                              /* the trial out */
            pick_at = (pick_at + (up ? 1 : n - 1)) % n;
        }
        fe_type(F, dik_of_char(pick_set[pick_at]), 0);   /* as the keyboard's */
    } else if (keys & FE_RIGHT) {
        if (pick_at < 0)                                       /* a space    */
            fe_type(F, 0x39, 0);
        pick_at = -1;
    } else if (keys & FE_LEFT) {
        fe_type(F, 0x0E, 0);
        pick_at = -1;
    }
    return keys & ~(unsigned)(FE_UP | FE_DOWN | FE_LEFT | FE_RIGHT);
}

static int dik_of_sdl(int sc)
{
    if (sc >= 4 && sc <= 29) return dik_of_char((char)('a' + sc - 4));
    if (sc >= 30 && sc <= 38) return 0x02 + (sc - 30);
    switch (sc) {
    case 39: return 0x0B;  case 42: return 0x0E;  case 44: return 0x39;
    case 45: return 0x0C;  case 55: return 0x34;
    }
    return 0;
}

static int write_canvas(const gfx2d_surface *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (i = 0; i < s->w * s->h; i++) {
        unsigned char c[3];
        uint32_t v = s->px[i];
        c[0] = (unsigned char)(v >> 16); c[1] = (unsigned char)(v >> 8);
        c[2] = (unsigned char)v;
        fwrite(c, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* with a window, the menu frame as it is shown - the canvas, then the
 * 3D models over it (fe_draw_3d) - read back from GL.  Without one, the canvas
 * alone, which is what --no-window always wrote: the models need a context. */
/* A menu screen: drawn for 4:3, so on a wider frame it sits in the middle
 * with black either side (plat_gfx_pillar) instead of stretching. */
static void menu_draw(fe *F)
{
    int w = G.w * 3 > G.h * 4 ? G.h * 4 / 3 : G.w;
    if (w != G.w) plat_gfx_pillar((G.w - w) / 2, w);
    gfx2d_present(fe_canvas(F), w, G.h);
    fe_draw_3d(F, w, G.h);
    if (fe_post(F)) gfx2d_present_over(fe_post(F), w, G.h);
    if (w != G.w) plat_gfx_pillar(0, 0);
}

static int menu_shot(fe *F, const char *path)
{
    int r;
    /* a window at another size than the canvas is shot as SHOWN -
     * the 800x600 canvas scaled - so 640x480 can be looked at */
    if (!fe_has_3d(F) && (!G.video || (G.w == 800 && G.h == 600)))
        return write_canvas(fe_canvas(F), path);
    if (!G.video) {
        /* headless: the draws run and are recorded, plat.h drops them; the
         * canvas is written as before (menu_models.sh reads the record) */
        fe_draw_3d(F, 800, 600);
        fe_report_3d(F, stdout);
        return write_canvas(fe_canvas(F), path);
    }
    plat_video_frame_begin();
    menu_draw(F);
    fe_report_3d(F, stdout);
    r = write_ppm(path);
    plat_video_present();
    return r;
}

static int menus(options *op)
{
    static vfs fs;
    char err[256], p_img[1024];
    fe *F;
    int rc = 0;

    vfs_init(&fs, op->assets, NULL);
    if (op->image) snprintf(p_img, sizeof p_img, "%s", op->image);
    else if (vfs_resolve(&fs, "RAL.EXE", p_img, sizeof p_img) != 0)
        die_missing("the game image (or give --image)", p_img);
    F = fe_open(op->assets, p_img, err, sizeof err);
    if (!F) {
        fprintf(stderr, "rally: the menus will not load: %s\n", err);
        return 1;
    }
    fe_set_scene_cb(F, finish_scene_cb, (void *)op);      
    /* a script writes nothing unless it names where */
    fe_set_user(F, op->user_dir,
                !op->no_save && (op->user_dir || !(op->menu_keys || op->menu_shot)));
    if (fe_load_saves(F, !op->no_ral_dat))
        printf("rally: var\\ral.dat read: player 1's car and gearbox are the saved ones "
               "(--no-ral-dat for the image's)\n");
    if (op->unlock_a8) fe_unlock_a8(F);                   
    fe_set_sound(F, &S);                                  
    if (op->vm_census) {                                  
        int none = fe_vm_census(F, op->vm_census, stdout);
        fe_close(F);
        return none < 0 ? 1 : 0;
    }
    if (op->in_game_music) S.set.race_music_on = 1;       /* the stand-in */
    if (op->codriver >= 0) S.set.voice[0] = op->codriver; /* the stand-in */
    printf("rally: the menus - the original's screens from %s and "
           "the install's art and 3D models\n", p_img);

    /* A script: one key a second on a fake clock, then the frame --menu-ms
     * after the last key.  A launch with no --menu-shot runs the race. */
    if (op->menu_keys || op->menu_shot) {
        const char *k = op->menu_keys ? op->menu_keys : "";
        uint32_t t = 1000, audio_t = t;
        int r;
        if (op->quit_confirm) fe_set_quit_confirm(F, 1);
        r = fe_frame(F, 0, t);
        for (; *k && r != FE_QUIT; k++) {
            unsigned b = menu_key(*k);
            if (!b && dik_of_char(*k) && (fe_typing(F) || fe_awaits_key(F))) {
                /* typed: a key awaited */
                fe_type(F, dik_of_char(*k), 0);
                t += 1000;
                r = fe_frame(F, 0, t);
                continue;
            }
            if (!b) continue;
            if (fe_typing(F) && (b & (FE_UP | FE_DOWN | FE_LEFT | FE_RIGHT))) {
                typing_pick(F, b);                         /* the pad's */
                t += 1000;
                r = fe_frame(F, 0, t);                     /* one typed key a */
                r = fe_frame(F, 0, t + 500);               /*   frame: two    */
                continue;
            }
            if (!fe_typing(F)) pick_at = -1;
            t += 1000;
            plat_audio_advance(t - audio_t);       /* U1: the script's clock */
            audio_t = t;
            snd_tick(&S);
            r = fe_frame(F, b, t);
            /* A launch mid-script (no --menu-shot) runs the race, comes back
             * to SELECT STAGE and goes on with the script: two races in one
             * process is what tests unload() (rally-menus). */
            if (r == FE_LAUNCH && k[1]) {
                rc |= race_from_menus(F, op);
                if (op->has_result) {
                    g_result.finished = 1; g_result.time_cs = op->result_cs;
                    g_result.time_left_cs = 0;   /* no race ran: the old 0 */
                    /* the best place the stage can give - bestpos,
                     * 15 10 7 5 3 1 - so a scripted level is still won on
                     * stage 6, and each stage starts where a real finish
                     * could leave it (a start of 1 is no stage's) */
                    g_result.arcadepo = A.bestpos > 0 ? A.bestpos : 1;
                    g_result.speed_x10 = op->result_mph * 10;
                }
                fe_after_race(F, t, &g_result);
                r = FE_RUN;
            } else if (r == FE_LAUNCH) {
                break;
            } else if (r == FE_VIEW && k[1]) {          /* the replay viewer, then on */
                rc |= view_from_menus(F, op);
                fe_after_view(F, t);
                r = FE_RUN;
            } else if (r == FE_VIEW) {
                break;
            }
        }
        /* a screen with models gets the frames the original's loop
         * would have drawn, one each 100 ms (the 0x004664C9 clamp), so a
         * carousel slide started by the last key has finished by the shot.
         * Screens without models keep the one frame they always had. */
        if (r == FE_RUN && fe_has_3d(F)) {
            long ms;
            for (ms = 100; ms < op->menu_ms; ms += 100)
                fe_frame(F, 0, t + (uint32_t)ms);
        }
        if (r == FE_RUN) fe_frame(F, 0, t + (uint32_t)op->menu_ms);
        printf("rally: menu script done: %s%s\n", fe_screen_name(F),
               r == FE_LAUNCH ? ", LAUNCH" : r == FE_QUIT ? ", QUIT" : "");
        /* the menus' cost - what one frame of this screen asks of the
         * interpreter (x86vm.c) and of the draw.  The frames are on the fake
         * clock, after --menu-ms; the timings are the host's real ones. */
        if (op->menu_cost > 0 && r == FE_RUN) {
            uint64_t s0 = fe_vm_steps(F), smax = 0, us_fe = 0, us_draw = 0;
            uint64_t us_fe_max = 0;
            long i;
            for (i = 0; i < op->menu_cost; i++) {
                uint64_t a = fe_vm_steps(F), t0 = plat_time_us(), t1;
                fe_frame(F, 0, t + (uint32_t)op->menu_ms + 16u * (uint32_t)(i + 1));
                t1 = plat_time_us();
                if (fe_vm_steps(F) - a > smax) smax = fe_vm_steps(F) - a;
                if (t1 - t0 > us_fe_max) us_fe_max = t1 - t0;
                us_fe += t1 - t0;
                if (G.video) {
                    plat_video_frame_begin();
                    menu_draw(F);
                    us_draw += plat_time_us() - t1;
                    plat_video_present();
                }
            }
            printf("rally: menu cost: %s  %ld frames  instructions/frame %.0f "
                   "(max %llu)  fe_frame %.3f ms (max %.3f)  draw %.3f ms\n",
                   fe_screen_name(F), op->menu_cost,
                   (double)(fe_vm_steps(F) - s0) / (double)op->menu_cost,
                   (unsigned long long)smax,
                   (double)us_fe / 1000.0 / (double)op->menu_cost,
                   (double)us_fe_max / 1000.0,
                   (double)us_draw / 1000.0 / (double)op->menu_cost);
        }
        if (op->menu_shot && strstr(op->menu_shot, "%d")) {
            /* every highlight phase, 0..511 ms after the last key: the
             * screenshot's own phase is unknown (tools/menuart.py fit) */
            uint32_t ms, i;
            char path[1024];
            for (i = 0; i < 512 && r == FE_RUN; i++) {
                /* with --menu-step, phase i is --menu-ms + i * step
                 * and the file is named by that ms - the models' yaw */
                ms = op->menu_step > 0
                   ? (uint32_t)op->menu_ms + i * (uint32_t)op->menu_step : i;
                fe_frame(F, 0, t + ms);
                snprintf(path, sizeof path, op->menu_shot, (int)ms);
                if (menu_shot(F, path) != 0) rc = 1;
            }
            printf("rally: 512 phases written to %s\n", op->menu_shot);
        } else if (op->menu_shot) {
            if (menu_shot(F, op->menu_shot) == 0)
                printf("rally: the menu written to %s\n", op->menu_shot);
            else
                rc = 1;
        } else if (r == FE_LAUNCH) {
            rc = race_from_menus(F, op);
        } else if (r == FE_VIEW) {
            rc = view_from_menus(F, op);                 
        }
        fe_close(F);
        return rc;
    }

    {
        static const int sc[6] = { 82, 81, 80, 79, 40, 41 };   /* SDL codes */
        static const unsigned bit[6] = { FE_UP, FE_DOWN, FE_LEFT, FE_RIGHT,
                                         FE_ENTER, FE_ESC };
        unsigned char down[6], tdown[64], fdown[0x80];
        int i, first = 1;
        /* the menus' rate line, once a second of real time, as the
         * race has: frames, the interpreter's share and its instructions */
        uint64_t st_us = plat_time_us(), st_fe = 0, st_draw = 0;
        uint64_t st_steps = fe_vm_steps(F), st_max = 0;
        long st_frames = 0;
        memset(down, 0, sizeof down);
        memset(tdown, 0, sizeof tdown);
        memset(fdown, 0, sizeof fdown);
        fe_set_quit_confirm(F, !op->no_quit_confirm);   /* the port's guard */
        while (!app_quit) {
            unsigned keys = 0;
            int r;
            uint32_t now = (uint32_t)(plat_time_us() / 1000u);
            plat_input_poll();
            if (plat_quit_requested()) break;
            snd_tick(&S);                       /* AUDIOTDB's service */
            set_pad_ctx(PADK_MENU);                
            for (i = 0; i < 6; i++) {
                int d = G.video ? key_down(sc[i]) : 0;
                if (i == 4 && G.video) d |= plat_input_key(88);   /* KP ENTER */
                if (d && !down[i] && !first) keys |= bit[i];
                down[i] = (unsigned char)d;
            }
            /* a slot's name being typed takes the letter keys */
            for (i = 4; i < 64 && G.video; i++) {
                int d = plat_input_key(i), dik = dik_of_sdl(i);
                if (dik && d && !tdown[i] && !first && fe_typing(F))
                    fe_type(F, dik, plat_input_key(225) || plat_input_key(229));
                tdown[i] = (unsigned char)d;
            }
            /* REDEFINE KEYBOARD waits for any key - each new press,
             * as its folded code (car_keys.h's table, read backwards) */
            for (i = 1; i < 0x80 && G.video; i++) {
                int d = car_key_sdl[i] ? plat_input_key(car_key_sdl[i]) : 0;
                if (d && !fdown[i] && !first && fe_awaits_key(F)) fe_type(F, i, 0);
                fdown[i] = (unsigned char)d;
            }
            first = 0;
            if (fe_typing(F)) keys = typing_pick(F, keys);   
            else pick_at = -1;
            {
                uint64_t a = fe_vm_steps(F), t0 = plat_time_us(), t1;
                r = fe_frame(F, keys, now);
                t1 = plat_time_us();
                if (fe_vm_steps(F) - a > st_max) st_max = fe_vm_steps(F) - a;
                st_fe += t1 - t0;
                plat_video_frame_begin();
                menu_draw(F);
                st_draw += plat_time_us() - t1;
                plat_video_present();
                st_frames++;
            }
            {
                uint64_t now_us = plat_time_us();
                if (now_us - st_us >= 1000000u) {
                    double sec = (double)(now_us - st_us) / 1e6;
                    double n = st_frames ? (double)st_frames : 1.0;
                    printf("rally: menus  %-12s %5.1f fps  instructions/frame %7.0f "
                           "(max %llu)  fe_frame %.2f ms  draw %.2f ms\n",
                           fe_screen_name(F), st_frames / sec,
                           (double)(fe_vm_steps(F) - st_steps) / n,
                           (unsigned long long)st_max,
                           st_fe / 1000.0 / n, st_draw / 1000.0 / n);
                    fflush(stdout);
                    st_us = now_us;
                    st_fe = st_draw = st_max = 0;
                    st_steps = fe_vm_steps(F);
                    st_frames = 0;
                }
            }
            if (r == FE_QUIT) break;
            if (r == FE_LAUNCH) {
                gfx2d_present_release();
                rc = race_from_menus(F, op);
                fe_after_race(F, (uint32_t)(plat_time_us() / 1000u), &g_result);
                first = 1;                      /* a held key is not a press */
            }
            if (r == FE_VIEW) {                 
                gfx2d_present_release();
                rc = view_from_menus(F, op);
                fe_after_view(F, (uint32_t)(plat_time_us() / 1000u));
                first = 1;
            }
        }
        gfx2d_present_release();
    }
    fe_close(F);
    return rc;
}
