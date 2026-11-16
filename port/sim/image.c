/* image.c - see image.h. */

#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

float ral_image_f32(const unsigned char *p)
{
    union { uint32_t u; float f; } cvt;
    cvt.u = rd_u32(p);
    return cvt.f;
}

int32_t ral_image_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

int ral_image_open(ral_image *im, const char *path, char *err, size_t errlen)
{
    FILE *f;
    long sz;
    uint32_t pe, opt;
    uint16_t opt_size, magic;

    if (err && errlen) err[0] = '\0';
    memset(im, 0, sizeof(*im));

    f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x400) {
        if (err) snprintf(err, errlen, "%s: too small to be a PE image", path);
        fclose(f);
        return -1;
    }
    rewind(f);
    im->len  = (size_t)sz;
    im->blob = (unsigned char *)malloc(im->len);
    if (!im->blob || fread(im->blob, 1, im->len, f) != im->len) {
        if (err) snprintf(err, errlen, "short read on %s", path);
        free(im->blob);
        im->blob = NULL;
        fclose(f);
        return -1;
    }
    fclose(f);

    if (im->blob[0] != 'M' || im->blob[1] != 'Z') {
        if (err) snprintf(err, errlen, "%s: not an MZ image", path);
        ral_image_close(im);
        return -2;
    }
    pe = rd_u32(im->blob + 0x3C);
    if ((size_t)pe + 24 > im->len || memcmp(im->blob + pe, "PE\0\0", 4) != 0) {
        if (err) snprintf(err, errlen, "%s: no PE header", path);
        ral_image_close(im);
        return -2;
    }
    im->nsec = rd_u16(im->blob + pe + 6);
    opt_size = rd_u16(im->blob + pe + 20);
    opt      = pe + 24;
    magic    = rd_u16(im->blob + opt);
    if (magic != 0x10B) {
        if (err) snprintf(err, errlen, "%s: not PE32", path);
        ral_image_close(im);
        return -2;
    }
    im->base = rd_u32(im->blob + opt + 28);
    im->sec  = im->blob + opt + opt_size;
    return 0;
}

void ral_image_close(ral_image *im)
{
    free(im->blob);
    im->blob = NULL;
    im->len  = 0;
}

const unsigned char *ral_image_at(const ral_image *im, uint32_t va, size_t n)
{
    unsigned i;
    for (i = 0; i < im->nsec; i++) {
        const unsigned char *h = im->sec + i * 40;
        uint32_t vsize   = rd_u32(h + 8);
        uint32_t vaddr   = rd_u32(h + 12) + im->base;
        uint32_t rawsize = rd_u32(h + 16);
        uint32_t rawptr  = rd_u32(h + 20);
        uint32_t span    = (vsize > rawsize) ? vsize : rawsize;
        uint32_t d;

        if (va < vaddr || va - vaddr >= span) continue;
        d = va - vaddr;
        /* Past the raw extent is the zero-fill tail: real addresses with no
         * bytes on disk.  Saying NULL rather than zeros is the point - the
         * caller must not mistake "uninitialised at load" for "the value". */
        if (d >= rawsize) return NULL;
        if ((uint64_t)d + n > rawsize) return NULL;
        if ((uint64_t)rawptr + d + n > im->len) return NULL;
        return im->blob + rawptr + d;
    }
    return NULL;
}
