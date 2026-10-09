/*
 * vnc_diff.h -- which rows of the screen have moved since this client was last
 * sent it.
 *
 * WHY THIS EXISTS, and it is not an optimisation for its own sake. rbp's screen
 * changes in a handful of widely separated places at once -- the waveform scrolls,
 * a time readout ticks, the drawers repaint -- so a whole-frame rectangle is mostly
 * a rectangle full of pixels that did not change. Measured on the unit against the
 * live screen (2026-10-09), a 300 ms interval moved 5.7 % of the pixels, spread
 * across SIX bands covering 29-37 % of the frame:
 *
 *     bands: 52-70  81-235  246-264  605-626  690-704  718-767
 *
 * Sent whole, that frame costs the client's 32-bpp format 112 KB compressed and
 * 101 ms of the Pi's CPU (55 of them converting, 46 deflating). Sent as those six
 * bands it costs 59-69 KB and 14-24 ms. Both numbers matter: this is a live audio
 * player and the second one is what keeps a viewer from being audible.
 *
 * Pure: no socket, no /dev, no allocation. test_vnc_diff.c pins the band rules.
 */
#ifndef RBPI4B_VNC_DIFF_H
#define RBPI4B_VNC_DIFF_H

#include <stddef.h>
#include <stdint.h>

/* A run of rows that must be resent, HALF-OPEN: rows y0 .. y1-1. */
struct vnc_band {
    int y0, y1;
};

/* A run of at most this many clean rows is absorbed into the band around it. It is
 * worth having because the gaps between rbp's own regions are small and a band
 * boundary costs a whole rectangle header plus, in Tight, a zlib block boundary.
 * The measured frame has six regions whether the tolerance is 0 or 8. */
#define VNC_BAND_GAP 4

/* More bands than this and the caller should send one whole-frame rectangle
 * instead: past a point the per-rectangle overhead is what is being paid for. */
#define VNC_BAND_MAX 24

/* Compare an RGB565 frame against the copy last sent to this client and describe
 * what moved. `a` and `b` are both w*h pixels.
 *
 * Returns the number of bands, which may EXCEED `max` -- in that case only the
 * first `max` are written and the caller must treat the frame as wholly changed.
 * Saying so is the point: silently truncating the list would leave a band of the
 * screen that is never resent, and a client looking at a picture that is correct
 * everywhere except one stripe is a much worse failure than a slow update.
 *
 * `dirty`, if not NULL, receives the number of changed pixels. */
int vnc_diff_bands(const uint16_t *a, const uint16_t *b, int w, int h,
                   int gap, struct vnc_band *out, int max, long *dirty);

/* Has anything at all moved? One memcmp per row, which is what makes the idle case
 * cost nothing: a paused deck re-diffs in about a millisecond and sends nothing. */
int vnc_diff_same(const uint16_t *a, const uint16_t *b, int w, int h);

#endif /* RBPI4B_VNC_DIFF_H */
