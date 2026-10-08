/*
 * fxpad_paint.h -- the momentary X/Y pad's HUD: a dot where the finger is, over rbp's own
 * BPM cell.
 *
 * IT DREW A FRAME ROUND THE CELL AND NO LONGER DOES. The ring was a one-pixel MENU_BORDER
 * rectangle on the cell's outermost row and column, and those are rbp's own 8,8,8 border,
 * which rbp repaints itself -- so the same pixels were written by both of us at different
 * rates and the ring flickered. The operator, on the glass, 2026-10-07: *"you don't need to
 * draw the white box outline, it flashes so its too distracting."* It is gone; the cell's
 * border is rbp's and stays rbp's. What is left is the one mark with information in it.
 *
 * The sixth drawing module in the menu_* family and the FIRST ONE THAT IS NOT ALLOWED TO
 * OWN ITS RECTANGLE. Everything before it -- menu_paint.c, menu_window_paint.c,
 * side_paint.c, prompt_paint.c, fx_paint.c -- draws a surface that is ours, so each of
 * them fills every pixel of its view and the family rule "painting it twice paints it
 * once" is about the writer. This one is a HUD over rbp's live readout:
 *
 *     "allow it to be a x/y pad to control the beat fx ... and then when you release set
 *      it back to what the beat and level was before you pressed it."
 *
 * The cell the operator is holding shows `125.0 BPM / 480 msec / 1 BEAT`, and rbp is
 * KEEPING THOSE NUMBERS LIVE while the pad moves them -- measured 2026-10-07: the same
 * cell went "480 msec / 1 BEAT" -> "960 / 2 BEAT" -> "1920 / 4 BEAT" as the rung climbed
 * (fxpad_zone.h records the run). So the numbers are the operator's readback, they are
 * rbp's drawing and not ours, and the pad must not cover them.
 *
 * WHICH IS WHY THIS MODULE COPIES. On its own plane -- and a plane is RGB565, i.e. OPAQUE
 * -- the dot has to be drawn ON TOP OF rbp's pixels, and the plane's buffer holds nothing
 * but what we put there. So each call takes rbp's own cell pixels from `page` (the
 * framebuffer, where rbp has just drawn them), copies them into the destination, and only
 * then draws the mark. On the page route the copy is skipped -- the destination IS those
 * pixels -- and only the dot is written, which is the same picture arrived at from the
 * other side. `page` being NULL says exactly that.
 *
 * The cost of the copy is the cell and no more: 160 x 137 at the page's scale, ~44 KB a
 * tick, and it is copied every tick because rbp repaints the performance screen ~57 times
 * a second and the numbers underneath are changing (menu_paint.h's "there is no
 * save-under" applies doubly here: a saved-under slot would hold an old BPM).
 *
 * THE PALETTE IS menu_pixel()'s, like every module in the family: the dot is the same
 * Pioneer blue rbp fills a selected box with, ringed in MENU_BORDER so it stays legible
 * over rbp's white ink (fx_paint.c borrows the blue for the same reason).
 *
 * PURE, in the sense the family means: a function of its arguments. It reads the PAGE it
 * is given and never the buffer it is writing, so painting the same page twice writes the
 * same bytes and a single-buffered plane cannot tear between two states.
 */
#ifndef RBPI4B_FXPAD_PAINT_H
#define RBPI4B_FXPAD_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "fxpad_zone.h"      /* FXPAD_* geometry, the cell's own rect */

/* The dot's radius, in the cell's own logical px -- so it scales with the panel like
 * every other mark in this shim, and the one number that decides how big a finger's
 * position reads. 9 of the cell's 160 px is a dot 18 px across in a 137 px tall cell:
 * large enough to see at a glance and small enough that the two ends of the X axis are
 * still distinguishable. */
#define FXPAD_DOT_R  9

/* Can this view be drawn into? The mark needs room -- the dot is FXPAD_DOT_R scaled twice
 * over plus its ring, across both axes -- so a page small enough that the cell collapses
 * is refused here rather than drawn as a grey smudge. The caller asks this once, on the
 * same view it is about to paint, exactly as fxlist_paint_ok() is asked. */
int fxpad_paint_ok(const struct menu_view *v);

/* The cell's size in framebuffer pixels for a page of dw x dh -- the size the pad's
 * plane has to be set up at, and the size of the rect the copy reads. The caller asks
 * rather than computing it twice, so the plane and the copy cannot disagree. */
int fxpad_paint_w(int page_dw);
int fxpad_paint_h(int page_dh);

/* Draw the pad's HUD into `dst`.
 *
 *   page   rbp's own pixels for this cell -- a view of the FRAMEBUFFER whose origin is
 *          the cell's top-left and whose size is dst's -- or NULL when `dst` is already
 *          those pixels (the page route, and every MENU_PLANE=0 machine).
 *   mark_on / mx / my   fxpad_mark()'s answer: whether to draw a dot, and where, in the
 *          menu's own logical coordinates.
 *
 * The copy happens first if `page` is given, then the dot over it. Every byte of `dst` is
 * written either way: a plane is a dumb buffer and must never be shown holding the bytes
 * it was created with. Does nothing when fxpad_paint_ok(dst) is 0, or
 * when the two views cannot be lined up (see the .c for why that fill is a fill and not
 * a refusal). */
void fxpad_paint(const struct menu_view *dst, const struct menu_view *page,
                 int mark_on, int mx, int my);

#endif /* RBPI4B_FXPAD_PAINT_H */
