/* objlit.h - the model path's LIT draw: light mode 4 and the reflection pass.
 *
 * is the reading behind every
 * line; this header carries the addresses so the code can be checked against
 * the image without it.
 *
 * WHAT IT IS.  objscene.c draws a model the way the instance layer does, with a
 * diagnostic shade (obj_shade) where the original lights.  This is the same
 * polygon walk with the original's lighting in it, for the one caller that has
 * been read end to end: the menus' 3D models (0x0048C4AC), which set
 *
 *     [0x00581BBC] = 0x200      the reflection strength, during the draw
 *     [0x005E948C] = 5          the draw context (not 4, the backdrop)
 *     [0x005E0984] = 4          the light mode
 *
 * and call FUN_0044FDE0.  Mode 4 lights every corner through FUN_0044F0CF ->
 * FUN_0044F193: the corner's VIEW-space normal (the object matrix's 3x3, scale
 * included - 0x00459F6F) dotted with the light, times [0x005E7F20], rounded
 * (FISTP), plus [0x005E7F1C], clamped to [[0x005E7F14], [0x005E7F18]], and
 * looked up in the 256-entry colour ramp [0x005E7FDC].  The polygon then
 * draws in vertex mode 4 (0x0044A790): the polygon's own plate, modulated, and
 *where [0x00581BBC], [0x005D6760] and the polygon's byte +9 are all non-zero
 *a SECOND, ADDITIVE pass of texture id 0x87 at sphere-mapped uvs
 * (FUN_0044F82E / FUN_0044F92E) whose vertex alpha is the polygon's weight.
 *
 * FUN_0044E477's FIVE GATES, read for this caller: 1
 * (0x004501D6) passes outside a level; 2 (0x0044E569) passes below type 0x50;
 * 3 (0x0044E634) is the fade, off without OBJSINFO's +0x1D/+0x22, which no
 * menu model has; 4 (0x0044E663) is the part gate below, and it is what hides
 * a car's damaged-glass and lamp polygons; 5 (0x0044FF9F) skips a polygon all
 * of whose fan triangles face away (a view-space cross product per triangle),
 * which the backend's per-triangle cull does.  NOT TRANSCRIBED, AND SAID: the
 * quicksort the draw loop walks (the polygons go
 * in FILE order here, as objscene.c does); vertex mode 3, the
 * [0x005EC82C] != 0x100 fade that gate 3 writes.  Light modes 0, 1, 2, 3 and
 * 5 are the race's.
*/
#ifndef RALLY_OBJLIT_H
#define RALLY_OBJLIT_H

#include <stdint.h>

#include "objscene.h"
#include "objxform.h"
#include "plat.h"
#include "stage.h"

/* The inputs the lit path reads, one field per global. */
typedef struct {
    float    light[3];        /* [0x005D6F3C..44], FUN_00425F36 (objlit_light) */
    int32_t  lo, hi;          /* [0x005E7F14], [0x005E7F18]: the clamp        */
    int32_t  ambient;         /* [0x005E7F1C]                                 */
    int32_t  diffuse;         /* [0x005E7F20]; 0 skips the dot (0x0044F198)    */
    uint32_t lut[256];        /* [0x005E7FDC], FUN_0044D1AD (objlit_ramp)      */
    uint32_t strength;        /* [0x00581BBC], the reflection strength         */
    int      reflect;         /* [0x005D6760] != 0                             */
    int      context;         /* [0x005E948C]: 4 = the backdrop, unlit         */
    /* The fog byte of working corner 0 as the PREVIOUS polygon's submit left
     * it - 0x0044F86D reads the absolute address 0x0091D930, not the corner
     * in hand.  255 wherever the fog ramp is all zero, which the menus' is. */
    uint8_t  fog0;
    plat_tex tex87;           /* the plate texture id 0x87 resolves to         */
    /* FUN_0044E663, the fourth of FUN_0044E477's gates: [0x0057AF70] (1
     * inside a level, 0 at 0x00442E8E) and the per-part state table
     * [0x005823D0 + b6*4] a polygon's byte +0x0B must equal when its byte +6
     * is non-zero, 256 entries.  NULL = all zero.  Outside a level
     * FUN_0044E351 resets it before every draw (0x0045FB45); inside one the
     * car's own state is written (0x0045FB37) -. */
    int      in_level;
    const int32_t *part_state;
    /* [0x005D7088], [0x005D708C], added at the vertex output (0x0044AA76). */
    float    off_x, off_y;
    /* PORT'S, not the original's: the original draws into an 800x600 back
     * buffer; the port's window may be another size. */
    float    scale_x, scale_y;
    /* PORT'S: the rasteriser convention, added to every screen x and y.
     * Direct3D samples a pre-transformed primitive at the pixel's corner and
     * GL at its centre, so the HUD's vertices go out +0.5 too (section 14); measures it for the models. */
    float    bias;
} objlit_env;

/* FUN_0044D1AD: lut[i] = lo + (hi - lo) * i / 255 per channel (IDIV, so
 * truncated toward zero), R from +0/+0xC, G from +4/+0x10, B from +8/+0x14,
 * alpha 0xFF.  `lo` and `hi` are R, G, B. */
void objlit_ramp(uint32_t lut[256], const int32_t lo[3], const int32_t hi[3]);

/* FUN_00425F36: (0, 0, 1) through Rx(-(pitch << 8)) * Ry(-(yaw << 8)) *
 * view_rot, i.e. [0x005E7F28] and [0x005E7F24].  `view_rot` NULL = identity,
 * which is what the menus' camera is (0x00437767 with every angle zero). */
void objlit_light(float out[3], int32_t yaw, int32_t pitch,
                  const ral_mat4 *view_rot);

/* 0x00426422, the menus' model matrix - and it has NO view in it:
 *     [0x0059ED14] = Ry(-[0x005E7ED0])               0x0042642F
 *     [0x0059ECD4] = Rx(-[0x005E7ECC])               0x0042643C
 *     [0x0059ED94] = T([0x5E7EAC], [0x5E7EB0], [0x5E7EB4])  0x00426452
 *     [0x0059EE54] = Ry * Rx                         0x00426469
 *     all sixteen floats *= [0x005E7EC8] / 16.0f     0x00426480
 *     [0x0059F054] = copy + translation              0x004264C1
 * The menus' view is the identity, so the object is placed straight into
 * camera space. */
typedef struct {
    int32_t  x, y, z;         /* [0x005E7EAC], [0x005E7EB0], [0x005E7EB4]      */
    int32_t  scale16;         /* [0x005E7EC8]                                  */
    uint32_t pitch;           /* [0x005E7ECC]                                  */
    uint32_t yaw;             /* [0x005E7ED0]                                  */
} objlit_place;

void objlit_matrix(ral_mat4 *out, const objlit_place *p);

typedef struct {
    unsigned long polys;      /* polygons drawn (>= 3 corners, past the gate)  */
    unsigned long gated;      /* polygons FUN_0044E663 turned away             */
    unsigned long tris;       /* fan triangles built                           */
    unsigned long drawn;      /* fan triangles submitted                       */
    unsigned long behind;     /* dropped: a corner behind the near plane       */
    unsigned long lit;        /* corners through the light                     */
    unsigned long white;      /* corners left 0xFFFFFF by 0x0044EE2F's gate    */
    unsigned long clamp_lo, clamp_hi;  /* lit corners clamped                   */
    unsigned long reflect;    /* polygons drawn with the second pass           */
    unsigned long tex_none;   /* polygons with no plate bound                  */
    float         min_sx, max_sx, min_sy, max_sy;
} objlit_stats;

void objlit_stats_init(objlit_stats *st);

/* Draw `type` through `mv` (its object-to-view matrix, [0x0059F054]) with the
 * view's projection (focal, centre, near; radius and detail for the depth). */
void objlit_draw(objscene *os, int type, const ral_mat4 *mv,
                 const stage_view *v, const objlit_env *e, objlit_stats *st);

/* FUN_0044F92E: the reflection uv of one corner (texels of a 256 page, as
 * objlit_draw's layer 1 uses them), from its VIEW-space normal `n` and view
 * position `p`; `rinv` is [0x0059EF54], the view's inverse rotation.  Shared
 * with objscene.c's race car. */
void objlit_env_uv(const float n[3], const float p[3], const ral_mat4 *rinv,
                   float *u, float *v);

/* For the probe: the table square root FUN_0045A06B normalises with, its two
 * tables built at boot by 0x004201A4. */
float objlit_sqrt(float x);

#endif /* RALLY_OBJLIT_H */
