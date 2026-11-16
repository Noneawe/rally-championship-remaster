/* bun.c - see bun.h. Transcribed from tools/bun.py, held byte-identical to it
 * by check.sh over the whole corpus.
 *
 * The rejection strings below are reproduced WORD FOR WORD from bun.py,
 * including their punctuation and their number formatting. That is not
 * pedantry: check.sh is a plain `diff`, so a reworded message is a failure, and
 * making the messages part of the compared surface is what stops the two
 * implementations drifting in the one place where drift is hardest to notice -
 * the error paths, which the happy-path corpus never exercises.
 */

#include "bun.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const bun_form_name[2] = { "compact", "extended" };

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void problem(bun_container *c, const char *fmt, ...)
{
    va_list ap;

    if (c->nproblems >= BUN_MAX_PROBLEMS) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(c->problem[c->nproblems], sizeof c->problem[0], fmt, ap);
    va_end(ap);
    c->nproblems++;
}

/* ---- the container ----------------------------------------------------- */

int bun_parse(const void *buf, size_t len, bun_container *c)
{
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t first;
    size_t count, i;
    long long sum = 0;          /* signed: see the note on bun_container.size */

    memset(c, 0, sizeof *c);
    c->filelen = len;

    if (len < 4) {
        problem(c, "file is shorter than one dword");
        return 1;
    }
    first = rd32(b);
    if (first % 4) {
        problem(c, "first dword %lu is not a multiple of 4",
                (unsigned long)first);
    }
    if (!(first >= 4 && (size_t)first <= len)) {
        problem(c, "first dword %lu is not a plausible table size (file %lu)",
                (unsigned long)first, (unsigned long)len);
        return 1;
    }

    count = first / 4;
    if (count > BUN_MAX_ENTRIES) {
        problem(c, "entry count %lu exceeds the parser's tripwire",
                (unsigned long)count);
        return 1;
    }
    c->off  = (uint32_t *)malloc(count * sizeof *c->off);
    c->size = (int64_t  *)malloc(count * sizeof *c->size);
    if (!c->off || !c->size) {
        bun_container_free(c);
        problem(c, "out of memory for %lu entries", (unsigned long)count);
        return 1;
    }
    c->n = count;
    for (i = 0; i < count; i++) {
        c->off[i] = rd32(b + i * 4);
    }

    /* The three checks bun.py makes, in its order, so the reports match. */
    for (i = 0; i + 1 < count; i++) {
        if (c->off[i] > c->off[i + 1]) {
            problem(c, "offset %lu (%lu) > offset %lu (%lu): not monotonic",
                    (unsigned long)i, (unsigned long)c->off[i],
                    (unsigned long)(i + 1), (unsigned long)c->off[i + 1]);
        }
    }
    for (i = 0; i < count; i++) {
        if (!((size_t)c->off[i] >= first && (size_t)c->off[i] <= len)) {
            problem(c, "offset %lu (%lu) is outside [%lu, %lu]",
                    (unsigned long)i, (unsigned long)c->off[i],
                    (unsigned long)first, (unsigned long)len);
        }
    }
    if (count && c->off[0] != first) {
        problem(c, "offset 0 (%lu) != table size (%lu)",
                (unsigned long)c->off[0], (unsigned long)first);
    }

    for (i = 0; i + 1 < count; i++) {
        c->size[i] = (int64_t)c->off[i + 1] - (int64_t)c->off[i];
    }
    if (count) {
        c->size[count - 1] = (int64_t)len - (int64_t)c->off[count - 1];
    }
    for (i = 0; i < count; i++) {
        sum += c->size[i];
    }
    if (sum + (long long)first != (long long)len) {
        problem(c, "closure fails: sum(sizes) %lld + table %lu != file %lu",
                sum, (unsigned long)first, (unsigned long)len);
    }
    return c->nproblems ? 1 : 0;
}

void bun_container_free(bun_container *c)
{
    free(c->off);
    free(c->size);
    c->off = NULL;
    c->size = NULL;
    c->n = 0;
}

/* ---- one entry --------------------------------------------------------- */

/* Growable arrays, because the run and pixel counts are not known until the
 * walk finishes and the walk is the thing being validated - counting in a
 * first pass would mean two copies of the decoder and would defeat the point. */
#define GROW(arr, cap, need, type)                                            \
    do {                                                                      \
        if ((need) > (cap)) {                                                 \
            size_t nc = (cap) ? (cap) * 2 : 64;                               \
            void *np;                                                         \
            while (nc < (need)) { nc *= 2; }                                  \
            np = realloc((arr), nc * sizeof(type));                           \
            if (!np) { goto oom; }                                            \
            (arr) = (type *)np;                                               \
            (cap) = nc;                                                       \
        }                                                                     \
    } while (0)

int bun_decode_entry(const void *buf, size_t len, size_t index,
                     bun_sprite *s, char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t tag;
    size_t hdr, unit, p, y;
    size_t runcap = 0, pxcap = 0;

    memset(s, 0, sizeof *s);

    if (len < 4) {
        snprintf(err, errlen,
                 "entry is %lu bytes, shorter than the 4-byte compact header",
                 (unsigned long)len);
        return 1;
    }
    tag = rd32(b);

    if (tag < BUN_EXT_TAG_MIN) {
        s->form = BUN_FORM_COMPACT;
        hdr  = BUN_HDR_COMPACT;
        unit = 1;
        s->xoff = b[0]; s->yoff = b[1]; s->w = b[2]; s->h = b[3];
    } else {
        s->form = BUN_FORM_EXTENDED;
        hdr  = BUN_HDR_EXTENDED;
        unit = (tag == BUN_EXT_TAG_U16) ? 2 : 4;
        if (len < 12) {
            snprintf(err, errlen,
                     "extended entry is %lu bytes, shorter than its "
                     "12-byte header", (unsigned long)len);
            return 1;
        }
        s->xoff = rd16(b + 4); s->yoff = rd16(b + 6);
        s->w    = rd16(b + 8); s->h    = rd16(b + 10);
    }
    s->index = index;
    s->tag   = tag;

    /* `w == 0` is a legitimate encoding, not a degenerate one: it is how a
     * blank advance-only glyph (the space) is stored, and draw_text
     * (0x004023E8) tests for it explicitly. Nothing is relaxed by allowing it -
     * with w == 0 the row walk below still rejects any packet at all, because
     * the first non-zero count pushes the cursor past w. */
    if (s->h == 0) {
        snprintf(err, errlen, "degenerate height 0 (%lux%lu)",
                 (unsigned long)s->w, (unsigned long)s->h);
        return 1;
    }

    p = hdr;
    for (y = 0; y < s->h; y++) {
        uint32_t x = 0;
        for (;;) {
            uint32_t count, skip, need, k;

            if (p + unit > len) {
                snprintf(err, errlen,
                         "ran off the end reading a count at row %lu "
                         "(offset %lu of %lu)",
                         (unsigned long)y, (unsigned long)p,
                         (unsigned long)len);
                goto reject;
            }
            count = (unit == 1) ? b[p] : (unit == 2) ? rd16(b + p) : rd32(b + p);
            p += unit;
            if (count == 0) {
                break;
            }
            if (p + unit > len) {
                snprintf(err, errlen,
                         "ran off the end reading a skip at row %lu "
                         "(offset %lu of %lu)",
                         (unsigned long)y, (unsigned long)p,
                         (unsigned long)len);
                goto reject;
            }
            skip = (unit == 1) ? b[p] : (unit == 2) ? rd16(b + p) : rd32(b + p);
            p += unit;
            x += skip;

            need = count * (uint32_t)unit;
            if (p + need > len) {
                snprintf(err, errlen,
                         "run of %lu pixels at row %lu overruns the entry "
                         "(offset %lu + %lu > %lu)",
                         (unsigned long)count, (unsigned long)y,
                         (unsigned long)p, (unsigned long)need,
                         (unsigned long)len);
                goto reject;
            }

            GROW(s->runs,   runcap, s->nruns + 1,          bun_run);
            GROW(s->pixels, pxcap,  s->npixels + count,    uint32_t);
            s->runs[s->nruns].x     = x;
            s->runs[s->nruns].first = (uint32_t)s->npixels;
            s->runs[s->nruns].count = count;
            s->runs[s->nruns].row   = (uint32_t)y;
            s->nruns++;
            for (k = 0; k < count; k++) {
                const uint8_t *q = b + p + k * unit;
                s->pixels[s->npixels + k] =
                    (unit == 1) ? *q : (unit == 2) ? rd16(q) : rd32(q);
            }
            s->npixels += count;
            p += need;

            x += count;
            if (x > s->w) {
                snprintf(err, errlen,
                         "row %lu cursor reached %lu, past the declared "
                         "width %lu",
                         (unsigned long)y, (unsigned long)x,
                         (unsigned long)s->w);
                goto reject;
            }
        }
    }

    if (p != len) {
        snprintf(err, errlen,
                 "payload closure fails: %lu rows consumed %lu of %lu bytes "
                 "(%+ld)", (unsigned long)s->h, (unsigned long)p,
                 (unsigned long)len, (long)p - (long)len);
        goto reject;
    }

    s->consumed = p;
    s->size     = len;
    return 0;

oom:
    snprintf(err, errlen, "out of memory decoding entry %lu",
             (unsigned long)index);
reject:
    bun_sprite_free(s);
    return 1;
}

void bun_sprite_free(bun_sprite *s)
{
    free(s->runs);
    free(s->pixels);
    s->runs = NULL;
    s->pixels = NULL;
    s->nruns = 0;
    s->npixels = 0;
}

/* ---- a whole file ------------------------------------------------------ */

int bun_decode_file(const void *buf, size_t len, bun_file *f)
{
    const uint8_t *b = (const uint8_t *)buf;
    size_t i;

    memset(f, 0, sizeof *f);
    if (bun_parse(buf, len, &f->c) != 0) {
        /* bun.py reports container problems as rejects with index -1 and does
         * not attempt any entry. Same here: index SIZE_MAX stands for -1 and
         * the tool prints it the way bun.py does. */
        f->rejects = (bun_reject *)calloc(f->c.nproblems + 1, sizeof *f->rejects);
        if (!f->rejects) {
            return 1;
        }
        for (i = 0; i < f->c.nproblems; i++) {
            f->rejects[i].index = (size_t)-1;
            snprintf(f->rejects[i].why, sizeof f->rejects[i].why,
                     "%s", f->c.problem[i]);
        }
        f->nrejects = f->c.nproblems;
        return 1;
    }

    f->sprites = (bun_sprite *)calloc(f->c.n ? f->c.n : 1, sizeof *f->sprites);
    f->rejects = (bun_reject *)calloc(f->c.n ? f->c.n : 1, sizeof *f->rejects);
    if (!f->sprites || !f->rejects) {
        bun_file_free(f);
        return 1;
    }

    for (i = 0; i < f->c.n; i++) {
        bun_sprite s;
        char why[160];

        /* Reached only when the container closed, so every size is >= 0.
         * The cast is where that invariant is stated. */
        if (bun_decode_entry(b + f->c.off[i], (size_t)f->c.size[i], i,
                             &s, why, sizeof why) == 0) {
            f->sprites[f->nsprites++] = s;
        } else {
            f->rejects[f->nrejects].index = i;
            snprintf(f->rejects[f->nrejects].why,
                     sizeof f->rejects[0].why, "%s", why);
            f->nrejects++;
        }
    }
    return f->nrejects ? 1 : 0;
}

void bun_file_free(bun_file *f)
{
    size_t i;

    for (i = 0; i < f->nsprites; i++) {
        bun_sprite_free(&f->sprites[i]);
    }
    free(f->sprites);
    free(f->rejects);
    bun_container_free(&f->c);
    f->sprites = NULL;
    f->rejects = NULL;
    f->nsprites = 0;
    f->nrejects = 0;
}
