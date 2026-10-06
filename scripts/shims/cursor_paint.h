/*
 * cursor_paint.h — the visible pointer, as pure pixel work.
 *
 * Why this exists at all: rbp's UI is the XDJ-RX3's, and that panel is a
 * *touchscreen*, so rbp draws no cursor of its own — there is nothing in the
 * binary to reuse. On a Pi with an HDMI monitor the operator's pointer is a
 * mouse, and a mouse with no cursor is unusable: POINT_MOUSE_SPEED's 1:1 mapping
 * crosses 1280 logical pixels in a little over an inch of hand movement, so
 * without something drawn on the screen there is no way to know where a click
 * will land. Measured on the unit before this existed: the operator aimed at the
 * INFO button and clicked at (1279,0) — the screen corner, 14 px right of and
 * 10 px above the button — because the edge clamp had pinned the invisible
 * pointer there.
 *
 * The compositing problem, and why it is not as simple as drawing an arrow:
 * with DFB_PRESENT=off the DirectFB layer surface *is* the fb page (pages=1,
 * pan=0, no blit), so rbp paints its UI directly into the framebuffer we are
 * also writing to. Anything drawn there is therefore overwritten the moment rbp
 * repaints that region, and a naive save-under restore will likewise paste stale
 * pixels *over* a fresh repaint the moment rbp redraws under a pointer that has
 * since moved. So the restore is conditional: a pixel is only put back where the
 * buffer still holds the exact pixel paint() wrote. Where it does not, rbp has
 * been there since, and the pixel is left alone.
 *
 * The other half of the same problem is getting an arrow rbp has painted over
 * back onto the screen, and the first version of this got it wrong in two
 * separate ways. It painted once and then asked each tick whether the tip pixel
 * still held what it had written, repainting only if it did not — a question with
 * no trustworthy answer, because the tip is an outline cell, the outline is
 * black, and much of rbp's UI is black too. And a repaint that fires is only
 * useful if it fires fast enough: rbp redraws the whole frame at about 60 Hz
 * (measured — see the numbers in fb_cursor.c), so at the 16 ms tick this shipped
 * with, a redrawn block at (640,400) was found damaged in 91% of intervals and
 * the arrow was on screen roughly 9% of the time. That is why no framebuffer
 * capture ever contained one, and why the pointer was invisible exactly as before
 * the feature existed.
 *
 * Both halves are answered by the same change: paint() is idempotent, and the
 * caller repaints every tick with the tick far shorter than rbp's frame time. A
 * cell whose pixel is already what we would write is left completely alone, so a
 * stationary arrow costs a read pass and nothing else, and a wiped one comes back
 * on the next tick.
 *
 * Everything here is a function of a caller-supplied buffer so it can be tested
 * without a framebuffer, a thread or a device — see test_cursor.c, whose third
 * case is the conditional restore: it lets a third party repaint a glyph cell
 * between the paint and the restore and asserts the repaint survives.
 */
#ifndef RBPI4B_CURSOR_PAINT_H
#define RBPI4B_CURSOR_PAINT_H

/* A 12x19 arrow, tip at (0,0) of the box. Classic shape, deliberately not
 * larger: it has to be readable at 1280x800 over a busy deck UI without hiding
 * the control underneath it. */
#define CURSOR_W 12
#define CURSOR_H 19

/* What a glyph cell is. Paint and restore only ever touch OUTLINE and BODY
 * cells, which is what lets the conditional restore reason about "the pixel we
 * wrote" without keeping a second copy of the arrow. */
enum cursor_class {
    CURSOR_TRANSPARENT = 0,
    CURSOR_OUTLINE     = 1,
    CURSOR_BODY        = 2
};

/* The glyph, as pure functions of the cell. */
int cursor_class_at(int col, int row);

/* The pixel value written for a class, in the framebuffer's own format:
 * bpp 16 is RGB565 (what this target's fb is — measured 1280x800 16bpp,
 * line_length 2560), bpp 32 is XRGB8888. Black outline with a white body, and
 * the two swap when the button is down: that inversion is the operator's only
 * feedback that a click landed, since rbp's own UI does not show one. */
unsigned int cursor_pixel(int bpp, int pressed, int cls);

/* Draw the arrow with its tip at (x,y), into a buffer of `fb_w` x `fb_h` pixels
 * whose rows are `pitch` pixels apart. `saved` receives CURSOR_W*CURSOR_H
 * entries, row-major, holding the pixels that were under the glyph — the caller
 * owns that storage and hands it back to cursor_restore(). Cells that fall off
 * the screen are skipped, so a pointer at an edge paints what fits and the
 * matching restore() is the exact inverse.
 *
 * Idempotent: a cell that already holds the pixel this call would write is not
 * touched, and `saved` is not updated for it. Calling it again over an intact
 * arrow therefore writes nothing at all — which is what lets the caller repaint
 * every tick, and what makes a cell rbp has repainted over heal by itself. */
void cursor_paint(void *pix, int pitch, int fb_w, int fb_h, int bpp,
                  int x, int y, int pressed, unsigned int *saved);

/* Put back what cursor_paint() saved — but only where the buffer still holds the
 * pixel paint() wrote there. See the header comment: that condition is the whole
 * reason this is not a memcpy. */
void cursor_restore(void *pix, int pitch, int fb_w, int fb_h, int bpp,
                    int x, int y, int pressed, unsigned int *saved);

#endif /* RBPI4B_CURSOR_PAINT_H */
