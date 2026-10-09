/*
 * vnc_compose.h -- put the overlay planes on top of the framebuffer.
 *
 * THE GAP THIS CLOSES. rbp paints the deck into /dev/fb0, but it paints everything
 * that is not the deck -- the two edge drawers, the top band, the USB-STOP chooser,
 * the fx picker -- onto vc4 DRM overlay planes on /dev/dri/card1. A screenshot built
 * from /dev/fb0 alone is a screen with no drawers and no band: docs/06-display.md
 * (the section on the band) records that a plane appears in fb0 as "nothing at all".
 * So a viewer that only reads fb0 is not showing the operator's screen, it is showing
 * most of it.
 *
 * Pure, on purpose: this module takes buffers and geometry and knows nothing about
 * DRM, ioctls or file descriptors -- all of that is vnc_capture.c's job. That split is
 * what lets the clipping and the z-ordering be tested on the Mac (test_vnc_compose.c)
 * instead of guessed at on the glass.
 *
 * COMPOSITING IS A COPY, NOT A BLEND. Every plane this port creates is RGB565 with no
 * alpha channel (RG16; see scripts/shims/drmband.h), so "on top" means "overwrite".
 */
#ifndef RBPI4B_VNC_COMPOSE_H
#define RBPI4B_VNC_COMPOSE_H

#include <stdint.h>

/* One plane, already resolved to pixels by the caller: where it sits on the screen,
 * how big it is, and where its own pixels are. */
struct vnc_plane {
    int dst_x, dst_y;         /* the plane's top-left, in screen pixels; may be off-screen */
    int w, h;                 /* the plane's dimensions, in pixels */
    const uint16_t *src;      /* RGB565, row-major, at least h rows of src_stride_px */
    int src_stride_px;        /* pixels per row in `src` (the fb's pitch / 2) */
    int zpos;                 /* stack order: LOWER IS PAINTED FIRST, so higher wins */
};

/* Copy one plane into an RGB565 frame, clipping to the frame's edges.
 *
 * Out-of-range destination rectangles are ordinary here, not an error: a drawer
 * slides in from off the edge, so most of a swipe has half the plane at a negative
 * x. Clipping is therefore the common case and is not logged. */
void vnc_compose_blit(uint16_t *dst, int dst_w, int dst_h, const struct vnc_plane *p);

/* Blit every plane, in ascending zpos, so that where two overlap the higher one wins.
 * `planes` is reordered in place (a straight insertion sort -- there are tens of
 * planes, not thousands, and this is called once a frame). */
void vnc_compose_planes(uint16_t *dst, int dst_w, int dst_h,
                        struct vnc_plane *planes, int n);

#endif /* RBPI4B_VNC_COMPOSE_H */
