/*
 * menu_window_paint.h -- the window's pixels. PURE, as menu_paint.h is.
 *
 * menu_window_paint() is a function of the window's state, its rect and the
 * classes in menu_paint.h's palette -- never of what was already in the buffer
 * beneath the chrome. The page underneath is the exception and is deliberately so:
 * the frame is painted into the buffer by the loader, and the chrome is drawn over
 * it, so "painting the chrome twice paints it once" holds for every chrome pixel
 * and the page is not re-fetched to redraw a border.
 *
 * THE TEXT IS THE ATLAS'S, and the same one the panel's labels use (menu_font.h):
 * the URL and the keyboard's caps are drawn glyph by glyph from the coverage table,
 * blended against whatever the field or the cap is filled with. That is why the
 * atlas grew lowercase and punctuation (bake_menu_font.py, 2026-10-04) -- a URL is
 * not capitals and digits.
 */
#ifndef RBLIVE4_MENU_WINDOW_PAINT_H
#define RBLIVE4_MENU_WINDOW_PAINT_H

#include "menu_paint.h"     /* struct menu_view */

/* Draw the whole window -- border, title bar, the two chrome boxes, the url field
 * with its text, the keyboard when it is up, and the page -- into `v`, whose buffer
 * holds the page already. `hit` is the chrome cell the finger is on (an MW_HIT_*) and
 * `key` the key index it is on (-1 for none), so the box and the cap under the finger
 * are drawn in the pressed class. */
void menu_window_paint(const struct menu_view *v, int hit, int key);

/* Just the chrome, the field's text and the keyboard, without touching the page:
 * what a minimize, a tap or a keystroke needs, and it keeps the frame's pixels out
 * of the repaint. */
void menu_window_paint_chrome(const struct menu_view *v, int hit, int key);

#endif /* RBLIVE4_MENU_WINDOW_PAINT_H */
