/* ext.h - the type the x87-faithful draw-side code computes in (spray.c,
 * wipers.c, driver.c via x87geom.h), as objlit.c's lit_ld does.
 *
 * On x86 `long double` IS the x87's 80-bit extended, so the rounding points
 * are the original's, and the gates (spray-vm, wiper-vm, ...) compare bit for
 * bit.  Elsewhere `long double` is no nearer the x87 (aarch64: IEEE 128-bit
 * quad, done in SOFTWARE, 50-500 cycles an operation): there `ext` is
 * `double`, the hardware's widest, and the libm calls are the double ones.
 * x86 results are unchanged; the handheld's draw may differ in the last bits
 * (a mask cell or a particle's place), which is what asks the eye
 * to check.  The rounding-mode calls (lrint, nearbyint) follow fesetround in
 * both, as the x87 follows its control word. */
#ifndef RALLY_GAME_EXT_H
#define RALLY_GAME_EXT_H

#include "x87.h"

#include <math.h>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
typedef long double ext;                 /* the x87's register              */
#define EXT_C(x)      x##L
#define ext_rint      nearbyintl
#define ext_lrint     lrintl
#define ext_llrint    llrintl
#define ext_sqrt      sqrtl
#define ext_fabs      fabsl
#define ext_fmod      fmodl
#define ext_sin       x87_sin
#define ext_cos       x87_cos
#else
typedef double ext;                      /* NOT the x87's 80 bits           */
#define EXT_C(x)      x
#define ext_rint      nearbyint
#define ext_lrint     lrint
#define ext_llrint    llrint
#define ext_sqrt      sqrt
#define ext_fabs      fabs
#define ext_fmod      fmod
#define ext_sin       sin
#define ext_cos       cos
#endif

#endif
