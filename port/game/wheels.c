/* wheels.c - see wheels.h.  0x0045ECC5..0x0045F04D, transcribed. */

#include "wheels.h"
#include "ground.h"     /* the x87 model: sim_x87_* */

/* FUN_00425E1D, as the x87 runs it: three FMULs (exact, 24 x 24 bits), the
 * last two added first and the first added to that, each add rounded to the
 * 64-bit significand, then ONE rounding to float at the FSTP; row 3 then adds
 * the right matrix's translation.  objxform.c's ral_mat4_mul rounds every
 * operation to float, which differs from the original in the last bit or
 * two of a long chain (wheels_vm_check, trial 0: 2 ULP in a translation);
 * the wheel chain is six of them deep, so it is done here the original's
 * way.  Column 3 is not written (the original's), and is not compared. */
static void mat_mul_x87(ral_mat4 *out, const ral_mat4 *a, const ral_mat4 *b)
{
    int r, c;

    for (c = 0; c < 3; c++) {
        for (r = 0; r < 4; r++) {
            sim_x87 p1 = sim_x87_mul(sim_x87_from_float(a->m[r * 4 + 0]),
                                     sim_x87_from_float(b->m[0 * 4 + c]));
            sim_x87 p2 = sim_x87_mul(sim_x87_from_float(a->m[r * 4 + 1]),
                                     sim_x87_from_float(b->m[1 * 4 + c]));
            sim_x87 p3 = sim_x87_mul(sim_x87_from_float(a->m[r * 4 + 2]),
                                     sim_x87_from_float(b->m[2 * 4 + c]));
            out->m[r * 4 + c] = sim_x87_to_float(
                sim_x87_add(p1, sim_x87_add(p2, p3)));
        }
        out->m[12 + c] += b->m[12 + c];
    }
    out->m[3] = a->m[3];  out->m[7] = a->m[7];
    out->m[11] = a->m[11]; out->m[15] = a->m[15];
}

/* `imul ebx(0x10000) / idiv ecx(1000)`: a 64-bit product, truncated toward
 * zero, into a 32-bit register. */
static int32_t mm_to_fixed(int32_t mm)
{
    return (int32_t)(((int64_t)mm * 0x10000) / 1000);
}

uint32_t wheels_mask(const wheel_geom *g)
{
    uint32_t m = 0;
    int i;

    for (i = 0; i < 4; i++) {
        m |= 1u << ((uint32_t)g->bit[i] & 31u);
    }
    return m;
}

/* One wheel: translate (fx, fy, fz) - the wheel's centre to the origin -,
 * Rx(spin), Ry(steer), translate (bx, by, bz).  The original builds each with
 * FUN_00425DBE / 0x00425BF1 / 0x00425C3A and multiplies them in this order
 * through [0x0059EE54] and [0x0059EE94]. */
static void one_wheel(ral_mat4 *out, int fx, int fy, int fz, unsigned spin,
                      unsigned steer, int bx, int by, int bz)
{
    ral_mat4 t, rx, ry, a, b;

    ral_obj_translate_i(&t, fx, fy, fz);
    ral_obj_rot_x16(&rx, spin);
    mat_mul_x87(&a, &t, &rx);
    ral_obj_rot_y16(&ry, steer);
    mat_mul_x87(&b, &a, &ry);
    ral_obj_translate_i(&t, bx, by, bz);
    mat_mul_x87(out, &b, &t);
}

void wheels_build(ral_mat4 m[4], const wheel_geom *g, int32_t turn_drop,
                  unsigned steer, int32_t spin_f, int32_t spin_r,
                  const int32_t sus[4])
{
    int32_t x1 = mm_to_fixed(g->track + 0xC3);       /* [0x005BB90C]       */
    int32_t y  = mm_to_fixed(g->height) - turn_drop; /* [0x005BB910]       */
    int32_t zf = mm_to_fixed(g->front_z);            /* [0x005BB914]       */
    int32_t zr = mm_to_fixed(-g->rear_z);            /* [0x005BB918]       */

    /* the front pair: spin from car+0x1F8, steered */
    one_wheel(&m[0],  x1, -y, -zf, (unsigned)spin_f, steer, -x1, sus[0], zf);
    one_wheel(&m[1], -x1, -y, -zf, (unsigned)spin_f, steer,  x1, sus[1], zf);
    /* 0x0045EF2A zeroes the steer; the rear pair spins from car+0x1FC */
    one_wheel(&m[2],  x1, -y, -zr, (unsigned)spin_r, 0,     -x1, sus[2], zr);
    one_wheel(&m[3], -x1, -y, -zr, (unsigned)spin_r, 0,      x1, sus[3], zr);
}

/* 0x0045304F..0x0045307F: the view-cone cut a car's draw starts with.  x and
 * z are the car's origin through the view ([0x005F445C], [0x005F4464]: the
 * x87's FISTP of 0x0044B1AB's products, 16.16).  Behind the eye by more than
 * 4.0 (CMP z, 0xFFFC0000 / JS), or off to the side by more than the depth
 * plus 8.0 (|x| - 0x80000 against z, JNS), and the car is not drawn at all.
 * The two flag tests are the 32-bit subtractions' SF, wrapping as the
 * hardware does. */
int wheels_view_cut(int32_t x, int32_t z)
{
    uint32_t ax = (uint32_t)x;

    if ((int32_t)((uint32_t)z - 0xFFFC0000u) < 0) return 1;
    if (x < 0) ax = 0u - ax;
    ax -= 0x80000u;
    return (int32_t)(ax - (uint32_t)z) >= 0;
}
