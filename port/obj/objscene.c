/* objscene.c - the object instance layer's loader and emitter.
 *
 * Read objscene.h first: it carries the transcription of the five functions
 * this file reproduces.  What is below is the code and the places where the
 * port must NOT be literal.
*/

#include "objscene.h"
#include "objlit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The x87 stack's type, as objlit.c's lit_ld: the 80-bit extended on x86,
 * `double` elsewhere - aarch64's long double is software quad, and the car's
 * per-corner light (obj_light3) cost ~17 ms a frame on the RG40XX-H. */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
typedef long double obj_ld;
#define OBJ_RINT nearbyintl
#else
typedef double obj_ld;
#define OBJ_RINT nearbyint
#endif

/* 0x004247D2-0x0042481A, in entry order 0x38..0x3F. */
const uint32_t obj_const_model[OBJ_CONST_N] = {
    0xcf, 0x79, 0xcc, 0xcd, 0xc8, 0xc9, 0xca, 0xcb
};

/* FUN_00437DC7's ladder and FUN_004380D2's four loads, in ONE table because
 * they are the same four bands - see objscene.h.  The `info` paths are in
 * ADDRESS order, which is also model-number order; FUN_004380D2 issues them
 * in a different order and that is the only reason the tiling is not obvious
 * from the disassembly. */
const obj_band obj_bands[OBJ_BANDS] = {
    {    0u, 400u, "a",      "a/OBJSINFO"      },
    {  400u, 200u, "b",      "b/CARSINFO"      },
    {  600u, 200u, "g/frnt", "g/frnt/FRNTINFO" },
    {  800u, 400u, "s/extr", "s/extr/EXTRINFO" }
};

/* [0x0057B09C] = 4096.0f, raw 16.16.  stage_view carries it; named here so
 * the fan can read it without a second constant. */

/* ===================================================================== *
 *  THE LOADER                                                           *
 * ===================================================================== */

void objscene_init(objscene *os)
{
    memset(os, 0, sizeof *os);
}

/* one distinct (vertex, normal) corner pair of a model, and what a
 * lit draw has worked out for it.  Each part carries the stamp (op_stamp) of
 * the draw that filled it, so nothing is carried from one draw to the next:
 * the matrix, the light and the dented points differ between draws. */
struct obj_pair {
    int32_t  vi, ni;
    uint32_t st_vn, st_col, st_env;
    uint32_t col_key, col;          /* col is for (vn, lit, grey) = col_key */
    float    vn[3];
    float    eu, ev;
};

static void obj_pair_free(objscene_type *ty)
{
    free(ty->pair);
    free(ty->pair_of_corner);
    ty->pair           = NULL;
    ty->pair_of_corner = NULL;
    ty->npairs         = 0;
}

void objscene_free(objscene *os)
{
    int t;

    for (t = 0; t < OBJ_TYPES; t++) {
        obj_pair_free(&os->type[t]);
        if (os->type[t].have) {
            ro3_free(&os->type[t].m);
            os->type[t].have = 0;
        }
    }
    free(os->info);
    os->info = NULL;
    os->have_info = 0;
}

char *objscene_model_path(char *out, size_t n, const char *files_root,
                          uint32_t model)
{
    const char *dir = obj_bands[0].dir;
    int i;

    /* `if (399 < eax) ... if (599 < eax) ... if (799 < eax)` - a ladder, so
     * a model number past the last band's end still resolves to that band.
     * Reproduced rather than clamped: the original cannot fail here. */
    for (i = OBJ_BANDS - 1; i >= 0; i--) {
        if (model >= obj_bands[i].first) {
            dir = obj_bands[i].dir;
            break;
        }
    }
    /* `%04d` is FUN_00437DC7's four digit stores: thousands, hundreds, tens,
     * units, each `+ '0'`, so a model number over 9999 would wrap the
     * thousands digit.  No shipped level names one - the largest is 495. */
    snprintf(out, n, "%s/%s/OBJ%04u.RO3", files_root, dir, model % 10000u);
    return out;
}

static void *slurp(const char *path, size_t *len)
{
    FILE  *f;
    long   n;
    void  *buf;

    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* The shipped tree is upper-case and the original's template is lower-case;
 * the game runs on a case-insensitive filesystem and the port does not. */
static void *slurp_either(char *path, size_t *len)
{
    void  *b = slurp(path, len);
    char  *p;

    if (b) {
        return b;
    }
    for (p = path; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') { *p = (char)(*p - 'A' + 'a'); }
    }
    return slurp(path, len);
}

void objscene_type_table(objscene *os, const ld_file *ld, int have_ld)
{
    int t;

    for (t = 0; t < OBJ_TYPES; t++) {
        os->type[t].model = 0u;
        os->type[t].mode  = -1;
        /* FUN_0044E389: type 0x38, and 0x3A..0x3F, need [0x005BBFEC] != 0,
         * and nothing in RAL.EXE writes it.  Type 0x39 falls through to the
         * general arm and does not occur in any shipped level.
         */
        os->type[t].gated = (t == 0x38 || (t >= 0x3a && t <= 0x3f));
    }

    /* 0x00424822-0x00424839: `for ecx = 1; ecx != 0x40; ecx++`.  Entry 0 is
     * NOT copied - FUN_004256C7 has just zeroed the whole 256-entry
     * destination, so type 0 keeps 0 and loads nothing. */
    if (have_ld && ld) {
        for (t = 1; t < OBJ_TYPETAB_N; t++) {
            os->type[t].model = ld_dw(ld, (unsigned)(OBJ_TYPETAB_OFF + t * 4));
        }
    }
    /* ...and then the eight constants, which win. */
    for (t = 0; t < OBJ_CONST_N; t++) {
        os->type[OBJ_CONST_FIRST + t].model = obj_const_model[t];
    }
}

/* FUN_004380D2's four reads, concatenated in ADDRESS order into one 1,200
 * record table.  A missing sibling leaves its band zero, which is what the
 * original's discarded carry flag produces. */
static void info_load(objscene *os, const char *files_root)
{
    char   path[1024];
    int    i;
    size_t got = 0;

    os->info = (uint8_t *)calloc(OBJ_INFO_BYTES, 1);
    if (!os->info) {
        return;
    }
    for (i = 0; i < OBJ_BANDS; i++) {
        size_t len = 0;
        void  *buf;

        snprintf(path, sizeof path, "%s/%s", files_root, obj_bands[i].info);
        buf = slurp_either(path, &len);
        if (!buf) {
            continue;
        }
        if (len > (size_t)obj_bands[i].count * OBJ_INFO_STRIDE) {
            len = (size_t)obj_bands[i].count * OBJ_INFO_STRIDE;
        }
        memcpy(os->info + (size_t)obj_bands[i].first * OBJ_INFO_STRIDE,
               buf, len);
        free(buf);
        got += len;
    }
    os->have_info = (got > 0);
}

/* OBJSINFO[model] + 0x20, the transform's mode selector.  Returns 0 - the
 * general Euler arm - for a model number the table cannot hold, which is the
 * ONE place this port deliberately differs from the original: FUN_00425FAA
 * indexes the table before anything checks the type, and for an unregistered
 * type that read lands 128 bytes below it.  The caller only ever asks about a
 * type whose model LOADED, so the difference is unobservable - but it is a
 * difference and it is written down. */
static int info_mode(const objscene *os, uint32_t model)
{
    if (!os->have_info || model >= (uint32_t)OBJ_INFO_RECORDS) {
        return RAL_OBJ_MODE_EULER;
    }
    return os->info[(size_t)model * OBJ_INFO_STRIDE + OBJ_INFO_MODE];
}

/* FUN_004256DA, 0x004256da-0x00425776, transcribed.
 *
 *   004256e0  LEA ESI,[0x9CBF54]                the staging buffer
 *   004256e6  EAX = [ESI+4]  EBX = [ESI+8]      npoints, npolys
 *   004256ec  ESI += 0x2C + npoints*0xC         past the header and points
 *   004256f4  ECX = [ESI]    EAX = word [ESI+4] corners, and THE TEXTURE ID
 *   00425709  linear search of [0x005E49FC] for EAX, append if absent
 *   0042571f  ESI += 0x10 + ECX*0x10            the next polygon
 *   0042573b  [0x005E49F8] = count
 *   00425741  if type >= 0x40: return           types 0x40 and up get no slot
 *   00425753  LEA EDI,[type*0x40 + 0x591C04]
 *   00425759  [EDI] = count;  [EDI+ECX*4+4] = each id
 *
 * The walk is the parser's, so the port reads it off ro3_model rather than
 * stepping a byte pointer - the two agree by construction because ro3_parse
 * is the same walk, and `formats-agree` gates that against tools/ro3.py.
 *
 * `word [ESI+4]` is the LOW WORD of the polygon header's +0x04, which is what
 * 0x0044ea31 loads at draw time.  ro3_poly::flags holds the whole dword.
 *
 * THE 0x40 CUT IS THE FUNCTION'S OWN, not the caller's.  0x00424A0A calls
 * this for `type < 0x41`, so type 0x40 harvests into the scratch list and
 * then falls out at 0x00425748 without writing a slot.  The port has no
 * scratch list to leave behind, so the cut is simply where the harvest stops. */
#define OBJ_TEX_TYPE_MAX  0x40u

/* The union, built once at load into a buffer that CANNOT overflow: 256 types
 * of at most fifteen ids is the exact ceiling, so there is no cap to exceed
 * and no silent truncation to reason about.  7.5 KB inside a struct that
 * already holds 256 parsed models. */
static void tex_union(objscene *os)
{
    int t;

    os->n_tex_all = 0u;
    for (t = 0; t < OBJ_TYPES; t++) {
        const objscene_type *ty = &os->type[t];
        unsigned k;

        for (k = 0u; k < ty->ntexid; k++) {
            uint16_t id = ty->texid[k];
            unsigned i  = 0u, j;

            while (i < os->n_tex_all && os->tex_all[i] < id) { i++; }
            if (i < os->n_tex_all && os->tex_all[i] == id) { continue; }
            for (j = os->n_tex_all; j > i; j--) {
                os->tex_all[j] = os->tex_all[j - 1];
            }
            os->tex_all[i] = id;
            os->n_tex_all++;
        }
    }
}


static void tex_harvest(objscene *os, objscene_type *ty)
{
    size_t p;

    if ((unsigned)(ty - os->type) >= OBJ_TEX_TYPE_MAX) {
        return;                              /* 0x00425741's `JAE`          */
    }
    for (p = 0u; p < ty->m.npolys; p++) {
        uint16_t id = (uint16_t)ro3_texture_id(&ty->m.polys[p]);
        unsigned k;

        for (k = 0u; k < ty->ntexid; k++) {
            if (ty->texid[k] == id) { break; }
        }
        if (k < ty->ntexid) {
            continue;
        }
        if (ty->ntexid >= OBJ_TEXIDS_MAX) {
            /* 0x00425765's loop has no bound and a 0x40-byte slot holds
             * fifteen, so the original would write into the next type's.  No
             * shipped model reaches nine - counted, not reproduced. */
            ty->texid_over++;
            if (ty->texid_over == 1u) { os->tex_overflow++; }
            continue;
        }
        ty->texid[ty->ntexid++] = id;
    }
}

/* FUN_0042566A, per point at 0x0042546C inside FUN_004252A5's ingest:
 * `CMP [0x005E7E78],0x190 / JB ; CMP ...,0x258 / JAE` - unsigned, so exactly
 * the files\b band, 400..599 - then (x, y, z) -> (z, y - [0x005C6CF8], -x)
 * with [0x005C6CF8] = 0xB333.  port/sim/objects.c carries the same turn for
 * the collision copy (model_point_turn);.
 *
 * ADDED BY, and it is a correction: until then this loader drew every
 * files\b model UNTURNED - 90 degrees off about y and 0.7 high.  Level
 * objects reach that band too (LEVEL21's types 19 and 20 are models 477 and
 * 476, `python tools/objmodels.py census`), so it moved those as well.  The
 * stored NORMALS are not turned, and need not be: the car models' normal
 * table was already measured to sit 90 degrees about y from the raw points
 *this turn is what brings the points
 * into the normals' frame.  The ro3 parser hands back 16.16 / 65536, so the
 * 0xB333 is subtracted in the same units; both are exact in a double. */
#define OBJ_TURN_LO  0x190u
#define OBJ_TURN_HI  0x258u
#define OBJ_TURN_DY  ((double)0xB333 / 65536.0)

static void obj_model_turn(objscene_type *ty)
{
    size_t i;

    if (!(ty->model >= OBJ_TURN_LO && ty->model < OBJ_TURN_HI)) {
        return;
    }
    for (i = 0; i < ty->m.npoints; i++) {
        ro3_vec q = ty->m.points[i];
        ty->m.points[i].x = q.z;
        ty->m.points[i].y = q.y - OBJ_TURN_DY;
        ty->m.points[i].z = -q.x;
    }
}

/* FUN_00424997 for one type: path, read, parse, ingest.  Returns 1 if the
 * type now has a model. */
static int obj_load_type(objscene *os, objscene_type *ty,
                         const char *files_root)
{
    char    path[1024];
    void   *buf;
    size_t  len = 0;
    char    err[256];

    os->types_named++;

    objscene_model_path(path, sizeof path, files_root, ty->model);
    buf = slurp_either(path, &len);
    if (!buf) {
        /* `if ([0x005BE004] == 0) return` at 0x004249BF - a missing file
         * is not an error, the type simply keeps a polygon count of 0.
         * Two model numbers in the shipped corpus have no file at all:
         * 170 and 399. */
        os->types_missing++;
        return 0;
    }
    err[0] = '\0';
    if (ro3_parse(buf, len, &ty->m, err, sizeof err) != 0) {
        os->types_badparse++;
        free(buf);
        return 0;
    }
    free(buf);
    obj_model_turn(ty);
    ty->have  = 1;
    ty->mode  = info_mode(os, ty->model);
    /* 0x0044FDA9: OBJSINFO +0x1F picks light mode 1 over 2 for a record */
    ty->flat_lit = os->have_info && ty->model < (uint32_t)OBJ_INFO_RECORDS
                && os->info[(size_t)ty->model * OBJ_INFO_STRIDE + OBJ_INFO_FLAG1F] != 0;
    os->types_loaded++;
    os->polys_loaded  += (unsigned long)ty->m.npolys;
    os->points_loaded += (unsigned long)ty->m.npoints;
    return 1;
}

void objscene_load(objscene *os, const ld_file *ld, int have_ld,
                   const char *files_root)
{
    int  t;

    objscene_type_table(os, ld, have_ld);
    info_load(os, files_root);

    /* FUN_004248B9 sweeps all 256 types.  Only 1..0x3F can carry a model
     * number from LOCALDAT; the car slots at 0x50 and up come from
     * FUN_004247CD's second loop over [0x006CE044], which is the race's car
     * list - objscene_load_car, one car at a time. */
    for (t = 0; t < OBJ_TYPES; t++) {
        objscene_type *ty = &os->type[t];

        if (ty->model == 0u) {
            continue;                 /* `if ([0x005E7E78] == 0) return`   */
        }
        if (obj_load_type(os, ty, files_root)) {
            tex_harvest(os, ty);
        }
    }
    tex_union(os);
    os->tex_ids = os->n_tex_all;
}

/* One type, any model number, outside the LOCALDAT sweep.  Returns 1 if the
 * type now has a model.  Does NOT rebuild the union - the callers do, once. */
static int obj_load_one(objscene *os, int type, uint32_t model,
                        const char *files_root)
{
    objscene_type *ty = &os->type[type];

    obj_pair_free(ty);
    if (ty->have) {
        ro3_free(&ty->m);
    }
    memset(ty, 0, sizeof *ty);
    ty->model = model;
    ty->mode  = -1;
    if (model == 0u || !obj_load_type(os, ty, files_root)) {
        return 0;
    }
    /* THE PORT'S BINDING MAP, NOT THE ORIGINAL'S HARVEST.  FUN_004256DA
     * stops at type 0x40 (0x00425741's `JAE`), so the original never builds
     * a texture-id set for a car type - and never needed one, because
     * nothing reads the set it does build.  This port binds plates through
     * that set, so every type gets one; the ids are the polygons' own, read
     * the way 0x0044ea31 reads them. */
    {
        size_t pi;
        for (pi = 0; pi < ty->m.npolys; pi++) {
            uint16_t id = (uint16_t)ro3_texture_id(&ty->m.polys[pi]);
            unsigned k;
            for (k = 0u; k < ty->ntexid; k++) {
                if (ty->texid[k] == id) { break; }
            }
            if (k < ty->ntexid) { continue; }
            if (ty->ntexid >= OBJ_TEXIDS_MAX) { ty->texid_over++; continue; }
            ty->texid[ty->ntexid++] = id;
        }
    }
    return 1;
}

int objscene_load_car(objscene *os, int car, uint32_t model,
                      const char *files_root)
{
    static const unsigned off[3] = { 0x00u, 0x0Au, 0x0Bu };
    static const uint32_t add[3] = { 0u, 1u, 100u };
    int  i, ok = 1;

    if (car < 0 || 0x50 + 0x14 * car + 0x0B >= OBJ_TYPES || model == 0u) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (!obj_load_one(os, 0x50 + 0x14 * car + (int)off[i], model + add[i],
                          files_root) && i == 0) {
            ok = 0;
        }
    }
    tex_union(os);
    os->tex_ids = os->n_tex_all;
    return ok ? 0 : -1;
}

/* THE WINDOWS' CRACK WORDS - the engine's crush pass.
 *
 * The engine's object array starts at 0x005787E0 (init_mark_q, 0x00454E85),
 * 0x60 below the render table at 0x00578840 that FUN_004547E1 swaps with the
 * car, so the table's +0x258..0x26C, which it copies into car+0x700..0x714,
 * is the engine object's +0x2B8..0x2CC: the demo build's
 * crush_info.window[6], with window_old[6] (+0x2D0) the rest baseline.
 *
 * Model A, type 0x50 + 20*car (crush_info +0x29C), is the car as drawn; B,
 * +0x0A (+0x2A0, the file one past it), is the same mesh fully crushed.
 * Coordinates are the vertex pool's int32 16.16 (0x0057C224), which the
 * parser hands back divided by 65536 after obj_model_turn: exact, so they
 * are rebuilt bit for bit.  The FPU is at 80 bits, round to nearest
 * every product below is exact in a double, so lrint is FISTP.
 *
 *   FUN_004C079B, per vertex of A whose rest copy differs from B: the
 *     octant is B's three sign bits, x<<2 | y<<1 | z, and
 *     live = FISTP((B - rest) * caroctdamage[octant] + rest).
 *   FUN_004C0B01, per polygon of A with byte +6 == 0x1C and k = byte +0x0B
 *     in 1..6, paired by index with B's: window[k] = FISTP(window[k] +
 *     sqrt(|live - B|^2)) per corner; then each word becomes
 *     FISTP(100 - window / window_old * 100) ([0x0070D558] = 100.0f).
 *     A window with no polygons is 0/0 there, which FISTP stores as
 *     0x80000000.
 *   FUN_004C0A02, at init, runs the same sum with A at rest into
 *     window_old.
 *   0x00454C6C, in FUN_004547E1: a 0x80000000 word takes the next word
 *     round the six that is not.
*/
static int32_t crush_fix(double v)
{
    return (int32_t)lrint(v * RO3_FIXED_ONE);
}

static void crush_sums(const ro3_model *a, const ro3_model *b,
                       const int32_t *live, const int32_t *bv, int32_t s[7])
{
    size_t pi;
    int    k;

    for (k = 0; k < 7; k++) s[k] = 0;
    for (pi = 0; pi < a->npolys; pi++) {
        const ro3_poly *pa = &a->polys[pi], *pb = &b->polys[pi];
        int32_t j;

        k = ro3_sel_b(pa);
        if (ro3_sel_a(pa) != 0x1C || k < 1 || k > 6) continue;
        for (j = 0; j < pa->corners; j++) {
            const int32_t *p = &live[3 * a->corner_vert[pa->first_corner + j]];
            const int32_t *q = &bv[3 * b->corner_vert[pb->first_corner + j]];
            double dx = (double)(int32_t)((uint32_t)p[0] - (uint32_t)q[0]);
            double dy = (double)(int32_t)((uint32_t)p[1] - (uint32_t)q[1]);
            double dz = (double)(int32_t)((uint32_t)p[2] - (uint32_t)q[2]);
            s[k] = (int32_t)lrint(sqrt(dz * dz + dy * dy + dx * dx)
                                  + (double)s[k]);
        }
    }
}

int objscene_car_crush(const objscene *os, int car, const float oct[8],
                       uint32_t out[6], ro3_vec *pts)
{
    const objscene_type *ta, *tb;
    const ro3_model *a, *b;
    int32_t *rest, *bv, *live, base[7], cur[7];
    uint32_t w[6];
    size_t   i, n;
    int      k, rc = -1;

    if (car < 0 || 0x50 + 0x14 * car + 0x0A >= OBJ_TYPES) return -1;
    ta = &os->type[0x50 + 0x14 * car];
    tb = &os->type[0x50 + 0x14 * car + 0x0A];
    if (!ta->have || !tb->have) return -1;
    a = &ta->m;
    b = &tb->m;
    n = a->npoints;
    /* The original pairs the two by index and trusts them to match. */
    if (n == 0 || b->npoints < n || b->npolys < a->npolys) return -1;
    for (i = 0; i < a->npolys; i++) {
        const ro3_poly *pa = &a->polys[i];
        int32_t j;
        if (ro3_sel_a(pa) != 0x1C) continue;
        if (b->polys[i].corners < pa->corners) return -1;
        for (j = 0; j < pa->corners; j++) {
            if ((size_t)a->corner_vert[pa->first_corner + j] >= n
                || (size_t)b->corner_vert[b->polys[i].first_corner + j] >= n)
                return -1;
        }
    }

    rest = malloc(3 * n * sizeof *rest);
    bv   = malloc(3 * n * sizeof *bv);
    live = malloc(3 * n * sizeof *live);
    if (!rest || !bv || !live) goto done;
    for (i = 0; i < n; i++) {
        rest[3 * i + 0] = crush_fix(a->points[i].x);
        rest[3 * i + 1] = crush_fix(a->points[i].y);
        rest[3 * i + 2] = crush_fix(a->points[i].z);
        bv[3 * i + 0]   = crush_fix(b->points[i].x);
        bv[3 * i + 1]   = crush_fix(b->points[i].y);
        bv[3 * i + 2]   = crush_fix(b->points[i].z);
    }

    crush_sums(a, b, rest, bv, base);             /* FUN_004C0A02         */

    for (i = 0; i < n; i++) {                     /* 0x004C068D..0x004C06B6 */
        const int32_t *r = &rest[3 * i], *q = &bv[3 * i];
        int32_t *o = &live[3 * i];
        int c;
        if (r[0] == q[0] && r[1] == q[1] && r[2] == q[2]) {
            o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
            continue;
        }
        {
            int    sx = (uint32_t)q[0] >> 31, sy = (uint32_t)q[1] >> 31,
                   sz = (uint32_t)q[2] >> 31;
            double f  = (double)oct[sx << 2 | sy << 1 | sz];
            for (c = 0; c < 3; c++)
                o[c] = (int32_t)lrint((double)((int64_t)q[c] - r[c]) * f
                                      + (double)r[c]);
        }
    }

    crush_sums(a, b, live, bv, cur);              /* FUN_004C0B01         */

    for (k = 1; k <= 6; k++) {                    /* 0x004C0CB6..         */
        if (base[k] == 0) {
            w[k - 1] = 0x80000000u;               /* 0/0, integer indefinite */
            continue;
        }
        w[k - 1] = (uint32_t)(int32_t)lrint(100.0 - (double)cur[k]
                                            / (double)base[k] * 100.0);
    }
    for (k = 0; k < 6; k++) {                     /* 0x00454C6C..0x00454CB6 */
        int j = k, m;
        if (w[k] != 0x80000000u) continue;
        for (m = 0; m < 6; m++) {
            if (w[j] != 0x80000000u) break;
            if (++j == 6) j = 0;
        }
        w[k] = w[j];
    }
    memcpy(out, w, sizeof w);
    /* The dent as drawn: model A's vertices are the live buffer, 16.16 back
     * to the parser's units (exact, as the rest copy was). */
    if (pts) {
        for (i = 0; i < n; i++) {
            pts[i].x = (double)live[3 * i + 0] / RO3_FIXED_ONE;
            pts[i].y = (double)live[3 * i + 1] / RO3_FIXED_ONE;
            pts[i].z = (double)live[3 * i + 2] / RO3_FIXED_ONE;
        }
    }
    rc = 0;
done:
    free(rest);
    free(bv);
    free(live);
    return rc;
}

int objscene_car_windows(const objscene *os, int car, const float oct[8],
                         uint32_t out[6])
{
    return objscene_car_crush(os, car, oct, out, NULL);
}

int objscene_load_types(objscene *os, const uint32_t *model, int ntypes,
                        const char *files_root)
{
    int t, n = 0;

    if (!os->info) {
        info_load(os, files_root);
    }
    for (t = 0; t < ntypes && t < OBJ_TYPES; t++) {
        if (model[t] != 0u) {
            n += obj_load_one(os, t, model[t], files_root);
        }
    }
    tex_union(os);
    os->tex_ids = os->n_tex_all;
    return n;
}

/* ===================================================================== *
 *  THE TEXTURES                                                         *
 * ===================================================================== */

unsigned objscene_texids(const objscene *os, uint16_t *out, unsigned max)
{
    unsigned i;

    for (i = 0u; i < os->n_tex_all && i < max; i++) {
        out[i] = os->tex_all[i];
    }
    return os->n_tex_all;
}

void objscene_set_texture(objscene *os, uint16_t id, plat_tex t)
{
    int tp;

    for (tp = 0; tp < OBJ_TYPES; tp++) {
        objscene_type *ty = &os->type[tp];
        unsigned       k;

        for (k = 0u; k < ty->ntexid; k++) {
            if (ty->texid[k] == id) {
                ty->tex[k] = t;
            }
            if ((uint16_t)(ty->texid[k] & ~7u) == id) {
                ty->tex_floor[k] = t;
            }
        }
    }
}

/* ===================================================================== *
 *  THE CAMERA                                                           *
 * ===================================================================== */

void objscene_camera(ral_obj_camera *cam, const stage_view *v)
{
    int   r, c;
    float yaw;

    memset(cam, 0, sizeof *cam);

    /* [0x0059EF94], the view ROTATION on its own: stage_view::m is the
     * COMPOSITION (0x0059F014 = view_trans x view_rot), so the rotation is
     * its linear part and the translation row is dropped. */
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            cam->view_rot.m[r * 4 + c] = v->m[r * 4 + c];
        }
    }
    cam->view_rot.m[15] = 1.0f;

    /* [0x0059EF14], the identity with row 3 = minus the camera position.
     * stage_view keeps eye x and z; eye y is only in m[13], which
     * stage_view_default sets to -(float)eye_y - unless the caller gives it
     * (the original's camera pitches, and then m[13] is not -eye_y). */
    cam->view_trans.m[0] = cam->view_trans.m[5] = cam->view_trans.m[10] = 1.0f;
    cam->view_trans.m[15] = 1.0f;
    cam->view_trans.m[12] = -(float)v->eye_x;
    cam->view_trans.m[13] = v->has_eye_y ? -(float)v->eye_y : v->m[13];
    cam->view_trans.m[14] = -(float)v->eye_z;

    /* [0x0059EF54] = FUN_0041FA3B(view_rot): the adjugate followed by a
     * Gram-Schmidt re-orthonormalisation, which for a rotation is the
     * TRANSPOSE. - and the point of that
     * section is that there is no divide by a determinant in it. */
    cam->view_rot_inv.m[15] = 1.0f;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            cam->view_rot_inv.m[r * 4 + c] = cam->view_rot.m[c * 4 + r];
        }
    }

    /* [0x005BA608], a THIRTY-TWO bit binary angle where the record's are 16.
     * FUN_00425D1C divides by 65536 twice and then by 1/2pi.  Mode 5 is the
     * only arm that reads it.
     *
     * AND THE SIGN IS A TRANSCRIPTION, not a choice.  The view builder negates
     * this global on its way into the rotation (0x00436EA6), so the game holds
     * view_rot = Ry(-cam_yaw); mat_rot_y writes m[2] = -sin, and
     * stage_view_default writes m[2] = +sin(yaw), so the stage's view rotation
     * is Ry(-yaw) and cam_yaw is therefore the stage's own yaw, not its
     * negation.  Mode 5's whole point is that the two cancel - obj_transform
     * _probe's `mode-5` gate is exactly this identity - so getting it backwards
     * doubles the camera yaw on the seven OBJSINFO records that use it. */
    yaw = atan2f(v->m[2], v->m[0]);
    cam->cam_yaw = (unsigned)(int64_t)((double)yaw
                                       * (4294967296.0 / 6.28318530717958648));
    cam->cam_extra_yaw = 0u;          /* [0x005BA614], zeroed at 0x004371AF */
}


/* ===================================================================== *
 *  THE HEADING TEST - see objscene.h                                    *
 * ===================================================================== */

/* The model's longer horizontal axis, in its OWN frame: the principal
 * eigenvector of the 2x2 covariance of its XZ point cloud.  Returns 0 - "this
 * model has no heading" - unless the two standard deviations differ by at
 * least OBJ_THIN_RATIO, which is what excludes a tree card, a barrel and a
 * telegraph pole. */
static int obj_long_axis(const ro3_model *m, double *ax, double *az)
{
    double mx = 0.0, mz = 0.0, cxx = 0.0, cxz = 0.0, czz = 0.0;
    double tr, det, l1, l2, vx, vz, n;
    size_t i;

    if (m->npoints < 3) {
        return 0;
    }
    for (i = 0; i < m->npoints; i++) { mx += m->points[i].x; mz += m->points[i].z; }
    mx /= (double)m->npoints;
    mz /= (double)m->npoints;
    for (i = 0; i < m->npoints; i++) {
        double dx = m->points[i].x - mx, dz = m->points[i].z - mz;
        cxx += dx * dx; cxz += dx * dz; czz += dz * dz;
    }
    tr  = cxx + czz;
    det = cxx * czz - cxz * cxz;
    n   = tr * tr * 0.25 - det;
    if (n < 0.0) { n = 0.0; }
    n  = sqrt(n);
    l1 = tr * 0.5 + n;
    l2 = tr * 0.5 - n;
    if (l1 <= 0.0 || l2 < 0.0) {
        return 0;
    }
    if (l2 * (OBJ_THIN_RATIO * OBJ_THIN_RATIO) > l1) {
        return 0;                       /* too round to have a heading */
    }
    /* the eigenvector for l1 */
    vx = cxz;
    vz = l1 - cxx;
    if (fabs(vx) + fabs(vz) < 1e-12) { vx = 1.0; vz = 0.0; }
    n = sqrt(vx * vx + vz * vz);
    *ax = vx / n;
    *az = vz / n;
    return 1;
}

typedef struct { int type; double x, z, fx, fz; } obj_inst;

int objscene_heading(objscene *os, const cdat_payload *cd, int brk,
                     unsigned long *tested, double *conc)
{
    double    axis_x[OBJ_TYPES], axis_z[OBJ_TYPES];
    double    sumc[OBJ_TYPES], sums[OBJ_TYPES];
    unsigned long cnt[OBJ_TYPES];
    int       thin[OBJ_TYPES];
    obj_inst *v;
    int32_t   i;
    size_t    n = 0;
    double    runmax = OBJ_RUN_MAX * 65536.0;
    int       t;

    *tested = 0;
    *conc   = 0.0;
    for (t = 0; t < OBJ_TYPES; t++) { sumc[t] = sums[t] = 0.0; cnt[t] = 0; }

    for (t = 0; t < OBJ_TYPES; t++) {
        thin[t] = 0;
        axis_x[t] = axis_z[t] = 0.0;
        if (!os->type[t].have || os->type[t].gated) { continue; }
        /* Only the general Euler arm: a billboard's matrix does not read the
         * record's yaw at all, so its heading says nothing about the sign. */
        if (os->type[t].mode != RAL_OBJ_MODE_EULER)                { continue; }
        thin[t] = obj_long_axis(&os->type[t].m, &axis_x[t], &axis_z[t]);
        if (thin[t] && getenv("RALLY_OBJ_HEADING_VERBOSE")) {
            printf("      heading: type %d model %u axis %.3f %.3f\n",
                   t, os->type[t].model, axis_x[t], axis_z[t]);
        }
    }

    v = (obj_inst *)malloc((size_t)cd->npols * sizeof *v);
    if (!v) { return 0; }

    for (i = 0; i < cd->npols; i++) {
        const uint8_t *pol = cdat_pol(cd, i);
        ral_obj_place  p;
        ral_mat4       model;
        int32_t        anchor, apt[3], dx, dy, dz;
        unsigned       ty;
        float          lx, lz;

        if (!pol || pol[CDAT_POL_CLASS] != CDAT_POLY_ROUTE) { continue; }
        ty = pol[CDAT_OBJ_TYPE];
        if (ty >= (unsigned)OBJ_TYPES || !thin[ty])         { continue; }
        memcpy(&anchor, pol + CDAT_OBJ_ANCHOR, 4);
        if (anchor < 0 || anchor >= cd->npnts)              { continue; }
        cdat_pnt(cd, anchor, apt);
        memcpy(&dx, pol + 0x30, 4);
        memcpy(&dy, pol + 0x34, 4);
        memcpy(&dz, pol + 0x38, 4);

        memset(&p, 0, sizeof p);
        p.x = apt[0] + dx; p.y = apt[1] + dy; p.z = apt[2] + dz;
        p.yaw       = (unsigned)pol[0x08] << 8;
        p.tilt      = (unsigned)pol[0x0c] << 8;
        p.tilt_axis = (unsigned)pol[0x10] << 8;
        if (pol[0x11]) {
            p.yaw2       = (unsigned)pol[0x09] << 8;
            p.tilt2_axis = (unsigned)pol[0x12] << 8;
            p.tilt2      = (unsigned)pol[0x13] << 8;
        }
        if (brk == OBJ_BREAK_YAW_SIGN) {
            p.yaw  = (unsigned)(-(int)p.yaw);
            p.yaw2 = (unsigned)(-(int)p.yaw2);
        }
        p.scale16 = (int)pol[0x18];
        ral_obj_transform(&model, NULL, &p, RAL_OBJ_MODE_EULER, NULL);

        /* the model's own long axis, through the placement's linear part */
        lx = (float)axis_x[ty];
        lz = (float)axis_z[ty];
        v[n].type = (int)ty;
        v[n].x    = (double)p.x;
        v[n].z    = (double)p.z;
        v[n].fx   = model.m[0] * lx + model.m[8] * lz;
        v[n].fz   = model.m[2] * lx + model.m[10] * lz;
        n++;
    }

    {
        size_t a, b;
        for (a = 0; a < n; a++) {
            double bestd = runmax * runmax;
            size_t bi = (size_t)-1;
            double dx, dz, len, flen, c, sn;

            for (b = 0; b < n; b++) {
                if (b == a || v[b].type != v[a].type) { continue; }
                dx = v[b].x - v[a].x;
                dz = v[b].z - v[a].z;
                len = dx * dx + dz * dz;
                if (len > 0.0 && len < bestd) { bestd = len; bi = b; }
            }
            if (bi == (size_t)-1) { continue; }
            dx = v[bi].x - v[a].x;
            dz = v[bi].z - v[a].z;
            len  = sqrt(dx * dx + dz * dz);
            flen = sqrt(v[a].fx * v[a].fx + v[a].fz * v[a].fz);
            if (len <= 0.0 || flen <= 0.0) { continue; }
            c = fabs((dx * v[a].fx + dz * v[a].fz) / (len * flen));
            /* The DOUBLED angle between the object's axis and the run
             * direction, accumulated as a unit vector.  Doubled because the
             * axis is defined mod 180 - a fence panel laid the other way up
             * is the same fence - and accumulated rather than thresholded
             * because the statistic is concentration, not alignment. */
            c  = (dx * v[a].fx + dz * v[a].fz) / (len * flen);   /* cos d  */
            sn = (dx * v[a].fz - dz * v[a].fx) / (len * flen);   /* sin d  */
            sumc[v[a].type] += c * c - sn * sn;                  /* cos 2d */
            sums[v[a].type] += 2.0 * c * sn;                     /* sin 2d */
            cnt[v[a].type]++;
            (*tested)++;
        }
    }
    free(v);

    {
        double wsum = 0.0, w = 0.0;
        for (t = 0; t < OBJ_TYPES; t++) {
            double r;
            if (cnt[t] < OBJ_HEAD_MIN_PAIRS) { continue; }
            r = sqrt(sumc[t] * sumc[t] + sums[t] * sums[t]) / (double)cnt[t];
            if (getenv("RALLY_OBJ_HEADING_VERBOSE")) {
                printf("      heading type %3d  %4lu pairs  R %.3f\n",
                       t, cnt[t], r);
            }
            wsum += r * (double)cnt[t];
            w    += (double)cnt[t];
        }
        if (w <= 0.0) { return 0; }
        *conc = wsum / w;
    }
    return 1;
}

/* ===================================================================== *
 *  THE EMITTER                                                          *
 * ===================================================================== */

void objscene_stats_init(objscene_stats *st)
{
    memset(st, 0, sizeof *st);
    st->min_x = st->min_y = st->min_z = 0x7fffffff;
    st->max_x = st->max_y = st->max_z = -0x7fffffff - 1;
    st->min_sx = st->min_sy =  1e30f;
    st->max_sx = st->max_sy = -1e30f;
}

typedef struct {
    float    vx, vy, vz, sx, sy;
    float    tu, tv;
    int      behind;
    uint32_t col;           /* the corner's colour, objscene_light's      */
    float    nx, ny, nz;    /* its view-space normal (light mode 3 only)  */
    float    eu, ev;        /* the reflection pass's uv, FUN_0044F92E's   */
} objvtx;

#define OBJ_MAXV 65535
static plat_vertex obj_vbuf[OBJ_MAXV];
static plat_vertex obj_vbuf1[OBJ_MAXV];     /* the reflection layer        */
/* single-pass, obj_vbuf[i]'s reflection (plat_gfx_draw_env) */
static float       obj_veu[OBJ_MAXV], obj_vev[OBJ_MAXV];
static uint32_t    obj_vecol[OBJ_MAXV];
static plat_vertex_env obj_venv[OBJ_MAXV];
static int         op_two_pass;           /* objscene_set_env_two_pass */
static int         op_race;               /* objscene_set_race         */
/* The record being drawn and its light (FUN_0044FB03), NULL outside a race:
 * obj_draw_polys then lights each corner through the level's shade ramp. */
static const uint8_t *op_rec;
static int         op_rec_flat;
static float       op_rec_L[3];
static int32_t     op_rec_lo, op_rec_hi, op_rec_base, op_rec_scale;
static uint32_t    op_rec_local;          /* [0x005E7EF8], FUN_0044F3DC     */
static int32_t     op_sun[4];             /* objscene_set_level_sun    */

void objscene_set_level_sun(int32_t az, int32_t el, int32_t base, int32_t scale)
{
    op_sun[0] = az; op_sun[1] = el; op_sun[2] = base; op_sun[3] = scale;
}

/* Light modes 1 and 2 for one corner of a race record (FUN_0044EEB9):
 *   mode 1 (OBJSINFO +0x1F): ramp0[base]                       0x0044EFE8
 *   mode 2: i = base + (scale ? FISTP(dot(n_view, L) * scale) : 0)
 *           clamped below to lo (JNS), then to hi (JS), ramp0[i] 0x0044F193
 * each plus [0x005E0954] / [0x005E7EF8], the local lights - [0x005E0954] is
 * zeroed by FUN_0044F112 for mode 2; [0x005E7EF8] is op_rec_local below
 * (it was left out as "0 with one car", and is the cars' headlight).
 * ramp0 is the stage's shade ramp, the view's shade_lo -> shade_hi. */
static uint32_t obj_satadd(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    int k;

    for (k = 0; k < 32; k += 8) {
        uint32_t x = ((a >> k) & 0xffu) + ((b >> k) & 0xffu);
        r |= (x > 0xffu ? 0xffu : x) << k;
    }
    return r;
}

static uint32_t obj_rec_light(const stage_view *v, const float vn[3])
{
    int32_t i = op_rec_base;
    uint32_t c = 0xff000000u;
    int k;

    if (!op_rec_flat) {
        if (op_rec_scale != 0) {
            i += (int32_t)lrintf((vn[0] * op_rec_L[0] + vn[1] * op_rec_L[1]
                                  + vn[2] * op_rec_L[2]) * (float)op_rec_scale);
        }
        if (i < op_rec_lo)  { i = op_rec_lo; }
        if (i >= op_rec_hi) { i = op_rec_hi; }
    }
    i &= 0xff;
    for (k = 0; k < 3; k++) {
        int lo = v->shade_lo[k], d = v->shade_hi[k] - lo;
        c |= (uint32_t)(lo + (d * i) / 255) << (16 - 8 * k);
    }
    return c;
}

void objscene_set_env_two_pass(int on)
{
    op_two_pass = on != 0;
}

/* The batch out: one plat_gfx_draw_env when any vertex in it reflects (and
 * the single pass is on), else the plain draw.  Returns the draws made. */
static unsigned obj_emit(unsigned nv, unsigned nenv, plat_tex env)
{
    /* nenv > 0 means the batch is in obj_venv already (the fan loop
     * moved it there at its first reflecting triangle), else it is in obj_vbuf. */
    if (!nenv) {
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, obj_vbuf, nv);
        return 1;
    }
    plat_gfx_draw_env(PLAT_PRIM_TRIANGLES, obj_venv, nv, env);
    return 1;
}
static objvtx      obj_tv[RO3_MAX_CORNERS];

/* model_render_probe.c's diagnostic shade, unchanged, and for its reason: the
 * colour must be SIGN-SENSITIVE or a face and the face behind it come out the
 * same colour and no cull-mode comparison can see anything. */
static uint32_t obj_shade(ro3_vec n)
{
    static const double l[3] = { 0.3714, 0.5571, -0.7428 };
    double d = n.x * l[0] + n.y * l[1] + n.z * l[2];
    int    k = (int)(0.18 * 255.0 + (d * 0.5 + 0.5) * 0.82 * 255.0);

    if (k < 0)   { k = 0; }
    if (k > 255) { k = 255; }
    return 0xFF000000u | ((uint32_t)k << 16) | ((uint32_t)k << 8) | (uint32_t)k;
}

/* Light mode 3 for one corner (objscene.h's objscene_light): 0x0044F035,
 * FUN_0044F238, then 0x0044A790's modulate by `g`.  `lit` is 0x0044EE2F's
 * gate: a corner it turns away is white before the modulate. */
static uint32_t obj_light3(const objscene_light *L, const float n[3],
                           int lit, uint32_t g)
{
    int32_t  ch[3], i = 0, k, a;
    uint32_t c, r = 0;

    c = L->base & 0x00FFFFFFu;
    if (lit) {
        if (L->diffuse != 0) {
            /* FUN_0044CC5C, as objlit.c's lit_corner: the dot in the FPU's
             * order, times (float)diffuse, FISTP */
            obj_ld d = (obj_ld)n[2] * L->light[2]
                     + ((obj_ld)n[1] * L->light[1]
                      + (obj_ld)n[0] * L->light[0]);
            i = (int32_t)OBJ_RINT(d * (obj_ld)(float)L->diffuse);
        }
        if (i < -255) i = -255;                         /* 0x0044F270 */
        if (i > 255)  i = 255;                          /* 0x0044F27C */
        a = i < 0 ? -i : i;
        ch[0] = (int32_t)((c >> 16) & 0xFFu);           /* [0x005E0956] */
        ch[1] = (int32_t)((c >> 8) & 0xFFu);            /* [0x005E0955] */
        ch[2] = (int32_t)(c & 0xFFu);                   /* [0x005E0954] */
        for (k = 0; k < 3; k++) {
            int32_t t = (i >= 0 ? L->lit[k] : L->shadow[k]) - ch[k];
            int32_t v = (int32_t)(((int64_t)a * t) >> 8) + ch[k];  /* SAR */
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            ch[k] = v;
        }
        c = ((uint32_t)ch[0] << 16) | ((uint32_t)ch[1] << 8) | (uint32_t)ch[2];
    } else {
        c = 0x00FFFFFFu;
    }
    for (k = 0; k < 24; k += 8)                         /* 0x0044A7C0 */
        r |= ((((c >> k) & 0xFFu) * ((g >> k) & 0xFFu)) >> 8) << k;
    return r | 0xFF000000u;
}

static float obj_area2(const objvtx *a, const objvtx *b, const objvtx *c)
{
    return (b->sx - a->sx) * (c->sy - a->sy)
         - (c->sx - a->sx) * (b->sy - a->sy);
}

/* FUN_0044AFE9's transform and FUN_0044B045's projection, through the OBJECT
 * matrix instead of [0x0059F014].  The .ro3 parser hands back world units and
 * the matrix's translation row is in RAW 16.16 (FUN_00425DBE FILDs the anchor
 * and does not divide), so the model point is scaled back up here.  That is
 * the one unit conversion in this file and OBJ_BREAK_MODEL_UNITS is it. */
static void obj_project(const stage_view *v, const ral_mat4 *m, ro3_vec p,
                        int brk, objvtx *o)
{
    float s = (brk == OBJ_BREAK_MODEL_UNITS) ? 1.0f : (float)RO3_FIXED_ONE;
    float x = (float)p.x * s, y = (float)p.y * s, z = (float)p.z * s;
    float sc;

    o->vx = m->m[0] * x + m->m[4] * y + m->m[ 8] * z + m->m[12];
    o->vy = m->m[1] * x + m->m[5] * y + m->m[ 9] * z + m->m[13];
    o->vz = m->m[2] * x + m->m[6] * y + m->m[10] * z + m->m[14];

    o->sx = 0.0f;
    o->sy = 0.0f;
    o->behind = 1;
    if (!(o->vz < v->near_z)) {
        sc = v->focal / o->vz;
        o->sx =  o->vx * sc + v->centre_x;
        o->sy = -o->vy * sc + v->centre_y;      /* FCHS at 0x0044B06A */
        o->behind = 0;
    }
}

/* FUN_0044FEAD's ring.  The IDENTITY plus a wrap element - NOT the stage
 * path's 2,0,1,3, which is the single most expensive thing to get wrong here
 * because an object placed with the stage's ring passes every coverage
 * assertion in this project and is inside out. */
static const uint8_t obj_stage_ring[4] = { 2, 0, 1, 3 };

static int obj_ring(int i, int n, int brk)
{
    if (brk == OBJ_BREAK_STAGE_RING && n == 4) {
        return obj_stage_ring[i];
    }
    return i;
}

static void obj_draw_polys(objscene_type *ty, const ral_mat4 *mv,
                           const stage_view *v, int brk, objscene_stats *st,
                           const objscene_parts *parts,
                           const ral_mat4 *rinv);

/* The near clip's vertex and clipper (the backdrop's, below). */
typedef struct { float x, y, z, u, v; float c[3]; } objclipv;
static int bg_clip_near(const objclipv *in, int n, float near_z,
                        objclipv *out);

void objscene_draw_record(objscene *os, const cdat_payload *cd,
                          const uint8_t *pol, const stage_view *v,
                          const ral_obj_camera *cam, int brk,
                          objscene_stats *st)
{
    ral_obj_place  place;
    ral_mat4       model, mview;
    objscene_type *ty;
    unsigned       type;
    int32_t        anchor, apt[3];

    if (st) { st->records++; }

    /* `MOVZX EAX,byte [ESI+0x01]` / `CMP EAX,0x64` / `JA` at 0x0044FB0D. */
    type = pol[CDAT_OBJ_TYPE];
    if (type > (unsigned)OBJ_TYPE_MAX) {
        if (st) { st->dropped_type++; }
        return;
    }
    ty = &os->type[type];

    /* FUN_0044E389's arm for {0x38, 0x3A..0x3F}: `CMP [0x005BBFEC],0 / JE ->
     * STC`, and [0x005BBFEC] has one reference image-wide and no writer.  The
     * retail build draws none of these - 81.35% of the layer. */
    if (ty->gated && brk != OBJ_BREAK_NO_GATE) {
        if (st) { st->gated++; }
        return;
    }

    /* FUN_0044E389's per-record arm, 0x0044E3E0..0x0044E3F9:
     *
     *     CMP [0x005D41B0],0 / JE -> skip the test
     *     EAX = [0x005E7EE8]            ; (pols[+0x25] >> 1) & 3, 0x0044FC0D
     *     if EAX != 0 and EAX-1 != [0x006CE08C]: STC
     *
     * [0x005D41B0] is set to 1 unconditionally by loadlevel_init at
     * 0x00442624, so in a race the test is LIVE (section 4.2 said otherwise; corrected).  Without it the port drew the
     * other direction's marker flags too - facing backwards, and with no
     * collision, because FUN_0045BCE8 (sim/objects.c) applies this test. */
    {
        unsigned sel = ((unsigned)pol[CDAT_OBJ_SELECTOR] >> 1) & 3u;
        if (sel != 0 && (int32_t)(sel - 1) != os->route_variant) {
            if (st) { st->other_direction++; }
            return;
        }
    }

    /* `CMP [EBX*4 + 0x005E5E00],0 / JNE +1 / RET` at 0x0044FDF2 - the type's
     * POLYGON COUNT.  THIS TEST COMES BEFORE ANY OBJSINFO READ in the
     * port even though it comes after two of them in the original; that is
     *'s trap and the reason this line is
     * where it is rather than twenty lines further down. */
    if (!ty->have || ty->m.npolys == 0) {
        if (st) { st->no_model++; }
        return;
    }

    /* `EAX = [ESI+0x04]`, bound-checked against [0x00581A68] at 0x0044FB2F. */
    memcpy(&anchor, pol + CDAT_OBJ_ANCHOR, 4);
    if (anchor < 0 || anchor >= cd->npnts) {
        if (st) { st->dropped_anchor++; }
        return;
    }
    cdat_pnt(cd, anchor, apt);

    memset(&place, 0, sizeof place);

    /* 0x0044FB72: pnts[anchor] + pols[+0x30,+0x34,+0x38], as raw 16.16.  The
     * offset is non-zero in 60 of 237,428 records. */
    {
        int32_t dx, dy, dz;
        memcpy(&dx, pol + 0x30, 4);
        memcpy(&dy, pol + 0x34, 4);
        memcpy(&dz, pol + 0x38, 4);
        if (brk == OBJ_BREAK_NO_ANCHOR) {
            place.x = dx; place.y = dy; place.z = dz;
        } else {
            place.x = apt[0] + dx;
            place.y = apt[1] + dy;
            place.z = apt[2] + dz;
        }
    }

    /* The angles, each `<< 8` into a 16-bit binary angle (0x0044FB8C and
     * five more).  pols[+0x0C] is the tilt MAGNITUDE and pols[+0x10] the
     * HEADING of its axis - - and the SECOND triple orders the same two
     * fields the other way round. */
    place.yaw       = (unsigned)pol[0x08] << 8;
    place.tilt      = (unsigned)pol[0x0c] << 8;
    place.tilt_axis = (unsigned)pol[0x10] << 8;
    if (pol[0x11]) {                       /* `CMP byte [ESI+0x11],0` 0x0044FBD3 */
        place.yaw2       = (unsigned)pol[0x09] << 8;
        place.tilt2_axis = (unsigned)pol[0x12] << 8;
        place.tilt2      = (unsigned)pol[0x13] << 8;
    }
    if (brk == OBJ_BREAK_YAW_SIGN) {
        /* 0x00425FFF's `NEG EDX`, undone.  objscene_heading() is what sees it. */
        place.yaw  = (unsigned)(-(int)place.yaw);
        place.yaw2 = (unsigned)(-(int)place.yaw2);
    }
    if (brk == OBJ_BREAK_YAW_AS_TILT) {
        /* The yaw read as a tilt about the axis heading 0, i.e. about X - a
         * reader who took FUN_00425BF1 for FUN_00425C3A.  It is orthonormal,
         * unit-determinant and lays every object on its side. */
        place.tilt      = place.yaw;
        place.tilt_axis = 0u;
        place.yaw       = 0u;
    }

    /* `MOVZX EAX,byte [ESI+0x18]` at 0x0044FCF2, and the matrix is multiplied
     * by it over 16 at 0x00426136. */
    place.scale16 = (brk == OBJ_BREAK_NO_SCALE) ? 16 : (int)pol[0x18];

    ral_obj_transform(&model, &mview, &place, ty->mode, cam);

    if (st) {
        st->placed++;
        if (ty->gated)     { st->placed_gated++;  }
        if (pol[0x18] != 16) { st->placed_scaled++; }
        if (place.x < st->min_x) { st->min_x = place.x; }
        if (place.x > st->max_x) { st->max_x = place.x; }
        if (place.y < st->min_y) { st->min_y = place.y; }
        if (place.y > st->max_y) { st->max_y = place.y; }
        if (place.z < st->min_z) { st->min_z = place.z; }
        if (place.z > st->max_z) { st->max_z = place.z; }

        /* THE SCALE, on every placed record: |row 0| == pols[+0x18]/16. */
        {
            float s   = (float)pol[0x18] / 16.0f;
            float len = sqrtf(model.m[0] * model.m[0]
                            + model.m[1] * model.m[1]
                            + model.m[2] * model.m[2]);
            float bad = fabsf(len - s);
            st->scale_tested++;
            if (bad > 1e-4f * (s > 1.0f ? s : 1.0f)) { st->scale_bad++; }
            if (bad > st->worst_scale) { st->worst_scale = bad; }
        }

        /* UPRIGHT.  With no tilt and no second triple the object's own matrix
         * is a scaled yaw about Y, so row 1 must be exactly (0, s, 0).  It
         * holds for 95.2% of the drawable records and it is the assertion
         * that separates "rotated about Y" from "rotated about anything
         * else" without needing a picture. */
        /* THE CONDITION IS READ FROM THE RECORD AND SO IS THE SCALE, and
         * both for the same reason: a gate whose inputs come out of the
         * struct a break has just rewritten is comparing the port against
         * itself.  Conditioning on `place.tilt` let OBJ_BREAK_YAW_AS_TILT -
         * which SETS a tilt - walk straight past this assertion by making it
         * decline, which is in one line. */
        if (ty->mode == RAL_OBJ_MODE_EULER && pol[0x0c] == 0
            && pol[0x11] == 0) {
            float s   = (float)pol[0x18] / 16.0f;
            float bad = fabsf(model.m[4]) + fabsf(model.m[6])
                      + fabsf(model.m[5] - s);
            st->upright_tested++;
            if (bad > 1e-4f * (s > 1.0f ? s : 1.0f)) { st->upright_bad++; }
            if (bad > st->worst_upright) { st->worst_upright = bad; }
        }
    }

    /* FUN_0044FB03's light set-up for a record, 0x0044FCF2..0x0044FDB1, in a
     * race only (0x0044EEC4's gate; the probes keep the diagnostic shade). */
    op_rec = NULL;
    if (op_race) {
        op_rec = pol;
        op_rec_flat = ty->flat_lit;
        op_rec_lo = pol[0x14]; op_rec_hi = pol[0x15];
        op_rec_base = pol[0x16]; op_rec_scale = pol[0x17];
        /* FUN_0044F3DC (0x0044F41A / 0x0044F531): [0x005E7EF8] = the cars'
         * headlight at the record's world position (FUN_0044F3C7 ->
         * FUN_0044D242 -> FUN_0044D2A1, the terrain's arithmetic) saturating-
         * added to the record's +0x2C dword ([0x005E7EE4]), once per record. */
        {
            int32_t wp[3];
            uint32_t k2c;
            wp[0] = place.x; wp[1] = place.y; wp[2] = place.z;
            memcpy(&k2c, pol + 0x2c, 4);
            op_rec_local = obj_satadd(stage_point_light(v, wp), k2c);
        }
        if (pol[0x24] != 0) {
            /* 0x0044FD31: the level's own sun and shade instead - LOCALDAT
             * +0x684 (negated) / +0x688 and +0x630 / +0x634, range 0..0xFF */
            op_rec_lo = 0; op_rec_hi = 0xff;
            op_rec_base = op_sun[2]; op_rec_scale = op_sun[3];
            objlit_light(op_rec_L, -op_sun[0], op_sun[1], &cam->view_rot);
        } else {
            objlit_light(op_rec_L, pol[0x22], pol[0x23], &cam->view_rot);
        }
    }
    obj_draw_polys(ty, &mview, v, brk, st, NULL, NULL);
    op_rec = NULL;
}

void objscene_draw_placed(objscene *os, int type, const float rot[16],
                          const int32_t pos[3], const stage_view *v,
                          const ral_obj_camera *cam, objscene_stats *st)
{
    objscene_draw_placed_parts(os, type, rot, pos, v, cam, st, NULL);
}

/* FUN_004530F2's draw of ONE impact-flash slot - the glow model
 * ([0x005D4238]: model 122, else 121, the floodlights' glows) placed at the
 * anchor with scale16 = k << 3 ([0x005E7EC8]), colour 0xFFFFFF, context 2,
 * through the same 0x00425FAA / 0x0044FDE0 pair a placed record takes.  The
 * model is a billboard (OBJSINFO +0x20 = 2, 0x00425FF3 -> 0x0042627E), so no
 * angle is read.  The type is found among the level's own (the floodlights'
 * glows are placed on the stages that have them); returns 0 when the level
 * has neither model loaded - the original loads it at level load regardless
 * (0x00442CFA), which the port does not yet. */
int objscene_draw_glow(objscene *os, const int32_t pos[3], int scale16,
                       const stage_view *v, const ral_obj_camera *cam,
                       objscene_stats *st)
{
    ral_obj_place  place;
    ral_mat4       model, mview;
    objscene_type *ty = NULL;
    int            t, want;

    for (want = 122; want >= 121 && !ty; want--) {
        for (t = 0; t < OBJ_TYPES; t++) {
            if (os->type[t].have && os->type[t].model == (uint32_t)want
                && os->type[t].m.npolys != 0) {
                ty = &os->type[t];
                break;
            }
        }
    }
    if (!ty) {
        return 0;
    }
    memset(&place, 0, sizeof place);
    place.x = pos[0]; place.y = pos[1]; place.z = pos[2];
    place.scale16 = scale16;
    ral_obj_transform(&model, &mview, &place, ty->mode, cam);
    op_rec = NULL;
    obj_draw_polys(ty, &mview, v, OBJ_BREAK_NONE, st, NULL, NULL);
    return 1;
}

/* 0x0044E5D2..0x0044E62E, the test's own order: the mask, then the lit
 * part, then the outside/inside pairs.  The mask is skipped when the debug
 * overlay is on page 5 or a camera blend runs (0x0044E5A4, 0x0044E5C9);
 * the caller expresses that as an all-ones mask, which 0x0045F6D3 also
 * writes during a blend.  Page 5's own per-part table is not ported. */
int objscene_part_drawn(const objscene_parts *g, unsigned part)
{
    int32_t n = (int32_t)(part & 0x1Fu);

    if (!g) {
        return 1;
    }
    /* 0x0044E597: mask 1 first - bodywork that has come off */
    if (!((g->mask1 >> n) & 1u)) {
        return 0;
    }
    if (!((g->mask >> n) & 1u)) {
        return 0;
    }
    if (n == g->lit_part && !g->lit_on) {
        return 0;
    }
    if (n == g->out_part[0] || n == g->out_part[1]) {
        return g->inside != 1;
    }
    if (n == g->in_part[0] || n == g->in_part[1]) {
        return g->inside != 0;
    }
    return 1;
}

void objscene_draw_placed_parts(objscene *os, int type, const float rot[16],
                                const int32_t pos[3], const stage_view *v,
                                const ral_obj_camera *cam, objscene_stats *st,
                                const objscene_parts *parts)
{
    objscene_draw_placed_pre(os, type, NULL, rot, pos, v, cam, st, parts);
}

void objscene_draw_placed_pre(objscene *os, int type, const ral_mat4 *pre,
                              const float rot[16], const int32_t pos[3],
                              const stage_view *v, const ral_obj_camera *cam,
                              objscene_stats *st, const objscene_parts *parts)
{
    objscene_type *ty;
    ral_mat4       cur, at, atv, placed, mview;
    int            r, c;

    if (type < 0 || type >= OBJ_TYPES) {
        return;
    }
    ty = &os->type[type];
    if (!ty->have || ty->m.npolys == 0) {
        if (st) { st->no_model++; }
        return;
    }
    if (st) { st->placed++; }

    /* obj_tail's shape (0x00426171..0x004261B5), with the rotation handed in
     * instead of built from a record's angles: the rotation's three rows,
     * placed at `pos` (FILD, no divide - raw 16.16), translated by the
     * camera's -position, and then into view space. */
    memset(&cur, 0, sizeof cur);
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            cur.m[r * 4 + c] = rot[r * 4 + c];
        }
    }
    cur.m[15] = 1.0f;
    if (pre) {
        ral_mat4 pc;
        ral_mat4_mul(&pc, pre, &cur);       /* 0x0045F8BB: wheel, then car */
        cur = pc;
    }
    ral_obj_translate_i(&at, pos[0], pos[1], pos[2]);
    ral_mat4_copy_add_t(&atv, &at, &cam->view_trans);
    ral_mat4_copy_add_t(&placed, &cur, &atv);
    ral_mat4_mul(&mview, &placed, &cam->view_rot);

    obj_draw_polys(ty, &mview, v, OBJ_BREAK_NONE, st, parts,
                   &cam->view_rot_inv);
}

/* The polygon loop of objscene_draw_record, lifted out unchanged so
 * that objscene_draw_placed - the car - goes through the same fan, texture
 * runs, projection and fog as every placed object.  `mv` is the object-to-
 * view matrix, [0x0059F054]. */
/* plat_gl.c's state snapshot, deliberately not in plat.h (rally.c declares
 * it the same way). */
typedef struct {
    uint32_t v[PLAT_RS__COUNT];
    int      valid[PLAT_RS__COUNT];
    plat_tex tex;
} plat_gfx_saved;
void plat_gfx_state_save(plat_gfx_saved *s);
void plat_gfx_state_restore(const plat_gfx_saved *s);

/* The reflection layer's draw (0x0044A940..0x0044A99A, as objlit.c's
 * lit_flush): ZWRITE off, SRCALPHA / ONE, unfogged vertices, then the state
 * the car draw runs under put back. */
static void obj_flush_env(plat_tex t, unsigned n)
{
    plat_gfx_saved saved;

    if (!n) {
        return;
    }
    plat_gfx_state_save(&saved);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_ONE);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_tex_bind(t);
    plat_gfx_draw(PLAT_PRIM_TRIANGLES, obj_vbuf1, n);
    plat_gfx_state_restore(&saved);
}

static unsigned long long op_us[4];     /* objscene_prof_take */
static unsigned long      op_draws;
static int                op_no_reflect; /* objscene_set_no_reflect */
static int                op_timing;     /* objscene_set_timing, */

/* The phase timers' clock: a read only when asked for, else 0, so that every
 * `op_us[k] += OBJ_NOW() - pt0` adds nothing and no clock runs per polygon. */
#define OBJ_NOW() (op_timing ? plat_time_us() : (uint64_t)0)

void objscene_set_timing(int on)
{
    op_timing = on != 0;
}

int objscene_timing(void)
{
    return op_timing;
}

void objscene_set_no_reflect(int on)
{
    op_no_reflect = on != 0;
}

/* [0x005D41B0], 1 from the race start (0x00442624).  FUN_0044EB7C tests it
 * before the OBJSINFO +0x20 == 2 arm below; the probes leave it 0. */
void objscene_set_race(int on)
{
    op_race = on != 0;
}

void objscene_set_atlas(objscene *os, int type, plat_tex atlas,
                        const float u0[], const float v0[], float su, float sv,
                        const int ok[])
{
    objscene_type *ty;
    unsigned k;
    if (type < 0 || type >= OBJ_TYPES) return;
    ty = &os->type[type];
    ty->atlas = atlas;
    ty->atlas_su = su;
    ty->atlas_sv = sv;
    for (k = 0u; k < ty->ntexid; k++) {
        ty->atlas_u0[k] = u0[k];
        ty->atlas_v0[k] = v0[k];
        ty->atlas_ok[k] = ok[k];
    }
}

void objscene_prof_take(unsigned long long us[4], unsigned long *draws)
{
    memcpy(us, op_us, sizeof op_us);
    *draws = op_draws;
    memset(op_us, 0, sizeof op_us);
    op_draws = 0;
}

/* the draw's stamp.  A pair part is good only while its stamp is the
 * current draw's.  (It wraps after 2^32 draws, about 25 days at 2,000 a
 * second; a part could then be taken for fresh only if no draw had touched it
 * in all that time, and the wrap skips 0, the never-filled stamp.) */
static uint32_t op_stamp;

/* The model's distinct (vertex, normal) pairs and the corner -> pair map.
 * Returns 0 (and leaves the model uncached) if there is nothing to index or
 * no memory; the draw then runs the per-corner path it always had. */
static int obj_pair_build(objscene_type *ty)
{
    size_t           nc = ty->m.ncorners, cap = 16, i;
    uint32_t         n = 0, *poc, *tab;
    struct obj_pair *pr;

    if (ty->pair_of_corner) {
        return 1;
    }
    if (nc == 0 || nc > 0x7fffffffu) {
        return 0;
    }
    while (cap < nc * 2u) {
        cap <<= 1;
    }
    poc = (uint32_t *)malloc(nc * sizeof *poc);
    pr  = (struct obj_pair *)calloc(nc, sizeof *pr);
    tab = (uint32_t *)malloc(cap * sizeof *tab);
    if (!poc || !pr || !tab) {
        free(poc); free(pr); free(tab);
        return 0;
    }
    memset(tab, 0xFF, cap * sizeof *tab);              /* 0xFFFFFFFF = empty */
    for (i = 0; i < nc; i++) {
        int32_t  vi = ty->m.corner_vert[i], ni = ty->m.corner_norm[i];
        uint32_t h  = (uint32_t)vi * 0x9E3779B1u ^ (uint32_t)ni * 0x85EBCA77u;

        h ^= h >> 15;
        h &= (uint32_t)(cap - 1u);
        while (tab[h] != 0xFFFFFFFFu
               && !(pr[tab[h]].vi == vi && pr[tab[h]].ni == ni)) {
            h = (h + 1u) & (uint32_t)(cap - 1u);
        }
        if (tab[h] == 0xFFFFFFFFu) {
            tab[h]  = n;
            pr[n].vi = vi;
            pr[n].ni = ni;
            n++;
        }
        poc[i] = tab[h];
    }
    free(tab);
    ty->pair_of_corner = poc;
    ty->pair           = pr;
    ty->npairs         = n;
    return 1;
}

static void obj_draw_polys(objscene_type *ty, const ral_mat4 *mv,
                           const stage_view *v, int brk, objscene_stats *st,
                           const objscene_parts *parts,
                           const ral_mat4 *rinv)
{
    uint64_t       pt0 = 0, pt1;                     
    /* a lit draw's per-pair cache, NULL for any other draw */
    struct obj_pair *pairs = NULL;
    const uint32_t  *poc   = NULL;
    uint32_t         stamp;
    const ral_mat4 mview = *mv;
    unsigned       nv = 0, nv1 = 0;
    unsigned       nenv = 0;                 /* single-pass reflectors */
    size_t         pi;
    float          dscale;
    /* The car's dented vertices (the crush pass), else the model's. */
    const ro3_vec *pts = (parts && parts->points) ? parts->points
                                                  : ty->m.points;
    /* The batch the fan is accumulating into, and the texture it is for.
     * `cur_have` distinguishes "nothing bound yet" from "bound to the null
     * texture", which are different states: the second is a real polygon
     * drawn untextured and must not be merged with a textured run. */
    plat_tex       cur_tex  = 0;
    plat_tex       cur_otex = 0;             /* the run's plate */
    int            cur_have = 0;
    /* FUN_0044EB7C 0x0044EBFC..0x0044EC0C: in a race, an OBJSINFO +0x20 of 2
     * ([0x005D420C], 0x0044F6E1) takes the arm at 0x0044ED4D - mode 4 with
     * [0x5D6F74] = 0, which FUN_00449AC7 turns into mode 9 (0x0044A23A): ONE
     * draw, TMB=4 FOG=1 SRC=5 DST=2 ABE=1 (0x0044A353..0x0044A399), between
     * 0x0044C46C / 0x0044C47C, ZWRITE 0 then 1.  ADDITIVE.  Models 121 and
     * 122, the floodlights' glows: drawn over-blended they were black discs.
     * FOGENABLE is left as the frame has it, as stage_layer2_block does. */
    int            glow = op_race && ty->mode == 2;
    plat_gfx_saved glow_saved;

    if (glow) {
        plat_gfx_state_save(&glow_saved);
        plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
        plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
        plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_ONE);
        plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    }

    dscale = stage_depth_scale(v->radius, v->detail);

    if (++op_stamp == 0u) {
        op_stamp = 1u;
    }
    stamp = op_stamp;
    if (parts && parts->light && obj_pair_build(ty)) {
        pairs = ty->pair;
        poc   = ty->pair_of_corner;
    }

    for (pi = 0; pi < ty->m.npolys; pi++) {
        const ro3_poly *p = &ty->m.polys[pi];
        ro3_vec  nrm, shade_n;
        int      have_n, front = 0;
        int32_t  i, n = p->corners;
        uint16_t texid;
        plat_tex ptex = 0;
        unsigned k;
        int      ak;                     /* the atlas slot, or -1 */
        plat_tex optex = 0;              /* the plate, before the atlas */
        int      env = 0;
        uint32_t ew = 0;
        int      b9 = (int)(((uint32_t)p->selector >> 8) & 0xFFu);

        /* 0x0044FEF1's `JS` - fewer than three corners draws nothing. */
        if (n < 3 || n > RO3_MAX_CORNERS) {
            continue;
        }
        /* the car's part gate (byte +7), objscene_part_drawn. */
        if (parts && !objscene_part_drawn(parts, (unsigned)(p->flags >> 24))) {
            continue;
        }
        /* FUN_0044E663 inside a level ([0x0057AF70] = 1).  +6 == 0x1C is a
         * window's crack decal (plate 1826): drawn only when +0x0B is 1..6
         * and that window's word car+0x6FC + k*4 is neither 0x80000000 nor
         * below 10, unsigned (0x0044E675..0x0044E6A5).  Any other non-zero
         * +6 only when +0x0B is that slot's state. */
        if (parts && parts->state) {
            int b6 = ro3_sel_a(p), bb = ro3_sel_b(p);
            int ok;
            if (b6 == 0x1C) {
                uint32_t w = (bb >= 1 && bb <= 6 && parts->glass)
                           ? parts->glass[bb - 1] : 0u;
                ok = bb >= 1 && bb <= 6 && w != 0x80000000u && w >= 10u;
            } else {
                ok = b6 == 0 || bb == parts->state[b6];
            }
            if (!ok) {
                if (st) { st->state_gated++; }
                continue;
            }
        }
        if (st) { st->polys++; }

        /* 0x0044ea31's `MOVZX EAX,word [EBP+4]`.  The lookup is always by the
         * TRUE id - the harvest is a transcription and holds no floored ids -
         * and the break switches which of the two plates bound against that
         * entry is used.  objscene.h's OBJ_BREAK_TEX_FLOOR. */
        texid = (uint16_t)ro3_texture_id(p);
        ak = -1;
        for (k = 0u; k < ty->ntexid; k++) {
            if (ty->texid[k] == texid) {
                ptex = (brk == OBJ_BREAK_TEX_FLOOR) ? ty->tex_floor[k]
                                                    : ty->tex[k];
                optex = ptex;
                /* the atlas stands in for the plate, same texels */
                if (ty->atlas && brk == OBJ_BREAK_NONE && ptex
                    && ty->atlas_ok[k]) {
                    ptex = ty->atlas;
                    ak = (int)k;
                }
                break;
            }
        }
        if (st) {
            /* A polygon whose id the harvest does not hold is impossible -
             * the harvest walks the same polygons - so this counts a loader
             * and renderer that have fallen out of step, not a data case. */
            if (k == ty->ntexid) { st->tex_missing_id++; }
            if (ptex) {
                st->tex_poly++;
                /* THE GATE FOR OBJ_BREAK_TEX_FLOOR, and the id it counts is
                 * the one the FRAME is textured from, which under the break
                 * is a multiple of 8 for every polygon. */
                if ((brk == OBJ_BREAK_TEX_FLOOR ? (unsigned)(texid & ~7u)
                                                : (unsigned)texid) & 7u) {
                    st->tex_odd_id++;
                }
            } else {
                st->tex_none++;
            }
        }
        /* Flush when the bound texture changes.  The fan below writes into
         * obj_vbuf from index `nv`, so the buffer has to go out before the
         * first vertex of a run that wants a different texture. */
        /* under the atlas the bound texture rarely changes, but the
         * reflection layer still goes out where the PLATE would have changed:
         * base and reflection interleave per run, and the blended glass shows
         * the order (4,671 pixels on LEVEL41 frame 200 without this).  A run
         * with no reflection pending merges on. */
        if (nv && (!cur_have || cur_tex != ptex
                   || (nv1 && cur_otex != optex))) {
            pt0 = OBJ_NOW();
            plat_gfx_tex_bind(cur_tex);
            op_draws += obj_emit(nv, nenv, nenv ? parts->light->env_tex : 0);
            if (st) { st->draws++; }
            nv = 0u;
            nenv = 0u;
            if (nv1) {
                obj_flush_env(parts->light->env_tex, nv1);
                op_draws++;
                nv1 = 0u;
            }
            op_us[3] += OBJ_NOW() - pt0;
        }
        cur_tex  = ptex;
        cur_otex = optex;
        cur_have = 1;
        pt0 = OBJ_NOW();

        /* Corner i of the FILE into working slot i - the loop at 0x0044EC19,
         * stride 0x44 per slot and 0x10 per corner.  No permutation. */
        for (i = 0; i < n; i++) {
            int32_t ci = obj_ring(i, n, brk);
            int32_t vi = ty->m.corner_vert[p->first_corner + (size_t)ci];
            ro3_vec pt;

            if (vi < 0 || (size_t)vi >= ty->m.npoints) {
                pt.x = pt.y = pt.z = 0.0;
            } else {
                pt = pts[vi];
            }
            obj_project(v, &mview, pt, brk, &obj_tv[i]);
            /* THE CORNER'S OWN u,v, and they follow the RING.  ro3_parse has
             * already divided the 24-bit fixed point by 65536, so these are
             * texels in [0, 256) -, measured over 707,004
             * values with nothing at or above 256.  The page is the format's,
             * not the plate's; plate.h's PLATE_MAX_W is a buffer bound and
             * would be the wrong divisor. */
            obj_tv[i].tu = (float)(ty->m.corner_u[p->first_corner + (size_t)ci]
                                   / RO3_TEXTURE_PAGE);
            obj_tv[i].tv = (float)(ty->m.corner_v[p->first_corner + (size_t)ci]
                                   / RO3_TEXTURE_PAGE);
            if (ak >= 0) {
                obj_tv[i].tu = ty->atlas_u0[ak] + obj_tv[i].tu * ty->atlas_su;
                obj_tv[i].tv = ty->atlas_v0[ak] + obj_tv[i].tv * ty->atlas_sv;
            }
            /* Light mode 3 per corner: the corner's own normal through the
             * object-to-view 3x3 (0x00459F6F), as objlit.c does. */
            if (parts && parts->light) {
                int32_t ni = ty->m.corner_norm[p->first_corner + (size_t)ci];
                float   vn[3] = { 0.0f, 0.0f, 0.0f };
                int     b6 = ro3_sel_a(p);
                int     ba = (int)(((uint32_t)p->selector >> 16) & 0xFFu);
                int     lit;
                /* the same arithmetic, done once per (vertex, normal)
                 * pair per draw; `pr` is NULL when the draw is not cached. */
                struct obj_pair *pr = pairs
                    ? &pairs[poc[p->first_corner + (size_t)ci]] : NULL;
                uint32_t key;
                if (pr && pr->st_vn == stamp) {
                    vn[0] = pr->vn[0]; vn[1] = pr->vn[1]; vn[2] = pr->vn[2];
                } else {
                    if (ni >= 0 && (size_t)ni < ty->m.nnormals) {
                        float a = (float)ty->m.normals[ni].x;
                        float b = (float)ty->m.normals[ni].y;
                        float c = (float)ty->m.normals[ni].z;
                        vn[0] = mview.m[0] * a + mview.m[4] * b + mview.m[ 8] * c;
                        vn[1] = mview.m[1] * a + mview.m[5] * b + mview.m[ 9] * c;
                        vn[2] = mview.m[2] * a + mview.m[6] * b + mview.m[10] * c;
                    }
                    if (pr) {
                        pr->vn[0] = vn[0]; pr->vn[1] = vn[1]; pr->vn[2] = vn[2];
                        pr->st_vn = stamp;
                    }
                }
                /* 0x0044EE2F's gate, context 1 */
                if (b6 == 0x1C)   lit = 1;
                else if (b6 == 5) lit = 0;
                else if (b6 < 10) lit = 1;
                else              lit = ro3_sel_b(p) == 0;
                key = ((uint32_t)lit << 8) | (uint32_t)ba;
                if (pr && pr->st_col == stamp && pr->col_key == key) {
                    obj_tv[i].col = pr->col;
                } else {
                    obj_tv[i].col = obj_light3(parts->light, vn, lit,
                                               ba ? (uint32_t)ba * 0x10101u
                                                  : 0x00FFFFFFu);
                    if (pr) {
                        pr->col = obj_tv[i].col;
                        pr->col_key = key;
                        pr->st_col = stamp;
                    }
                }
                obj_tv[i].nx = vn[0]; obj_tv[i].ny = vn[1]; obj_tv[i].nz = vn[2];
            } else if (op_rec) {
                /* A race record: 0x0044EE2F's gate, FUN_0044EEB9's mode 1 / 2
                 * through the level's ramp, then the mode-2 arm's modulate by
                 * the polygon's +0x0A grey ([0x005D6F60], 0x0044EA57). */
                int32_t ni = ty->m.corner_norm[p->first_corner + (size_t)ci];
                float   vn[3] = { 0.0f, 0.0f, 0.0f };
                int     b6 = ro3_sel_a(p);
                int     ba = (int)(((uint32_t)p->selector >> 16) & 0xFFu);
                int     lit;
                uint32_t c;
                if (ni >= 0 && (size_t)ni < ty->m.nnormals) {
                    float a = (float)ty->m.normals[ni].x;
                    float b = (float)ty->m.normals[ni].y;
                    float cc = (float)ty->m.normals[ni].z;
                    vn[0] = mview.m[0] * a + mview.m[4] * b + mview.m[ 8] * cc;
                    vn[1] = mview.m[1] * a + mview.m[5] * b + mview.m[ 9] * cc;
                    vn[2] = mview.m[2] * a + mview.m[6] * b + mview.m[10] * cc;
                }
                if (b6 == 0x1C)   lit = 1;
                else if (b6 == 5) lit = 0;
                else if (b6 < 10) lit = 1;
                else              lit = ro3_sel_b(p) == 0;
                /* The glows' arm lights nothing: 0x0044ED63 calls 0x0044EE84,
                 * whose colour is [0x005E7EE4] - a light colour whose writer
                 * on this path is unread, 0xFFFFFF
                 * on the 0x3B list; white here. */
                c = glow ? 0xFFFFFFFFu
                  : lit ? obj_satadd(obj_rec_light(v, vn), op_rec_local)
                        : 0xFFFFFFFFu;
                obj_tv[i].col = stage_modulate8(c, ba ? (uint32_t)ba * 0x10101u
                                                      : 0x00FFFFFFu) | 0xFF000000u;
            }
        }
        pt1 = OBJ_NOW();
        op_us[0] += pt1 - pt0;
        pt0 = pt1;

        /* THE REFLECTION PASS (0x0044EBC2, 0x0044F862..0x0044F8A1): on when
         * [0x00581BBC], [0x005D6760] (1 in a race, 0x004570D6), the context
         * (1: not the backdrop's 4) and the polygon's byte +9 are all
         * non-zero.  Every corner gets the SAME weight, with F corner 0's
         * fog byte (the original reads 0x0091D930, what the previous
         * polygon's submit left; the port takes this polygon's corner 0 -
         * the car is near, and the byte is 255 there). */
        if (parts && parts->light && parts->light->reflect != 0
            && parts->light->env_tex != 0 && rinv && b9 != 0
            && !op_no_reflect) {
            uint32_t fog0 = v->fog ? (uint32_t)stage_fog_byte(v->fog, obj_tv[0].vz)
                                   : 0xFFu;
            uint32_t x = ((uint32_t)b9 * parts->light->reflect * fog0) >> 12;
            ew = x > 0xFFu ? 0xFFu : x;
            env = ew != 0;
            if (env) {
                for (i = 0; i < n; i++) {
                    float nn[3], pp[3];
                    /* objlit_env_uv is a pure function of the pair's
                     * normal and position, so once per pair per draw. */
                    struct obj_pair *pr = pairs
                        ? &pairs[poc[p->first_corner
                                     + (size_t)obj_ring(i, n, brk)]] : NULL;

                    if (pr && pr->st_env == stamp) {
                        obj_tv[i].eu = pr->eu;
                        obj_tv[i].ev = pr->ev;
                        continue;
                    }
                    nn[0] = obj_tv[i].nx; nn[1] = obj_tv[i].ny; nn[2] = obj_tv[i].nz;
                    pp[0] = obj_tv[i].vx; pp[1] = obj_tv[i].vy; pp[2] = obj_tv[i].vz;
                    objlit_env_uv(nn, pp, rinv, &obj_tv[i].eu, &obj_tv[i].ev);
                    if (pr) {
                        pr->eu = obj_tv[i].eu;
                        pr->ev = obj_tv[i].ev;
                        pr->st_env = stamp;
                    }
                }
                if (st) { st->reflect++; }
            }
        }
        pt1 = OBJ_NOW();
        op_us[1] += pt1 - pt0;
        pt0 = pt1;

        /* FUN_0045053C: reject when dot(vertex - eye, normal) > 0, so a face
         * is FRONT-facing when its stored normal points toward the eye.  Done
         * in VIEW space, where the eye is the origin - the object matrix's
         * linear part carries a uniform non-negative scale, which cannot flip
         * the sign of the dot product. */
        /* the face normal feeds only the no-light shade and the
         * statistics (`front`, the winding counts), so a lit or record draw
         * with the statistics off does not look it up. */
        shade_n.x = shade_n.y = shade_n.z = 0.0;
        have_n = 0;
        if (st || !((parts && parts->light) || op_rec)) {
            have_n = (ty->m.nnormals > 0)
                  && ro3_normal(&ty->m, p->normal_index, &nrm);
            if (!have_n) {
                if (!ro3_face_normal(&ty->m, p, &shade_n)) {
                    shade_n.x = 0.0; shade_n.y = 1.0; shade_n.z = 0.0;
                }
            } else {
                shade_n = nrm;
            }
        }
        if (st && have_n) {
            float nx = (float)nrm.x, ny = (float)nrm.y, nz = (float)nrm.z;
            float vnx = mview.m[0] * nx + mview.m[4] * ny + mview.m[ 8] * nz;
            float vny = mview.m[1] * nx + mview.m[5] * ny + mview.m[ 9] * nz;
            float vnz = mview.m[2] * nx + mview.m[6] * ny + mview.m[10] * nz;
            const objvtx *q = &obj_tv[0];

            front = (q->vx * vnx + q->vy * vny + q->vz * vnz) <= 0.0f;
        }

        /* The fan: (0,1,2) at 0x0044FF28, then (0, i, i+1) for i = 2..N-1. */
        for (i = 1; i + 1 < n; i++) {
            const objvtx *a = &obj_tv[0], *b = &obj_tv[i], *c = &obj_tv[i + 1];
            objvtx cv[4];
            int    ncv = 3, t, clipped = 0;
            float area;
            int   j, want_env, venv_on;

            if (st) { st->tris++; }

            if (a->behind || b->behind || c->behind) {
                /* FUN_0044AD8A's near clip (FUN_0044BBEA), as the
                 * backdrop does it.  A placed object never straddled the
                 * near plane until DRIVER / INTERIOR put the eye inside the
                 * car: 722 of the cabin's 2,104 triangles touched it per
                 * frame, and dropping them opened the doors to the road. */
                objclipv in[3], out[4];
                const objvtx *src[3];
                int k;

                src[0] = a; src[1] = b; src[2] = c;
                for (k = 0; k < 3; k++) {
                    in[k].x = src[k]->vx; in[k].y = src[k]->vy;
                    in[k].z = src[k]->vz;
                    in[k].u = src[k]->tu; in[k].v = src[k]->tv;
                    in[k].c[0] = (float)((src[k]->col >> 16) & 0xFFu);
                    in[k].c[1] = (float)((src[k]->col >> 8) & 0xFFu);
                    in[k].c[2] = (float)(src[k]->col & 0xFFu);
                }
                ncv = bg_clip_near(in, 3, v->near_z, out);
                if (ncv < 3) {
                    if (st) { st->behind++; }
                    continue;
                }
                if (st) { st->clipped++; }
                clipped = 1;        /* the reflection layer skips it       */
                for (k = 0; k < ncv; k++) {
                    float sc = v->focal / out[k].z;
                    cv[k].vx = out[k].x; cv[k].vy = out[k].y;
                    cv[k].vz = out[k].z;
                    cv[k].tu = out[k].u; cv[k].tv = out[k].v;
                    cv[k].col = 0xFF000000u
                              | ((uint32_t)lrintf(out[k].c[0]) << 16)
                              | ((uint32_t)lrintf(out[k].c[1]) << 8)
                              | (uint32_t)lrintf(out[k].c[2]);
                    cv[k].sx =  out[k].x * sc + v->centre_x;
                    cv[k].sy = -out[k].y * sc + v->centre_y;
                    cv[k].behind = 0;
                }
            } else {
                cv[0] = *a; cv[1] = *b; cv[2] = *c;
            }

            for (t = 1; t + 1 < ncv; t++) {
            a = &cv[0]; b = &cv[t]; c = &cv[t + 1];
            if (st) {
                int k = (i == 1) ? 0 : 1;   /* the fan's first, or a later one */
                area = obj_area2(a, b, c);   /* statistics only */
                if (!have_n)          { st->no_normal++; }
                else if (area > 0.0f) { if (front) st->front_cw[k]++;  else st->back_cw[k]++;  }
                else if (area < 0.0f) { if (front) st->front_ccw[k]++; else st->back_ccw[k]++; }
            }

            if (nv + 3 > OBJ_MAXV) {
                continue;
            }
            /* a batch with a reflecting triangle is written straight
             * into obj_venv.  It starts in obj_vbuf (the plain draw's array);
             * the first reflecting triangle moves what is there across, once,
             * and the rest of the batch is written in place. */
            want_env = env && !clipped && !op_two_pass;
            if (want_env && nenv == 0u) {
                unsigned q;
                for (q = 0; q < nv; q++) {
                    obj_venv[q].v    = obj_vbuf[q];
                    obj_venv[q].eu   = obj_veu[q];
                    obj_venv[q].ev   = obj_vev[q];
                    obj_venv[q].ecol = obj_vecol[q];
                }
            }
            venv_on = nenv != 0u || want_env;
            for (j = 0; j < 3; j++) {
                const objvtx *s = (j == 0) ? a : (j == 1) ? b : c;
                plat_vertex  *o = venv_on ? &obj_venv[nv].v : &obj_vbuf[nv];
                nv++;
                float         sz = s->vz * dscale;

                o->sx  = s->sx;
                o->sy  = s->sy;
                o->rhw = (sz > 0.0f) ? (0.01f / sz) : 0.0f;
                if (sz < 0.0f) { sz = 0.0f; }
                if (sz > 1.0f) { sz = 1.0f; }
                o->sz  = sz;
                o->diffuse  = ((parts && parts->light) || op_rec)
                            ? s->col : obj_shade(shade_n);
                /* PORT'S CHOICE, and it is a choice rather than a
                 * transcription: the stage path's fog byte is written at
                 * 0x00449D6B out of the working slot's +0x20, and what fills
                 * +0x20 on the MODEL path is not read by anyone.  Leaving it
                 * zero is NOT the neutral option - zero is alpha 0, which is
                 * FULLY fogged, so every object would come out a flat
                 * FOGCOLOR silhouette.  The stage's own ramp is used instead,
                 * which at least puts the objects in the same air as the
                 * terrain around them. */
                o->specular = v->fog
                            ? ((uint32_t)stage_fog_byte(v->fog, s->vz) << 24)
                            : 0xff000000u;
                o->tu = s->tu;
                o->tv = s->tv;

                /* single pass - this vertex's reflection rides with
                 * it (weight 0 = none, as an unreflected run was) */
                if (venv_on) {
                    obj_venv[nv - 1].eu   = env ? s->eu : 0.0f;
                    obj_venv[nv - 1].ev   = env ? s->ev : 0.0f;
                    obj_venv[nv - 1].ecol = want_env
                        ? ((s->col & 0x00FFFFFFu) | (ew << 24)) : 0u;
                } else {
                    obj_veu[nv - 1] = env ? s->eu : 0.0f;
                    obj_vev[nv - 1] = env ? s->ev : 0.0f;
                    obj_vecol[nv - 1] = 0u;
                }
                if (want_env) {
                    nenv++;
                } else if (env && !clipped && nv1 + 3 <= OBJ_MAXV) {
                    plat_vertex *o1 = &obj_vbuf1[nv1++];
                    *o1 = *o;
                    o1->diffuse  = (s->col & 0x00FFFFFFu) | (ew << 24);
                    o1->specular = 0xFF000000u;          /* unfogged        */
                    o1->tu = s->eu;
                    o1->tv = s->ev;
                }

                if (st) {
                    if (s->sx < st->min_sx) { st->min_sx = s->sx; }
                    if (s->sx > st->max_sx) { st->max_sx = s->sx; }
                    if (s->sy < st->min_sy) { st->min_sy = s->sy; }
                    if (s->sy > st->max_sy) { st->max_sy = s->sy; }
                }
            }
            if (st) { st->drawn++; }
            }
        }
        op_us[2] += OBJ_NOW() - pt0;
    }

    if (nv) {
        /* The last run.  `cur_tex` is 0 when nothing was bound for it, which
         * is plat.h's null texture and draws untextured - what the whole
         * layer did before. */
        pt0 = OBJ_NOW();
        plat_gfx_tex_bind(cur_tex);
        op_draws += obj_emit(nv, nenv, nenv ? parts->light->env_tex : 0);
        if (st) { st->draws++; }
        if (nv1) {
            obj_flush_env(parts->light->env_tex, nv1);
            op_draws++;
        }
        op_us[3] += OBJ_NOW() - pt0;
    }
    if (glow) {
        plat_gfx_state_restore(&glow_saved);
    }
}

/* ===================================================================== *
 *  THE FRAME BACKGROUND -,                             *
 * ===================================================================== *
 *
 * objscene.h carries the transcription of 0x0045343E and FUN_00425E6E.
 * What is below is the code and the two places where the port is not
 * literal, both marked.
*/

void objscene_backdrop_params(objscene_backdrop *b, const ld_file *ld,
                              int have_ld, int32_t radius, int32_t detail)
{
    int32_t zfar = radius * 32;             /* 0x0044C516, R cells of 32    */
    int32_t d    = 100 - detail;            /* 0x0044C5D5                   */
    float   d01, s;

    memset(b, 0, sizeof *b);

    if (d < 90)  { d = 90; }                /* `CMP 0x5a` / `JNS` 0x0044C5E5 */
    if (d > 100) { d = 100; }               /* `CMP 0x64` / `JS`  0x0044C5F8 */
    d01 = (float)d * 0.01f;                 /* [0x005D70B8]                  */

    /* 0x0044C61D: `MUL EBX` with EBX = 0x10000 and EDX DISCARDED, so the
     * product is the low 32 bits - then FILD, which reads it SIGNED.  At the
     * corpus's radii (1..15, so zfar <= 480) it cannot reach 2^31, and the
     * port does not reproduce a wrap it can show never happens. */
    s = (float)((int32_t)((uint32_t)zfar * 65536u)) / d01;   /* [0x005D705C] */

    /* 0x0044C666: `FMUL 0.9 / FDIV 1.5 / FDIV 65536.0 / FISTP`.  The order is
     * the original's; the three constants are [0x005D70BC], [0x005D70B4] and
     * [0x0057B01C], each read exactly once in the image.
     *
     * `FISTP` ROUNDS, IT DOES NOT TRUNCATE, and a C cast does.  The x87
     * control word's RC field is round-to-nearest-even at its default and
     * nothing in this image changes it, so `lrintf` under the default FP
     * environment is the transcription and `(int32_t)` is not.
     *
     * IT IS NOT ACADEMIC.  The value is `0.6 * R * 32 / D`, and over the
     * fifteen legal radii and the two D endpoints the two differ at
     * **eleven** of the thirty combinations - R = 3, 4, 8, 9, 13 and 14 at
     * D = 1.00, and R = 2, 5, 8, 11 and 14 at D = 0.90.  At R = 6, which is
     * what every gate in this project forces, the value is 115.2 and they
     * agree, so the defect was invisible to all 48 levels of the sweep and
     * was found by re-deriving the arithmetic rather than by a picture.
     * Standing rule 6. */
    b->scale = (int32_t)lrintf(s * 0.9f / 1.5f / 65536.0f);  /* [0x005D7064] */

    if (have_ld && ld) {
        b->yaw = (int)ld_dw(ld, OBJ_SKY_YAW_OFF);
        b->y   = (int)ld_dw(ld, OBJ_SKY_Y_OFF);
    }
}

void objscene_backdrop_matrix(ral_mat4 *out, const objscene_backdrop *b,
                              const ral_obj_camera *cam, int brk)
{
    ral_mat4 ry, t, cur;
    int32_t  scale = (brk == OBJ_BREAK_SKY_NO_SCALE) ? 1 : b->scale;
    int      i;

    /* 0x00425E6E: `EDX = [0x005BBFF0] << 8 / NEG EDX / CALL 0x00425C3A`.
     * The NEG is the same one the instance path applies to its own yaw at
     * 0x00425FFF, so the two headings are in the same sense. */
    ral_obj_rot_y16(&ry, (unsigned)(-(int)((unsigned)b->yaw << 8)));

    /* 0x00425E7E: x and z are written ZERO and only y comes from the file.
     * The dome has no horizontal offset from the eye at all. */
    ral_obj_translate_i(&t, 0, (int)((unsigned)b->y << 8), 0);

    /* 0x00425EC7 - FUN_00425DF4, a copy plus a translation ADD. */
    ral_mat4_copy_add_t(&cur, &ry, &t);

    /* 0x00425ED6 onward.  The scale reaches the float through [0x005E7EC8]
     * as `scale << 4` and is then divided by 16.0, which is exact for an int
     * and is why this is `scale` and not `scale/16`.  ALL SIXTEEN floats,
     * translation row included - and here, unlike obj_tail's, the translation
     * row is ALREADY FILLED, so the sky's height offset is scaled too. */
    for (i = 0; i < 16; ++i) {
        cur.m[i] *= (float)(((int32_t)((uint32_t)scale << 4))) / 16.0f;
    }

    if (brk == OBJ_BREAK_SKY_AT_ORIGIN) {
        /* obj_tail's 0x00426187 put back.  FUN_00425E6E has no such line. */
        ral_mat4 fixed = cur;
        ral_mat4_copy_add_t(&cur, &fixed, &cam->view_trans);
    }

    if (brk == OBJ_BREAK_SKY_PINNED) {
        ral_mat4 ident;
        memset(&ident, 0, sizeof ident);
        ident.m[0] = ident.m[5] = ident.m[10] = ident.m[15] = 1.0f;
        ral_mat4_mul(out, &cur, &ident);
    } else {
        /* 0x00425F30 - into view space, and NOTHING has added the camera's
         * position.  The dome therefore stands on the eye. */
        ral_mat4_mul(out, &cur, &cam->view_rot);
    }
}

/* ---- the backdrop's near-plane clip ------------------------------------ *
 *
 * THE BACKDROP NEEDS ONE AND THE INSTANCE LAYER DOES NOT, and the reason is
 * geometric rather than stylistic: the dome is centred ON THE EYE, so roughly
 * half of its 70 fan triangles straddle the near plane on every frame.
 * objscene_draw_record's "drop it and count it" leaves 47 of 70 unsubmitted
 * and puts BLACK WEDGES in the top corners of the frame.
 *
 * THE ORIGINAL CLIPS.  FUN_0044AD8A's middle gate calls FUN_0044BBEA, two
 * Sutherland-Hodgman passes - near, then far - and the sky path disables only
 * the FAR one ([0x005D4240] = 1 at 0x004534C5, read at 0x0044BC03).  So the
 * near clip is live for the dome in the game, and this is that behaviour, not
 * a port's invention.  It is NOT a transcription of FUN_0044BBEA's code:
 * that one rewrites the shared working-vertex array and its order table, and
 * this one clips one triangle at a time into a local buffer.
 *
 * A triangle clipped against one plane has at most four corners, so four
 * slots is exact and not headroom.  (objclipv is declared above
 * obj_draw_polys, which clips with this too since.) */
static int bg_clip_near(const objclipv *in, int n, float near_z,
                        objclipv *out)
{
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const objclipv *a = &in[i];
        const objclipv *c = &in[(i + 1) % n];
        int ain = !(a->z < near_z);
        int cin = !(c->z < near_z);

        if (ain) {
            out[m++] = *a;
        }
        if (ain != cin) {
            float t = (near_z - a->z) / (c->z - a->z);
            out[m].x = a->x + (c->x - a->x) * t;
            out[m].y = a->y + (c->y - a->y) * t;
            out[m].z = near_z;
            out[m].u = a->u + (c->u - a->u) * t;
            out[m].v = a->v + (c->v - a->v) * t;
            out[m].c[0] = a->c[0] + (c->c[0] - a->c[0]) * t;
            out[m].c[1] = a->c[1] + (c->c[1] - a->c[1]) * t;
            out[m].c[2] = a->c[2] + (c->c[2] - a->c[2]) * t;
            m++;
        }
    }
    return m;
}

void objscene_draw_backdrop(objscene *os, const objscene_backdrop *b,
                            const stage_view *v, const ral_obj_camera *cam,
                            int brk, objscene_stats *st)
{
    objscene_type *ty = &os->type[OBJ_BACKDROP_TYPE];
    ral_mat4       mview;
    unsigned       nv = 0;
    size_t         pi;
    float          dscale;
    plat_tex       cur_tex  = 0;
    int            cur_have = 0;

    /* `CMP [EBX*4 + 0x005E5E00],0 / JNE +1 / RET` at 0x0044FDF2, with the
     * type fixed at 1 by 0x0045347F.  A level whose type 1 has no model draws
     * no backdrop and the frame keeps whatever the shim last left - which is
     * exactly what the original does and is worth counting rather than
     * asserting away. */
    if (!ty->have || ty->m.npolys == 0) {
        if (st) { st->no_model++; }
        return;
    }

    objscene_backdrop_matrix(&mview, b, cam, brk);
    dscale = stage_depth_scale(v->radius, v->detail);
    if (st) { st->placed++; }
    /* The dome's corners carry the flat fog (below), so the frame's fog must
     * be live before the stage pass sets it: FOGCOLOR is the level's from the
     * load in the original, and mode 2's block turns FOGENABLE on. */
    if (v->fog) {
        plat_gfx_set_state(PLAT_RS_FOGCOLOR,  v->fog->color);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
    }

    for (pi = 0; pi < ty->m.npolys; pi++) {
        const ro3_poly *p = &ty->m.polys[pi];
        int32_t  i, n = p->corners;
        uint16_t texid;
        plat_tex ptex = 0;
        unsigned k;

        if (n < 3 || n > RO3_MAX_CORNERS) {
            continue;
        }
        if (st) { st->polys++; }

        texid = (uint16_t)ro3_texture_id(p);
        for (k = 0u; k < ty->ntexid; k++) {
            if (ty->texid[k] == texid) {
                ptex = (brk == OBJ_BREAK_TEX_FLOOR) ? ty->tex_floor[k]
                                                    : ty->tex[k];
                break;
            }
        }
        if (st) {
            if (k == ty->ntexid) { st->tex_missing_id++; }
            if (ptex) {
                st->tex_poly++;
                if ((brk == OBJ_BREAK_TEX_FLOOR ? (unsigned)(texid & ~7u)
                                                : (unsigned)texid) & 7u) {
                    st->tex_odd_id++;
                }
            } else {
                st->tex_none++;
            }
        }
        if (nv && (!cur_have || cur_tex != ptex)) {
            plat_gfx_tex_bind(cur_tex);
            plat_gfx_draw(PLAT_PRIM_TRIANGLES, obj_vbuf, nv);
            if (st) { st->draws++; }
            nv = 0u;
        }
        cur_tex  = ptex;
        cur_have = 1;

        for (i = 0; i < n; i++) {
            int32_t ci = obj_ring(i, n, brk);
            int32_t vi = ty->m.corner_vert[p->first_corner + (size_t)ci];
            ro3_vec pt;

            if (vi < 0 || (size_t)vi >= ty->m.npoints) {
                pt.x = pt.y = pt.z = 0.0;
            } else {
                pt = ty->m.points[vi];
            }
            obj_project(v, &mview, pt, brk, &obj_tv[i]);
            obj_tv[i].tu = (float)(ty->m.corner_u[p->first_corner + (size_t)ci]
                                   / RO3_TEXTURE_PAGE);
            obj_tv[i].tv = (float)(ty->m.corner_v[p->first_corner + (size_t)ci]
                                   / RO3_TEXTURE_PAGE);
        }

        /* NO BACKFACE TEST.  0x0044EC33's `CMP [0x005E948C],4 / JE` skips the
         * block the instance path runs, and thirteen of the fifteen dome
         * models carry no normal table at all to test with
         * The caller draws
         * this with culling off. */
        for (i = 1; i + 1 < n; i++) {
            objclipv tri[3], clipped[4];
            int      j, m, k;

            if (st) { st->tris++; }
            for (j = 0; j < 3; j++) {
                const objvtx *q = (j == 0) ? &obj_tv[0]
                                : (j == 1) ? &obj_tv[i] : &obj_tv[i + 1];
                tri[j].x = q->vx; tri[j].y = q->vy; tri[j].z = q->vz;
                tri[j].u = q->tu; tri[j].v = q->tv;
            }
            m = bg_clip_near(tri, 3, v->near_z, clipped);
            if (m < 3) {
                /* Wholly behind the near plane - the half of the dome that is
                 * behind the camera, every frame. */
                if (st) { st->behind++; }
                continue;
            }
            if (m > 3 && st) { st->clipped++; }
            if (nv + (unsigned)(m - 2) * 3u > OBJ_MAXV) {
                continue;
            }
            for (k = 1; k + 1 < m; k++)
            for (j = 0; j < 3; j++) {
                const objclipv *s = (j == 0) ? &clipped[0]
                                  : (j == 1) ? &clipped[k] : &clipped[k + 1];
                plat_vertex  *o = &obj_vbuf[nv++];
                float         sc = v->focal / s->z;
                float         sz = s->z * dscale;

                o->sx  =  s->x * sc + v->centre_x;
                o->sy  = -s->y * sc + v->centre_y;   /* FCHS at 0x0044B06A */
                o->rhw = (sz > 0.0f) ? (0.01f / sz) : 0.0f;
                if (sz < 0.0f) { sz = 0.0f; }
                if (sz > 1.0f) { sz = 1.0f; }
                o->sz  = sz;
                /* 0x0044EE2F: `MOV EAX,0xFFFFFF / CMP [0x005E948C],4 / JE`.
                 * The backdrop is FULLBRIGHT and unlit, and 0x00453493 has
                 * already written 0xFF into all three colour dwords. */
                o->diffuse  = 0xFFFFFFFFu;
                /* ~~AND IT IS NOT FOGGED~~ - READ: it is fogged by the
                 * level's FLAT amount.  FUN_0044AD8A calls 0x0044C2F5 ->
                 * FUN_0044C321, whose 0x0044C355 `CMP [0x005E948C],4 / JNE`
                 * swaps the distance lookup for [0x005E94D4] - stage_fog's
                 * `flat` - and 0x0044C363 writes 255 - it to the corner's fog
                 * byte.  On day stages flat clamps to 0 (no fog: run 17's
                 * panorama); on LEVEL46 it is ~250 toward FOGCOLOR (0,0,4),
                 * which is the original's black night sky. */
                o->specular = v->fog
                            ? ((uint32_t)(255u - ((unsigned)v->fog->flat & 0xffu)) << 24)
                            : 0xff000000u;
                o->tu = s->u;
                o->tv = s->v;
                if (st && j == 2) { st->drawn++; }
            }
        }
    }

    if (nv) {
        plat_gfx_tex_bind(cur_tex);
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, obj_vbuf, nv);
        if (st) { st->draws++; }
    }
}
