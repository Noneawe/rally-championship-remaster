/* objxform.c - FUN_00425FAA and its seven helpers, transcribed.
 *
 * is the
 * transcription; this file is meant to be readable beside it.
 *
 * THE SHAPE OF IT
 * ---------------
 * Four scratch matrices live in .data at 0x0059ECD4, 0x0059ED14, 0x0059ED54
 * and 0x0059ED94.  Each of the four helpers writes ONLY the three or four
 * slots that vary and leaves the rest of the matrix alone - so the constant
 * parts (the 1s on the diagonal and the zeros) come from the .data image and
 * are never rewritten.  That is measured, not assumed: every reference to
 * those four addresses in the whole image is a `lea` into esi or edi, and the
 * two composition routines write through ebp, which no site ever loads with
 * one of them.  A port cannot share static buffers that way and does not need
 * to, so each helper below builds a whole matrix.
 *
 * THE ANGLES ARE NEGATED.  Every angle reaching a rotation helper on the
 * object path is negated first (0x00425FFF, 0x00426028, 0x0042604C,
 * 0x004260A8, 0x004260CC, 0x004260F0), except the two that CLOSE a
 * conjugation (0x0042606A, 0x0042610E) and mode 5's pair, which are the
 * camera's and are deliberately un-negated.
*/

#include "objxform.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* the sine table - FUN_0041E368, 0x0041E368..0x0041E3AD                   */
/* ---------------------------------------------------------------------- */
/*
 *   ecx = 0
 *   do { st = (float)ecx * pi;  st /= 2048.0;  fsin
 *        [ecx*4 + 0x9c3ee4] = st;  ecx++ } while (ecx != 0x1400)
 *
 * 5,120 entries of sin(i * pi / 2048).  2,048 steps to pi is 4,096 to a full
 * turn, which is exactly a 16-bit binary angle shifted right by 4 - and the
 * cosine table at 0x009C4EE4 is the SAME table read 0x1000 bytes in, which is
 * 1,024 entries, which is a quarter turn.  The 1,024 entries of overrun are
 * what make that read safe for every index.
 *
 * The port reproduces the table rather than calling sinf() at the angle,
 * because the table is what the original's floats came out of and a
 * differential test compares floats.
 */

#define RAL_SIN_QUART  1024            /* the cosine's offset               */
#define RAL_SIN_LEN    5120            /* 0x1400, what the loop counts to   */

static float ral_sin_tab[RAL_SIN_LEN];
static int   ral_sin_ready;

void ral_obj_sincos_init(void)
{
    int i;
    if (ral_sin_ready) return;
    for (i = 0; i < RAL_SIN_LEN; ++i)
        ral_sin_tab[i] = (float)sin((double)i * 3.14159265358979323846 / 2048.0);
    /* The two entries where the original does not agree with a double sine
     * (wheels_vm_check read the original's own table out of the VM,
     * all 5,120 entries): FSIN of i * FLDPI / 2048 at i = 2048 and 4096 is
     * the x87's own pi minus its 64-bit pi: -2^-64 and +2^-63, the floats
     * 0x9F800000 and 0x20000000 - where a double gives 1.2246e-16 and
     * -2.449e-16.  Every other entry is the same float.  They are the sine
     * of 180 degrees and, read a quarter turn on, the cosine of 90 and 270
     * (and 450): the wheel matrices at those spin angles were the first to
     * show it. */
    { union { uint32_t u; float f; } t;
      t.u = 0x9F800000u; ral_sin_tab[2048] = t.f;
      t.u = 0x20000000u; ral_sin_tab[4096] = t.f; }
    ral_sin_ready = 1;
}

/* `and ebx,0xffff / shr ebx,4`, 0x00425BF9.  For the object path the angle is
 * a byte shifted left by 8, so the >> 4 loses nothing: the index is b * 16 and
 * every instance record lands exactly on a table entry. */
static int ral_ang_index(unsigned a) { return (int)((a & 0xffffu) >> 4); }

float ral_obj_sin16(unsigned a)
{
    ral_obj_sincos_init();
    return ral_sin_tab[ral_ang_index(a)];
}

float ral_obj_cos16(unsigned a)
{
    ral_obj_sincos_init();
    return ral_sin_tab[ral_ang_index(a) + RAL_SIN_QUART];
}

/* ---------------------------------------------------------------------- */
/* the four matrix builders                                               */
/* ---------------------------------------------------------------------- */

static void mat_identity(ral_mat4 *m)
{
    memset(m, 0, sizeof *m);
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

/* FUN_00425BF1 -> [0x0059ECD4].  Writes +0x14, +0x18, +0x24, +0x28, which is
 * m[1][1], m[1][2], m[2][1], m[2][2] - a rotation about X.  The .data image
 * supplies m[0][0] = 1 and m[3][3] = 1 and nothing else. */
static void mat_rot_x(ral_mat4 *m, float s, float c)
{
    mat_identity(m);
    m->m[5] = c;  m->m[6]  =  s;
    m->m[9] = -s; m->m[10] =  c;
}

/* FUN_00425C3A -> [0x0059ED14].  +0x00, +0x08, +0x20, +0x28 - about Y.
 * FUN_00425D1C writes the same four slots of the same buffer from FSINCOS. */
static void mat_rot_y(ral_mat4 *m, float s, float c)
{
    mat_identity(m);
    m->m[0] = c; m->m[2]  = -s;
    m->m[8] = s; m->m[10] =  c;
}

/* FUN_00425C82 -> [0x0059ED54].  +0x00, +0x04, +0x10, +0x14 - about Z.
 * Reached only from the arm 0x00425FB6 never takes; kept because the .data
 * constants prove what it is and a reader of the dead arm needs it. */
static void mat_rot_z(ral_mat4 *m, float s, float c)
{
    mat_identity(m);
    m->m[0] =  c; m->m[1] = s;
    m->m[4] = -s; m->m[5] = c;
}

static void mat_rot_x16(ral_mat4 *m, unsigned a)
{ mat_rot_x(m, ral_obj_sin16(a), ral_obj_cos16(a)); }

static void mat_rot_y16(ral_mat4 *m, unsigned a)
{ mat_rot_y(m, ral_obj_sin16(a), ral_obj_cos16(a)); }

void ral_obj_rot_z16(ral_mat4 *m, unsigned a)
{ ral_obj_sincos_init(); mat_rot_z(m, ral_obj_sin16(a), ral_obj_cos16(a)); }

/* FUN_00425C3A -> [0x0059ED14], exported for the FRAME BACKGROUND, which is
 * the one caller of this builder that is not ral_obj_transform: FUN_00425E6E
 * builds the sky dome's matrix out of the same three routines but composes
 * them in its own order and never touches the instance record. */
void ral_obj_rot_y16(ral_mat4 *m, unsigned a)
{ ral_obj_sincos_init(); mat_rot_y(m, ral_obj_sin16(a), ral_obj_cos16(a)); }

/* FUN_00425BF1 -> [0x0059ECD4], exported: the menus' model matrix
 * (0x00426422) and the light direction (FUN_00425F36) both call it directly. */
void ral_obj_rot_x16(ral_mat4 *m, unsigned a)
{ ral_obj_sincos_init(); mat_rot_x(m, ral_obj_sin16(a), ral_obj_cos16(a)); }

/* FUN_00425D1C's angle unit is NOT the table's.  It divides by 65536 twice
 * and then by 0.15915494 (= 1/2pi), so the argument is a 32-BIT binary angle:
 * theta = a / 2^32 * 2pi.  The camera's angles are held that way and mode 5
 * reads two of them, which is why this exists beside mat_rot_y16. */
static void mat_rot_y32(ral_mat4 *m, unsigned a)
{
    double t = (double)a / 4294967296.0 * 6.28318530717958647692;
    mat_rot_y(m, (float)sin(t), (float)cos(t));
}

/* FUN_00425DBE -> [0x0059ED94] + 0x30/0x34/0x38.  FILD, so the 16.16 ints go
 * in as whole numbers: world space is 65,536 units to the metre on both sides
 * of this call and nothing here divides. */
static void mat_translate_i(ral_mat4 *m, int x, int y, int z)
{
    mat_identity(m);
    m->m[12] = (float)x; m->m[13] = (float)y; m->m[14] = (float)z;
}

/* ---------------------------------------------------------------------- */
/* the two composition routines                                           */
/* ---------------------------------------------------------------------- */

/* FUN_00425E1D.  Three columns, four rows, plus row 3 of `b` added in -
 * a 4x3 affine composition.  Column 3 is not touched by the original and is
 * carried through from `a` here so the struct never holds a stale value. */
void ral_mat4_mul(ral_mat4 *out, const ral_mat4 *a, const ral_mat4 *b)
{
    int i, j;
    for (j = 0; j < 3; ++j) {
        for (i = 0; i < 4; ++i)
            out->m[i * 4 + j] = a->m[i * 4 + 0] * b->m[0 * 4 + j]
                              + a->m[i * 4 + 1] * b->m[1 * 4 + j]
                              + a->m[i * 4 + 2] * b->m[2 * 4 + j];
        out->m[3 * 4 + j] += b->m[3 * 4 + j];
    }
    out->m[3]  = a->m[3];  out->m[7]  = a->m[7];
    out->m[11] = a->m[11]; out->m[15] = a->m[15];
}

/* FUN_00425DF4.  `rep movsd` of all sixteen from esi, then row 3 of edi added
 * to row 3 of the copy.  It is NOT a multiply, and reading it as one puts the
 * object at the camera. */
static void mat_copy_add_t(ral_mat4 *out, const ral_mat4 *src,
                           const ral_mat4 *add)
{
    *out = *src;
    out->m[12] += add->m[12];
    out->m[13] += add->m[13];
    out->m[14] += add->m[14];
}

/* FUN_00425DBE -> [0x0059ED94], likewise exported.  The sky dome's
 * translation is (0, LOCALDAT+0x664 << 8, 0) and goes in the same way the
 * anchor does: FILD, no divide, raw 16.16. */
void ral_obj_translate_i(ral_mat4 *m, int x, int y, int z)
{ mat_translate_i(m, x, y, z); }

/* FUN_00425DF4, exported: the sky path composes Ry with a pure
 * translation through it exactly as obj_tail does, and a second copy of a
 * routine whose whole point is that it is NOT a multiply is the last thing
 * this file should grow. */
void ral_mat4_copy_add_t(ral_mat4 *out, const ral_mat4 *src,
                         const ral_mat4 *add)
{ mat_copy_add_t(out, src, add); }

/* ---------------------------------------------------------------------- */
/* the tail - 0x00426130, and its two verbatim copies at 0x00426291 and    */
/* 0x00426385.  The original writes it out three times; the port does not.  */
/* ---------------------------------------------------------------------- */

static void obj_tail(ral_mat4 *out_model, ral_mat4 *out_view,
                     ral_mat4 *cur, const ral_obj_place *p,
                     const ral_obj_camera *cam)
{
    ral_mat4 pos, posv, placed;
    float s;
    int i;

    /* 0x00426136: fild the scale, then `fld1` and four `fadd st(0)` build
     * 16.0 and divide.  ALL SIXTEEN floats are scaled, the translation row
     * and the untouched column 3 included - which costs nothing only because
     * both are still zero at this point. */
    s = (float)p->scale16 / 16.0f;
    for (i = 0; i < 16; ++i) cur->m[i] *= s;

    if (out_model) *out_model = *cur;
    if (!out_view) return;

    /* 0x00426171 */
    mat_translate_i(&pos, p->x, p->y, p->z);
    /* 0x00426187: [0x0059F1CC] = pos, translated by the camera's -position */
    mat_copy_add_t(&posv, &pos, &cam->view_trans);
    /* 0x0042619E: the scaled rotation, translated by that */
    mat_copy_add_t(&placed, cur, &posv);
    /* 0x004261B5: and into view space */
    ral_mat4_mul(out_view, &placed, &cam->view_rot);
}

/* ---------------------------------------------------------------------- */

void ral_obj_transform(ral_mat4 *out_model, ral_mat4 *out_view,
                       const ral_obj_place *p, int mode,
                       const ral_obj_camera *cam)
{
    ral_mat4 cur, r, tmp;

    ral_obj_sincos_init();

    /* FUN_00425F36 runs first (0x00425FAA) and is NOT part of the transform:
     * it is bracketed by `pushal`/`popal` and its only lasting output is the
     * three floats at [0x005D6F3C..0x005D6F44], a direction built from
     * pols[+0x22] and pols[+0x23] through the camera.  See
     * - whoever reads those three
     * globals owns it, and nobody has.
     *
     * 0x00425FAF's `cmp [0x005E9494],0 / je` is NOT taken - the dword is 1 in
     * .data and has exactly one reference image-wide, which is that `cmp`.
     * The 177-byte arm at 0x004261CD is dead code and is not ported.
     *
     * 0x00425FBC's `cmp [0x005E948C],4 / ret` cannot fire either: all three
     * call sites store 2 into it in the instruction before the call
     * (0x0044FD77, 0x004530D0, 0x00453141). */

    if (mode == RAL_OBJ_MODE_FACE || mode == RAL_OBJ_MODE_FACE_1) {
        /* 0x0042627E.  Both `cmp al,1` and `cmp al,2` branch here. */
        if (!cam) return;
        cur = cam->view_rot_inv;
    } else if (mode == RAL_OBJ_MODE_FACE_YAW) {
        /* 0x0042632E.  The identity at [0x0059EC94], then the camera's two
         * yaws applied WITHOUT the negation the camera itself uses - so the
         * object's yaw cancels the camera's and the card faces front. */
        if (!cam) return;
        mat_identity(&cur);
        mat_rot_y32(&r, cam->cam_extra_yaw);
        ral_mat4_mul(&tmp, &cur, &r);
        mat_rot_y32(&r, cam->cam_yaw);
        ral_mat4_mul(&cur, &tmp, &r);
    } else {
        /* 0x00425FF9, the general arm: 391 of the 400 OBJSINFO records. */

        /* the yaw, negated, straight into the output (0x00426006's rep movsd) */
        mat_rot_y16(&cur, (unsigned)(-(int)p->yaw));

        /* 0x00426019: the first tilt, skipped when its MAGNITUDE is zero -
         * which it is in 95.20% of the drawable records. */
        if (p->tilt) {
            mat_rot_y16(&r, (unsigned)(-(int)p->tilt_axis));
            ral_mat4_mul(&tmp, &cur, &r);
            mat_rot_x16(&r, (unsigned)(-(int)p->tilt));
            ral_mat4_mul(&cur, &tmp, &r);
            mat_rot_y16(&r, p->tilt_axis);
            ral_mat4_mul(&tmp, &cur, &r);
            cur = tmp;
        }

        /* 0x0042608C: the second triple, run when EITHER its yaw or its tilt
         * magnitude is non-zero.  10 of 44,270 drawable records. */
        if (p->yaw2 || p->tilt2) {
            mat_rot_y16(&r, (unsigned)(-(int)p->yaw2));
            ral_mat4_mul(&tmp, &cur, &r);
            mat_rot_y16(&r, (unsigned)(-(int)p->tilt2_axis));
            ral_mat4_mul(&cur, &tmp, &r);
            mat_rot_x16(&r, (unsigned)(-(int)p->tilt2));
            ral_mat4_mul(&tmp, &cur, &r);
            mat_rot_y16(&r, p->tilt2_axis);
            ral_mat4_mul(&cur, &tmp, &r);
        }
    }

    obj_tail(out_model, out_view, &cur, p, cam);
}
