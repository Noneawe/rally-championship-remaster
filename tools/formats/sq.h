/* sq.h - Rally Championship `.sq` files, as a buffer-taking parser.
 *
 * The third format in the C toolkit, after
 * `.ro3` and `.bun`, and it is here because cannot reach a level
 * without it: all 48 shipped `cdat00c` payloads are SQSL-compressed and the
 * renderer cannot call tools/sq.py.
 *
 * A `.sq` is NOT one format. The four-byte magic's last character selects
 * between two unrelated payloads that share only a 16-byte header:
 *
 *     SQSL   files\levels\levelNN\cdat00c.sq   a compressed dword stream
 *     SQSM   files\maps\mapNN.sq               a tiled 2048x2048 stage map
 *
 * Both are transcribed from the game's own readers:
 *
 *     FUN_0041adae  0x0041adae-0x0041b512   the SQSL decoder (1,893 bytes)
 *     FUN_0041aab9  0x0041aab9-0x0041ac3a   the SQSM reader
 *
 * This is a transcription of tools/sq.py, which is the instrument the format
 * was decoded with, and it is held to BYTE-IDENTICAL output by check.sh.
 *
 *
*/
#ifndef RAL_SQ_H
#define RAL_SQ_H

#include <stddef.h>
#include <stdint.h>

#define SQ_HDR_SIZE   0x10          /* magic, 0, version, count */
#define SQ_VERSION    0x00050000u   /* XOR-compared at 0x0040306b; "Rally 5" */

/* SQSM. The reader allocates 0x10000 bytes for the cell table and walks it
 * 0x4000 times (0x0041aba7); the generator path uses a row stride of 0x200
 * bytes with a row index bounded by 0x80 (0x0041a8f9 / 0x0041a901). */
#define SQ_MAP_CELLS        0x4000              /* 128 x 128 */
#define SQ_MAP_GRID         128
#define SQ_MAP_TILE         16                  /* 16 x 16 pixels */
#define SQ_MAP_TILE_BYTES   0x300               /* 256 px x 3 bytes */
#define SQ_MAP_INDEX_BYTES  (SQ_MAP_CELLS * 2)
#define SQ_MAP_PIXELS       (SQ_MAP_GRID * SQ_MAP_TILE)   /* 2048 */

/* The SQSL payload's own sectioning, from FUN_004208d5 (93 bytes):
 *     base + 0x100 + zube*0x20 + pols*0x40 + pnts*0x0c == payload size
 * Repeated in cdat.h, which is the reader that uses them. */
#define SQ_PAYLOAD_HDR 0x100
#define SQ_ZUBE_SIZE   0x20
#define SQ_POLS_SIZE   0x40
#define SQ_PNTS_SIZE   0x0c

enum sq_magic { SQ_MAGIC_NONE = 0, SQ_MAGIC_LEVEL = 1, SQ_MAGIC_MAP = 2 };

typedef struct {
    int      magic;     /* enum sq_magic */
    uint32_t pad;       /* +0x04, zero in every shipped file */
    uint32_t version;
    uint32_t count;     /* SQSL: output words.  SQSM: tiles. */
} sq_header;

/* Returns 0 on success; on rejection returns non-zero and fills `err` with
 * sq.py's exact SqError wording. */
int sq_read_header(const void *buf, size_t len, sq_header *h,
                   char *err, size_t errlen);

/* ---- SQSL - the compressed dword stream -------------------------------- */
/*
 * The decoder emits exactly `count` 32-bit words, one per token, either a
 * literal or a back-reference to a word it has already emitted. The prefix
 * code is read MSB-first out of little-endian 32-bit words:
 *
 *   0      -                emit 0
 *   10     8 bits  d        emit out[i - d - 1]
 *   1100   12 bits d        emit out[i - d - 1]
 *   1101   17 bits unsigned emit it
 *   1110   28 bits signed   emit it   (sign bit first, then 27 more)
 *   1111   32 bits          emit it
 *
 * The back-reference index is `i - d - 1` and not `i - d`: the game computes
 * `[edi + (-d)*4 - 4]` with edi already pointing at the slot about to be
 * written (0x0041b4ea), so `d == 0` repeats the previous word.
 */
typedef struct {
    uint32_t *word;
    size_t    n;
    size_t    consumed;   /* bytes of the input the bitstream used */
} sq_level;

/* The six prefix arms, and the two figures `tokens` reports about the
 * back-reference distance.  Filled only when a census is asked for. */
enum { SQ_ARM_ZERO = 0, SQ_ARM_REF8, SQ_ARM_REF12, SQ_ARM_LIT17,
       SQ_ARM_LIT28S, SQ_ARM_LIT32, SQ_ARM_N };

extern const char *const sq_arm_name[SQ_ARM_N];

typedef struct {
    uint64_t arm[SQ_ARM_N];
    uint64_t zero[2];     /* distance == 0, for ref8 and ref12 */
    uint32_t dmax[2];
} sq_tokens;

/* `census` may be NULL.  Returns 0 on success; the caller frees with
 * sq_level_free. */
int  sqsl_decode(const void *buf, size_t len, sq_level *out, sq_tokens *census,
                 char *err, size_t errlen);
void sq_level_free(sq_level *l);

/* ---- SQSM - the tiled stage map ---------------------------------------- */

typedef struct {
    const uint8_t *cell;     /* SQ_MAP_CELLS little-endian u16; 0 empty, n = tile n-1 */
    const uint8_t *tiles;    /* ntiles * SQ_MAP_TILE_BYTES, 256 BGR triples each */
    size_t         ntiles;
    size_t         consumed;
} sq_map;

/* Points into `buf`; it must outlive the sq_map.  Nothing is allocated, so
 * there is no free. */
int sqsm_decode(const void *buf, size_t len, sq_map *out,
                char *err, size_t errlen);

/* The cell table is read a byte at a time rather than aliased through a
 * uint16_t*: the port targets aarch64 and this parser ships. */
unsigned sq_map_cell(const sq_map *m, size_t i);

/* ---- the one documented divergence from tools/sq.py --------------------
 *
 * A back-reference whose distance reaches before the start of the output is
 * REJECTED here ("back-reference before the start of the output at word N").
 * tools/sq.py writes `out[i - d - 1]`, and Python's negative list indexing
 * silently reads from the END of what has been emitted instead - a wrong word,
 * not an error.
 *
 * This is a deliberate difference and not an oversight: the Python behaviour
 * is a bug that no valid file can reach, and reproducing it in code that SHIPS
 * would put a garbage-in-garbage-out path in the port's asset loader. The
 * whole shipped corpus is monotonic here, so the compared surface is
 * unaffected; check.sh does not mutate an SQSL file into this state, and
 * records the divergence rather than leaving it to be found.
*/

#endif /* RAL_SQ_H */
