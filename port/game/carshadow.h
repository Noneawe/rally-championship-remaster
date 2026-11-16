/* carshadow.h - the cars' shadows.
 *
 * A REMASTER, NOT A PORT.  The original's shadow (FUN_00455378, read in
 * hulls the four wheel points, clips the hull
 * against every ground triangle under the car and draws the pieces in the
 * stage's mode 10.  This does not: it draws ONE soft, car-shaped quad per car,
 * laid on the ground at the heights under its four corners, in a single
 * batched blended draw.  What it keeps of the original is the SUN: the shadow
 * leans away from the level's sun (LOCALDAT +0x684 azimuth, +0x688 elevation,
 * as FUN_00455EA4 reads them), which is a decision of the user's.
 *
 * Nothing here is read back by the simulation: it is presentation only, so
 * the replay gates cannot move.
 *
 * carshadow_build is the pure half - no GL, no globals - so a gate can check
 * its geometry (carshadow_check.c).  carshadow_begin / _add / _flush are the
 * drawing half. */
#ifndef RALLY_CARSHADOW_H
#define RALLY_CARSHADOW_H

#include <stdint.h>

#include "car.h"
#include "ground.h"
#include "stage.h"

#define CARSHADOW_MAX 32        /* QUADS per frame (two layers a car); more are dropped */

/* The two layers of a car's shadow (2026-11-13: one shape could not be both a
 * planted car and a cast one).  CONTACT: tight, dark, straight under the car,
 * no lean - what makes it sit on the ground.  CAST: wider, lighter, leaning
 * away from the sun.  The cast is drawn first, the contact over it. */
#define CARSHADOW_CAST    0
#define CARSHADOW_CONTACT 1
#define CARSHADOW_LAYERS  2

/* The level's sun, LOCALDAT +0x684 / +0x688 (byte angles, 128 = half a turn);
 * `have` 0 = no lean (a level without a sun, or the menus). */
typedef struct {
    int have;
    int az, el;
} carshadow_sun;

/* The ground under a world point: 16.16 x, z in, the plane's y out (16.16).
 * Returns nonzero when the point is on the level. */
typedef int (*carshadow_height_fn)(void *ctx, int32_t x16, int32_t z16,
                                   int32_t *y16);

/* One shadow: a CARSHADOW_N x CARSHADOW_N grid of vertices (index j * N + i,
 * i across the car's width, j along its length), each laid on the ground at
 * its own height so the shadow DRAPES over a bank or a crown instead of being
 * cut by it; the corners are 0, N-1, N*N-1 and N*(N-1) (-,-  +,-  +,+  -,+).
 * World 16.16 (x, y, z), the texture coordinates, and the opacity 0..1 (the
 * airborne fade is in it). */
#define CARSHADOW_N   4
#define CARSHADOW_NV  (CARSHADOW_N * CARSHADOW_N)
#define CARSHADOW_TRIS (2 * (CARSHADOW_N - 1) * (CARSHADOW_N - 1))
typedef struct {
    float p[CARSHADOW_NV][3];
    float u[CARSHADOW_NV], v[CARSHADOW_NV];
    float alpha;
    int   layer;                /* CARSHADOW_CAST / _CONTACT: picks the texture */
} carshadow_quad;

/* The layer's alpha texture at normalised (p, q), each -1..1 across the quad's
 * width and length (q +1 the car's nose): a rounded box a little narrower at
 * the nose.  The CAST's edge is long and soft, the CONTACT's short and firm,
 * so the contact reads as the car's outline.  Exposed so the gate can state it. */
float carshadow_shape(int layer, float p, float q);

/* The tunables, here so the gate can state them. */
#define CARSHADOW_PEAK      0.60f   /* the contact layer's darkness (the shape review ran at 0.95) */
#define CARSHADOW_CAST_FRAC 0.60f   /* the cast layer's darkness, as a fraction of it (the shape review ran at 0.9) */
#define CARSHADOW_LIFT      0x1999  /* above the ground, 16.16: the original's
                                     * own lift, `[0x5F47E8]` (0x00455378),
                                     * which FUN_004559B2 adds to every corner */
#define CARSHADOW_BODY_H    1.20f   /* the height the sun's lean is taken at */
#define CARSHADOW_LEAN_MAX  2.00f   /* the cast layer's lean, in world units, capped (the shape review ran at 3.0) */
#define CARSHADOW_FADE_LO   0.40f   /* the car's clearance where it starts   */
#define CARSHADOW_FADE_HI   2.20f   /*   to fade, and where it is gone       */
#define CARSHADOW_RIDE      0.70f   /* the wheel points' height under the car's
                                     * origin      */

/* 1 and `q` filled for the layer (CARSHADOW_CAST or _CONTACT), or 0: no
 * shadow for this car (fewer than four collision points, a car on its side,
 * no vertex on the level, or in the air). */
int carshadow_build(carshadow_quad *q, const car_state *car,
                    const sim_collision_points *pts,
                    carshadow_height_fn height, void *ctx,
                    const carshadow_sun *sun, int layer);

/* The drawing half. */
void carshadow_begin(void);
void carshadow_add(const carshadow_quad *q);
/* Draws what was added in one call and empties the list.  `view_rot` /
 * `view_trans` are the object layer's two matrices (objscene.h).  The caller
 * saves and restores the render state and the cull mode around it. */
void carshadow_flush(const stage_view *v, const float view_rot[16],
                     const float view_trans[16]);
void carshadow_free(void);

#endif
