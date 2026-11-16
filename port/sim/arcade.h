/* arcade.h - the arcade's rivals: the field, the streamer, the position, the
 * rubber band and the rival driver.
 *
 * Transcribed from retail's bytes this session (tools/ralimg.py disasm);
 * is the reading and section 18 this
 * port, with the corrections the transcription made to the reading:
 *
 *   arcade_game_mode_init  0x00434C2B  the field, the start list, the shuffle
 *   FUN_00447C3D           (setuparcadecars)  the three rival car types
 *   FUN_00460C7D           the driver's memory cleared - not all of it
 *   FUN_00435A90           the streamer, one call a tick from 0x00435191
 *                          (0x0045E5DD, after the car loop and the object
 *                          tick; FUN_0045E3FC, the settle, calls it too)
 *   0x00435FF7 / 0x00436044  "is this marker free"
 *   0x00436065 / 0x00436135  spawn / despawn
 *   FUN_0043618F           the rubber band
 *   FUN_004601BA           the rival driver
 *   FUN_0043895C           0x00438A52..0x00438ADD: the two above for a
 *                          driver-2 car in gmode 2, into the staging struct
 *   FUN_0043D4B3           0x0043D4C7..0x0043D583: the reset's +-64 search
 *
 * THE ORIGINAL'S BUGS, KEPT:
 *  every rival shares slot 0's controller memory ([0x006CEBA8] = 0 at
 *         0x004601EF), and the weave phase is nb & 31 alone;
 *  [0x006CEBAC] is 1 on both arms of 0x00460293, so the first speed
 *         table and the 0x6E / 0x46 lookahead thresholds are dead;
 *  the position's second tie-break compares W0's bloffstz against a
 *         GAP, not slot 0's bloffstz: the first rival's gap helper
 *         (0x00435DF6 / 0x00435E12) overwrites EDX, which held it;
 *  FUN_00460C7D clears the 16-byte position memories as if they were
 *         dwords (the first 13 dwords of each), and never the reverse count
 *         [0x006CEB3C] - it carries from race to race;
 *  the driver's sharp-corner lookahead (nb + 4 when [0x006CEBC0] and
 *         speed >= 0x46) is always overwritten: the next test, speed >= 0x3C,
 *         is true whenever it was, and sets nb + 3.
 *
 * WHAT IS NOT HERE.  [0x006CEF7C] (the credits' computer driver, section
 * 16.5) is not modelled: the override runs only for gmode 2's driver 2.
 * Fields above the 1,668-byte record (+0xAA8 the rival's car number, +0xAB0)
 * are kept in sim_arcade per slot. */
#ifndef RALLY_SIM_ARCADE_H
#define RALLY_SIM_ARCADE_H

#include "car.h"
#include "image.h"

#include <stdint.h>

struct sim_tick;
struct sim_route;
struct sim_ground_geom;
struct sim_input_globals;

#define SIM_ARCADE_LEVELS   6
#define SIM_ARCADE_CARS     23    /* the car order [0x005B9540]              */
#define SIM_ARCADE_FIELD    128   /* N <= 85 ([0x005B89C8]); the lists'
                                   * arrays are 0x5B922C.. and 0x5B9394..,
                                   * 0x168 bytes = 90 dwords each           */
#define SIM_ARCADE_LIST     90
#define SIM_ARCADE_MEM      10    /* the driver's ten-slot arrays            */

/* The image's tables and constants, read from the user's RAL.EXE:
 * none is transcribed into the port. */
typedef struct sim_arcade_consts {
    int32_t numarccars[SIM_ARCADE_LEVELS];   /* 0x005B89C8 */
    int32_t bestpos[SIM_ARCADE_LEVELS];      /* 0x005B89E0, by stage       */
    float   lo[SIM_ARCADE_LEVELS];           /* 0x005B89F8                 */
    float   hi[SIM_ARCADE_LEVELS];           /* 0x005B8A10                 */
    int32_t order[SIM_ARCADE_CARS];          /* 0x005B9540                 */
    float   weave[32];                       /* 0x006CE978, -1..1          */
    uint8_t speed[3][28];                    /* 0x006CE9F8 / A14 / A30     */
    float   band;                            /* [0x005B921C] -0.02         */
    float   push;                            /* [0x005B9220]  0.05         */
    float   thirty;                          /* [0x005B9224] 30.0          */
    float   upright;                         /* [0x006CEA4C]  0.2          */
    float   up[3];                           /* [0x006CEA50] (0, 1, 0)     */
    float   respawn_speed;                   /* [0x0057AF90] 20.0          */
    float   one, zero;                       /* [0x0057AFBC], [0x0057B014] */
    float   fixed;                           /* [0x0057B01C] 65536.0       */
    float   steer_scale;                     /* [0x0057B0B0] 256.0         */
} sim_arcade_consts;

/* 0, or -1 with `err` filled (an address outside the image). */
int sim_arcade_consts_read(const ral_image *im, sim_arcade_consts *k,
                           char *err, size_t n);

/* The process state the arcade keeps, the game's globals by address.  The
 * caller keeps ONE for the process: the driver's reverse count is never
 * cleared, and [0x005C7984] carries. */
typedef struct sim_arcade {
    const sim_arcade_consts *k;

    /* arcade_game_mode_init's */
    int32_t  n;                  /* numarccars [0x005B9514]                  */
    float    lo, hi;             /* [0x005B9504], [0x005B9508]               */
    float    skill_last;         /* [0x005B950C], the last spawn's skill     */
    int32_t  start;              /* arcadestartposition [0x005B951C]         */
    int32_t  bestpos;            /* [0x005B9520]                             */
    int32_t  carno[SIM_ARCADE_LIST];  /* [0x005B9394], the shuffled numbers */
    int32_t  slot[SIM_ARCADE_LIST];   /* [0x005B922C], slot 1..3 per entry  */
    uint32_t rnd_fixed;          /* [0x005C7984], rnd_lfsr_step_fixed's      */
    int32_t  types[3];           /* shauncarnum2..4 [0x005D6728..0x5D6730]   */

    /* the streamer's own globals (slot 0's record holds the rest) */
    int32_t  wi;                 /* [0x005B9500], the entry being spawned    */
    float    spawn_speed;        /* [0x005B9510]                             */
    int32_t  d1, d2, dsum;       /* [0x005B9534], [0x005B9538], [0x005B953C] */

    /* above the record, per slot */
    int32_t  car_number[8];      /* +0xAA8 ctd_carnum                        */
    int32_t  ab0[8];             /* +0xAB0, zeroed by the despawn            */
    int32_t  cls920[8];          /* +0x920, the last step's layer-0 class at
                                  * contact point 0 (the tick copies it)     */

    /* the rival driver's process state, 0x006CEA64.. */
    int32_t  out[6];             /* [0x006CEA64..0x6CEA78]                   */
    float    factor;             /* [0x006CEA80]                             */
    int32_t  stuck4;             /* [0x006CEA7C], read by nothing absolute   */
    int32_t  stuck_t[SIM_ARCADE_MEM];   /* [0x006CEA84]                     */
    int32_t  rev_t[SIM_ARCADE_MEM];     /* [0x006CEAAC]                     */
    int32_t  mark[SIM_ARCADE_MEM];      /* [0x006CEAEC], the corner's marker */
    int32_t  hard[SIM_ARCADE_MEM];      /* [0x006CEB14]                     */
    int32_t  rev_n[SIM_ARCADE_MEM];     /* [0x006CEB3C], never cleared */
    int32_t  worst[SIM_ARCADE_MEM];     /* [0x006CEB64]                     */
    int32_t  pos_new[40];        /* 0x006CEBC4, 16 bytes a slot (x, 0, z, -) */
    int32_t  pos_old[40];        /* 0x006CEC64                               */
    int32_t  speed;              /* [0x006CEAE8]                             */
    int32_t  target;             /* [0x006CEAE4]                             */
    int32_t  turn;               /* [0x006CEAD8]                             */
    int32_t  steer;              /* [0x006CEADC]                             */
    int32_t  moved;              /* [0x006CEAD4]                             */
    int32_t  narrow;             /* [0x006CEB98]                             */
    int32_t  ac;                 /* [0x006CEBAC], 1 always            */
    int32_t  sharp;              /* [0x006CEBC0]                             */
    int32_t  poly_a, poly_b;     /* [0x006CEBB0], [0x006CEBB4]               */
    float    weave_q;            /* [0x006CEBA4], this step's weave          */

    /* counted, not the original's */
    long     spawns, despawns, fell_back, got_ahead, resets_refused;
} sim_arcade;

/* arcade_game_mode_init 0x00434C2B's field half, on records the caller has
 * loaded (FUN_00434FC3 for all four slots, the rivals' +0x3C = 0x80 and
 * driver 0, FUN_00435148).  `level` and `stage` are shaunrallynumber and
 * shaunstagenumber (0..5 each); `entry` is [0x005B5384] (1 the menus' start,
 * 2 a replay or the demo); `carno1` is player 1's +0xAA8.  a->start comes in
 * as graham_to_shaun left it (the previous stage's arcadepo) and is N on
 * stage 1.  `arcrandom` seeds [0x005C7984].  Writes slot 0's streamer
 * fields (P = N - S, the init flag and both windows 0).  0, or -1 when the
 * tables would index past the port's arrays. */
int  sim_arcade_init_q(sim_arcade *a, const sim_arcade_consts *k,
                       int32_t level, int32_t stage, int32_t entry,
                       int32_t carno1, uint32_t arcrandom, car_state *car0);

/* FUN_00447C3D: the three rival car types for player 1's `carnum1`, from
 * [0x005C7984] reseeded with `arcrandom` (the first draw) and rnd_next
 * (`rnd`, [0x004DB13C]; the later two).  Fills a->types. */
void sim_arcade_rival_types_q(sim_arcade *a, const sim_arcade_consts *k,
                              int32_t carnum1, uint32_t arcrandom,
                              uint32_t *rnd);

/* FUN_00460C7D. */
void sim_arcade_driver_clear_q(sim_arcade *a);

/* FUN_00435A90 over the tick's four records.  Spawns place and level a
 * rival with its slot's params, ground and world (sim_tick_car), so every
 * slot the start list names must have them.  0, or the placement's -1. */
int  sim_arcade_stream_q(sim_arcade *a, struct sim_tick *t);

/* FUN_0043895C's override for a driver-2 car in gmode 2: the rubber band
 * on the working car, then the driver, its six outputs into the staging
 * struct.  `slot` is [0x006CE084]; `car0` is slot 0's RECORD (the band and
 * the driver read [0x006C21A8] directly). */
void sim_arcade_drive_q(sim_arcade *a, car_state *car, uint32_t slot,
                        const car_state *car0, const struct sim_route *r,
                        const struct sim_ground_geom *geom,
                        struct sim_input_globals *g);

/* 0x00438ADE, an UNREFERENCED second entry (no CALL, JMP or pointer to it):
 * the driver with factor 1.0 and no rubber band.  The port uses it only as a
 * DIAGNOSTIC autopilot for car 0 (sim_race.autopilot_q, rally's
 * RALLY_AUTOPILOT), so a scripted arcade stage can pass rivals - it shares
 * the rivals' controller memory, so it changes how they drive. */
void sim_arcade_autopilot_q(sim_arcade *a, car_state *car, uint32_t slot,
                            const struct sim_route *r,
                            const struct sim_ground_geom *geom,
                            struct sim_input_globals *g);

/* 0x0043D5A3: carry = the candidate marker is taken by `rival`. */
int  sim_arcade_reset_taken_q(const car_state *rival, int32_t cand,
                              int32_t route_n);

#endif
