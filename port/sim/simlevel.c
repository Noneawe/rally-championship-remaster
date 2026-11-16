/* simlevel.c - see simlevel.h.
 *
 * Every block below was MOVED out of trace_check.c's main(), run() and
 * chain_pass(), in the order it ran there.  The comments that explain WHY a
 * value is what it is travelled with the code. */

#include "simlevel.h"
#include "camera.h"
#include "ro3.h"
#include "stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    b = (unsigned char *)malloc((size_t)(n > 0 ? n : 1));
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

void sim_level_init(sim_level *L)
{
    memset(L, 0, sizeof(*L));
}

int sim_level_load_image(sim_level *L, const char *image_path,
                         char *err, size_t errlen)
{
    if (sim_consts_load(image_path, &L->k, err, errlen) != 0
        || dt_consts_load(image_path, &L->dk, err, errlen) != 0
        || car_params_load(image_path, &L->tab, err, errlen) != 0
        || sim_surface_table_load(image_path, &L->st, err, errlen) != 0)
        return -1;
    sim_sin_build(L->sin_tab);
    return 0;
}

int sim_level_load_stage(sim_level *L, const char *cdat_path,
                         const char *localdat_path, const char *textinfo_path,
                         char *err, size_t errlen)
{
    int b;

    L->raw = slurp(cdat_path, &L->raw_len);
    L->ld  = slurp(localdat_path, &L->ld_len);
    L->ti  = slurp(textinfo_path, &L->ti_len);
    if (!L->raw || !L->ld || !L->ti || L->ld_len < 0x100 + 1024) {
        snprintf(err, errlen, "cannot read the level files");
        return -1;
    }
    if (cdat_open_buffer(L->raw, L->raw_len, &L->pl, err, errlen) != 0
        || !L->pl.fits) {
        snprintf(err, errlen, "%s is not a usable level", cdat_path);
        return -2;
    }
    /* The game's load fills each polygon's two triangle normals
     * (store_norms_q); the file holds zeros there.  A writable copy, so
     * the collision responses read real normals. */
    L->pols = (uint8_t *)malloc((size_t)L->pl.npols * 0x40u);
    L->pols_pristine = (uint8_t *)malloc((size_t)L->pl.npols * 0x40u);
    if (!L->pols || !L->pols_pristine) {
        snprintf(err, errlen, "out of memory");
        return -4;
    }
    memcpy(L->pols, L->pl.buf + L->pl.pols_off, (size_t)L->pl.npols * 0x40u);
    /* FUN_0044D0EF's LOCALDAT arm (0x0044D157), the car's ground light. */
    if (L->ld_len >= 0x18A4 + 12) {
        int32_t t[4][3];
        static const uint32_t off[4] = { 0x600, 0x60C, 0x1898, 0x18A4 };
        int i, k;
        for (i = 0; i < 4; i++)
            for (k = 0; k < 3; k++) {
                const unsigned char *q = L->ld + off[i] + 4u * (unsigned)k;
                t[i][k] = (int32_t)((uint32_t)q[0] | (uint32_t)q[1] << 8
                                  | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24);
            }
        sim_ground_light_build(&L->light, t[0], t[1], t[2], t[3]);
        L->have_light = 1;
    }
    L->norms_stale = sim_level_store_norms_q(L->pols, L->pl.npols,
                                             L->pl.buf + L->pl.pnts_off,
                                             L->pl.npnts);
    if (L->norms_stale < 0
        || sim_surface_level_build(&L->lv, L->ld + 0x100, L->ti, L->ti_len,
                                   L->pols, L->pl.npols) != 0) {
        snprintf(err, errlen, "%s is not a usable level", cdat_path);
        return -2;
    }
    memcpy(L->pols_pristine, L->pols, (size_t)L->pl.npols * 0x40u);
    L->branches = sim_ground_host_rcpss() ? 2 : 1;
    for (b = 0; b < L->branches; b++) {
        if (sim_ground_geom_build(&L->geom[b], L->pl.buf + L->pl.zube_off,
                                  L->pl.nzube, L->pols, L->pl.npols,
                                  L->pl.buf + L->pl.pnts_off, L->pl.npnts,
                                  err, errlen) != 0)
            return -3;
        L->geom[b].branch = b ? SIM_GROUND_SSE : SIM_GROUND_X87;
        L->geom[b].rcp = sim_ground_host_rcpss();
    }
    /* Point 4 is (0,0,0) by FUN_00425AAF's own construction, whatever
     * the model's 31 points are - so zeros stand in for them, and only
     * point 4's answer is ever read back.  The bump map moves the
     * HEIGHT only, which nothing here reads.  sim_level_load_car_points
     * replaces them with the car's own. */
    {
        static const int32_t zeros[SIM_GROUND_POINTS][3];
        sim_collision_points_build(&L->pts, zeros, SIM_GROUND_POINTS - 2);
    }
    L->in.pts = &L->pts;
    L->in.bump_map = stage_fog_noise();
    sim_weather_level_from_localdat(&L->weather, L->ld, L->ld_len,
                                    stage_fog_noise());
    L->in.wheel_drop = L->k.wheel_drop;
    L->in.susp_half = L->k.susp_half;
    L->in.bump_scale = L->k.bump_scale;
    L->in.bump_div = L->k.bump_div;
    L->have_level = 1;
    /* the route the start is taken from, for course direction 0.
     * A level whose route will not build only cannot START a car; the
     * replay does not need it, so this is not a load failure. */
    sim_level_build_route(L, 0);
    return 0;
}

int sim_level_build_route(sim_level *L, int32_t variant)
{
    if (L->have_route) sim_route_free(&L->route);
    L->have_route = 0;
    if (sim_route_build(&L->route, L->pols_pristine, L->pl.npols,
                        L->pl.buf + L->pl.pnts_off, L->pl.npnts, variant) != 0)
        return -1;
    L->have_route = 1;
    return 0;
}

int sim_level_load_objects(sim_level *L, const char *files_dir)
{
    int rc;
    /* The level's objects: the type tables from LOCALDAT and the .ro3
     * files, the instance records in the writable pols copy.  The tick
     * state's LFSR seed is the game's clock-seeded [0x004DB13C] - unknown,
     * so 1. */
    sim_objects_init(&L->objs);
    rc = simobj_load_level(&L->objs, &L->k, L->ld, L->ld_len, files_dir,
                           &L->ost);
    if (rc != 0) return rc;
    sim_object_state_init(&L->ostate, L->pols, L->pl.npols, 1u);
    L->have_objects = 1;
    return 0;
}

int sim_level_load_car_points(sim_level *L, const char *files_b_dir,
                              int32_t carnum, char *path_out, size_t path_len)
{
    int rc = sim_level_read_car_points(L, files_b_dir, carnum, &L->pts,
                                       &L->collision_model, path_out,
                                       path_len);
    if (rc == 0) L->model_points = 1;
    return rc;
}

int sim_level_read_car_points(sim_level *L, const char *files_b_dir,
                              int32_t carnum, sim_collision_points *out,
                              int32_t *model_out, char *path_out,
                              size_t path_len)
{
    /* the car's collision points from its own collision
     * model (FUN_00425777), replacing
     * the zero placeholders. */
    int32_t model = sim_car_collision_model_q(&L->k, carnum);
    size_t len = 0;
    unsigned char *buf;
    ro3_model m;
    char err[256];
    static int32_t pts[SIM_GROUND_POINTS][3];
    int32_t i, np;

    if (model_out) *model_out = model;
    snprintf(path_out, path_len, "%s/OBJ%04d.RO3", files_b_dir, (int)model);
    buf = slurp(path_out, &len);
    if (!buf || ro3_parse(buf, len, &m, err, sizeof(err)) != 0) {
        free(buf);
        return -1;
    }
    np = (int32_t)m.npoints;
    if (np > SIM_GROUND_POINTS - 2) np = -1;
    /* The raw 16.16 triples, as FUN_004252A5 appends them. */
    for (i = 0; i < np; i++) {
        const unsigned char *q = buf + 0x2C + 12 * (size_t)i;
        pts[i][0] = (int32_t)((uint32_t)q[0] | (uint32_t)q[1] << 8
                    | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24);
        pts[i][1] = (int32_t)((uint32_t)q[4] | (uint32_t)q[5] << 8
                    | (uint32_t)q[6] << 16 | (uint32_t)q[7] << 24);
        pts[i][2] = (int32_t)((uint32_t)q[8] | (uint32_t)q[9] << 8
                    | (uint32_t)q[10] << 16 | (uint32_t)q[11] << 24);
    }
    ro3_free(&m);
    free(buf);
    if (sim_car_collision_points(out, &L->k, model, pts, np) != 0)
        return -2;
    return 0;
}

void sim_level_reset_objects(sim_level *L)
{
    memcpy(L->pols, L->pols_pristine, (size_t)L->pl.npols * 0x40u);
    if (L->have_objects)
        sim_object_state_init(&L->ostate, L->pols, L->pl.npols, 1u);
}

void sim_level_free(sim_level *L)
{
    int b;
    for (b = 0; b < L->branches; b++) sim_ground_geom_free(&L->geom[b]);
    if (L->have_objects) sim_objects_free(&L->objs);
    if (L->have_route) sim_route_free(&L->route);
    free(L->pols);
    free(L->pols_pristine);
    free(L->raw);
    free(L->ld);
    free(L->ti);
    memset(L, 0, sizeof(*L));
}

/* ---- the replayed tick ------------------------------------------------- */

void sim_replay_init(sim_replay *r, sim_level *L, const car_params *p)
{
    int c;
    memset(r, 0, sizeof(*r));
    r->L = L;
    r->p = p;
    /* Exactly the set-up of the step's ground and geometry that run() and
     * chain_pass() each did in trace_check.c. */
    sim_ground_points_init(&r->points);
    r->ground.table = &L->st;
    r->ground.level = &L->lv;
    r->ground.geom = &L->geom[0];
    r->ground.in = L->in;
    r->ground.state = &r->points;
    r->ground.objects = L->have_objects ? &L->objs : NULL;
    r->ground.object_state = L->have_objects ? &L->ostate : NULL;
    r->ground.light = L->have_light ? &L->light : NULL;
    r->geom.wheel_z[0] =  1.2f;
    r->geom.wheel_z[1] =  1.2f;
    r->geom.wheel_z[2] = -1.2f;
    r->geom.wheel_z[3] = -1.2f;
    if (L->model_points)                      /* car+0xCF0..0xCFC */
        for (c = 0; c < 4; c++) r->geom.wheel_z[c] = L->pts.z[c];
    r->rev_q = 0.0f;
    /* the race rules over the level's route as it stands now (a
     * caller that changes the course direction rebuilds the route first).
     * One-player rally mode, gmode 0: the mode sim_replay_step ticks in. */
    if (L->have_route) {
        sim_start_mode m;
        memset(&m, 0, sizeof(m));
        m.numpl = 1;
        sim_race_init(&r->race, &L->route, &L->geom[0], &m);
        r->have_race = 1;
    }
}

int sim_replay_add_rival(sim_replay *r, int slot, const car_params *p,
                         const sim_collision_points *pts)
{
    sim_replay_rival *v;
    int c;
    if (slot < 1 || slot > SIM_REPLAY_RIVALS || !p || !pts) return -1;
    v = &r->rival[slot - 1];
    v->p = p;
    v->pts = *pts;
    sim_ground_points_init(&v->points);
    v->ground = r->ground;                 /* the level's, as car 0's       */
    v->ground.in.pts = &v->pts;            /* its own collision points      */
    v->ground.state = &v->points;          /* its own hints                 */
    for (c = 0; c < 4; c++) v->geom.wheel_z[c] = v->pts.z[c];
    r->nrivals = SIM_REPLAY_RIVALS;
    return 0;
}

int sim_replay_step(sim_replay *r, car_state *car, const car_state *inputs,
                    const car_state *damage_from, sim_world *w)
{
    static const uint32_t no_device[4] = { 0, 0, 0, 0 };
    car_state last_driven;
    sim_tick tk;
    int s;
    /* The input device's four words, as FUN_0043895C copied them into the
     * record the step goes TO.  The port derives the edge
     * (+0x474) and the gear flags from them against the starting record's
     * dannyebx - the game's own edge is not handed over. */
    uint32_t words[4];
    int rc;

    /* ~~The tick's INPUT STAGE runs before the step and is not ported ...
     * So the step sees the END record's values.~~: it is ported
     * (input.c), and sim_tick_run runs it from the four words below, so
     * +0x638/+0x63C/+0x640/+0x644/+0x648/+0x64C/+0x650 are the stage's own,
     * computed from the START record's accumulators.  Nothing is copied
     * from the end record but the device's words. */
    memset(w, 0, sizeof(*w));
    /* step_world.shaungmode was set to 1 here, while tk.gmode below -
     * the same [0x005D6750] - was 0: the capture sides
     * with 0.  The game's fwhlspd is exactly 0 on all 799 frames with
     * alivet_r < 0x320 and non-zero on all 35,847 after, so step 13's
     * hold runs, which it does only when [shaungmode] != 1.
     * sim_tick_run hands tk.gmode to the step. */
    words[0] = inputs->dannyeax; words[1] = inputs->dannyebx;
    words[2] = inputs->dannyecx; words[3] = inputs->dannyedx;
    memset(&tk, 0, sizeof(tk));
    tk.k = &r->L->k; tk.dk = &r->L->dk; tk.sin_table = r->L->sin_tab;
    /* The race's mode: 0, or 2 for an arcade stage started by
     * sim_replay_start; a capture replays in 0 (no race: 0 too). */
    tk.gmode = r->have_race ? r->race.mode.gmode : 0;
    tk.numcars = 1;
    tk.car[0].rec = car;
    tk.car[0].params = r->p;
    tk.car[0].geom = &r->geom;
    tk.car[0].ground = &r->ground;
    tk.car[0].world = w;
    tk.car[0].input_words = words;
    tk.objects = r->ground.objects;
    tk.object_state = r->ground.object_state;
    /* The capture's car is the human player's: an input device,
     * and (unknown, above the prefix) the default camera. */
    tk.has_device[0] = 1;
    /* with a camera, the mask's camera inputs are its own words as
     * the previous step left them, which is when FUN_0043739C reads them;
     * the step runs the camera itself (sim_world.camera). */
    if (r->camera) {
        w->camera = r->camera;
        w->weather = &r->L->weather;       /* FUN_0043A16A        */
        tk.camera_view[0] = r->camera->cam.view;
        tk.camera_flag[0] = r->camera->cam.blending;
    }
    /* [0x005C7478] is not in the record: carry the port's own
     * from the previous step. */
    tk.rev_q = r->rev_q;
    /* The input stage's process state: carried like rev_q.  On a
     * run's first step it is unseeded, and the tick seeds ste k / ste j
     * from the car - exact for one car whose ste s 1 does not change. */
    tk.input = r->input;
    /* The damage model's process globals are not in the record
     * either, but each is a copy or an average of record fields
     * at the previous step's end: seed them from the
     * record the step starts from. */
    /* the arcade's four slots.  The globals are the LAST STEPPED
     * car's - with rivals on the road, the highest driven slot's record as
     * this tick finds it, which is how the previous tick ended. */
    if (r->nrivals) {
        tk.numcars = 1u + (uint32_t)r->nrivals;
        last_driven = *damage_from;
        for (s = 1; s <= r->nrivals; s++) {
            sim_replay_rival *v = &r->rival[s - 1];
            memset(&v->w, 0, sizeof(v->w));
            memcpy(v->w.binary_angle, v->binary_angle, sizeof(v->binary_angle));
            memcpy(v->w.active_frame, v->active_frame, sizeof(v->active_frame));
            tk.car[s].rec = &v->car;
            tk.car[s].params = v->p;
            tk.car[s].geom = &v->geom;
            tk.car[s].ground = &v->ground;
            tk.car[s].world = &v->w;
            tk.car[s].input_words = no_device;
            tk.has_device[s] = 0;
            if (v->car.driver != 0) last_driven = v->car;
        }
        damage_from = &last_driven;
    }
    sim_damage_globals_from_car_q(damage_from, &r->L->k, &tk.damage);
    tk.settle = r->settle;
    tk.race = r->have_race ? &r->race : NULL;  
    /* +0x9C0..+0x9C8 are above the record: the port's own, carried. */
    memcpy(w->binary_angle, r->binary_angle, sizeof(r->binary_angle));
    rc = sim_tick_run(&tk);
    memcpy(r->binary_angle, w->binary_angle, sizeof(r->binary_angle));
    for (s = 1; s <= r->nrivals; s++) {
        sim_replay_rival *v = &r->rival[s - 1];
        memcpy(v->binary_angle, v->w.binary_angle, sizeof(v->binary_angle));
        memcpy(v->active_frame, v->w.active_frame, sizeof(v->active_frame));
    }
    r->rev_q = tk.rev_q;
    r->input = tk.input;
    return rc;
}

/* ---- the race start --------------------------------------------- */

int sim_replay_start(sim_replay *r, car_state *car, const sim_start_mode *m,
                     uint32_t slot, uint32_t seed, int settle_steps)
{
    sim_damage_globals dmg;
    sim_world w;
    car_state zero;
    int n, rc;

    if (!r->L->have_route) return -1;
    /* The race keeps the start's mode, and sim_replay_step ticks in it:
     * 0, 2 since (the arcade's clock), 1 since (the
     * championship: its false start; its live position counted, not yet
     * computed).  3 is refused - its ten rivals' splits are unported. */
    if (m->gmode != 0 && m->gmode != 1 && m->gmode != 2) return -1;
    if (r->have_race) r->race.mode = *m;
    /* messagesystem_init: message 202, the race start, for 300
     * steps.  Above the record, so no replayed word moves. */
    if (r->have_race) sim_message_system_init_q(&r->race);
    /* initcarpositions_q -> FUN_00444639 for this car.  FUN_0043D033's
     * damage input is the globals as the car's own record makes them - the
     * same rule sim_replay_step applies. */
    memset(&w, 0, sizeof(w));
    w.camera = r->camera;                          /* FUN_0043E00B's half    */
    sim_damage_globals_from_car_q(car, &r->L->k, &dmg);
    if (sim_place_car_q(car, slot, m, &r->L->route, &r->L->geom[0], r->p,
                        &r->L->k, r->L->sin_tab, &r->ground, &dmg, &w,
                        NULL) != 0)
        return -1;
    /* The placement's angles are the car's +0x9C0..+0x9C8. */
    memcpy(r->binary_angle, w.binary_angle, sizeof(r->binary_angle));
    /* resettimers_q, FUN_00442FEA */
    sim_reset_timers_q(car, m, seed);
    /* both loop over all eight slots, and each car's seed is the
     * next rnd_next draw (0x00443008) - so with `seed` taken as car 0's
     * draw, the rivals' follow from it.  initcarpositions_q places them
     * too (0x004445EF, numcars of them), but the streamer's spawn places
     * them again before they are first stepped: the port does not. */
    {
        uint32_t st = seed;
        int s;
        for (s = 1; s <= r->nrivals; s++)
            sim_reset_timers_q(&r->rival[s - 1].car, m, sim_rnd_next_q(&st));
    }
    /* singlecarinit: FUN_0045E3FC, settle_steps times (SIM_SETTLE_STEPS in
     * the game).  ONE car: the settle steps every car in the roster, and a
     * second car 2.5 m away is not in this one's (the port's limit). */
    memset(&zero, 0, sizeof(zero));
    r->settle = 1;
    for (n = 0; n < settle_steps; n++) {
        car_state from = *car;
        rc = sim_replay_step(r, car, &zero, &from, &w);
        if (rc != 0) {
            r->settle = 0;
            return rc;
        }
    }
    r->settle = 0;
    /* raceinit's last call (0x004445C4): resetalldamagemessages, 0x0043E2B9 -
     * every slot's 34 warning states set from its values, no message (a carried wear starts quiet, run 34) */
    sim_damage_warn_rebuild_q(car);
    {
        int s;
        for (s = 1; s <= r->nrivals; s++) sim_damage_warn_rebuild_q(&r->rival[s - 1].car);
    }
    return 0;
}

/* ---- the capture -------------------------------------------------------- */

#define CAPTURE_CARS    4
#define CAPTURE_RECORD  (4u + CAPTURE_CARS * CAR_REPLAY_PREFIX)

int sim_capture_load(sim_capture *c, const char *path)
{
    FILE *f;
    long sz, n;
    unsigned char rec[CAPTURE_RECORD];

    memset(c, 0, sizeof(*c));
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0) {
        fclose(f);
        return -1;
    }
    if ((unsigned long)sz % CAPTURE_RECORD != 0) {
        fclose(f);
        return -2;
    }
    c->frames = sz / (long)CAPTURE_RECORD;
    rewind(f);
    c->car0 = (unsigned char *)malloc((size_t)c->frames * CAR_REPLAY_PREFIX);
    if (!c->car0) {
        fclose(f);
        c->frames = 0;
        return -3;
    }
    for (n = 0; n < c->frames; n++) {
        uint32_t idx;
        if (fread(rec, 1, CAPTURE_RECORD, f) != CAPTURE_RECORD) {
            fclose(f);
            sim_capture_free(c);
            return -1;
        }
        idx = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8)
            | ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);
        if (idx != (uint32_t)n) c->seq_bad++;
        memcpy(c->car0 + (size_t)n * CAR_REPLAY_PREFIX, rec + 4,
               CAR_REPLAY_PREFIX);
    }
    fclose(f);
    return 0;
}

const car_state *sim_capture_car(const sim_capture *c, long frame)
{
    if (!c->car0 || frame < 0 || frame >= c->frames) return NULL;
    /* The capture is a byte image of the game's own working block and car.h
     * is that block with compile-time offset assertions on it.  malloc's
     * alignment and CAR_REPLAY_PREFIX (a multiple of 4) keep every record
     * aligned for the struct. */
    return (const car_state *)(const void *)
           (c->car0 + (size_t)frame * CAR_REPLAY_PREFIX);
}

void sim_capture_free(sim_capture *c)
{
    free(c->car0);
    memset(c, 0, sizeof(*c));
}
