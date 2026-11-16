/* sq_tool.c - the CLI over sq.c.
 *
 * Byte-identical to tools/sq.py, so check.sh can be a plain diff.
 *
 *   sq files    ROOT     every *.sq under ROOT, one per line
 *   sq info     FILE     header + what the reader would do
 *   sq check    [ROOT]   does every file close exactly at EOF?
 *   sq sections [ROOT]   does the payload's section identity close?
 *   sq tokens   [ROOT]   which prefix arms does the corpus exercise?
 *
 * `decode`, `export` and `render` are deliberately NOT here. The first two
 * write a decompressed level, which is the copyrighted level in another form
 * and does not go in the repo; `render` needs a PNG writer, i.e.
 * zlib, and that is not code that ships in the port. tools/sq.py keeps all
 * three. Said explicitly rather than left as a gap, because a missing
 * subcommand looks like an oversight.
*/

#include "sq.h"
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

static int32_t s32(uint32_t w)
{
    return (int32_t)w;
}

/* ---- files ------------------------------------------------------------- */

static int cmd_files(int argc, char **argv)
{
    corpus_list l;
    size_t i;

    if (argc < 1) {
        fprintf(stderr, "usage: sq files ROOT\n");
        return 2;
    }
    if (corpus_find(argv[0], ".sq", &l) != 0) {
        fprintf(stderr, "cannot walk %s\n", argv[0]);
        return 2;
    }
    for (i = 0; i < l.n; i++) {
        printf("%s\n", l.path[i]);
    }
    corpus_free(&l);
    return 0;
}

/* ---- info -------------------------------------------------------------- */

static int cmd_info(int argc, char **argv)
{
    void *raw;
    size_t len;
    sq_header h;
    char err[256];

    if (argc < 1) {
        fprintf(stderr, "usage: sq info FILE\n");
        return 2;
    }
    raw = slurp_or_die(argv[0], &len);
    if (sq_read_header(raw, len, &h, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    printf("%s\n", argv[0]);
    printf("  magic          %.4s\n", (const char *)raw);
    printf("  +0x04          %u\n", h.pad);
    printf("  version        0x%08x\n", h.version);
    printf("  +0x0c          %u\n", h.count);
    printf("  file           %lu bytes\n", (unsigned long)len);

    if (h.magic == SQ_MAGIC_MAP) {
        sq_map m;
        size_t end = SQ_HDR_SIZE + SQ_MAP_INDEX_BYTES +
                     (size_t)h.count * SQ_MAP_TILE_BYTES;
        long used = 0;
        unsigned hi = 0;
        size_t i;

        if (sqsm_decode(raw, len, &m, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 2;
        }
        for (i = 0; i < SQ_MAP_CELLS; i++) {
            unsigned c = sq_map_cell(&m, i);

            if (c) {
                used++;
            }
            if (c > hi) {
                hi = c;
            }
        }
        printf("  -> %u tiles of %dx%d, %d-byte index grid of %dx%d cells\n",
               h.count, SQ_MAP_TILE, SQ_MAP_TILE, SQ_MAP_INDEX_BYTES,
               SQ_MAP_GRID, SQ_MAP_GRID);
        printf("  -> %ld/%d cells occupied (%.1f%%), image %dx%d\n",
               used, SQ_MAP_CELLS, 100.0 * (double)used / SQ_MAP_CELLS,
               SQ_MAP_PIXELS, SQ_MAP_PIXELS);
        printf("  -> 16 + %d + %u*%d = %lu, file %lu, slack %ld\n",
               SQ_MAP_INDEX_BYTES, h.count, SQ_MAP_TILE_BYTES,
               (unsigned long)end, (unsigned long)len, (long)len - (long)end);
        printf("  -> highest cell index %u, tile count %u %s\n",
               hi, h.count, hi <= h.count ? "(in range)" : "(OUT OF RANGE)");
    } else {
        sq_level lv;
        size_t i, n;

        if (sqsl_decode(raw, len, &lv, NULL, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 2;
        }
        printf("  -> %u words = %u bytes, ratio %.2fx\n",
               h.count, h.count * 4, h.count * 4.0 / (double)len);
        printf("  -> consumed %lu of %lu bytes, slack %ld\n",
               (unsigned long)lv.consumed, (unsigned long)len,
               (long)len - (long)lv.consumed);
        printf("  -> payload header, first 12 of its 64 dwords:\n");
        printf("     ");
        n = lv.n < 12 ? lv.n : 12;
        for (i = 0; i < n; i++) {
            printf("%s%d", i ? " " : "", s32(lv.word[i]));
        }
        printf("\n");
        sq_level_free(&lv);
    }
    free(raw);
    return 0;
}

/* ---- check ------------------------------------------------------------- */

static int cmd_check(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    long seen[2] = { 0, 0 }, exact[2] = { 0, 0 };   /* [0] SQSL, [1] SQSM */
    struct { char path[1024]; long slack; int bad; } *resid;
    size_t nresid = 0;

    if (corpus_find(root, ".sq", &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    resid = malloc((files.n ? files.n : 1) * sizeof *resid);
    if (!resid) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        sq_header h;
        char err[256];
        char extra[256];
        size_t consumed = 0;
        long slack;
        int bad = 0, slot;

        raw = slurp_or_die(files.path[fi], &len);
        if (sq_read_header(raw, len, &h, err, sizeof err) != 0) {
            printf("BAD  %-52s %s\n", files.path[fi], err);
            free(raw);
            continue;
        }
        if (h.magic == SQ_MAGIC_MAP) {
            sq_map m;
            long used = 0;
            unsigned hi = 0;
            size_t i;

            if (sqsm_decode(raw, len, &m, err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                return 2;
            }
            for (i = 0; i < SQ_MAP_CELLS; i++) {
                unsigned c = sq_map_cell(&m, i);

                if (c) used++;
                if (c > hi) hi = c;
            }
            snprintf(extra, sizeof extra, "%u tiles, %ld/%d cells",
                     h.count, used, SQ_MAP_CELLS);
            bad = hi > h.count;
            consumed = m.consumed;
            slot = 1;
        } else {
            sq_level lv;
            size_t i, n;
            int off;

            if (sqsl_decode(raw, len, &lv, NULL, err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                return 2;
            }
            /* The payload's own header is 64 dwords (the loader block-copies
             * 0x40 of them to 0x00581a5c at 0x004438c2). Its [2] and [3] are
             * the two counts the game reads most; [3] > [2] holds corpus-wide
             * bar one file, so it is reported, not asserted. */
            off = snprintf(extra, sizeof extra, "%u words -> %u bytes, hdr[0..3] = ",
                           h.count, h.count * 4);
            n = lv.n < 4 ? lv.n : 4;
            for (i = 0; i < n && off > 0 && (size_t)off < sizeof extra; i++) {
                off += snprintf(extra + off, sizeof extra - (size_t)off,
                                "%s%d", i ? " " : "", s32(lv.word[i]));
            }
            consumed = lv.consumed;
            sq_level_free(&lv);
            slot = 0;
        }
        slack = (long)len - (long)consumed;
        seen[slot]++;
        if (slack == 0 && !bad) {
            exact[slot]++;
        } else {
            snprintf(resid[nresid].path, sizeof resid[nresid].path, "%s",
                     files.path[fi]);
            resid[nresid].slack = slack;
            resid[nresid].bad = bad;
            nresid++;
        }
        printf("%s %-52s %s slack=%ld\n", (slack == 0 && !bad) ? "ok  " : "NOTE",
               files.path[fi], extra, slack);
        free(raw);
    }
    printf("\n");
    if (seen[0]) {
        printf("SQSL: %ld of %ld close exactly at EOF\n", exact[0], seen[0]);
    }
    if (seen[1]) {
        printf("SQSM: %ld of %ld close exactly at EOF\n", exact[1], seen[1]);
    }
    {
        size_t i;

        for (i = 0; i < nresid; i++) {
            printf("  residue: %s slack=%ld payload_ok=%s\n", resid[i].path,
                   resid[i].slack, resid[i].bad ? "False" : "True");
        }
    }
    printf("corpus: %lu file(s) under %s\n", (unsigned long)files.n, root);
    free(resid);
    corpus_free(&files);
    return 0;
}

/* ---- sections ---------------------------------------------------------- */

static int cmd_sections(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    long ok = 0, bad = 0;
    char rawpath[1024];
    void *rawbuf;
    size_t rawlen;

    if (corpus_find(root, ".sq", &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        sq_level lv;
        char err[256];
        long n, calc;

        raw = slurp_or_die(files.path[fi], &len);
        if (len < 4 || memcmp(raw, "SQSL", 4) != 0) {
            free(raw);
            continue;
        }
        if (sqsl_decode(raw, len, &lv, NULL, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 2;
        }
        n = (long)lv.n * 4;
        calc = SQ_PAYLOAD_HDR + (long)s32(lv.word[1]) * SQ_ZUBE_SIZE +
               (long)s32(lv.word[2]) * SQ_POLS_SIZE +
               (long)s32(lv.word[3]) * SQ_PNTS_SIZE;
        ok += (calc == n);
        bad += (calc != n);
        printf("%s %-52s zube=%-6d pols=%-7d pnts=%-7d calc=%-9ld payload=%-9ld\n",
               calc == n ? "ok  " : "BAD ", files.path[fi],
               s32(lv.word[1]), s32(lv.word[2]), s32(lv.word[3]), calc, n);
        sq_level_free(&lv);
        free(raw);
    }

    /* The demo's RAW SRCLVLS copy satisfies the same identity without going
     * through the decoder at all - the one figure in that does
     * not rest on this tool being right. */
    snprintf(rawpath, sizeof rawpath, "%s/demo/SRCLVLS/LEVEL71/CDAT00C", root);
    rawbuf = corpus_slurp(rawpath, &rawlen);
    if (rawbuf) {
        const unsigned char *b = (const unsigned char *)rawbuf;
        int32_t h[4];
        long calc;
        int i;

        for (i = 0; i < 4; i++) {
            h[i] = (int32_t)((uint32_t)b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) |
                             ((uint32_t)b[i * 4 + 2] << 16) |
                             ((uint32_t)b[i * 4 + 3] << 24));
        }
        calc = SQ_PAYLOAD_HDR + (long)h[1] * SQ_ZUBE_SIZE +
               (long)h[2] * SQ_POLS_SIZE + (long)h[3] * SQ_PNTS_SIZE;
        ok += (calc == (long)rawlen);
        bad += (calc != (long)rawlen);
        printf("%s %-52s zube=%-6d pols=%-7d pnts=%-7d calc=%-9ld file=%-9ld  "
               "(RAW, never decompressed)\n",
               calc == (long)rawlen ? "ok  " : "BAD ", rawpath,
               h[1], h[2], h[3], calc, (long)rawlen);
        free(rawbuf);
    }
    printf("\nsection identity holds in %ld of %ld\n", ok, ok + bad);
    corpus_free(&files);
    return 0;
}

/* ---- tokens ------------------------------------------------------------ */

static int cmd_tokens(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : DEFAULT_ROOT;
    corpus_list files;
    size_t fi;
    sq_tokens tot;
    long nfiles = 0;
    uint64_t n = 0;
    int a;

    memset(&tot, 0, sizeof tot);
    if (corpus_find(root, ".sq", &files) != 0) {
        fprintf(stderr, "cannot walk %s\n", root);
        return 2;
    }
    for (fi = 0; fi < files.n; fi++) {
        void *raw;
        size_t len;
        sq_level lv;
        sq_tokens t;
        char err[256];
        int k;

        raw = slurp_or_die(files.path[fi], &len);
        if (len < 4 || memcmp(raw, "SQSL", 4) != 0) {
            free(raw);
            continue;
        }
        nfiles++;
        if (sqsl_decode(raw, len, &lv, &t, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 2;
        }
        for (k = 0; k < SQ_ARM_N; k++) {
            tot.arm[k] += t.arm[k];
        }
        for (k = 0; k < 2; k++) {
            tot.zero[k] += t.zero[k];
            if (t.dmax[k] > tot.dmax[k]) {
                tot.dmax[k] = t.dmax[k];
            }
        }
        sq_level_free(&lv);
        free(raw);
    }
    for (a = 0; a < SQ_ARM_N; a++) {
        n += tot.arm[a];
    }
    printf("%ld SQSL file(s), %llu tokens\n\n", nfiles,
           (unsigned long long)n);
    for (a = 0; a < SQ_ARM_N; a++) {
        printf("  %-5s %12llu  %6.2f%%%s\n", sq_arm_name[a],
               (unsigned long long)tot.arm[a],
               n ? 100.0 * (double)tot.arm[a] / (double)n : 0.0,
               tot.arm[a] ? "" : "   <-- NEVER TAKEN, bit count unvalidated");
    }
    printf("\n");
    for (a = 0; a < 2; a++) {
        printf("  %-5s distance 0 in %llu token(s), max distance %u\n",
               sq_arm_name[a + 1], (unsigned long long)tot.zero[a], tot.dmax[a]);
    }
    corpus_free(&files);
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sq {files|info|check|sections|tokens} [ARGS]\n");
        return 2;
    }
    if (strcmp(argv[1], "files") == 0)    return cmd_files(argc - 2, argv + 2);
    if (strcmp(argv[1], "info") == 0)     return cmd_info(argc - 2, argv + 2);
    if (strcmp(argv[1], "check") == 0)    return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "sections") == 0) return cmd_sections(argc - 2, argv + 2);
    if (strcmp(argv[1], "tokens") == 0)   return cmd_tokens(argc - 2, argv + 2);
    fprintf(stderr, "usage: sq {files|info|check|sections|tokens} [ARGS]\n");
    return 2;
}
