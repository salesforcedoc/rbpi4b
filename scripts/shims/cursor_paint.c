/*
 * cursor_paint.c — see cursor_paint.h. Pure pixel work: no fb, no thread, no
 * environment, so the whole compositing rule is testable in-process.
 */
#include "cursor_paint.h"

#include <stddef.h>

/* '@' outline, '.' body, ' ' transparent. Written as one concatenated literal
 * with the length checked at compile time, because a row that is silently one
 * cell short would put every cell after it out of alignment — a bug that looks
 * like a mangled arrow rather than like a coding error. */
static const char glyph[] =
    "@           "
    "@@          "
    "@.@         "
    "@..@        "
    "@...@       "
    "@....@      "
    "@.....@     "
    "@......@    "
    "@.......@   "
    "@........@  "
    "@.....@@@@@@"
    "@..@..@     "
    "@.@ @..@    "
    "@@  @..@    "
    "@    @..@   "
    "     @..@   "
    "      @..@  "
    "      @..@  "
    "       @@   ";

_Static_assert(sizeof glyph - 1 == CURSOR_W * CURSOR_H,
               "the cursor glyph must be CURSOR_W * CURSOR_H cells");

int cursor_class_at(int col, int row)
{
    char c;

    if (col < 0 || col >= CURSOR_W || row < 0 || row >= CURSOR_H)
        return CURSOR_TRANSPARENT;
    c = glyph[row * CURSOR_W + col];
    if (c == '@')
        return CURSOR_OUTLINE;
    if (c == '.')
        return CURSOR_BODY;
    return CURSOR_TRANSPARENT;
}

unsigned int cursor_pixel(int bpp, int pressed, int cls)
{
    int dark;

    if (cls == CURSOR_TRANSPARENT)
        return 0;                  /* never written; see the apply() loop */
    /* Outline is the dark one normally; a press swaps them. */
    dark = (cls == CURSOR_OUTLINE) ? !pressed : pressed;
    if (bpp == 16)
        return dark ? 0x0000u : 0xffffu;    /* RGB565 */
    return dark ? 0x000000u : 0xffffffu;    /* XRGB8888 */
}

static unsigned int px_get(const void *pix, int pitch, int bpp, int x, int y)
{
    if (bpp == 16)
        return ((const unsigned short *)pix)[(size_t)y * pitch + x];
    return ((const unsigned int *)pix)[(size_t)y * pitch + x];
}

static void px_set(void *pix, int pitch, int bpp, int x, int y, unsigned int v)
{
    if (bpp == 16)
        ((unsigned short *)pix)[(size_t)y * pitch + x] = (unsigned short)v;
    else
        ((unsigned int *)pix)[(size_t)y * pitch + x] = v;
}

/* One loop for both directions, because they must agree exactly about which
 * cells they touch: restore() has to visit the same set paint() did, or a cell
 * would be left holding a pointer pixel with nothing to put back. The only
 * difference is which way the pixel moves — and in the restoring direction, the
 * condition that the buffer still holds what we wrote. */
static void cursor_apply(void *pix, int pitch, int fb_w, int fb_h, int bpp,
                         int x, int y, int pressed, unsigned int *saved,
                         int restoring)
{
    int col, row;

    for (row = 0; row < CURSOR_H; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_h)
            continue;
        for (col = 0; col < CURSOR_W; col++) {
            int px = x + col;
            int cls = cursor_class_at(col, row);
            unsigned int want, have;
            size_t slot = (size_t)row * CURSOR_W + col;

            if (cls == CURSOR_TRANSPARENT || px < 0 || px >= fb_w)
                continue;
            want = cursor_pixel(bpp, pressed, cls);
            have = px_get(pix, pitch, bpp, px, py);
            if (restoring) {
                /* Not ours any more: rbp has repainted this pixel since, and
                 * putting the old value back would punch a hole in the UI. */
                if (have == want)
                    px_set(pix, pitch, bpp, px, py, saved[slot]);
            } else if (have != want) {
                /* Already ours: rbp has not touched this cell since we drew it,
                 * so there is nothing to save and nothing to write. This is what
                 * makes a stationary arrow free, and what keeps it from blinking
                 * out while it is restored and redrawn — see cursor_paint.h. */
                saved[slot] = have;
                px_set(pix, pitch, bpp, px, py, want);
            }
        }
    }
}

void cursor_paint(void *pix, int pitch, int fb_w, int fb_h, int bpp,
                  int x, int y, int pressed, unsigned int *saved)
{
    cursor_apply(pix, pitch, fb_w, fb_h, bpp, x, y, pressed, saved, 0);
}

void cursor_restore(void *pix, int pitch, int fb_w, int fb_h, int bpp,
                    int x, int y, int pressed, unsigned int *saved)
{
    cursor_apply(pix, pitch, fb_w, fb_h, bpp, x, y, pressed, saved, 1);
}
