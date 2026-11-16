/* localdat_tool.c - the CLI over localdat.c.
 *
 * Byte-identical to tools/localdat.py, which is what lets check.sh be a plain
 * diff. The 72-column word wrap in `fields` is the Python's `_wrap`, copied
 * rather than approximated, for the same reason.
 *
 *   localdat files    ROOT     every LOCALDAT under ROOT, one per line
 *   localdat fields            what the readers say each field is
 *   localdat info     FILE     decode one LOCALDAT
 *   localdat check    [ROOT]   do the readers' invariants hold corpus-wide?
 *   localdat textures [ROOT]   which TEXTINFO records does the corpus use?
*/

#include "localdat.h"
#include "corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_ROOT "original_files"

static void *slurp_or_die(const char *path, size_t *len)
{
    void *d = corpus_slurp(path, len);

    if (!d) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    return d;
}

/* localdat.py: every file whose basename lowercases to "localdat". */
static int find_localdat(const char *root, corpus_list *out)
{
    corpus_list all;
    size_t i;

    memset(out, 0, sizeof(*out));
    if (corpus_find(root, "localdat", &all) != 0) {
        return -1;
    }
    out->path = (char **)malloc((all.n ? all.n : 1) * sizeof *out->path);
    if (!out->path) {
        corpus_free(&all);
        return -1;
    }
    for (i = 0; i < all.n; i++) {
        const char *b = strrchr(all.path[i], '/');

        b = b ? b + 1 : all.path[i];
        if (strlen(b) == 8) {       /* the whole basename, not just a suffix */
            out->path[out->n++] = all.path[i];
            all.path[i] = NULL;
        }
    }
    corpus_free(&all);
    return 0;
}

static int32_t s32(uint32_t v)
{
    return (int32_t)v;
}

/* ---- files ------------------------------------------------------------- */

static int cmd_files(int argc, char **argv)
{
    corpus_list l;
    size_t i;

    if (argc < 1) {
        fprintf(stderr, "usage: localdat files ROOT\n");
        return 2;
    }
    if (find_localdat(argv[0], &l) != 0) {
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

/* localdat.py's _wrap: greedy, on whitespace, at `width` columns. */
static void wrap_print(const char *text, int width, const char *indent)
{
    char line[512];
    size_t linelen = 0;
    const char *p = text;

    for (;;) {
        const char *start;
        size_t wordlen;

        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (!*p) {
            break;
        }
        start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
            p++;
        }
        wordlen = (size_t)(p - start);
        if (linelen && linelen + 1 + wordlen > (size_t)width) {
            line[linelen] = 0;
            printf("%s%s\n", indent, line);
            linelen = 0;
        }
        if (linelen) {
            line[linelen++] = ' ';
        }
        if (linelen + wordlen < sizeof line) {
            memcpy(line + linelen, start, wordlen);
            linelen += wordlen;
        }
    }
    if (linelen) {
        line[linelen] = 0;
        printf("%s%s\n", indent, line);
    }
}

static int cmd_fields(void)
{
    size_t i;

    printf("LOCALDAT - %d bytes (0x%x), %d dwords, loaded at 0x%08x\n\n",
           LD_SIZE, LD_SIZE, LD_NDW, LD_BASE);
    for (i = 0; i < ld_nregions; i++) {
        printf("+0x%04x..+0x%04x  %-14s (%d dwords)\n",
               ld_regions[i].off, ld_regions[i].off + ld_regions[i].len - 1,
               ld_regions[i].key, ld_regions[i].len / 4);
        wrap_print(ld_regions[i].note, 72, "    ");
        printf("\n");
    }
    printf("manifest slots (from FUN_00422cee):\n");
    for (i = 0; i < ld_nmanifest; i++) {
        printf("    slot %2d  +0x%04x  kind 0x%02x\n",
               ld_manifest[i].slot,
               (unsigned)(LD_MANIFEST_BASE + ld_manifest[i].slot * LD_MANIFEST_SLOT),
               ld_manifest[i].kind);
    }
    printf("    dead (stepped over by every reader): ");
    for (i = 0; i < ld_nmanifest_dead; i++) {
        printf("%s%d", i ? ", " : "", ld_manifest_dead[i]);
    }
    printf("\n");
    return 0;
}

/* ---- info -------------------------------------------------------------- */

static int cmd_info(int argc, char **argv)
{
    void *raw;
    size_t len;
    ld_file f;
    int i;
    long nused = 0;
    uint32_t maxidx = 0;
    unsigned plates[LD_TEXINFO_N];
    size_t nplates = 0;
    long nlive = 0;

    if (argc < 1) {
        fprintf(stderr, "usage: localdat info FILE\n");
        return 2;
    }
    raw = slurp_or_die(argv[0], &len);
    ld_parse(raw, len, &f);

    if (len == LD_SIZE) {
        printf("%s  %lu bytes\n", argv[0], (unsigned long)len);
    } else {
        printf("%s  %lu bytes  (SHORT by %ld -- the game tolerates this, the "
               "build tool does not)\n",
               argv[0], (unsigned long)len, (long)(LD_SIZE - (long)len));
    }

    for (i = 0; i < LD_TEXINFO_N; i++) {
        uint32_t v = ld_dw(&f, (unsigned)(LD_TEXINFO_OFF + 4 * i));

        if (v) {
            nused++;
            if (v > maxidx) {
                maxidx = v;
            }
        }
    }
    printf("\ntexinfo set: %ld of %d slots non-zero, max index %u (of %d records)\n",
           nused, LD_TEXINFO_N, maxidx, LD_TEXTINFO_RECORDS);

    /* sorted({t >> 4 for t in used}) */
    for (i = 0; i < LD_TEXINFO_N; i++) {
        uint32_t v = ld_dw(&f, (unsigned)(LD_TEXINFO_OFF + 4 * i));
        unsigned plate;
        size_t k;
        int seen = 0;

        if (!v) {
            continue;
        }
        plate = (unsigned)(v >> 4);
        for (k = 0; k < nplates; k++) {
            if (plates[k] == plate) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            plates[nplates++] = plate;
        }
    }
    {
        size_t a, b;

        for (a = 1; a < nplates; a++) {
            unsigned v = plates[a];

            for (b = a; b > 0 && plates[b - 1] > v; b--) {
                plates[b] = plates[b - 1];
            }
            plates[b] = v;
        }
    }
    printf("  plates (index>>4): ");
    for (i = 0; i < (int)nplates; i++) {
        printf("%s%u", i ? " " : "", plates[i]);
    }
    printf("\n");

    printf("\nmanifest:\n");
    for (i = 0; i < (int)ld_nmanifest; i++) {
        uint32_t ids[4];
        int k;

        ld_slot_ids(&f, ld_manifest[i].slot, ids);
        printf("  slot %2d kind 0x%02x ", ld_manifest[i].slot, ld_manifest[i].kind);
        for (k = 0; k < 4; k++) {
            char cell[32];

            if (ids[k] == 0) {
                snprintf(cell, sizeof cell, "-");
            } else if (ids[k] == 0xffffffffu) {
                snprintf(cell, sizeof cell, "x");
            } else {
                snprintf(cell, sizeof cell, "%u", ids[k]);
            }
            printf(" %6s", cell);
        }
        printf("\n");
    }

    for (i = 0; i < LD_TEXSLOTS; i++) {
        if (ld_byte(&f, (unsigned)(LD_TEXRATE_OFF + i))) {
            nlive++;
        }
    }
    printf("\nanimated texture slots: %ld\n", nlive);
    for (i = 0; i < LD_TEXSLOTS; i++) {
        unsigned rate = ld_byte(&f, (unsigned)(LD_TEXRATE_OFF + i));
        int ph;

        if (!rate) {
            continue;
        }
        printf("  slot %3d rate %3u  phases [", i, rate);
        for (ph = 0; ph < LD_TEXANIM_PHASES; ph++) {
            printf("%s%u", ph ? ", " : "",
                   ld_byte(&f, (unsigned)(LD_TEXANIM_OFF + i * LD_TEXANIM_PHASES + ph)));
        }
        printf("]\n");
    }

    printf("\nparams +0x600:\n");
    {
        static const struct { const char *name; unsigned off; } rgb[3] = {
            { "rgb_a", 0x600 }, { "rgb_b", 0x60c }, { "rgb_c", 0x618 }
        };

        for (i = 0; i < 3; i++) {
            printf("  %-6s (%3u,%3u,%3u)\n", rgb[i].name,
                   ld_dw(&f, rgb[i].off), ld_dw(&f, rgb[i].off + 4),
                   ld_dw(&f, rgb[i].off + 8));
        }
    }
    printf("  scalars +0x624..+0x6ac: ");
    {
        unsigned o;
        int first = 1;

        for (o = 0x624; o < 0x6b0; o += 4) {
            printf("%s%d", first ? "" : " ", s32(ld_dw(&f, o)));
            first = 0;
        }
    }
    printf("\n");

    printf("\npresets: index at +0x%04x = %u\n", LD_PRESET_INDEX_OFF,
           ld_dw(&f, LD_PRESET_INDEX_OFF));
    for (i = 0; i < 6; i++) {
        int k;

        printf("  +0x%04x ", ld_preset_tables[i]);
        for (k = 0; k < LD_PRESET_N; k++) {
            printf(" %7d", s32(ld_dw(&f, ld_preset_tables[i] + 4u * (unsigned)k)));
        }
        printf("\n");
    }

    free(raw);
    return 0;
}

/* ---- check ------------------------------------------------------------- */

/* The union of TEXTINFO record indices, as a bitmap rather than a hash set:
 * 16,000 records is 2 KB and the answer has to be a COUNT of distinct values,
 * which a bitmap gives exactly. */
static unsigned char tex_union[(LD_TEXTINFO_RECORDS + 7) / 8];

static int cmd_check(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi, i;
    long bad_tex = 0, bad_dead = 0, bad_preset = 0, bad_map = 0;
    long n_preset = 0;
    long bands[4] = { 0, 0, 0, 0 };
    long man_ids = 0;
    uint32_t man_min = 0xffffffffu, man_max = 0;
    size_t sizes_len[64];
    long sizes_n[64];
    size_t nsizes = 0;
    int ok = 1;

    if (find_localdat(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    if (files.n == 0) {
        fprintf(stderr, "error: no LOCALDAT found under %s\n", root);
        return 2;
    }
    memset(tex_union, 0, sizeof tex_union);

    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        ld_file f;
        int k;

        raw = slurp_or_die(files.path[fi], &len);
        ld_parse(raw, len, &f);

        {   /* a dict keyed by length, printed in sorted key order */
            int seen = 0;

            for (i = 0; i < nsizes; i++) {
                if (sizes_len[i] == len) {
                    sizes_n[i]++;
                    seen = 1;
                    break;
                }
            }
            if (!seen && nsizes < sizeof sizes_len / sizeof sizes_len[0]) {
                sizes_len[nsizes] = len;
                sizes_n[nsizes] = 1;
                nsizes++;
            }
        }

        /* (1) every texinfo slot must name a real TEXTINFO record */
        for (k = 0; k < LD_TEXINFO_N; k++) {
            uint32_t v = ld_dw(&f, (unsigned)(LD_TEXINFO_OFF + 4 * k));

            if (v >= LD_TEXTINFO_RECORDS) {
                bad_tex++;
            } else if (v) {
                tex_union[v >> 3] |= (unsigned char)(1u << (v & 7));
            }
        }

        /* (2) the manifest's id bands, as the build tool partitions them.
         *     A measurement, not an assertion. */
        for (i = 0; i < ld_nmanifest; i++) {
            uint32_t ids[4];
            int q;

            ld_slot_ids(&f, ld_manifest[i].slot, ids);
            for (q = 0; q < 4; q++) {
                uint32_t v = ids[q];

                if (v == 0 || v == 0xffffffffu) {
                    continue;
                }
                man_ids++;
                if (v < man_min) man_min = v;
                if (v > man_max) man_max = v;
                bands[v < LD_MANIFEST_BAND_LO ? 0 :
                      v < LD_MANIFEST_BAND_HI ? 1 :
                      v < LD_MANIFEST_COUNTED ? 2 : 3]++;
            }
        }

        /* (3) the slots every reader steps over must carry nothing */
        for (i = 0; i < ld_nmanifest_dead; i++) {
            uint32_t ids[4];

            ld_slot_ids(&f, ld_manifest_dead[i], ids);
            if (ids[0] || ids[1] || ids[2] || ids[3]) {
                bad_dead++;
            }
        }

        /* (4) and (5) only apply to files that actually CONTAIN the presets
         *     region. The April 1999 rolling demo's six LOCALDAT are 0x1FCC
         *     bytes and stop exactly where `presets` begins, so ld_parse pads
         *     them with zeros and both tests would be scoring padding. */
        if (len <= LD_PRESET_REGION_OFF) {
            free(raw);
            continue;
        }
        n_preset++;
        if (ld_dw(&f, LD_PRESET_INDEX_OFF) != 0) {
            bad_preset++;
        }
        for (i = 0; i < 6; i++) {
            if (ld_dw(&f, ld_preset_map[i].src) != ld_dw(&f, ld_preset_map[i].dst)) {
                bad_map++;
            }
        }
        free(raw);
    }

    printf("population: %lu LOCALDAT under %s\n", (unsigned long)files.n, root);
    {
        size_t a, b;

        for (a = 1; a < nsizes; a++) {
            size_t vl = sizes_len[a];
            long vn = sizes_n[a];

            for (b = a; b > 0 && sizes_len[b - 1] > vl; b--) {
                sizes_len[b] = sizes_len[b - 1];
                sizes_n[b] = sizes_n[b - 1];
            }
            sizes_len[b] = vl;
            sizes_n[b] = vn;
        }
        for (a = 0; a < nsizes; a++) {
            printf("  %ld files of %lu bytes%s\n", sizes_n[a],
                   (unsigned long)sizes_len[a],
                   sizes_len[a] == LD_SIZE ? "" : "  (short)");
        }
    }
    printf("\n");
    printf("invariants the readers imply:\n");
    {
        char label[64];
        long denom;

        snprintf(label, sizeof label, "texinfo indices < %d", LD_TEXTINFO_RECORDS);
        denom = (long)files.n * LD_TEXINFO_N;
        printf("  %-34s %s  (%ld of %ld)\n", label, bad_tex ? "FAIL" : "OK  ",
               denom - bad_tex, denom);
        ok = ok && !bad_tex;

        denom = (long)files.n * (long)ld_nmanifest_dead;
        printf("  %-34s %s  (%ld of %ld)\n", "dead manifest slots all zero",
               bad_dead ? "FAIL" : "OK  ", denom - bad_dead, denom);
        ok = ok && !bad_dead;

        denom = n_preset;
        printf("  %-34s %s  (%ld of %ld)\n", "preset index == 0",
               bad_preset ? "FAIL" : "OK  ", denom - bad_preset, denom);
        ok = ok && !bad_preset;

        denom = n_preset * 6;
        printf("  %-34s %s  (%ld of %ld)\n", "preset[0] == the live field",
               bad_map ? "FAIL" : "OK  ", denom - bad_map, denom);
        ok = ok && !bad_map;
    }

    {
        long distinct = 0;
        int v;

        for (v = 0; v < LD_TEXTINFO_RECORDS; v++) {
            if (tex_union[v >> 3] & (1u << (v & 7))) {
                distinct++;
            }
        }
        printf("\nmeasurements (asserted by nothing in the image):\n");
        printf("  texinfo records referenced corpus-wide: %ld of %d (%.1f%%)\n",
               distinct, LD_TEXTINFO_RECORDS,
               100.0 * (double)distinct / LD_TEXTINFO_RECORDS);
    }
    printf("  manifest ids: %ld live, range %u..%u\n", man_ids, man_min, man_max);
    {
        char label[80];

        snprintf(label, sizeof label, " <%d", LD_MANIFEST_BAND_LO);
        printf("     %-42s %4ld\n", label, bands[0]);
        snprintf(label, sizeof label, " %d..%d", LD_MANIFEST_BAND_LO,
                 LD_MANIFEST_BAND_HI - 1);
        printf("     %-42s %4ld\n", label, bands[1]);
        snprintf(label, sizeof label, " %d..%d", LD_MANIFEST_BAND_HI,
                 LD_MANIFEST_COUNTED - 1);
        printf("     %-42s %4ld\n", label, bands[2]);
        snprintf(label, sizeof label,
                 " >=%d (above the build tool's counter)", LD_MANIFEST_COUNTED);
        printf("     %-42s %4ld\n", label, bands[3]);
    }

    corpus_free(&files);
    return ok ? 0 : 1;
}

/* ---- textures ---------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return x < y ? -1 : x > y ? 1 : 0;
}

/* len(set(v)) over an array, by sorting a copy. A bitmap would have been
 * cheaper and it is deliberately NOT used here: localdat.py's `textures` does
 * NOT drop an out-of-range index the way its `check` does, so the value space
 * is the whole of uint32 and a 16,000-bit map would silently disagree with the
 * Python on exactly the file that made the question interesting. */
static long distinct_of(uint32_t *v, size_t n)
{
    size_t i;
    long d = 0;

    qsort(v, n, sizeof *v, cmp_u32);
    for (i = 0; i < n; i++) {
        if (i == 0 || v[i] != v[i - 1]) {
            d++;
        }
    }
    return d;
}

static int cmd_textures(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    size_t rootlen = strlen(root);
    uint32_t *all = NULL, *allp = NULL;
    size_t nall = 0;

    if (find_localdat(root, &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    all = (uint32_t *)malloc((files.n + 1) * LD_TEXINFO_N * sizeof *all);
    allp = (uint32_t *)malloc((files.n + 1) * LD_TEXINFO_N * sizeof *allp);
    if (!all || !allp) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    /* Two passes, because the header lines quote the union and the per-file
     * rows come after it - exactly the order localdat.py prints. */
    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        ld_file f;
        int k;

        raw = slurp_or_die(files.path[fi], &len);
        ld_parse(raw, len, &f);
        for (k = 0; k < LD_TEXINFO_N; k++) {
            uint32_t v = ld_dw(&f, (unsigned)(LD_TEXINFO_OFF + 4 * k));

            if (v) {
                all[nall] = v;
                allp[nall] = v >> 4;
                nall++;
            }
        }
        free(raw);
    }
    printf("population: %lu LOCALDAT under %s\n", (unsigned long)files.n, root);
    printf("distinct TEXTINFO records referenced: %ld (of %d)\n",
           distinct_of(all, nall), LD_TEXTINFO_RECORDS);
    printf("distinct plates (index>>4): %ld\n\n", distinct_of(allp, nall));

    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        ld_file f;
        int k;
        size_t n = 0;
        const char *rel = files.path[fi];
        uint32_t one[LD_TEXINFO_N], onep[LD_TEXINFO_N];

        raw = slurp_or_die(files.path[fi], &len);
        ld_parse(raw, len, &f);
        for (k = 0; k < LD_TEXINFO_N; k++) {
            uint32_t v = ld_dw(&f, (unsigned)(LD_TEXINFO_OFF + 4 * k));

            if (v) {
                one[n] = v;
                onep[n] = v >> 4;
                n++;
            }
        }
        /* os.path.relpath(path, root): every path here was built as
         * root + "/" + rest, so the prefix strip is the whole of it. */
        if (strncmp(rel, root, rootlen) == 0 && rel[rootlen] == '/') {
            rel += rootlen + 1;
        }
        printf("  %-46s %3ld textures  %3ld plates\n", rel,
               distinct_of(one, n), distinct_of(onep, n));
        free(raw);
    }
    free(all);
    free(allp);
    corpus_free(&files);
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: localdat {files|fields|info|check|textures} [ARGS]\n");
        return 2;
    }
    if (strcmp(argv[1], "files") == 0)    return cmd_files(argc - 2, argv + 2);
    if (strcmp(argv[1], "fields") == 0)   return cmd_fields();
    if (strcmp(argv[1], "info") == 0)     return cmd_info(argc - 2, argv + 2);
    if (strcmp(argv[1], "check") == 0)    return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "textures") == 0) return cmd_textures(argc - 2, argv + 2);
    fprintf(stderr, "usage: localdat {files|fields|info|check|textures} [ARGS]\n");
    return 2;
}
