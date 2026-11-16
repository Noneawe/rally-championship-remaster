/* collision_int.h - collision.c's two shared pieces, for objects.c.
 *
 * Not part of the port's API: FUN_0045BCE8 (objects.c) calls the same shared
 * impulse solver and the same rollback the ground responses use, and the
 * original has one copy of each.  So does the port. */

#ifndef RALLY_SIM_COLLISION_INT_H
#define RALLY_SIM_COLLISION_INT_H

#include "physics.h"

/* FUN_0045CCD3, the shared contact impulse solver, into `record`. */
int sim_contact_impulse_q(const car_state *car, const sim_consts *k,
                          const sim_world *w, const float n[3],
                          const float r[3], float mu, sim_force_rec *record,
                          float *normal_component, float *tangent_speed_out);

/* FUN_0043C100: restore C0B0's snapshot and increment [0x005C746C]. */
void sim_collision_rollback_q(car_state *car, sim_ground_points *points,
                              sim_world *w,
                              const sim_collision_snapshot *snapshot);

/* FUN_0045B233: the lowest body point, points 4 and up, into
 * w->body_contact_clearance/_index; both left stale for four or fewer
 * points.  The spiral search (tyre.c) calls it on its own. */
int sim_body_contact_candidate_q(car_state *car, const sim_ground *ground,
                                 sim_world *w);

/* overlap.c: 0 if sim_car_overlap_q would accept `roster` (NULL included),
 * -1 if it would refuse it.  sim_car_step asks before touching the car. */
int sim_roster_check_q(const sim_roster *roster);

/* overlap.c, FUN_0045D306's two pieces a capture can test on their own:
 * the car-frame velocity through a +0x9CC (0x0045D5C6), and the push, 1.5 x
 * the centre separation (0x0045D4E1 and FUN_00420010).  twocar_check. */
void sim_car_overlap_cspd_q(car_state *car, const float frame[16]);
void sim_car_overlap_push_q(const car_state *cur, const car_state *oth,
                            const sim_consts *k, float push[3]);

#endif /* RALLY_SIM_COLLISION_INT_H */
