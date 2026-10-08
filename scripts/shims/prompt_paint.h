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
 * and one row of PR_ROW_H logical px, which has to host the font's line box
 * (menu_font.h's MENU_FONT_LINE) after scaling, or the labels collide with the frames
 * -- and each of the two buttons has to be wide enough to hold ITS CELL'S OWN LABEL
 * WHOLE, which is a second, independent refusal because a button is only half the row
 * wide and the narrowest page this shim supports scales the box down with it. A view that
 * fails either is refused here rather than drawn as a smear -- menu_paint.h's
 * menu_view_ok() answers the same two questions for the band (menu_labels_fit()),
 * and side_paint.h's side_paint_ok() for the drawer.
 *
 * THE WIDTH REFUSAL IS ABOUT THE CELL, NOT ABOUT THE NAME. A button reads
 * "HOLD <the device's own name>" when its stick has one (prompt_zone.h), and a name is
 * both longer than the cell's default label and the operator's to choose -- so it is CUT
 * TO FIT at paint time rather than refused. What is refused here is a cell too narrow for
 * a button to be legible on at all, measured against the label every cell is guaranteed
 * to have. A state-dependent refusal would be worse than useless on this route: the
 * caller asks this once and paints only if it answers yes, so a long name would take the
 * whole box off the glass. */
int prompt_paint_ok(const struct menu_view *v);

/* The box's size in framebuffer pixels for a page of dw x dh -- the size the plane
 * has to be set up at, and the one prompt_view_for() below is built with. The
 * caller asks this rather than computing it twice. */
int prompt_paint_w(int page_dw);
int prompt_paint_h(int page_dh);

/* Draw the box into `v`.
 *
 *   S             rbp's answer about the two devices AND what the host calls them
 *                 (prompt_zone.h): a cell whose device rbp reports absent is drawn in
 *                 the palette's two OFF classes, and a cell whose device has a name of
 *                 its own wears that name, cut to fit the button (prompt_text_clip()).
 *   pressed_cell  the cell under the finger, 1..PROMPT_CELLS, or 0 -- prompt_pressed(),
 *                 which is menu_pressed()'s question asked of this module.
 *   flash_cell    the lit half of a running hold's blink, or 0 -- prompt_hold_cell() when
 *                 prompt_hold_flash() is 1. It wears the SAME pressed pair, which is the
 *                 whole of the flash: the button alternates between that pair and its
 *                 normal live face every PR_HOLD_FLASH_MS until the hold fires.
 *
 * A pressed cell is never an off cell: prompt_zone.c starts no hold on a dim button but
 * a finger on one still highlights it, because a finger that gets no feedback at all
 * reads as a dead panel rather than as a refused button. The highlight here is
 * therefore the ordinary pressed pair for every cell, and a cell is drawn pressed when
 * it is the finger's OR the flash's -- the two are never the same cell, which is what
 * gives the blink somewhere to blink to (prompt_zone.c's prompt_pressed()).
 *
 * Every pixel of the view is written. Does nothing when prompt_paint_ok() is 0. */
void prompt_paint(const struct menu_view *v, const struct prompt_state *S,
                  int pressed_cell, int flash_cell);

#endif /* RBPI4B_PROMPT_PAINT_H */
