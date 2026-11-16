/* pacenote.c - the co-driver's pacenotes: the sentences built at level load,
 * and the trigger in the step that speaks them.
 *
 * is the reading; tools/pacenotes.py is its
 * transcription, and the gate pacenotes-diff holds this file to it on every
 * strip, token for token.  Every function below was read again from retail's
 * bytes this session (tools/ralimg.py disasm).
 *
 *   init_pacenotes_q  0x0043FED0  collection: start codes, terminators, gaps
 *                     0x0044004D  emission: distance word, notes, "into"
 *                     0x0044011E  one note's codes; 0x63 a computed corner
 *   FUN_00443C7D                  the route's cumulative distance, cum[]
 *   FUN_0043FC56                  the trigger, 0x00439E16 in the step
 *   FUN_0043FD1D                  one sentence spoken
 *
 * THE PORT'S DECISIONS, each counted where it bites (sim_pacenotes):
 *  the pool is zeroed on every build, so a stale byte the original would
 *     read (A[n], a 0x62 hole, the last sentence's missing terminator, the
 *     previous level's pool) reads 0, as in a fresh process.  The run-on
 *     defect of is NOT reproduced;
 *  a code read past the route stops the walk (the original reads
 *     route[routeCount], whatever the collected list holds there - six reads
 *     corpus-wide, all on LEVEL01-06's demo strips, none ever spoken);
 *  the arrays are bounded where the original writes on.
 *  FUN_0043FD1D's on-screen arrows (FUN_0043FDB7 -> FUN_0043FE55, the
 *     car's +0xF58 queue) and FUN_0043FDD5's ageing of them are not ported:
 *     counted in icons_due, the HUD's. */

#include "physics.h"

#include <math.h>
#include <string.h>

/* [0x005C7A44]: the pols byte each of a marker's eight slots reads. */
static const uint8_t slot_off[8] = { 0x28, 0x29, 0x2A, 0x2B, 0x19, 0x1A, 0x1B, 0x20 };

/* [0x005C7A68]: the distance word for floor((gap >> 16) / 10), capped at 40;
 * 0 says nothing.  Tokens 32..40 are 030 050 070 100 150 200 250 300 400. */
static const uint8_t dist_word[41] = {
    0x00, 0x20, 0x20, 0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x22, 0x23, 0x23,
    0x23, 0x23, 0x23, 0x24, 0x24, 0x24, 0x24, 0x24, 0x25, 0x25, 0x25, 0x25,
    0x25, 0x26, 0x26, 0x26, 0x26, 0x26, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
    0x27, 0x27, 0x27, 0x27, 0x28
};

/* [0x005C79AC]: the arrow of each token below 0x20, -1 for none. */
static const int8_t icon_of[32] = {
    -1,  8,  9, 10, 11, 12, 13, 14, 15,  8,  9, 10, 11, 12, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  0,  1,  2,  3,  4, -1, -1
};

#define INTO      0x29          /* 0x0044026B: the chain word                */
#define CHAIN_GAP 0x1E0000      /* 0x0044009C: chain when the gap <= 30.0    */
#define NO_CHAIN  0x62          /* a note ended by this never chains         */
#define END       0x64          /* the ordinary terminator                   */
#define CORNER    0x63          /* 0x0044015C: computed from the route       */
#define MAX_TOKEN 0x53          /* FUN_0043FD1D: >= 0x54 is invalid          */

static int32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return (int32_t)v;
}

static int32_t wsub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

static int32_t wadd(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

/* FISTP, round to nearest even; out of range or NaN is the integer
 * indefinite.  race.c's rule. */
static int32_t fistp_ld(long double v)
{
    long double f, d, r;
    if (!(v > -2147483648.5L && v < 2147483647.5L)) return INT32_MIN;
    f = floorl(v);
    d = v - f;
    if (d > 0.5L)      r = f + 1.0L;
    else if (d < 0.5L) r = f;
    else               r = (fmodl(f, 2.0L) == 0.0L) ? f : f + 1.0L;
    return (int32_t)r;
}

/* ---- init_pacenotes_q ---------------------------------------------------- */

typedef struct build {
    sim_pacenotes   *pn;
    const sim_route *r;
    const uint8_t   *pols, *pnts;
    int32_t          npols, npnts;
    int32_t         *cum;                  /* FUN_00443C7D, r->n entries     */
    int32_t A[SIM_PACE_NOTES + 1], B[SIM_PACE_NOTES + 1], C[SIM_PACE_NOTES + 1];
    int32_t D[SIM_PACE_NOTES + 1];
    int32_t nb;                            /* B[0 .. nb-1] written           */
    int32_t w;                             /* the pool's write position      */
} build;

/* The note code at position `pos` (marker pos >> 3, slot pos & 7); -1 past
 * the route. */
static int code(const build *b, int32_t pos)
{
    int32_t m = pos >> 3, pol;
    if (pos < 0 || m >= b->r->n) return -1;
    pol = b->r->pol[m];
    if (pol < 0 || pol >= b->npols) return -1;
    return b->pols[(size_t)pol * 0x40u + slot_off[pos & 7]];
}

static int32_t cum_at(build *b, int32_t i)
{
    if (i >= 0 && i < b->r->n) return b->cum[i];
    b->pn->past_route++;
    return 0;
}

static int32_t angle_at(build *b, int32_t i)
{
    if (i >= -1 && i <= b->r->n) return b->r->angle[i];
    b->pn->past_route++;
    return 0;
}

/* FUN_00443C7D: cum[0] = 0, cum[i+1] = cum[i] + FISTP(sqrt(dx^2 + dz^2)).
 * The x87 holds dx^2 + dz^2 exactly (< 2^64); an integer square root
 * rounded to nearest is FSQRT + FISTP (pacenotes.py's). */
static void distances(build *b)
{
    int32_t i;
    b->cum[0] = 0;
    for (i = 0; i + 1 < b->r->n; i++) {
        const uint8_t *P = b->pnts + (size_t)rd32(b->pols + (size_t)b->r->pol[i] * 0x40u + 4) * 12u;
        const uint8_t *Q = b->pnts + (size_t)rd32(b->pols + (size_t)b->r->pol[i + 1] * 0x40u + 4) * 12u;
        int64_t dx = wsub(rd32(Q + 0), rd32(P + 0));
        int64_t dz = wsub(rd32(Q + 8), rd32(P + 8));
        uint64_t s = (uint64_t)(dx * dx) + (uint64_t)(dz * dz), r;
        r = (uint64_t)sqrtl((long double)s);
        while (r * r > s) r--;
        while ((r + 1) * (r + 1) <= s) r++;
        if (s - r * r > r) r++;
        b->cum[i + 1] = wadd(b->cum[i], (int32_t)(uint32_t)r);
    }
}

/* 0x0044015C..: the corner of note k - direction, severity, "long". */
static int corner(build *b, int32_t k)
{
    int32_t a = b->A[k], e, ln, tok, p, d;
    uint32_t q;
    if (k >= b->nb) {                      /* B[k] never written             */
        b->pn->unterminated++;
        return -1;
    }
    e = b->B[k];
    ln = wsub(cum_at(b, e >> 3), cum_at(b, a >> 3));
    tok = ln >= 0x780000 ? 8 : 0;          /* 120.0 units: "long"            */
    if (ln < 0x3C0000) ln = 0x3C0000;      /* 60.0 units, a floor            */
    p = a - 1;
    if (p < 0) p = 0;
    if (p > 0x1869F) p = 0x1869F;
    p >>= 3;
    d = wsub(angle_at(b, (e >> 3) + 1), angle_at(b, p + 1));
    if (d >= 0) tok += 16;                 /* the right-hand set             */
    /* |d| * 60 / len: MUL / DIV, unsigned 64/32 */
    q = (uint32_t)(((uint64_t)(d < 0 ? 0u - (uint32_t)d : (uint32_t)d) * 60u)
                   / (uint32_t)ln);
    tok += q > 0x3330 ? 5 : q > 0x2664 ? 4 : q > 0x1998 ? 3 : q > 0xCCC ? 2 : 1;
    return tok;
}

static void put(build *b, int v)
{
    if (b->w >= SIM_PACE_POOL) { b->pn->wild++; return; }
    b->pn->pool[b->w++] = (uint8_t)v;
}

/* FUN_0044011E: note k's codes into the pool. */
static void note(build *b, int32_t k)
{
    int32_t p = b->A[k];
    for (;; p++) {
        int c = code(b, p);
        if (c < 0) { b->pn->past_route++; return; }
        if (c == 0 || c == END) return;    /* 0x62 does NOT stop it          */
        if (c == CORNER) {
            c = corner(b, k);
            if (c < 0) return;
        }
        if (c == NO_CHAIN) {               /* the byte is skipped, not written */
            b->pn->holes++;
            if (b->w >= SIM_PACE_POOL) b->pn->wild++;
            else b->w++;
            continue;
        }
        if ((c == 0x2F || c == 0x3C || (c >= 0x34 && c <= 0x38))
            && b->w > 0 && b->pn->pool[b->w - 1] == INTO)
            b->w--;                        /* drop a preceding "into"        */
        put(b, c);
    }
}

int sim_pacenotes_build(sim_pacenotes *pn, const sim_route *r,
                        const uint8_t *pols, int32_t npols,
                        const uint8_t *pnts, int32_t npnts)
{
    static build b;                        /* 20 KB of arrays: not the stack */
    int32_t n, m, pos, s, e, na;
    int state_b = 0;

    memset(pn, 0, sizeof(*pn));
    if (r->n < 2) return 0;                /* routeCount < 2: count stays 0  */
    memset(&b, 0, sizeof(b));
    b.pn = pn; b.r = r;
    b.pols = pols; b.npols = npols; b.pnts = pnts; b.npnts = npnts;
    {
        static int32_t cum[SIM_PACE_NOTES * 8];
        if (r->n > (int32_t)(sizeof cum / sizeof cum[0])) { pn->wild++; return 0; }
        b.cum = cum;
    }
    distances(&b);

    /* 0x0043FEF1: the first marker whose tag (+0x26) is 2, plus two */
    for (m = 0; m < r->n; m++)
        if (pols[(size_t)r->pol[m] * 0x40u + 0x26] == 2) break;
    m += 2;
    b.A[0] = m * 8; b.B[0] = m * 8 + 1; b.nb = 1;
    n = 1; pos = m * 8;
    for (;;) {
        int c = code(&b, pos);
        if (c < 0) { pn->past_route++; break; }
        if (!state_b) {
            if (c != 0 && c != NO_CHAIN && c < END) {
                if (n > SIM_PACE_NOTES) { pn->wild++; break; }
                b.A[n] = pos;
                state_b = 1;
            }
            pos++;
            if (!state_b && (pos >> 3) == r->n) break;
        } else {
            if (c == NO_CHAIN || c == END) {
                b.C[n] = c; b.B[n] = pos;
                n++;
                b.nb = n;
                state_b = 0;
            }
            pos++;
            if (state_b && (pos >> 3) == r->n) break;
        }
    }
    /* 0x00440002: CMP n,1 / JB is never taken (n starts at 1), so a strip
     * with no notes still emits one sentence, from A[n] - stale when the
     * collection ended outside a note (0 here, as in a fresh process). */
    na = state_b ? n + 1 : n;              /* A[0 .. na-1] written           */
    if (n >= na) { pn->stale_a++; b.A[n] = 0; }
    pn->notes = n;
    for (e = 0; e < n; e++)                /* the gap after note e           */
        b.D[e] = wsub(cum_at(&b, b.A[e + 1] >> 3), cum_at(&b, b.B[e] >> 3));

    /* 0x0044004D: the sentences */
    s = 0; e = 0;
    for (;;) {
        int t;
        if (s >= SIM_PACE_NOTES) { pn->wild++; break; }
        pn->key[s] = b.B[e] >> 3;          /* spoken as the car passes note e's END */
        pn->offset[s] = b.w;
        {
            uint32_t g = ((uint32_t)b.D[e] >> 16) / 10u;     /* FUN_004400E2 */
            t = dist_word[g > 40u ? 40u : g];
        }
        if (t) put(&b, t);
        note(&b, e + 1);
        e++;
        while (e != n && b.D[e] <= CHAIN_GAP && b.C[e] != NO_CHAIN) {
            put(&b, INTO);                 /* 0x0044026B                     */
            note(&b, e + 1);
            e++;
        }
        if (e == n) {                      /* NO terminator: 0x00440093      */
            pn->count = s + 1;
            pn->last_open = 1;
            break;
        }
        put(&b, 0);                        /* FUN_004400D7                   */
        s++;
    }
    pn->used = b.w;
    return 0;
}

/* ---- the race: FUN_0043FC56 and FUN_0043FD1D ----------------------------- */

void sim_speech_push(sim_race *rc, int32_t kind, uint32_t slot, int32_t value)
{
    sim_speech *q;
    if (rc->nspeech >= SIM_SPEECH_RING) { rc->speech_lost++; return; }
    q = &rc->speech[rc->nspeech++];
    q->kind = kind;
    q->slot = (int32_t)slot;
    q->value = value;
}

void sim_pacenote_speak_q(sim_race *rc, uint32_t slot, uint32_t index)
{
    const sim_pacenotes *pn = rc->notes;
    int32_t i, o;
    if (!pn || pn->count == 0) return;     /* CMP [0x005C8B44],0 / JE ret    */
    for (i = 0; i < pn->count; i++)
        if (pn->key[i] == (int32_t)index) break;
    if (i == pn->count) return;
    rc->pace_spoken++;
    if (rc->pace_heard) rc->pace_heard[i]++;
    for (o = pn->offset[i]; o >= 0 && o < SIM_PACE_POOL; o++) {
        int t = pn->pool[o];
        if (t == 0) return;
        if (t > MAX_TOKEN) {               /* "!!! invalid pace data"        */
            rc->pace_invalid++;
            return;
        }
        sim_speech_push(rc, SIM_SPEECH_QUEUE, slot, t);   /* 0x0045AD2B      */
        if (t < 0x20 && icon_of[t] != -1)  /* FUN_0043FDB7 -> FUN_0043FE55   */
            rc->icons_due++;
    }
}

void sim_pacenote_trigger_q(car_state *car, sim_race *rc, uint32_t slot)
{
    int32_t look, target;
    uint32_t i;

    if (car->rfn != 0 || car->oot != 0 || car->oor != 0) return;
    if (rc->quiet_q != 0 || rc->mode.countdown_skip != 0) return;
    if (car->driver != 1) return;
    if (car->fs_count != 0) return;
    if (car->yaw_couple_gate != 0) return;            /* +0x254              */
    if ((uint32_t)car->alivet_r < 1000u) return;      /* CMP 0x3E8 / JAE     */
    /* FLD czspd, FMUL [0x005C79A8] (1.0), FDIV [0x0057AFD4] (8.0), FISTP */
    look = fistp_ld((long double)car->czspd * 1.0L / 8.0L);
    target = car->currentb;
    if (look >= 0) target = wadd(target, look);       /* CMP 0 / JS          */
    /* CMP n / JE zero / JS keep: at or past the route's end, 0 */
    if (target == rc->route->n
        || (int32_t)((uint32_t)target - (uint32_t)rc->route->n) >= 0)
        target = 0;
    if ((uint32_t)target > car->l_p_plyd) {           /* JBE                 */
        if ((uint32_t)target - car->l_p_plyd > 16u)   /* JBE                 */
            car->l_p_plyd = (uint32_t)target;
        i = car->l_p_plyd;
        do {
            i++;
            sim_pacenote_speak_q(rc, slot, i);
        } while (i < (uint32_t)target);               /* JB                  */
        car->l_p_plyd = (uint32_t)target;
    }
    /* 0x0043FD17: FUN_0043FDD5, the arrows' ageing - not ported */
}
