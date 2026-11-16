/* platecache.c - see platecache.h: moved here from
 * tests/stage_render_probe.c's plate_get(), unchanged in behaviour. */

#include "platecache.h"

#include <stdlib.h>
#include <string.h>

/* In the PLATE's own order - B,G,R,A in memory, i.e. 0xAARRGGBB - because it
 * is written before plate_upload does its one swizzle.  Opaque red. */
#define PLATE_FLAG_COLOUR 0xffff0000u

int plate_cache_init(plate_cache *pc)
{
    memset(pc, 0, sizeof *pc);
    pc->flag = -1;
    pc->pl = (plate *)malloc(sizeof *pc->pl);
    return pc->pl ? 0 : -1;
}

void plate_cache_free(plate_cache *pc)
{
    free(pc->pl);
    pc->pl = NULL;
}

int plate_cache_get(plate_cache *pc, const char *files_root, unsigned id,
                    const plate_grade *g, plat_tex *out,
                    char *err, size_t errlen)
{
    unsigned j;

    for (j = 0; j < pc->n; j++) {
        if (pc->id[j] == id) { *out = pc->tex[j]; return 0; }
    }
    if (plate_load(pc->pl, files_root, id, 1, g, err, errlen) != 0) {
        return 1;
    }
    if (pc->flag >= 0 && (unsigned long)pc->flag == (unsigned long)id) {
        /* The WHOLE staging buffer, so every mip level the chain built is
         * flagged too - a minified plate that still showed its own colours
         * would make the test answer a narrower question than it claims. */
        size_t k;
        for (k = 0; k < PLATE_MAX_PIXELS * 2u; k++) {
            pc->pl->pixels[k] = PLATE_FLAG_COLOUR;
        }
    }
    *out = plate_upload(pc->pl);
    /* plate_load REPORTS the fallback.  Sniffing for it in the pixels - "is
     * it 256x256 and white at both ends?" - cannot tell the fallback from a
     * genuinely white plate, which is standing rule 1 in miniature. */
    if (pc->pl->fallback == PLATE_FALLBACK_NONE)          { pc->decoded++; }
    else if (pc->pl->fallback == PLATE_FALLBACK_BAD_SIZE) { pc->badsize++; }
    else                                                  { pc->white++; }
    if (pc->n < PLATE_CACHE_MAX) {
        pc->id[pc->n]  = id;
        pc->tex[pc->n] = *out;
        pc->n++;
    }
    return 0;
}
