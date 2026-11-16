/* pcx.h - ZSoft PCX version 5, as a buffer-taking parser.
 *
 * The eighth format in the C toolkit, and the
 * one that stands between the stage renderer and textured pixels: a texture
 * plate IS a PCX file.
 * - `plate id -> files\<band>\tx<dddd>.pcx`, 942 of them.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT, BECAUSE THE DISTINCTION MATTERS HERE
 * --------------------------------------------------------------------
 * Every other parser in tools/formats/ is a TRANSCRIPTION of one of the
 * game's own readers, at the instruction.  THIS ONE IS NOT.  The game's PCX
 * decoder is FUN_00411704 and it has never been read -
 * section 8 and both say so, and both nominate it
 * for work this ticket did not need.
 *
 * So this is an implementation of the PUBLISHED PCX SPECIFICATION, and the
 * evidence that it is the right one is of a different kind: it is the corpus.
 * All 942 shipped plates decode with the run stream landing EXACTLY on the
 * end of the pixel buffer - no slack, no overrun - which a wrong reading of
 * the RLE arm or the scanline stride does not survive.  pcx_decode returns an
 * error rather than a picture when it does not land exactly, so that property
 * is checked on every file rather than assumed.  Over the corpus, measured:
 *
 *     manufacturer 10, version 5, encoding 1, bpp 8   942 of 942
 *     planes 3 (24-bit colour)                        939
 *     planes 1 (8-bit + VGA palette)                    3   - the alpha
 *     bytes_per_line == width, xmin == ymin == 0      942 of 942
 *
 * THE PALETTE IS APPLIED, and that is a reading rather than a transcription.
 * The three one-plane files carry a 0x0c-marked 256x3 VGA palette at EOF, and
 * in TX1818.PCX that palette is a SEVENTEEN-STEP GREY RAMP - 0, 16, 32 ... 240
 *with two stray entries.  The image therefore uses indices 0..16 only.
 * FUN_0040518a takes byte 0 of the decoded pixel as the alpha plane
 * if the decoder did NOT apply the palette
 * that byte would span 0..16 and every alpha-bearing plate would come out
 * effectively opaque.  Applying it gives the 0..240 ramp the file was authored
 * as.  The argument is from the DATA, not from the decoder, and it is written
 * here rather than left implicit because FUN_00411704 could still settle it.
 *
 * BYTE ORDER.  pcx_decode writes 0xAARRGGBB - B, G, R, A in memory - which is
 * the order the game's own staging buffer is in (three independent witnesses) and therefore the order every constant in the
 * colour-key, grade and mip passes is written against.  It is NOT plat.h's
 * texture order; port/stage/plate.c swizzles once, at upload.
 *
 * Held byte-identical to `python tools/plates.py decode` by check.sh.
*/
#ifndef RAL_PCX_H
#define RAL_PCX_H

#include <stddef.h>
#include <stdint.h>

#define PCX_HDR_SIZE      128
#define PCX_MANUFACTURER  0x0a
#define PCX_VERSION       5
#define PCX_ENCODING_RLE  1
#define PCX_PAL_MARKER    0x0c
#define PCX_PAL_BYTES     768

/* The widest shipped plate is 320 (TX0059.PCX) and bytes_per_line == width in
 * all 942, so this is 3x headroom rather than a guess.  pcx_decode refuses a
 * scanline wider than this rather than overrunning. */
#define PCX_MAX_BYTES_PER_LINE 1024

typedef struct {
    unsigned w, h;
    unsigned planes;          /* 1 or 3 */
    unsigned bits_per_pixel;  /* 8 in every shipped plate */
    unsigned bytes_per_line;
    int      has_palette;     /* a 0x0c-marked VGA palette at EOF */
} pcx_info;

/* Returns 0 on success; non-zero fills `err` with plates.py's exact wording. */
int pcx_read_header(const void *buf, size_t len, pcx_info *pi,
                    char *err, size_t errlen);

/* The 800x600 canvas at [0x005154EC]/[0x005154F0], a hard ceiling under every
 * request in FUN_00411749.  The plate loader asks for 256x256, so for a plate
 * it never binds; it is here because it is part of the clamp, not because any
 * caller in this toolkit reaches it. */
#define PCX_CANVAS_W 800u
#define PCX_CANVAS_H 600u

/* Decodes into `out`, which must hold at least w*h uint32.  Alpha is forced
 * to 0xff; the plate pipeline overwrites it. */
int pcx_decode(const void *buf, size_t len, uint32_t *out, size_t outcap,
               char *err, size_t errlen);

/* THE SAME DECODE UNDER A REQUESTED SIZE, WHICH IS WHAT THE GAME ALWAYS DOES.
 *
 * FUN_00411749 clamps the decoded size TWICE - to the caller's request at
 * [0x004EF9A4]/[0x004EF9A8], then to the 800x600 canvas - and BOTH decoder
 * arms then STORE only [0x00512C9C] pixels per row while CONSUMING
 * BytesPerLine of them:
 *
 *   004119ea  CMP ECX,[0x00512C9C]   ; the CLAMPED width, 1-plane arm
 *   004119f0  JNC 0x004119f7         ; past it: decode the run byte, DROP it
 *   004119f2  MOV [EDI],EAX / ADD EDI,4
 *   004119f9  CMP ECX,[0x00513154]   ; ...but keep going to BytesPerLine
 *
 *   00411a8a  CMP ECX,[0x00512C9C]   ; the same pair of tests, 3-plane arm
 *   00411a90  JAE 0x00411a95
 *   00411a92  MOV [EDI + ECX*4],AL
 *   00411a96  CMP ECX,[0x00513154]
 *
 * and both row loops run to [0x00512CA0], the CLAMPED height.  SO A FILE
 * LARGER THAN THE REQUEST DECODES AS ITS TOP-LEFT CORNER AND THE DECODE
 * RETURNS SUCCESS.
 *
 * `req_w`/`req_h` of 0 mean "no request", which is `pcx_decode` above and is
 * a property of this toolkit rather than of the game: every call the game
 * makes sets both.  `dw`/`dh`, when non-NULL, receive the DECODED size, which
 * is what the caller must upload - not the file's.
 *
 * One consequence is worth stating because it is the only place this decoder
 * stops agreeing with itself: when the request clips the HEIGHT, the rows past
 * it are never consumed, so the run stream does not land on the end of the
 * file and the "lands exactly" check cannot be made.  It is therefore made
 * only when the full height was decoded.  No shipped plate is taller than the
 * 256 the plate loader asks for, so the corpus check is unweakened. */
int pcx_decode_request(const void *buf, size_t len, uint32_t *out,
                       size_t outcap, unsigned req_w, unsigned req_h,
                       unsigned *dw, unsigned *dh, char *err, size_t errlen);

/* a ONE-plane file's palette INDICES, not its colours, with the
 * palette itself in `pal` as 0x00RRGGBB.  The frontend's font sheets are cut
 * on indices (FUN_0041185A keeps them; the glyphs are drawn through a LUT),
 * and a palette with two equal colours makes the colour decode irreversible.
 * The same run walk as pcx_decode, and the same "lands exactly" check.  A
 * three-plane file, or one with no palette, is refused. */
int pcx_decode_index(const void *buf, size_t len, uint8_t *out, size_t outcap,
                     uint32_t pal[256], unsigned *dw, unsigned *dh,
                     char *err, size_t errlen);

#endif
