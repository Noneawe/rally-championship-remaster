/* plate.h - a texture plate, from a filename to a plat_tex.
 *
 * This is everything the original does between "the level names plate 137"
 * and "there is a texture bound", and all of it is transcribed at the
 * instruction from two documents that were written for exactly this:
 *
 *   the band table, the path digits,
 *                                         the white fallback, the per-level
 *                                         colour grade and its two exemptions
 *   FUN_004199ed - the colour key, the
 *                                         inverted alpha plane, the grade's
 *                                         arithmetic, the alpha-aware mip
 *                                         reducer, and THE CHANNEL ORDER
 *
 * FOUR THINGS HERE ARE NOT GUESSABLE FROM THE FILE FORMAT, and every one of
 * them is a silent wrongness rather than a crash.  They are the reason this
 * file exists instead of "decode the PCX and call glTexImage2D":
 *
 *   1. THE DECODED PIXEL IS B G R A IN MEMORY - 0xAARRGGBB.  GL_RGBA reads it
 *      as R,G,B,A and swaps red and blue, and NOTHING ELSE LOOKS WRONG.
 * three independent witnesses.
 *   2. A PLATE WITH NO ALPHA COMPONENT IS COLOUR-KEYED ON MAGENTA, on the top
 *      five bits of each channel, with a near-black guard that stops a black
 *      texel quantising into the key.  Section 5.1.
 *   3. A PLATE WITH ONE HAS ITS ALPHA INVERTED - the file is a TRANSPARENCY
 *      map, `alpha = 255 - byte0`.  Section 5.2.  Copy it straight through and
 *      every alpha-bearing plate comes out exactly backwards.
 *   4. THE MIP REDUCER AVERAGES COLOUR OVER THE NON-TRANSPARENT TAPS ONLY.
 *      Section 7.  glGenerateMipmap drags the keyed black into every edge and
 *      darkens every colour-keyed plate as it minifies, which is why
 *      plat_gfx_tex_create now takes a CHAIN rather than a level count.
 *
 * WHAT IS DELIBERATELY NOT HERE, and is named rather than left as a gap:
 *
 *  THE GRAVEL OVERRIDE is NOT in plate_path or plate_load: it is per CAR
 *     (a tyre >= 3 car's livery sheet), so it
 *     is plate_load_tyre below, and rally.c's car atlas is its only caller.
 *  THE `files\m` UNITS-DIGIT ANOMALY.  0x00424503 writes plate 901's units
 *     digit from AL (the TENS) rather than DL, so the original resolves plate
 *     901's colour path to tx0900.pcx.  plate_path writes the correct name;
 *     whether the bug is ever reached is unestablished (section 3) and the
 *     shipped corpus cannot reach it.
 *  THE 320x256 PLATE, AT ITS OWN SIZE.  TX0059.PCX is named by the
 *     manifests of four shipped levels and by object model 74, and the
 *     original's size ladder cannot represent 320.  The decoder never offers
 *     it 320: `pcx_setup_dest` CLAMPS to the 256x256 the plate loader asked
 *     for and the decode loop stores only that many pixels per row while
 *     consuming BytesPerLine of them, so the game gets the plate's LEFT 256
 *     COLUMNS and a success return.  plate_load does the same, through
 *     pcx_decode_request.
 *
 *     This port drew it WHITE until 2026-09-21 -'s
 *     reading 1, chosen as "the reading that needs no mechanism the original
 *     is not already known to have" and because it kept every texture a power
 *     of two.  Both arguments were sound and neither was a measurement;
 *     reading FUN_00411704 settled it the other way, standing rule 1,
 *
 *  THE RESIDENCY CACHE at 0x0058e6b0 - a plate is reloaded only when a
 *     slot's id CHANGES.  A caller that reloads unconditionally is correct and
 *     slower; this header leaves the caching to the caller because the port
 *     has no level-change path yet.
*/
#ifndef RAL_PLATE_H
#define RAL_PLATE_H

#include <stddef.h>
#include <stdint.h>

#include "plat.h"

/* THE REQUEST.  `0x0040509e` sets both before every plate it loads:
 *
 *   0040509e  MOV [0x4ef9a4],0x100      the requested WIDTH
 *            MOV [0x4ef9a8],0x100       the requested HEIGHT
 *            MOV EAX,0xffffffff / REP STOSD 0x10000     pre-fill white
 *
 * and `pcx_setup_dest` CLAMPS the decoded size to it rather than refusing a
 * larger file (pcx.h).  So 256x256
 * is a ceiling on every decoded plate, not merely the fallback's size, and the
 * staging buffer is exactly that - it used to be 320 wide, to hold TX0059 at
 * its own size, back when this port drew that plate white. */
#define PLATE_REQUEST_W  256u
#define PLATE_REQUEST_H  256u
#define PLATE_MAX_W      PLATE_REQUEST_W
#define PLATE_MAX_H      PLATE_REQUEST_H

/* THE PORT'S: the largest side a graphics override may have - the HD
 * tier, twice the original's 256 (an "hd" override is exactly 2x its plate).  UHD (1024 and up) is a later tier whose
 * cap measures.  The staging buffer is sized for this, not for the
 * request: a shipped plate still decodes at most PLATE_REQUEST_W x _H. */
#define PLATE_OVERRIDE_MAX 512u
#define PLATE_MAX_PIXELS (PLATE_OVERRIDE_MAX * PLATE_OVERRIDE_MAX)

/* `CMP EBP,9 / JA -> fail` at 0x00404fac.  256 -> 1 is nine levels; an
 * override's 512 -> 1 is ten. */
#define PLATE_MAX_LEVELS 10

/* `plate.fallback` */
#define PLATE_FALLBACK_NONE     0
#define PLATE_FALLBACK_NO_FILE  1
#define PLATE_FALLBACK_BAD_SIZE 2
#define PLATE_FALLBACK_SENTINEL 3

typedef struct {
    unsigned w, h;
    unsigned levels;                     /* 1 .. PLATE_MAX_LEVELS            */
    /* Non-zero when this plate is the white fallback rather than a decoded
     * file: 1 = no file, 2 = a size the original's ladder cannot represent,
     * 3 = the empty-slot sentinel (id 0).  Reported rather than left for a
     * caller to sniff out of the pixels - a white 256x256 plate and the
     * fallback are indistinguishable by looking, and that is standing rule
     * 1's trap in miniature. */
    int      fallback;
    /* THE PORT'S (/03): 1 for the shipped plate or a "default"-pack
     * override, 2 for an "hd"-pack override - texels per shipped texel.  A
     * caller that addresses texels by the shipped layout (the HUD's car
     * picture) multiplies by it.  `override` is non-zero when the pixels came
     * from an override PNG. */
    unsigned scale;
    int      override;
    uint32_t *level[PLATE_MAX_LEVELS];   /* into `pixels`, B G R A per texel */
    uint32_t  pixels[PLATE_MAX_PIXELS * 2u];
} plate;

/* The per-level colour grade, LOCALDAT +0x64c and +0x650.  0x100 is neutral
 * and the pair is 8.8 fixed point - FUN_004052e9 bails when both are 0x100.
 * `python tools/plates.py grade` prints them in this order. */
typedef struct {
    unsigned saturation;   /* LOCALDAT +0x64c -> [0x004efa00] */
    unsigned brightness;   /* LOCALDAT +0x650 -> [0x004ef9fc] */
} plate_grade;

#define PLATE_GRADE_NEUTRAL 0x100u

/* `files/<band>/tx<dddd>.pcx`, built the way FUN_00424462 builds it: four
 * decimal digits, most significant first, patched into a template whose second
 * character is a LITERAL x.  Returns 0 on success.  `alpha` non-zero asks for
 * the id+1 component path (0x00424525's bare INC EAX). */
int plate_path(char *out, size_t outlen, const char *files_root,
               unsigned id, int alpha);

/* `want_alpha`: 0 = colour key only; 1 (PLATE_ALPHA_IF_ONE_PLANE) = use
 * tx<id+1>.pcx as the inverted alpha when it is a single-plane file - the
 * stage's and the objects' rule, plate.c has the why; 2 (PLATE_ALPHA_FORCED)
 * = use it whatever its plane count - the HUD's plates, whose components are
 * 3-plane. */
#define PLATE_ALPHA_IF_ONE_PLANE 1
#define PLATE_ALPHA_FORCED       2

/* Load, key-or-alpha, grade and build the mip chain for one plate.
 *
 * A MISSING FILE IS NOT AN ERROR: FUN_0040509d pre-fills its staging buffer
 * with 0xffffffff and falls back to 256x256, so an absent plate draws OPAQUE
 * WHITE and the level loads regardless.  The shipped data relies on it - 35
 * (band, plate) pairs that a level's texinfo names have no file.  This returns
 * 0 and a white plate in that case, and non-zero only when a file is present
 * and will not decode. */
int plate_load(plate *p, const char *files_root, unsigned id,
               int want_alpha, const plate_grade *g,
               char *err, size_t errlen);

/* THE GRAVEL OVERRIDE: a CAR whose tyre
 * (`[0x5d64c8 + car*4]`, the raw setup tyre) is >= 3 loads its livery sheet
 * from files/b/gravel - the same sheet with rally wheels - when the file
 * exists.  `plate_gravel_applies` is the gate (id in 1000..1999, tyre >= 3,
 * the colour file there); `plate_load_tyre` is plate_load with it applied.
 * The component is `gravel/tx<id+1>.pcx`, and is simply absent for most of
 * the sheets, which colour-keys them (the original's self-healing flag).
 * Graphics overrides are not consulted for a gravel plate. */
int plate_gravel_path(char *out, size_t outlen, const char *files_root,
                      unsigned id, int alpha);
int plate_gravel_applies(const char *files_root, unsigned id, int tyre);
int plate_load_tyre(plate *p, const char *files_root, unsigned id,
                    int want_alpha, const plate_grade *g, int tyre,
                    char *err, size_t errlen);

/* Rebuilds the mip chain from level 0, after a caller has written into it
 * (the HUD's car picture).  THE PORT'S: the original writes only the
 * top surface (0x00419DAD locks the record's +0x14 alone). */
void plate_rechain(plate *p);

/* GRAPHICS OVERRIDES - THE PORT'S, not the original's (a remaster feature).
 * When set, plate_load first looks for `<root>/<pack>/gfx/<band>/tx<dddd>.png`,
 * then `<root>/default/gfx/...`, and decodes that INSTEAD of the PCX.  The
 * PNG then takes the shipped plate's path exactly: no alpha channel means
 * the magenta colour key and black guard; an alpha channel is used as-is
 * (opacity, NOT the PCX components' inverted transparency).  The level grade
 * applies either way, so replacement art is lit like the stage around it.
 * Its size is its own check, not the original's ladder: in the "hd"
 * pack exactly twice the shipped plate's width and height, in any other pack
 * exactly the same.  Any other size is logged and the shipped plate used.
 * `root` NULL or "" turns it off;
 * `pack` NULL or "" means "default" alone.  Both strings are copied. */
void plate_set_overrides(const char *root, const char *pack);

/* how many plates plate_load has taken from an override since the last
 * call, and resets the count.  A loader calls it once when it is done, and
 * logs it. */
unsigned plate_overrides_taken(void);

/* Upload the chain through plat.h.  This is where B,G,R,A becomes plat.h's
 * documented R,G,B,A - one swizzle, at load time. */
plat_tex plate_upload(const plate *p);

#endif /* RAL_PLATE_H */
