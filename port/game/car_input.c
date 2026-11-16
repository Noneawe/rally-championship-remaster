/* car_input.c - the live source of the car's four device words.
 * car_input.h says what this is the stand-in for. */

#include "car_input.h"
#include "car_keys.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FUN_00408D37 from 0x00408D4A, the keyboard arms only.  Each slot's key
 * test is TEST BYTE [EBP + 0x4F1AA4],0x80 (the DirectInput key state), and
 * a key that is down takes the digital arm:
 *
 *   EBX = POV << 24 | FUN_004087E0()     no joystick: 0 | 0xE00, the "no
 *                                        shifter" code the gearbox reads as
 *                                        "no gear selected directly"
 *   slot 0 down: ECX = -0x100, EBX |= 0x1000      else ECX = 0
 *   slot 1 down: EDX = +0x100, EBX |= 0x1000      else EDX = 0
 *   ECX += EDX  (0x00408E53 - both held cancel), and with 0x1000 set a
 *               non-zero ECX is put back to -0x100 / +0x100 by its sign
 *   slot 2 down: EAX = 0x100, EBX |= 0x4000
 *   slot 3 down: EDX = 0x100, EBX |= 0x2000
 *   slots 4..11: EBX |= 0x01 .. 0x80
 *
 * and the cached words are EAX, EBX, ECX, EDX = dannyeax..dannyedx. */
void car_input_words(car_slots down, uint32_t words[4])
{
    static const car_dev none;
    static const uint32_t nomap[CAR_SLOTS];
    car_input_device_words(down, &none, nomap, words);
}

/* the whole of FUN_00408D37 (0x00408D37..0x0040920A), the axis arms
 * with the keyboard's.  Each slot's binding record is +0x00 key, +0x04 axis
 * offset, +0x08 scale, +0x0C button mask, as INITCONTROLDATA stores what
 * 0x00408CD5 returns (0x00408B30..). */
typedef struct joyrec { int axis; int32_t scale; uint32_t mask; } joyrec;

static joyrec joy_decode(uint32_t m, int present)        /* 0x00408CD5 */
{
    joyrec r = { 0, 0, 0 };
    uint32_t n;
    if (!present) return r;                     /* [esi+0x20] != 1: zeros */
    r.scale = 1;
    if (m & 0x400u)      { r.axis = 0x28; r.scale = -1; }
    else if (m & 0x800u)   r.axis = 0x28;
    else if (m & 0x200u)   r.axis = 0x30;
    else if (m & 0x100u)   r.axis = 0x2C;
    n = m & 0xFFu;
    if (n) r.mask = 1u << ((n - 1) & 31);       /* SHL EBX,CL: CL mod 32 */
    return r;
}

static int32_t joy_axis(const car_dev *d, int axis)      /* [EBP + ESI] */
{
    return axis == 0x28 ? d->x : axis == 0x2C ? d->ydown : d->yup;
}

/* Slots 0..3: the value arms.  `v` is the value a held key or button
 * gives; the unmasked axis keeps its magnitude, clamped at 0
 * (CMP r,0 / JGE).  Returns the register, sets `bit` in *ebx when the arm is
 * the digital one. */
static int32_t joy_value(int key, const joyrec *r, const car_dev *d,
                         int32_t v, uint32_t bit, uint32_t *ebx)
{
    int32_t a;
    if (key) { *ebx |= bit; return v; }
    if (r->axis) {
        a = (int32_t)((uint32_t)joy_axis(d, r->axis) * (uint32_t)r->scale);
        if (a < 0) a = 0;
        if (!r->mask) return v < 0 ? -a : a;    /* slot 0's NEG 0x00408DD7 */
        if (a > 0x40 && (d->buttons & r->mask)) { *ebx |= bit; return v; }
        return 0;
    }
    if (r->mask && (d->buttons & r->mask)) { *ebx |= bit; return v; }
    return 0;
}

/* Slots 4..11: flag only (0x00408F8F..0x004091EC, eight identical arms). */
static int joy_flag(int key, const joyrec *r, const car_dev *d)
{
    if (key) return 1;
    if (r->axis) {
        int32_t a = (int32_t)((uint32_t)joy_axis(d, r->axis)
                              * (uint32_t)r->scale);
        if (a <= 0x40) return 0;
        return !r->mask || (d->buttons & r->mask) != 0;
    }
    return r->mask && (d->buttons & r->mask);
}

void car_input_device_words(car_slots down, const car_dev *d,
                            const uint32_t map[12], uint32_t words[4])
{
    joyrec r[CAR_SLOTS];
    uint32_t ebx = 0, pov = 0;
    int32_t ecx, edx, eax;
    int s;

    for (s = 0; s < CAR_SLOTS; s++) r[s] = joy_decode(map[s], d->present);
    /* 0x004087B6: the POV, when there is a device and no shifter
     * ([esi+0x414] == 0 - the port has none); 0xFFFF in the low word is
     * centred.  0x004087E0: no device, or no shifter, is 0xE00. */
    if (d->present && (d->pov & 0xFFFFu) != 0xFFFFu)
        pov = (d->pov << 8) / 0x8CA0u;
    ebx = pov << 24 | 0xE00u;

    ecx = joy_value(down & (1u << 0) ? 1 : 0, &r[0], d, -0x100, 0x1000u, &ebx);
    edx = joy_value(down & (1u << 1) ? 1 : 0, &r[1], d, 0x100, 0x1000u, &ebx);
    ecx += edx;                                 /* 0x00408E53 */
    if ((ebx & 0x1000u) && ecx != 0) ecx = ecx < 0 ? -0x100 : 0x100;
    eax = joy_value(down & (1u << 2) ? 1 : 0, &r[2], d, 0x100, 0x4000u, &ebx);
    edx = joy_value(down & (1u << 3) ? 1 : 0, &r[3], d, 0x100, 0x2000u, &ebx);
    if (d->jstype == 1) {                       /* 0x00408F61: all digital */
        ebx |= 0x7000u;
        if (ecx) ecx = ecx < 0 ? -0x100 : 0x100;
        if (edx) edx = 0x100;
        if (eax) eax = 0x100;
    }
    for (s = 4; s < CAR_SLOTS; s++)
        if (joy_flag(down & (1u << s) ? 1 : 0, &r[s], d))
            ebx |= 1u << (s - 4);
    words[0] = (uint32_t)eax;
    words[1] = ebx;
    words[2] = (uint32_t)ecx;
    words[3] = (uint32_t)edx;
}

/* ---- the pad: a remaster, named ------------------------------- */

/* The scheme's mapping: the image's, plus the port's for two slots the image
 * leaves unbound - without them a pad could not pause or call the rescue.
 * PORT'S CHOICE, not the binary's: slot 10 pause = button 5, slot 11 reset
 * car = button 6 (pad_button: START and LB). */
static uint32_t joymap[CAR_SLOTS];
static int joymap_set;

static void joymap_default(void)
{
    memcpy(joymap, car_joy_image, sizeof joymap);
    joymap[10] = 5;
    joymap[11] = 6;
    joymap_set = 1;
}

const uint32_t *car_input_joymap(void)
{
    if (!joymap_set) joymap_default();
    return joymap;
}

void car_input_set_joymap(const uint32_t rec[11])
{
    int i;
    joymap_default();
    for (i = 0; i < 11; i++) joymap[car_joy_rec_slot[i]] = rec[i];
    /* the port's two additions stand while the record leaves them unbound */
    if (!joymap[10]) joymap[10] = 5;
    if (!joymap[11]) joymap[11] = 6;
}

/* The DirectInput button numbers the pad's buttons stand for - the PORT'S
 * LAYOUT (a driving game's: the face buttons for the gears, the handbrake
 * and the view), not the binary's: a 1999 joystick's buttons had no names.
 * Button n is bit n-1, so the image's mapping (1 gear up, 2 gear down,
 * 3 handbrake, 4 camera) lands on B, X, A, Y. */
static const struct { unsigned pad; int di; } pad_button[] = {
    { PLAT_PAD_B, 1 },          /* gear up      (image: button 1) */
    { PLAT_PAD_X, 2 },          /* gear down    (image: button 2) */
    { PLAT_PAD_A, 3 },          /* handbrake    (image: button 3) */
    { PLAT_PAD_Y, 4 },          /* camera view  (image: button 4) */
    { PLAT_PAD_START, 5 },      /* pause        (the port's)      */
    { PLAT_PAD_LB, 6 },         /* reset car    (the port's)      */
    { PLAT_PAD_RB, 7 },
    { PLAT_PAD_LSTICK, 8 },
    { PLAT_PAD_RSTICK, 9 },
    { PLAT_PAD_BACK, 10 },
    /* the d-pad's four, so program controller can bind them too (a handheld's
     * main control): the original maps buttons 1..32, and nothing reads the
     * POV */
    { PLAT_PAD_UP, 11 },
    { PLAT_PAD_DOWN, 12 },
    { PLAT_PAD_LEFT, 13 },
    { PLAT_PAD_RIGHT, 14 }
};

/* joylowerdz / joyupperdz.  The PORT'S DEFAULT: 10 and 5 - a modern pad's
 * stick drifts, and the image's default (-1: none) would steer it. */
static int dz_lower = 10, dz_upper = 5;

void car_input_set_deadzone(int lower, int upper)
{
    if (lower < 0) lower = 0;
    if (lower > 90) lower = 90;
    if (upper < 0) upper = 0;
    if (upper > 99 - lower) upper = 99 - lower;
    dz_lower = lower;
    dz_upper = upper;
}

void car_input_get_deadzone(int *lower, int *upper)
{
    *lower = dz_lower;
    *upper = dz_upper;
}

/* inp_configure_device_q's two DIPROPDWORDs (0x0040830C, section 12). */
void car_input_dz_props(int lower, int upper, int *deadzone, int *saturation)
{
    *deadzone = lower * 100;
    *saturation = 10000 * (100 - lower - upper) / (100 - lower);
}

/* What DirectInput does with them, for a magnitude on a 0..256 range: inside
 * the dead zone the centre, beyond the saturation the end, linear between.
 * The port does it because SDL does not (section 12). */
int car_input_dz_apply(int raw, int deadzone, int saturation)
{
    long p;
    if (raw < 0) raw = 0;
    if (raw > 32767) raw = 32767;
    p = (long)raw * 10000L / 32767L;
    if (p <= deadzone) return 0;
    if (p >= saturation) return 256;
    return (int)(256L * (p - deadzone) / (saturation - deadzone));
}

/* the steering stick's response curve, a remaster's
 * (rally.cfg pad_curve; the original is linear).  An exponent in percent,
 * 100..300: between whole powers it blends the two (150 = half t, half t^2),
 * in integers so the checks need no libm.  100 leaves the value alone. */
static int curve_pct = 100;

void car_input_set_curve(int pct)
{
    curve_pct = pct < 100 ? 100 : pct > 300 ? 300 : pct;
}

int car_input_curve(int v)
{
    long a = 256, b;
    int i, n = curve_pct / 100, fr = curve_pct % 100;
    if (curve_pct == 100 || v <= 0 || v >= 256) return v;
    for (i = 0; i < n; i++) a = a * v / 256;
    b = a * v / 256;
    return (int)((a * (100 - fr) + b * fr) / 100);
}

static int axis_mode = 2;               /* the image's player record: +0x34 = 2 */

int car_input_jstype(int mode)
{
    return mode == 1 ? 1 : 2;           /* 0x00408A5E: 1 digital; 0 -> 2 */
}

void car_input_set_axis_mode(int mode) { axis_mode = mode; }

int car_input_pad(car_dev *d)
{
    static const unsigned dirs[4] = {
        PLAT_PAD_UP, PLAT_PAD_RIGHT, PLAT_PAD_DOWN, PLAT_PAD_LEFT
    };
    plat_pad p;
    int dz, sat, i, n = 0, sum = 0;
    size_t k;

    memset(d, 0, sizeof *d);
    if (!plat_pad_read(&p)) return 0;
    car_input_dz_props(dz_lower, dz_upper, &dz, &sat);
    d->present = 1;
    d->jstype = car_input_jstype(axis_mode);   /* 2 analogue; 1 digital */
    d->x = p.lx < 0 ? -car_input_curve(car_input_dz_apply(-p.lx, dz, sat))
                    : car_input_curve(car_input_dz_apply(p.lx, dz, sat));
    /* The triggers as the two pedal axes: +0x30 the accelerator's (slot 2,
     * image mapping 0x200), +0x2C the brake's (slot 3, 0x100).  The image's
     * jstype <= 2 split ONE stick's Y into them (0x00408733: up +0x30, down
     * +0x2C); a jstype > 2 device takes two axes of its own (0x00408745) -
     * the shape the port uses, with the triggers as the axes. */
    d->yup = car_input_dz_apply(p.rt, dz, sat);
    d->ydown = car_input_dz_apply(p.lt, dz, sat);
    for (k = 0; k < sizeof pad_button / sizeof pad_button[0]; k++)
        if (p.buttons & pad_button[k].pad)
            d->buttons |= 1u << (pad_button[k].di - 1);
    /* the d-pad as a hat, in DirectInput's hundredths of a degree */
    for (i = 0; i < 4; i++)
        if (p.buttons & dirs[i]) { n++; sum += i * 9000; }
    if (n == 0 || n > 2) d->pov = 0xFFFFFFFFu;
    else if (n == 1) d->pov = (uint32_t)sum;
    else d->pov = (p.buttons & PLAT_PAD_UP) && (p.buttons & PLAT_PAD_LEFT)
                  ? 31500u : (uint32_t)(sum / 2);
    return 1;
}

/* The scheme's twelve key bytes, by record index. */
static unsigned char keys[CAR_SLOTS];
static int keys_set;

void car_input_set_keys(const unsigned char rec[12])
{
    memcpy(keys, rec, sizeof keys);
    keys_set = 1;
}

int car_input_slot_key(int slot)
{
    int i;
    if (!keys_set) car_input_set_keys(car_key_default);
    for (i = 0; i < CAR_SLOTS; i++)
        if (car_key_slot[i] == slot)
            return keys[i] < 0x80 ? car_key_sdl[keys[i]] : 0;
    return 0;
}

int car_input_keys_are_default(void)
{
    return !keys_set || memcmp(keys, car_key_default, sizeof keys) == 0;
}

/* Each slot's TEST BYTE [code + 0x4F1AA4],0x80: the folded code's key state,
 * here the SDL scancode it stands for. */
car_slots car_input_keyboard(void)
{
    car_slots down = 0;
    int i;
    if (!keys_set) car_input_set_keys(car_key_default);
    for (i = 0; i < CAR_SLOTS; i++) {
        int sc = keys[i] < 0x80 ? car_key_sdl[keys[i]] : 0;
        if (sc > 0 && plat_input_key(sc))
            down |= 1u << car_key_slot[i];
    }
    return down;
}

static car_slots parse_keys(const char *p, int *ok)
{
    car_slots k = 0;
    *ok = 1;
    for (; *p && *p != '#' && *p != '\n' && *p != '\r'; p++) {
        switch (*p) {
        case 'L': k |= 1u << 0; break;
        case 'R': k |= 1u << 1; break;
        case 'T': k |= 1u << 2; break;
        case 'B': k |= 1u << 3; break;
        case 'U': k |= 1u << 4; break;
        case 'D': k |= 1u << 5; break;
        case 'H': k |= 1u << 6; break;
        case 'V': k |= 1u << 9; break;
        case 'P': k |= 1u << 10; break;             
        case 'X': k |= 1u << 11; break;
        case '-': case ' ': case '\t': break;
        default: *ok = 0; return 0;
        }
    }
    return k;
}

int car_script_load(car_script *s, const char *path, long *bad_line)
{
    FILE *f = fopen(path, "r");
    char line[256];
    long cap = 0, ln = 0;

    memset(s, 0, sizeof *s);
    *bad_line = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *end;
        long step;
        int ok;
        ln++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        step = strtol(p, &end, 10);
        if (end == p || step < 0 || (s->n > 0 && step < s->step[s->n - 1])) {
            *bad_line = ln;
            fclose(f);
            return -1;
        }
        if (s->n == cap) {
            long nc = cap ? cap * 2 : 64;
            long *ns = (long *)realloc(s->step, (size_t)nc * sizeof *ns);
            car_slots *nk;
            if (!ns) { fclose(f); return -1; }
            s->step = ns;
            nk = (car_slots *)realloc(s->keys, (size_t)nc * sizeof *nk);
            if (!nk) { fclose(f); return -1; }
            s->keys = nk;
            cap = nc;
        }
        s->step[s->n] = step;
        s->keys[s->n] = parse_keys(end, &ok);
        if (!ok) {
            *bad_line = ln;
            fclose(f);
            return -1;
        }
        s->n++;
    }
    fclose(f);
    return 0;
}

car_slots car_script_at(const car_script *s, long step)
{
    long lo = 0, hi = s->n - 1, best = -1;
    while (lo <= hi) {                       /* the last line at or before */
        long mid = (lo + hi) / 2;
        if (s->step[mid] <= step) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best < 0 ? 0 : s->keys[best];
}

void car_script_free(car_script *s)
{
    free(s->step);
    free(s->keys);
    memset(s, 0, sizeof *s);
}
