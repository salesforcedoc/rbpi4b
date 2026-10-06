/*
 * prompt_paint.h -- the USB STOP chooser's image.
 *
 * The fourth drawing module after menu_paint.c, menu_window_paint.c and
 * side_paint.c, and written to the same rule: pure, a function of its arguments and
 * of nothing else, in the same palette (menu_pixel(), which is public for exactly
 * this reason). "Painting it twice paints it once" is load-bearing here as it is
 * there -- the box is presented on a plane whose buffer is single-buffered, so a
 * paint that is not idempotent is a paint that tears. Every pixel of the box is
 * written on every call.
 *
 * THE VIEW'S PICTURE RECT IS THE BOX. That is the one thing that differs from
 * side_paint.c, and it is what makes the arithmetic simple: the caller sets `dw`/`dh`
 * to the box's own size in framebuffer pixels -- `PR_W`/`PR_H` logical px at the page's
 * scale -- and `bx`/`by` to where that lands. Logical box coordinates are then scaled
 * by `dw/PR_W` and `dh/PR_H`, so the same code draws the box on its own plane and on
 * the page, and the plane is created at exactly the size the picture says.
 */
#ifndef RBPI4B_PROMPT_PAINT_H
#define RBPI4B_PROMPT_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "prompt_zone.h"     /* PR_* geometry, struct prompt_state, PROMPT_ROWS */

/* Is this view something prompt_paint() can draw into? The box has a title, a rule
 * and two rows of PR_ROW_H logical px, each of which has to host the font's line box
 * (menu_font.h's MENU_FONT_LINE) after scaling, or the labels collide with the frames
 * -- and each of the four buttons has to be wide enough to hold its own label WHOLE,
 * which is a second, independent refusal because a button is half a row wide now and
 * the narrowest page this shim supports scales the box down with it. A view that
 * fails either is refused here rather than drawn as a smear -- menu_paint.h's
 * menu_view_ok() answers the same two questions for the band (menu_labels_fit()),
 * and side_paint.h's side_paint_ok() for the drawer. */
int prompt_paint_ok(const struct menu_view *v);

/* The box's size in framebuffer pixels for a page of dw x dh -- the size the plane
 * has to be set up at, and the one prompt_view_for() below is built with. The
 * caller asks this rather than computing it twice. */
int prompt_paint_w(int page_dw);
int prompt_paint_h(int page_dh);

/* Draw the box into `v`.
 *
 *   S             rbp's answer about the two devices (prompt_zone.h); a cell with
 *                 nothing to do is drawn in the palette's two OFF classes.
 *   pressed_cell  the cell under the finger, 1..PROMPT_CELLS, or 0 -- prompt_pressed(),
 *                 which is menu_pressed()'s question asked of this module.
 *   selected_cell the cell the operator has armed, or 0 -- prompt_selected(), which is
 *                 the one question this box has that the band does not.
 *
 * A pressed cell is never an off cell: prompt_zone.c answers a dead cell with
 * PR_ACT_NONE but it still highlights it while the finger is on it, because a finger
 * that gets no feedback at all reads as a dead panel rather than as a refused button.
 * The highlight here is therefore the ordinary pressed pair for every cell.
 *
 * AN ARMED CELL WEARS THE SAME PRESSED PAIR, deliberately. The palette has no third
 * button state, and "lit up like the one you are touching" is the clearest available
 * way to say "this is the one OK would stop". The two coincide while the arming tap
 * is still down and are otherwise on different cells, so the reading is never
 * ambiguous where it matters -- on the device buttons -- and the answers below them
 * are never armed at all.
 *
 * Every pixel of the view is written. Does nothing when prompt_paint_ok() is 0. */
void prompt_paint(const struct menu_view *v, const struct prompt_state *S,
                  int pressed_cell, int selected_cell);

#endif /* RBPI4B_PROMPT_PAINT_H */
