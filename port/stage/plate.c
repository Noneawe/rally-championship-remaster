/* plate.c - see plate.h.
 *
 * Every constant below is an operand in the shipped image, transcribed rather
 * than re-derived (standing rule 3).  The address is at the line that uses it.
 * reproduces all of them with
 * `python tools/texupload.py`.
*/

#include "plate.h"
#include "pcx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
/* stb_image's failure reason is _Thread_local by default; MinGW emulates
 * that through libwinpthread-1.dll, which rally.exe would then need.  The
 * port decodes on one thread, so a plain global does. */
#define STBI_NO_THREAD_LOCALS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------- the path */

/* The band table, FUN_00424672, read in full: a ladder of
 * `cmp [0x5bdc3c],N / jb ret` where [0x5bdc3c] is the plate id.  The ladder is
 * ascending and every rung returns, so the first rung an id satisfies wins.
 *
 *   00424678  cmp 0x384   jb -> files\a          (< 900)
 *   00424696  cmp 0x386   jb -> files\m          (900..901)
 *   004246ae  cmp 0x3e8   jb -> files\a          (902..999)
 *   004246c6  cmp 0x7d0   jb -> files\b          (1000..1999)
 *   004246de  cmp 0xbb8   jb -> files\g\frnt     (2000..2999)
 *   004246ea                -> files\s\extr      (>= 3000)
 *
 * The gravel override that substitutes files\b\gravel for files\b is below,
 * plate_load_tyre: it is per CAR, not per plate id. */
static const struct { unsigned limit; const char *dir; } BANDS[] = {
    { 0x384u, "a"      },
    { 0x386u, "m"      },
    { 0x3e8u, "a"      },
    { 0x7d0u, "b"      },
    { 0xbb8u, "g/frnt" },
    { 0u,     "s/extr" }         /* limit 0 == the unconditional last rung */
};

int plate_path(char *out, size_t outlen, const char *files_root,
               unsigned id, int alpha)
{
    const char *dir = NULL;
    unsigned    i, n;

    /* 0x00424525 is a bare `INC EAX` before the second group of divisions: the
     * function builds BOTH paths per call, tx<id>.pcx and tx<id+1>.pcx, and
     * the second is the alpha component.  The BAND is chosen from the
     * original id (it is stored at [0x5bdc3c] on entry, before the INC). */
    n = alpha ? id + 1u : id;

    for (i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++) {
        if (BANDS[i].limit == 0u || id < BANDS[i].limit) {
            dir = BANDS[i].dir;
            break;
        }
    }
    if (!dir) {
        return 1;
    }
    /* FUN_00424462 patches four decimal digits into a template whose second
     * character is a LITERAL x - the template in .data reads `txxxxx.pcx`,
     * five x bytes, and only the last four are ever overwritten.  Four digits
     * is the whole field, so an id above 9999 has nowhere to go. */
    if (n > 9999u) {
        return 1;
    }
    if ((size_t)snprintf(out, outlen, "%s/%s/tx%04u.pcx", files_root, dir, n)
        >= outlen) {
        return 1;
    }
    return 0;
}

/* --------------------------------------------- the destination format */

/* `min_nonzero_colour`, FUN_00405103: it isolates the LOWEST SET BIT of each
 * of the four channel masks (`x & ~(x << 1)`) and unpacks the result back to
 * 32 bits through FUN_00405a41.
 *
 * The port has one destination format, RGBA8 - plat.h section 5, and the
 * original reaches the same place whenever the display is 32bpp
 * ([0x4ef928] == 0x20 makes all three texture roles 8888,
 * section 2.2).  With eight bits a channel the lowest set bit of each mask is
 * 1, so the value is 0x00010101 and the guard below nudges black to (1,1,1)
 * rather than to the (8,8,8) a 555 destination would need.  That is the
 * original's own 32bpp behaviour, not an approximation of its 16bpp one. */
#define PLATE_MIN_COLOUR 0x00010101u

/* ------------------------------------------------ the two alpha passes */
/* Exactly one runs, chosen on [descriptor+0x5c] at 0x00419a0a - and that flag
 * is set at CREATION from the requested texture role, never from the file
 * (0x00404ff5). */

/* FUN_00405153.  The key is MAGENTA - blue high, green zero, red high - and it
 * is tested on the TOP FIVE BITS of each channel, not exactly: the key is a
 * neighbourhood, which is what a 555 destination needs. */
static void plate_colour_key(uint32_t *px, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t t = px[i] & 0x00f8f8f8u;

        if (t == 0u) {
            px[i] = PLATE_MIN_COLOUR | 0xff000000u;   /* near-black guard */
        } else if (t == 0x00f800f8u) {
            px[i] = 0x00000000u;                      /* THE COLOUR KEY */
        } else {
            px[i] |= 0xff000000u;
        }
    }
}

/* FUN_0040518a.  `NOT AL` at 0x00405199: the tx<id+1>.pcx component is a
 * TRANSPARENCY map, not an opacity map - 0 in the file means opaque.  Byte 0
 * of the decoded alpha plate is what is read (`MOV AL,[ESI]` / `ADD ESI,4`),
 * which with B,G,R,A in memory is the BLUE channel; the three shipped
 * components are grey ramps, so the choice of channel is not observable and is
 * recorded here rather than claimed.
 *
 * No colour key and no black guard run on this path. */
static void plate_alpha_plane(uint32_t *px, const uint32_t *ap, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t a = 255u - (ap[i] & 0xffu);

        px[i] = (px[i] & 0x00ffffffu) | (a << 24);
    }
}

/* ------------------------------------------------------- the colour grade */

/* FUN_004052e9, read to the end of its basic block.  It bails unless
 * [0x4ef9f8] == 1 and bails again if BOTH params are 0x100, so 0x100 is
 * neutral and the pair is 8.8 fixed point.
 *
 *   ramp[i] = min(255, (i * brightness) >> 8)          0x00405324
 *   m       = floor((b + g + r) / 3)                   0x0040537f, [0x4efb04]
 *   c       = clamp(m + ((c - m) * saturation >> 8))   0x0040538d, SAR then
 *                                                      JS -> 0, JA 0xff -> 255
 *
 * So the second parameter scales each channel's DEVIATION FROM THE MEAN and
 * re-centres it - a SATURATION control - and the first is a plain brightness
 * gain.  The shift is SAR at 0x004053a2, arithmetic on a signed product, which
 * is why the intermediate below is `int` and not `unsigned`.
 *
 * Pixels with byte 3 == 0 are skipped: a keyed texel stays keyed. */
static void plate_grade_apply(uint32_t *px, size_t n, const plate_grade *g)
{
    uint8_t  ramp[256];
    unsigned i;
    size_t   k;

    if (g->brightness == PLATE_GRADE_NEUTRAL &&
        g->saturation == PLATE_GRADE_NEUTRAL) {
        return;                                  /* 0x00405305's early out */
    }
    for (i = 0; i < 256u; i++) {
        unsigned v = (i * g->brightness) >> 8;
        ramp[i] = (uint8_t)(v > 255u ? 255u : v);
    }
    for (k = 0; k < n; k++) {
        uint32_t p = px[k];
        int      b, gr, r, m;

        if ((p >> 24) == 0u) {
            continue;
        }
        b  = ramp[p & 0xffu];
        gr = ramp[(p >> 8) & 0xffu];
        r  = ramp[(p >> 16) & 0xffu];
        m  = (b + gr + r) / 3;                   /* the [0x4efb04] i/3 table */

        b  = m + (((b  - m) * (int)g->saturation) >> 8);
        gr = m + (((gr - m) * (int)g->saturation) >> 8);
        r  = m + (((r  - m) * (int)g->saturation) >> 8);
        if (b  < 0) { b  = 0; } else if (b  > 255) { b  = 255; }
        if (gr < 0) { gr = 0; } else if (gr > 255) { gr = 255; }
        if (r  < 0) { r  = 0; } else if (r  > 255) { r  = 255; }

        p = (p & 0xff000000u) | ((uint32_t)r << 16) |
            ((uint32_t)gr << 8) | (uint32_t)b;
        /* The SAME near-black guard as the colour key, on the same 0xf8f8f8
         * test, with the same min colour.  That is why the guard exists in two
         * places: GRADING CAN CREATE A BLACK PIXEL. */
        if ((p & 0x00f8f8f8u) == 0u) {
            p = (p & 0xff000000u) | PLATE_MIN_COLOUR;
        }
        px[k] = p;
    }
}

/* The two exemptions, 0x00424217-0x0042422c.  `jc` on the low end and `ja` on
 * the high end, so the range is INCLUSIVE AT BOTH ENDS: plates 1798..1999,
 * plus plate 990 on its own.  1798..1999 is where the alpha-bearing plates
 * live and grading an alpha channel would be a bug. */
static int plate_grade_exempt(unsigned id)
{
    return id == 0x3deu || (id >= 0x706u && id <= 0x7cfu);
}

/* ---------------------------------------------------- the mip reducer */

/* FUN_004051a6, and it is the reason plat_gfx_tex_create takes a CHAIN.
 *
 * Per output texel it reads the 2x2 source block but SKIPS ANY TAP WHOSE BYTE
 * 3 IS ZERO (0x004051c8, ...e6, 0x00405205, ...28) and counts how many
 * contributed:
 *
 *   alpha_out  = (sum of the four alpha bytes) >> 2      always /4
 *   if alpha_out == 0: the texel is 0x00000000
 *   colour_out = sum of the CONTRIBUTING taps / count    1 as-is, 2 >>1,
 *                                                        3 the i/3 table, 4 >>2
 *
 * The alpha is a plain box filter but THE COLOUR IS AVERAGED ONLY OVER THE
 * VISIBLE TEXELS.  A naive 2x2 average drags the transparent texels' colour -
 * usually the keyed black of the colour-key pass - into every edge, and the
 * plate darkens as it minifies.  glGenerateMipmap does exactly that. */
static void plate_reduce(const uint32_t *src, unsigned w, unsigned h,
                         uint32_t *dst)
{
    unsigned x, y;
    unsigned dw = w / 2u, dh = h / 2u;

    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            const uint32_t t[4] = {
                src[(size_t)(y * 2u) * w + x * 2u],
                src[(size_t)(y * 2u) * w + x * 2u + 1u],
                src[(size_t)(y * 2u + 1u) * w + x * 2u],
                src[(size_t)(y * 2u + 1u) * w + x * 2u + 1u]
            };
            unsigned asum = 0, n = 0, i;
            unsigned b = 0, g = 0, r = 0, a;

            for (i = 0; i < 4u; i++) {
                asum += t[i] >> 24;
                if ((t[i] >> 24) != 0u) {
                    b += t[i] & 0xffu;
                    g += (t[i] >> 8) & 0xffu;
                    r += (t[i] >> 16) & 0xffu;
                    n++;
                }
            }
            a = asum >> 2;
            if (a == 0u) {
                dst[(size_t)y * dw + x] = 0u;
                continue;
            }
            switch (n) {
            case 1: break;
            case 2: b >>= 1; g >>= 1; r >>= 1; break;
            case 3: b /= 3u; g /= 3u; r /= 3u; break;   /* the i/3 table */
            default: b >>= 2; g >>= 2; r >>= 2; break;
            }
            dst[(size_t)y * dw + x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* ------------------------------------------------------------- the load */

/* The original's size ladder, [0x00564f5c]: five { level, pixels } records,
 * { 0,256 } { 1,128 } { 2,64 } { 3,32 } { 4,1024 }.  FUN_004199ed looks a
 * plate's width and height up in it and EVERY LOOKUP CAN FAIL FATALLY -
 * GURU 0x80a18741/42/44/45.  No shipped plate is 1024 in either axis. */
static int plate_size_representable(unsigned v)
{
    return v == 32u || v == 64u || v == 128u || v == 256u || v == 1024u;
}

static void plate_white(plate *p, int why)
{
    size_t i;

    p->fallback = why;

    /* 0x0040509e: the fallback is 256x256 of 0xffffffff - `MOV [0x4ef9a4],0x100`
     * / `MOV [0x4ef9a8],0x100` / `MOV EAX,0xffffffff` / `REP STOSD 0x10000`. */
    p->w = 256u;
    p->h = 256u;
    for (i = 0; i < 256u * 256u; i++) {
        p->pixels[i] = 0xffffffffu;
    }
}

static void plate_chain(plate *p)
{
    unsigned w = p->w, h = p->h;
    size_t   off = (size_t)w * h;

    p->level[0] = p->pixels;
    p->levels   = 1u;
    while (p->levels < PLATE_MAX_LEVELS && w > 1u && h > 1u &&
           (w & 1u) == 0u && (h & 1u) == 0u) {
        uint32_t *dst = p->pixels + off;

        plate_reduce(p->level[p->levels - 1u], w, h, dst);
        w /= 2u;
        h /= 2u;
        p->level[p->levels++] = dst;
        off += (size_t)w * h;
    }
}

void plate_rechain(plate *p)
{
    plate_chain(p);
}

/* ------------------------------------------------- the override packs */

static char ov_root[512];
static char ov_pack[64];
static unsigned ov_taken;

unsigned plate_overrides_taken(void)
{
    unsigned n = ov_taken;

    ov_taken = 0;
    return n;
}

void plate_set_overrides(const char *root, const char *pack)
{
    snprintf(ov_root, sizeof ov_root, "%s", root ? root : "");
    snprintf(ov_pack, sizeof ov_pack, "%s",
             pack && *pack ? pack : "default");
}

/* THE PORT'S: the size the shipped plate draws at - its PCX header
 * clamped to the request, as the decoder clamps it (TX0059's 320 is 256), or
 * the white fallback's 256x256 when there is no file.  Read into `buf`. */
static void plate_shipped_size(const char *files_root, unsigned id,
                               uint8_t *buf, size_t bufsz,
                               unsigned *w, unsigned *h)
{
    char     path[1024], err[128];
    FILE    *fh;
    size_t   len;
    pcx_info pi;

    *w = PLATE_REQUEST_W;
    *h = PLATE_REQUEST_H;
    if (plate_path(path, sizeof path, files_root, id, 0) != 0) return;
    fh = fopen(path, "rb");
    if (!fh) return;
    len = fread(buf, 1, bufsz, fh);
    fclose(fh);
    if (pcx_read_header(buf, len, &pi, err, sizeof err) != 0) return;
    *w = pi.w < PLATE_REQUEST_W ? pi.w : PLATE_REQUEST_W;
    *h = pi.h < PLATE_REQUEST_H ? pi.h : PLATE_REQUEST_H;
}

/* 1 and `p->pixels` filled (B G R A, alpha 0 or the PNG's) when an override
 * exists and decodes; *has_alpha says which.  0 = use the shipped plate.  A
 * PNG that exists but will not decode is reported on stderr and skipped -
 * a bad override must not cost the player the original texture.
 *
 * the size rule (the user's, 2026-10-25): an override in the "hd"
 * pack is EXACTLY twice the shipped plate's width and height; in any other
 * pack ("default" - same-size replacement art) exactly the same.  Anything
 * else is rejected and logged.  2 x 256 is PLATE_OVERRIDE_MAX. */
static int plate_override(plate *p, const char *files_root, unsigned id,
                          uint8_t *buf, size_t bufsz, int *has_alpha)
{
    const char *packs[2];
    unsigned    k, np = 0;

    if (!ov_root[0]) return 0;
    packs[np++] = ov_pack;
    if (strcmp(ov_pack, "default") != 0) packs[np++] = "default";

    for (k = 0; k < np; k++) {
        char     gfx[640], path[1024];
        size_t   len, n;
        FILE    *fh;
        int      w, h, ch;
        unsigned x, y, sw, sh, scale;
        unsigned char *img;
        uint8_t *png;
        long     fsz;

        snprintf(gfx, sizeof gfx, "%s/%s/gfx", ov_root, packs[k]);
        if (plate_path(path, sizeof path, gfx, id, 0) != 0) return 0;
        n = strlen(path);
        memcpy(path + n - 3, "png", 3);          /* tx<dddd>.pcx -> .png */
        fh = fopen(path, "rb");
        if (!fh) continue;
        /* Its own buffer, the file's size: a 512x512 RGBA PNG can pass the
         * 1 MiB that holds every shipped PCX (measured, the gate). */
        img = NULL;
        if (fseek(fh, 0, SEEK_END) == 0 && (fsz = ftell(fh)) > 0 &&
            fsz < 0x7fffffffL && fseek(fh, 0, SEEK_SET) == 0 &&
            (png = (uint8_t *)malloc((size_t)fsz)) != NULL) {
            len = fread(png, 1, (size_t)fsz, fh);
            img = stbi_load_from_memory(png, (int)len, &w, &h, &ch, 4);
            free(png);
        }
        fclose(fh);
        if (!img) {
            fprintf(stderr, "plate: override %s: %s\n", path,
                    stbi_failure_reason());
            continue;
        }
        plate_shipped_size(files_root, id, buf, bufsz, &sw, &sh);
        scale = strcmp(packs[k], "hd") == 0 ? 2u : 1u;
        if ((unsigned)w != scale * sw || (unsigned)h != scale * sh ||
            (unsigned)w > PLATE_OVERRIDE_MAX ||
            (unsigned)h > PLATE_OVERRIDE_MAX) {
            fprintf(stderr, "plate: override %s: %dx%d rejected, the \"%s\" "
                    "pack wants %ux%u (%ux the shipped %ux%u)\n", path, w, h,
                    packs[k], scale * sw, scale * sh, scale, sw, sh);
            stbi_image_free(img);
            continue;
        }
        p->w = (unsigned)w;
        p->h = (unsigned)h;
        for (y = 0; y < p->h; y++) {
            for (x = 0; x < p->w; x++) {
                const unsigned char *s = img + ((size_t)y * w + x) * 4u;
                uint32_t a = (ch == 4) ? s[3] : 0u;
                p->pixels[y * p->w + x] = (a << 24) | ((uint32_t)s[0] << 16) |
                                          ((uint32_t)s[1] << 8) | s[2];
            }
        }
        stbi_image_free(img);
        *has_alpha = (ch == 4);
        p->scale    = scale;
        p->override = 1;
        ov_taken++;
        return 1;
    }
    return 0;
}

/* The gravel override's two paths, files\b\gravel\tx<dddd>.pcx for the colour
 * and tx<dddd+1>.pcx for the component (0x00424626 / ECX = 0x005bdd00; the
 * game's own log prints the second).  Only 1000..1999 has the folder. */
int plate_gravel_path(char *out, size_t outlen, const char *files_root,
                      unsigned id, int alpha)
{
    if (id < 1000u || id >= 2000u) return 1;
    if ((size_t)snprintf(out, outlen, "%s/b/gravel/tx%04u.pcx", files_root,
                         alpha ? id + 1u : id) >= outlen) return 1;
    return 0;
}

int plate_gravel_applies(const char *files_root, unsigned id, int tyre)
{
    char  path[1024];
    FILE *fh;

    /* 0x00424621: `cmp eax,3 / js` is SIGNED, so the presets' tyre -1 skips;
     * then the existence probe 0x00402ca3 on the colour path alone. */
    if (tyre < 3 || plate_gravel_path(path, sizeof path, files_root, id, 0) != 0)
        return 0;
    fh = fopen(path, "rb");
    if (!fh) return 0;
    fclose(fh);
    return 1;
}

static int plate_load_impl(plate *p, const char *files_root, unsigned id,
                           int want_alpha, const plate_grade *g, int gravel,
                           char *err, size_t errlen);

int plate_load(plate *p, const char *files_root, unsigned id,
               int want_alpha, const plate_grade *g,
               char *err, size_t errlen)
{
    return plate_load_impl(p, files_root, id, want_alpha, g, 0, err, errlen);
}

int plate_load_tyre(plate *p, const char *files_root, unsigned id,
                    int want_alpha, const plate_grade *g, int tyre,
                    char *err, size_t errlen)
{
    return plate_load_impl(p, files_root, id, want_alpha, g,
                           plate_gravel_applies(files_root, id, tyre),
                           err, errlen);
}

static int plate_load_impl(plate *p, const char *files_root, unsigned id,
                           int want_alpha, const plate_grade *g, int gravel,
                           char *err, size_t errlen)
{
    char        path[1024];
    FILE       *fh;
    /* The largest shipped plate file is TX0233.PCX at 274.2 KiB (measured over
     * the 942-file corpus), so 1 MiB is nearly 4x headroom.  A file that did
     * not fit would be truncated by the fread and pcx_decode would then refuse
     * it with "run stream ends at N of M" rather than draw a half-picture. */
    static uint8_t file[1u << 20];
    static uint32_t scratch[PLATE_MAX_PIXELS];
    size_t      len;
    pcx_info    pi;
    int         have_alpha = 0;
    unsigned    aw = 0, ah = 0;

    memset(p, 0, sizeof *p);
    p->scale = 1u;

    /* 0x00424420-0x0042442c: FUN_0042441f skips id 0 and id -1 outright,
     * BEFORE the path is ever built.  Plate 0 is the empty-slot sentinel and
     * all 48 levels name it. */
    if (id == 0u) {
        plate_white(p, PLATE_FALLBACK_SENTINEL);
        plate_chain(p);
        return 0;
    }

    /* The alpha component FIRST, because the colour decode reuses the staging
     * buffer - 0x0040506b-0x00405084 decodes tx<id+1>.pcx, relocates it to
     * +0x40000, and only then decodes the colour. */
    if (want_alpha && (gravel ? plate_gravel_path(path, sizeof path, files_root, id, 1)
                              : plate_path(path, sizeof path, files_root, id, 1)) == 0) {
        fh = fopen(path, "rb");
        if (fh) {
            len = fread(file, 1, sizeof file, fh);
            fclose(fh);
            /* WHICH PLATES HAVE AN ALPHA COMPONENT IS THE ONE THING THE PORT
             * CANNOT TRANSCRIBE, AND GETTING IT WRONG IS SPECTACULAR.
             *
             * The original decides at DESCRIPTOR CREATION from the requested
             * texture ROLE - [descriptor+0x5c] = (role == 3), 0x00404ff5 - and
             * never from the file.  Who asks for role 3 is READ (2026-11-12,
             * FUN_004211B6, by texture id - the
             * HUD's ids only.  The two rules below select the same plates on
             * every path the port loads.  So a port
             * that simply tries tx<id+1>.pcx for every plate loads THE NEXT
             * PLATE as an alpha map, which leaves nonzero alpha everywhere,
             * suppresses the colour key, and paints the key's magenta across
             * every tree and horizon.  (Measured: that is exactly what the
             * first textured render of LEVEL61 did.)
             *
             * The test used instead is the one the shipped data supports: a
             * component is 8 bits by ONE plane, a colour plate is 8 by 3, and
             * the game's own error string says so - `Component colour/alpha
             * files must be 8bit` at 0x004d6a5f.  Over the whole corpus
             * exactly THREE of 942 files are single-plane (tx0901, tx1818,
             * tx1819 -) and they are precisely the
             * alpha components, so this selects the same plates the role does.
             * `python tools/plates.py sizes` is the population.
             * NOT for the HUD: tx1801,
             * tx1803, tx1805.. are 3-plane and ARE the inverted alpha of
             * tx1800, tx1802, tx1804..; this test misses them.
             * So a caller that KNOWS its plate has a component - the HUD,
             * whose ids setplatesall_q hard-wires - asks with
             * PLATE_ALPHA_FORCED, and the plane count is not consulted:
             * byte 0 of the decoded texel (blue) is read, as for one plane. */
            if (pcx_read_header(file, len, &pi, err, errlen) == 0 &&
                (pi.planes == 1u || want_alpha == PLATE_ALPHA_FORCED) &&
                pcx_decode_request(file, len, scratch, PLATE_MAX_PIXELS,
                                   PLATE_REQUEST_W, PLATE_REQUEST_H,
                                   &aw, &ah, err, errlen) == 0) {
                have_alpha = 1;
            }
            /* 0x00405088 clears [descriptor+0x5c] when the component fails to
             * load: the flag is self-healing and the plate falls back to the
             * colour key. */
        }
    }

    {
        int ov_alpha = 0;

        /* The graphics overrides are keyed by plate id, so one would put the
         * ordinary wheels back on a gravel-tyre car. */
        if (!gravel &&
            plate_override(p, files_root, id, file, sizeof file, &ov_alpha)) {
            if (!ov_alpha) {
                plate_colour_key(p->pixels, (size_t)p->w * p->h);
            }
            if (g && !plate_grade_exempt(id)) {
                plate_grade_apply(p->pixels, (size_t)p->w * p->h, g);
            }
            plate_chain(p);
            return 0;
        }
    }

    if ((gravel ? plate_gravel_path(path, sizeof path, files_root, id, 0)
                : plate_path(path, sizeof path, files_root, id, 0)) != 0) {
        if (err && errlen) {
            snprintf(err, errlen, "plate %u has no band", id);
        }
        return 1;
    }
    fh = fopen(path, "rb");
    if (!fh) {
        /* NOT AN ERROR.  35 (band, plate) pairs that a level's texinfo names
         * have no file on disk and the shipped data relies on the fallback. */
        plate_white(p, PLATE_FALLBACK_NO_FILE);
        plate_chain(p);
        return 0;
    }
    len = fread(file, 1, sizeof file, fh);
    fclose(fh);

    if (pcx_read_header(file, len, &pi, err, errlen) != 0) {
        return 1;
    }
    /* THE DECODER CLIPS.  0x00411775 clamps the decoded width to the request,
     * which 0x0040509e sets to 0x100, and the decode loop then stores only
     * that many pixels per row while CONSUMING BytesPerLine of them.  So
     * TX0059.PCX - 320x256, named by four levels' manifests and by object
     * model 74 - decodes as its LEFT 256 COLUMNS and the decode SUCCEEDS.
     *
     * This is the fix called for: the port took
     * section 9's reading 1 and drew such a plate white, and FUN_00411704 says
     * reading 2.  plate.h has the history.
     *
     * The size ladder below therefore sees the CLAMPED size, never the file's,
     * and over the shipped corpus it can no longer fire at all - 942 of 942
     * plates decode to 256x256 or 128x128 once clamped.  It is kept because it
     * is the original's own test (FUN_004199ed against the five-record table
     * at 0x00564f5c, and every lookup there can GURU), because a plate that
     * really did miss the ladder must not reach plat_gfx_tex_create, and
     * because "unreachable over this corpus" is a measurement that can go
     * stale.  `python tools/plates.py sizes` is the population.
     *
     * DECODE FIRST, THEN THE LADDER, and that order is the original's: the
     * ladder lookup is in FUN_004199ED, the UPLOAD, which runs after
     * FUN_0040509D has already called pcx_load.  So a plate that both misses
     * the ladder AND will not decode fails as a decode here, which is what
     * the original does too.  No shipped plate is either. */
    if (pcx_decode_request(file, len, p->pixels, PLATE_MAX_PIXELS,
                           PLATE_REQUEST_W, PLATE_REQUEST_H,
                           &p->w, &p->h, err, errlen) != 0) {
        return 1;
    }
    if (!plate_size_representable(p->w) || !plate_size_representable(p->h)) {
        plate_white(p, PLATE_FALLBACK_BAD_SIZE);
        plate_chain(p);
        return 0;
    }

    /* The component is decoded under the SAME request, so a colour plate and
     * its component can only differ in size if the two FILES differ.  Three
     * shipped components exist and all three match their colour plate; a
     * mismatch would walk `scratch` past what the component wrote, so it is
     * refused rather than trusted.  0x00405088 clears [descriptor+0x5c] when
     * the component will not load, and the plate then falls back to the colour
     * key - which is what dropping `have_alpha` here does. */
    if (have_alpha && (aw != p->w || ah != p->h)) {
        have_alpha = 0;
    }
    if (have_alpha) {
        plate_alpha_plane(p->pixels, scratch, (size_t)p->w * p->h);
    } else {
        plate_colour_key(p->pixels, (size_t)p->w * p->h);
    }
    if (g && !plate_grade_exempt(id)) {
        plate_grade_apply(p->pixels, (size_t)p->w * p->h, g);
    }
    plate_chain(p);
    return 0;
}

/* ----------------------------------------------------------- the upload */

plat_tex plate_upload(const plate *p)
{
    /* THE ONE SWIZZLE.  The decoded pixel is 0xAARRGGBB - B,G,R,A in memory -
     * and plat.h section 5 documents its texture buffer as R,G,B,A, which is
     * GL_RGBA/GL_UNSIGNED_BYTE without a swizzle in the backend.
     *
     * suggests GL_BGRA where
     * GL_EXT_texture_format_BGRA8888 exists.  This port does not take that
     * route: GLES2 does not guarantee the extension, plat.h's texture contract
     * is already fixed and documented, and the cost of getting it wrong is
     * INVISIBLE - red and blue swap and nothing else looks amiss.  One pass
     * over the pixels at load time buys a backend that cannot be wrong about
     * it, and the document itself calls this a load-time one-off. */
    static uint32_t out[PLATE_MAX_PIXELS * 2u];
    const uint32_t *lv[PLATE_MAX_LEVELS];
    size_t   off = 0;
    unsigned i;
    unsigned w = p->w, h = p->h;

    /* DIAGNOSTIC, not the original's code: RALLY_PLATE_555 stores each
     * colour channel at 5 bits, widened by bit replication, which is what an
     * x555 / 1555 destination (a 16bpp display)
     * holds.  Off by default; measures it. */
    static int q555 = -1;
    if (q555 < 0) q555 = getenv("RALLY_PLATE_555") != NULL;

    for (i = 0; i < p->levels; i++) {
        size_t n = (size_t)w * h, k;

        lv[i] = out + off;
        for (k = 0; k < n; k++) {
            uint32_t c = p->level[i][k];
            if (q555) {
                uint32_t q = c & 0xff000000u;
                int s;
                for (s = 0; s < 24; s += 8) {
                    uint32_t v = ((c >> s) & 0xffu) >> 3;
                    q |= ((v << 3) | (v >> 2)) << s;
                }
                /* ...and FUN_00405103's min colour is x555's lowest step,
                 * (8, 8, 8), where the 32bpp port's guard is 0x010101 */
                if ((q & 0xff000000u) && !(q & 0x00ffffffu)) q |= 0x00080808u;
                c = q;
            }
            out[off + k] = (c & 0xff00ff00u) |
                           ((c & 0x00ff0000u) >> 16) |
                           ((c & 0x000000ffu) << 16);
        }
        off += n;
        w /= 2u;
        h /= 2u;
    }
    {
        plat_tex t = plat_gfx_tex_create(p->w, p->h, lv, p->levels);
        /* override art takes the remaster's filtering */
        if (p->override) plat_gfx_tex_smooth(t, p->levels);
        return t;
    }
}
