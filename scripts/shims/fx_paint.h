/*
 * fx_paint.h -- the effect picker's image: rbp's own BEAT FX plate, redrawn over itself,
 * holding fourteen of his cells.
 *
 * The fifth drawing module after menu_paint.c, menu_window_paint.c, side_paint.c and
 * prompt_paint.c, and written to the same rule: pure, a function of its arguments and
 * of nothing else, in the same palette (menu_pixel(), which is public for exactly this
 * reason -- and which carries the two of rbp's own colours this one borrows). "Painting
 * it twice paints it once" is load-bearing here as it is there --
 * the box is presented on a plane whose buffer is single-buffered, so a paint that is
 * not idempotent is a paint that tears. Every pixel of the box is written on every
 * call.
 *
 * THE VIEW'S PICTURE RECT IS THE BOX, the contract prompt_paint.h states and for the
 * same reason: the caller sets `dw`/`dh` to the box's own size in framebuffer pixels --
 * FX_W/FX_H logical px at the page's scale -- and `bx`/`by` to where that lands.
 * Logical box coordinates are then scaled by dw/FX_W and dh/FX_H, so the same code
 * draws the box on its own plane and on the page, and the plane is created at exactly
 * the size the picture says.
 */
#ifndef RBPI4B_FX_PAINT_H
#define RBPI4B_FX_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "fx_zone.h"         /* FX_* geometry, FXLIST_ROWS */

/* Is this view something fxlist_paint() can draw into? The box has fourteen rows of
 * FX_ROW_H logical px, each of which has to host the font's line box (menu_font.h's
 * MENU_FONT_LINE) after scaling, or a label runs into the row above or below it -- and
 * each row has to be wide enough to hold its own label WHOLE, which is a second,
 * independent refusal because the narrowest page this shim supports scales the box down
 * with it. A view that fails either is refused here rather than drawn as a smear --
 * menu_paint.h's menu_view_ok() asks the same two questions for the band
 * (menu_labels_fit()) and prompt_paint.h's prompt_paint_ok() for the USB chooser. */
int fxlist_paint_ok(const struct menu_view *v);

/* The box's size in framebuffer pixels for a page of dw x dh -- the size the plane has
 * to be set up at. The caller asks this rather than computing it twice. */
int fxlist_paint_w(int page_dw);
int fxlist_paint_h(int page_dh);

/* Draw the box into `v`.
 *
 *   pressed_row  the row under the finger, 1..FXLIST_ROWS, or 0 -- fxlist_pressed(),
 *                which is menu_pressed()'s question asked of this module.
 *
 * THERE IS NO OTHER STATE, and that is the whole difference from the USB chooser: a row
 * is the answer, so there is nothing armed, nothing dead and nothing to light but the
 * finger. The header of fx_zone.h records why the current effect is not marked either
 * (rbp exposes no readable type word).
 *
 * Every pixel of the view is written. Does nothing when fxlist_paint_ok() is 0. */
void fxlist_paint(const struct menu_view *v, int pressed_row);

#endif /* RBPI4B_FX_PAINT_H */
