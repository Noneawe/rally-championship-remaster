/* cdat.h - Rally Championship's `cdat00c` level payload, as a buffer-taking
 * parser.
 *
 * and it is the format is
 * blocked on: this is the stage geometry, and the renderer cannot call
 * tools/cdat.py.
 *
 * A decompressed `cdat00c` is a 256-byte header plus three record arrays -
 * `zube`, `pols`, `pnts` - and the sectioning is FUN_004208d5's:
 *
 *     base + 0x100 + zube*0x20 + pols*0x40 + pnts*0x0c  ==  payload size
 *
 * which closes in 55 of 55 payloads. Every offset below is an operand at a
 * named address; nothing here was pattern-matched off a dump. See
 * for the transcription of
 * each one with the address it came from.
 *
 * This is a transcription of tools/cdat.py and is held to BYTE-IDENTICAL
 * output by check.sh.
*/
#ifndef RAL_CDAT_H
#define RAL_CDAT_H

#include <stddef.h>
#include <stdint.h>

#define CDAT_HDR_BYTES 0x100   /* FUN_004208d5: base + 0x100 is the zube section */
#define CDAT_HDR_WORDS 64      /* what FUN_004438c2 block-copies to 0x00581a5c */
#define CDAT_ZUBE_SIZE 0x20    /* `mov ebx,0x20` / `shl esi,5` */
#define CDAT_POLS_SIZE 0x40    /* `mov ebx,0x40` / `shl esi,6` */
#define CDAT_PNTS_SIZE 0x0c    /* `mov ebx,0xc`  / x3 then `shl esi,2` */

/* The cell shift. FUN_00440424 / FUN_0045bde9: `shr eax,0x15` on a 16.16
 * world coordinate, so a cell is 2**21 / 65536 == 32.0 world units and the
 * quotient is an 11-bit wrapping index. FUN_00452b2f does the inverse. */
#define CDAT_CELL_SHIFT 21
#define CDAT_CELL_MASK  0x7ffu          /* `and eax,0x7ff`, 0x00452b21 */
#define CDAT_ZUBE_KEY_END 0xffffffffu   /* `cmp eax,-1` / `jz` ends every walk */

/* `cmp al,0x10` / `jae` - three readers agree it skips the record. */
#define CDAT_POLY_DRAWABLE_MAX 0x10
#define CDAT_POLY_ROUTE        0xff   /* FUN_00443a33 collects exactly these */
#define CDAT_POLY_DELETED      0x80   /* FUN_00420afa writes this and -1 corners */

/* FUN_004438d6 sets [0x005d6520], the marker texture id, to 0x3e - or to 0x3d
 * when [0x006ce08c] == 1, and to 0 otherwise. */
#define CDAT_ROUTE_TEX_A 62
#define CDAT_ROUTE_TEX_B 61

/* `pols` field offsets. The six normal dwords are SCRATCH: store_norms_q
 * 0x00420bf4 -> FUN_00420c64 writes them at load, and the shipped files leave
 * them zero in 1,570,376 of 1,570,376 drawable polygons. The early demo does
 * NOT, which is what makes them readable as floats and is the one check here
 * that owes nothing to the disassembly. */
#define CDAT_POL_CLASS      0x00
#define CDAT_POL_TEX0       0x01
#define CDAT_POL_FLAGS      0x02
#define CDAT_POL_CORNER0    0x04
#define CDAT_POL_SHADE      0x14
#define CDAT_POL_TEX1_SYM   0x20
#define CDAT_POL_TEX1       0x21
#define CDAT_POL_BLEND      0x22
#define CDAT_POL_ROUTE_TAG  0x26

/* The class-0xff variant is an OBJECT INSTANCE, not a polygon, and only
 * +0x00 means the same thing in both. */
#define CDAT_OBJ_TYPE       0x01   /* NOT a texture id */
#define CDAT_OBJ_ANCHOR     0x04   /* `pnts` index, bound-checked at 0x0044fb2f */
#define CDAT_OBJ_SELECTOR   0x25   /* bits 1-2: the strip selector */
#define CDAT_OBJ_TYPE_MAX   0x64   /* `cmp [0x005d422c],0x64` / `ja` -> dropped */

extern const unsigned cdat_normal_scratch[6];        /* 0x18 0x1c 0x30 0x34 0x38 0x3c */
extern const unsigned cdat_normal_xyz[2][3];         /* per triangle: x, y, z */
extern const unsigned cdat_triangle_corners[2][3];   /* FUN_00420c64's register order */

typedef struct {
    const uint8_t *buf;     /* the DECOMPRESSED payload */
    size_t         len;
    uint8_t       *owned;   /* non-NULL when cdat_open_buffer decompressed it */

    int32_t  hdr[CDAT_HDR_WORDS];
    int32_t  nzube, npols, npnts;
    size_t   zube_off, pols_off, pnts_off, end;

    /* Do the three declared record arrays lie inside the buffer?  See the
     * contract on the accessors below - this is a memory-safety flag, not a
     * report, and it is computed in 64-bit so a count near UINT32_MAX cannot
     * wrap the multiplication on a 32-bit host. */
    int      fits;
} cdat_payload;

/* Section a payload that is already decompressed. Never fails: the identity
 * check is `cdat_closes`, exactly as cdat.py reports it rather than refusing
 * the file. */
void cdat_section(const void *buf, size_t len, cdat_payload *p);

/* Take a `.sq` OR an already-decompressed payload - the magic decides, which
 * is what cdat.py's open_payload does. On an SQSL file the decompressed bytes
 * are allocated and owned by the payload.
 *
 * RETURNS 0 ON SUCCESS, -1 ON FAILURE, with `err` set. This is stated because
 * it is the OPPOSITE of the convention the other predicate in this header
 * (`cdat_closes`) uses, and's first draft got it the wrong way
 * round and refused every valid level. */
int  cdat_open_buffer(const void *buf, size_t len, cdat_payload *p,
                      char *err, size_t errlen);
void cdat_free(cdat_payload *p);

int  cdat_closes(const cdat_payload *p);

/* ---- THE ACCESSORS' CONTRACT, and it is load-bearing -------------------
 *
 * A caller must check `p->fits` before calling ANY accessor below, and must
 * pass an index below the matching declared count. The accessors do not
 * re-check: they are the inner loop of the renderer's cell walk, over
 * 1.8 million polygons a level.
 *
 * This is written down rather than assumed because it was a live defect for
 * the length of one afternoon. Nothing validated the three header counts, so
 * a payload whose header was garbage - which is what a CORRUPT DECOMPRESSOR
 * produces - sent every accessor reading past the end of the buffer, and
 * `cdat check` SEGFAULTED. It was found by breaking sqsl_decode's
 * back-reference distance on purpose to see whether the agreement gate would
 * notice (standing rule 11); the gate noticed, by crashing.
 *
 * The general shape is the one already records one level
 * down: a transcription between languages is most wrong where the languages
 * DIFFER, and the valid corpus cannot reach it. There it was Python's
 * unbounded integers against C's fixed width. Here it is Python's
 * memory safety against C's - struct.unpack_from raises, a C load does not.
 *
 * `cdat_closes(p)` implies `p->fits`: if the sections end exactly at EOF then
 * every index below a declared count is inside the buffer. The two are kept
 * separate anyway, because `closes` is a FINDING about the format that the
 * tools print, and `fits` is a precondition the port must honour.
*/

/* Record accessors. `zube` is eight int32; `pol` returns a pointer to the
 * 64 raw bytes; `pnt` is x, y, z as int32 16.16 with y up. */
void           cdat_zube(const cdat_payload *p, int32_t i, int32_t out[8]);
uint32_t       cdat_zube_key(const cdat_payload *p, int32_t i);
const uint8_t *cdat_pol(const cdat_payload *p, int32_t i);
void           cdat_pnt(const cdat_payload *p, int32_t i, int32_t out[3]);

/* Little-endian scalar reads out of a record. */
uint32_t cdat_u32(const uint8_t *rec, unsigned off);
uint16_t cdat_u16(const uint8_t *rec, unsigned off);
float    cdat_f32(const uint8_t *rec, unsigned off);

/* FUN_00440424's `shr eax,0x15`. Logical, so a negative 16.16 coordinate
 * wraps into the top of an 11-bit index - which is what the readers then mask
 * with 0x7ff. */
uint32_t cdat_cell_of(int32_t coord);

#endif /* RAL_CDAT_H */
