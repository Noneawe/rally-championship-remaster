/* tick.c - the outer tick: FUN_0045E525's slot loop, 0x0045E55D..0x0045E5D8.
 *
 *
 * THE ORDER OF THE BYTES
 *   [0x006CE084] = 0
 *   do {                                      0x0045E55D
 *       esi = car[slot & 7]                   0x006CE044 + slot*4
 *       copy the WHOLE 0xFE0-byte record into the working car, 0x006CCC64
 *                                             FUN_0045E636, 0x3F8 dwords
 *       if (working.driver != 0) {            [0x006CCEC0], car+0x25C
 *           ... six calls, the physics step FUN_00439B37 the sixth ...
 *       }
 *       [0x006CCC6C]++                        car+0x008, alivet
 *       [0x006CCC78]++                        car+0x014, alivet_r
 *       copy the working car back             FUN_0045E653
 *   } while (++slot != [0x005F0A50]);         0x0045E5C5..0x0045E5D6, a
 *                                             do-while: slot 0 always runs
 *   FUN_0045C51F (-> FUN_0045C65B)            0x0045E5D8, ONCE, after the loop
 *   ... then lifecycle calls the port does not model (0x0045E5DD..0x0045E600)
 *
 * THE TWO COUNTERS RUN WHETHER OR NOT THE CAR IS DRIVEN: the `je` at
 * 0x0045E58E lands on the INC at 0x0045E5B3.
 *
 * WHAT THE COPY-IN / COPY-OUT MEANS.  The step runs on a WORKING copy; the
 * array slot holds the car as it stood at the start of this car's turn.  So a
 * car's own step does not see its own slot updated, and FUN_0045D16A - which
 * reaches the OTHER cars through the slot pointers - sees every lower slot as
 * this tick left it and every higher slot as the LAST tick left it, and its
 * writes into another car's slot land in that slot's record directly.  The
 * roster handed to sim_car_step is therefore the array, with the current
 * slot's entry replaced by the working copy.
 *
 * THE INPUT STAGE.  The five calls before FUN_00439B37 (0x0043895C,
 * 0x00438208, 0x004383C3, 0x00438616, 0x004386E5) are sim_input_stage_q
 * (input.c), run when the slot has input_words.
 *
 * WHAT IS NOT HERE.  FUN_00438880 before the copy-in (the caller supplies
 * its four words), FUN_00461919 after the step (the multiplayer relay, gated
 * on [0x00524038]), and the six calls after the object tick.  The capture's +0x00C, which this loop does not
 * touch, is one of theirs. */

#include "physics.h"
#include "arcade.h"

#include <string.h>

uint32_t sim_object_tick_mask_q(const car_state *car, int has_device,
                                int32_t camera_view, int32_t camera_flag)
{
    /* 0x00437394: FLD [0x006CCD2C] (car+0xC8) / FCOMP 0.0 / FNSTSW / SHR AH,1
     * / JAE 0x00437461: C0 is set for less-than AND for unordered, so the
     * check proceeds for a negative value and for NaN, not for -0.0 or zero. */
    int below = !(car->matrix[5] >= 0.0f);

    if (has_device && below && camera_flag == 0
        && (camera_view == 0 || camera_view == 1))
        return SIM_GROUND_MASK_CAMERA;
    return SIM_GROUND_MASK_CONTACT;
}

int sim_tick_run(sim_tick *t)
{
    uint32_t slot, i;
    int rc, last_driven = -1;

    if (!t || !t->k || !t->dk || !t->sin_table
        || t->numcars < 1 || t->numcars > SIM_ROSTER_SLOTS)
        return -1;
    for (i = 0; i < t->numcars; i++)
        if (!t->car[i].rec || !t->car[i].world) return -1;
    /* The race's mode and the tick's are one global, [0x005D6750]. */
    if (t->race && t->race->mode.gmode != t->gmode) return -1;
    /* the array [0x006CE044 + slot*4], for the reset's search. */
    if (t->race)
        for (i = 0; i < SIM_ROSTER_SLOTS; i++)
            t->race->rec[i] = i < t->numcars ? t->car[i].rec : NULL;

    for (slot = 0; slot < t->numcars; slot++) {
        sim_tick_car *c = &t->car[slot];
        car_state work = *c->rec;                     /* FUN_0045E636 */

        if (work.driver != 0) {
            sim_roster ro;
            memset(&ro, 0, sizeof(ro));
            ro.gmode = t->gmode;
            ro.numcars = t->numcars;
            ro.slot = slot;
            ro.rev_q = &t->rev_q;
            ro.damage = &t->damage;                   /* one set, every car */
            c->world->shaungmode = t->gmode;          /* the same [0x005D6750] */
            /* the race rules' context, and [0x006CE084]. */
            c->world->race = t->race;
            c->world->race_slot = slot;
            for (i = 0; i < t->numcars; i++) {
                ro.car[i] = (i == slot) ? &work : t->car[i].rec;
                ro.active_frame[i] = t->car[i].world->active_frame;
            }
            if (!c->params || !c->ground || !c->geom) return -1;
            /* 0x0045E590..0x0045E5A4: the input stage, all five calls
             * (had the gear half). */
            if (t->settle) {
                /* FUN_0045E3FC: the words zeroed at 0x0045E40C, then
                 * 0x0043895C and 0x00438208 only. */
                sim_input_settle_q(&work, t->has_device[slot], t->gmode,
                                   &t->input, c->world);
            } else if (c->input_words) {
                if (!t->input.seeded) {
                    sim_input_ste_q(&work, t->k, &t->input);
                    t->input.seeded = 1;
                }
                uint32_t held = work.rescue_hold_q;
                sim_input_stage_q(&work, c->input_words, t->has_device[slot],
                                  t->gmode, &t->input, c->world);
                /* FUN_004386E5 calls FUN_0043D4B3 as its last act when the
                 * hold reaches exactly 100, and it is the last of the five
                 * calls, so the reset runs here, before the step. */
                if (t->race && work.rescue_hold_q == 100u && held != 100u) {
                    sim_place_ctx pc;
                    pc.p = c->params;
                    pc.k = t->k;
                    pc.sin_table = t->sin_table;
                    pc.ground = c->ground;
                    pc.dmg = &t->damage;
                    if (sim_car_reset_q(&work, c->world, t->race, &pc) < 0)
                        return -1;
                }
            }
            rc = sim_car_step(&work, c->params, t->k, t->dk, c->ground,
                              c->geom, t->sin_table, c->world, &ro);
            if (rc != 0) return rc;
            /* car+0x920, which the next step's rival driver reads -
             * the record keeps it, the port's world does not. */
            if (t->race && t->race->arcade)
                t->race->arcade->cls920[slot] = c->world->surf.cls_a[0];
            /* FUN_0043D033 rewrote [0x005C7940]/[0x005C793C] in the step. */
            t->input.ste_k = c->world->ste_k;
            t->input.ste_j = c->world->ste_j;
            t->input.seeded = 1;
        }
        if (work.driver != 0) last_driven = (int)slot;
        if (!t->settle) {                             /* FUN_0045E3FC has no INC */
            work.alivet++;                            /* 0x0045E5B3 */
            work.alivet_r++;                          /* 0x0045E5B9 */
        }
        *c->rec = work;                               /* FUN_0045E653 */
    }

    /* [0x005D31A0] as the LAST DRIVEN car's step left it: 0x80 from its
     * per-point queries, 0x40 after them if it is a player car whose camera
     * check queried.  With no driven car nothing wrote it this tick. */
    if (last_driven >= 0)
        t->object_mask = sim_object_tick_mask_q(
            t->car[last_driven].rec, t->has_device[last_driven],
            t->camera_view[last_driven], t->camera_flag[last_driven]);

    /* FUN_0045C51F's first call, once, after every car. */
    if (t->objects && t->object_state && t->car[0].ground
        && t->car[0].ground->geom && t->car[0].ground->state) {
        rc = sim_object_tick_q(t->objects, t->object_state,
                               t->car[0].ground->geom, t->sin_table,
                               t->car[0].rec->pos,
                               t->car[0].ground->state->poly[4],
                               t->object_mask);
        if (rc != 0) return rc;
    }

    /* 0x0045E5DD: FUN_00435191, the mode's per-tick rule - gmode 2's is the
     * streamer FUN_00435A90 (arcade.c).  The settle calls it too. */
    if (t->race && t->gmode == 2 && t->race->arcade) {
        rc = sim_arcade_stream_q(t->race->arcade, t);
        if (rc != 0) return rc;
    }

    /* 0x0045E5E2: FUN_0045A31E (weather.c), car 0's weather words
     * into the audio thread's levels; the thunder draws rnd_next().  Not in
     * the settle, which calls only C51F and 5191 after its loop. */
    if (t->race && !t->settle && t->car[0].world)
        sim_weather_tick_q(&t->race->weather, &t->car[0].world->wx,
                           t->object_state ? &t->object_state->rnd : NULL);

    /* 0x0045E5E7: FUN_0043F31E, after FUN_00435191 and FUN_0045A31E (above;
     * FUN_00435191 is not ported): FUN_0043F355 for car slot 0 into player 1's olaydraw, then
     * slot 1 into player 2's ([0x006CE084] = 0, 1).  The HUD's countdown
     * plate (race.c).  FUN_0045E3FC, the settle, has no such call. */
    if (t->race && !t->settle) {
        sim_countdown_q(t->car[0].rec, &t->race->olaydraw[0], t->race, 0);
        if (t->numcars > 1)
            sim_countdown_q(t->car[1].rec, &t->race->olaydraw[1], t->race, 1);
    }
    return 0;
}
