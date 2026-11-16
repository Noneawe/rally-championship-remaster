/* objload.c - see objload.h.
 *
 * The tables here are the same ones port/obj/objscene.c transcribes for the
 * renderer (obj_bands, obj_const_model, the LOCALDAT model table).  They are
 * repeated rather than shared because objscene links the stage and the
 * platform layer and this loader has to build in the sim tools (and in
 * break_test.sh's ad hoc compile) without either; ground truth for both is
 * and. */

#include "objload.h"
#include "ro3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPETAB_OFF  0x0500u
#define TYPETAB_N    64u
#define CONST_FIRST  0x38
#define CONST_N      8
#define INFO_STRIDE  0x80u
#define INFO_RECORDS 1200u
#define INFO_CLASS   0x1Cu
#define INFO_MATERIAL 0x24u         /* 0x0045A8E1: the hit's impact sound  */
#define SIMOBJ_TYPES 0x50           /* sim_objects_set_type refuses above */

static const uint32_t const_model[CONST_N] = {
    0xcf, 0x79, 0xcc, 0xcd, 0xc8, 0xc9, 0xca, 0xcb
};

static const struct { uint32_t first, count; const char *dir, *info; } bands[4] = {
    {   0u, 400u, "a",      "a/OBJSINFO"      },
    { 400u, 200u, "b",      "b/CARSINFO"      },
    { 600u, 200u, "g/frnt", "g/frnt/FRNTINFO" },
    { 800u, 400u, "s/extr", "s/extr/EXTRINFO" }
};

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1u);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* The shipped tree is upper-case, the original's templates lower-case. */
static void *slurp_either(char *path, size_t *len)
{
    void *b = slurp(path, len);
    char *p;
    if (b) return b;
    for (p = path; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return slurp(path, len);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

int simobj_load_level(sim_objects *o, const sim_consts *k, const uint8_t *ld,
                      size_t ld_len, const char *files_root, simobj_stats *st)
{
    uint32_t model[SIMOBJ_TYPES];
    uint8_t *info = NULL;
    size_t got = 0;
    char path[1024];
    simobj_stats local;
    int t, i;

    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    if (!o || !k || !ld || !files_root || ld_len < TYPETAB_OFF + 4u * TYPETAB_N)
        return -1;

    memset(model, 0, sizeof(model));
    for (t = 1; t < (int)TYPETAB_N && t < SIMOBJ_TYPES; t++)
        model[t] = rd32(ld + TYPETAB_OFF + 4u * (unsigned)t);
    for (t = 0; t < CONST_N; t++)
        model[CONST_FIRST + t] = const_model[t];

    info = (uint8_t *)calloc(INFO_RECORDS, INFO_STRIDE);
    if (!info) return -2;
    for (i = 0; i < 4; i++) {
        size_t len = 0;
        void *buf;
        snprintf(path, sizeof(path), "%s/%s", files_root, bands[i].info);
        buf = slurp_either(path, &len);
        if (!buf) continue;
        if (len > (size_t)bands[i].count * INFO_STRIDE)
            len = (size_t)bands[i].count * INFO_STRIDE;
        memcpy(info + (size_t)bands[i].first * INFO_STRIDE, buf, len);
        free(buf);
        got += len;
    }
    st->have_info = got > 0;
    if (!st->have_info) { free(info); return -3; }

    for (t = 1; t < SIMOBJ_TYPES; t++) {
        ro3_model m;
        char err[128];
        const char *dir = bands[0].dir;
        void *buf;
        size_t len = 0;
        int32_t (*pts)[3];
        size_t n;
        int32_t cls;

        if (model[t] == 0u) continue;
        st->named++;
        /* FUN_00437DC7's ladder: a number past the last band still resolves
         * to that band. */
        for (i = 3; i >= 0; i--)
            if (model[t] >= bands[i].first) { dir = bands[i].dir; break; }
        snprintf(path, sizeof(path), "%s/%s/OBJ%04u.RO3", files_root, dir,
                 (unsigned)(model[t] % 10000u));
        buf = slurp_either(path, &len);
        if (!buf) {
            st->missing++;
            sim_objects_set_unloaded(o, t, 0);
            continue;
        }
        err[0] = '\0';
        if (ro3_parse(buf, len, &m, err, sizeof(err)) != 0) {
            free(buf);
            st->badparse++;
            sim_objects_set_unloaded(o, t, 0);
            continue;
        }
        free(buf);
        n = m.npoints;
        pts = (int32_t (*)[3])malloc((n ? n : 1u) * sizeof(*pts));
        if (!pts) { ro3_free(&m); free(info); return -2; }
        for (i = 0; i < (int)n; i++) {
            pts[i][0] = (int32_t)lrint(m.points[i].x * RO3_FIXED_ONE);
            pts[i][1] = (int32_t)lrint(m.points[i].y * RO3_FIXED_ONE);
            pts[i][2] = (int32_t)lrint(m.points[i].z * RO3_FIXED_ONE);
        }
        cls = model[t] < INFO_RECORDS
              ? (int32_t)info[(size_t)model[t] * INFO_STRIDE + INFO_CLASS] : 0;
        if (sim_objects_set_type(o, k, t, (int32_t)model[t], cls,
                                 (const int32_t (*)[3])pts, (int32_t)n) == 0) {
            if (model[t] < INFO_RECORDS)
                o->type[t].material = (int32_t)info[(size_t)model[t] * INFO_STRIDE
                                                    + INFO_MATERIAL];
            st->loaded++;
            st->points += (long)n;
        }
        free(pts);
        ro3_free(&m);
    }
    free(info);
    return 0;
}
