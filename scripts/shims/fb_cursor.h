/*
 * fb_cursor.h — the visible pointer, as a thread in the player's process.
 *
 * Started from tscfake_open(), i.e. the moment rbp opens the fake touch device,
 * because that is the same trigger pointsrc uses and it identifies the process
 * that owns the screen. Idempotent: rbp opens the device more than once.
 *
 * Gated by POINT_CURSOR (default 1) and paced by POINT_CURSOR_MS (default 0.5,
 * i.e. ~2 kHz). The rate is not a taste question: the arrow is composited into
 * the page rbp paints its UI into, so it is erased once per rbp frame and has to
 * be redrawn between frames — see the measurements in fb_cursor.c. It draws
 * nothing at all unless a *relative* device is driving the pointer — see
 * pointsrc_cursor() — because an absolute digitiser reports where it is being
 * touched and needs no arrow, while a mouse is unusable without one.
 */
#ifndef RBPI4B_FB_CURSOR_H
#define RBPI4B_FB_CURSOR_H

int fb_cursor_start(void);

#endif /* RBPI4B_FB_CURSOR_H */
