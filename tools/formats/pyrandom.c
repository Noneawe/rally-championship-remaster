/* pyrandom.c - see pyrandom.h.
 *
 * MT19937 as CPython ships it (_randommodule.c), seeded the way
 * random.seed(int) seeds it: abs(n) split into 32-bit little-endian words,
 * passed to init_by_array. getrandbits(k<=32) is the top k bits of one
 * output word; _randbelow(n) is rejection sampling on n.bit_length() bits.
 */

#include "pyrandom.h"

#define N 624
#define M 397
#define MATRIX_A   0x9908b0dfUL
#define UPPER_MASK 0x80000000UL
#define LOWER_MASK 0x7fffffffUL

static uint32_t mt[N];
static int mti = N + 1;

static void init_genrand(uint32_t s)
{
    mt[0] = s;
    for (mti = 1; mti < N; mti++)
        mt[mti] = (uint32_t)(1812433253UL * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + (uint32_t)mti);
}

static void init_by_array(const uint32_t *key, size_t klen)
{
    size_t i = 1, j = 0, k;
    init_genrand(19650218UL);
    k = (N > klen) ? N : klen;
    for (; k; k--) {
        mt[i] = (uint32_t)((mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525UL))
                           + key[j] + (uint32_t)j);
        i++; j++;
        if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
        if (j >= klen) j = 0;
    }
    for (k = N - 1; k; k--) {
        mt[i] = (uint32_t)((mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941UL))
                           - (uint32_t)i);
        i++;
        if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
    }
    mt[0] = 0x80000000UL;
}

static uint32_t genrand_uint32(void)
{
    uint32_t y;
    static const uint32_t mag01[2] = { 0x0UL, MATRIX_A };

    if (mti >= N) {
        int kk;
        if (mti == N + 1) init_genrand(5489UL);
        for (kk = 0; kk < N - M; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        for (; kk < N - 1; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        y = (mt[N - 1] & UPPER_MASK) | (mt[0] & LOWER_MASK);
        mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1UL];
        mti = 0;
    }
    y = mt[mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y;
}

void py_seed(uint32_t n)
{
    uint32_t key[1];
    key[0] = n;
    init_by_array(key, 1);
}

/* getrandbits(k) for 0 < k <= 32. */
static uint32_t getrandbits(int k)
{
    if (k <= 0) return 0;
    return genrand_uint32() >> (32 - k);
}

static int bit_length(uint32_t n)
{
    int b = 0;
    while (n) { b++; n >>= 1; }
    return b;
}

/* _randbelow_with_getrandbits. k is n.bit_length(), NOT (n-1).bit_length() -
 * getting that wrong changes every draw. */
static uint32_t randbelow(uint32_t n)
{
    int k;
    uint32_t r;
    if (!n) return 0;
    k = bit_length(n);
    r = getrandbits(k);
    while (r >= n) r = getrandbits(k);
    return r;
}

void py_shuffle(int32_t *a, size_t n)
{
    size_t i;
    if (n < 2) return;
    for (i = n - 1; i >= 1; i--) {
        uint32_t j = randbelow((uint32_t)(i + 1));
        int32_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}
