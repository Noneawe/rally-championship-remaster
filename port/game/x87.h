/* x87.h - the x87's own FSIN and FCOS, for the code that must agree with the
 * original's to the last bit: the interpreter (x86vm.c) and the ports it
 * gates (wipers.c's load).
 *
 * On an x86 host they are the hardware's instructions, which is what the
 * original ran: libm's long double routines differ from the x87's microcode
 * in the last bits (x87_vm_check measured it, 2026-10-25).  Elsewhere (the
 * handheld) libm stands in - close, NOT exact.  Both honour the host's
 * rounding mode (fesetround), as the x87 honours its control word. */
#ifndef RALLY_GAME_X87_H
#define RALLY_GAME_X87_H

#include <math.h>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#define X87_NATIVE 1
static inline long double x87_sin(long double x) { __asm__("fsin" : "+t"(x)); return x; }
static inline long double x87_cos(long double x) { __asm__("fcos" : "+t"(x)); return x; }
#else
static inline long double x87_sin(long double x) { return sinl(x); }
static inline long double x87_cos(long double x) { return cosl(x); }
#endif

#endif
