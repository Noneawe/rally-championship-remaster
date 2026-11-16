/* pyrandom.h - MT19937 plus the exact shuffle CPython's `random` module
 * performs, so the C tool and tools/ro3.py can be diffed byte-for-byte
 * including the shuffled CONTROL column of `normals`.
 *
 * This exists only to make the two instruments comparable. Nothing in the
 * port needs it.
 */
#ifndef RAL_PYRANDOM_H
#define RAL_PYRANDOM_H

#include <stddef.h>
#include <stdint.h>

/* random.seed(n) for a small non-negative int, which is what ro3.py uses. */
void py_seed(uint32_t n);

/* random.shuffle(list) - Fisher-Yates downwards using _randbelow(i+1). */
void py_shuffle(int32_t *a, size_t n);

#endif /* RAL_PYRANDOM_H */
