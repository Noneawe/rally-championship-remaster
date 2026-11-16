/* driver.h - the cockpit's articulated parts: the steering wheel and the
 * driver's two hands;
 * sections 4 and 4.2 are the reading.
 *
 * Like the wipers, the original has no draw call for them: every engine update
 * 0x004A9398 (one caller, 0x004B0E7E, after the wipers' 0x004B0D4C) re-poses
 * the vertices of the car's own model and the ordinary car draw shows them.
 *
 *   load  0x004B0E39 -> 0x004A92DB / 0x004A9314 -> 0x004A9640: four part
 *         records of the car's model, each the vertices (at most 128, in
 *         first-seen order) of every polygon whose material (byte +0x07 & 0x1F)
 *         is one of the per-car words [0x00730E4A / 4E / 52 / 56] - the wheel,
 *         the two hands and a fourth group.  Each is taken to its own origin
 *         (the bounding box's middle, 0x004A9A8B) and turned by the Euler
 *         angles at +0x20 / +0x24 / +0x28 (0x004A9BF4; +0x20 is the angle
 *         between the box's y-z diagonal and the z axis, an arc-cosine found
 *         by 16 bisections, 0x0046252C).  The hands share the wheel's origin
 *         and +0x20, and their vertices are SORTED into three groups by height
 *         (0x004A9F24 / 0x004A9F96: the forearm; the wrist; the rest - the
 *         third is never posed).
 *   step  0x004A9398: the gear hand's animation [0x00711862] (0..0xFF, 0x80
 *         the middle) from the shift latch and the handbrake; then ONE DRAW
 *         from the shared generator [0x006CF334] (a random walk of
 *         [0x0071184A], whose sine [0x0071184E] nothing reads); then
 *         0x004A950B, which gives every record the steering angle (0.8 of it;
 *         0.32 of it for the fourth group), the hands a second angle by its
 *         sign, and poses the records:
 *           wheel, fourth group  0x004A9CBC  every vertex through
 *                                Rx(a20) Ry(a24) Rz(a28), plus the origin;
 *           each hand            0x004A9D7A  the forearm through the same, the
 *                                wrist through the second angle; the gear
 *                                animation (0x004AA254, the hand the lever
 *                                is not [0x00730E46]'s) moves the forearm by a
 *                                spline blend (0x004AA1F0, the file's weights
 *                                files\m\spline.dat, 256 x 3 floats).
 *
 * Every address is retail's, every constant is read from the image or the
 * file.  THE PORT'S, not the original's: the model's vertices are the port's
 * doubles (16.16 / 65536, as wipers.c); the arithmetic is long double, the
 * x87's 80 bits (x87geom.h), on aarch64 a vertex may differ by one unit.  The
 * original's pointer to a vertex is the vertex's index.  The dead stores
 * (0x004AA186's farthest pair, the records' +0x18 / +0x1C, +0x44) are not
 * ported.
 *
 * The step is gated against the original's own code in x86vm.c (driver-vm). */
#ifndef RALLY_DRIVER_H
#define RALLY_DRIVER_H

#include "image.h"
#include "ro3.h"

#include <stdint.h>

#define DRIVER_VERTS 128
#define DRIVER_PARTS 4

typedef struct {
    int      n;                          /* rec +0x50                       */
    int32_t  id[DRIVER_VERTS];           /* +0x454: the original's pointers */
    int32_t  rest[DRIVER_VERTS][3];      /* +0x654                          */
    int32_t  cls[DRIVER_VERTS];          /* +0xC90 (hands)                  */
    int32_t  origin[3];                  /* +0xC54                          */
    int32_t  start[3];                   /* +0x00 / +0x04 / +0x08           */
    int32_t  count[3];                   /* +0x0C / +0x10 / +0x14           */
    int32_t  hi, lo;                     /* +0x30, +0x34 (hands)            */
    int32_t  c[3];                       /* +0x44..+0x4C (hands)            */
    float    ang[3];                     /* +0x20, +0x24, +0x28             */
    float    ang2;                       /* +0x2C                           */
} driver_part;

/* The render snapshot's words (block +0x26DC, +0x26FC, +0x2700). */
typedef struct {
    float    steer;                      /* [0x0057AEBC]                    */
    int32_t  latch;                      /* [0x0057AEDC]                    */
    int32_t  hold;                       /* [0x0057AEE0]                    */
} driver_block;

typedef struct {
    int          have;
    int          car;
    driver_part  p[DRIVER_PARTS];        /* 0x00713C46, 0x0071B0C6, 0x00722546,
                                          * 0x007299C6                      */
    float        w[256][3];              /* 0x00711866                      */
    int          have_table;
    int          seeded;                 /* walk is a carried global       */
    int32_t      swap;                   /* [0x00730E46]                    */
    int32_t      anim;                   /* [0x00711862]                    */
    float        walk;                   /* [0x0071184A]: the image's, a
                                          * process carries it over         */
    float        wobble;                 /* [0x0071184E]                    */
    int32_t      hand;                   /* [0x0071185E]                    */
    float        k[12];                  /* 0x00731099 .. 0x007310C1        */
    driver_block blk;
    unsigned long steps;
} driver;

/* The weights, files\m\spline.dat (0xC00 bytes) at `path`.  0 or -1. */
int  driver_load_table(driver *d, const char *path, char *err, size_t errlen);

/* 0x004B0E39 .. 0x004A9640: the records out of `m` (16.16 from its points).
 * `carnum1` is shauncarnum1.  The weights and the walk survive a load.
 * 0, or -1 with a message (the parts then never move). */
int  driver_load(driver *d, const ral_image *im, const ro3_model *m,
                 int32_t carnum1, char *err, size_t errlen);

/* 0x00454779 .. 0x004547C2: the snapshot's words from car 0: `steer_raw`
 * car+0x650, `hold` car+0x640 != 0, `*car_latch` car+0x23C (copied if set,
 * then ZEROED, as the original does), `shake_h` [0x005EA508]. */
void driver_snapshot(driver *d, int32_t steer_raw, float shake_h, int hold,
                     int32_t *car_latch);

/* 0x004A9398: one engine update.  `dt` is [0x0070D340], `gen` the shared
 * generator [0x006CF334].  Writes the posed vertices into `m`'s points. */
void driver_step(driver *d, ro3_model *m, int32_t dt, uint32_t *gen);

#endif
