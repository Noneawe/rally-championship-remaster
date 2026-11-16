/* pcx.c - see pcx.h. */

#include "pcx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void pcx_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || errlen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

int pcx_read_header(const void *buf, size_t len, pcx_info *pi,
                    char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    unsigned xmin, ymin, xmax, ymax;

    memset(pi, 0, sizeof *pi);
    if (!b || len < PCX_HDR_SIZE) {
        pcx_err(err, errlen, "short file: %lu bytes", (unsigned long)len);
        return 1;
    }
    if (b[0] != PCX_MANUFACTURER) {
        pcx_err(err, errlen, "bad manufacturer %d", (int)b[0]);
        return 1;
    }
    if (b[1] != PCX_VERSION) {
        pcx_err(err, errlen, "bad version %d", (int)b[1]);
        return 1;
    }
    if (b[2] != PCX_ENCODING_RLE) {
        pcx_err(err, errlen, "bad encoding %d", (int)b[2]);
        return 1;
    }
    if (b[3] != 8) {
        pcx_err(err, errlen, "bad bits per pixel %d", (int)b[3]);
        return 1;
    }
    if (b[65] != 1 && b[65] != 3) {
        pcx_err(err, errlen, "bad plane count %d", (int)b[65]);
        return 1;
    }
    xmin = rd16(b + 4);
    ymin = rd16(b + 6);
    xmax = rd16(b + 8);
    ymax = rd16(b + 10);
    if (xmax < xmin || ymax < ymin) {
        pcx_err(err, errlen, "bad window %u %u %u %u", xmin, ymin, xmax, ymax);
        return 1;
    }
    pi->w              = xmax - xmin + 1;
    pi->h              = ymax - ymin + 1;
    pi->planes         = b[65];
    pi->bits_per_pixel = b[3];
    pi->bytes_per_line = rd16(b + 66);
    if (pi->bytes_per_line < pi->w) {
        pcx_err(err, errlen, "bytes per line %u below width %u",
                pi->bytes_per_line, pi->w);
        return 1;
    }
    pi->has_palette = (pi->planes == 1 && len >= PCX_PAL_BYTES + 1 &&
                       b[len - PCX_PAL_BYTES - 1] == PCX_PAL_MARKER);
    return 0;
}

int pcx_decode(const void *buf, size_t len, uint32_t *out, size_t outcap,
               char *err, size_t errlen)
{
    return pcx_decode_request(buf, len, out, outcap, 0u, 0u, NULL, NULL,
                              err, errlen);
}

/* The one walk behind both entry points.  `out` receives 0xAARRGGBB, or -
 * when `idx` is non-NULL - `idx` receives the raw scanline bytes of a ONE-
 * plane file instead (the font sheets are cut on palette indices, not
 * colours, FUN_0041185A).  Exactly one of the two is written. */
static int decode(const void *buf, size_t len, uint32_t *out, uint8_t *idx,
                  size_t outcap, unsigned req_w, unsigned req_h,
                  unsigned *dw, unsigned *dh, char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    const uint8_t *pal = NULL;
    pcx_info  pi;
    size_t    src = PCX_HDR_SIZE, want, got = 0;
    size_t    end;
    unsigned  y, x, w, h;
    /* The decoded run stream, one scanline of `planes` planes at a time.  The
     * whole image is never materialised in planar form: a scanline is at most
     * 3 * bytes_per_line and the shipped maximum is 320. */
    uint8_t   line[3u * PCX_MAX_BYTES_PER_LINE];

    if (pcx_read_header(buf, len, &pi, err, errlen) != 0) {
        return 1;
    }
    /* 0x00411775 / 0x004117A8: the request first, then the canvas.  Both are
     * `if (v > limit) v = limit`, so a request LARGER than the file leaves the
     * file's own size alone - the clamp only ever shrinks. */
    w = pi.w;
    h = pi.h;
    if (req_w != 0u) {
        if (w > req_w)         { w = req_w; }
        if (w > PCX_CANVAS_W)  { w = PCX_CANVAS_W; }
    }
    if (req_h != 0u) {
        if (h > req_h)         { h = req_h; }
        if (h > PCX_CANVAS_H)  { h = PCX_CANVAS_H; }
    }
    if (dw) { *dw = w; }
    if (dh) { *dh = h; }
    if ((size_t)w * h > outcap) {
        pcx_err(err, errlen, "output buffer holds %lu of %lu pixels",
                (unsigned long)outcap, (unsigned long)((size_t)w * h));
        return 1;
    }
    if ((size_t)pi.bytes_per_line * pi.planes > sizeof line) {
        pcx_err(err, errlen, "scanline %lu bytes exceeds the decoder's %lu",
                (unsigned long)((size_t)pi.bytes_per_line * pi.planes),
                (unsigned long)sizeof line);
        return 1;
    }

    /* The palette sits BEFORE the 0x0c marker's own byte at EOF, so the run
     * stream stops there.  With three planes there is no palette and the run
     * stream runs to the end of the file. */
    end = len;
    if (pi.has_palette) {
        pal = b + len - PCX_PAL_BYTES;
        end = len - PCX_PAL_BYTES - 1;
    }

    /* `want` counts the bytes the ROWS THAT ARE DECODED hold, because it only
     * ever appears in the "run stream ends at N of M" message and that message
     * has to describe the walk that ran out. */
    want = (size_t)pi.bytes_per_line * pi.planes * h;
    for (y = 0; y < h; y++) {
        size_t need = (size_t)pi.bytes_per_line * pi.planes, n = 0;

        while (n < need) {
            unsigned v, count;

            if (src >= end) {
                pcx_err(err, errlen,
                        "run stream ends at %lu of %lu decoded bytes",
                        (unsigned long)(got + n), (unsigned long)want);
                return 1;
            }
            v = b[src++];
            if ((v & 0xc0u) == 0xc0u) {
                count = v & 0x3fu;
                if (src >= end) {
                    pcx_err(err, errlen,
                            "run stream ends at %lu of %lu decoded bytes",
                            (unsigned long)(got + n), (unsigned long)want);
                    return 1;
                }
                v = b[src++];
            } else {
                count = 1;
            }
            /* A run that would cross the end of the scanline is malformed
             * PCX; the specification forbids it and no shipped plate does it.
             * Refusing it is what makes "the stream lands exactly" a check. */
            if (n + count > need) {
                pcx_err(err, errlen,
                        "run of %u crosses the scanline end at byte %lu",
                        count, (unsigned long)n);
                return 1;
            }
            while (count--) {
                line[n++] = (uint8_t)v;
            }
        }
        got += n;

        /* 0x00411A8A / 0x004119EA: the store is bounded by the CLAMPED width
         * while the run walk above consumed BytesPerLine.  Columns past `w`
         * are decoded and dropped. */
        if (idx) {
            for (x = 0; x < w; x++) {
                idx[(size_t)y * w + x] = line[x];
            }
            continue;
        }
        for (x = 0; x < w; x++) {
            uint32_t r, g, bl;

            if (pi.planes == 3) {
                r  = line[x];
                g  = line[pi.bytes_per_line + x];
                bl = line[2u * pi.bytes_per_line + x];
            } else if (pal) {
                unsigned i = line[x];
                r  = pal[i * 3u + 0u];
                g  = pal[i * 3u + 1u];
                bl = pal[i * 3u + 2u];
            } else {
                /* No shipped one-plane plate reaches this - all three carry a
                 * palette - but a decoder that silently produced black for a
                 * palette it could not find would hide the fault. */
                r = g = bl = line[x];
            }
            out[(size_t)y * w + x] =
                0xff000000u | (r << 16) | (g << 8) | bl;
        }
    }

    /* The whole point of the corpus check: the stream must land exactly.
     * Trailing bytes mean the stride or the arm was read wrong in a way that
     * still produced a plausible picture.
     *
     * A HEIGHT clip is the one case where it cannot be asked: the game's row
     * loop stops at [0x00512CA0] and never touches the rows past it, so bytes
     * are SUPPOSED to be left over.  A WIDTH clip leaves the check intact,
     * because the dropped columns are still consumed.  pcx.h says which way
     * this errs; no shipped plate is over 256 tall, so the 942-file corpus
     * check runs with this test live on every one of them. */
    if (h == pi.h && src != end) {
        pcx_err(err, errlen, "%lu bytes left after the last scanline",
                (unsigned long)(end - src));
        return 1;
    }
    return 0;
}

int pcx_decode_request(const void *buf, size_t len, uint32_t *out,
                       size_t outcap, unsigned req_w, unsigned req_h,
                       unsigned *dw, unsigned *dh, char *err, size_t errlen)
{
    return decode(buf, len, out, NULL, outcap, req_w, req_h, dw, dh,
                  err, errlen);
}

int pcx_decode_index(const void *buf, size_t len, uint8_t *out, size_t outcap,
                     uint32_t pal[256], unsigned *dw, unsigned *dh,
                     char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    pcx_info pi;
    unsigned i;

    if (pcx_read_header(buf, len, &pi, err, errlen) != 0) {
        return 1;
    }
    if (pi.planes != 1 || !pi.has_palette) {
        pcx_err(err, errlen, "an index decode needs one plane and a palette "
                "(%u planes, palette %d)", pi.planes, pi.has_palette);
        return 1;
    }
    for (i = 0; i < 256u; i++) {
        const uint8_t *c = b + len - PCX_PAL_BYTES + 3u * i;
        pal[i] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
    }
    return decode(buf, len, NULL, out, outcap, 0, 0, dw, dh, err, errlen);
}
