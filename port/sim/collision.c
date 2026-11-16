/* collision.c - rigid-body contact against the queried road plane. */

#include "physics.h"
#include "collision_int.h"

#include <math.h>
#include <string.h>

static float collision_store(double value)
{
    return (float)value;
}

static float collision_dot(const float a[3], const float b[3])
{
    return collision_store((double)a[0] * b[0]
                         + (double)a[1] * b[1]
                         + (double)a[2] * b[2]);
}

static float collision_length(const float v[3])
{
    return collision_store(sqrt((double)v[0] * v[0]
                              + (double)v[1] * v[1]
                              + (double)v[2] * v[2]));
}

static float collision_f32(const uint8_t *p)
{
    float value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void collision_scale(const float in[3], float scale, float out[3])
{
    int axis;
    for (axis = 0; axis < 3; axis++)
        out[axis] = collision_store((double)in[axis] * scale);
}

int sim_body_contact_candidate_q(car_state *car, const sim_ground *ground,
                                 sim_world *w)
{
    sim_ground_points *points;
    int32_t minimum, index, i;

    if (!car || !ground || !ground->state || !w
        || car->numcollpts < 0 || car->numcollpts > SIM_GROUND_POINTS)
        return -1;

    points = ground->state;
    if (car->numcollpts <= 4) return 0;

    minimum = 0x00640000;
    index = -1;
    for (i = 4; i < car->numcollpts; i++) {
        if (points->clear[i] < minimum) {
            minimum = points->clear[i];
            index = i;
        }
    }
    w->body_contact_clearance = minimum;
    w->body_contact_index = index;
    return 0;
}

static int collision_missed_side_cf(int32_t from_x, int32_t from_z,
                                    int32_t to_x, int32_t to_z,
                                    int32_t point_x, int32_t point_z)
{
    int32_t move_x = (int32_t)((uint32_t)to_x - (uint32_t)from_x);
    int32_t move_z = (int32_t)((uint32_t)to_z - (uint32_t)from_z);
    int32_t rel_x = (int32_t)((uint32_t)point_x - (uint32_t)from_x);
    int32_t rel_z = (int32_t)((uint32_t)point_z - (uint32_t)from_z);
    uint64_t product_a = (uint64_t)((int64_t)move_z * rel_x);
    uint64_t product_b = (uint64_t)((int64_t)move_x * rel_z);
    uint64_t difference = product_b - product_a;

    /* B796 subtracts the signed IMUL pairs as EDX:EAX and branches on bit 63.
     * It sets CF for a nonnegative product_b - product_a result. */
    return (difference >> 63) == 0;
}

int sim_missed_point_edge_q(const sim_ground_geom *geom,
                            const sim_ground_points *points,
                            int32_t *edge_start, int32_t *edge_end)
{
    int32_t vertex[4], vertex_x[4], vertex_z[4];
    int32_t missed, polygon, from_x, from_z, to_x, to_z;
    const uint8_t *poly;
    int i;

    if (!geom || !points || !edge_start || !edge_end
        || geom->npols < 0 || !geom->pols || geom->npnts < 0 || !geom->pnts)
        return -1;
    missed = points->last_miss;
    if (missed == -1) return 0;
    if (missed < 0 || missed >= SIM_GROUND_POINTS) return -1;

    polygon = points->prev_poly[missed];
    if (polygon < 0 || polygon >= geom->npols) return -1;
    poly = geom->pols + (size_t)polygon * 0x40u;
    memcpy(&vertex[0], poly + 0x04, sizeof(vertex[0]));
    memcpy(&vertex[1], poly + 0x08, sizeof(vertex[1]));
    memcpy(&vertex[2], poly + 0x0c, sizeof(vertex[2]));
    memcpy(&vertex[3], poly + 0x10, sizeof(vertex[3]));
    for (i = 0; i < 4; i++) {
        const uint8_t *point;
        if (vertex[i] < 0 || vertex[i] >= geom->npnts) return -1;
        point = geom->pnts + (size_t)vertex[i] * 0x0cu;
        memcpy(&vertex_x[i], point, sizeof(vertex_x[i]));
        memcpy(&vertex_z[i], point + 8, sizeof(vertex_z[i]));
    }

    from_x = points->prev_ixyz[0][missed];
    from_z = points->prev_ixyz[2][missed];
    to_x = points->ix[missed];
    to_z = points->iz[missed];

    /* B796 tests the quad perimeter in the order 0 -> 1 -> 3 -> 2 -> 0, and
     * each block takes edge (a, b) when a is on the non-negative side of the
     * movement line and b on the negative one (0x0045B87E..0x0045BB4B, four
     * blocks, each a pair of sign tests):
     *   0x0045B8D0  T(v0) >= 0 and T(v1) < 0   -> 0x0045BB63  edge (0, 1)
     *   0x0045B986  T(v1) >= 0 and T(v3) < 0   -> 0x0045BB5B  edge (1, 3)
     *   0x0045BA3C  T(v3) >= 0 and T(v2) < 0   -> 0x0045BB53  edge (3, 2)
     *   otherwise                                0x0045BB4B  edge (2, 0)
     * The second block's first test, T(v1) >= 0, was missing until
     * (the code read `else if (!side3)`): with v1 AND v3 on the
     * negative side the port took edge (1, 3) where the game takes (2, 0),
     * whose normal is the opposite way round - 92 of run 20's 134 captured
     * missed-point responses, every one of them a ten-pass exhaustion. */
    {
        int side0 = collision_missed_side_cf(from_x, from_z, to_x, to_z,
                                             vertex_x[0], vertex_z[0]);
        int side1 = collision_missed_side_cf(from_x, from_z, to_x, to_z,
                                             vertex_x[1], vertex_z[1]);
        int side3 = collision_missed_side_cf(from_x, from_z, to_x, to_z,
                                             vertex_x[3], vertex_z[3]);
        int side2 = collision_missed_side_cf(from_x, from_z, to_x, to_z,
                                             vertex_x[2], vertex_z[2]);

        if (side0 && !side1) {
            *edge_start = vertex[0];
            *edge_end = vertex[1];
        } else if (side1 && !side3) {
            *edge_start = vertex[1];
            *edge_end = vertex[3];
        } else if (side3 && !side2) {
            *edge_start = vertex[3];
            *edge_end = vertex[2];
        } else {
            *edge_start = vertex[2];
            *edge_end = vertex[0];
        }
    }
    return 1;
}

/* 0x0045AA51, the body hit every FUN_0045CCD3 is followed by (but
 * C90A's, which scrapes, and the object response's): impactb at
 * round([0x00685E2C] x 50) - 1, [0x00685E2C] being the impulse's normal
 * component - for a car a local player drives. */
static void body_hit_sound(const car_state *car, sim_world *w, float normal)
{
    sim_sound_view sv;
    sim_sound_feed *sf = sim_sound_feed_local(w, &sv);
    if (sf) sim_sound_body_hit_q(car, sf, &sv, normal);
}

static int collision_contact_impulse_q(const car_state *car,
                                       const sim_consts *k,
                                       const sim_world *w,
                                       const float n[3], const float r[3],
                                       float mu, sim_force_rec *record,
                                       float *normal_component,
                                       float *tangent_speed_out)
{
    float angular[3], velocity[3], vp[3], wr[3];
    float t[3], t_local[3], t_world[3], tr[3];
    float projection, normal_speed, denominator, normal_impulse;
    float tangent_speed, tangent[3], normal_j[3], impulse[3];
    float impulse_local[3], point_local[3], friction_scale;
    float normal_velocity[3];
    int i;

    memcpy(angular, car->rotspd, sizeof(angular));
    memcpy(velocity, car->newspd, sizeof(velocity));
    sim_vec3_cross(angular, r, wr);
    for (i = 0; i < 3; i++)
        vp[i] = collision_store((double)wr[i] + velocity[i]);

    normal_speed = -collision_dot(vp, n);
    {
        float floor_speed = collision_store(1.0 - fabs((double)n[1])
                                             + k->contact_normal_floor);
        if (normal_speed < floor_speed) normal_speed = floor_speed;
    }

    sim_vec3_cross(r, n, t);
    sim_vec3_by_matrix(t, w->active_frame, t_local);
    for (i = 0; i < 3; i++)
        t_local[i] = collision_store((double)t_local[i] * k->inv_inertia[i]);
    sim_vec3_by_matrix(t_local, car->matrix, t_world);
    sim_vec3_cross(t_world, r, tr);
    denominator = collision_store((double)collision_dot(n, tr) + 1.0);
    normal_impulse = collision_store((double)normal_speed
                                     * k->contact_restitution / denominator);
    collision_scale(n, normal_impulse, normal_j);

    projection = collision_dot(vp, n);
    collision_scale(n, projection, normal_velocity);
    if (normal_component) *normal_component = collision_length(normal_velocity);
    for (i = 0; i < 3; i++) {
        float projected = collision_store((double)projection * n[i]);
        tangent[i] = collision_store((double)vp[i] - projected);
        tangent[i] = -tangent[i];
    }
    tangent_speed = collision_length(tangent);
    if (tangent_speed == 0.0f) {
        tangent[0] = tangent[1] = tangent[2] = 0.0f;
    } else if (tangent_speed > mu) {
        friction_scale = collision_store((double)mu / tangent_speed);
        collision_scale(tangent, friction_scale, tangent);
    }

    if (w->collision_passes != 0) {
        if (k->passes <= 0) return -1;
        normal_impulse = collision_store((double)k->contact_push
                                         * w->collision_passes / k->passes);
        collision_scale(n, normal_impulse, normal_j);
    }
    for (i = 0; i < 3; i++)
        impulse[i] = collision_store((double)normal_j[i] + tangent[i]);

    sim_vec3_by_matrix(impulse, w->active_frame, impulse_local);
    sim_vec3_by_matrix(r, w->active_frame, point_local);
    for (i = 0; i < 3; i++) {
        record->f[i] = impulse_local[i] / k->dt;
        record->p[i] = point_local[i];
    }
    if (tangent_speed_out) *tangent_speed_out = tangent_speed;
    return 0;
}

/* FUN_00420C64's plane primitive, FUN_00420E36: the cross product of two
 * integer edge vectors, each component ONE x87 expression
 * FILD,FILD,FMULP,FILD,FILD,FMULP,FSUBP,FSTP-float.  Two int32 products are
 * exact in the 64-bit significand and so is their difference (under 2^63), so
 * the only rounding is the final store: int64 -> float, nearest-even.  The
 * three corners are pnts indices (12-byte records); u = B - A, v = C - A with
 * the subtraction wrapping in 32 bits, as the SUBs do. */
static void store_norms_plane(const uint8_t *pnts, uint32_t a, uint32_t b,
                              uint32_t c, float out[3])
{
    int32_t pa[3], pb[3], pc[3], u[3], v[3];
    int i;

    memcpy(pa, pnts + (size_t)a * 12u, sizeof(pa));
    memcpy(pb, pnts + (size_t)b * 12u, sizeof(pb));
    memcpy(pc, pnts + (size_t)c * 12u, sizeof(pc));
    for (i = 0; i < 3; i++) {
        u[i] = (int32_t)((uint32_t)pb[i] - (uint32_t)pa[i]);
        v[i] = (int32_t)((uint32_t)pc[i] - (uint32_t)pa[i]);
    }
    out[0] = (float)((int64_t)v[2] * u[1] - (int64_t)v[1] * u[2]);
    out[1] = (float)((int64_t)v[0] * u[2] - (int64_t)v[2] * u[0]);
    out[2] = (float)((int64_t)v[1] * u[0] - (int64_t)v[0] * u[1]);
}

/* store_norms_q, 0x00420BF4: "SHAUNINITEVERYTHING : store norms".  Fills every
 * drawable polygon's two cached triangle normals, which the shipped file
 * carries as ZERO - so anything that reads
 * them from the file reads (0, 0, 0), and FUN_0045CB48's and FUN_0045C90A's
 * impulses come out exactly zero.
 *
 * Per polygon with type byte < 0x10: triangle 0 is corners +4, +8, +0xC into
 * +0x18/+0x30/+0x38, triangle 1 is +0x10, +0xC, +8 into +0x1C/+0x34/+0x3C;
 * each is the plane primitive then FUN_0041E9C6.  A corner index at or above
 * npnts skips the primitive, and the store then writes the SCRATCH as the last
 * call left it (0x0057B1E0, zero before any) - counted, returned.  The
 * 0x200 flag the same function sets at +2 is the renderer's and is not set.
 * Normalisation goes through sim_normalise_q, which is double where the
 * original is extended: a differing float needs a double-rounding straddle,
 * about 2^-29 per component.  Returns the count of stale-scratch stores. */
int sim_level_store_norms_q(uint8_t *pols, int32_t npols, const uint8_t *pnts,
                            int32_t npnts)
{
    static const uint8_t corner[2][3] = { { 0x04, 0x08, 0x0c },
                                          { 0x10, 0x0c, 0x08 } };
    static const uint8_t slot[2][3] = { { 0x18, 0x30, 0x38 },
                                        { 0x1c, 0x34, 0x3c } };
    float scratch[3] = { 0.0f, 0.0f, 0.0f };
    int32_t i, stale = 0;

    if (!pols || !pnts || npols < 0 || npnts < 0) return -1;
    for (i = 0; i < npols; i++) {
        uint8_t *rec = pols + (size_t)i * 0x40u;
        int t;
        if (rec[0] >= 0x10) continue;
        for (t = 0; t < 2; t++) {
            uint32_t idx[3];
            int c, ok = 1;
            for (c = 0; c < 3; c++) {
                memcpy(&idx[c], rec + corner[t][c], sizeof(uint32_t));
                if (idx[c] >= (uint32_t)npnts) ok = 0;
            }
            if (ok) {
                store_norms_plane(pnts, idx[0], idx[1], idx[2], scratch);
                (void)sim_normalise_q(scratch);
            } else {
                stale++;
            }
            for (c = 0; c < 3; c++)
                memcpy(rec + slot[t][c], &scratch[c], sizeof(float));
        }
    }
    return stale;
}

static int collision_polygon_edge_cf(const uint8_t *old_poly,
                                     const uint8_t *new_poly,
                                     int32_t *edge_start, int32_t *edge_end)
{
    static const uint8_t old_edge[4][2] = {
        { 0x04, 0x08 }, { 0x0c, 0x04 },
        { 0x10, 0x0c }, { 0x08, 0x10 }
    };
    static const uint8_t new_edge[4][2] = {
        { 0x04, 0x0c }, { 0x08, 0x04 },
        { 0x10, 0x08 }, { 0x0c, 0x10 }
    };
    int old_index, new_index;

    for (old_index = 0; old_index < 4; old_index++) {
        int32_t start, end;
        memcpy(&start, old_poly + old_edge[old_index][0], sizeof(start));
        memcpy(&end, old_poly + old_edge[old_index][1], sizeof(end));
        for (new_index = 0; new_index < 4; new_index++) {
            int32_t candidate_start, candidate_end;
            memcpy(&candidate_start, new_poly + new_edge[new_index][0],
                   sizeof(candidate_start));
            memcpy(&candidate_end, new_poly + new_edge[new_index][1],
                   sizeof(candidate_end));
            if (start == candidate_start && end == candidate_end) {
                *edge_start = start;
                *edge_end = end;
                return 0;
            }
        }
    }
    return 1;
}

/* 0x0045B495..0x0045B4CD: FCHS/FSTP each component of newspd, then
 * FUN_0041FFFB's length against 0.0 (C3: equal or unordered -> FLDZ into all
 * three), then FUN_0041E9C6. replaced a hand twin here that
 * summed (x+y)+z, kept a -0.0 component and let a NaN length through
 * sim_normalise_q's own zero test covers
 * the pre-zeroing, so the result is the same vector. */
static void collision_fallback_normal(const car_state *car, float normal[3])
{
    int axis;

    for (axis = 0; axis < 3; axis++) normal[axis] = -car->newspd[axis];
    (void)sim_normalise_q(normal);
}

static void collision_rollback_q(car_state *car, sim_ground_points *points,
                                 sim_world *w,
                                 const sim_collision_snapshot *snapshot)
{
    memcpy(car->pos, snapshot->pos, sizeof(snapshot->pos));
    memcpy(car->matrix, snapshot->matrix, sizeof(snapshot->matrix));
    memcpy(points->poly, snapshot->poly,
           (size_t)car->numcollpts * sizeof(snapshot->poly[0]));
    ++w->collision_retry_flag;
}

int sim_missed_point_response_q(car_state *car, const sim_consts *k,
                                const sim_ground *ground,
                                const float *sin_table, sim_world *w)
{
    const sim_ground_points *points;
    const uint8_t *pnt_start, *pnt_end;
    int32_t missed, edge_start, edge_end, dx, dz;
    uint16_t angle_word;
    uint32_t table_index;
    float normal[3], r[3], normal_component;
    int rc;

    if (!car || !k || !ground || !ground->geom || !ground->state
        || !sin_table || !w)
        return -1;
    w->collision_missed_point = 0;
    w->collision_missed_point_retry = 0;
    w->collision_missed_point_normal_speed = 0.0f;

    rc = sim_missed_point_edge_q(ground->geom, ground->state,
                                 &edge_start, &edge_end);
    if (rc <= 0) return rc;

    points = ground->state;
    missed = points->last_miss;
    pnt_start = ground->geom->pnts + (size_t)edge_start * 0x0cu;
    pnt_end = ground->geom->pnts + (size_t)edge_end * 0x0cu;
    memcpy(&dx, pnt_start, sizeof(dx));
    memcpy(&dz, pnt_start + 8, sizeof(dz));
    {
        int32_t end_x, end_z;
        memcpy(&end_x, pnt_end, sizeof(end_x));
        memcpy(&end_z, pnt_end + 8, sizeof(end_z));
        /* 0x0045BBB4..0x0045BBD5: EAX = [0x5C6EE8] (the edge's SECOND
         * vertex, EBX at 0x0045BB75) less [0x5C6EE0] (the first, popped into
         * EBX at 0x0045BB94): dx = end - start, then FPATAN(dx, dz).  This
         * line said start - end until, which flipped every
         * missed-point normal: the capture's 51 port-only spiral searches on
         * B28F frames, each a point the response pushed further out. */
        dx = (int32_t)((uint32_t)end_x - (uint32_t)dx);
        dz = (int32_t)((uint32_t)end_z - (uint32_t)dz);
    }

    /* FPATAN -> turns * 65536, then B796's rounded 1/4096-turn lookup. */
    angle_word = (uint16_t)lrint(atan2((double)dx, (double)dz)
                                 * (double)0.15915493667125702f * 65536.0);
    table_index = (uint16_t)(angle_word + 0x4008u) >> 4;
    normal[0] = sin_table[table_index];
    normal[1] = 0.0f;
    normal[2] = sin_table[table_index + 1024u];

    r[0] = points->wx[missed];
    r[1] = collision_store((double)points->wy[missed] * 0.1);
    r[2] = points->wz[missed];
    if (!w->collision_b796_seen) {
        memcpy(w->collision_b796_n, normal, sizeof(normal));
        memcpy(w->collision_b796_v, car->newspd, sizeof(w->collision_b796_v));
        w->collision_b796_seen = 1;
    }
    rc = collision_contact_impulse_q(car, k, w, normal, r, 0.0f,
                                    &w->collision_force[1],
                                    &normal_component, NULL);
    if (rc != 0) return rc;
    body_hit_sound(car, w, normal_component);          /* 0x0045BCE2      */
    w->collision_missed_point = 1;
    w->collision_missed_point_retry = 1;
    w->collision_missed_point_normal_speed = normal_component;
    return 1;
}

int sim_collision_snapshot_save(const car_state *car,
                                const sim_ground *ground,
                                sim_collision_snapshot *snapshot)
{
    if (!car || !ground || !ground->state || !snapshot
        || car->numcollpts < 0 || car->numcollpts > SIM_GROUND_POINTS)
        return -1;

    memcpy(snapshot->pos, car->pos, sizeof(snapshot->pos));
    memcpy(snapshot->matrix, car->matrix, sizeof(snapshot->matrix));
    memcpy(snapshot->poly, ground->state->poly,
           (size_t)car->numcollpts * sizeof(snapshot->poly[0]));
    return 0;
}

int sim_missed_point_collision_q(car_state *car, const sim_consts *k,
                                 const sim_ground *ground,
                                 const float *sin_table, sim_world *w,
                                 const sim_collision_snapshot *snapshot)
{
    sim_ground_points *points;
    float normal_speed;
    int rc;

    if (!car || !k || !ground || !ground->state || !sin_table || !w
        || !snapshot || car->numcollpts < 0
        || car->numcollpts > SIM_GROUND_POINTS)
        return -1;

    points = ground->state;
    if (points->last_miss == -1) return 0;
    if (points->last_miss < 0 || points->last_miss >= car->numcollpts)
        return -1;

    if (!ground->in.pts || points->last_miss >= ground->in.pts->n)
        return -1;
    car->collision_scratch_224[0] = ground->in.pts->x[points->last_miss];
    car->collision_scratch_224[1] = ground->in.pts->y[points->last_miss];
    car->collision_scratch_224[2] = ground->in.pts->z[points->last_miss];
    collision_rollback_q(car, points, w, snapshot);
    rc = sim_missed_point_response_q(car, k, ground, sin_table, w);
    if (rc <= 0) return rc;

    normal_speed = fabsf(w->collision_missed_point_normal_speed);
    if (normal_speed >= car->collision_peak_normal_speed) {
        car->collision_peak_normal_speed = normal_speed;
        w->collision_peak_normal_speed = normal_speed;
    } else {
        w->collision_peak_normal_speed = car->collision_peak_normal_speed;
    }
    car->collision_active = 1;
    car->collision_response_timer = 0x32;
    w->collision_active = 1;
    w->collision_response_timer = 0x32;
    return 1;
}

int sim_polygon_transition_collision_q(car_state *car, const sim_consts *k,
                                       const sim_ground *ground,
                                       const float *sin_table, sim_world *w,
                                       const sim_collision_snapshot *snapshot)
{
    sim_ground_points *points;
    uint32_t current_flags = 0;
    int32_t transition = -1, old_id = -1, new_id = -1;
    int32_t i;

    if (!car || !k || !ground || !ground->geom || !ground->state
        || !sin_table || !w || !snapshot
        || ground->geom->npols < 0 || !ground->geom->pols
        || ground->geom->npnts < 0 || !ground->geom->pnts
        || car->numcollpts < 0 || car->numcollpts > SIM_GROUND_POINTS
        || !ground->in.pts || ground->in.pts->n < car->numcollpts)
        return -1;

    points = ground->state;
    for (i = 0; i < car->numcollpts; i++) {
        const uint8_t *new_poly;
        uint16_t new_flags, old_flags;

        new_id = points->poly[i];
        if (new_id < 0 || new_id >= ground->geom->npols) continue;
        new_poly = ground->geom->pols + (size_t)new_id * 0x40u;
        memcpy(&new_flags, new_poly + 2, sizeof(new_flags));
        current_flags |= new_flags;

        old_id = snapshot->poly[i];
        if (new_id == old_id || old_id < 0
            || old_id >= ground->geom->npols)
            continue;
        memcpy(&old_flags,
               ground->geom->pols + (size_t)old_id * 0x40u + 2,
               sizeof(old_flags));
        if ((((uint32_t)new_flags | old_flags) & 0x0c00u) != 0x0c00u)
            continue;
        transition = i;
        break;
    }

    if (transition >= 0) {
        const uint8_t *old_poly = ground->geom->pols
                                + (size_t)old_id * 0x40u;
        const uint8_t *new_poly = ground->geom->pols
                                + (size_t)new_id * 0x40u;
        float normal[3], point[3] = { 0.0f, 0.0f, 0.0f };
        float normal_component;
        int32_t edge_start, edge_end;
        int edge_cf;

        car->collision_scratch_224[0] = ground->in.pts->x[transition];
        car->collision_scratch_224[1] = ground->in.pts->y[transition];
        car->collision_scratch_224[2] = ground->in.pts->z[transition];
        edge_cf = collision_polygon_edge_cf(old_poly, new_poly,
                                            &edge_start, &edge_end);
        if (edge_cf == 0) {
            const uint8_t *start_point, *end_point;
            int32_t start_x, start_z, end_x, end_z, dx, dz;
            uint16_t angle_word;
            uint32_t table_index;

            if (edge_start < 0 || edge_start >= ground->geom->npnts
                || edge_end < 0 || edge_end >= ground->geom->npnts)
                return -1;
            start_point = ground->geom->pnts + (size_t)edge_start * 0x0cu;
            end_point = ground->geom->pnts + (size_t)edge_end * 0x0cu;
            memcpy(&start_x, start_point, sizeof(start_x));
            memcpy(&start_z, start_point + 8, sizeof(start_z));
            memcpy(&end_x, end_point, sizeof(end_x));
            memcpy(&end_z, end_point + 8, sizeof(end_z));
            dx = (int32_t)((uint32_t)end_x - (uint32_t)start_x);
            dz = (int32_t)((uint32_t)end_z - (uint32_t)start_z);
            angle_word = (uint16_t)lrint(atan2((double)dx, (double)dz)
                                 * (double)0.15915493667125702f * 65536.0);
            table_index = (uint16_t)(angle_word + 0x4008u) >> 4;
            normal[0] = sin_table[table_index];
            normal[1] = 0.0f;
            normal[2] = sin_table[table_index + 1024u];
            point[0] = points->wx[transition];
            point[1] = collision_store((double)points->wy[transition] * 0.1);
            point[2] = points->wz[transition];
        } else {
            collision_fallback_normal(car, normal);
        }

        if (collision_contact_impulse_q(car, k, w, normal, point, 0.0f,
                                        &w->collision_force[4],
                                        &normal_component, NULL) != 0)
            return -1;
        body_hit_sound(car, w, normal_component);      /* 0x0045B55C      */
        if (fabsf(normal_component) >= car->collision_peak_normal_speed) {
            car->collision_peak_normal_speed = fabsf(normal_component);
            w->collision_peak_normal_speed = fabsf(normal_component);
        } else {
            w->collision_peak_normal_speed = car->collision_peak_normal_speed;
        }
        car->collision_active = 1;
        car->collision_response_timer = 0x32;
        w->collision_active = 1;
        w->collision_response_timer = 0x32;
        collision_rollback_q(car, points, w, snapshot);
        return 1;
    }

    if ((current_flags & 0x0c00u) == 0x0c00u) {
        float normal[3], point[3] = { 0.0f, 0.0f, 0.0f }, nc = 0.0f;

        memcpy(car->collision_scratch_224, &car->cxspd,
               sizeof(car->collision_scratch_224));
        collision_fallback_normal(car, normal);
        if (collision_contact_impulse_q(car, k, w, normal, point, 0.0f,
                                        &w->collision_force[4], &nc,
                                        NULL) != 0)
            return -1;
        body_hit_sound(car, w, nc);                    /* 0x0045B764      */
        car->collision_active = 1;
        car->collision_response_timer = 0x32;
        w->collision_active = 1;
        w->collision_response_timer = 0x32;
        collision_rollback_q(car, points, w, snapshot);
        return 1;
    }
    return 0;
}

void sim_collision_save_cspd_q(const car_state *car, sim_world *w)
{
    if (!car || !w) return;
    w->collision_saved_cspd[0] = car->cxspd;
    w->collision_saved_cspd[1] = car->cyspd_q;
    w->collision_saved_cspd[2] = car->czspd;
}

int sim_collision_impact_q(car_state *car, const sim_consts *k,
                           const sim_world *w)
{
    const float *saved;
    float length, ratio = 0.0f;

    if (!car || !k || !w) return 0;
    saved = w->collision_saved_cspd;
    /* 0x0041FFFB and 0x00420027 both sum x + (y + z); only the length is
     * stored to a float before use. */
    length = collision_store(sqrt((double)saved[0] * saved[0]
                                  + ((double)saved[1] * saved[1]
                                     + (double)saved[2] * saved[2])));
    /* AND AH,0x40 / JNE: the ratio is skipped only for an exact zero.  Retail
     * then leaves [0x005C6DE4] stale, but a zero length fails the next test. */
    if (length != 0.0f) {
        double dot = (double)car->cxspd * saved[0]
                   + ((double)car->cyspd_q * saved[1]
                      + (double)car->czspd * saved[2]);
        ratio = collision_store(dot / length / length);
    }
    /* FCOMP / JB returns on C0: less-than or unordered. */
    if (!(fabsf(length) >= k->impact_speed_min)) return 0;
    /* FCOMP / JAE returns on C0 clear; an unordered ratio fires. */
    if (ratio >= k->impact_retain_ratio) return 0;
    car->gear = 0;
    car->fwhlspd = 0.0f;
    car->rwhlspd = 0.0f;
    return 1;
}

void sim_airborne_flags_q(car_state *car)
{
    uint32_t susp[4];
    int front_airborne, rear_airborne, all_airborne;

    if (!car) return;
    memcpy(susp, car->susp, sizeof(susp));
    front_airborne = (susp[0] | susp[1]) == 0;
    rear_airborne = (susp[2] | susp[3]) == 0;
    all_airborne = front_airborne && rear_airborne;
    car->front_airborne = front_airborne;              /* 0x0043D67B, */
    car->drag_gate_q = rear_airborne;
    car->yaw_couple_gate = all_airborne;
    car->freerev = car->drive_class == 0 ? front_airborne : all_airborne;
    if (car->freerev == 0) car->freerev_clear_1e8_q = 0;  /* 0x0043D711 */
}

int sim_collision_refresh_ground_q(car_state *car, const sim_ground *ground,
                                   sim_world *w)
{
    int rc;

    if (!car || !ground || !ground->geom || !ground->state || !w) return -1;
    rc = sim_ground_query_contacts(ground->geom, car, &ground->in,
                                   ground->state);
    if (rc != 0) return rc;
    rc = sim_body_contact_candidate_q(car, ground, w);
    if (rc != 0) return rc;
    sim_airborne_flags_q(car);
    return 0;
}

static int sim_body_contact_internal_q(car_state *car, const sim_consts *k,
                                       const sim_ground *ground, sim_world *w,
                                       float *normal_component)
{
    sim_ground_points *points;
    const uint8_t *poly;
    int32_t min_clearance, index;
    float n[3], r[3], up_local[3] = { 0.0f, 1.0f, 0.0f };
    float up[3], mu, tangent_speed, yaw_scale;
    int32_t threshold;

    if (!car || !k || !ground || !ground->geom || !ground->state || !w
        || ground->geom->npols < 0 || !ground->geom->pols
        || car->numcollpts < 0 || car->numcollpts > SIM_GROUND_POINTS)
        return -1;

    points = ground->state;
    w->body_contact = 0;
    if (sim_body_contact_candidate_q(car, ground, w) != 0) return -1;
    /* Retail leaves both scratch values stale for four or fewer points. */
    min_clearance = w->body_contact_clearance;
    index = w->body_contact_index;
    w->body_contact_index = index;
    threshold = k->body_contact_threshold;
    if (index < 0 || index >= SIM_GROUND_POINTS
        || min_clearance >= threshold
        || (car->yaw_couple_gate == 0 && min_clearance >= 0))
        return 0;

    if (points->poly[index] < 0 || points->poly[index] >= ground->geom->npols
        || (points->tri[index] != 0 && points->tri[index] != 1))
        return -1;

    poly = ground->geom->pols + (size_t)points->poly[index] * 0x40u;
    if (points->tri[index] == 0) {
        n[0] = collision_f32(poly + 0x18);
        n[1] = collision_f32(poly + 0x30);
        n[2] = collision_f32(poly + 0x38);
    } else {
        n[0] = collision_f32(poly + 0x1C);
        n[1] = collision_f32(poly + 0x34);
        n[2] = collision_f32(poly + 0x3C);
    }

    r[0] = points->wx[index];
    r[1] = points->wy[index];
    r[2] = points->wz[index];

    sim_vec3_by_matrix(up_local, car->matrix, up);
    mu = collision_store(((double)(-collision_dot(up, n) + k->body_mu_base)
                           / 2.0 + k->body_mu_scale)
                         * k->gravity * k->dt * k->body_contact_friction
                         * w->mass);

    if (collision_contact_impulse_q(car, k, w, n, r, mu,
                                    &w->force[SIM_REC_BODY_CONTACT],
                                    normal_component, &tangent_speed) != 0)
        return -1;
    w->force[SIM_REC_BODY_CONTACT].p[0] = 0.0f;
    w->force[SIM_REC_BODY_CONTACT].p[1] = 0.0f;
    w->force[SIM_REC_BODY_CONTACT].p[2] = 0.0f;

    w->body_contact = 1;
    w->body_contact_16_16[0] = points->ix[index];
    w->body_contact_16_16[1] = points->ground[index];
    w->body_contact_16_16[2] = points->iz[index];
    w->body_contact_vt = tangent_speed;

    yaw_scale = collision_store(((double)car->matrix[5] + k->body_yaw_divisor)
                                / k->body_yaw_divisor);
    if (yaw_scale >= 1.0f) yaw_scale = 1.0f;
    car->rotspd[1] = collision_store((double)car->rotspd[1] * yaw_scale);
    return 1;
}

int sim_body_contact_q(car_state *car, const sim_consts *k,
                       const sim_ground *ground, sim_world *w)
{
    int rc = sim_body_contact_internal_q(car, k, ground, w, NULL);
    /* FUN_0045C90A's two sounds - the scrape after its impulse
     * (0x0045CAB0), its end on both early-outs (0x0045CB42) */
    if (rc >= 0) {
        sim_sound_view sv;
        sim_sound_feed *sf = sim_sound_feed_local(w, &sv);
        if (sf) {
            if (rc == 1) sim_sound_scrape_q(car, sf, &sv);
            else sim_sound_scrape_end_q(sf);
        }
    }
    return rc;
}

int sim_body_contact_loop_q(car_state *car, const sim_consts *k,
                            const sim_ground *ground, sim_world *w,
                            const sim_collision_snapshot *snapshot)
{
    sim_consts loop_consts;
    sim_force_rec saved_record, loop_record;
    float saved_yaw, point_world[3], point_local[3], normal_component;
    int index, rc;

    if (!car || !k || !ground || !ground->state || !w || !snapshot) return -1;
    saved_record = w->force[SIM_REC_BODY_CONTACT];
    saved_yaw = car->rotspd[1];
    loop_consts = *k;
    loop_consts.body_contact_friction = 0.0f;
    w->collision_body_contact = 0;
    w->collision_body_contact_retry = 0;

    rc = sim_body_contact_internal_q(car, &loop_consts, ground, w,
                                     &normal_component);
    if (rc == 1) {
        body_hit_sound(car, w, normal_component);      /* 0x0045CC75      */
        index = w->body_contact_index;
        if (!ground->in.pts || index < 0 || index >= ground->in.pts->n) {
            rc = -1;
            goto restore_state;
        }
        loop_record = w->force[SIM_REC_BODY_CONTACT];
        point_world[0] = ground->state->wx[index];
        point_world[1] = ground->state->wy[index];
        point_world[2] = ground->state->wz[index];
        sim_vec3_by_matrix(point_world, w->active_frame, point_local);
        memcpy(loop_record.p, point_local, sizeof(point_local));
        w->collision_force[2] = loop_record;
        car->collision_scratch_224[0] = ground->in.pts->x[index];
        car->collision_scratch_224[1] = ground->in.pts->y[index];
        car->collision_scratch_224[2] = ground->in.pts->z[index];
        if (fabsf(normal_component) >= car->collision_peak_normal_speed) {
            car->collision_peak_normal_speed = fabsf(normal_component);
            w->collision_peak_normal_speed = fabsf(normal_component);
        } else {
            w->collision_peak_normal_speed = car->collision_peak_normal_speed;
        }
        w->collision_body_contact = 1;
        w->collision_body_contact_retry = w->body_contact_clearance < 0;
        if (w->collision_body_contact_retry)
            collision_rollback_q(car, ground->state, w, snapshot);
    }

restore_state:
    w->force[SIM_REC_BODY_CONTACT] = saved_record;
    car->rotspd[1] = saved_yaw;
    return rc;
}

int sim_contact_impulse_q(const car_state *car, const sim_consts *k,
                          const sim_world *w, const float n[3],
                          const float r[3], float mu, sim_force_rec *record,
                          float *normal_component, float *tangent_speed_out)
{
    return collision_contact_impulse_q(car, k, w, n, r, mu, record,
                                       normal_component, tangent_speed_out);
}

void sim_collision_rollback_q(car_state *car, sim_ground_points *points,
                              sim_world *w,
                              const sim_collision_snapshot *snapshot)
{
    collision_rollback_q(car, points, w, snapshot);
}
