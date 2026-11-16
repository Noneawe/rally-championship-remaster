/* cdat_tool.c - the CLI over cdat.c.
 *
 * Every subcommand here exists to produce output that is BYTE-IDENTICAL to
 * tools/cdat.py's, so check.sh can be a plain `diff`. Where a format string
 * looks fussy, it is copied from the Python and the fussiness is the point -
 * including the Python repr of a list of tuples and of a dict, which two of
 * these subcommands print.
 *
 *   cdat files    ROOT     every payload under ROOT, one per line
 *   cdat fields            what the readers say each field is
 *   cdat info     FILE     decode one payload (.sq or raw)
 *   cdat check    [ROOT]   do the readers' invariants hold corpus-wide?
 *   cdat route    [ROOT]   the route-marker layer FUN_00443a33 collects
 *   cdat surfaces [ROOT]   which surface classes does the corpus use?
 *   cdat objects  [ROOT]   the class-0xff object-instance layer
 *
 * There is no `decode` and no `export`: a decompressed level is the
 * copyrighted level in another form, and derived output does not go in the
 * repo. tools/sq.py keeps both.
*/

#include "cdat.h"
#include "localdat.h"
#include "sq.h"
#include "corpus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_ROOT "original_files"

/* ---- the field map, verbatim from cdat.py's FIELDS ---------------------- */

typedef struct {
    const char *rec;
    unsigned off;
    unsigned size;
    const char *what;
} cdat_field;

static const cdat_field FIELDS[] = {
    { "zube", 0x00, 4, "cell key = cellX | cellZ<<11, cellX = (u32)x >> 21. "
      "Unique over the array in 55 of 55. 0xffffffff terminates. "
      "Readers: FUN_00420b34, FUN_00420da8, FUN_00452886, FUN_00455444" },
    { "zube", 0x04, 4, "NO reader in retail. 0 in all 91,463 records" },
    { "zube", 0x08, 4, "first `pols` index, inclusive; -1 = empty. "
      "FUN_00452b97, FUN_00440443, FUN_0045a440, FUN_0045be02" },
    { "zube", 0x0c, 4, "last `pols` index, inclusive" },
    { "zube", 0x10, 4, "first `pnts` index, inclusive; -1 = empty. FUN_00420932" },
    { "zube", 0x14, 4, "last `pnts` index, inclusive" },
    { "zube", 0x18, 4, "NO reader and NO writer in retail" },
    { "zube", 0x1c, 4, "NO reader and NO writer in retail" },
    { "pols", 0x00, 1, "class/orientation byte. bits 0-2 = one of 8 texture "
      "symmetries (FUN_0043bfee); >= 0x10 is not drawn; 0x80 = deleted; "
      "0xff = an OBJECT INSTANCE, not a polygon -- a different record; "
      "FUN_00443a33 collects the two of its 61 types that are route markers" },
    { "pols", 0x01, 1, "primary texture id -> descriptor 0x00922330 + id*0x40, "
      "which initgametexinfbuf_q builds from LOCALDAT's TEXTINFO index" },
    { "pols", 0x02, 2, "flags. 0x200 = degenerate, set at load by FUN_00420c64; "
      "0x1000 forces surface class 5 (sim_surface_solve_q 0x0043bc05)" },
    { "pols", 0x04, 4, "`pnts` index of corner 0. FUN_0044afe9 uses the whole "
      "dword as the index and its low 12 bits as a transform-cache slot" },
    { "pols", 0x08, 4, "`pnts` index of corner 1" },
    { "pols", 0x0c, 4, "`pnts` index of corner 2" },
    { "pols", 0x10, 4, "`pnts` index of corner 3 (quads only)" },
    { "pols", 0x14, 4, "four per-corner bytes, read one at a time by FUN_0044cd28" },
    { "pols", 0x18, 4, "normal 0 x -- SCRATCH, written by store_norms_q" },
    { "pols", 0x1c, 4, "normal 1 x -- SCRATCH" },
    { "pols", 0x20, 1, "secondary texture symmetry (sim_surface_solve_q)" },
    { "pols", 0x21, 1, "secondary texture id; 0 disables the second layer" },
    { "pols", 0x22, 4, "four per-corner blend bytes between the two layers" },
    { "pols", 0x26, 2, "read by init_pacenotes_q (`cmp byte[esi+0x26],2`) and by "
      "the setbestline pipeline" },
    { "pols", 0x2c, 4, "read by FUN_0044417a / FUN_004441c2 (setbestline)" },
    { "pols", 0x30, 4, "normal 0 y -- SCRATCH" },
    { "pols", 0x34, 4, "normal 1 y -- SCRATCH" },
    { "pols", 0x38, 4, "normal 0 z -- SCRATCH" },
    { "pols", 0x3c, 4, "normal 1 z -- SCRATCH" },
    /* ---- the class-0xff variant.  A DIFFERENT record: an object instance,
     * not a polygon.  Readers transcribed 2026-09-14 from
     * FUN_0044fb03 / FUN_0044fde0 / FUN_0044e389.  See
     */
    { "pols(0xff)", 0x00, 1, "class byte, always 0xff" },
    { "pols(0xff)", 0x01, 1, "OBJECT TYPE, not a texture id.  FUN_0044fb03 "
      "drops the record when > 0x64; [0x00581fc4 + type*4] is its OBJSINFO "
      "record index" },
    { "pols(0xff)", 0x04, 4, "`pnts` index of the object's anchor point.  "
      "FUN_0044fb03 bound-checks it against the pnts count at [0x00581a68]" },
    { "pols(0xff)", 0x08, 1, "yaw, x256 into a 16-bit binary angle.  "
      "FUN_00425faa NEGATES it before building the matrix" },
    { "pols(0xff)", 0x09, 1, "second yaw; used only when +0x11 != 0" },
    { "pols(0xff)", 0x0c, 1, "the MAGNITUDE of a tilt, x256 -- NOT pitch.  "
      "FUN_00425faa applies Ry(-[+0x10]) Rx(-[+0x0c]) Ry(+[+0x10]), a "
      "conjugation." },
    { "pols(0xff)", 0x10, 1, "the HEADING of the axis +0x0c tilts about, x256 "
      "-- NOT roll.  Non-zero in 1403 of 237428 records, all drawable" },
    { "pols(0xff)", 0x11, 1, "enables the second angle triple +0x09/+0x12/+0x13" },
    { "pols(0xff)", 0x12, 1, "the second triple's AXIS -- the two triples order "
      "axis and magnitude the OPPOSITE way round" },
    { "pols(0xff)", 0x13, 1, "the second triple's MAGNITUDE" },
    { "pols(0xff)", 0x14, 4, "four bytes -> [0x005e7f14..0x005e7f20]; "
      "overridden wholesale when +0x24 != 0" },
    { "pols(0xff)", 0x18, 1, "the object's SCALE in sixteenths: FUN_00425faa "
      "multiplies the matrix by [0x005e7ec8]/16 at 0x00426136.  16 -- unit -- "
      "in 215706 of 237428 records" },
    { "pols(0xff)", 0x19, 3, "read ONLY as an all-zero test, and only for "
      "types 0x3d/0x3e (FUN_0044fb03 0x0044fccb)" },
    { "pols(0xff)", 0x20, 1, "same all-zero test" },
    { "pols(0xff)", 0x22, 2, "-> [0x005e7f24], [0x005e7f28]" },
    { "pols(0xff)", 0x24, 1, "!= 0 replaces the six [0x005e7f14..] dwords from "
      "globals [0x005bbfa0]/[0x005bbfa4]/[0x005bbff4]/[0x005bbff8]" },
    { "pols(0xff)", 0x25, 1, "bits 1-2 are the STRIP SELECTOR: (b>>1)&3, "
      "0 = always, else drawn only when [0x006ce08c] == value-1 "
      "(FUN_0044e389 0x0044e3e9)" },
    { "pols(0xff)", 0x26, 1, "init_pacenotes_q's tag (`cmp byte[esi+0x26],2`). "
      "DISJOINT from +0x25's selector over the whole corpus" },
    { "pols(0xff)", 0x28, 4, "+0x28..+0x2b: the same all-zero test as +0x19" },
    { "pols(0xff)", 0x2c, 4, "-> [0x005e7ee4].  Also read by FUN_0044417a / "
      "FUN_004441c2 on the setbestline path" },
    { "pols(0xff)", 0x30, 12, "+0x30/+0x34/+0x38: a signed 16.16 position "
      "offset ADDED to pnts[+0x04].  Non-zero in 60 of 237,428 records" },
    { "pnts", 0x00, 4, "x, int32 16.16.  FUN_0044afe9 FILDs it as an integer" },
    { "pnts", 0x04, 4, "y, int32 16.16 -- the up axis" },
    { "pnts", 0x08, 4, "z, int32 16.16" }
};
static const size_t NFIELDS = sizeof FIELDS / sizeof FIELDS[0];

/* ---- plumbing ---------------------------------------------------------- */

static void *slurp_or_die(const char *path, size_t *len)
{
    void *d = corpus_slurp(path, len);

    if (!d) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    return d;
}

/* cdat.py: os.path.basename(os.path.dirname(path)) or path - the PARENT
 * directory, because all 48 levels are called cdat00c.sq. */
static void payload_name(const char *path, char *out, size_t outlen)
{
    const char *slash = NULL, *prev = NULL, *p;
    size_t n;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            prev = slash;
            slash = p;
        }
    }
    if (!slash) {
        snprintf(out, outlen, "%s", path);   /* dirname is "", so `or path` */
        return;
    }
    /* dirname = path[:slash]; its basename starts after `prev`. */
    if (slash == path) {
        snprintf(out, outlen, "%s", path);   /* dirname "/" -> basename "" */
        return;
    }
    n = (size_t)(slash - (prev ? prev + 1 : path));
    if (n + 1 > outlen) {
        n = outlen - 1;
    }
    memcpy(out, prev ? prev + 1 : path, n);
    out[n] = 0;
}

/* find_payloads: every *.sq whose magic is SQSL, plus every file named
 * `cdat00c`, sorted as one list. */
static int find_payloads(const char *root, corpus_list *out)
{
    corpus_list sq, raw;
    size_t i, cap;

    memset(out, 0, sizeof(*out));
    if (corpus_find(root, ".sq", &sq) != 0) {
        return -1;
    }
    if (corpus_find(root, "cdat00c", &raw) != 0) {
        corpus_free(&sq);
        return -1;
    }
    cap = sq.n + raw.n;
    out->path = (char **)malloc((cap ? cap : 1) * sizeof *out->path);
    if (!out->path) {
        corpus_free(&sq);
        corpus_free(&raw);
        return -1;
    }
    for (i = 0; i < sq.n; i++) {
        /* Only the magic is needed, and mapNN.sq runs to several megabytes -
         * cdat.py can afford `load(p)[:4]`, a corpus walk that slurps 48 maps
         * cannot. */
        char m[4];
        FILE *fh = fopen(sq.path[i], "rb");
        int is_level = 0;

        if (fh) {
            is_level = (fread(m, 1, 4, fh) == 4 && memcmp(m, "SQSL", 4) == 0);
            fclose(fh);
        }
        if (is_level) {
            out->path[out->n++] = sq.path[i];
            sq.path[i] = NULL;
        }
    }
    for (i = 0; i < raw.n; i++) {
        /* corpus_find matches by SUFFIX; cdat.py matches the whole basename. */
        const char *b = strrchr(raw.path[i], '/');

        b = b ? b + 1 : raw.path[i];
        if (strlen(b) == 7) {
            out->path[out->n++] = raw.path[i];
            raw.path[i] = NULL;
        }
    }
    corpus_free(&sq);
    corpus_free(&raw);
    {   /* one sorted() over the concatenation, exactly as the Python does */
        size_t a, b;

        for (a = 1; a < out->n; a++) {
            char *v = out->path[a];

            for (b = a; b > 0 && strcmp(out->path[b - 1], v) > 0; b--) {
                out->path[b] = out->path[b - 1];
            }
            out->path[b] = v;
        }
    }
    return 0;
}

/* Every caller of this walks records immediately afterwards, so the accessors'
 * precondition (cdat.h) is checked HERE, once, rather than trusted at 1.8
 * million call sites. A payload whose three header counts do not fit the
 * buffer is refused loudly.
 *
 * tools/cdat.py reaches the same state as an uncaught struct.error or
 * IndexError - a traceback. This prints one line and exits 2. The two differ
 * only on a file the corpus does not contain, and the difference is the right
 * way round: the C tool must not be the one that reads out of bounds. */
static int open_payload_or_die(const char *path, cdat_payload *p, void **raw)
{
    size_t len;
    char err[256];

    *raw = slurp_or_die(path, &len);
    if (cdat_open_buffer(*raw, len, p, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        exit(2);
    }
    if (!p->fits) {
        fprintf(stderr,
                "%s: header declares zube=%d pols=%d pnts=%d, which needs %lu "
                "bytes; the payload is %lu. Refusing to walk it.\n",
                path, p->nzube, p->npols, p->npnts,
                (unsigned long)p->end, (unsigned long)p->len);
        exit(2);
    }
    return 0;
}

/* ---- files ------------------------------------------------------------- */

static int cmd_files(int argc, char **argv)
{
    corpus_list l;
    size_t i;

    if (argc < 1) {
        fprintf(stderr, "usage: cdat files ROOT\n");
        return 2;
    }
    if (find_payloads(argv[0], &l) != 0) {
        fprintf(stderr, "cannot walk %s\n", argv[0]);
        return 2;
    }
    for (i = 0; i < l.n; i++) {
        printf("%s\n", l.path[i]);
    }
    corpus_free(&l);
    return 0;
}

/* ---- fields ------------------------------------------------------------ */

static int cmd_fields(void)
{
    const char *cur = NULL;
    size_t i;

    for (i = 0; i < NFIELDS; i++) {
        if (!cur || strcmp(FIELDS[i].rec, cur) != 0) {
            printf("\n%s record\n", FIELDS[i].rec);
            cur = FIELDS[i].rec;
        }
        printf("  +0x%02x  %u  %s\n", FIELDS[i].off, FIELDS[i].size,
               FIELDS[i].what);
    }
    printf("\n");
    return 0;
}

/* ---- info -------------------------------------------------------------- */

static int cmd_info(int argc, char **argv)
{
    static const int HDR_READ[] = { 0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 19, 27,
                                    35, 43, 51 };
    cdat_payload p;
    void *raw;
    int i;
    int32_t k;
    size_t nclass = 0;
    struct { unsigned v; long n; } klass[256];

    if (argc < 1) {
        fprintf(stderr, "usage: cdat info FILE\n");
        return 2;
    }
    open_payload_or_die(argv[0], &p, &raw);

    printf("%s  %lu bytes  zube=%d pols=%d pnts=%d  identity %s\n",
           argv[0], (unsigned long)p.len, p.nzube, p.npols, p.npnts,
           cdat_closes(&p) ? "closes" : "DOES NOT CLOSE");
    printf("\nheader words with an absolute reader in retail:\n");
    for (i = 0; i < (int)(sizeof HDR_READ / sizeof HDR_READ[0]); i++) {
        int w = HDR_READ[i];

        printf("  word[%2d] +0x%03x = %11d  0x%08x\n",
               w, w * 4, p.hdr[w], (unsigned)p.hdr[w]);
    }
    printf("\nfirst three zube:\n");
    for (k = 0; k < (p.nzube < 3 ? p.nzube : 3); k++) {
        int32_t z[8];
        uint32_t key;

        cdat_zube(&p, k, z);
        key = (uint32_t)z[0];
        printf("  [%d] key=0x%08x cell=(%4u,%4u) pols=[%d..%d] pnts=[%d..%d] "
               "+0x04=%d +0x18=0x%08x +0x1c=0x%08x\n",
               k, key, key & CDAT_CELL_MASK, (key >> 11) & CDAT_CELL_MASK,
               z[2], z[3], z[4], z[5], z[1], (unsigned)z[6], (unsigned)z[7]);
    }

    /* sorted(Counter(...).items()) - a Python list of 2-tuples. */
    {
        long count[256];
        int v;

        memset(count, 0, sizeof count);
        for (k = 0; k < p.npols; k++) {
            count[cdat_pol(&p, k)[CDAT_POL_CLASS]]++;
        }
        for (v = 0; v < 256; v++) {
            if (count[v]) {
                klass[nclass].v = (unsigned)v;
                klass[nclass].n = count[v];
                nclass++;
            }
        }
    }
    printf("\npols class byte +0x00: [");
    {
        size_t j;

        for (j = 0; j < nclass; j++) {
            printf("%s(%u, %ld)", j ? ", " : "", klass[j].v, klass[j].n);
        }
    }
    printf("]\n");

    cdat_free(&p);
    free(raw);
    return 0;
}

/* ---- check ------------------------------------------------------------- */

typedef struct {
    unsigned char *bits;
    size_t         n;
} bitset;

static void bitset_init(bitset *b, size_t n)
{
    b->n = n;
    b->bits = (unsigned char *)calloc((n + 7) / 8 + 1, 1);
}

static void bitset_set_range(bitset *b, int32_t lo, int32_t hi)
{
    int32_t i;

    for (i = lo; i <= hi; i++) {
        if ((size_t)i < b->n) {
            b->bits[(size_t)i >> 3] |= (unsigned char)(1u << ((size_t)i & 7));
        }
    }
}

static size_t bitset_count(const bitset *b)
{
    size_t i, n = 0;

    for (i = 0; i < b->n; i++) {
        if (b->bits[i >> 3] & (1u << (i & 7))) {
            n++;
        }
    }
    return n;
}

static void bitset_free(bitset *b)
{
    free(b->bits);
    b->bits = NULL;
}

typedef struct {
    char name[128];
    long off;
} offcell;

static int cmd_check(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;

    long key_total = 0, key_unique = 0;
    long f04_total = 0, f04_zero = 0;
    long pnt_total = 0, pnt_in_cell = 0;
    long vtx_total = 0, vtx_in_range = 0;
    long poly_drawable = 0, poly_scratch_zero = 0;
    long baked_drawable = 0;
    long norm_total = 0, norm_nan = 0, norm_nan_degenerate = 0;
    long norm_unit = 0, norm_finite_degenerate = 0;

    long n_files = 0, n_key_unique = 0, n_term_last = 0, n_ranges_sane = 0;
    long n_pols_part = 0, n_pnts_part = 0;
    long n_scratch_zero = 0, n_scratch_baked = 0, n_scratch_mixed = 0;

    offcell *offs = NULL;
    size_t noffs = 0;
    char **baked = NULL;
    size_t nbaked = 0;

    if (find_payloads(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    offs = (offcell *)calloc(files.n ? files.n : 1, sizeof *offs);
    baked = (char **)calloc(files.n ? files.n : 1, sizeof *baked);
    if (!offs || !baked) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (fi = 0; fi < files.n; fi++) {
        cdat_payload p;
        void *raw;
        char name[128];
        int32_t i;
        bitset pol_seen, pnt_seen;
        long pol_sz = 0, pnt_sz = 0;
        int ranges_sane = 1;
        long off_here = 0;
        long drawable = 0, zeroed = 0;
        uint32_t *keys;
        int unique;

        open_payload_or_die(files.path[fi], &p, &raw);
        payload_name(files.path[fi], name, sizeof name);
        if (!cdat_closes(&p)) {
            printf("BAD identity: %s\n", files.path[fi]);
            cdat_free(&p);
            free(raw);
            continue;
        }
        n_files++;

        /* 1 -- the key is unique over the array (FUN_00420da8 stops at the
         *      first match, so a duplicate would be unreachable) */
        keys = (uint32_t *)malloc((size_t)(p.nzube ? p.nzube : 1) * sizeof *keys);
        if (!keys) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        for (i = 0; i < p.nzube; i++) {
            keys[i] = cdat_zube_key(&p, i);
        }
        key_total += p.nzube;
        {
            /* len(set(keys)): sort a copy and count distinct. */
            uint32_t *s = (uint32_t *)malloc((size_t)(p.nzube ? p.nzube : 1) * sizeof *s);
            size_t a, b, distinct = 0;

            memcpy(s, keys, (size_t)p.nzube * sizeof *s);
            for (a = 1; a < (size_t)p.nzube; a++) {
                uint32_t v = s[a];

                for (b = a; b > 0 && s[b - 1] > v; b--) {
                    s[b] = s[b - 1];
                }
                s[b] = v;
            }
            for (a = 0; a < (size_t)p.nzube; a++) {
                if (a == 0 || s[a] != s[a - 1]) {
                    distinct++;
                }
            }
            key_unique += (long)distinct;
            unique = (distinct == (size_t)p.nzube);
            free(s);
        }
        n_key_unique += unique;

        /* 2 -- exactly one terminator, and it is last */
        {
            long nterm = 0;
            int last_only = 1;

            for (i = 0; i < p.nzube; i++) {
                if (keys[i] == CDAT_ZUBE_KEY_END) {
                    nterm++;
                    if (i != p.nzube - 1) {
                        last_only = 0;
                    }
                }
            }
            n_term_last += (nterm == 1 && last_only);
        }

        /* 3 -- the ranges partition `pols` and `pnts` exactly */
        bitset_init(&pol_seen, (size_t)(p.npols > 0 ? p.npols : 0));
        bitset_init(&pnt_seen, (size_t)(p.npnts > 0 ? p.npnts : 0));
        for (i = 0; i < p.nzube; i++) {
            int32_t z[8];

            cdat_zube(&p, i, z);
            if (z[2] != -1) {
                if (!(0 <= z[2] && z[2] <= z[3] && z[3] < p.npols)) {
                    ranges_sane = 0;
                } else {
                    pol_sz += z[3] - z[2] + 1;
                    bitset_set_range(&pol_seen, z[2], z[3]);
                }
            }
            if (z[4] != -1) {
                if (!(0 <= z[4] && z[4] <= z[5] && z[5] < p.npnts)) {
                    ranges_sane = 0;
                } else {
                    pnt_sz += z[5] - z[4] + 1;
                    bitset_set_range(&pnt_seen, z[4], z[5]);
                }
            }
            f04_total++;
            f04_zero += (z[1] == 0);
        }
        n_ranges_sane += ranges_sane;
        n_pols_part += (pol_sz == p.npols && (size_t)p.npols == bitset_count(&pol_seen));
        n_pnts_part += (pnt_sz == p.npnts && (size_t)p.npnts == bitset_count(&pnt_seen));
        bitset_free(&pol_seen);
        bitset_free(&pnt_seen);

        /* 4 -- every point of a cell lies in the 32x32 cell the key names */
        for (i = 0; i < p.nzube; i++) {
            int32_t z[8], j;
            uint32_t cx, cz;

            cdat_zube(&p, i, z);
            if (keys[i] == CDAT_ZUBE_KEY_END || z[4] == -1) {
                continue;
            }
            /* The range is used as an INDEX here, so it is bounded here.
             * Invariant 3 above only MEASURES whether the ranges are sane; on
             * the corpus they are, in 55 of 55, which is exactly why this was
             * missing and why nothing noticed. cdat.py gets an exception out
             * of struct.unpack_from; C gets a wild read, and did.  Refusing
             * loudly keeps the two the right way round. */
            if (!(z[4] >= 0 && z[4] <= z[5] && z[5] < p.npnts)) {
                fprintf(stderr,
                        "%s: zube[%d] names points [%d..%d] of %d. Refusing to "
                        "walk it.\n", files.path[fi], i, z[4], z[5], p.npnts);
                exit(2);
            }
            cx = keys[i] & CDAT_CELL_MASK;
            cz = (keys[i] >> 11) & CDAT_CELL_MASK;
            for (j = z[4]; j <= z[5]; j++) {
                int32_t v[3];

                cdat_pnt(&p, j, v);
                pnt_total++;
                if (cdat_cell_of(v[0]) == cx && cdat_cell_of(v[2]) == cz) {
                    pnt_in_cell++;
                } else {
                    off_here++;
                }
            }
        }
        if (off_here) {
            snprintf(offs[noffs].name, sizeof offs[noffs].name, "%s", name);
            offs[noffs].off = off_here;
            noffs++;
        }

        /* 5 -- the six normal dwords are zero in the file for every drawable
         *      polygon, and 6 -- every vertex index is in range */
        for (i = 0; i < p.npols; i++) {
            const uint8_t *rec = cdat_pol(&p, i);
            int all_zero = 1, any = 0, c;

            if (rec[CDAT_POL_CLASS] >= CDAT_POLY_DRAWABLE_MAX) {
                continue;
            }
            drawable++;
            for (c = 0; c < 6; c++) {
                if (cdat_u32(rec, cdat_normal_scratch[c])) {
                    all_zero = 0;
                    any = 1;
                }
            }
            zeroed += all_zero;
            for (c = 0; c < 4; c++) {
                vtx_total++;
                vtx_in_range += (cdat_u32(rec, CDAT_POL_CORNER0 + 4u * (unsigned)c)
                                 < (uint32_t)p.npnts);
            }

            /* 7 -- where a file DOES ship the normals, are they unit floats,
             *      and is every NaN a triangle with a repeated corner?  This
             *      is the one check that owes nothing to the disassembly. */
            if (any) {
                int tri;

                for (tri = 0; tri < 2; tri++) {
                    double n3[3];
                    uint32_t corner[3];
                    int degenerate, q;

                    for (q = 0; q < 3; q++) {
                        n3[q] = (double)cdat_f32(rec, cdat_normal_xyz[tri][q]);
                        corner[q] = cdat_u32(rec, cdat_triangle_corners[tri][q]);
                    }
                    degenerate = (corner[0] == corner[1] || corner[0] == corner[2] ||
                                  corner[1] == corner[2]);
                    norm_total++;
                    if (isnan(n3[0]) || isnan(n3[1]) || isnan(n3[2])) {
                        norm_nan++;
                        norm_nan_degenerate += degenerate;
                    } else {
                        double mag = sqrt(n3[0] * n3[0] + n3[1] * n3[1] +
                                          n3[2] * n3[2]);

                        norm_unit += (fabs(mag - 1.0) < 1e-3);
                        norm_finite_degenerate += degenerate;
                    }
                }
            }
        }
        poly_drawable += drawable;
        poly_scratch_zero += zeroed;
        if (zeroed == drawable) {
            n_scratch_zero++;
        } else if (zeroed == 0) {
            n_scratch_baked++;
            baked_drawable += drawable;
            baked[nbaked] = (char *)malloc(strlen(name) + 1);
            strcpy(baked[nbaked++], name);
        } else {
            /* cdat.py adds to `baked_drawable` in the all-baked arm ONLY; a
             * mixed file is named but its polygons are not counted. */
            n_scratch_mixed++;
            baked[nbaked] = (char *)malloc(strlen(name) + 16);
            sprintf(baked[nbaked++], "%s (MIXED)", name);
        }

        free(keys);
        cdat_free(&p);
        free(raw);
    }

    printf("population: %ld payloads under %s -- every `cdat00c.sq` plus every "
           "raw `CDAT00C`,\nso the demo's SRCLVLS copy and the early demo's six "
           "levels are all in it\n\n", n_files, root);
    printf("1  zube key unique over the array     %7ld of %7ld records, %ld of %ld files\n",
           key_unique, key_total, n_key_unique, n_files);
    printf("2  one 0xffffffff key, and it is last %31ld of %ld files\n",
           n_term_last, n_files);
    printf("3a zube +0x04 is zero                 %7ld of %7ld records\n",
           f04_zero, f04_total);
    printf("3b pols ranges partition the array    %31ld of %ld files\n",
           n_pols_part, n_files);
    printf("3c pnts ranges partition the array    %31ld of %ld files\n",
           n_pnts_part, n_files);
    printf("4  points lie in the cell their key names  %7ld of %7ld points\n",
           pnt_in_cell, pnt_total);
    if (noffs) {
        size_t i;

        printf("     off-cell points occur in %lu file(s): ", (unsigned long)noffs);
        for (i = 0; i < noffs; i++) {
            printf("%s%s %ld", i ? ", " : "", offs[i].name, offs[i].off);
        }
        printf("\n");
    }
    printf("5  the six normal dwords are zero     %7ld of %7ld drawable polygons,"
           " all-zero in %ld of %ld files\n",
           poly_scratch_zero, poly_drawable, n_scratch_zero, n_files);
    if (nbaked) {
        size_t i;

        printf("     %ld file(s) ship the normals baked in instead (%ld polygons): ",
               n_scratch_baked + n_scratch_mixed, baked_drawable);
        for (i = 0; i < nbaked; i++) {
            printf("%s%s", i ? ", " : "", baked[i]);
        }
        printf("\n");
    }
    printf("6  vertex index < pnts count          %7ld of %7ld indices\n",
           vtx_in_range, vtx_total);
    if (norm_total) {
        printf("7  where a file ships the normals, they read back as unit floats:\n");
        printf("     |n| within 1e-3 of 1.0            %7ld of %7ld triangles\n",
               norm_unit, norm_total - norm_nan);
        printf("     NaN, with a repeated corner       %7ld of %7ld NaN\n",
               norm_nan_degenerate, norm_nan);
        printf("     finite, but with a repeated corner    %7ld  (expected 0)\n",
               norm_finite_degenerate);
    }

    {
        size_t i;

        for (i = 0; i < nbaked; i++) {
            free(baked[i]);
        }
    }
    free(baked);
    free(offs);
    corpus_free(&files);
    (void)n_ranges_sane;
    return 0;
}

/* ---- route ------------------------------------------------------------- */

static int cmd_route(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    long n = 0, paired = 0, two_strip = 0;
    char **one_strip = NULL, **odd = NULL;
    size_t none = 0, nodd = 0;

    if (find_payloads(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    one_strip = (char **)calloc(files.n ? files.n : 1, sizeof *one_strip);
    odd = (char **)calloc(files.n ? files.n : 1, sizeof *odd);

    printf("%-10s %9s %9s %8s %8s   %s\n", "level", "pols", "type0xff",
           "tex62", "tex61", "pols+0x26 over the tex-62 markers");
    for (fi = 0; fi < files.n; fi++) {
        cdat_payload p;
        void *raw;
        char name[128];
        int32_t i;
        long route = 0, a = 0, b = 0;
        long tags[256];
        int v;
        int first = 1;

        open_payload_or_die(files.path[fi], &p, &raw);
        payload_name(files.path[fi], name, sizeof name);
        n++;
        memset(tags, 0, sizeof tags);
        for (i = 0; i < p.npols; i++) {
            const uint8_t *rec = cdat_pol(&p, i);

            if (rec[CDAT_POL_CLASS] != CDAT_POLY_ROUTE) {
                continue;
            }
            route++;
            if (rec[CDAT_POL_TEX0] == CDAT_ROUTE_TEX_A) {
                a++;
                tags[rec[CDAT_POL_ROUTE_TAG]]++;
            } else if (rec[CDAT_POL_TEX0] == CDAT_ROUTE_TEX_B) {
                b++;
            }
        }
        if (b) {
            two_strip++;
            if (labs(a - b) <= 1) {
                paired++;
            } else {
                odd[nodd] = (char *)malloc(strlen(name) + 48);
                sprintf(odd[nodd++], "%s (%ld vs %ld)", name, a, b);
            }
        } else if (a) {
            one_strip[none] = (char *)malloc(strlen(name) + 1);
            strcpy(one_strip[none++], name);
        }
        printf("%-10s %9d %9ld %8ld %8ld   {", name, p.npols, route, a, b);
        for (v = 0; v < 256; v++) {
            if (tags[v]) {
                printf("%s%d: %ld", first ? "" : ", ", v, tags[v]);
                first = 0;
            }
        }
        printf("}\n");
        cdat_free(&p);
        free(raw);
    }
    printf("\npopulation: %ld payloads under %s\n", n, root);
    printf("payloads carrying both strips: %ld.  Of those, the two counts differ\n"
           "by at most 1 in %ld.\n", two_strip, paired);
    if (nodd) {
        size_t i;

        printf("  exception(s): ");
        for (i = 0; i < nodd; i++) {
            printf("%s%s", i ? ", " : "", odd[i]);
            free(odd[i]);
        }
        printf("\n");
    }
    if (none) {
        size_t i;

        printf("texture 62 only, no texture 61 at all: ");
        for (i = 0; i < none; i++) {
            printf("%s%s", i ? ", " : "", one_strip[i]);
            free(one_strip[i]);
        }
        printf("\n");
    }
    free(one_strip);
    free(odd);
    corpus_free(&files);
    return 0;
}

/* ---- surfaces ---------------------------------------------------------- */

/* The class-0xff object-instance layer.  FUN_0044e389's gate set: type 0x38,
 * or 0x3a <= type < 0x40, needs [0x005bbfec] != 0 -- and NOTHING in RAL.EXE
 * writes that global. */
static int obj_debug_type(unsigned t)
{
    return t == 0x38 || (t >= 0x3a && t < 0x40);
}

static int cmd_objects(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    long n = 0, total = 0, bad_pnt = 0, bad_hi = 0, overlap = 0;
    long types[256], sel[4];
    long debug = 0, over = 0, drawable;
    int v;

    if (find_payloads(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    memset(types, 0, sizeof types);
    memset(sel, 0, sizeof sel);

    for (fi = 0; fi < files.n; fi++) {
        cdat_payload p;
        void *raw;
        int32_t i;

        open_payload_or_die(files.path[fi], &p, &raw);
        n++;
        for (i = 0; i < p.npols; i++) {
            const uint8_t *rec = cdat_pol(&p, i);
            uint32_t idx;
            unsigned s2;

            if (rec[CDAT_POL_CLASS] != CDAT_POLY_ROUTE) {
                continue;
            }
            total++;
            types[rec[CDAT_OBJ_TYPE]]++;
            idx = (uint32_t)rec[CDAT_OBJ_ANCHOR]
                | ((uint32_t)rec[CDAT_OBJ_ANCHOR + 1] << 8)
                | ((uint32_t)rec[CDAT_OBJ_ANCHOR + 2] << 16)
                | ((uint32_t)rec[CDAT_OBJ_ANCHOR + 3] << 24);
            if (idx >= (uint32_t)p.npnts) {
                bad_pnt++;
            }
            if (rec[0x03] || rec[0x07]) {
                bad_hi++;
            }
            s2 = (unsigned)((rec[CDAT_OBJ_SELECTOR] >> 1) & 3);
            sel[s2]++;
            if (s2 && rec[CDAT_POL_ROUTE_TAG]) {
                overlap++;
            }
        }
        cdat_free(&p);
        free(raw);
    }

    for (v = 0; v < 256; v++) {
        if (!types[v]) {
            continue;
        }
        if (obj_debug_type((unsigned)v)) {
            debug += types[v];
        }
        if (v > CDAT_OBJ_TYPE_MAX) {
            over += types[v];
        }
    }
    drawable = total - debug - over;

    printf("population: %ld payloads under %s, %ld class-0xff records\n",
           n, root, total);
    printf("\n");
    printf("  FUN_0044fb03's bound check, +0x04 >= the pnts count   %8ld  "
           "(expected 0)\n", bad_pnt);
    printf("  +0x03 or +0x07 non-zero (the index is 24-bit)         %8ld  "
           "(expected 0)\n", bad_hi);
    printf("  +0x25's selector and +0x26's tag both non-zero        %8ld  "
           "(expected 0)\n", overlap);
    printf("\n");
    printf("  type > 0x64, dropped by FUN_0044fb03                  %8ld  %6.2f%%\n",
           over, 100.0 * (double)over / (double)total);
    printf("  type in {0x38, 0x3a..0x3f}, gated on [0x005bbfec]     %8ld  %6.2f%%\n",
           debug, 100.0 * (double)debug / (double)total);
    printf("  everything else -- reaches the model path             %8ld  %6.2f%%\n",
           drawable, 100.0 * (double)drawable / (double)total);
    printf("\n");
    printf("  +0x25 strip selector (b>>1)&3: {");
    {
        int first = 1;

        for (v = 0; v < 4; v++) {
            if (sel[v]) {
                printf("%s%d: %ld", first ? "" : ", ", v, sel[v]);
                first = 0;
            }
        }
    }
    printf("}\n");
    printf("\n");
    printf("type histogram (+0x01):\n");
    for (v = 0; v < 256; v++) {
        const char *mark;

        if (!types[v]) {
            continue;
        }
        mark = obj_debug_type((unsigned)v) ? "  debug"
             : (v > CDAT_OBJ_TYPE_MAX ? "  DROPPED" : "");
        printf("  %3d 0x%02x  %8ld  %6.2f%%%s\n", v, v, types[v],
               100.0 * (double)types[v] / (double)total, mark);
    }
    corpus_free(&files);
    return 0;
}

static int cmd_surfaces(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    char ti_path[1024];
    uint8_t *ti;
    size_t ti_len, fi;
    long prim[256], sec[256];
    long nlevels = 0;
    int v, hi = 0;

    snprintf(ti_path, sizeof ti_path, "%s/files/a/TEXTINFO", root);
    ti = (uint8_t *)corpus_slurp(ti_path, &ti_len);
    if (!ti) {
        printf("no %s\n", ti_path);
        return 0;
    }
    printf("TEXTINFO: %lu bytes = %lu records of 0x80\n",
           (unsigned long)ti_len, (unsigned long)(ti_len / 0x80));

    memset(prim, 0, sizeof prim);
    memset(sec, 0, sizeof sec);
    if (find_payloads(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        free(ti);
        return 2;
    }
    for (fi = 0; fi < files.n; fi++) {
        char ld_path[1024];
        const char *slash;
        size_t dirlen, ld_len;
        void *ld_raw;
        ld_file ld;
        cdat_payload p;
        void *raw;
        int32_t i;

        slash = strrchr(files.path[fi], '/');
        dirlen = slash ? (size_t)(slash - files.path[fi]) : 1;
        if (dirlen + 16 > sizeof ld_path) {
            continue;
        }
        memcpy(ld_path, files.path[fi], dirlen);
        strcpy(ld_path + dirlen, "/LOCALDAT");
        ld_raw = corpus_slurp(ld_path, &ld_len);
        if (!ld_raw) {
            strcpy(ld_path + dirlen, "/localdat");
            ld_raw = corpus_slurp(ld_path, &ld_len);
        }
        if (!ld_raw) {
            continue;
        }
        ld_parse(ld_raw, ld_len, &ld);

        open_payload_or_die(files.path[fi], &p, &raw);
        nlevels++;
        for (i = 0; i < p.npols; i++) {
            const uint8_t *rec = cdat_pol(&p, i);
            int layer;

            if (rec[CDAT_POL_CLASS] >= CDAT_POLY_DRAWABLE_MAX) {
                continue;
            }
            for (layer = 0; layer < 2; layer++) {
                unsigned id = layer == 0 ? rec[CDAT_POL_TEX0] : rec[CDAT_POL_TEX1];
                uint32_t r = ld_dw(&ld, (unsigned)(LD_TEXINFO_OFF + 4 * id));
                long *counter = layer == 0 ? prim : sec;
                int q;

                if (((size_t)r + 1) * 0x80 > ti_len) {
                    continue;
                }
                for (q = 0; q < 4; q++) {
                    counter[ti[(size_t)r * 0x80 + 0x20 + (size_t)q]]++;
                }
            }
        }
        cdat_free(&p);
        free(raw);
        free(ld_raw);
    }
    printf("population: %ld levels with both a payload and a LOCALDAT\n\n", nlevels);
    printf("surface class, primary layer (pols+0x01):\n");
    for (v = 0; v < 256; v++) {
        if (prim[v]) {
            printf("   %2d  %9ld\n", v, prim[v]);
            if (v > hi) hi = v;
        }
    }
    printf("surface class, secondary layer (pols+0x21):\n");
    for (v = 0; v < 256; v++) {
        if (sec[v]) {
            printf("   %2d  %9ld\n", v, sec[v]);
            if (v > hi) hi = v;
        }
    }
    printf("\nhighest class seen: %d.  The grip table at 0x005C66B0 is 16 "
           "records of 0x30.\n", hi);

    corpus_free(&files);
    free(ti);
    return 0;
}

/* ---- main -------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: cdat {files|fields|info|check|route|surfaces} [ARGS]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "files") == 0)    return cmd_files(argc - 2, argv + 2);
    if (strcmp(argv[1], "fields") == 0)   return cmd_fields();
    if (strcmp(argv[1], "info") == 0)     return cmd_info(argc - 2, argv + 2);
    if (strcmp(argv[1], "check") == 0)    return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "route") == 0)    return cmd_route(argc - 2, argv + 2);
    if (strcmp(argv[1], "surfaces") == 0) return cmd_surfaces(argc - 2, argv + 2);
    if (strcmp(argv[1], "objects") == 0)  return cmd_objects(argc - 2, argv + 2);
    usage();
    return 2;
}
