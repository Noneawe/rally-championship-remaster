/* wear.c - the car's wear between races (wear.h) */
#include "wear.h"

#include <math.h>
#include <string.h>

#define CAR_DAM0 0x2B8                /* car+0x2B8: dam[0]                 */

/* The launcher, 0x00480A68..: P+off -> l[k], in the code's order (P+0x104
 * and P+0x1A0 both go to l[22], the later one standing). */
static const struct { uint16_t p; uint8_t k; } LAUNCH[] = {
    { 0x0D4, 0 }, { 0x0D8, 1 }, { 0x0DC, 2 }, { 0x0E0, 3 }, { 0x0E4, 4 },
    { 0x0E8, 5 }, { 0x0EC, 6 }, { 0x0F0, 7 }, { 0x0F4, 8 }, { 0x0F8, 9 },
    { 0x0FC, 10 }, { 0x100, 11 }, { 0x104, 22 }, { 0x108, 12 }
    /* then P+0x17C + 4j -> l[13 + j], j = 0..35 (0x00480B70..) */
};

/* graham_to_shaun / shaun_to_graham: l[k] <-> dam[d], both in this order
 * (0x004455B1.. and 0x004467FE.. walk the same list). */
static const struct { uint8_t k, d; } CAR[] = {
    { 0, 0 },   { 2, 1 },   { 3, 22 },  { 4, 2 },   { 6, 3 },   { 7, 30 },
    { 8, 4 },   { 10, 5 },  { 40, 35 }, { 48, 43 }, { 43, 38 }, { 35, 29 },
    { 22, 15 }, { 1, 21 },  { 13, 6 },
    { 15, 8 },  { 16, 9 },  { 17, 10 }, { 18, 11 }, { 19, 12 }, { 20, 13 },
    { 21, 14 },                                       /* gear1..7          */
    { 23, 16 }, { 24, 17 }, { 25, 18 }, { 26, 19 },   /* whl1..4           */
    { 27, 20 },                                       /* bdywrk            */
    { 29, 23 }, { 30, 24 }, { 31, 25 }, { 32, 26 },   /* lights1..4        */
    { 33, 27 }, { 34, 28 },                           /* lightsf, lightsb  */
    { 36, 31 }, { 37, 32 }, { 38, 33 }, { 39, 34 },   /* susp1..4          */
    { 41, 36 }, { 42, 37 },                           /* brakef, brakeb    */
    { 44, 39 }, { 45, 40 }, { 46, 41 }, { 47, 42 }    /* tyre1..4          */
};

/* The return, 0x0048123A..0x0048144F: l[k] -> P+off, in order (no l[9]). */
static const struct { uint8_t k; uint16_t p; } RET[] = {
    { 0, 0x0D4 }, { 1, 0x0D8 }, { 2, 0x0DC }, { 3, 0x0E0 }, { 4, 0x0E4 },
    { 5, 0x0E8 }, { 6, 0x0EC }, { 7, 0x0F0 }, { 8, 0x0F4 }, { 10, 0x0FC },
    { 11, 0x100 }, { 22, 0x104 }, { 12, 0x108 }
    /* then l[13 + j] -> P+0x17C + 4j, j = 0..35 */
};

/* 0x004814B1..0x004814D7: the summaries after the sum */
static const struct { uint8_t k; uint16_t p; } SUMMARY[] = {
    { 40, 0x0F8 }, { 48, 0x108 }, { 43, 0x100 }, { 35, 0x0E8 }
};

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

void wear_from_record(wear_block *w, wear_rd_fn rd, void *ud, uint32_t P)
{
    size_t i;
    memset(w, 0, sizeof *w);
    for (i = 0; i < 8; i++)                       /* 0x00480A49 REP MOVSB  */
        w->oct[i] = rd(ud, P + 0x24Cu + 4u * (uint32_t)i);
    for (i = 0; i < NELEM(LAUNCH); i++)
        w->l[LAUNCH[i].k] = (int32_t)rd(ud, P + LAUNCH[i].p);
    for (i = 0; i < 36; i++)
        w->l[13 + i] = (int32_t)rd(ud, P + 0x17Cu + 4u * (uint32_t)i);
}

void wear_into_car(car_state *car, const wear_block *w)
{
    size_t i;
    for (i = 0; i < NELEM(CAR); i++)              /* FILD / FSTP           */
        car->dam[CAR[i].d] = (float)w->l[CAR[i].k];
    memcpy(car->caroctdamage, w->oct, sizeof car->caroctdamage);  /* PUSH / POP */
}

/* FISTP with the x87's round-to-nearest-even; a value it cannot hold is the
 * integer indefinite. */
static int32_t fistp32(float v)
{
    long double x = (long double)v;
    if (!(x > -2147483649.0L && x < 2147483648.0L)) return INT32_MIN;
    return (int32_t)llrintl(x);
}

void wear_from_car(wear_block *w, const car_state *car)
{
    size_t i;
    for (i = 0; i < NELEM(CAR); i++)
        w->l[CAR[i].k] = fistp32(car->dam[CAR[i].d]);
    memcpy(w->oct, car->caroctdamage, sizeof w->oct);
}

void wear_into_record(const wear_block *w, wear_wr_fn wr, void *ud, uint32_t P)
{
    uint32_t s, e;
    size_t i;
    for (i = 0; i < 8; i++)                       /* 0x0048121A REP MOVSB  */
        wr(ud, P + 0x24Cu + 4u * (uint32_t)i, w->oct[i]);
    for (i = 0; i < NELEM(RET); i++)
        wr(ud, P + RET[i].p, (uint32_t)w->l[RET[i].k]);
    for (i = 0; i < 36; i++)
        wr(ud, P + 0x17Cu + 4u * (uint32_t)i, (uint32_t)w->l[13 + i]);
    /* 0x00481455: EBX = whl1 + .. + whl4, DIV 10 (unsigned) unless 0;
     * EAX = bdywrk x 6 (IMUL), DIV 10 unless 0; EBX += EAX */
    s = (uint32_t)w->l[23] + (uint32_t)w->l[24] + (uint32_t)w->l[25]
        + (uint32_t)w->l[26];
    if (s != 0) s /= 10u;
    e = (uint32_t)w->l[27] * 6u;
    if (e != 0) e /= 10u;
    wr(ud, P + 0x0D8u, s + e);
    for (i = 0; i < NELEM(SUMMARY); i++)
        wr(ud, P + SUMMARY[i].p, (uint32_t)w->l[SUMMARY[i].k]);
}

void wear_print_maps(FILE *out)
{
    size_t i;
    fprintf(out, "launch P+0x24C oct0..7 (32 bytes)\n");
    for (i = 0; i < NELEM(LAUNCH); i++)
        fprintf(out, "launch P+0x%03X l%u\n", LAUNCH[i].p, LAUNCH[i].k);
    for (i = 0; i < 36; i++)
        fprintf(out, "launch P+0x%03X l%u\n", (unsigned)(0x17C + 4 * i),
                (unsigned)(13 + i));
    for (i = 0; i < NELEM(CAR); i++)
        fprintf(out, "in l%u car+0x%03X\n", CAR[i].k, CAR_DAM0 + 4u * CAR[i].d);
    for (i = 0; i < 8; i++)
        fprintf(out, "in oct%u car+0x%03X raw\n", (unsigned)i, (unsigned)(0x64 + 4 * i));
    for (i = 0; i < NELEM(CAR); i++)
        fprintf(out, "out car+0x%03X l%u\n", CAR_DAM0 + 4u * CAR[i].d, CAR[i].k);
    for (i = 0; i < 8; i++)
        fprintf(out, "out car+0x%03X oct%u raw\n", (unsigned)(0x64 + 4 * i), (unsigned)i);
    fprintf(out, "ret oct0..7 P+0x24C (32 bytes)\n");
    for (i = 0; i < NELEM(RET); i++)
        fprintf(out, "ret l%u P+0x%03X\n", RET[i].k, RET[i].p);
    for (i = 0; i < 36; i++)
        fprintf(out, "ret l%u P+0x%03X\n", (unsigned)(13 + i), (unsigned)(0x17C + 4 * i));
    fprintf(out, "ret sum(l23,l24,l25,l26)/10+l27*6/10 P+0x0D8\n");
    for (i = 0; i < NELEM(SUMMARY); i++)
        fprintf(out, "ret l%u P+0x%03X\n", SUMMARY[i].k, SUMMARY[i].p);
}
