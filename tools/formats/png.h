/* png.h - PNG, as a buffer-taking parser.
 *
 * The frontend's backgrounds are files\g\graphics\screens\*.png, 800x600,
 * 8-bit RGB, non-interlaced (all 66 in the retail install, measured with
 * `file`).  The game decodes them in FUN_00411573, which has not been read;
 * like pcx.c this is an implementation of the PUBLISHED specification (PNG,
 * RFC 2083; zlib RFC 1950; DEFLATE RFC 1951), and the evidence that it is
 * the right one is the pictures: the frames match the user's screenshots
 * of the original to the 5-6-5 bit wherever the background shows
 *
 *
 * WHAT IT ACCEPTS: bit depth 8; colour types 2 (RGB), 6 (RGBA), 0 (grey),
 * 3 (indexed, with PLTE); no interlace.  Anything else is REFUSED with a
 * reason - a decoder that guessed would hide the fault.  The CRCs and the
 * zlib Adler-32 are CHECKED, so a corrupt file is an error, not a picture.
 *
 * Output is 0xAARRGGBB (B, G, R, A in memory), the order pcx.c and gfx2d use.
*/
#ifndef RAL_PNG_H
#define RAL_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Reads the IHDR only.  0 on success. */
int png_size(const void *buf, size_t len, unsigned *w, unsigned *h,
             char *err, size_t errlen);

/* Decodes into `out`, which must hold w*h uint32.  0 on success. */
int png_decode(const void *buf, size_t len, uint32_t *out, size_t outcap,
               unsigned *w, unsigned *h, char *err, size_t errlen);

#endif
