/* ro3_tool.c - the command-line half of the .ro3 toolkit.
 *
 * A transcription of tools/ro3.py into the plain C that asks
 * for. Every subcommand is intended to print BYTE-IDENTICAL output to the
 * Python tool over the same corpus, so `make check` is a plain diff and the
 * two are a genuine second route to the same figures rather than one
 * instrument quoted twice.
 *
 *     ro3 info      FILE [--polys N]   header + section walk
 *     ro3 obj       FILE [-o OUT.obj] [--all]
 *     ro3 validate  [ROOT]             does the container close at EOF?
 *     ro3 frames    [ROOT]             the normal table's coordinate frame
 *     ro3 normals   [ROOT] [--seed N]  corner +0x0c, with a shuffle control
 *     ro3 header08  [ROOT] [--top N]   polygon header +0x08
 *     ro3 repeats   [ROOT]             the exporter's own accept/reject rule
 *     ro3 lwo       [ROOT]             reconstruction vs paired .lwo POLS
 *     ro3 uv        [ROOT]             texel range -> the texture page size
 *
 * Every subcommand that reports a figure prints the population it measured
 * over, so a number in a document can name the command that produced it.
*/

#include "corpus.h"
#include "lwo.h"
#include "pyrandom.h"
#include "ro3.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEFAULT_ROOT "original_files"

/* ------------------------------------------------------------------ */
/* a counting map, keyed on uint32, remembering insertion order so that
 * "commonest N" can break ties the way collections.Counter does        */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t key; long count; long seq; int used; } u32ent;
typedef struct { u32ent *e; size_t cap, n; long seq; } u32map;

static void u32map_init(u32map *m)
{
    m->cap = 256; m->n = 0; m->seq = 0;
    m->e = calloc(m->cap, sizeof *m->e);
}

static void u32map_free(u32map *m) { free(m->e); memset(m, 0, sizeof *m); }

static void u32map_grow(u32map *m);

static void u32map_add(u32map *m, uint32_t key, long by)
{
    size_t i;
    if ((m->n + 1) * 10 >= m->cap * 7) u32map_grow(m);
    i = (size_t)(key * 2654435761u) & (m->cap - 1);
    for (;;) {
        if (!m->e[i].used) {
            m->e[i].used = 1; m->e[i].key = key;
            m->e[i].count = by; m->e[i].seq = m->seq++;
            m->n++;
            return;
        }
        if (m->e[i].key == key) { m->e[i].count += by; return; }
        i = (i + 1) & (m->cap - 1);
    }
}

static void u32map_grow(u32map *m)
{
    u32map bigger;
    size_t i;
    bigger.cap = m->cap * 2; bigger.n = 0; bigger.seq = m->seq;
    bigger.e = calloc(bigger.cap, sizeof *bigger.e);
    for (i = 0; i < m->cap; i++) {
        if (!m->e[i].used) continue;
        {
            size_t j = (size_t)(m->e[i].key * 2654435761u) & (bigger.cap - 1);
            while (bigger.e[j].used) j = (j + 1) & (bigger.cap - 1);
            bigger.e[j] = m->e[i];
            bigger.n++;
        }
    }
    free(m->e);
    *m = bigger;
}

/* entries, flattened; caller frees */
static u32ent *u32map_entries(const u32map *m, size_t *n)
{
    u32ent *out = malloc((m->n ? m->n : 1) * sizeof *out);
    size_t i, k = 0;
    for (i = 0; i < m->cap; i++) if (m->e[i].used) out[k++] = m->e[i];
    *n = k;
    return out;
}

static int cmp_by_count(const void *a, const void *b)
{
    const u32ent *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;  /* stable, like nlargest */
}

static int cmp_by_key(const void *a, const void *b)
{
    const u32ent *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key;
}

/* ------------------------------------------------------------------ */
/* a counting map keyed on a string, for .lwo surface names             */
/* ------------------------------------------------------------------ */

typedef struct { char *name; long count; long seq; } sent;
typedef struct { sent *e; size_t n, cap; long seq; } smap;

static void smap_add(smap *m, const char *name)
{
    size_t i;
    for (i = 0; i < m->n; i++)
        if (strcmp(m->e[i].name, name) == 0) { m->e[i].count++; return; }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->e = realloc(m->e, m->cap * sizeof *m->e);
    }
    m->e[m->n].name = strdup(name);
    m->e[m->n].count = 1;
    m->e[m->n].seq = m->seq++;
    m->n++;
}

static void smap_free(smap *m)
{
    size_t i;
    for (i = 0; i < m->n; i++) free(m->e[i].name);
    free(m->e);
    memset(m, 0, sizeof *m);
}

static int cmp_sent(const void *a, const void *b)
{
    const sent *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* ------------------------------------------------------------------ */
/* corpus iteration                                                     */
/* ------------------------------------------------------------------ */

/* Load and parse one .ro3. Returns 0 on success. Mirrors ro3.py's `_iter`,
 * which silently skips a file it cannot parse - `validate` is the subcommand
 * that reports those. */
static int load_ro3(const char *path, ro3_model *m, char *err, size_t errlen)
{
    size_t len;
    void *buf = corpus_slurp(path, &len);
    int r;
    if (!buf) { snprintf(err, errlen, "cannot read"); return -1; }
    r = ro3_parse(buf, len, m, err, errlen);
    free(buf);
    return r;
}

static const char *dirname_of(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    size_t l = slash ? (size_t)(slash - path) : 0;
    if (l >= n) l = n - 1;
    memcpy(out, path, l);
    out[l] = 0;
    return out;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------ */
/* info                                                                 */
/* ------------------------------------------------------------------ */

static int cmd_info(const char *path, int npolys_to_dump)
{
    ro3_model m;
    char err[256];
    u32map corners, pages;
    u32ent *ent;
    size_t n, i;
    long rej = 0;
    int all_zero = 1;

    if (load_ro3(path, &m, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }
    printf("%s  %zu bytes\n", path, m.size);
    printf("  version       %d\n", m.version);
    printf("  npoints       %zu\n", m.npoints);
    printf("  npolys        %zu\n", m.npolys);
    printf("  nnormals      %zu\n", m.nnormals);
    for (i = 0; i < sizeof m.header_tail; i++) if (m.header_tail[i]) all_zero = 0;
    if (all_zero) {
        printf("  0x10..0x2b    all zero\n");
    } else {
        printf("  0x10..0x2b    ");
        for (i = 0; i < sizeof m.header_tail; i++) printf("%02x", m.header_tail[i]);
        printf("\n");
    }
    printf("  normal frame  %s\n", ro3_frame_name[ro3_normal_frame(&m)]);
    printf("  walk ends at 0x%zx, EOF 0x%zx  -> %s\n",
           m.consumed, m.size, ro3_closes_at_eof(&m) ? "EXACT" : "MISMATCH");

    u32map_init(&corners);
    u32map_init(&pages);
    for (i = 0; i < m.npolys; i++) {
        u32map_add(&corners, (uint32_t)m.polys[i].corners, 1);
        u32map_add(&pages, (uint32_t)ro3_texture_page(&m.polys[i]), 1);
        if (!ro3_exporter_accepts(&m.polys[i])) rej++;
    }
    printf("  corner counts ");
    ent = u32map_entries(&corners, &n);
    qsort(ent, n, sizeof *ent, cmp_by_key);
    for (i = 0; i < n; i++) printf("%s%u->%ld", i ? ", " : "", ent[i].key, ent[i].count);
    printf("\n");
    free(ent);
    printf("  texture pages ");
    ent = u32map_entries(&pages, &n);
    qsort(ent, n, sizeof *ent, cmp_by_key);
    for (i = 0; i < n; i++) printf("%s%u->%ld", i ? ", " : "", ent[i].key, ent[i].count);
    printf("\n");
    free(ent);
    printf("  exporter-rejected (`numrepeats`) %ld of %zu\n", rej, m.npolys);

    for (i = 0; i < m.npolys && i < (size_t)npolys_to_dump; i++) {
        ro3_poly *p = &m.polys[i];
        size_t c;
        printf("  poly %-4zu corners=%d +04=0x%08x +08=0x%08x nrm=%d %s\n",
               i, p->corners, (unsigned)p->flags, (unsigned)p->selector,
               p->normal_index, ro3_exporter_accepts(p) ? "" : "REJECTED");
        for (c = 0; c < (size_t)p->corners; c++)
            printf("      v=%-5d u=%9.3f v=%9.3f vn=%d\n",
                   m.corner_vert[p->first_corner + c],
                   m.corner_u[p->first_corner + c],
                   m.corner_v[p->first_corner + c],
                   m.corner_norm[p->first_corner + c]);
    }

    u32map_free(&corners);
    u32map_free(&pages);
    ro3_free(&m);
    return 0;
}

/* ------------------------------------------------------------------ */
/* obj                                                                  */
/* ------------------------------------------------------------------ */

static int cmd_obj(const char *path, const char *outpath, double page,
                   int include_all, int quiet)
{
    ro3_model m;
    char err[256], name[256], out_buf[1024];
    const char *base;
    FILE *fh;
    size_t i, vt = 1, written = 0;
    int cur_page = -1, frame;
    const char *dot;

    if (load_ro3(path, &m, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }
    base = basename_of(path);
    dot = strrchr(base, '.');
    {
        size_t l = dot ? (size_t)(dot - base) : strlen(base);
        if (l >= sizeof name) l = sizeof name - 1;
        memcpy(name, base, l);
        name[l] = 0;
    }
    if (!outpath) {
        snprintf(out_buf, sizeof out_buf, "%s.obj", name);
        outpath = out_buf;
    }
    fh = fopen(outpath, "w");
    if (!fh) { perror(outpath); ro3_free(&m); return 1; }

    frame = ro3_normal_frame(&m);
    fprintf(fh, "# %s -> Wavefront OBJ, tools/formats/ro3\n", base);
    fprintf(fh, "# %zu points, %zu polygons, %zu normals; normal frame %s\n",
            m.npoints, m.npolys, m.nnormals, ro3_frame_name[frame]);
    fprintf(fh, "# u/v are 16.16 texel units over a %g-texel page\n", page);
    for (i = 0; i < m.npoints; i++)
        fprintf(fh, "v %.6f %.6f %.6f\n", m.points[i].x, m.points[i].y, m.points[i].z);
    /* One vt per corner: .ro3 texture coordinates are per-corner, so they
     * cannot be folded into the vertex array. v is flipped for OBJ's
     * bottom-left origin. */
    for (i = 0; i < m.ncorners; i++)
        fprintf(fh, "vt %.6f %.6f\n", m.corner_u[i] / page, 1.0 - m.corner_v[i] / page);
    for (i = 0; i < m.nnormals; i++) {
        ro3_vec nv;
        if (!ro3_normal(&m, (int32_t)i, &nv)) { nv.x = 0.0; nv.y = 1.0; nv.z = 0.0; }
        fprintf(fh, "vn %.6f %.6f %.6f\n", nv.x, nv.y, nv.z);
    }
    for (i = 0; i < m.npolys; i++) {
        ro3_poly *p = &m.polys[i];
        size_t c;
        if (!include_all && !ro3_exporter_accepts(p)) { vt += (size_t)p->corners; continue; }
        if (ro3_texture_page(p) != cur_page) {
            cur_page = ro3_texture_page(p);
            fprintf(fh, "g %s_page%d\n", name, cur_page);
        }
        fprintf(fh, "f");
        for (c = 0; c < (size_t)p->corners; c++) {
            int32_t cn = m.corner_norm[p->first_corner + c];
            int32_t v  = m.corner_vert[p->first_corner + c];
            if (m.nnormals) {
                int32_t nref = ((cn >= 0 && (size_t)cn < m.nnormals) ? cn : p->normal_index) + 1;
                fprintf(fh, " %d/%zu/%d", v + 1, vt, nref);
            } else {
                /* 53 of the 429 files carry no normal table at all. */
                fprintf(fh, " %d/%zu", v + 1, vt);
            }
            vt++;
        }
        fprintf(fh, "\n");
        written++;
    }
    fclose(fh);
    if (!quiet)
        printf("wrote %s  (%zu v, %zu f, %zu vn, frame %s)\n",
               outpath, m.npoints, written, m.nnormals, ro3_frame_name[frame]);
    ro3_free(&m);
    return 0;
}

/* ------------------------------------------------------------------ */
/* validate                                                             */
/* ------------------------------------------------------------------ */

static int cmd_validate(const char *root)
{
    corpus_list files;
    size_t i, j;
    long ok = 0, exact = 0;
    double tpoints = 0, tpolys = 0, tcorners = 0, tnormals = 0;
    char **bad_path = NULL, **bad_why = NULL;
    size_t nbad = 0;
    char **hdr = NULL;
    size_t nhdr = 0;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    bad_path = calloc(files.n + 1, sizeof *bad_path);
    bad_why  = calloc(files.n + 1, sizeof *bad_why);
    hdr      = calloc(files.n + 1, sizeof *hdr);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256];
        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) {
            bad_path[nbad] = strdup(files.path[i]);
            bad_why[nbad++] = strdup(err);
            continue;
        }
        ok++;
        if (ro3_closes_at_eof(&m)) {
            exact++;
        } else {
            char why[128];
            snprintf(why, sizeof why, "walk ends at 0x%zx, EOF 0x%zx", m.consumed, m.size);
            bad_path[nbad] = strdup(files.path[i]);
            bad_why[nbad++] = strdup(why);
        }
        tpoints  += (double)m.npoints;
        tpolys   += (double)m.npolys;
        tcorners += (double)m.ncorners;
        tnormals += (double)m.nnormals;
        for (j = 0; j < sizeof m.header_tail; j++)
            if (m.header_tail[j]) { hdr[nhdr++] = strdup(files.path[i]); break; }
        ro3_free(&m);
    }

    printf("files            %zu\n", files.n);
    printf("parsed           %ld\n", ok);
    printf("closed at EOF    %ld\n", exact);
    printf("%-16s %.0f\n", "points", tpoints);
    printf("%-16s %.0f\n", "polygons", tpolys);
    printf("%-16s %.0f\n", "corners", tcorners);
    printf("%-16s %.0f\n", "normals", tnormals);
    printf("header 0x10..0x2b non-zero: %zu  ", nhdr);
    for (i = 0; i < nhdr; i++) printf("%s%s", i ? ", " : "", hdr[i]);
    printf("\n");
    for (i = 0; i < nbad; i++) printf("FAIL %s: %s\n", bad_path[i], bad_why[i]);

    for (i = 0; i < nbad; i++) { free(bad_path[i]); free(bad_why[i]); }
    for (i = 0; i < nhdr; i++) free(hdr[i]);
    free(bad_path); free(bad_why); free(hdr);
    corpus_free(&files);
    return nbad ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* frames                                                               */
/* ------------------------------------------------------------------ */

typedef struct { char dir[512]; int frame; long files; } dirframe;

static int cmp_dirframe(const void *a, const void *b)
{
    const dirframe *x = a, *y = b;
    int c = strcmp(x->dir, y->dir);
    if (c) return c;
    return strcmp(ro3_frame_name[x->frame], ro3_frame_name[y->frame]);
}

static int cmd_frames(const char *root)
{
    corpus_list files;
    dirframe *per = NULL;
    size_t nper = 0, capper = 0, i;
    double in_files = 0, ex_range = 0, ex_zero = 0, ex_degen = 0;
    double decidable = 0, agree_id = 0, agree_rot = 0;
    char **mix_path = NULL;
    long *mix_a = NULL, *mix_b = NULL;
    size_t nmix = 0;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    mix_path = calloc(files.n + 1, sizeof *mix_path);
    mix_a = calloc(files.n + 1, sizeof *mix_a);
    mix_b = calloc(files.n + 1, sizeof *mix_b);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256], dir[512];
        long hit_id = 0, hit_rot = 0, n = 0, best_hits;
        int best;
        size_t k;

        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        if (!m.nnormals) { ro3_free(&m); continue; }

        for (k = 0; k < m.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            ro3_vec nv, fn;
            in_files++;
            if (p->normal_index < 0 || (size_t)p->normal_index >= m.nnormals) {
                ex_range++; continue;
            }
            nv = m.normals[p->normal_index];
            if (!ro3_normalise(&nv)) { ex_zero++; continue; }
            if (!ro3_face_normal(&m, p, &fn)) { ex_degen++; continue; }
            n++;
            if (ro3_dot(nv, fn) > 0.99) hit_id++;
            if (ro3_dot(nv, ro3_rot_y90(fn)) > 0.99) hit_rot++;
        }
        if (!n) { ro3_free(&m); continue; }

        /* Ties go to IDENTITY, matching max() over a dict whose first key is
         * the identity frame. */
        best = (hit_rot > hit_id) ? RO3_FRAME_ROT_Y90 : RO3_FRAME_IDENTITY;
        best_hits = (best == RO3_FRAME_ROT_Y90) ? hit_rot : hit_id;

        dirname_of(files.path[i], dir, sizeof dir);
        {
            size_t j;
            int found = 0;
            for (j = 0; j < nper; j++)
                if (per[j].frame == best && strcmp(per[j].dir, dir) == 0) {
                    per[j].files++; found = 1; break;
                }
            if (!found) {
                if (nper == capper) {
                    capper = capper ? capper * 2 : 16;
                    per = realloc(per, capper * sizeof *per);
                }
                snprintf(per[nper].dir, sizeof per[nper].dir, "%s", dir);
                per[nper].frame = best;
                per[nper].files = 1;
                nper++;
            }
        }
        if (best == RO3_FRAME_ROT_Y90) agree_rot += (double)best_hits;
        else                           agree_id  += (double)best_hits;
        decidable += (double)n;
        if (best_hits != n) {
            mix_path[nmix] = strdup(files.path[i]);
            mix_a[nmix] = best_hits;
            mix_b[nmix] = n;
            nmix++;
        }
        ro3_free(&m);
    }

    qsort(per, nper, sizeof *per, cmp_dirframe);
    printf("directory                                    frame    files\n");
    for (i = 0; i < nper; i++)
        printf("  %-42s %-8s %ld\n", per[i].dir, ro3_frame_name[per[i].frame], per[i].files);
    printf("\n");
    printf("polygons in the files that have a normal table: %.0f\n", in_files);
    printf("  %-34s %.0f\n", "excluded: index out of range", ex_range);
    printf("  %-34s %.0f\n", "excluded: stored normal is zero", ex_zero);
    printf("  %-34s %.0f\n", "excluded: face is degenerate", ex_degen);
    printf("  -> decidable population                %.0f\n", decidable);
    printf("     agree under %s: %.0f\n", ro3_frame_name[RO3_FRAME_IDENTITY], agree_id);
    printf("     agree under %s: %.0f\n", ro3_frame_name[RO3_FRAME_ROT_Y90], agree_rot);
    printf("     total agreeing: %.0f  (%.2f%%)\n", agree_id + agree_rot,
           100.0 * (agree_id + agree_rot) / (decidable < 1 ? 1 : decidable));
    printf("\n");
    printf("The excluded polygons are UNDECIDABLE, not disagreeing: a degenerate\n");
    printf("face has no geometric normal to compare against. This instrument drops\n");
    printf("what it cannot decide rather than scoring it as agreement, so the\n");
    printf("percentage is over a defined subset and cannot be inflated by them.\n");
    printf("files whose polygons do NOT all agree under one frame: %zu\n", nmix);
    for (i = 0; i < nmix && i < 20; i++)
        printf("   %s %ld/%ld\n", mix_path[i], mix_a[i], mix_b[i]);

    for (i = 0; i < nmix; i++) free(mix_path[i]);
    free(mix_path); free(mix_a); free(mix_b); free(per);
    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* normals                                                              */
/* ------------------------------------------------------------------ */

static const char *const BINS[6] = { ">0.99", ">0.90", ">0.50", ">0.00", "<=0.00", "<=-0.50" };

static int bin_of(double d)
{
    if (d >  0.99) return 0;
    if (d >  0.9)  return 1;
    if (d >  0.5)  return 2;
    if (d >  0.0)  return 3;
    if (d > -0.5)  return 4;
    return 5;
}

static void hist_print(const double *bins)
{
    double tot = 0;
    int i;
    for (i = 0; i < 6; i++) tot += bins[i];
    if (tot == 0) tot = 1;
    for (i = 0; i < 6; i++)
        if (bins[i] != 0)
            printf("   %-8s %8.0f  %6.2f%%\n", BINS[i], bins[i], 100.0 * bins[i] / tot);
}

/* one corner, tagged with the normal index it is grouped under */
typedef struct { int32_t key; uint32_t poly; uint32_t corner; uint32_t seq; } member;

static int cmp_member(const void *a, const void *b)
{
    const member *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    /* stable within a group: the summation order has to match Python's, or
     * the floating-point totals differ in the last bits */
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void accumulate_groups(ro3_model *m, member *mem, size_t nmem, double *bins)
{
    size_t i = 0;
    qsort(mem, nmem, sizeof *mem, cmp_member);
    while (i < nmem) {
        size_t j = i;
        ro3_vec acc, nv;
        acc.x = acc.y = acc.z = 0.0;
        while (j < nmem && mem[j].key == mem[i].key) {
            ro3_poly *p = &m->polys[mem[j].poly];
            ro3_vec fn;
            if (ro3_face_normal(m, p, &fn)) {
                double w = ro3_corner_angle(m, p, mem[j].corner);
                acc.x += fn.x * w; acc.y += fn.y * w; acc.z += fn.z * w;
            }
            j++;
        }
        if (ro3_normal(m, mem[i].key, &nv) && ro3_normalise(&acc))
            bins[bin_of(ro3_dot(nv, acc))] += 1.0;
        i = j;
    }
}

static int cmd_normals(const char *root, uint32_t seed)
{
    corpus_list files;
    size_t i;
    double real[6] = {0}, ctrl[6] = {0};
    double ncorner = 0, in_range = 0, unused = 0, ntot = 0;
    long nfiles = 0, cover_exact = 0, cover_slack = 0;
    int first_cover = -1;   /* 0 exact, 1 slack - Counter prints in insertion order */

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    py_seed(seed);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256];
        unsigned char *used;
        member *mem;
        int32_t *table;
        size_t k, c, nmem = 0, nused = 0;

        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        if (!m.nnormals) { ro3_free(&m); continue; }
        nfiles++;

        used = calloc(m.nnormals, 1);
        for (k = 0; k < m.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            if (p->normal_index >= 0 && (size_t)p->normal_index < m.nnormals)
                used[p->normal_index] = 1;
            ncorner += (double)p->corners;
            for (c = 0; c < (size_t)p->corners; c++) {
                int32_t cn = m.corner_norm[p->first_corner + c];
                if (cn >= 0 && (size_t)cn < m.nnormals) { used[cn] = 1; in_range += 1.0; }
            }
        }
        for (k = 0; k < m.nnormals; k++) nused += used[k];
        ntot   += (double)m.nnormals;
        unused += (double)(m.nnormals - nused);
        if (nused == m.nnormals) { cover_exact++; if (first_cover < 0) first_cover = 0; }
        else                     { cover_slack++; if (first_cover < 0) first_cover = 1; }
        free(used);

        mem = malloc((m.ncorners ? m.ncorners : 1) * sizeof *mem);
        table = malloc((m.ncorners ? m.ncorners : 1) * sizeof *table);
        for (k = 0; k < m.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            for (c = 0; c < (size_t)p->corners; c++) {
                mem[nmem].key    = m.corner_norm[p->first_corner + c];
                mem[nmem].poly   = (uint32_t)k;
                mem[nmem].corner = (uint32_t)c;
                mem[nmem].seq    = (uint32_t)nmem;
                table[nmem] = mem[nmem].key;
                nmem++;
            }
        }
        accumulate_groups(&m, mem, nmem, real);

        /* The control shuffles the same multiset of indices across the same
         * corners: if the grouping carried no information the two histograms
         * would match. */
        py_shuffle(table, nmem);
        for (k = 0; k < nmem; k++) { mem[k].key = table[k]; mem[k].seq = (uint32_t)k; }
        /* mem was sorted by accumulate_groups, so poly/corner no longer run in
         * order; rebuild it so index k is corner k again. */
        {
            size_t idx = 0;
            for (k = 0; k < m.npolys; k++) {
                ro3_poly *p = &m.polys[k];
                for (c = 0; c < (size_t)p->corners; c++) {
                    mem[idx].poly = (uint32_t)k;
                    mem[idx].corner = (uint32_t)c;
                    mem[idx].key = table[idx];
                    mem[idx].seq = (uint32_t)idx;
                    idx++;
                }
            }
        }
        accumulate_groups(&m, mem, nmem, ctrl);

        free(mem);
        free(table);
        ro3_free(&m);
    }

    printf("files with a normal table            %ld\n", nfiles);
    printf("corners                              %.0f\n", ncorner);
    printf("corner +0x0c in [0, nnormals)        %.0f  (%.2f%%)\n",
           in_range, 100.0 * in_range / (ncorner < 1 ? 1 : ncorner));
    /* Printed the way collections.Counter does: keys in first-seen order, and
     * a key that never occurred does not exist at all rather than reading 0. */
    {
        const char *name[2];
        long count[2];
        int n = 0, order[2], j;
        order[0] = (first_cover == 1) ? 1 : 0;
        order[1] = 1 - order[0];
        for (j = 0; j < 2; j++) {
            long c = order[j] ? cover_slack : cover_exact;
            if (c == 0) continue;
            name[n] = order[j] ? "slack" : "exact";
            count[n] = c;
            n++;
        }
        printf("normal-table coverage                {");
        for (j = 0; j < n; j++)
            printf("%s'%s': %ld", j ? ", " : "", name[j], count[j]);
        printf("}\n");
    }
    printf("normals referenced by no face and no corner: %.0f of %.0f\n", unused, ntot);
    printf("\ndot(normals[cn], angle-weighted mean of the face normals of the corners naming cn)\n");
    printf("NOTE: one row per distinct index cn - the denominator is GROUPS, not corners.\n");
    printf("REAL:\n");
    hist_print(real);
    printf("CONTROL - same multiset of indices, shuffled across the same corners:\n");
    hist_print(ctrl);

    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* header08                                                             */
/* ------------------------------------------------------------------ */

static int cmd_header08(const char *root, int top)
{
    corpus_list files;
    u32map vals, per_file_distinct;
    double bits[32] = {0};
    double n = 0, in_pts = 0, in_polys = 0, in_norms = 0, zero = 0;
    long nfiles = 0;
    size_t i;
    u32ent *ent;
    size_t nent;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    u32map_init(&vals);
    u32map_init(&per_file_distinct);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256];
        u32map seen;
        size_t k;
        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        nfiles++;
        u32map_init(&seen);
        for (k = 0; k < m.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            uint32_t v = (uint32_t)p->selector;
            int b;
            n++;
            u32map_add(&vals, v, 1);
            u32map_add(&seen, v, 1);
            if (v == 0) zero++;
            /* SIGNED comparisons: these are the index readings being tested. */
            if (p->selector >= 0 && (size_t)p->selector < m.npoints)  in_pts++;
            if (p->selector >= 0 && (size_t)p->selector < m.npolys)   in_polys++;
            if (p->selector >= 0 && (size_t)p->selector < m.nnormals) in_norms++;
            for (b = 0; b < 32; b++) if ((v >> b) & 1u) bits[b]++;
        }
        u32map_add(&per_file_distinct, (uint32_t)seen.n, 1);
        u32map_free(&seen);
        ro3_free(&m);
    }

    printf("files                     %ld\n", nfiles);
    printf("polygons                  %.0f\n", n);
    printf("distinct values overall   %zu      <- an index would have many more\n", vals.n);
    printf("+0x08 == 0                %.0f  (%.2f%%)\n", zero, 100.0 * zero / (n < 1 ? 1 : n));
    printf("index readings, all of which FAIL:\n");
    printf("  in [0, npoints)         %.2f%%\n", 100.0 * in_pts / (n < 1 ? 1 : n));
    printf("  in [0, npolys)          %.2f%%\n", 100.0 * in_polys / (n < 1 ? 1 : n));
    printf("  in [0, nnormals)        %.2f%%\n", 100.0 * in_norms / (n < 1 ? 1 : n));
    printf("files by distinct-value count: ");
    ent = u32map_entries(&per_file_distinct, &nent);
    qsort(ent, nent, sizeof *ent, cmp_by_key);
    for (i = 0; i < nent && i < 10; i++)
        printf("%s%u->%ld", i ? ", " : "", ent[i].key, ent[i].count);
    printf("\n");
    free(ent);

    printf("\nbit positions ever set:\n");
    {
        int b;
        for (b = 0; b < 32; b++)
            if (bits[b] != 0)
                printf("   bit %2d  %8.0f  %6.2f%%\n", b, bits[b], 100.0 * bits[b] / n);
    }
    printf("\ncommonest values:\n");
    ent = u32map_entries(&vals, &nent);
    qsort(ent, nent, sizeof *ent, cmp_by_count);
    for (i = 0; i < nent && i < (size_t)top; i++)
        printf("   0x%08x  %ld\n", ent[i].key, ent[i].count);
    free(ent);

    u32map_free(&vals);
    u32map_free(&per_file_distinct);
    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* repeats                                                              */
/* ------------------------------------------------------------------ */

static int same_corner_list(const ro3_model *m, const ro3_poly *a, const ro3_poly *b)
{
    size_t c;
    if (a->corners != b->corners) return 0;
    for (c = 0; c < (size_t)a->corners; c++)
        if (m->corner_vert[a->first_corner + c] != m->corner_vert[b->first_corner + c])
            return 0;
    return 1;
}

static void lower_strip(const char *in, char *out, size_t n)
{
    size_t len, start = 0, end;
    while (in[start] && isspace((unsigned char)in[start])) start++;
    len = strlen(in + start);
    end = len;
    while (end > 0 && isspace((unsigned char)in[start + end - 1])) end--;
    if (end >= n) end = n - 1;
    {
        size_t i;
        for (i = 0; i < end; i++) out[i] = (char)tolower((unsigned char)in[start + i]);
        out[end] = 0;
    }
}

static int cmd_repeats(const char *root)
{
    corpus_list files;
    u32map pairs;
    smap by_surface;
    double tot = 0, rej = 0, dup_of_accepted = 0;
    long filesr = 0;
    size_t i;
    u32ent *ent;
    size_t nent;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    u32map_init(&pairs);
    memset(&by_surface, 0, sizeof by_surface);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256];
        size_t k, j;
        long nrej = 0;
        char *lp;

        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        tot += (double)m.npolys;
        for (k = 0; k < m.npolys; k++) {
            if (ro3_exporter_accepts(&m.polys[k])) continue;
            nrej++;
            rej++;
            u32map_add(&pairs, ((uint32_t)ro3_sel_a(&m.polys[k]) << 8) |
                               (uint32_t)ro3_sel_b(&m.polys[k]), 1);
            for (j = 0; j < m.npolys; j++)
                if (ro3_exporter_accepts(&m.polys[j]) &&
                    same_corner_list(&m, &m.polys[k], &m.polys[j])) {
                    dup_of_accepted++;
                    break;
                }
        }
        filesr += (nrej != 0);

        lp = nrej ? corpus_paired(files.path[i], ".lwo") : NULL;
        if (lp) {
            size_t len;
            void *buf = corpus_slurp(lp, &len);
            if (buf) {
                lwo_file lf;
                if (lwo_parse(buf, len, &lf) == 0 && lf.npolys == m.npolys) {
                    for (k = 0; k < m.npolys; k++) {
                        int s;
                        if (ro3_exporter_accepts(&m.polys[k])) continue;
                        s = lf.polys[k].surface;
                        if (s >= 1 && (size_t)s <= lf.nsrfs) {
                            char nm[256];
                            lower_strip(lf.srfs[s - 1], nm, sizeof nm);
                            smap_add(&by_surface, nm);
                        }
                    }
                }
                lwo_free(&lf);
                free(buf);
            }
            free(lp);
        }
        ro3_free(&m);
    }

    printf("polygons                                  %.0f\n", tot);
    printf("rejected by the exporter (`numrepeats`)   %.0f  (%.2f%%)\n",
           rej, 100.0 * rej / (tot < 1 ? 1 : tot));
    printf("files containing at least one             %ld\n", filesr);
    printf("rejected polygons whose corner list also appears on an accepted one: %.0f\n",
           dup_of_accepted);
    printf("\ncommonest (byte[rec+6], byte[rec+0x0b]) pairs:\n");
    ent = u32map_entries(&pairs, &nent);
    qsort(ent, nent, sizeof *ent, cmp_by_count);
    for (i = 0; i < nent && i < 12; i++)
        printf("   +04b2=%-4u +08b3=%-4u  %ld\n",
               (ent[i].key >> 8) & 0xFFu, ent[i].key & 0xFFu, ent[i].count);
    free(ent);

    printf("\nrejected polygons by .lwo surface name:\n");
    qsort(by_surface.e, by_surface.n, sizeof *by_surface.e, cmp_sent);
    for (i = 0; i < by_surface.n && i < 12; i++)
        printf("   %-20s %ld\n", by_surface.e[i].name, by_surface.e[i].count);

    smap_free(&by_surface);
    u32map_free(&pairs);
    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lwo                                                                  */
/* ------------------------------------------------------------------ */

static int cmd_lwo(const char *root)
{
    corpus_list files;
    long pairs = 0, poly_match = 0, idx_exact = 0;
    double ex_polys = 0, tot_polys = 0;
    char **mm_path;
    long *mm_a, *mm_b;
    size_t nmm = 0, i;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    mm_path = calloc(files.n + 1, sizeof *mm_path);
    mm_a = calloc(files.n + 1, sizeof *mm_a);
    mm_b = calloc(files.n + 1, sizeof *mm_b);

    for (i = 0; i < files.n; i++) {
        ro3_model m;
        lwo_file lf;
        char err[256];
        char *lp;
        void *buf;
        size_t len, k, same = 0;

        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        lp = corpus_paired(files.path[i], ".lwo");
        if (!lp) { ro3_free(&m); continue; }
        buf = corpus_slurp(lp, &len);
        free(lp);
        if (!buf) { ro3_free(&m); continue; }
        if (lwo_parse(buf, len, &lf) != 0) { free(buf); ro3_free(&m); continue; }
        free(buf);

        pairs++;
        if (lf.npolys != m.npolys) {
            mm_path[nmm] = strdup(files.path[i]);
            mm_a[nmm] = (long)m.npolys;
            mm_b[nmm] = (long)lf.npolys;
            nmm++;
            lwo_free(&lf);
            ro3_free(&m);
            continue;
        }
        poly_match++;
        for (k = 0; k < lf.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            size_t c;
            int eq = ((size_t)p->corners == (size_t)lf.polys[k].n);
            for (c = 0; eq && c < (size_t)p->corners; c++)
                if (m.corner_vert[p->first_corner + c] != (int32_t)lf.polys[k].idx[c]) eq = 0;
            if (eq) same++;
        }
        ex_polys  += (double)same;
        tot_polys += (double)lf.npolys;
        if (same == lf.npolys) idx_exact++;

        lwo_free(&lf);
        ro3_free(&m);
    }

    printf("pairs                         %ld\n", pairs);
    printf("polygon count agrees          %ld\n", poly_match);
    printf("every corner list identical   %ld files\n", idx_exact);
    printf("corner lists identical        %.0f of %.0f polygons  (%.2f%%)\n",
           ex_polys, tot_polys, 100.0 * ex_polys / (tot_polys < 1 ? 1 : tot_polys));
    printf("polygon-count disagreements:\n");
    for (i = 0; i < nmm; i++)
        printf("   %-52s ro3=%-6ld lwo=%ld\n", mm_path[i], mm_a[i], mm_b[i]);

    for (i = 0; i < nmm; i++) free(mm_path[i]);
    free(mm_path); free(mm_a); free(mm_b);
    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* uv                                                                   */
/* ------------------------------------------------------------------ */

static int cmd_uv(const char *root)
{
    corpus_list files;
    long long lo = 0, hi = 0;
    int have = 0;
    double n = 0;
    double over[4] = {0};
    static const double LIMS[4] = { 128.0, 256.0, 512.0, 1024.0 };
    size_t i;

    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    for (i = 0; i < files.n; i++) {
        ro3_model m;
        char err[256];
        size_t k, c;
        if (load_ro3(files.path[i], &m, err, sizeof err) != 0) continue;
        for (k = 0; k < m.npolys; k++) {
            ro3_poly *p = &m.polys[k];
            for (c = 0; c < (size_t)p->corners; c++) {
                double u = m.corner_u[p->first_corner + c];
                double v = m.corner_v[p->first_corner + c];
                int j;
                long long ru = (long long)llround(u * RO3_FIXED_ONE);
                long long rv = (long long)llround(v * RO3_FIXED_ONE);
                n += 2;
                if (!have) { lo = hi = ru; have = 1; }
                if (ru < lo) lo = ru;
                if (ru > hi) hi = ru;
                if (rv < lo) lo = rv;
                if (rv > hi) hi = rv;
                for (j = 0; j < 4; j++)
                    if (u > LIMS[j] || v > LIMS[j]) over[j] += 1;
            }
        }
        ro3_free(&m);
    }

    printf("texel values      %.0f\n", n);
    printf("raw 16.16 min     %lld\n", lo);
    printf("raw 16.16 max     %lld   (0x%06llX)\n", hi, hi);
    printf("   as texels      %.6f .. %.6f\n", lo / RO3_FIXED_ONE, hi / RO3_FIXED_ONE);
    printf("   2^24 - 1 =     %d      2^24 = %d\n", (1 << 24) - 1, 1 << 24);
    {
        int j;
        for (j = 0; j < 4; j++)
            printf("u or v > %-5d    %.0f corners\n", (int)LIMS[j], over[j]);
    }
    printf("\n");
    printf("The range is [0, 2^24), never reaching it - so u and v are 24-bit\n");
    printf("unsigned, which is exactly the width bld_uv_pack masks to, and the\n");
    printf("page is 256 texels with the top of it exclusive. A smaller page is\n");
    printf("ruled out by the counts above; a larger one would leave the top of\n");
    printf("the range unused in every one of the 429 files.\n");

    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* export - the whole corpus to .obj, MIRRORING the source tree.
 *
 * Not in ro3.py, and it exists because of a measurement. The corpus holds 429
 * .ro3 but only 361 distinct basenames: demo/FILES/A/OBJ0001.RO3 and
 * files/a/OBJ0001.RO3 are different models with the same name. A batch export
 * that names its output after the basename alone therefore writes 429 files
 * and leaves 361, silently overwriting 68 - and the two trees are exactly the
 * pair that says store their normals in different frames, so the models
 * lost are the ones a frame question would want to look at.
 *
 * Mirroring the tree is the fix; the per-file `obj` subcommand is unchanged.
*/

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, n;
    snprintf(tmp, sizeof tmp, "%s", path);
    n = strlen(tmp);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char save = tmp[i];
            tmp[i] = 0;
#if defined(_WIN32)
            /* MinGW's mkdir takes no mode (the Windows build). */
            if (mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
#endif
            tmp[i] = save;
        }
    }
    return 0;
}

static int cmd_export(const char *root, const char *outdir, double page, int include_all)
{
    corpus_list files;
    size_t i, rootlen = strlen(root), written = 0, failed = 0;

    if (!outdir) { fprintf(stderr, "export needs an output directory\n"); return 2; }
    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    while (rootlen && root[rootlen - 1] == '/') rootlen--;

    for (i = 0; i < files.n; i++) {
        char dst[1024], dir[1024];
        const char *rel = files.path[i];
        const char *dot;
        size_t l;

        if (strncmp(rel, root, rootlen) == 0 && rel[rootlen] == '/') rel += rootlen + 1;
        dot = strrchr(rel, '.');
        l = dot ? (size_t)(dot - rel) : strlen(rel);
        snprintf(dst, sizeof dst, "%s/%.*s.obj", outdir, (int)l, rel);
        dirname_of(dst, dir, sizeof dir);
        if (mkdir_p(dir) != 0) { fprintf(stderr, "cannot create %s\n", dir); failed++; continue; }
        if (cmd_obj(files.path[i], dst, page, include_all, 1) != 0) failed++;
        else written++;
    }
    printf("exported %zu of %zu models to %s (%zu failed)\n",
           written, files.n, outdir, failed);
    corpus_free(&files);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* files - the corpus in walk order. Not in ro3.py; it exists so that
 * `make check` can prove the two tools iterate the SAME files in the
 * SAME order before it compares anything that depends on the order.  */
/* ------------------------------------------------------------------ */

static int cmd_files(const char *root)
{
    corpus_list files;
    size_t i;
    if (corpus_find(root, ".ro3", &files) != 0) return 1;
    for (i = 0; i < files.n; i++) printf("%s\n", files.path[i]);
    corpus_free(&files);
    return 0;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "ro3 - reader, converter and corpus checker for Rally Championship .ro3 geometry\n\n"
        "  ro3 info      FILE [--polys N]\n"
        "  ro3 obj       FILE [-o OUT.obj] [--page N] [--all]\n"
        "  ro3 validate  [ROOT]\n"
        "  ro3 frames    [ROOT]\n"
        "  ro3 normals   [ROOT] [--seed N]\n"
        "  ro3 header08  [ROOT] [--top N]\n"
        "  ro3 repeats   [ROOT]\n"
        "  ro3 lwo       [ROOT]\n"
        "  ro3 uv        [ROOT]\n"
        "  ro3 files     [ROOT]\n");
}

int main(int argc, char **argv)
{
    const char *cmd, *pos = NULL, *out = NULL;
    int i, npolys = 0, top = 16, include_all = 0;
    uint32_t seed = 1;
    double page = RO3_TEXTURE_PAGE;

    if (argc < 2) { usage(); return 2; }
    cmd = argv[1];

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--polys") == 0 && i + 1 < argc) npolys = atoi(argv[++i]);
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) top = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) page = atof(argv[++i]);
        else if (strcmp(argv[i], "--all") == 0) include_all = 1;
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-') { usage(); return 2; }
        else pos = argv[i];
    }

    if (strcmp(cmd, "info") == 0) {
        if (!pos) { usage(); return 2; }
        return cmd_info(pos, npolys);
    }
    if (strcmp(cmd, "obj") == 0) {
        if (!pos) { usage(); return 2; }
        return cmd_obj(pos, out, page, include_all, 0);
    }
    if (!pos) pos = DEFAULT_ROOT;
    if (strcmp(cmd, "export") == 0) return cmd_export(pos, out, page, include_all);
    if (strcmp(cmd, "validate") == 0) return cmd_validate(pos);
    if (strcmp(cmd, "frames")   == 0) return cmd_frames(pos);
    if (strcmp(cmd, "normals")  == 0) return cmd_normals(pos, seed);
    if (strcmp(cmd, "header08") == 0) return cmd_header08(pos, top);
    if (strcmp(cmd, "repeats")  == 0) return cmd_repeats(pos);
    if (strcmp(cmd, "lwo")      == 0) return cmd_lwo(pos);
    if (strcmp(cmd, "uv")       == 0) return cmd_uv(pos);
    if (strcmp(cmd, "files")    == 0) return cmd_files(pos);
    usage();
    return 2;
}
