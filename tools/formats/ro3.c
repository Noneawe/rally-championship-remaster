/* ro3.c - see ro3.h. No I/O: the parser takes a buffer and nothing else. */

#include "ro3.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const ro3_frame_name[2] = { "+x+y+z", "+z+y-x" };

/* How many decidable polygons ro3_normal_frame() looks at before deciding.
 * The `frames` subcommand scans every polygon in every file; this is the
 * cheap version of the same test, for a loader. */
#define FRAME_SAMPLE 64

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static float rd_f32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && n) vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return -1;
}

int ro3_parse(const void *vbuf, size_t len, ro3_model *m, char *err, size_t errlen)
{
    const uint8_t *buf = (const uint8_t *)vbuf;
    int32_t npoints, npolys, nnormals;
    size_t off, need, i, c, total_corners = 0;

    memset(m, 0, sizeof(*m));
    m->_frame = -1;

    if (len < RO3_HDR_SIZE)
        return fail(err, errlen, "shorter than a header (%zu bytes)", len);

    m->size    = len;
    m->version = rd_i32(buf + 0);
    npoints    = rd_i32(buf + 4);
    npolys     = rd_i32(buf + 8);
    nnormals   = rd_i32(buf + 12);

    if (m->version != 1)
        return fail(err, errlen, "version %d, expected 1", m->version);
    if (npoints < 0 || npolys < 0 || nnormals < 0)
        return fail(err, errlen, "negative count in header: %d/%d/%d",
                    npoints, npolys, nnormals);
    memcpy(m->header_tail, buf + 0x10, sizeof(m->header_tail));

    off  = RO3_HDR_SIZE;
    need = (size_t)npoints * RO3_POINT_SIZE;
    if (off + need > len)
        return fail(err, errlen, "point array runs past EOF");
    off += need;

    /* Pass 1: walk the polygons for their total corner count, so the corner
     * arrays can be one allocation. Same walk and same bounds checks as
     * pass 2, transcribed from bld_ro3_index_polys: read corners, advance
     * 0x10 + corners * 0x10. No count is trusted but npolys. */
    {
        size_t scan = off;
        for (i = 0; i < (size_t)npolys; i++) {
            int32_t corners;
            if (scan + RO3_POLY_HDR_SIZE > len)
                return fail(err, errlen, "polygon header runs past EOF at 0x%zx", scan);
            corners = rd_i32(buf + scan);
            if (corners < 0 || corners > RO3_MAX_CORNERS)
                return fail(err, errlen, "implausible corner count %d at 0x%zx", corners, scan);
            scan += RO3_POLY_HDR_SIZE;
            if (scan + (size_t)corners * RO3_CORNER_SIZE > len)
                return fail(err, errlen, "corner array runs past EOF at 0x%zx", scan);
            scan += (size_t)corners * RO3_CORNER_SIZE;
            total_corners += (size_t)corners;
        }
        if (scan + (size_t)nnormals * RO3_NORMAL_SIZE > len)
            return fail(err, errlen, "normal array runs past EOF at 0x%zx", scan);
    }

    m->npoints  = (size_t)npoints;
    m->npolys   = (size_t)npolys;
    m->nnormals = (size_t)nnormals;
    m->ncorners = total_corners;

    m->points      = npoints  ? calloc(m->npoints,  sizeof *m->points)  : NULL;
    m->polys       = npolys   ? calloc(m->npolys,   sizeof *m->polys)   : NULL;
    m->normals     = nnormals ? calloc(m->nnormals, sizeof *m->normals) : NULL;
    m->corner_vert = total_corners ? calloc(total_corners, sizeof *m->corner_vert) : NULL;
    m->corner_u    = total_corners ? calloc(total_corners, sizeof *m->corner_u)    : NULL;
    m->corner_v    = total_corners ? calloc(total_corners, sizeof *m->corner_v)    : NULL;
    m->corner_norm = total_corners ? calloc(total_corners, sizeof *m->corner_norm) : NULL;
    if ((npoints && !m->points) || (npolys && !m->polys) || (nnormals && !m->normals) ||
        (total_corners && (!m->corner_vert || !m->corner_u ||
                           !m->corner_v || !m->corner_norm))) {
        ro3_free(m);
        return fail(err, errlen, "out of memory");
    }

    off = RO3_HDR_SIZE;
    for (i = 0; i < m->npoints; i++) {
        const uint8_t *p = buf + off + i * RO3_POINT_SIZE;
        m->points[i].x = rd_i32(p + 0) / RO3_FIXED_ONE;
        m->points[i].y = rd_i32(p + 4) / RO3_FIXED_ONE;
        m->points[i].z = rd_i32(p + 8) / RO3_FIXED_ONE;
    }
    off += (size_t)npoints * RO3_POINT_SIZE;

    {
        size_t cn = 0;
        for (i = 0; i < m->npolys; i++) {
            ro3_poly *p = &m->polys[i];
            p->corners      = rd_i32(buf + off + 0);
            p->flags        = rd_i32(buf + off + 4);
            p->selector     = rd_i32(buf + off + 8);
            p->normal_index = rd_i32(buf + off + 12);
            off += RO3_POLY_HDR_SIZE;
            p->first_corner = cn;
            for (c = 0; c < (size_t)p->corners; c++) {
                const uint8_t *q = buf + off + c * RO3_CORNER_SIZE;
                m->corner_vert[cn + c] = rd_i32(q + 0);
                m->corner_u[cn + c]    = rd_i32(q + 4) / RO3_FIXED_ONE;
                m->corner_v[cn + c]    = rd_i32(q + 8) / RO3_FIXED_ONE;
                m->corner_norm[cn + c] = rd_i32(q + 12);
            }
            off += (size_t)p->corners * RO3_CORNER_SIZE;
            cn  += (size_t)p->corners;
        }
    }

    for (i = 0; i < m->nnormals; i++) {
        const uint8_t *p = buf + off + i * RO3_NORMAL_SIZE;
        m->normals[i].x = rd_f32(p + 0);
        m->normals[i].y = rd_f32(p + 4);
        m->normals[i].z = rd_f32(p + 8);
    }
    off += (size_t)nnormals * RO3_NORMAL_SIZE;

    m->consumed = off;
    return 0;
}

void ro3_free(ro3_model *m)
{
    free(m->points);
    free(m->polys);
    free(m->normals);
    free(m->corner_vert);
    free(m->corner_u);
    free(m->corner_v);
    free(m->corner_norm);
    memset(m, 0, sizeof(*m));
    m->_frame = -1;
}

/* -- geometry ------------------------------------------------------------ */

ro3_vec ro3_sub(ro3_vec a, ro3_vec b)
{
    ro3_vec r; r.x = a.x - b.x; r.y = a.y - b.y; r.z = a.z - b.z; return r;
}

ro3_vec ro3_cross(ro3_vec a, ro3_vec b)
{
    ro3_vec r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

double ro3_dot(ro3_vec a, ro3_vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

int ro3_normalise(ro3_vec *v)
{
    double n = sqrt(ro3_dot(*v, *v));
    if (n < 1e-12) return 0;
    v->x /= n; v->y /= n; v->z /= n;
    return 1;
}

ro3_vec ro3_rot_y90(ro3_vec v)   { ro3_vec r; r.x =  v.z; r.y = v.y; r.z = -v.x; return r; }
ro3_vec ro3_unrot_y90(ro3_vec v) { ro3_vec r; r.x = -v.z; r.y = v.y; r.z =  v.x; return r; }

int ro3_face_normal(const ro3_model *m, const ro3_poly *p, ro3_vec *out)
{
    int32_t ia, ib, ic;
    ro3_vec n;
    if (p->corners < 3) return 0;
    ia = m->corner_vert[p->first_corner + 0];
    ib = m->corner_vert[p->first_corner + 1];
    ic = m->corner_vert[p->first_corner + 2];
    if (ia < 0 || (size_t)ia >= m->npoints ||
        ib < 0 || (size_t)ib >= m->npoints ||
        ic < 0 || (size_t)ic >= m->npoints) return 0;
    n = ro3_cross(ro3_sub(m->points[ib], m->points[ia]),
                  ro3_sub(m->points[ic], m->points[ia]));
    if (!ro3_normalise(&n)) return 0;
    *out = n;
    return 1;
}

double ro3_corner_angle(const ro3_model *m, const ro3_poly *p, size_t i)
{
    size_t n = (size_t)p->corners;
    int32_t ip, ia, ib;
    ro3_vec u, v;
    double d;
    if (!n) return 0.0;
    ip = m->corner_vert[p->first_corner + i];
    ia = m->corner_vert[p->first_corner + (i + n - 1) % n];
    ib = m->corner_vert[p->first_corner + (i + 1) % n];
    if (ip < 0 || (size_t)ip >= m->npoints ||
        ia < 0 || (size_t)ia >= m->npoints ||
        ib < 0 || (size_t)ib >= m->npoints) return 0.0;
    u = ro3_sub(m->points[ia], m->points[ip]);
    v = ro3_sub(m->points[ib], m->points[ip]);
    if (!ro3_normalise(&u) || !ro3_normalise(&v)) return 0.0;
    d = ro3_dot(u, v);
    if (d < -1.0) d = -1.0;
    if (d >  1.0) d =  1.0;
    return acos(d);
}

int ro3_normal_frame(ro3_model *m)
{
    size_t i, seen = 0;
    int hit_id = 0, hit_rot = 0;
    if (m->_frame >= 0) return m->_frame;
    for (i = 0; i < m->npolys && seen < FRAME_SAMPLE; i++) {
        ro3_poly *p = &m->polys[i];
        ro3_vec nv, fn;
        if (p->normal_index < 0 || (size_t)p->normal_index >= m->nnormals) continue;
        nv = m->normals[p->normal_index];
        if (!ro3_normalise(&nv)) continue;
        if (!ro3_face_normal(m, p, &fn)) continue;
        seen++;
        if (ro3_dot(nv, fn) > 0.99) hit_id++;
        if (ro3_dot(nv, ro3_rot_y90(fn)) > 0.99) hit_rot++;
    }
    m->_frame = (hit_rot > hit_id) ? RO3_FRAME_ROT_Y90 : RO3_FRAME_IDENTITY;
    return m->_frame;
}

int ro3_normal(ro3_model *m, int32_t i, ro3_vec *out)
{
    ro3_vec n;
    if (i < 0 || (size_t)i >= m->nnormals) return 0;
    n = m->normals[i];
    if (ro3_normal_frame(m) == RO3_FRAME_ROT_Y90) n = ro3_unrot_y90(n);
    if (!ro3_normalise(&n)) return 0;
    *out = n;
    return 1;
}
