/*
 * point_xform.h — raw device coordinates -> the logical 1280x800 space rbp's UI
 * lives in.
 *
 * This is deliberately a pure function of its inputs: no env, no I/O, no state.
 * The axis algebra is the one part of the pointer path that cannot be settled by
 * reading code — the repo's two previous shim generations disagree about whether
 * logical_x comes from raw Y, and about which way round it runs — so it has to be
 * *measured* on the target with POINT_DEBUG=1, and the flags below are what turn
 * that measurement into configuration instead of a rebuild.
 *
 * The math is a strict generalization of the SC Live 4's:
 *
 *     px = rx * 800  / 2048;   py = ry * 1280 / 2048;
 *     lx = py;                 ly = px;
 *
 * which this reproduces exactly with swap=1, raw_min=0, raw_max=2047 (2048 is
 * what `max - min + 1` evaluates to, and the two dimensions cross over because
 * that panel was portrait while rbp's UI is 1280x800).
 */
#ifndef RBLIVE4_POINT_XFORM_H
#define RBLIVE4_POINT_XFORM_H

/* The logical space. rbp renders 1280x800 landscape; the shim feeds it logical
 * coordinates and the display layer (DirectFB/the fbdev present path) is what
 * deals with the panel's real geometry. Nothing here should learn about the
 * physical mode. */
#define POINT_LOGICAL_W 1280
#define POINT_LOGICAL_H 800

struct point_xform {
    /* Take logical_x from the device's Y axis (and logical_y from X). The SC
     * Live 4's portrait panel needed this; a monitor or a USB mouse does not.
     * From POINT_SWAP_XY. */
    int swap_xy;

    /* Mirror each logical axis. From POINT_INVERT_X / POINT_INVERT_Y. */
    int invert_x;
    int invert_y;

    /* The device's reported range, from EVIOCGABS, or the defaults below. The
     * range is inclusive on both ends: the divisor is (max - min + 1), which is
     * the count of distinguishable positions and what makes a 0..2047 device
     * divide by 2048 exactly as the old hardcoded math did. */
    int raw_min_x, raw_max_x;
    int raw_min_y, raw_max_y;
};

/* Defaults for a device that reports nothing usable — the SC Live 4 touch
 * controller's own range, which is also the right shape for most resistive
 * panels (0..2047 is the tsc2007's 11-bit scale). */
#define POINT_RAW_MIN_DEFAULT 0
#define POINT_RAW_MAX_DEFAULT 2047

/* Fill in the defaults, then apply the environment's POINT_* overrides. Safe to
 * call before the device is known: pointsrc replaces the ranges with whatever
 * EVIOCGABS reports as soon as it has a device. */
void point_xform_init(struct point_xform *x);

/* Absolute device position -> logical. Values outside the logical space are
 * clamped, which is what the SC Live 4 code did (and what keeps a noisy panel
 * from sending rbp an off-screen coordinate). */
void point_xform_abs(const struct point_xform *x, int raw_x, int raw_y,
                     int *logical_x, int *logical_y);

/* Relative device delta -> the next absolute pointer position, clamped.
 * `cursor_x`/`cursor_y` are both the input (current) and the output (new)
 * position. Scale is POINT_MOUSE_SPEED, which exists because EVDEV reports
 * counts, not pixels, and a 1:1 mapping on a 1280-wide logical space makes a
 * physical mouse crawl. */
void point_xform_rel(const struct point_xform *x, int dx, int dy,
                     int *cursor_x, int *cursor_y);

#endif /* RBLIVE4_POINT_XFORM_H */
