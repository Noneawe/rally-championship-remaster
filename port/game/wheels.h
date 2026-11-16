/*
 * wheels.h - the exterior wheels.
 *
 * The original does not draw a car's tyres as part of its body.  The model
 * loader (FUN_00424997, the files\b branch at 0x00424A3B) splits one .ro3 into
 * types by material: the body type (+0) takes every polygon EXCEPT those whose
 * material bit is one of the car's four wheel bits, and types +1..+4 take one
 * wheel's polygons each.  FUN_0045F094's wheel draws (0x0045F7EF..) then put
 * each wheel under its own matrix, built per car by 0x0045ECC5..0x0045F04D:
 * the pivot to the origin, the spin about the axle, the steer about the
 * vertical (front pair only), and out again at the wheel's own height.
 *
 * This file is that builder, and the table it reads.  The split itself is
 * the part mask the objscene draw already takes (objscene_parts.mask1,
 * tested against the polygon's material): the body draws with the wheel bits
 * cleared, each wheel with only its own set.
*/

#ifndef RALLY_WHEELS_H
#define RALLY_WHEELS_H

#include <stdint.h>
#include "objxform.h"

/* One car's row of the parameter table [0x0057E10C + id * 0x12C]: the four
 * wheel material bit indices (+0x50, +0x54, +0x58, +0x5C - front left, front
 * right, rear left, rear right) and the geometry words, millimetres. */
typedef struct wheel_geom {
    int32_t bit[4];
    int32_t front_z;       /* +0x24 [0x0057E130] */
    int32_t rear_z;        /* +0x28 [0x0057E134] */
    int32_t track;         /* +0x2C [0x0057E138] */
    int32_t height;        /* +0x30 [0x0057E13C] */
} wheel_geom;

/* The model turn's drop, [0x005C6CF8] (0xB333: objscene.c's OBJ_TURN_DY). */

/* 0x0045ECC5..0x0045F04D: the four wheel matrices, [0x005F46A0], [0x005F46E0],
 * [0x005F4720], [0x005F4760].  `steer` is car+0x650 >> 16 (the 16-bit binary
 * angle the front pair turns by), spin_f / spin_r car+0x1F8 / +0x1FC, and
 * sus[i] car+0x910 + 4i.  A vertex of the wheel's type goes through m[i] and
 * then through the car's own rotation, position and the view. */
void wheels_build(ral_mat4 m[4], const wheel_geom *g, int32_t turn_drop,
                  unsigned steer, int32_t spin_f, int32_t spin_r,
                  const int32_t sus[4]);

/* The wheels' bits as one mask: what the body type leaves out. */
uint32_t wheels_mask(const wheel_geom *g);

/* 0x0045304F..0x0045307F: 1 when the original skips a car's draw by its
 * origin's view-space x and depth (16.16): more than 4.0 behind the eye, or
 * off to the side by more than the depth plus 8.0. */
int wheels_view_cut(int32_t x, int32_t z);

#endif
