/* bun.h - Rally Championship `.bun` bundles, as a buffer-taking parser.
 *
 * The second format in the C toolkit, after
 * `.ro3`, and the same shape for the same reason: the port reads assets out of
 * `.bun` bundles, so nothing here opens, seeks or closes anything. Paths live
 * in corpus.c and nowhere else.
 *
 * This is a transcription of tools/bun.py, which is the instrument the format
 * was decoded with, and it is held to BYTE-IDENTICAL output by check.sh. A
 * second implementation is only worth having if it is independent evidence
 * rather than the same instrument quoted twice.
 *
 * THE CONTAINER, from sprite_blit (0x004066F9):
 *
 *     MOV EBX,dword ptr [ESI]     ; ESI = bundle base
 *     SHR EBX,0x2                 ; entry count = offset[0] / 4
 *     CMP EAX,EBX / JNC <error>   ; bounds-check the sprite index
 *     ADD ESI,dword ptr [ESI + EAX*0x4]
 *
 * There is no magic, no version and no count field: the first dword is the
 * offset table's size AND, divided by four, the entry count.
 *
 * THE ENTRY, also from sprite_blit: a per-row RLE sprite in two header forms,
 * compact (4-byte header, 1-byte units) and extended (12-byte header, 2- or
 * 4-byte units chosen by the tag). Each row is a run of
 * `count ; skip ; count pixels` packets terminated by `count == 0`.
 *
 *
*/
#ifndef RAL_BUN_H
#define RAL_BUN_H

#include <stddef.h>
#include <stdint.h>

/* The tag range that selects the extended header. Below it, the first dword is
 * really four header bytes. */
#define BUN_EXT_TAG_MIN  0xFFFFFFFCu
#define BUN_EXT_TAG_U16  0xFFFFFFFEu

#define BUN_HDR_COMPACT  4
#define BUN_HDR_EXTENDED 12

/* Tripwires, not format limits: a mis-parse should fail loudly rather than
 * allocate wildly. Same role as RO3_MAX_CORNERS. */
#define BUN_MAX_ENTRIES  (1u << 20)
#define BUN_MAX_PROBLEMS 16

enum bun_form { BUN_FORM_COMPACT = 0, BUN_FORM_EXTENDED = 1 };

extern const char *const bun_form_name[2];

/* ---- the container ----------------------------------------------------- */

typedef struct {
    uint32_t *off;      size_t n;

    /* SIGNED, and that is a transcription decision rather than a style one.
     * bun.py computes `off[i+1] - off[i]` with Python integers, so a
     * non-monotonic table yields NEGATIVE sizes that still telescope to the
     * right total and leave its closure check passing. Holding these in a
     * uint32_t wraps each one to ~4e9 and invents a second, spurious "closure
     * fails" problem on every non-monotonic file. That bug was in the first
     * version of bun.c and only the mutated-file comparison found it - the
     * real corpus is monotonic everywhere. */
    int64_t  *size;     /* n entries; the last runs to EOF */
    size_t    filelen;

    /* Every problem found, in the order bun.py finds them, because `check`
     * prints them all and a C tool that stopped at the first would produce a
     * shorter and therefore different report. */
    char   problem[BUN_MAX_PROBLEMS][160];
    size_t nproblems;
} bun_container;

/* Never fails destructively: on a badly broken file it records problems and
 * returns non-zero with `n == 0`. Returns 0 when the container closes. */
int  bun_parse(const void *buf, size_t len, bun_container *c);
void bun_container_free(bun_container *c);

/* ---- one entry --------------------------------------------------------- */

typedef struct {
    uint32_t x;         /* cursor position where the run starts */
    uint32_t first;     /* index into the sprite's flat pixel array */
    uint32_t count;
    uint32_t row;
} bun_run;

typedef struct {
    size_t   index;
    int      form;          /* enum bun_form */
    uint32_t tag;
    uint32_t xoff, yoff, w, h;

    bun_run  *runs;     size_t nruns;
    uint32_t *pixels;   size_t npixels;   /* flat, in walk order */

    size_t   consumed;  /* where the walk ended - must equal `size` */
    size_t   size;      /* the entry's bytes */
} bun_sprite;

/* Decode one entry. Returns 0 on success; on rejection returns non-zero, fills
 * `err` with bun.py's exact wording, and allocates nothing.
 *
 * The acceptance test is DELIBERATELY a falsifier: the walk must consume the
 * entry EXACTLY, and no row's cursor may pass the declared width. A decoder
 * that ran off the end or stopped early fails here rather than producing a
 * plausible wrong image, so this can only ever claim FEWER entries than really
 * decode. */
int  bun_decode_entry(const void *buf, size_t len, size_t index,
                      bun_sprite *s, char *err, size_t errlen);
void bun_sprite_free(bun_sprite *s);

/* ---- a whole file ------------------------------------------------------ */

typedef struct {
    size_t index;
    char   why[160];
} bun_reject;

typedef struct {
    bun_container c;
    bun_sprite  *sprites;   size_t nsprites;
    bun_reject  *rejects;   size_t nrejects;
} bun_file;

int  bun_decode_file(const void *buf, size_t len, bun_file *f);
void bun_file_free(bun_file *f);

#endif /* RAL_BUN_H */
