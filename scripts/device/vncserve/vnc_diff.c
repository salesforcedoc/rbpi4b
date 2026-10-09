/*
 * vnc_diff.c -- the row scan. Pure; see vnc_diff.h.
 */
#include "vnc_diff.h"

#include <string.h>

int vnc_diff_same(const uint16_t *a, const uint16_t *b, int w, int h)
{
    size_t bytes = (size_t)w * 2;
    int y;

    for (y = 0; y < h; y++)
        if (memcmp(a + (size_t)y * w, b + (size_t)y * w, bytes) != 0)
            return 0;
    return 1;
}

int vnc_diff_bands(const uint16_t *a, const uint16_t *b, int w, int h,
                   int gap, struct vnc_band *out, int max, long *dirty)
{
    size_t bytes = (size_t)w * 2;
    long changed = 0;
    int n = 0, cur = -1, clean = 0, y;

    for (y = 0; y < h; y++) {
        const uint16_t *ra = a + (size_t)y * w;
        const uint16_t *rb = b + (size_t)y * w;
        int row_moved = memcmp(ra, rb, bytes) != 0;

        if (row_moved && dirty) {
            int x;
            for (x = 0; x < w; x++)
                if (ra[x] != rb[x])
                    changed++;
        }

        if (row_moved) {
            if (cur < 0) cur = y;
            clean = 0;
        } else if (cur >= 0 && ++clean > gap) {
            /* The band ended `clean` rows ago; those clean rows stay outside it. */
            if (n < max) {
                out[n].y0 = cur;
                out[n].y1 = y - clean + 1;
            }
            n++;
            cur = -1;
            clean = 0;
        }
    }
    if (cur >= 0) {
        /* THE LAST BAND IS NOT AUTOMATICALLY THE REST OF THE FRAME. Closing it at `h`
         * is the tempting one-liner and it resends every clean row below the last
         * change -- which on a screen whose bottom region is usually static is most of
         * the height. `clean` counts the trailing run of unmoved rows, and they stay
         * outside. */
        int end = h - clean;
        if (end <= cur)
            end = h;
        if (n < max) {
            out[n].y0 = cur;
            out[n].y1 = end;
        }
        n++;
    }

    if (dirty) *dirty = changed;
    return n;
}
