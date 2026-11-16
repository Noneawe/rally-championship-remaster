/* objscene.h - the object instance layer: which model, from which file, and
 * where does it stand.
 *
 * WHAT THIS IS
 * ------------
 * port/obj/objxform.c turns one class-0xff instance record into a matrix.
 * This file is everything on either side of that: the LOADER that gives a
 * type its geometry, and the EMITTER that fans the geometry through the
 * matrix into the shim.  Between them they are the answer to
 *'s "which types does
 * load_objects_q load a model for".
 *
 * THE CHAIN, AND EVERY LINK IS TRANSCRIBED
 * ----------------------------------------
 *   FUN_004248B9  for type = 0 .. 0xff:
 *                     [0x005E7E78] = [0x00581BC4 + type*4]   the MODEL NUMBER
 *                     [0x005E7E7C] = type
 *                     CALL FUN_00424997
 *   FUN_004247CD  filled [0x00581BC4] first: entries 1..0x3F are COPIED from
 *                 [0x005BBE70] (0x00424827), which is LOCALDAT + 0x0500 -
 *                 the block loads at 0x005BB970 and 0x005BBE70 - 0x005BB970
 *                 is 0x500.  Entries 0x38..0x3F are then OVERWRITTEN with
 *                 eight constants at 0x004247D2-0x0042481A.
 *   FUN_00424997  returns at once if the model number is 0;
 *                 [0x00581FC4 + type*4] = the model number   (0x004249A6)
 *                 CALL FUN_00437DC7                          builds the path
 *                 byte [[0x005BD9E0] + 7] = '3'              forces .ro3
 *                 CALL 0x00420846 -> &[0x009CBF54]           READS THE FILE
 *                 if [0x005BE004] == 0: return               short or missing
 *                 CALL FUN_004252A5                          ingest into the
 *                                                            polygon arena
 *   FUN_00437DC7  files\a\obj????.ro3       model <  400
 *                 files\b\obj????.ro3       model 400..599
 *                 files\g\frnt\obj????.ro3  model 600..799
 *                 files\s\extr\obj????.ro3  model >= 800
 *                 with ???? the four decimal digits of the model number.
 *
 * SO 0x009CBF54 IS NOT AN ARENA OF MODELS.  It is the one-file staging buffer
 * the loader reads each .ro3 into; the arena is [0x0094BB30], the pointer
 * array FUN_004252A5 appends to.  The ticket and two session briefs had it the
 * other way round -.
 *
 * THE MODEL NUMBER IS ALSO THE OBJSINFO RECORD INDEX, and the index space runs
 * across FOUR files, not one.  FUN_004380D2 loads them at four addresses that
 * TILE EXACTLY:
 *
 *      0x00926330 + 0xC800 = 0x00932B30   files\a\objsinfo   400 records
 *      0x00932B30 + 0x6400 = 0x00938F30   files\b\carsinfo   200
 *      0x00938F30 + 0x6400 = 0x0093F330   files\g\frnt\frntinfo 200
 *      0x0093F330 + 0xC800 = 0x0094BB30   files\s\extr\extrinfo 400
 *
 * and 0x0094BB30 is the polygon pointer array itself.  1,200 records of 128
 * bytes, and the four bands 0..399 / 400..599 / 600..799 / 800..1199 are
 * EXACTLY FUN_00437DC7's four directory bands.  So `OBJSINFO[model]` is a
 * single lookup for every model number, and that is why `objsinfo` and the
 * per-directory model numbering agree.
 *
 * THE TRAP, and it is the reason info_mode() tests `have` first.
 * FUN_0044FB03 reads OBJSINFO[+0x1F] and FUN_00425FAA reads OBJSINFO[+0x20]
 * for a type whose map entry is still -1, which lands 128 bytes BELOW the
 * table.  Harmless in the original because the polygon-count test discards the
 * result; a port that reproduces the order literally reads out of bounds.
 *
*/

#ifndef RALLY_OBJSCENE_H
#define RALLY_OBJSCENE_H

#include <stddef.h>
#include <stdint.h>

#include "cdat.h"
#include "localdat.h"
#include "objxform.h"
#include "plat.h"
#include "ro3.h"
#include "stage.h"

/* ---- the type table ---------------------------------------------------- */

/* LOCALDAT + 0x0500, 64 dwords, copied wholesale to [0x00581BC4].  The region
 * map in has carried the offset and
 * the destination since; what it is has been open until now. */
#define OBJ_TYPETAB_OFF   0x0500
#define OBJ_TYPETAB_N     64

/* `CMP [0x005D422C],0x64` / `JA` at 0x0044FB0D - the record is dropped. */
#define OBJ_TYPE_MAX      CDAT_OBJ_TYPE_MAX      /* 0x64 */
#define OBJ_TYPES         256

/* 0x004247D2-0x0042481A: eight `MOV dword ptr [m],imm` into
 * [0x005BBF50]..[0x005BBF6C], i.e. type-table entries 0x38..0x3F, AFTER the
 * copy from LOCALDAT and therefore overriding whatever the file carries.  The
 * file carries the same eight values in all 48 shipped levels, which is how
 * the +0x0500 base was confirmed rather than merely computed. */
#define OBJ_CONST_FIRST   0x38
#define OBJ_CONST_N       8
extern const uint32_t obj_const_model[OBJ_CONST_N];

/* ---- OBJSINFO and its three siblings ----------------------------------- */

#define OBJ_INFO_STRIDE   0x80
#define OBJ_INFO_RECORDS  1200                   /* 400 + 200 + 200 + 400   */
#define OBJ_INFO_BYTES    (OBJ_INFO_RECORDS * OBJ_INFO_STRIDE)
#define OBJ_INFO_MODE     0x20                   /* the transform selector  */
#define OBJ_INFO_FLAG1F   0x1f                   /* picks [0x005E0984] 1/2  */

/* The four bands, in address order, which is also model-number order. */
typedef struct {
    uint32_t    first;        /* first model number in the band             */
    uint32_t    count;        /* records                                    */
    const char *dir;          /* the directory, forward-slashed             */
    const char *info;         /* the sibling info file, relative to `files` */
} obj_band;
#define OBJ_BANDS 4
extern const obj_band obj_bands[OBJ_BANDS];

/* ---- one type ---------------------------------------------------------- */

/* ---- the per-type texture-id set, FUN_004256DA -------------------------- *
 *
 * `[0x00591C04 + type*0x40]` is a COUNT followed by up to fifteen distinct
 * texture ids, harvested out of the .ro3 sitting in the staging buffer at
 * 0x009CBF54 and written for every type below 0x40. swept the block
 * and the 0x80 bytes either side of it - 1,088 dwords - for any absolute
 * reference at all: there is exactly ONE, and it is FUN_004256DA's own
 * `LEA EDI,[ECX+0x591C04]` at 0x00425753.  Nothing reads the table, nothing
 * reads the scratch list `[0x005E49FC]` it is copied from, and the count
 * `[0x005E49F8]`'s only other reference is the `MOV ...,0` that clears it at
 * 0x004248BE.  IT HAS NO READER.
 *
 * SO THE PORT BUILDS IT FOR THE USE THE ORIGINAL NEVER MADE OF IT.  The
 * renderer needs a texture id -> plat_tex map at draw time, and this is the
 * map the original's own loader already computes: fifteen entries at most,
 * and the corpus never names more than nine for one model.  The harvest is a
 * transcription; the plat_tex beside each id is the port's.
 *
 * `MOV [EDI+ECX*4+4],EAX` at 0x0042576C with a 0x40-byte slot leaves room for
 * fifteen ids after the count, and FUN_004256DA has NO bound on its insert
 * loop - a sixteenth id would write into the next type's slot.  No shipped
 * model gets near it (`python tools/objtex.py resolve`), but the port counts
 * the overflow rather than reproducing the overrun. */
#define OBJ_TEXIDS_MAX    15

struct obj_pair;                  /* private to objscene.c */

typedef struct {
    uint32_t  model;          /* [0x00581BC4 + type*4]; 0 = no model        */
    int       have;           /* a .ro3 was present AND parsed              */
    int       gated;          /* type in {0x38, 0x3A..0x3F} - section 5     */
    int       mode;           /* OBJSINFO[model] + 0x20, or -1 when !have   */
    int       flat_lit;       /* OBJSINFO[model] + 0x1F != 0: light mode 1  */
    ro3_model m;
    /* FUN_004256DA's harvest, plus the binding the original has no field for */
    uint16_t  texid[OBJ_TEXIDS_MAX];
    plat_tex  tex[OBJ_TEXIDS_MAX];
    /* The plate `texid[k] & ~7` names, which is what OBJ_BREAK_TEX_FLOOR
     * binds.  It is here rather than in the probe because the break has to be
     * switchable per FRAME - the sweep re-renders, it does not reload - and
     * the harvest stays a pure transcription that holds no floored ids. */
    plat_tex  tex_floor[OBJ_TEXIDS_MAX];
    unsigned  ntexid;
    unsigned  texid_over;     /* ids past the fifteenth: the overrun, counted */
    /* THE PORT'S (not the original's): every plate of the type in one
     * texture, so the polygon loop stops flushing a draw at each texture
     * change - the same polygons in the same order.  0 = none (the
     * default); objscene_set_atlas. */
    plat_tex  atlas;
    float     atlas_u0[OBJ_TEXIDS_MAX], atlas_v0[OBJ_TEXIDS_MAX];
    float     atlas_su, atlas_sv;
    int       atlas_ok[OBJ_TEXIDS_MAX];
    /* THE PORT'S: the model's distinct (vertex, normal) corner pairs
     * and, per corner, which pair it is, so a lit draw does each pair's work
     * once instead of once per corner.  Built by the first lit draw
     * (objscene.c obj_pair_build); NULL until then and freed with the model. */
    struct obj_pair *pair;
    uint32_t        *pair_of_corner;
    uint32_t         npairs;
} objscene_type;

typedef struct {
    objscene_type type[OBJ_TYPES];
    uint8_t      *info;                    /* OBJ_INFO_BYTES, or NULL       */
    int           have_info;
    /* load-time tallies, every one of them a population for a figure */
    unsigned long types_named;             /* model != 0                    */
    unsigned long types_loaded;            /* .ro3 parsed                   */
    unsigned long types_missing;           /* named, no file on disk        */
    unsigned long types_badparse;          /* present and would not parse   */
    unsigned long polys_loaded;            /* `total polys` in the log      */
    unsigned long points_loaded;           /* `total points`                */
    /* the union of every loaded type's harvest - what a caller must load.
     * 256 types of at most fifteen ids is the exact ceiling, so this cannot
     * overflow and there is no truncation to reason about. */
    uint16_t      tex_all[OBJ_TYPES * OBJ_TEXIDS_MAX];
    unsigned      n_tex_all;
    unsigned long tex_ids;                 /* == n_tex_all, for the report  */
    unsigned long tex_overflow;            /* types with more than fifteen  */
    /* [0x006CE08C], the course direction, for FUN_0044E389's per-record
     * test on pols[+0x25].  objscene_init leaves it 0; the caller sets it
     * after loading, from the same value the simulation's collision uses
     * (sim_objects.route_variant) - so a record is drawn exactly when it can
     * be hit. */
    int32_t       route_variant;
} objscene;

/* ---- loading ----------------------------------------------------------- */

void objscene_init(objscene *os);
void objscene_free(objscene *os);

/* FUN_00437DC7, in one function.  Writes `<files_root>/<band>/OBJnnnn.RO3`.
 * Returns `out`.  The original's template is lower-case and the shipped tree
 * is upper-case; the caller tries both. */
char *objscene_model_path(char *out, size_t n, const char *files_root,
                          uint32_t model);

/* FUN_004247CD's copy plus its eight constants, and nothing else.  `ld` may
 * be NULL, in which case only the eight constants are set - which is what the
 * game does when LOCALDAT is missing, because 0x00420846 leaves the block
 * holding whatever it held and the block is zero at startup. */
void objscene_type_table(objscene *os, const ld_file *ld, int have_ld);

/* FUN_004248B9's loop: for every type with a non-zero model number, resolve
 * the path, read the file and parse it.  Never fails: a missing or unparsable
 * model leaves `have` zero, which is exactly what the original's
 * `[0x005BE004] == 0` return and the polygon-count test produce. */
void objscene_load(objscene *os, const ld_file *ld, int have_ld,
                   const char *files_root);

/* FUN_004247CD's SECOND loop, for one car: per car c it writes
 * three type slots from the car's display model M = FUN_0046102B(carnum)
 * (0x0042484E..0x0042487C):
 *
 *     type 0x50 + 0x14c   M          the car as drawn
 *     type 0x5A + 0x14c   M + 1
 *     type 0x5B + 0x14c   M + 100    the collision model
 *
 * and FUN_004248B9's sweep then loads them like any other type.  Call it
 * AFTER objscene_load, which resets the whole type table.  `model` is M; the
 * caller resolves it (sim_consts.car_model[carnum], the table at
 * 0x006CEF1C).  Returns 0 if type 0x50 + 0x14c has a model, -1 if not.
 *
 * Every files\b model - so every car - goes through FUN_0042566A's turn on
 * load; see obj_model_turn in objscene.c. */
#define OBJ_CAR_TYPE(c)   (0x50 + 0x14 * (c))
int objscene_load_car(objscene *os, int car, uint32_t model,
                      const char *files_root);

/* A whole type table that is NOT LOCALDAT's - the frontend's, which
 * FUN_004713BA writes as code constants (0x004715EC..0x004717C5) - loaded the
 * way FUN_004248B9 loads LOCALDAT's: `model[t]` for t < ntypes, 0 = none.
 * Each type gets the port's binding-map harvest, as a car does.  Returns the
 * number of types that now have a model. */
int objscene_load_types(objscene *os, const uint32_t *model, int ntypes,
                        const char *files_root);

/* ---- the textures ------------------------------------------------------ *
 *
 * THE ID IS THE WHOLE LOW WORD OF +0x04, AND IT IS A PLATE FILENAME.  The
 * renderer reads it at 0x0044ea31 - `MOVZX EAX,word [EBP+4]` - and the only
 * other thing in the image that touches it is the dead batching loop's key at
 * 0x0044eab7:
 *
 *     0044eab7  sub eax,0x3e8  /  and eax,0xfffffff8  /  add eax,0x3e8
 *
 * 0x3e8 is 1,000 and 1,000 is a MULTIPLE OF 8, so the subtract and the add
 * cancel and the whole expression is `eax & ~7` for every one of the 65,536
 * values the word can take (`python tools/objtex.py floor`).  It is a BATCH
 * KEY over eight consecutive ids, NOT a band test and NOT a page selector -
 * and the eight ids it groups are eight separate 256x256 plate FILES, which
 * is what the 1024x512 "atlas of 4x2 cells" in turns into
 * once it is shipped.  `python tools/objtex.py resolve`: the whole word finds
 * a file for 190 of the corpus's 192 distinct ids and the floored one for
 * 161, so the low three bits are part of the id.  OBJ_BREAK_TEX_FLOOR is the
 * other reading, kept so it can be seen to fail.
 *
 * The band and the four decimal digits are the STAGE's - FUN_00424672 and
 * FUN_00424462, which port/stage/plate.c already transcribes.  That is not a
 * contradiction of's "a different regime": what
 * differs is the UV convention (per-corner texels in [0,256) against the
 * stage's TEXTINFO quad) and the id's PROVENANCE (a polygon header against a
 * level's 256 texture slots).  Routing an object id through stage_texslot()
 * is still wrong; routing it through plate_path() is right.
 *
 * WHO LOADS THE PIXELS.  The caller, exactly as with stage_set_texture: a
 * renderer that opens files cannot be driven from a buffer, and an id an
 * object names may also be one of the level's own stage plates, which must
 * decode once and not twice. */

/* The distinct texture ids this scene needs, across every loaded type.
 * Writes at most `max` of them in ascending order and returns how many there
 * are - which may exceed `max`, so a caller can size a buffer from a first
 * call.  Zero until objscene_load has run. */
unsigned objscene_texids(const objscene *os, uint16_t *out, unsigned max);

/* Bind `t` to every type slot that harvested `id`.  A type that never names
 * `id` is untouched, and an id nobody named is a no-op.  0 (the plat.h null
 * texture) is the default and draws untextured, which is what the layer did
 * before and what it still does when a caller binds nothing.
 *
 * It ALSO binds `t` as the floored plate of every slot whose id floors to
 * `id`, which is what OBJ_BREAK_TEX_FLOOR draws.  A caller that wants that
 * row of the break sweep to mean anything must therefore load the plates for
 * `id & ~7` as well as for `id`; one that does not gets a white break rather
 * than a wrong-texture one, and the gate catches both. */
void objscene_set_texture(objscene *os, uint16_t id, plat_tex t);

/* ---- drawing ----------------------------------------------------------- */

typedef struct {
    unsigned long records;          /* class-0xff records met               */
    unsigned long dropped_type;     /* type > 0x64  (0x0044FB0D)            */
    unsigned long dropped_anchor;   /* anchor index out of range (0x0044FB2F)*/
    unsigned long gated;            /* the [0x005BBFEC] debug set           */
    unsigned long other_direction;  /* pols[+0x25] names the other direction */
    unsigned long no_model;         /* polygon count 0  (0x0044FDF2)        */
    unsigned long placed;           /* instances that reached the fan       */
    unsigned long placed_gated;     /* ...of which were the debug set: MUST 0*/
    /* Placed records whose pols[+0x18] is NOT 16.  It is 16 in 90.85% of the
     * corpus, so on some frames NOTHING carries a scale and the scale gate
     * has nothing to check - which it must say rather than pass. */
    unsigned long placed_scaled;
    unsigned long polys;            /* model polygons with >= 3 corners     */
    unsigned long state_gated;      /* hidden by the car's state table      */
    unsigned long reflect;          /* polygons given the reflection pass   */
    unsigned long tris;             /* fan triangles built                  */
    unsigned long drawn;            /* fan triangles submitted              */
    unsigned long behind;           /* dropped: a corner behind the near    */
    /* only.  The BACKDROP clips against the near plane instead of
     * dropping - FUN_0044BBEA's near pass is live for the dome, because
     * 0x004534C5 disables the FAR clip and not that one - so `behind` counts
     * the triangles that were WHOLLY behind and this counts the ones that
     * gained a corner.  The instance layer leaves both at the meaning they
     * had: it still drops. */
    unsigned long clipped;
    /* the texture census, counted over the polygons that reached the
     * fan and per DRAW rather than per triangle where it says so. */
    unsigned long draws;            /* plat_gfx_draw calls: one per id run  */
    unsigned long tex_poly;         /* polygons drawn with a plate bound    */
    unsigned long tex_none;         /* ...and with nothing bound            */
    /* Polygons whose bound plate's id is NOT a multiple of 8.  Zero is what
     * OBJ_BREAK_TEX_FLOOR produces and it is the gate that catches it; zero
     * is ALSO what a frame with no object in it produces, so the gate must
     * DECLINE on tex_poly == 0 rather than pass.  Standing rule 12. */
    unsigned long tex_odd_id;
    unsigned long tex_missing_id;   /* polygons naming an id no type harvested */
    /*'s instrument, run through the OBJECT
     * transform this time.  [0] is the fan's FIRST triangle, corners (0,1,2) -
     * the ones the stored face normal is of - and [1] is every later one.  The
     * split is not bookkeeping: a later fan triangle shares the stored normal
     * only if the polygon is planar, and the format has no planarity rule.  It
     * is also the half that sees a wrong corner ring, because the stage's
     * 2,0,1,3 leaves the FIRST triangle a cyclic rotation of (0,1,2) - the
     * same three corners, the same winding - and reverses every later one. */
    unsigned long front_cw[2], front_ccw[2], back_cw[2], back_ccw[2];
    unsigned long no_normal;
    /* Row 1 of the object's own matrix, for the records with no tilt and no
     * second triple: it must be exactly (0, pols[+0x18]/16, 0).  The scale is
     * read from the RECORD and not from the place struct, so this catches a
     * dropped scale as well as a rotation about the wrong axis. */
    unsigned long upright_tested, upright_bad;
    float         worst_upright;
    /* THE SCALE, ON EVERY PLACED RECORD AND NOT JUST THE UNTILTED ONES.  The
     * object matrix is scale * R, so the LENGTH OF ROW 0 is pols[+0x18]/16
     * whatever the tilt is and whichever of the four arms built it.  The
     * upright test above can only speak for the 95% with no tilt, and on
     * LEVEL44 every record that carries a scale also carries a tilt - so
     * without this the scale gate declined on exactly the level that would
     * have caught a dropped scale. */
    unsigned long scale_tested, scale_bad;
    float         worst_scale;
    /* world extent of the placed anchors, raw 16.16 */
    int32_t       min_x, max_x, min_y, max_y, min_z, max_z;
    float         min_sx, max_sx, min_sy, max_sy;
} objscene_stats;

void objscene_stats_init(objscene_stats *st);

/* the model path's time by phase, microseconds summed since the last
 * call, which zeroes them: [0] the corners (project, light), [1] the
 * reflection's uvs, [2] the fan (clip, emit), [3] the GL draws and flushes;
 * *draws = the plat_gfx_draw calls. */
void objscene_prof_take(unsigned long long us[4], unsigned long *draws);

/* type `type`'s atlas.  Plate k (the type's texid[k]) is at
 * (u0[k], v0[k]) and spans (su, sv) of the atlas; ok[k] 0 keeps that id on
 * its own texture.  The OBJ_BREAK_NONE path only. */
void objscene_set_atlas(objscene *os, int type, plat_tex atlas,
                        const float u0[], const float v0[], float su, float sv,
                        const int ok[]);

/* 1 draws the reflection layer as its own draws over each run (the
 * original's shape); 0, the default, adds it in the base's own draw
 * (plat_gfx_draw_env) - the same sum, rounded once, a fraction of the draws. */
void objscene_set_env_two_pass(int on);

/* 1 times the model path's phases (objscene_prof_take).  Default 0:
 * the timers are a clock read each, four to six per polygon, and a clock read
 * is a syscall on an older vendor kernel - so with it off the four figures stay
 * 0 and only the draw count is kept. */
void objscene_set_timing(int on);
int  objscene_timing(void);

/* a measurement switch: 1 skips the reflection layer's draws. */
void objscene_set_no_reflect(int on);

/* [0x005D41B0], the in-a-race flag: 1 draws an OBJSINFO +0x20 == 2 model
 * (the floodlights' glows, 121 and 122) additively, as FUN_0044EB7C does.
 * Default 0, which the probes keep. */
void objscene_set_race(int on);

/* The level's sun and shade for a record whose byte +0x24 is set
 * (0x0044FD39..0x0044FD5E): LOCALDAT +0x684, +0x688, +0x630, +0x634. */
void objscene_set_level_sun(int32_t az, int32_t el, int32_t base, int32_t scale);

/* The camera side of ral_obj_camera, built from the stage view the frame is
 * drawn with so that objects and terrain cannot be looking at two different
 * worlds.  The camera yaw is RECOVERED from the view matrix rather than passed
 * in, so it cannot go stale against the frame it is drawn with.
 *
 * view_rot is the rotation part of stage_view::m with a zero translation row;
 * view_trans is the identity with row 3 = minus the eye; view_rot_inv is the
 * TRANSPOSE, which for a rotation is what FUN_0041FA3B's adjugate plus
 * Gram-Schmidt computes. */
void objscene_camera(ral_obj_camera *cam, const stage_view *v);

/* THE BREAKS.  Standing rule 11: a gate is a claim, and a claim gets checked
 * in the failing direction.  Each one is a misreading a careful reader could
 * make from the same disassembly, and the probe asserts which gate catches it.
 */
enum {
    OBJ_BREAK_NONE        = 0,
    OBJ_BREAK_STAGE_RING  = 1,  /* fan through the STAGE's ring 2,0,1,3     */
    OBJ_BREAK_NO_ANCHOR   = 2,  /* forget pnts[pols+0x04]: stand at 0,0,0   */
    OBJ_BREAK_NO_SCALE    = 3,  /* ignore pols[+0x18]                       */
    OBJ_BREAK_YAW_AS_TILT = 4,  /* rotate about X instead of Y              */
    OBJ_BREAK_NO_GATE     = 5,  /* draw the [0x005BBFEC] debug set too      */
    OBJ_BREAK_MODEL_UNITS = 6,  /* forget that .ro3 points are 16.16        */
    /* THE ONE THE TRANSFORM CANNOT SEE.  `NEG EDX` at 0x00425FFF negates the
     * yaw on its way into Ry, and obj_transform_probe --break 1 is published
     * as NOT CAUGHT because a matrix built from +yaw is orthonormal,
     * unit-determinant, correctly scaled and correctly translated.  Here it is
     * again one level up, where something CAN see it - objscene_heading(). */
    OBJ_BREAK_YAW_SIGN    = 7,
    /* Bind the plate named by `id & ~7` - the dead batching loop's
     * BATCH KEY at 0x0044eab7 - instead of the plate named by the id.  This
     * is the ticket's "ignore the low 3 bits of +0x04, draw every polygon out
     * of atlas cell 0" written in the units the measurement settled on, and
     * it is invisible in every coverage count the project owns: the same
     * polygons are drawn, at the same places, with a texture bound to every
     * one of them.  31 of the corpus's 192 ids then have no file and 133 more
     * resolve to the WRONG file.
     *
     * The white-fallback COUNT cannot catch it and `objtex.py levels` says so
     * in as many words: the worst level under the true reading loses 40
     * polygons to a missing plate and the best level under this one loses 25,
     * so the two ranges overlap and no threshold separates them.  What
     * catches it is exact and needs no threshold - under this break every
     * plate the layer binds has an id that is a multiple of 8, and 164 of the
     * 192 ids in the corpus do not. */
    OBJ_BREAK_TEX_FLOOR   = 8,
    /* ----, the FRAME BACKGROUND.  Three misreadings of FUN_00425E6E,
     * each of which a careful reader of the OBJECT path could make, because
     * the object path does the thing each one does. */
    /* Add the camera's -position the way obj_tail does (0x00426187).  The sky
     * builder does NOT: it never touches [0x0059EF14], which is the single
     * line that makes the dome eye-centred.  Under this break the dome sits
     * at the world origin and the frame is mostly empty. */
    OBJ_BREAK_SKY_AT_ORIGIN = 9,
    /* Scale 1 instead of [0x005D7064].  The dome then has radius 1.5 WORLD
     * units around the eye instead of ~the far plane, so most of it falls
     * behind the near plane and the emitter drops it. */
    OBJ_BREAK_SKY_NO_SCALE  = 10,
    /* Compose with the IDENTITY instead of the view rotation - the reading
     * that makes the backdrop a viewport-pinned image, which is what every
     * framing of this question before run 15 assumed it was.  It fills the
     * frame exactly as well as the right one does, so no coverage count can
     * see it; what sees it is that the frame then does not change with the
     * camera's heading. */
    OBJ_BREAK_SKY_PINNED    = 11,
    OBJ_BREAK__COUNT
};

/* ---- THE FRAME BACKGROUND, --------------------------------------- *
 *
 * `FUN_0045343E` - 619 bytes, called once from `0x004536E8`, at the head of
 * the in-race frame `FUN_004536A9`.  It draws OBJECT TYPE 1, the 12-sided
 * dome, and it does it BEFORE both draw
 * queues.
 *
 * IT WAS NEVER HIDDEN, and the first draft of this comment said it was "in no
 * function, which is why the function list never showed it".  That is FALSE -
 * functions.tsv has `0045343e / 619 / 1 caller`.  What actually happened is
 * standing rule 15: printed and
 * described this exact function two days earlier and stopped ONE INSTRUCTION
 * before 0x0045347F's `MOV dword ptr [0x005D422C], 1`.  An identification is a
 * claim, and it is the one nobody checks.
 *
 *   0045343E  CALL 0x0045340C ; [0x00581A1C] = 0
 *   0045344D  CMP [0x005E9488],2 / JE ret ; CMP ...,1 / JE ret   the mode gate
 *   00453467  CALL 0x00425E6E                 the dome's matrix -> [0x0059F054]
 *   0045346C  REP MOVSD [0x0059F054] -> [0x0059F094]   16 dwords
 *   0045347F  [0x005D422C] = 1                THE TYPE
 *   00453489  [0x005E948C] = 4                the draw context
 *   00453493  [0x005E7F14/18/1C] = 0xFF       white
 *   004534B1  [0x005E7F24] = [0x005E7F28] = 0 no pre-rotation
 *   004534C5  [0x005D4240] = 1                far clip OFF (0x0044BC03)
 *   004534CF  CALL 0x0044C4E8                 ZENABLE 1, ZWRITE 1, ZFUNC 8
 *   004534D4  [0x005E0984] = 0
 *   004534DE  CALL 0x0044FDE0                 the object draw
 *   004534E8  CALL 0x0044C4BA                 ZENABLE 1, ZWRITE 1, ZFUNC 4
 *   004534ED  [0x005E948C] = 0 ; [0x005D4240] = 0
 *   00453515  ... the SUN, which is not this port's yet
 *
 * ZFUNC 8 is D3DCMP_ALWAYS and it is the ONLY write of 8 to state 23 in the
 * image (counts `4`x3, `8`x1).  With Z-WRITE still
 * on, the dome overwrites every pixel and initialises every depth value in
 * one pass - SO THE SKY DRAW IS THE CLEAR, colour and depth both, which is
 * what guessed and could not measure.  Nothing clears the colour buffer
 * anywhere and nothing needed to.
 *
 * `[0x005E948C] == 4` is read at three places and means "backdrop":
 *   0x00425FBC  the object transform RETURNS AT ONCE - the matrix is already
 *               built, which is why this path can hand it one.
 *   0x0044EC33  the block guarded by [0x005D4230] is skipped.
 *   0x0044EE2F  the colour is forced to 0xFFFFFF - FULLBRIGHT, unlit.
*/
#define OBJ_BACKDROP_TYPE   1

/* LOCALDAT, whose block base is 0x005BB970. */
#define OBJ_SKY_YAW_OFF     0x680      /* [0x005BBFF0], a BYTE angle, << 8   */
#define OBJ_SKY_Y_OFF       0x664      /* [0x005BBFD4], signed, << 8 = 16.16 */

typedef struct {
    /* LOCALDAT +0x680.  Domain over the 48 shipped levels is 0..255 with 28
     * zeros, which is what a 256-step binary angle looks like. */
    int     yaw;
    /* LOCALDAT +0x664.  -47..99 over the corpus, 20 zeros.  It is the dome
     * CENTRE's height above the eye, in 16.16 after the << 8 AND AFTER the
     * scale, because FUN_00425E6E scales all sixteen floats and the
     * translation row is already filled when it does. */
    int     y;
    /* [0x005D7064], written once, at 0x0044C666:
     *     [0x005D705C] = (float)(zfar << 16) / D          ; 0x0044C61D
     *     [0x005D7064] = (int)([0x005D705C] * 0.9 / 1.5 / 65536)
     * with zfar = radius * 32 and D = clamp(100 - detail, 90, 100) / 100.
     * 1.5 is the dome's own radius and 0.9 is where it is put, so the shell
     * lands at 0.9 * zfar / D world units - AT the far plane when D is 0.90.
     * stage_depth_scale() is 1/[0x005D705C] and this is built from it. */
    int32_t scale;
} objscene_backdrop;

/* FUN_0044C516's tail plus 0x0044C666.  `ld` may be absent, which gives yaw
 * and y of zero - the value 20 of the 48 shipped levels carry anyway. */
void objscene_backdrop_params(objscene_backdrop *b, const ld_file *ld,
                              int have_ld, int32_t radius, int32_t detail);

/* FUN_00425E6E, and it is four calls:
 *     [0x0059ED14] = Ry(-(yaw << 8))                     0x00425E79
 *     [0x0059ED94] = T(0, y << 8, 0)                     0x00425EB0
 *     [0x0059EE54] = copy(Ry) with that translation      0x00425EC7
 *     all sixteen floats *= scale                        0x00425F0C
 *     [0x0059F054] = [0x0059EE54] x [0x0059EF94]         0x00425F30
 * THE CAMERA'S TRANSLATION IS NEVER ADDED.  That one absence is the whole
 * "it rotates with the camera and translates with nothing" the operator
 * reported from run 17, and OBJ_BREAK_SKY_AT_ORIGIN is it put back. */
void objscene_backdrop_matrix(ral_mat4 *out, const objscene_backdrop *b,
                              const ral_obj_camera *cam, int brk);

/* Draw type 1 through that matrix.  Same fan, same texture runs and same
 * projection as objscene_draw_record - the difference is the matrix, the
 * fullbright white of 0x0044EE2F, and that the caller brackets it with
 * ZFUNC ALWAYS.  Stats land in the same struct; `polys`/`tris`/`drawn`/
 * `behind`/`draws`/`tex_poly`/`tex_none` are the fields it fills. */
void objscene_draw_backdrop(objscene *os, const objscene_backdrop *b,
                            const stage_view *v, const ral_obj_camera *cam,
                            int brk, objscene_stats *st);

/* ---- the heading test, and it is the yaw SIGN's instrument -------------- *
 *
 * THE PROBLEM.  0x00425FFF negates the yaw, and NO IDENTITY OVER THE
 * TRANSFORM'S OWN OUTPUT CAN SEE THAT SIGN: a matrix built from +yaw is
 * orthonormal, unit-determinant, correctly scaled and correctly translated.
 * publishes `--break 1` as NOT CAUGHT and
 * says settling it needs an external reference.  IT DOES NOT.
 *
 * THE INSTRUMENT, and why it works.  Let theta be an object's world heading
 * and phi the direction from its anchor to the nearest other instance of the
 * same type.  A level places LONG THIN objects - fence panels, walls, marker
 * boards, barriers - in RUNS, and within a run the object is laid at a FIXED
 * angle to the run: along it for a fence, across it for a marker board.  So
 * theta - phi is a CONSTANT, whatever that constant is.
 *
 * Negating the yaw maps theta -> -theta and leaves phi alone, so the measured
 * quantity becomes -theta - phi = -2*phi - (theta - phi).  It therefore stops
 * being constant AS SOON AS phi VARIES - i.e. as soon as the track bends.
 *
 * So the statistic is not "aligned", it is CONCENTRATION: the circular
 * resultant length R of the DOUBLED angle (doubled because a fence panel laid
 * the other way up is the same fence, so the axis is defined mod 180).  R = 1
 * is a perfectly consistent placement and R = 0 is uniform noise.  The right
 * sign concentrates and the wrong one smears - and the whole thing is
 * arithmetic over the level file and the .ro3 models, with no camera, no
 * display and no photograph.
 *
 * `conc` comes back as the pair-weighted mean of R over the types that
 * qualify: at least OBJ_HEAD_MIN_PAIRS pairs, and a model whose XZ point
 * cloud is at least OBJ_THIN_RATIO times longer than it is wide - which
 * excludes a tree card, a barrel and a telegraph pole, none of which has a
 * heading to be wrong about.
 *
 * Returns 0 and leaves the outputs zero if no type qualifies.  Every figure
 * carries its own population, which is `tested`.
*/
#define OBJ_THIN_RATIO      2.0
#define OBJ_HEAD_MIN_PAIRS  8
/* Two instances further apart than this are not in the same run.  6 world
 * units: a fence panel is a couple of metres and a cell is 32. */
#define OBJ_RUN_MAX         6.0

int objscene_heading(objscene *os, const cdat_payload *cd, int brk,
                     unsigned long *tested, double *conc);

/* Draw one class-0xff record.  `pol` is the 64-byte `pols` record; `cd` is
 * needed for the anchor point.  Submits through plat_gfx_draw once per RUN of
 * polygons sharing a texture id: the fan goes into one buffer and is flushed
 * when the bound texture changes, which for 141 of the corpus's 191 models
 * never happens because they name exactly one id (`objtex.py resolve`).
 *
 * The corner u and v are per-corner TEXELS in [0, 256) -
 * measured that over 707,004 values - so the divisor is RO3_TEXTURE_PAGE and
 * not the plate's width.  They coincide on every plate an object names, and
 * they are different claims: the page is a property of the FORMAT.
 *
 * The coincidence is now exact rather than "189 of 190 and the odd one draws
 * white anyway": TX0059.PCX is 320 wide in the FILE, and since 2026-09-21
 * plate_load clamps it to the decoder's 256x256 request the way the original
 * does, so all 190 resolving plates are 256x256 DECODED,
 */
void objscene_draw_record(objscene *os, const cdat_payload *cd,
                          const uint8_t *pol, const stage_view *v,
                          const ral_obj_camera *cam, int brk,
                          objscene_stats *st);

/* Draw `type`'s model at a caller-supplied pose: `rot`'s upper-left
 * 3x3 is the rotation in the engine's row-vector convention (v' = v * M, the
 * object's axes in the rows) and `pos` is the world position in raw 16.16.
 * The car record's matrix (car+0x0B4) and pos (car+0x134) are exactly that.
 *
 * THE PORT'S PATH, NOT A TRANSCRIPTION.  How the original builds the car's
 * draw matrix from its record has not been read.  This goes through
 * obj_tail's shape - rotation, placed at pos, translated by the camera's
 * -position, then into view space - and the same polygon loop as
 * objscene_draw_record, so the car is drawn by the object path's fan,
 * texture runs, projection and fog, owner the
 * follow-up. */
void objscene_draw_placed(objscene *os, int type, const float rot[16],
                          const int32_t pos[3], const stage_view *v,
                          const ral_obj_camera *cam, objscene_stats *st);

/* Light mode 3, the race's car (0x0045F6DD..0x0045F7A1 set it per car;
 * Per corner, 0x0044F035 then FUN_0044F238:
 *     c = base (+ the local lights, not ported)
 *     i = clamp(FISTP(dot(n_view, light) * diffuse), -255, 255)
 *     each channel moves toward lit[] by i/256 when i >= 0, else toward
 *     shadow[] by -i/256: ch += ((target - ch) * |i|) >> 8, clamped 0..255
 * and then 0x0044A790 multiplies by the polygon's +0x0A global colour.
 *   light    [0x005D6F3C..44]  FUN_00425F36 from [0x5E7F24]/[0x5E7F28]
 *   diffuse  [0x005E7F20]      LOCALDAT +0x6A8
 *   base     [0x005E0964]      car+0x240 (0x00453019)
 *   lit      LOCALDAT +0x60C, +0x610, +0x614  ([0x005BBF7C..84]) R, G, B
 *   shadow   LOCALDAT +0x600, +0x604, +0x608  ([0x005BBF70..78]) R, G, B */
typedef struct objscene_light {
    float    light[3];
    int32_t  diffuse;
    uint32_t base;
    int32_t  lit[3];
    int32_t  shadow[3];
    /* The reflection pass: a polygon
     * whose byte +9 is non-zero gets a second, ADDITIVE pass of texture id
     * 0x87 at FUN_0044F92E's sphere-map uvs, weight
     * clamp(+9 * reflect * fog >> 12, 0, 255).
     *   reflect  [0x00581BBC] as the car's draw sees it: 0x0045F053's
     *            min(0x3FCE0 - [0x005D6C14], 0x40000) * [0x005BD9C8] >> 18,
     *            then 0x0045F53C's x (1 - fade) outside the cockpit views.
     *            0 = no pass.
     *   env_tex  the plate texture id 0x87 resolves to (0 = none)         */
    uint32_t reflect;
    plat_tex env_tex;
} objscene_light;

/* the car's PART GATE, 0x0044E5D2..0x0044E62E.  A car polygon's byte
 * +7 (ro3_poly::flags bits 24..28) is a part number 0..31, and a polygon is
 * drawn only if the gate passes it.  The fields are the globals the test
 * reads; rally.c's car_parts builds them from the car's row
 * (0x0057E10C + id*0x12C) the way 0x0045F44A..0x0045F6D3 does.
 *   mask       [0x005BA5DC]  bit n set = part n drawn (not tested if all)
 *   lit_part   [0x00581BA8]  row +0x64: drawn only while lit_on
 *   lit_on     [0x005E0960]  LOCALDAT +0x6A0 bit 1 (0x00442ABB; unnamed)
 *   out_part   [0x00581B98], [0x00581B9C]  row +0x68/+0x6C: hidden inside
 *   in_part    [0x00581BA0], [0x00581BA4]  row +0x70/+0x74: hidden outside
 *   inside     [0x00684628]  car+0xAB8 = (view == 2 || 3), 0x0045E92E
 * A part number of -1 matches nothing (the row's "none").
 *
 *   state      [0x005823D0]  FUN_0044E663, FUN_0044E477's fourth gate,
 *                            inside a level: a polygon whose byte +6 is
 *                            non-zero and not 0x1C draws only when its
 *                            byte +0x0B equals state[+6].  0x0045F9E6
 *                            fills it per car before the draw and
 *                            0x0045F9D3 zeroes slots 0..63 after it
 * NULL
 *                            draws every state at once - every race-number
 *                            digit, both wheel plates, every lamp. */
typedef struct objscene_parts {
    uint32_t mask;
    int32_t  lit_part;
    int      lit_on;
    int32_t  out_part[2];
    int32_t  in_part[2];
    int      inside;
    const int32_t *state;
    /* car+0x700..0x714, the six windows' crack words that 0x00454C53 copies
     * in each frame (a 0x80000000 slot then borrows the next live one).
     * NULL = all 0: no window cracked. */
    const uint32_t *glass;
    /* The crush pass's live vertex buffer (FUN_004C079B): model A moved
     * toward the crushed model by caroctdamage, which the original draws.
     * npoints entries, from objscene_car_crush.  NULL = the rest mesh. */
    const ro3_vec  *points;
    const objscene_light *light;      /* NULL: the diagnostic shade         */
    /* MASK 1, [0x005D423C] (0x0045F311..0x0045F3CB): all ones, less the part
     * bit of each bodywork piece carpartsleft (car+0x298) says has come off.
     * Tested FIRST by the gate (0x0044E597).  Build it with
     * rally.c's car_mask1; 0 here would hide everything, so callers set it. */
    uint32_t mask1;
} objscene_parts;

int  objscene_part_drawn(const objscene_parts *g, unsigned part);

/* The six windows' crack words for car slot `car`, as the original's car
 * record holds them at car+0x700..0x714.
 * The engine's crush pass computes them: model 0x50 + 20*car is moved from
 * its rest shape toward the crushed model 0x5A + 20*car by the octant damage
 * (FUN_004C079B), and each word is how far, in percent, the vertices of that
 * window's crack polygons have gone (FUN_004C0B01, against FUN_004C0A02's
 * rest baseline); 0x00454C6C's borrow then fills a window with no polygons
 * from the next one that has some.  `oct` is car+0x64..0x80, caroctdamage.
 * Returns 0, or -1 (and `out` untouched) when either model is missing or the
 * two do not pair up. */
int  objscene_car_windows(const objscene *os, int car, const float oct[8],
                          uint32_t out[6]);
/* objscene_car_windows, and the dented model A itself into `pts` (when not
 * NULL; type 0x50 + 20*car's npoints entries) for objscene_parts::points. */
int  objscene_car_crush(const objscene *os, int car, const float oct[8],
                        uint32_t out[6], ro3_vec *pts);

/* one impact-flash slot's glow, FUN_004530F2.  1 drawn, 0 no glow model. */
int objscene_draw_glow(objscene *os, const int32_t pos[3], int scale16,
                       const stage_view *v, const ral_obj_camera *cam,
                       objscene_stats *st);

/* objscene_draw_placed through a part gate; `parts` NULL draws every part. */
void objscene_draw_placed_parts(objscene *os, int type, const float rot[16],
                                const int32_t pos[3], const stage_view *v,
                                const ral_obj_camera *cam, objscene_stats *st,
                                const objscene_parts *parts);

/* The same, with `pre` applied to the model first (a wheel's own matrix,
 * game/wheels.h): 0x0045F8BB multiplies [0x005F46A0..] by the car's rotation
 * before the position and the view.  NULL is objscene_draw_placed_parts. */
void objscene_draw_placed_pre(objscene *os, int type, const ral_mat4 *pre,
                              const float rot[16], const int32_t pos[3],
                              const stage_view *v, const ral_obj_camera *cam,
                              objscene_stats *st, const objscene_parts *parts);

#endif /* RALLY_OBJSCENE_H */
