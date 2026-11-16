/* bun_tool.c - the CLI over bun.c.
 *
 * Every subcommand here exists to produce output that is BYTE-IDENTICAL to
 * tools/bun.py's, so check.sh can be a plain `diff`. Where a format string
 * looks fussy, it is copied from the Python and the fussiness is the point.
 *
 *   bun files  ROOT              every *.bun under ROOT, one per line
 *   bun check  PATH [PATH ...]   validate the container
 *   bun list   PATH              one row per entry
 *   bun decode PATH [--verbose]  validate the payload; per-entry closure
 *   bun pixels PATH [PATH ...]   census of literal pixel values
 *
 * `extract` and `render` are deliberately NOT here. `extract` writes bytes the
 * repo must not contain and `render` needs a PNG writer, i.e. zlib -
 * neither is code that ships in the port, which is what this toolkit is for.
 * tools/bun.py keeps both. Said explicitly rather than left as a gap, because
 * a missing subcommand looks like an oversight.
*/

#include "bun.h"
#include "corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *basename_of(const char *path)
{
    const char *s = path, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            s = p + 1;
        }
    }
    return s;
}

static void *slurp_or_die(const char *path, size_t *len)
{
    void *d = corpus_slurp(path, len);

    if (!d) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    return d;
}

/* ---- files ------------------------------------------------------------- */

static int cmd_files(int argc, char **argv)
{
    corpus_list l;
    size_t i;

    if (argc < 1) {
        fprintf(stderr, "usage: bun files ROOT\n");
        return 2;
    }
    if (corpus_find(argv[0], ".bun", &l) != 0) {
        fprintf(stderr, "cannot walk %s\n", argv[0]);
        return 2;
    }
    for (i = 0; i < l.n; i++) {
        printf("%s\n", l.path[i]);
    }
    corpus_free(&l);
    return 0;
}

/* ---- check ------------------------------------------------------------- */

static int cmd_check(int argc, char **argv)
{
    int bad = 0, i;

    for (i = 0; i < argc; i++) {
        size_t len, k;
        void *data = slurp_or_die(argv[i], &len);
        bun_container c;
        long long sum = 0;
        int64_t lo, hi;
        size_t shortn = 0;

        bun_parse(data, len, &c);
        if (c.nproblems) {
            bad++;
            printf("FAIL %-14s %lu bytes\n", basename_of(argv[i]),
                   (unsigned long)len);
            for (k = 0; k < c.nproblems; k++) {
                printf("       %s\n", c.problem[k]);
            }
            bun_container_free(&c);
            free(data);
            continue;
        }
        lo = hi = c.size[0];
        for (k = 0; k < c.n; k++) {
            sum += c.size[k];
            if (c.size[k] < lo) { lo = c.size[k]; }
            if (c.size[k] > hi) { hi = c.size[k]; }
            if (c.size[k] < 4)  { shortn++; }
        }
        printf("ok   %-14s %6lu bytes | %3lu entries | table %4lu | sizes %lld..%lld\n",
               basename_of(argv[i]), (unsigned long)len, (unsigned long)c.n,
               (unsigned long)c.off[0], (long long)lo, (long long)hi);
        printf("       closure: sum(sizes) %lld + table %lu == file %lu\n",
               sum, (unsigned long)c.off[0], (unsigned long)len);
        printf("       entries shorter than 4 bytes: %lu\n",
               (unsigned long)shortn);
        bun_container_free(&c);
        free(data);
    }
    printf("\n");
    printf("%d file(s), %d failed\n", argc, bad);
    return bad ? 1 : 0;
}

/* ---- list -------------------------------------------------------------- */

static int cmd_list(int argc, char **argv)
{
    size_t len, i, k;
    void *data;
    const uint8_t *b;
    bun_container c;

    if (argc < 1) {
        fprintf(stderr, "usage: bun list PATH\n");
        return 2;
    }
    data = slurp_or_die(argv[0], &len);
    b = (const uint8_t *)data;
    bun_parse(data, len, &c);
    if (c.nproblems) {
        for (k = 0; k < c.nproblems; k++) {
            printf("PROBLEM: %s\n", c.problem[k]);
        }
        bun_container_free(&c);
        free(data);
        return 1;
    }
    printf("idx    offset   size  first 8 bytes\n");
    for (i = 0; i < c.n; i++) {
        char head[3 * 8], *h = head;
        size_t n8 = c.off[i] + 8 <= len ? 8 : len - c.off[i];

        for (k = 0; k < n8; k++) {
            h += sprintf(h, k ? " %02x" : "%02x", b[c.off[i] + k]);
        }
        *h = '\0';
        printf("%3lu  %8lu  %5lld  %s\n", (unsigned long)i,
               (unsigned long)c.off[i], (long long)c.size[i], head);
    }
    bun_container_free(&c);
    free(data);
    return 0;
}

/* ---- decode ------------------------------------------------------------ */

static int cmd_decode(int argc, char **argv, int verbose)
{
    int bad = 0, i, nfiles = 0;

    for (i = 0; i < argc; i++) {
        size_t len, k;
        void *data;
        bun_file f;

        if (strcmp(argv[i], "--verbose") == 0) {
            continue;
        }
        nfiles++;
        data = slurp_or_die(argv[i], &len);
        bun_decode_file(data, len, &f);

        if (f.nrejects) {
            bad++;
        }
        printf("%s %-14s %3lu entries | %3lu decoded, %lu rejected\n",
               f.nrejects ? "FAIL" : "ok  ", basename_of(argv[i]),
               (unsigned long)f.c.n, (unsigned long)f.nsprites,
               (unsigned long)f.nrejects);

        if (f.nsprites) {
            size_t ncompact = 0, nextended = 0;
            uint32_t wlo, whi, hlo, hhi, xlo, xhi, ylo, yhi;
            unsigned long long consumed = 0;
            long long payload = 0;
            int first = 1;

            wlo = whi = hlo = hhi = xlo = xhi = ylo = yhi = 0;
            for (k = 0; k < f.nsprites; k++) {
                const bun_sprite *s = &f.sprites[k];

                if (s->form == BUN_FORM_COMPACT) { ncompact++; } else { nextended++; }
                if (first) {
                    wlo = whi = s->w; hlo = hhi = s->h;
                    xlo = xhi = s->xoff; ylo = yhi = s->yoff;
                    first = 0;
                } else {
                    if (s->w    < wlo) { wlo = s->w; }
                    if (s->w    > whi) { whi = s->w; }
                    if (s->h    < hlo) { hlo = s->h; }
                    if (s->h    > hhi) { hhi = s->h; }
                    if (s->xoff < xlo) { xlo = s->xoff; }
                    if (s->xoff > xhi) { xhi = s->xoff; }
                    if (s->yoff < ylo) { ylo = s->yoff; }
                    if (s->yoff > yhi) { yhi = s->yoff; }
                }
                consumed += s->consumed;
            }
            for (k = 0; k < f.c.n; k++) {
                payload += f.c.size[k];
            }

            /* bun.py builds a dict of form -> count and prints
             * sorted(forms.items()), so "compact" precedes "extended"
             * alphabetically and a form with no members is absent. */
            printf("       forms: ");
            if (ncompact && nextended) {
                printf("compact %lu, extended %lu\n",
                       (unsigned long)ncompact, (unsigned long)nextended);
            } else if (ncompact) {
                printf("compact %lu\n", (unsigned long)ncompact);
            } else {
                printf("extended %lu\n", (unsigned long)nextended);
            }
            printf("       widths %lu..%lu  heights %lu..%lu  xoff %lu..%lu  yoff %lu..%lu\n",
                   (unsigned long)wlo, (unsigned long)whi,
                   (unsigned long)hlo, (unsigned long)hhi,
                   (unsigned long)xlo, (unsigned long)xhi,
                   (unsigned long)ylo, (unsigned long)yhi);
            printf("       payload closure: %lu of %lu entries consumed their bytes "
                   "exactly (%llu of %lld payload bytes)\n",
                   (unsigned long)f.nsprites, (unsigned long)f.c.n,
                   consumed, payload);
        }
        for (k = 0; k < f.nrejects; k++) {
            printf("       reject entry %ld: %s\n",
                   (long)f.rejects[k].index, f.rejects[k].why);
        }
        if (verbose) {
            printf("       idx  form      size   w x  h   xoff yoff  runs  bytes\n");
            for (k = 0; k < f.nsprites; k++) {
                const bun_sprite *s = &f.sprites[k];

                printf("       %3lu  %-8s %5lu  %3lu x %3lu  %4lu %4lu  %4lu  %5lu\n",
                       (unsigned long)s->index, bun_form_name[s->form],
                       (unsigned long)s->size, (unsigned long)s->w,
                       (unsigned long)s->h, (unsigned long)s->xoff,
                       (unsigned long)s->yoff, (unsigned long)s->nruns,
                       (unsigned long)s->consumed);
            }
        }
        bun_file_free(&f);
        free(data);
    }
    printf("\n");
    printf("%d file(s), %d with rejects\n", nfiles, bad);
    return bad ? 1 : 0;
}

/* ---- pixels ------------------------------------------------------------ */

typedef struct { uint32_t value; unsigned long long count; size_t seen; } hist_ent;

/* bun.py sorts with `sorted(hist, key=lambda k: -hist[k])`, and Python's sort
 * is STABLE, so ties are broken by the dict's insertion order - which is the
 * order each value was first seen. Reproducing that is the whole reason
 * `seen` exists; without it the two tools disagree on any file with tied
 * counts, and they would disagree differently on different libc qsorts. */
static int hist_cmp(const void *a, const void *b)
{
    const hist_ent *x = (const hist_ent *)a, *y = (const hist_ent *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return x->seen < y->seen ? -1 : x->seen > y->seen ? 1 : 0;
}

static int cmd_pixels(int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        size_t len, k, r, j, nh = 0, cap = 256;
        void *data = slurp_or_die(argv[i], &len);
        bun_file f;
        hist_ent *h = (hist_ent *)malloc(cap * sizeof *h);
        unsigned long long total = 0;

        if (!h) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        bun_decode_file(data, len, &f);

        for (k = 0; k < f.nsprites; k++) {
            const bun_sprite *s = &f.sprites[k];

            for (r = 0; r < s->npixels; r++) {
                uint32_t v = s->pixels[r];

                for (j = 0; j < nh; j++) {
                    if (h[j].value == v) {
                        break;
                    }
                }
                if (j == nh) {
                    if (nh == cap) {
                        hist_ent *nhp;

                        cap *= 2;
                        nhp = (hist_ent *)realloc(h, cap * sizeof *h);
                        if (!nhp) {
                            fprintf(stderr, "out of memory\n");
                            return 2;
                        }
                        h = nhp;
                    }
                    h[nh].value = v;
                    h[nh].count = 0;
                    h[nh].seen  = nh;
                    nh++;
                }
                h[j].count++;
                total++;
            }
        }

        printf("%-14s %lu of %lu entries decoded, %lu rejected, %llu literal pixels, "
               "%lu distinct value(s)\n",
               basename_of(argv[i]), (unsigned long)f.nsprites,
               (unsigned long)f.c.n, (unsigned long)f.nrejects, total,
               (unsigned long)nh);

        qsort(h, nh, sizeof *h, hist_cmp);
        for (j = 0; j < nh && j < 16; j++) {
            printf("       value 0x%02lx  %7llu  %6.2f%%\n",
                   (unsigned long)h[j].value, h[j].count,
                   100.0 * (double)h[j].count / (double)total);
        }
        if (nh > 16) {
            printf("       ... %lu more value(s)\n", (unsigned long)(nh - 16));
        }
        free(h);
        bun_file_free(&f);
        free(data);
    }
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int verbose = 0, i, n = 0;
    char **rest;

    if (argc < 2) {
        fprintf(stderr,
                "usage: bun {files|check|list|decode|pixels} ...\n"
                "  see the header comment; `extract` and `render` are in "
                "tools/bun.py only, deliberately\n");
        return 2;
    }

    rest = (char **)malloc((size_t)argc * sizeof *rest);
    if (!rest) {
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else {
            rest[n++] = argv[i];
        }
    }

    if (strcmp(argv[1], "files")  == 0) { i = cmd_files(n, rest); }
    else if (strcmp(argv[1], "check")  == 0) { i = cmd_check(n, rest); }
    else if (strcmp(argv[1], "list")   == 0) { i = cmd_list(n, rest); }
    else if (strcmp(argv[1], "decode") == 0) { i = cmd_decode(n, rest, verbose); }
    else if (strcmp(argv[1], "pixels") == 0) { i = cmd_pixels(n, rest); }
    else {
        fprintf(stderr, "unknown subcommand %s\n", argv[1]);
        i = 2;
    }
    free(rest);
    return i;
}
