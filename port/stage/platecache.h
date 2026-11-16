/* platecache.h - plates cached by ID, shared by the stage and the object
 * layer. lifted this out of tests/stage_render_probe.c, where
 * had written it, so that the rally executable loads plates the same way the
 * probe does rather than through a second copy.
 *
 * Plates are cached by ID, not by the thing that names one.  Sixteen texinfo
 * slots share one plate (record >> 4), and added a SECOND namer: an
 * object polygon's +0x04 low word is a plate id in the same band ladder, and
 * an id may be named by both a level's texture slots and a .ro3.  A per-namer
 * load would decode the same file twice.  That is the residency cache at
 * 0x0058e6b0 in miniature. */
#ifndef RAL_PLATECACHE_H
#define RAL_PLATECACHE_H

#include "plate.h"

#define PLATE_CACHE_MAX 1024

typedef struct {
    plate        *pl;                    /* ~1.3 MB staging; heap            */
    unsigned      id[PLATE_CACHE_MAX];
    plat_tex      tex[PLATE_CACHE_MAX];
    unsigned      n;
    /* What each MISS turned out to be.  A hit counts nothing: the plate was
     * counted when it was first decoded. */
    unsigned long decoded, white, badsize;
    /* DIAGNOSTIC.  >= 0 replaces that plate's pixels with flat opaque
     * red at every mip level, AFTER it has been decoded, keyed and graded -
     * the answer to "does this plate reach a pixel?".  -1 = off, which is the
     * original's behaviour; stage_render_probe's RALLY_PLATE_FLAG sets it. */
    long          flag;
} plate_cache;

/* 0, or -1 if the staging buffer cannot be allocated. */
int  plate_cache_init(plate_cache *pc);
void plate_cache_free(plate_cache *pc);

/* Returns 0 and the texture.  Non-zero only when a file is PRESENT and will
 * not decode, which is a defect rather than a skip - a missing file is the
 * white fallback and plate_load reports it (counted in `white`). */
int  plate_cache_get(plate_cache *pc, const char *files_root, unsigned id,
                     const plate_grade *g, plat_tex *out,
                     char *err, size_t errlen);

#endif
