/* plat_glthread.c - the GL thread: the game thread records plat_gfx_* calls
 * (with copies of the vertices and pixels they point at) into a ring, and one
 * thread that owns the GL context runs them and does the swap.  The Mali blocks
 * in the swap for the frame's GPU work and in every draw for driver work; this
 * takes both off the thread that runs the simulation and the scene walk.
 *
 * Off unless RALLY_GL_THREAD=1; then every wrapper calls plat_gl.c directly. */
#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

typedef struct {                  /* plat_gl.c's snapshot, same layout */
    uint32_t v[PLAT_RS__COUNT];
    int      valid[PLAT_RS__COUNT];
    plat_tex tex;
} plat_gfx_saved;

/* the implementations, plat_gl.c under plat_glthread.h's names */
void     plat_glimpl_set_state(plat_rs state, uint32_t value);
void     plat_glimpl_reset_state(void);
void     plat_glimpl_draw(plat_prim p, const plat_vertex *v, unsigned nv);
void     plat_glimpl_draw_env(plat_prim p, const plat_vertex_env *v, unsigned nv,
                              plat_tex env);
void     plat_glimpl_draw_indexed(plat_prim p, const plat_vertex *v, unsigned nv,
                                  const uint16_t *idx, unsigned ni);
int      plat_glimpl_read_pixel(int x, int y, unsigned char rgba[4]);
int      plat_glimpl_cull_mode(int mode);
int      plat_glimpl_inset(int x, int y, int w, int h);
int      plat_glimpl_pillar(int x, int w);
void     plat_glimpl_state_save(plat_gfx_saved *s);
void     plat_glimpl_state_restore(const plat_gfx_saved *s);
int      plat_glimpl_read_frame(unsigned char *rgba, int w, int h);
int      plat_glimpl_clear_depth(void);
plat_tex plat_glimpl_frame_tex_create(unsigned w, unsigned h);
int      plat_glimpl_frame_copy(plat_tex t, int w, int h);
plat_tex plat_glimpl_tex_create(unsigned w, unsigned h,
                                const uint32_t *const *levels, unsigned nlevels);
void     plat_glimpl_tex_destroy(plat_tex t);
void     plat_glimpl_tex_update(plat_tex t, const uint32_t *level0);
int      plat_glimpl_tex_bgra(void);
plat_tex plat_glimpl_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px);
void     plat_glimpl_tex_update_bgra(plat_tex t, const uint32_t *px);
void     plat_glimpl_tex_smooth(plat_tex t, unsigned nlevels);
void     plat_glimpl_tex_bind(plat_tex t);
void     plat_glimpl_frame_begin(void);
void     plat_glimpl_scene_begin(void);
void     plat_glimpl_scene_end(void);
void     plat_gl_present_begin(int dw, int dh);
void     plat_gl_present_end(int dw, int dh);

#define RING_SZ   (16u << 20)
#define MAX_TEX   4096              /* plat_gl.c's PLAT_GL_MAX_TEX */
#define N_SLOTS   256
#define BIG_COPY  (1u << 20)        /* a bigger texture update waits instead */

static int                g_on;
static unsigned char     *g_ring;
static uint32_t           g_head, g_tail;   /* bytes ever published / consumed (wraps; RING_SZ divides 2^32) */
static uint32_t           g_wr;             /* the producer's own head        */
static SDL_Thread        *g_thr;
static SDL_Window        *g_win;
static SDL_GLContext      g_ctx;
static SDL_sem           *g_wake, *g_ack;   /* consumer asleep; producer waits */
static int                g_asleep;
static uint32_t           g_pres_enq, g_pres_done;
static plat_gfx_saved     g_slot[N_SLOTS];
static unsigned           g_slot_next;
static unsigned short     g_tw[MAX_TEX], g_th[MAX_TEX];  /* sizes, for updates */
static int                g_bgra = -1;

enum { OP_WRAP = 1, OP_SET_STATE, OP_RESET_STATE, OP_DRAW, OP_DRAW_ENV,
       OP_DRAW_INDEXED, OP_CULL, OP_INSET, OP_PILLAR, OP_SAVE, OP_RESTORE, OP_CLEAR_DEPTH,
       OP_FRAME_COPY, OP_TEX_DESTROY, OP_TEX_UPDATE, OP_TEX_UPDATE_BGRA,
       OP_TEX_SMOOTH, OP_TEX_BIND, OP_FRAME_BEGIN, OP_SCENE_BEGIN,
       OP_SCENE_END, OP_PRESENT, OP_CALL, OP_QUIT };

typedef struct { uint32_t op, size; } hdr;     /* size: bytes, with the header */

/* ---- producer ---------------------------------------------------------- */

static void *q_alloc(uint32_t op, size_t payload)
{
    uint32_t total = (uint32_t)((sizeof(hdr) + payload + 7u) & ~(size_t)7u);
    uint32_t off   = g_wr % RING_SZ, need = total;
    hdr     *h;

    if (off + total > RING_SZ) {               /* pad to the end, start over */
        need += RING_SZ - off;
    }
    while (g_wr + need - __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) > RING_SZ) {
        SDL_SemWaitTimeout(g_ack, 1);          /* the ring is full: wait     */
    }
    if (off + total > RING_SZ) {
        h = (hdr *)(g_ring + off);
        h->op = OP_WRAP;
        h->size = (uint32_t)(RING_SZ - off);
        g_wr += h->size;
        __atomic_store_n(&g_head, g_wr, __ATOMIC_RELEASE);
        off = 0;
    }
    h = (hdr *)(g_ring + off);
    h->op = op;
    h->size = total;
    return h + 1;
}

static void q_commit(void *payload)
{
    hdr *h = (hdr *)payload - 1;

    g_wr += h->size;
    __atomic_store_n(&g_head, g_wr, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_asleep, __ATOMIC_SEQ_CST) &&
        __atomic_exchange_n(&g_asleep, 0, __ATOMIC_SEQ_CST)) {
        SDL_SemPost(g_wake);
    }
}

/* run fn(arg) on the GL thread and wait for it: everything before it in the
 * ring has run, and what fn writes is visible here */
typedef struct { void (*fn)(void *); void *arg; int *done; } call_p;

static void call_sync(void (*fn)(void *), void *arg)
{
    int done = 0;
    call_p *c = q_alloc(OP_CALL, sizeof *c);

    c->fn = fn; c->arg = arg; c->done = &done;
    q_commit(c);
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        SDL_SemWaitTimeout(g_ack, 1);
    }
}

/* ---- the GL thread ----------------------------------------------------- */

static void consume(const hdr *h)
{
    const unsigned char *p = (const unsigned char *)(h + 1);

    switch (h->op) {
    case OP_SET_STATE: {
        const uint32_t *a = (const uint32_t *)p;
        plat_glimpl_set_state((plat_rs)a[0], a[1]);
        break; }
    case OP_RESET_STATE: plat_glimpl_reset_state(); break;
    case OP_DRAW: {
        const uint32_t *a = (const uint32_t *)p;     /* prim, nv, has */
        plat_glimpl_draw((plat_prim)a[0], a[2] ? (const plat_vertex *)(p + 16)
                                               : NULL, a[1]);
        break; }
    case OP_DRAW_ENV: {
        const uint32_t *a = (const uint32_t *)p;     /* prim, nv, env, has */
        plat_glimpl_draw_env((plat_prim)a[0], a[3] ? (const plat_vertex_env *)
                             (p + 16) : NULL, a[1], (plat_tex)a[2]);
        break; }
    case OP_DRAW_INDEXED: {
        const uint32_t *a = (const uint32_t *)p;     /* prim, nv, ni, has v|i */
        const plat_vertex *v = (a[3] & 1u) ? (const plat_vertex *)(p + 16) : NULL;
        const uint16_t *idx = (a[3] & 2u) ? (const uint16_t *)
            (p + 16 + (((size_t)a[1] * sizeof(plat_vertex) + 7u) & ~7u)) : NULL;
        plat_glimpl_draw_indexed((plat_prim)a[0], v, a[1], idx, a[2]);
        break; }
    case OP_CULL:  plat_glimpl_cull_mode(*(const int *)p); break;
    case OP_INSET: {
        const int *a = (const int *)p;
        plat_glimpl_inset(a[0], a[1], a[2], a[3]);
        break; }
    case OP_PILLAR: {
        const int *a = (const int *)p;
        plat_glimpl_pillar(a[0], a[1]);
        break; }
    case OP_SAVE:    plat_glimpl_state_save(&g_slot[*(const uint32_t *)p]); break;
    case OP_RESTORE: plat_glimpl_state_restore(&g_slot[*(const uint32_t *)p]); break;
    case OP_CLEAR_DEPTH: plat_glimpl_clear_depth(); break;
    case OP_FRAME_COPY: {
        const int *a = (const int *)p;
        plat_glimpl_frame_copy((plat_tex)a[0], a[1], a[2]);
        break; }
    case OP_TEX_DESTROY: plat_glimpl_tex_destroy(*(const plat_tex *)p); break;
    case OP_TEX_UPDATE:
        plat_glimpl_tex_update(*(const plat_tex *)p, (const uint32_t *)(p + 8));
        break;
    case OP_TEX_UPDATE_BGRA:
        plat_glimpl_tex_update_bgra(*(const plat_tex *)p, (const uint32_t *)(p + 8));
        break;
    case OP_TEX_SMOOTH: {
        const uint32_t *a = (const uint32_t *)p;
        plat_glimpl_tex_smooth((plat_tex)a[0], a[1]);
        break; }
    case OP_TEX_BIND: plat_glimpl_tex_bind(*(const plat_tex *)p); break;
    case OP_FRAME_BEGIN: plat_glimpl_frame_begin(); break;
    case OP_SCENE_BEGIN: plat_glimpl_scene_begin(); break;
    case OP_SCENE_END:   plat_glimpl_scene_end(); break;
    case OP_PRESENT: {
        const int *a = (const int *)p;
        plat_gl_present_begin(a[0], a[1]);
        SDL_GL_SwapWindow(g_win);
        plat_gl_present_end(a[0], a[1]);
        __atomic_fetch_add(&g_pres_done, 1, __ATOMIC_RELEASE);
        SDL_SemPost(g_ack);
        break; }
    case OP_CALL: {
        const call_p *c = (const call_p *)p;
        c->fn(c->arg);
        __atomic_store_n(c->done, 1, __ATOMIC_RELEASE);
        SDL_SemPost(g_ack);
        break; }
    default: break;
    }
}

static int gl_thread(void *unused)
{
    uint32_t t = 0;

    (void)unused;
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_SemPost(g_ack);                        /* started */
    for (;;) {
        if (t == __atomic_load_n(&g_head, __ATOMIC_ACQUIRE)) {
            /* asleep, unless it arrives; a lost wake-up costs one timeout */
            __atomic_store_n(&g_asleep, 1, __ATOMIC_SEQ_CST);
            if (t == __atomic_load_n(&g_head, __ATOMIC_SEQ_CST)) {
                SDL_SemWaitTimeout(g_wake, 1);
            }
            __atomic_store_n(&g_asleep, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        {
            const hdr *h = (const hdr *)(g_ring + t % RING_SZ);
            if (h->op == OP_QUIT) {
                break;
            }
            if (h->op != OP_WRAP) {
                consume(h);
            }
            t += h->size;
            __atomic_store_n(&g_tail, t, __ATOMIC_RELEASE);
            if (h->op == OP_WRAP) {
                SDL_SemPost(g_ack);
            }
        }
    }
    SDL_GL_MakeCurrent(g_win, NULL);
    return 0;
}

int glt_active(void) { return g_on; }

/* the context is current on the calling thread; it moves to the GL thread */
int glt_start(SDL_Window *w, SDL_GLContext ctx)
{
    const char *e = getenv("RALLY_GL_THREAD");

    if (g_on || !e || atoi(e) == 0) {
        return 0;
    }
    g_ring = (unsigned char *)malloc(RING_SZ);
    g_wake = SDL_CreateSemaphore(0);
    g_ack  = SDL_CreateSemaphore(0);
    if (!g_ring || !g_wake || !g_ack) {
        return 0;
    }
    g_win = w;
    g_ctx = ctx;
    SDL_GL_MakeCurrent(w, NULL);
    g_thr = SDL_CreateThread(gl_thread, "gl", NULL);
    if (!g_thr) {
        SDL_GL_MakeCurrent(w, ctx);
        return 0;
    }
    SDL_SemWait(g_ack);
    g_on = 1;
    return 1;
}

/* drain, stop, and leave the context current nowhere */
void glt_stop(void)
{
    if (!g_on) {
        return;
    }
    q_commit(q_alloc(OP_QUIT, 0));
    SDL_WaitThread(g_thr, NULL);
    g_thr = NULL;
    g_on = 0;
}

void glt_call(void (*fn)(void *), void *arg) { call_sync(fn, arg); }

/* the frame is recorded: swap it on the GL thread, and let the game thread
 * run on at most one frame ahead of what the GL thread has finished */
void glt_present(int dw, int dh)
{
    int *a = q_alloc(OP_PRESENT, 2 * sizeof(int));
    uint32_t enq;

    a[0] = dw; a[1] = dh;
    q_commit(a);
    enq = ++g_pres_enq;
    while ((int32_t)(enq - 1 - __atomic_load_n(&g_pres_done, __ATOMIC_ACQUIRE)) > 0) {
        SDL_SemWaitTimeout(g_ack, 1);
    }
}

/* ---- the public names -------------------------------------------------- */

void plat_gfx_set_state(plat_rs state, uint32_t value)
{
    if (g_on) {
        uint32_t *a = q_alloc(OP_SET_STATE, 8);
        a[0] = (uint32_t)state; a[1] = value;
        q_commit(a);
    } else {
        plat_glimpl_set_state(state, value);
    }
}

#define SIMPLE(name, op, impl) \
void name(void) { if (g_on) q_commit(q_alloc(op, 0)); else impl(); }
SIMPLE(plat_gfx_reset_state,    OP_RESET_STATE, plat_glimpl_reset_state)
SIMPLE(plat_video_frame_begin,  OP_FRAME_BEGIN, plat_glimpl_frame_begin)
SIMPLE(plat_video_scene_begin,  OP_SCENE_BEGIN, plat_glimpl_scene_begin)
SIMPLE(plat_video_scene_end,    OP_SCENE_END,   plat_glimpl_scene_end)

void plat_gfx_draw(plat_prim p, const plat_vertex *v, unsigned nv)
{
    if (g_on) {
        size_t   b = v ? (size_t)nv * sizeof *v : 0;
        uint32_t *a = q_alloc(OP_DRAW, 16 + b);
        a[0] = (uint32_t)p; a[1] = v ? nv : 0; a[2] = v != NULL;
        if (b) memcpy(a + 4, v, b);
        q_commit(a);
    } else {
        plat_glimpl_draw(p, v, nv);
    }
}

void plat_gfx_draw_env(plat_prim p, const plat_vertex_env *v, unsigned nv,
                       plat_tex env)
{
    if (g_on) {
        size_t   b = v ? (size_t)nv * sizeof *v : 0;
        uint32_t *a = q_alloc(OP_DRAW_ENV, 16 + b);
        a[0] = (uint32_t)p; a[1] = v ? nv : 0; a[2] = (uint32_t)env;
        a[3] = v != NULL;
        if (b) memcpy(a + 4, v, b);
        q_commit(a);
    } else {
        plat_glimpl_draw_env(p, v, nv, env);
    }
}

void plat_gfx_draw_indexed(plat_prim p, const plat_vertex *v, unsigned nv,
                           const uint16_t *idx, unsigned ni)
{
    if (g_on) {
        size_t   bv = v ? (((size_t)nv * sizeof *v) + 7u) & ~(size_t)7u : 0;
        size_t   bi = idx ? (size_t)ni * sizeof *idx : 0;
        uint32_t *a = q_alloc(OP_DRAW_INDEXED, 16 + bv + bi);
        a[0] = (uint32_t)p; a[1] = v ? nv : 0; a[2] = idx ? ni : 0;
        a[3] = (v ? 1u : 0u) | (idx ? 2u : 0u);
        if (bv) memcpy(a + 4, v, (size_t)nv * sizeof *v);
        if (bi) memcpy((unsigned char *)(a + 4) + bv, idx, bi);
        q_commit(a);
    } else {
        plat_glimpl_draw_indexed(p, v, nv, idx, ni);
    }
}

/* the return values of these four are ignored by every caller */
int plat_gfx_cull_mode(int mode)
{
    if (!g_on) return plat_glimpl_cull_mode(mode);
    { int *a = q_alloc(OP_CULL, 4); *a = mode; q_commit(a); }
    return 0;
}

int plat_gfx_inset(int x, int y, int w, int h)
{
    if (!g_on) return plat_glimpl_inset(x, y, w, h);
    { int *a = q_alloc(OP_INSET, 16); a[0] = x; a[1] = y; a[2] = w; a[3] = h; q_commit(a); }
    return 1;
}

int plat_gfx_pillar(int x, int w)
{
    if (!g_on) return plat_glimpl_pillar(x, w);
    { int *a = q_alloc(OP_PILLAR, 8); a[0] = x; a[1] = w; q_commit(a); }
    return 1;
}

int plat_gfx_clear_depth(void)
{
    if (!g_on) return plat_glimpl_clear_depth();
    q_commit(q_alloc(OP_CLEAR_DEPTH, 0));
    return 1;
}

int plat_gfx_frame_copy(plat_tex t, int w, int h)
{
    if (!g_on) return plat_glimpl_frame_copy(t, w, h);
    { int *a = q_alloc(OP_FRAME_COPY, 12); a[0] = (int)t; a[1] = w; a[2] = h; q_commit(a); }
    return 1;
}

/* plat_gfx_saved is opaque to its callers: the first word names a GL-thread slot */
void plat_gfx_state_save(plat_gfx_saved *s)
{
    if (!g_on) { plat_glimpl_state_save(s); return; }
    {
        uint32_t *a = q_alloc(OP_SAVE, 4);
        s->v[0] = *a = g_slot_next++ % N_SLOTS;
        q_commit(a);
    }
}

void plat_gfx_state_restore(const plat_gfx_saved *s)
{
    if (!g_on) { plat_glimpl_state_restore(s); return; }
    { uint32_t *a = q_alloc(OP_RESTORE, 4); *a = s->v[0]; q_commit(a); }
}

void plat_gfx_tex_destroy(plat_tex t)
{
    if (!g_on) { plat_glimpl_tex_destroy(t); return; }
    { plat_tex *a = q_alloc(OP_TEX_DESTROY, sizeof *a); *a = t; q_commit(a); }
}

void plat_gfx_tex_bind(plat_tex t)
{
    if (!g_on) { plat_glimpl_tex_bind(t); return; }
    { plat_tex *a = q_alloc(OP_TEX_BIND, sizeof *a); *a = t; q_commit(a); }
}

void plat_gfx_tex_smooth(plat_tex t, unsigned nlevels)
{
    if (!g_on) { plat_glimpl_tex_smooth(t, nlevels); return; }
    { uint32_t *a = q_alloc(OP_TEX_SMOOTH, 8); a[0] = (uint32_t)t; a[1] = nlevels; q_commit(a); }
}

typedef struct { plat_tex t; const uint32_t *px;
                 void (*f)(plat_tex, const uint32_t *); } big_a;
static void s_big_update(void *p) { big_a *c = p; c->f(c->t, c->px); }

static void tex_update_any(int op, plat_tex t, const uint32_t *px,
                           void (*impl)(plat_tex, const uint32_t *))
{
    size_t b;

    if (!g_on) { impl(t, px); return; }
    if (!px || t == 0 || t >= MAX_TEX || !g_tw[t]) return;
    b = (size_t)g_tw[t] * g_th[t] * 4u;
    if (b > BIG_COPY) {                        /* wait instead of copying */
        big_a c;
        c.t = t; c.px = px; c.f = impl;
        call_sync(s_big_update, &c);
        return;
    }
    {
        unsigned char *a = q_alloc((uint32_t)op, 8 + b);
        *(plat_tex *)a = t;
        memcpy(a + 8, px, b);
        q_commit(a);
    }
}

void plat_gfx_tex_update(plat_tex t, const uint32_t *level0)
{
    tex_update_any(OP_TEX_UPDATE, t, level0, plat_glimpl_tex_update);
}

void plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px)
{
    tex_update_any(OP_TEX_UPDATE_BGRA, t, px, plat_glimpl_tex_update_bgra);
}

/* the ones that return a handle or a picture: the caller waits */
typedef struct {
    unsigned w, h, n; const uint32_t *const *lv; const uint32_t *px;
    unsigned char *rgba; int iw, ih, x, y; int r; plat_tex t;
} sync_a;

static void s_tex_create(void *p)
{ sync_a *a = p; a->t = plat_glimpl_tex_create(a->w, a->h, a->lv, a->n); }
static void s_tex_create_bgra(void *p)
{ sync_a *a = p; a->t = plat_glimpl_tex_create_bgra(a->w, a->h, a->px); }
static void s_frame_tex(void *p)
{ sync_a *a = p; a->t = plat_glimpl_frame_tex_create(a->w, a->h); }
static void s_read_frame(void *p)
{ sync_a *a = p; a->r = plat_glimpl_read_frame(a->rgba, a->iw, a->ih); }
static void s_read_pixel(void *p)
{ sync_a *a = p; a->r = plat_glimpl_read_pixel(a->x, a->y, a->rgba); }
static void s_bgra(void *p)
{ sync_a *a = p; a->r = plat_glimpl_tex_bgra(); }

static plat_tex remember(plat_tex t, unsigned w, unsigned h)
{
    if (t && t < MAX_TEX) { g_tw[t] = (unsigned short)w; g_th[t] = (unsigned short)h; }
    return t;
}

plat_tex plat_gfx_tex_create(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels)
{
    sync_a a;
    if (!g_on) return plat_glimpl_tex_create(w, h, levels, nlevels);
    memset(&a, 0, sizeof a); a.w = w; a.h = h; a.lv = levels; a.n = nlevels;
    call_sync(s_tex_create, &a);
    return remember(a.t, w, h);
}

plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px)
{
    sync_a a;
    if (!g_on) return plat_glimpl_tex_create_bgra(w, h, px);
    memset(&a, 0, sizeof a); a.w = w; a.h = h; a.px = px;
    call_sync(s_tex_create_bgra, &a);
    return remember(a.t, w, h);
}

plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h)
{
    sync_a a;
    if (!g_on) return plat_glimpl_frame_tex_create(w, h);
    memset(&a, 0, sizeof a); a.w = w; a.h = h;
    call_sync(s_frame_tex, &a);
    return a.t;
}

int plat_gfx_read_frame(unsigned char *rgba, int w, int h)
{
    sync_a a;
    if (!g_on) return plat_glimpl_read_frame(rgba, w, h);
    memset(&a, 0, sizeof a); a.rgba = rgba; a.iw = w; a.ih = h;
    call_sync(s_read_frame, &a);
    return a.r;
}

int plat_gfx_read_pixel(int x, int y, unsigned char rgba[4])
{
    sync_a a;
    if (!g_on) return plat_glimpl_read_pixel(x, y, rgba);
    memset(&a, 0, sizeof a); a.x = x; a.y = y; a.rgba = rgba;
    call_sync(s_read_pixel, &a);
    return a.r;
}

int plat_gfx_tex_bgra(void)
{
    sync_a a;
    if (!g_on) return plat_glimpl_tex_bgra();
    if (g_bgra < 0) {
        memset(&a, 0, sizeof a);
        call_sync(s_bgra, &a);
        g_bgra = a.r;
    }
    return g_bgra;
}
