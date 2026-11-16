/* car_params.c - read the handling table out of the user's own RAL.EXE.
 * See car_params.h for why this is a loader.
 *
 * The PE walk here is the C twin of tools/ralimg.py's `Image` class, and it
 * keeps that class's one non-obvious rule: a section's span is the LARGER of
 * VirtualSize and SizeOfRawData, because this image genuinely has data living
 * in raw tails that run past VirtualSize.  A reader that uses VirtualSize
 * alone finds 0x0057CB8C fine and then fails on something else later, which is
 * the worst kind of difference to debug - so the rule is copied rather than
 * simplified.
 *
 * Endianness: the image is PE32/x86, so every field is little-endian on disk.
 * The loads below are byte-assembled rather than memcpy'd into a uint32_t, so
 * this reads the same on the aarch64 target.  (aarch64 is little-endian too,
 * so it would work either way today - but the whole point of building for both
 * targets from the start is not to leave that kind of thing to luck.)
*/

#include "car_params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian scalar reads ---------------------------------------- */

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t rd_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

/* IEEE-754 single from four bytes.  Assembled through a union rather than a
 * pointer cast: a cast is a strict-aliasing violation and -O2 is entitled to
 * act on it.  Both targets are IEEE binary32, so the bit pattern is the
 * value. */
static float rd_f32(const unsigned char *p)
{
    union { uint32_t u; float f; } cvt;
    cvt.u = rd_u32(p);
    return cvt.f;
}

/* ---- the PE walk -------------------------------------------------------- */

typedef struct {
    unsigned char *blob;
    size_t         len;
    uint32_t       image_base;
    const unsigned char *sec;    /* first section header */
    unsigned       nsec;
} pe_image;

static int pe_open(const char *path, pe_image *im, char *err, size_t errlen)
{
    FILE *f;
    long  sz;
    uint32_t pe, opt;
    uint16_t opt_size, magic;

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
    if (!im->blob) {
        if (err) snprintf(err, errlen, "out of memory reading %s", path);
        fclose(f);
        return -1;
    }
    if (fread(im->blob, 1, im->len, f) != im->len) {
        if (err) snprintf(err, errlen, "short read on %s", path);
        free(im->blob);
        im->blob = NULL;
        fclose(f);
        return -1;
    }
    fclose(f);

    if (im->blob[0] != 'M' || im->blob[1] != 'Z') {
        if (err) snprintf(err, errlen, "%s: not an MZ image", path);
        return -2;
    }
    pe = rd_u32(im->blob + 0x3C);
    if (pe + 24 > im->len || memcmp(im->blob + pe, "PE\0\0", 4) != 0) {
        if (err) snprintf(err, errlen, "%s: no PE header", path);
        return -2;
    }
    im->nsec = rd_u16(im->blob + pe + 6);
    opt_size = rd_u16(im->blob + pe + 20);
    opt      = pe + 24;
    magic    = rd_u16(im->blob + opt);
    if (magic != 0x10B) {
        if (err) snprintf(err, errlen, "%s: not PE32 (magic 0x%X)", path, magic);
        return -2;
    }
    im->image_base = rd_u32(im->blob + opt + 28);
    im->sec        = im->blob + opt + opt_size;

    if ((size_t)(im->sec - im->blob) + (size_t)im->nsec * 40 > im->len) {
        if (err) snprintf(err, errlen, "%s: section table runs past EOF", path);
        return -2;
    }
    return 0;
}

static void pe_close(pe_image *im)
{
    free(im->blob);
    im->blob = NULL;
}

/* VA -> a pointer to `n` bytes of raw file data, or NULL.
 *
 * NULL covers both "no section holds this VA" and "this VA is in a section's
 * zero-fill tail, so there are no bytes on disk" - ralimg.py's `off()` makes
 * the same distinction and it matters: BSS addresses resolve to a section and
 * still have nothing to read. */
static const unsigned char *pe_at(const pe_image *im, uint32_t va, size_t n)
{
    unsigned i;

    for (i = 0; i < im->nsec; i++) {
        const unsigned char *h = im->sec + i * 40;
        uint32_t vsize   = rd_u32(h + 8);
        uint32_t vaddr   = rd_u32(h + 12) + im->image_base;
        uint32_t rawsize = rd_u32(h + 16);
        uint32_t rawptr  = rd_u32(h + 20);
        uint32_t span    = (vsize > rawsize) ? vsize : rawsize;

        if (va < vaddr || va - vaddr >= span)
            continue;

        {
            uint32_t delta = va - vaddr;
            if (delta >= rawsize)
                return NULL;                 /* zero-fill tail */
            if ((uint64_t)delta + n > rawsize)
                return NULL;                 /* would run off the section */
            if ((uint64_t)rawptr + delta + n > im->len)
                return NULL;                 /* ...or off the file */
            return im->blob + rawptr + delta;
        }
    }
    return NULL;
}

/* ---- the table ---------------------------------------------------------- */

static float floor_at(float v, float floor_value)
{
    return (v < floor_value) ? floor_value : v;
}

int car_params_load(const char *image_path, car_params_table *out,
                    char *err, size_t errlen)
{
    pe_image im;
    unsigned i;
    int rc;

    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    rc = pe_open(image_path, &im, err, errlen);
    if (rc != 0) {
        pe_close(&im);
        return rc;
    }

    for (i = 0; i < CAR_PARAMS_COUNT; i++) {
        const unsigned char *r =
            pe_at(&im, CAR_PARAMS_TABLE_VA + i * CAR_PARAMS_STRIDE,
                  CAR_PARAMS_STRIDE);
        car_params *p = &out->rec[i];
        unsigned k;

        if (!r) {
            if (err)
                snprintf(err, errlen,
                         "record %u (0x%08X) is not backed by raw data",
                         i, (unsigned)(CAR_PARAMS_TABLE_VA
                                       + i * CAR_PARAMS_STRIDE));
            pe_close(&im);
            return -3;
        }

        /* THE NAME IS SPACE-PADDED, NOT NUL-TERMINATED.  Measured: record 0
         * is "00 renault maxi megane  " and record 24 is "24" followed by 22
         * spaces.  A reader that only appends a NUL gets strlen == 24 for
         * every record, which silently turns the "24 named cars" figure into
         * 32 - a reported number that reads like a measurement and is not.
         * Trim here, once, so nothing downstream has to know. */
        memcpy(p->name, r, CAR_PARAMS_NAME_LEN);
        p->name[CAR_PARAMS_NAME_LEN] = '\0';
        for (k = CAR_PARAMS_NAME_LEN; k > 0 && p->name[k - 1] == ' '; k--)
            p->name[k - 1] = '\0';

        p->drive_class = rd_i32(r + 0x18);

        /* CLAMPED TO [5,7] ON LOAD - s3.  Copy the clamp,
         * not the raw field: it is the loaded value the gear ladder uses. */
        p->ngears = rd_i32(r + 0x1C);
        if (p->ngears < 5) p->ngears = 5;
        if (p->ngears > 7) p->ngears = 7;

        p->unk20      = rd_i32(r + 0x20);
        p->grip_t     = rd_f32(r + 0x24);
        p->powr_scale = rd_f32(r + 0x28);
        p->acc_grip   = rd_f32(r + 0x2C);
        p->brk_grip   = rd_f32(r + 0x30);
        p->sidegrip   = rd_f32(r + 0x34);

        /* x gravity ON LOAD.  The record holds a multiple of g; the globals
         * the physics reads are decelerations in m/s^2. */
        p->handbrake_decel = rd_f32(r + 0x38) * CAR_GRAVITY;
        p->brake_decel     = rd_f32(r + 0x3C) * CAR_GRAVITY;

        p->unk40 = rd_i32(r + 0x40);
        p->unk44 = rd_i32(r + 0x44);
        p->unk48 = rd_i32(r + 0x48);
        /* +0x4C is NOT READ by sim_load_car_params, so it is not read here.
         * +0x50/+0x54/+0x58 are read and then unconditionally overwritten by
         * globals, so storing them would invent a difference. */
        p->drag = rd_f32(r + 0x60);

        /* The four gearing endpoints.  int -> float, each FLOORED at 8.0.
         * it is a floor, not a cap - this was published backwards once. */
        p->first_lo = (int32_t)floor_at((float)rd_i32(r + 0x64), 8.0f);
        p->top_lo   = (int32_t)floor_at((float)rd_i32(r + 0x68), 8.0f);
        p->first_hi = (int32_t)floor_at((float)rd_i32(r + 0x6C), 8.0f);
        p->top_hi   = (int32_t)floor_at((float)rd_i32(r + 0x70), 8.0f);

        p->maxrevs = floor_at((float)rd_i32(r + 0x74), 1000.0f);

        /* Stored raw.  The final drive is +0x78 x [0x005C6BF8] / maxrevs and
         * that constant is read from the image by dt_consts_load, so the
         * multiply happens there - see car_params.h. */
        p->engscale_raw = rd_i32(r + 0x78);

        for (k = 0; k < CAR_PARAMS_CURVE_PTS; k++)
            p->torque[k] = rd_f32(r + 0x7C + k * 4);

        p->upshift         = rd_f32(r + 0xA0);
        p->coast_off_brake = rd_f32(r + 0xA4);
        p->coast_on_brake  = rd_f32(r + 0xA8);
    }

    /* THE SELF-CHECK.  Every record's name begins with its own index, "00"
     * through "31".  This is what makes 0x0057CB8C/0xAC a reading of the data
     * rather than a claim about it, and it is the only thing here that can
     * detect a wrong base, a wrong stride, or a differently-built image.
     * Standing rule 11: a gate you have only seen pass is not evidence, so
     * this one is written to be able to fail and is exercised in the test. */
    for (i = 0; i < CAR_PARAMS_COUNT; i++) {
        char want[3];
        snprintf(want, sizeof(want), "%02u", i);
        if (out->rec[i].name[0] != want[0] || out->rec[i].name[1] != want[1]) {
            if (err)
                snprintf(err, errlen,
                         "record %u names itself \"%.8s\", not \"%s\" - the "
                         "table base or stride is wrong for this image",
                         i, out->rec[i].name, want);
            pe_close(&im);
            return -4;
        }
    }

    /* Records 24-31 are placeholders: all-1.000 grip, identical brake figures,
     * and a name that is ONLY the index.  So this counts records whose trimmed
     * name is longer than its own two-digit index, and on retail 5.25 that is
     * 24 -'s own figure, reached here by a second
     * implementation rather than copied from the document (standing rule 16).
     *
     * REPORTED, never asserted: whether the game can ever SELECT one of the
     * eight placeholders is unknown, and AND EBX,0x1f would let it. */
    out->named = 0;
    for (i = 0; i < CAR_PARAMS_COUNT; i++)
        if (strlen(out->rec[i].name) > 2)
            out->named++;

    pe_close(&im);
    return 0;
}

const car_params *car_params_for(const car_params_table *t, int32_t carnum)
{
    /* AND EBX,0x1f, exactly as FUN_004201FB does it.  The mask is the reason
     * a bad car number cannot index off the end, and copying it is copying
     * the original's behaviour rather than adding a bounds check it lacks. */
    return &t->rec[(uint32_t)carnum & 0x1Fu];
}
