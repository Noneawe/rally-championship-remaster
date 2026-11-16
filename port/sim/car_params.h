/* car_params.h - the 32-record car handling table, LOADED AT RUNTIME.
 *
 *
 * WHY THIS IS A LOADER AND NOT A TABLE
 * ------------------------------------
 * The 32 records at 0x0057CB8C in RAL.EXE are the game's tuned handling data:
 * the car names ("00 renault maxi megane", ...), the nine-point torque curve,
 * the gearing endpoints, the grip multipliers and the shift thresholds.  That
 * is copyrighted content of the original, so it is read out of THE USER'S OWN
 * COPY at runtime and never transcribed into a checked-in .c or .h array.
 * is the policy; the own text restates it for this table
 * specifically; port/formats is the pattern this follows.
 *
 * A caller with no image must degrade the way the rest of the tree does -
 * car_params_load() returns an error and the gate SKIPS, saying so, rather
 * than passing on defaults that prove nothing.
 *
 * WHERE THE LAYOUT COMES FROM
 * ---------------------------
 * which is one line of
 * sim_load_car_params (FUN_004201FB) at a time.  Its addressing is:
 *
 *     MOV  EBX,[0x006ccc88]        ; car0+0x24 = carnum1
 *     AND  EBX,0x1f                ; <- an UPPER BOUND FROM THE MASK,
 *     IMUL EBX,EBX,0xac            ;    not a measurement of how many exist
 *     LEA  ESI,[EBX + 0x57cb8c]
 *
 * What makes the reading certain rather than plausible is the data itself:
 * every one of the 32 records begins with a name whose first two characters
 * are that record's own index, "00" through "31".  A wrong base or a wrong
 * stride breaks that immediately, so the table validates the reading.
 * car_params_load() checks it, because a gate that cannot fail checks nothing.
 *
 * THE FIELDS NOT HERE ARE NOT HERE ON PURPOSE. lists every
 * field sim_load_car_params touches and no others; +0x4C is absent because the
 * function does not read it, and +0x50/+0x54/+0x58 are absent because the
 * function's own epilogue OVERWRITES them unconditionally with globals (2.4,
 * 0.5, 4.0).  Storing a per-car value that never survives the load would be
 * inventing a difference the original does not have.
 *
 * tools/carparams.py is the Python twin of this reader.  Keep them agreeing.
*/

#ifndef RALLY_SIM_CAR_PARAMS_H
#define RALLY_SIM_CAR_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#define CAR_PARAMS_TABLE_VA   0x0057CB8Cu
#define CAR_PARAMS_STRIDE     0xACu
#define CAR_PARAMS_COUNT      32u     /* from AND EBX,0x1f - a bound, not a count */
#define CAR_PARAMS_NAME_LEN   0x18u
#define CAR_PARAMS_CURVE_PTS  9u

/* Gravity, [0x005C6CFC] = 9.8.  sim_load_car_params multiplies the two brake
 * fields by it as it loads, which is what turns them into m/s^2.  The thirteen
 * readers of this address all address it absolutely; it is NOT element 4 of
 * the [0x005C6CEC] table it happens to sit next to (s3). */
#define CAR_GRAVITY           9.8f

typedef struct car_params {
    char  name[CAR_PARAMS_NAME_LEN + 1];  /* +0x00, NUL-terminated here      */

    int32_t drive_class;    /* +0x18 -> car0+0x40.  0 = FWD, 1 = 4WD.        */
    int32_t ngears;         /* +0x1C -> [0x5C7944], CLAMPED TO [5,7] ON LOAD */
    int32_t unk20;          /* +0x20 -> [0x5C7948], no reader read           */

    float grip_t;           /* +0x24 -> [0x5C78A8]  yaw grip-limit scale     */
    float powr_scale;       /* +0x28 -> [0x5C78AC]  scales ROTSPDPOWR        */
    float acc_grip;         /* +0x2C -> [0x5C78E8]  GAME'S OWN NAME          */
    float brk_grip;         /* +0x30 -> [0x5C78EC]  GAME'S OWN NAME          */
    float sidegrip;         /* +0x34 -> [0x5C78F0]  GAME'S OWN NAME          */
    float handbrake_decel;  /* +0x38 -> [0x5C78E0]  x GRAVITY on load, m/s^2 */
    float brake_decel;      /* +0x3C -> [0x5C78E4]  x GRAVITY on load, m/s^2 */

    int32_t unk40, unk44, unk48;   /* +0x40/44/48 -> [0x5C78F4/F8/FC]        */

    float drag;             /* +0x60 -> [0x5C74A0], read by the drag term    */

    /* +0x64/+0x6C are first gear at slider 0 / slider 4; +0x68/+0x70 are top
     * gear at the same two.  Road speed in "game mph" at the rev limit.
     * Each is int->float and FLOORED at 8.0 - a floor, not a cap. */
    int32_t first_lo, top_lo, first_hi, top_hi;   /* +0x64 +0x68 +0x6C +0x70 */

    float maxrevs;          /* +0x74 -> car0+0x1DC, FLOORED at 1000.  */

    /* +0x78, RAW.  sim_load_car_params turns it into the final drive as
     *     [0x5806A0] = +0x78 x [0x005C6BF8] / maxrevs
     * and [0x005C6BF8] (8500.0) is an IMAGE constant, so that multiply lives
     * in drivetrain.c where the constants table is, not here with an 8500
     * written into the source. again: a number transcribed out of the
     * image is still a number out of the image. */
    int32_t engscale_raw;

    float torque[CAR_PARAMS_CURVE_PTS];  /* +0x7C..+0x9C, nine breakpoints
                                          * at 0.0, 0.125 ... 1.0           */

    float upshift;          /* +0xA0  fraction of maxrevs; x0.9 = downshift  */
    float coast_off_brake;  /* +0xA4  + [0x0057B060] on load                 */
    float coast_on_brake;   /* +0xA8  + [0x0057B060] on load                 */
} car_params;

/* The whole table, plus what the loader learned about the image it came from. */
typedef struct car_params_table {
    car_params rec[CAR_PARAMS_COUNT];
    unsigned   named;      /* records whose name is more than just the index.
                            * REPORTED, not asserted: records 24-31 are
                            * placeholders and whether the game can select one
                            * is unknown (s2). */
} car_params_table;

/* Load from a PE32 image on disk.  Returns 0 on success.
 *
 *   -1  the file could not be opened or read          (caller should SKIP)
 *   -2  it is not a PE32 image this reader understands
 *   -3  0x0057CB8C is not backed by raw data in it
 *   -4  the index-prefix self-check failed - the base or stride is wrong for
 *       this image, which is exactly the case a silent default would hide
 *
 * `err` may be NULL; otherwise it receives a short human-readable reason. */
int car_params_load(const char *image_path, car_params_table *out,
                    char *err, size_t errlen);

/* The record a car record selects, masked the way FUN_004201FB masks it. */
const car_params *car_params_for(const car_params_table *t, int32_t carnum);

#endif /* RALLY_SIM_CAR_PARAMS_H */
