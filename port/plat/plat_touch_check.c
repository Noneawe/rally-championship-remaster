/* one runnable check for plat_touch: build with the port, run, exit code 0 */
#include "plat_touch.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    touch_btn b[TOUCH_MAX_BTN];
    touch_finger f[2] = {{0}};
    plat_pad p;
    unsigned lit;
    float st;
    int n = plat_touch_layout(1600, 720, 1, b), i;

    assert(n > 0 && n <= TOUCH_MAX_BTN);
    for (i = 0; i < n; i++)          /* everything on screen */
        assert(b[i].x >= 0 && b[i].y >= 0 &&
               b[i].x + b[i].w <= 1600 && b[i].y + b[i].h <= 720);
    /* a finger on GAS */
    f[0].down = 1; f[0].x0 = f[0].x = b[0].x + 5; f[0].y0 = f[0].y = b[0].y + 5;
    plat_touch_eval(f, 2, 1600, 720, 1, &p, &lit, &st);
    assert(p.rt == 32767 && p.lt == 0 && p.lx == 0 && (lit & 1));
    /* a left-side drag to the right steers right, not a button */
    f[1].down = 1; f[1].x0 = 200; f[1].y0 = 400; f[1].x = 400; f[1].y = 400;
    plat_touch_eval(f, 2, 1600, 720, 1, &p, &lit, &st);
    assert(p.lx == 32767 && st == 1.0f);
    /* menus: no steering zone, OK presses A */
    n = plat_touch_layout(1600, 720, 0, b);
    f[0].x0 = f[0].x = b[4].x + 5; f[0].y0 = f[0].y = b[4].y + 5; f[1].down = 0;
    plat_touch_eval(f, 2, 1600, 720, 0, &p, &lit, &st);
    assert((p.buttons & PLAT_PAD_A) && p.lx == 0);
    puts("plat_touch_check ok");
    return 0;
}
