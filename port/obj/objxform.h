/* objxform.h - the object transform.  FUN_00425FAA, transcribed.
 *
 *
 *
 * WHAT THIS IS
 * ------------
 * The class-0xff instance record carries a type, an anchor point, two Euler
 * triples and a scale byte.  FUN_00425FAA turns them into the 64-byte matrix
 * at [0x0059F18C] and then into the object-to-VIEW matrix at [0x0059F054],
 * which is what the model path draws through.  Between
 * (which read the record) and
 * (which read the draw) this was the hole, and
 * says so in as many words.
 *
 * Everything here is transcribed from RAL.EXE at a named address.  The
 * transcription is; read that before changing
 * anything in objxform.c, because several of the choices below look wrong and
 * are not:
 *
 *  THE ANGLES ARE NEGATED on the way in (0x00425FFF and five more).
 *  THE SECOND AND THIRD ANGLES ARE NOT PITCH AND ROLL.  They are a tilt
 *     MAGNITUDE and the HEADING OF THE AXIS it tilts about, applied as the
 *     conjugation Ry(-h) * Rx(-t) * Ry(+h).  Reading them as Euler pitch/roll
 *     puts 4.80% of the drawable objects at the wrong angle.
 *  THE TWO TRIPLES ORDER THEIR FIELDS DIFFERENTLY.  pols[+0x0c] is the
 *     first triple's tilt and pols[+0x10] its axis; in the second triple it is
 *     pols[+0x13] that is the tilt and pols[+0x12] the axis.
 *  THE SINE TABLE IS 4096 STEPS PER TURN and the port reproduces its
 *     quantisation rather than calling sinf().  A differential test against
 *     the original compares floats.
 *
 * ROW-VECTOR, ROW-MAJOR, TRANSLATION IN ROW 3.  v' = v * M, and a composition
 * `a then b` is written mat_mul(&out, &a, &b).  That is not a convention this
 * port chose: FUN_00425DBE writes the three translation floats to [buf+0x30],
 * [buf+0x34] and [buf+0x38] (0x00425DC9), and FUN_00425E1D's inner product
 * runs down esi's ROW against edi's COLUMN.
*/

#ifndef RALLY_OBJXFORM_H
#define RALLY_OBJXFORM_H

/* A 4x4 in the engine's own storage order.  m[r*4 + c].  Column 3 is never
 * written by any of the engine's matrix routines - FUN_00425E1D computes three
 * columns and copies the fourth from nothing - so it is carried as whatever
 * the .data image holds, which is 0,0,0,1. */
typedef struct { float m[16]; } ral_mat4;

/* The transform inputs, one field per global FUN_0044FB03 fills from the
 * instance record.  The names are this port's; the addresses are the game's
 * and are what to grep for. */
typedef struct {
    /* the first triple.  pols[+0x08], [+0x0c], [+0x10], each x256. */
    unsigned yaw;             /* [0x005E7ECC]  binary angle, 16 bits        */
    unsigned tilt;            /* [0x005E7ED0]  the MAGNITUDE of the tilt    */
    unsigned tilt_axis;       /* [0x005E7ED4]  the HEADING of its axis      */

    /* the second triple.  Zeroed by FUN_0044FB03 (0x0044FBB5) unless
     * pols[+0x11] is non-zero, which it is in 10 of 44,270 drawable records -
     * so `enabled` is not a separate field here: leave these zero. */
    unsigned yaw2;            /* [0x005E7EF4]  pols[+0x09]                  */
    unsigned tilt2;           /* [0x005E7EF0]  pols[+0x13] - the MAGNITUDE  */
    unsigned tilt2_axis;      /* [0x005E7EEC]  pols[+0x12] - the AXIS       */

    /* pols[+0x18], a byte.  The matrix is multiplied by scale16 / 16.0f, so
     * 16 means unit - and 16 is the value in 215,706 of 237,428 records. */
    int scale16;              /* [0x005E7EC8]                               */

    /* pnts[anchor] + pols[+0x30,+0x34,+0x38], as the 16.16 fixed-point INTS
     * the game holds.  FUN_00425DBE converts with FILD and does NOT divide,
     * so world space here is 65536 units to the metre. */
    int x, y, z;              /* [0x005E7EAC], [0x005E7EB0], [0x005E7EB4]   */
} ral_obj_place;

/* OBJSINFO[+0x20], the mode selector.  Domain over the shipped 400-record
 * file is {0: 391, 2: 2, 5: 7}; the early demo's file uses 1 for the same
 * seven records retail marks 5.  Modes 1 and 2 are THE SAME ARM - 0x00425FE3
 * and 0x00425FF3 both branch to 0x0042627E. */
enum {
    RAL_OBJ_MODE_EULER    = 0,  /* build it from the record's own angles    */
    RAL_OBJ_MODE_FACE_1   = 1,  /* the early demo's spelling of mode 2      */
    RAL_OBJ_MODE_FACE     = 2,  /* the INVERSE of the view rotation         */
    RAL_OBJ_MODE_FACE_YAW = 5   /* yaw only, cancelling the camera's yaw    */
};

/* What the object transform needs from the camera.  The game holds these as
 * globals the port does not have yet; naming them here is what lets
 * objxform.c be the transcription and nothing else. */
typedef struct {
    ral_mat4 view_rot;        /* [0x0059EF94]  world -> view ROTATION       */
    ral_mat4 view_trans;      /* [0x0059EF14]  identity, row 3 = -camera    */
    ral_mat4 view_rot_inv;    /* [0x0059EF54]  FUN_0041FA3B of view_rot     */
    unsigned cam_yaw;         /* [0x005BA608]  32-BIT binary angle          */
    unsigned cam_extra_yaw;   /* [0x005BA614]  32-bit, mode 5 only          */
} ral_obj_camera;

/* ------------------------------------------------------------------------ */

/* Build the sine table FUN_0041E368 builds: 5,120 floats, sin(i*pi/2048).
 * Idempotent; every entry point below calls it, so callers need not.  The
 * table is 4096 entries for a full turn plus 1,024 of overrun, which is what
 * makes the cosine a read at +1,024 and never a bounds check. */
void ral_obj_sincos_init(void);

/* sin and cos of a 16-bit binary angle, THE WAY THE GAME TAKES THEM:
 * (a & 0xffff) >> 4 into the table.  0x00425BF9. */
float ral_obj_sin16(unsigned a);
float ral_obj_cos16(unsigned a);

/* out = a * b.  FUN_00425E1D: three columns of a 4x3, then row 3 of `a`
 * translated by row 3 of `b`.  Aliasing `out` with `a` or `b` is not allowed -
 * the original writes through a third pointer and so does this. */
void ral_mat4_mul(ral_mat4 *out, const ral_mat4 *a, const ral_mat4 *b);

/* The Z rotation, FUN_00425C82 -> [0x0059ED54].  Exported rather than left
 * static because the only arm that reaches it is the one 0x00425FB6 never
 * takes, and a builder with no caller is a builder nobody checks. */
void ral_obj_rot_z16(ral_mat4 *m, unsigned a);

/* FUN_00425C3A -> [0x0059ED14], FUN_00425DBE -> [0x0059ED94] and
 * FUN_00425DF4.  Exported: FUN_00425E6E, the FRAME BACKGROUND's
 * matrix builder, is made of these three and the multiply above, in its own
 * order, and reads no instance record at all.  port/obj/objscene.c is its
 * one caller; nothing else here composes a matrix by hand. */
void ral_obj_rot_y16(ral_mat4 *m, unsigned a);
/* FUN_00425BF1 -> [0x0059ECD4], about X.  Exported (objlit.h). */
void ral_obj_rot_x16(ral_mat4 *m, unsigned a);
void ral_obj_translate_i(ral_mat4 *m, int x, int y, int z);
void ral_mat4_copy_add_t(ral_mat4 *out, const ral_mat4 *src,
                         const ral_mat4 *add);

/* THE FUNCTION.  `mode` is OBJSINFO[+0x20].
 *
 *   out_model  <- [0x0059F18C], the object's own matrix, scaled
 *   out_view   <- [0x0059F054], that matrix composed with the camera
 *
 * Either may be NULL.  `cam` may be NULL only for mode 0, which does not read
 * it - and passing NULL for a mode that does is the one input this function
 * refuses rather than guesses at. */
void ral_obj_transform(ral_mat4 *out_model, ral_mat4 *out_view,
                       const ral_obj_place *p, int mode,
                       const ral_obj_camera *cam);

#endif /* RALLY_OBJXFORM_H */
