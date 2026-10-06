/*
 * side_paint.h -- the edge drawers' image.
 *
 * The second drawing module after menu_window_paint.c, and written to the same
 * rule: pure, a function of its arguments and of nothing else, over the caller's
 * `struct menu_view`, in the same palette the band uses (menu_pixel()). "Painting
 * the drawer twice paints it once" is load-bearing here and not a nicety -- the
 * drawer is presented on a SINGLE-BUFFERED plane, so a paint that is not idempotent
 * is a paint that tears. Every pixel of the panel rectangle is written on every
 * call, which is what makes that true by construction.
 *
 * COORDINATES ARE PANEL-LOCAL AND THE MIRROR IS APPLIED HERE. side_zone.h works in
 * local x (0 = the outer edge); the view's buffer is in SCREEN order, because the
 * plane is placed at the panel's screen rectangle. `side` is what turns one into the
 * other, and it is the only reason this function takes it.
 */
#ifndef RBLIVE4_SIDE_PAINT_H
#define RBLIVE4_SIDE_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "side_zone.h"       /* SZ_* geometry, side_local_x() */

/* NO FRAME IS DRAWN AROUND THE PANEL -- *"you don't need a white border on the side
 * swipe menus"* (2026-10-06) -- so this is no longer a border width and is kept only
 * as the margin side_paint_ok() asks the view to have. It is the number the band's own
 * border was (menu_paint.c's MENU_BORDER_PX, not exported), and the band's frame is
 * gone for the same reason, so the two still read as one family. Do not delete it
 * without moving the guard: the guard is what refuses a panel too small to draw. */
#define SP_BORDER    2

/* The fader's handle, in logical rows. It is wider than the track and taller than it
 * is wide -- the shape a fader cap reads as -- and it is CLAMPED inside the track's
 * own ends so it never hangs out of the well at either extreme. */
#define SP_HANDLE_H  34

/* THE NUDGE MARKS: half-length and weight, in 1:1 DEVICE pixels like the glyphs
 * beside them. They are drawn as bars and not set in the font because the atlas has
 * no '+' at all (side_paint.c), so the two cells have to match by construction -- and
 * they are the tightest thing the panel has to host, which is why side_paint_ok()
 * measures a nudge cell against them. */
#define SP_SIGN_ARM    12
#define SP_SIGN_THICK  4

/* Is this view something side_paint() can draw into? menu_paint.h's menu_view_ok()
 * answers the same question for the BAND and refuses a panel whose band cannot host
 * the font's line box; this is that check for the drawer's own rectangles, so a
 * 480x320 panel is refused here rather than drawn as a smear. */
int side_paint_ok(const struct menu_view *v);

/* Draw the drawer `side` (SZ_LEFT/SZ_RIGHT) into `v`, whose buffer is the panel's
 * screen rectangle.
 *
 *   pressed_hit  the control under the finger, SZ_HIT_* -- from side_pressed(side);
 *                SZ_HIT_NONE when nothing is down.
 *   fader_v      the channel fader's position, 0..1023, top = full. The caller
 *                supplies it because the drawer has no position of its own when it
 *                is not being dragged: it is the same value rbp's mixer is being
 *                sent, so what is drawn is what the channel is doing.
 *
 * Every pixel of the view is written. Does nothing when side_paint_ok() is 0. */
void side_paint(const struct menu_view *v, int side, int pressed_hit, int fader_v);

#endif /* RBLIVE4_SIDE_PAINT_H */
