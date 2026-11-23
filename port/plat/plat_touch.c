#include "plat_touch.h"
#include <string.h>

static void btn(touch_btn *b, int *n, float x, float y, float w, float h,
                unsigned pad, int axis, const char *label)
{
    touch_btn *t = &b[(*n)++];
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->pad = pad; t->axis = axis;
    strncpy(t->label, label, sizeof t->label - 1);
    t->label[sizeof t->label - 1] = '\0';
}

int plat_touch_layout(int w, int h, int race, touch_btn out[TOUCH_MAX_BTN])
{
    float u = (float)h / 10.0f, W = (float)w, H = (float)h;
    int n = 0;

    if (race == 2) {                 /* a yes / no question: big buttons */
        btn(out, &n, W * 0.5f - 6.0f * u, H - 4.2f * u, 5.0f * u, 3.0f * u,
            PLAT_PAD_A, 0, "YES");
        btn(out, &n, W * 0.5f + 1.0f * u, H - 4.2f * u, 5.0f * u, 3.0f * u,
            PLAT_PAD_B, 0, "NO");
        return n;
    }
    if (!race) {
        float cx = 3.0f * u, cy = H - 3.0f * u, s = 1.7f * u;
        btn(out, &n, cx - s / 2, cy - s * 1.5f, s, s, PLAT_PAD_UP,    0, "^");
        btn(out, &n, cx - s / 2, cy + s * 0.5f, s, s, PLAT_PAD_DOWN,  0, "v");
        btn(out, &n, cx - s * 1.5f, cy - s / 2, s, s, PLAT_PAD_LEFT,  0, "<");
        btn(out, &n, cx + s * 0.5f, cy - s / 2, s, s, PLAT_PAD_RIGHT, 0, ">");
        btn(out, &n, W - 3.2f * u, H - 3.4f * u, 2.4f * u, 2.4f * u,
            PLAT_PAD_A, 0, "OK");
        btn(out, &n, W - 6.0f * u, H - 2.6f * u, 2.0f * u, 2.0f * u,
            PLAT_PAD_B, 0, "ESC");
        return n;
    }
    btn(out, &n, W - 3.3f * u, H - 4.0f * u, 2.9f * u, 3.5f * u, 0, 1, "GAS");
    btn(out, &n, W - 6.5f * u, H - 3.0f * u, 2.9f * u, 2.5f * u, 0, 2, "BRK");
    btn(out, &n, W - 3.3f * u, H - 6.1f * u, 1.4f * u, 1.7f * u, PLAT_PAD_B, 0, "UP");
    btn(out, &n, W - 1.8f * u, H - 6.1f * u, 1.4f * u, 1.7f * u, PLAT_PAD_X, 0, "DN");
    btn(out, &n, W - 6.5f * u, H - 5.4f * u, 2.9f * u, 1.9f * u, PLAT_PAD_A, 0, "HB");
    btn(out, &n, W - 2.4f * u, 0.8f * u, 2.0f * u, 1.3f * u, PLAT_PAD_Y, 0, "CAM");
    btn(out, &n, W * 0.5f - 1.0f * u, 0.7f * u, 2.0f * u, 1.3f * u, PLAT_PAD_START, 0, "II");
    btn(out, &n, W * 0.5f + 1.4f * u, 0.7f * u, 2.4f * u, 1.3f * u, PLAT_PAD_BACK, 0, "ESC");
    return n;
}

static int inside(const touch_btn *b, float x, float y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

void plat_touch_eval(const touch_finger *f, int nf, int w, int h, int race,
                     plat_pad *p, unsigned *lit, float *steer)
{
    touch_btn b[TOUCH_MAX_BTN];
    int n = plat_touch_layout(w, h, race, b), i, k;
    float u = (float)h / 10.0f, st = 0.0f;
    unsigned l = 0;

    memset(p, 0, sizeof *p);
    p->present = 1;
    for (i = 0; i < nf; i++) {
        if (!f[i].down) continue;
        /* a drag that began in the left 45% steers (race only) */
        if (race == 1 && f[i].x0 < 0.45f * (float)w) {
            float s = (f[i].x - f[i].x0) / (2.2f * u);
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            st = s;
            continue;
        }
        for (k = 0; k < n; k++) {
            if (!inside(&b[k], f[i].x, f[i].y)) continue;
            l |= 1u << k;
            p->buttons |= b[k].pad;
            if (b[k].axis == 1) p->rt = 32767;
            if (b[k].axis == 2) p->lt = 32767;
        }
    }
    p->lx = (int)(st * 32767.0f);
    if (lit) *lit = l;
    if (steer) *steer = st;
}
