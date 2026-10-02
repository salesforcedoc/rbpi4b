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

/* Where a logical_w x logical_h surface lands in a fb_w x fb_h framebuffer.
 * Aspect-fit unless `stretch`, then both axes are filled; either way the
 * rectangle is centred.  Its size comes back in *dw and *dh, its top-left in
 * *bx and *by.  (Spelled out rather than as a slash-joined pair, because two
 * names around a slash open a nested comment and gcc says so.)
 *
 * THIS IS A SECOND COPY OF ONE RULE.  The original is fbdev_present_fit() in
 * work/dfb-src/systems/fbdev/fbdev.c, which decides where the DirectFB present
 * path blits the UI; this one decides where the cursor draws on top of it.  They
 * are in different builds -- the driver is cross-built for the target, this is
 * part of an LD_PRELOAD shim -- and cannot share a header, so *a change to one
 * is a change to both*.  A drift is not silent: the driver logs its rectangle in
 * the one-shot PRESENT: line (/tmp/dfbdig9.log) and fb_cursor.c logs this one,
 * so the two reports disagreeing on the unit is the tell.
 *
 * The arithmetic is 16.16.  The scale factor is FLOORED, which is what bounds
 * the rectangle: s <= (fb_w << 16) / logical_w and s <= (fb_h << 16) / logical_h,
 * so logical_w * s <= fb_w << 16 and logical_h * s <= fb_h << 16 hold, and
 * *dw <= fb_w / *dh <= fb_h hold for any final rounding that adds less than 65536.
 * The final shift therefore ROUNDS TO NEAREST (+32768) rather than down: the
 * step is already floored, so flooring again loses a whole row and column on
 * every inexact fit (1280x800 into 1280x720 gives 1151x719 floored, 1152x720
 * rounded).  On a fb that matches the logical size this returns dw==fb_w,
 * dh==fb_h, bx==by==0, so the caller's mapping is the identity it has always
 * been.
 *
 * Pure: no env, no I/O, no state -- same rule as the rest of this header, and
 * the reason it lives here is that test_point links this file directly. */
void point_fit(int logical_w, int logical_h, int fb_w, int fb_h, int stretch,
               int *dw, int *dh, int *bx, int *by);

#endif /* RBLIVE4_POINT_XFORM_H */
