/*
 * menu_draw.h -- the driver: keep the panel's image on the real framebuffer.
 *
 * The shim's third drawing thing (cursor_paint.c draws the arrow, menu_paint.c the
 * panel) and, like the arrow's, it runs on a thread of its own -- see menu_draw.c
 * for the measurement that ruled out the alternative, fb_shim.c's
 * FBIOPAN_DISPLAY hook. It is the same compositor the arrow uses, at the same
 * period, over a much larger region:
 *
 *   - the framebuffer is mapped once, lazily, and re-mapped if it ever fails;
 *   - a tick does nothing at all while the panel is closed (one int read);
 *   - while it is open, a tick asks whether the image is still on the page and
 *     repaints it only if it is not, so the steady-state cost is one full paint
 *     per rbp frame rather than one per tick;
 *   - nothing is drawn when the panel CLOSES: rbp repaints its whole frame ~57
 *     times a second, so its next frame is the restore (menu_paint.h).
 *
 * The thread is started from tscfake.c beside fb_cursor_start(), because that is
 * the moment the process that owns the screen has a reason to draw on it, and is
 * gated by POINT_MENU: with the menu off there is nothing to composite, so the
 * thread is not created.
 */
#ifndef RBLIVE4_MENU_DRAW_H
#define RBLIVE4_MENU_DRAW_H

/* Start the paint thread. Once per process (a second call is a no-op), and
 * returns 0 whether or not a thread was created -- the only failure it reports is
 * a pthread_create() that did not work. As with fb_cursor_start(), a failure here
 * costs the feature and not the unit. */
int menu_draw_start(void);

/* One look at the panel: paint it if the picture changed or if rbp has painted
 * over it, and nothing at all while it is closed. Called by the paint thread, and
 * by fb_shim.c's FBIO_WAITFORVSYNC interposer -- the frame boundary rbp drives its
 * own render loop from (measured 57.1/s). Safe to call from both: every step is
 * idempotent. */
void menu_frame_tick(void);

#endif /* RBLIVE4_MENU_DRAW_H */
