/* ro3.h - Rally Championship .ro3 geometry, as a buffer-taking parser.
 *
 * asks for plain C so the parser lifts into the port unchanged, and for
 * a parser that takes a *memory buffer* rather than a path or a FILE* - the
 * port reads out of .bun bundles. ro3_parse() is the whole API; nothing in
 * this file or in ro3.c opens, seeks or closes anything.
 *
 * The layout constants below are the specification. Every one is either an
 * operand in the game's own walk (bld_ro3_index_polys, 0x00457d95) or a byte
 * comparison against .lwo ground truth. See
 *
 *
 * This is a transcription of tools/ro3.py, which is the instrument the format
 * was decoded with. The two are kept agreeing on the whole 429-file corpus by
 * `make check`;.
*/
#ifndef RAL_RO3_H
#define RAL_RO3_H

#include <stddef.h>
#include <stdint.h>

#define RO3_HDR_SIZE      0x2C   /* version, npoints, npolys, nnormals, then 28 zero bytes */
#define RO3_POINT_SIZE    12     /* x, y, z as int32 16.16 fixed point */
#define RO3_POLY_HDR_SIZE 0x10   /* corners, flags, selector, face-normal index */
#define RO3_CORNER_SIZE   0x10   /* vertex index, u, v, vertex-normal index */
#define RO3_NORMAL_SIZE   12     /* x, y, z as IEEE float32 */
#define RO3_FIXED_ONE     65536.0

/* The texture page. `ro3 uv` measures u,v in [0, 256) over the whole corpus
 * with nothing above it, which is what bld_uv_pack's divisors predict: a
 * 1024x512 atlas of 4x2 cells of 256x256, the cell chosen by the low 3 bits
 * of +0x04. */
#define RO3_TEXTURE_PAGE  256.0

/* The maximum corner count the parser will believe. Not a format limit - a
 * tripwire, so a mis-parse fails loudly instead of allocating wildly. */
#define RO3_MAX_CORNERS   4096

/* The normal table is not always in the vertex array's frame - see `frames`. */
enum ro3_frame {
    RO3_FRAME_IDENTITY = 0,   /* +x+y+z */
    RO3_FRAME_ROT_Y90  = 1    /* +z+y-x */
};

extern const char *const ro3_frame_name[2];

typedef struct { double x, y, z; } ro3_vec;

typedef struct {
    int32_t corners;
    int32_t flags;         /* +0x04 */
    int32_t selector;      /* +0x08 */
    int32_t normal_index;  /* +0x0c, index into the face/vertex normal table */
    size_t  first_corner;  /* into the model's flat corner arrays */
} ro3_poly;

typedef struct {
    int32_t  version;
    uint8_t  header_tail[RO3_HDR_SIZE - 0x10];  /* bytes 0x10..0x2b */

    ro3_vec *points;    size_t npoints;
    ro3_poly *polys;    size_t npolys;
    ro3_vec *normals;   size_t nnormals;        /* face AND vertex normals, pooled */

    /* Corner data, flat across the whole model - structure-of-arrays, because
     * that is the shape the port's vertex upload wants. */
    size_t   ncorners;
    int32_t *corner_vert;
    double  *corner_u;
    double  *corner_v;
    int32_t *corner_norm;   /* corner +0x0c */

    size_t size;        /* the buffer it was parsed from */
    size_t consumed;    /* where the walk ended */

    int _frame;         /* lazily measured; -1 until ro3_normal_frame() runs */
} ro3_model;

/* Parse a whole .ro3 out of a buffer. Returns 0 on success, non-zero on
 * failure with a human-readable reason in `err`. On failure nothing is
 * allocated. Coordinates are doubles; the fixed-point encoding does not escape
 * the parser. */
int  ro3_parse(const void *buf, size_t len, ro3_model *m, char *err, size_t errlen);
void ro3_free(ro3_model *m);

static inline int ro3_closes_at_eof(const ro3_model *m) { return m->consumed == m->size; }

/* Low 3 bits of +0x04, laid into the top of u (2 bits) and v (1 bit) by
 * bld_uv_pack (0x0045838d). CONFIRMED - every term is a literal.
 *
 * THIS IS A BUILD-TOOL FACT AND A RENDERER MUST NOT USE IT. settled
 * what the low word of +0x04 is: a texture-PLATE id, in FUN_00424672's band
 * ladder, and the 4x2 sheet is the AUTHORING layout - each cell ships as its
 * own 256x256 tx<dddd>.pcx and these three bits are simply the low three bits
 * of that filename. 190 of the corpus's 192 object texture ids find a file as
 * written; only 161 do when floored to a multiple of 8.
 *
 * So a renderer binds ro3_texture_id() and divides u,v by RO3_TEXTURE_PAGE.
 * Applying this as a UV offset as well draws the wrong quarter of the wrong
 * plate - it is port/obj's OBJ_BREAK_TEX_FLOOR, and on LEVEL21 it moves 23
 * pixels. */
static inline int ro3_texture_page(const ro3_poly *p) { return (int)((uint32_t)p->flags & 7u); }

/* The polygon's texture plate: the LOW WORD of +0x04, which is what the game
 * loads at 0x0044ea31 (`MOVZX EAX,word ptr [EBP+4]`). */
static inline int ro3_texture_id(const ro3_poly *p) { return (int)((uint32_t)p->flags & 0xFFFFu); }

/* The exporter's own two selector bytes, read at 0x0045807e-0x0045809a. */
static inline int ro3_sel_a(const ro3_poly *p) { return (int)(((uint32_t)p->flags >> 16) & 0xFFu); }   /* byte[rec + 6]    */
static inline int ro3_sel_b(const ro3_poly *p) { return (int)(((uint32_t)p->selector >> 24) & 0xFFu); } /* byte[rec + 0x0b] */

/* bld_lwo_write_surface's test, transcribed. A polygon it rejects is counted
 * into `numrepeats` at [0x00660146] and never reaches the .lwo. */
static inline int ro3_exporter_accepts(const ro3_poly *p)
{
    int a = ro3_sel_a(p);
    if (a == 0) return 1;
    if (a == 6 || a == 7 || a == 8 || a == 9) return 0;
    return ro3_sel_b(p) == 0;
}

/* -- geometry ------------------------------------------------------------ */

ro3_vec ro3_sub(ro3_vec a, ro3_vec b);
ro3_vec ro3_cross(ro3_vec a, ro3_vec b);
double  ro3_dot(ro3_vec a, ro3_vec b);
/* Normalise in place. Returns 0 and leaves *v untouched if it is too short to
 * have a direction - the caller must treat that as UNDECIDABLE, not as zero. */
int     ro3_normalise(ro3_vec *v);
/* The map that takes a geometric face normal to its stored form in the
 * files/b frame: (x, y, z) -> (z, y, -x), and its inverse. */
ro3_vec ro3_rot_y90(ro3_vec v);
ro3_vec ro3_unrot_y90(ro3_vec v);

/* Geometric normal from the polygon's own first three vertices, normalised.
 * Returns 0 for a degenerate face or an out-of-range vertex index. */
int ro3_face_normal(const ro3_model *m, const ro3_poly *p, ro3_vec *out);
/* Interior angle at corner i, for angle-weighted vertex normals. */
double ro3_corner_angle(const ro3_model *m, const ro3_poly *p, size_t i);
/* RO3_FRAME_*, measured from this file's own header face-normal indices
 * against its own geometry. Detected rather than assumed from the path,
 * because a loader cannot see the path. */
int ro3_normal_frame(ro3_model *m);
/* Normal `i`, brought into the vertex array's frame. Returns 0 if `i` is out
 * of range or the stored normal has no direction. */
int ro3_normal(ro3_model *m, int32_t i, ro3_vec *out);

#endif /* RAL_RO3_H */
