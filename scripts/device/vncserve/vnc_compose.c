/*
 * vnc_compose.c -- see vnc_compose.h for why this exists.
 */
#include "vnc_compose.h"

#include <string.h>

void vnc_compose_blit(uint16_t *dst, int dst_w, int dst_h, const struct vnc_plane *p)
{
    int src_x0, src_y0, copy_x0, copy_y0, copy_w, copy_h, row;

    if (!p->src || p->w <= 0 || p->h <= 0)
        return;

    /* Clip against all four edges. A drawer mid-swipe spends most of its life
     * half off-screen, so this is the ordinary path, not a corner case. */
    src_x0 = p->dst_x < 0 ? -p->dst_x : 0;
    src_y0 = p->dst_y < 0 ? -p->dst_y : 0;
    copy_x0 = p->dst_x > 0 ? p->dst_x : 0;
    copy_y0 = p->dst_y > 0 ? p->dst_y : 0;

    copy_w = p->w - src_x0;
    if (copy_w > dst_w - copy_x0) copy_w = dst_w - copy_x0;
    copy_h = p->h - src_y0;
    if (copy_h > dst_h - copy_y0) copy_h = dst_h - copy_y0;

    if (copy_w <= 0 || copy_h <= 0)
        return;

    for (row = 0; row < copy_h; row++) {
        const uint16_t *s = p->src + (size_t)(src_y0 + row) * (size_t)p->src_stride_px + src_x0;
        uint16_t *d = dst + (size_t)(copy_y0 + row) * (size_t)dst_w + copy_x0;
        memcpy(d, s, (size_t)copy_w * sizeof *d);
    }
}

void vnc_compose_planes(uint16_t *dst, int dst_w, int dst_h,
                        struct vnc_plane *planes, int n)
{
    int i, j;

    /* Insertion sort by zpos. n is the number of active planes (tens at most, and
     * usually two or three), so an O(n^2) sort over a linked list of DRM objects
     * would cost more in complexity than it saves in time. */
    for (i = 1; i < n; i++) {
        struct vnc_plane key = planes[i];
        for (j = i - 1; j >= 0 && planes[j].zpos > key.zpos; j--)
            planes[j + 1] = planes[j];
        planes[j + 1] = key;
    }

    for (i = 0; i < n; i++)
        vnc_compose_blit(dst, dst_w, dst_h, &planes[i]);
}
