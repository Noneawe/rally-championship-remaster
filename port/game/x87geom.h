/* x87geom.h - the original's rotation helpers at 0x004623E0 .. 0x00462486, as
 * wipers.c and driver.c both use them (moved them here from wipers.c).
 * Row-major float32 3 x 3, the arithmetic in the x87's 80 bits (long double),
 * FILD in and FISTP out; x87.h has the sine and cosine. */
#ifndef RALLY_X87GEOM_H
#define RALLY_X87GEOM_H

#include "x87.h"

#include <math.h>
#include <stdint.h>

#include "ext.h"

static inline int32_t fistp(ext x)       /* FISTP, round to nearest even    */
{
    return (int32_t)ext_lrint(x);
}

/* 0x004623E0: FSINCOS, then a rotation about x */
static inline void rot_x(float m[9], ext a)
{
    float c = (float)ext_cos(a), s = (float)ext_sin(a);
    m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f;
    m[3] = 0.0f; m[4] = c;    m[5] = s;
    m[6] = 0.0f; m[7] = -s;   m[8] = c;
}

/* 0x00462403: about y */
static inline void rot_y(float m[9], ext a)
{
    float c = (float)ext_cos(a), s = (float)ext_sin(a);
    m[0] = c;    m[1] = 0.0f; m[2] = s;
    m[3] = 0.0f; m[4] = 1.0f; m[5] = 0.0f;
    m[6] = -s;   m[7] = 0.0f; m[8] = c;
}

/* 0x00462426: about z */
static inline void rot_z(float m[9], ext a)
{
    float c = (float)ext_cos(a), s = (float)ext_sin(a);
    m[0] = c;    m[1] = s;    m[2] = 0.0f;
    m[3] = -s;   m[4] = c;    m[5] = 0.0f;
    m[6] = 0.0f; m[7] = 0.0f; m[8] = 1.0f;
}

/* 0x00462449: r = a b, each element a0 b0 + (a1 b1 + a2 b2) in the x87 */
static inline void mat_mul(float r[9], const float a[9], const float b[9])
{
    int i, j;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++)
            r[i * 3 + j] = (float)((ext)a[i * 3] * b[j]
                                   + ((ext)a[i * 3 + 1] * b[3 + j]
                                      + (ext)a[i * 3 + 2] * b[6 + j]));
}

/* 0x00462486: one vector through m, FILD in, FISTP out - the third row
 * sums in another order (FXCH), kept */
static inline void xform(int32_t o[3], const float m[9], const int32_t v[3])
{
    ext x = v[0], y = v[1], z = v[2];
    o[0] = fistp(x * m[0] + (y * m[1] + z * m[2]));
    o[1] = fistp(x * m[3] + (y * m[4] + z * m[5]));
    o[2] = fistp((x * m[6] + z * m[8]) + y * m[7]);
}

#endif
