/* lwo.h - the minimum of LightWave Object needed to use the .lwo files beside
 * the .ro3 as an ANSWER KEY.
 *
 * This is not a general LWO reader and should not become one. The build tool
 * wrote these files from the same data it wrote the .ro3 from, so PNTS, POLS
 * and SRFS are ground truth for the reconstruction - see
 *
 *
 * IFF/EA-85: big-endian, chunks padded to even length.
 *
 * Buffer-taking, like ro3.h: nothing here opens a file.
*/
#ifndef RAL_LWO_H
#define RAL_LWO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t *idx;     /* vertex indices */
    int       n;       /* how many */
    int       surface; /* 1-based into the SRFS name list */
} lwo_poly;

typedef struct {
    lwo_poly *polys;   size_t npolys;
    uint16_t *idx_pool;                 /* backing store for every polys[i].idx */
    char    **srfs;    size_t nsrfs;    /* surface names, in SRFS order */
    char     *srf_pool;
    size_t    npnts;                    /* PNTS count, for a sanity check */

    /* Builder state. A file may carry more than one POLS chunk, and the
     * polygons of the second continue the numbering of the first, so these
     * have to outlive a single chunk. */
    size_t _polycap, _poolused, _poolcap, _srfcap;
} lwo_file;

/* Returns 0 on success. Not a FORM/LWOB, or a truncated chunk, is a failure. */
int  lwo_parse(const void *buf, size_t len, lwo_file *out);
void lwo_free(lwo_file *f);

#endif /* RAL_LWO_H */
