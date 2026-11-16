/* car_load.h - THE CAR'S LOAD: what fills the record before raceinit_q's
 * placement; replaces the car_setup_standin.
 *
 * The game builds car 0's configuration words in this order, and so does
 * car_load - each line names the writer:
 *
 *   graham_to_shaun   FUN_00444F87   the sliders (+0x20 +0x28 +0x30 +0x34
 *                                    +0x3C, x 2.56f, brake balance inverted),
 *                                    the HUD mask +0x220, carpartsleft +0x298
 *                                    (= player +0x278), the tyre into
 *                                    [0x5D64C8] (not the car)
 *   init_car_setup_q  0x00420240     +0x1DC maxrevs = the handling record's
 *                                    +0x74; +0x1E0 = [0x005C6C44], 0.85
 *   load objects      0x00425BEA     +0x260 numcollpts (the collision model)
 *   gamemodestuff_init FUN_00434B2D, by gmode:
 *     FUN_004363C3   (gmodes 0, 2)   the L table's row for the track into
 *                                    [0x005B5794..]; other modes keep the
 *                                    last such race's (never acted on)
 *     FUN_00434FC3   (every mode)    +0x25C driver = slot < numcars, +0x38
 *                                    autom = 1, +0x8C = [0x5B5794] x 100,
 *                                    a run of zeroes, the car numbers +0x24
 *     FUN_00435148   (every mode)    +0x24 car, +0x2C tyre [0x5D64C8], +0x38
 *                                    the gearbox [0x5D6744]
 *     FUN_0043595A   (gmodes 0, 2)   THE AUTO-SETUP: the sliders and tyre
 *                                    from a 36-stage table at 0x005B8758,
 *                                    over the player's - in a single stage
 *                                    and the arcade the player's setup is
 *                                    never used
 *     gmode 1 only                   +0x88 = [0x70CF70] (player +0x288)
 *
 *     FUN_00436222   (gmode 2)       the arcade's bonus on section 0 and its
 *                                    car factor on every section
 *
 * Every table is read from the user's RAL.EXE (car_load_tables_read): none is
 * transcribed here.  NOT PORTED, and said where it matters: the
 * multiplayer relay's ordering ([0x524038], net play is deferred).  The
 * arcade's rivals' slots are car_load_rival. */
#ifndef RALLY_GAME_CAR_LOAD_H
#define RALLY_GAME_CAR_LOAD_H

#include "car.h"
#include "car_params.h"
#include "image.h"
#include "physics.h"
#include "setup.h"

#define CAR_LOAD_STAGES 36

typedef struct {
    /* 0x005B8758: 36 records of four dwords - set h, set s, the tyre, the
     * gearing + 1 - indexed ((t - 21) / 10) * 6 + (t - 21) % 10 */
    int32_t autosetup[CAR_LOAD_STAGES][4];
    /* 0x005B8B14: the per-section allowance table, ASCII, 0x708 bytes */
    unsigned char ltab[0x708];
    /* [0x005C6C44] */
    float   target_revs;
    /* [0x005B5794..] at boot: the image's .data (all 256 dwords) */
    int32_t allow_boot[SIM_RACE_ALLOWANCE_N];
    /* FUN_00436222's: 0x005B8A28 the time percentage by stage
     * ordinal, 0x005B8AB8 the per-car factor (floats) */
    int32_t pct[CAR_LOAD_STAGES];
    float   carfactor[23];
} car_load_tables;

/* 0, or -1 with `err` filled. */
int car_load_tables_read(const ral_image *im, car_load_tables *t, char *err,
                         size_t n);

/* What the launch hands the race, as the game's globals hold it. */
typedef struct {
    int32_t gmode;              /* [0x5D6750]                                 */
    int32_t track;              /* [0x5D671C] shauntracknum - LEVELnn's nn     */
    int32_t numcars;            /* [0x5F0A50]                                 */
    int32_t carnum;             /* [0x5D6724] shauncarnum1                    */
    int32_t gearbox;            /* [0x5D6744] <- [0x70CD98] <- player +0x30   */
    /* The player's setup, in menu units (player +0x10C..+0x120) */
    setup_sliders sliders;
    uint32_t hud_mask;          /* car +0x220 as graham_to_shaun builds it    */
    int32_t carparts;           /* [0x70CF80] <- player +0x278                */
    int32_t champ88;            /* [0x70CF70] <- player +0x288 (gmode 1)      */
    /* [0x005B5B94], initcheckpoints_q's at level load (sim_race_init):
     * FUN_00436222's loop count */
    int32_t numcheckposits;
} car_load_in;

/* What a new player record holds - a new career's sliders (FUN_0046F209,
 * 0x0046F2A1: 3, 50, 70, 30, steering +0x280's 50, 2), every part
 * (0x00482FE1: -1), the HUD mask every capture shows (0xF7), gmode 0. */
void car_load_in_default(car_load_in *in);

/* Fill `car` (zeroed first) as the load leaves car 0. */
void car_load(car_state *car, const car_load_in *in, const car_load_tables *t,
              const car_params *p, int32_t numcollpts);

/* an arcade rival's record, slot 1..3, after car_load for car 0 (the
 * allowance is the one car 0's load left): FUN_00434FC3's slot fields with
 * the car `type` (shauncarnum2..4), arcade_game_mode_init's +0x3C = 0x80 and
 * driver 0, FUN_00435148's player-2 tyre and gearbox `gearbox2` for slot 1
 * ([0x5D6748]), and the auto-setup.  `p` is the type's handling. */
void car_load_rival(car_state *car, int slot, int32_t type, int32_t gearbox2,
                    const car_load_in *in, const car_load_tables *t,
                    const car_params *p, int32_t numcollpts);

/* [0x005B5794..], as the last car_load left it: the section allowances, in
 * seconds, that the race's checkpoints pay from (sim_race.allowance) -
 * FUN_004363C3's row and, in gmode 2, FUN_00436222's bonus and car factor.
 * SIM_RACE_ALLOWANCE_N dwords; it persists from race to race, as in the
 * game. */
const int32_t *car_load_allowance(void);

/* FUN_00436222's extra time for `track`, seconds ([0x005B5FB4]), and -1 for
 * a track off its ladder - for the gate. */
int32_t car_load_extra_time(const car_load_tables *t, int32_t track,
                            int32_t numcheckposits, int32_t totatime);

/* FUN_0043595A's index, or -1 when it keeps its presets (the unsigned
 * 20 <= t < 80 fails).  Inside that range but not a stage - 20, 27..30 and
 * the like, which no launch can produce - the original indexes past or into
 * the wrong row; this returns -1 for anything outside the table's 36 rows,
 * and says so. */
int car_load_autosetup_index(int32_t track);

#endif
