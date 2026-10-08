/*
 * fxpad_paint.c -- the dot over rbp's own BPM cell.
 *
 * See fxpad_paint.h for what this is and why it is the one module in the family that
 * COPIES before it draws. This file is the whole of the drawing: one mark and one
 * memcpy-shaped loop, ~10 lines of pixel accessor duplicated from fx_paint.c and
 * side_paint.c for the reason menu_paint.h records (exporting it would mean an accessor
 * that has to know three module-internal notions to save ten lines).
 *
 * NOTHING HERE READS THE DESTINATION. The copy reads the PAGE -- rbp's pixels, which is
 * the one thing the HUD has to be a function of -- and every write goes to dst. So a
 * second paint of the same page writes the same bytes, and the plane, which is
 * single-buffered, cannot tear between two states of the pad.
 */
#include "fxpad_paint.h"

#include <stddef.h>          /* size_t, NULL */

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void fxp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
{
    unsigned char *row;

    if (fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)fx * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)fx * 2) = (unsigned short)pv;
}

/* The page's pixel at the same view-local point. Out of range answers 0, which no
 * palette value is -- the same convention menu_paint.c's menu_get() uses, and it cannot
 * arise here because the two views are the same size by construction (the caller derives
 * both from fxpad_paint_w/h). */
static unsigned int fxp_get(const struct menu_view *v, int fx, int fy)
{
    const unsigned char *row;

    if (!v || !v->pix || fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return 0;
    row = (const unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        return *(const unsigned int *)(row + (size_t)fx * 4);
    return *(const unsigned short *)(row + (size_t)fx * 2);
}

/* --- logical (cell-local) to framebuffer ------------------------------------
 *
 * The view's picture rect IS the cell (fxpad_paint.h), the contract fx_paint.h states for
 * the picker's box: `dw`/`dh` are the cell at the page's scale and `bx`/`by` are where it
 * lands, so the same two marks are drawn on the pad's own plane and, on the fallback
 * route, straight onto the page. The origin subtracted first is FXPAD_X0/FXPAD_Y0, so the
 * arithmetic is in cell-local coordinates. */
static int fxp_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - FXPAD_X0) * v->dw / FXPAD_W;
}

static int fxp_fy(const struct menu_view *v, int ly)
{
    return v->by + (ly - FXPAD_Y0) * v->dh / FXPAD_H;
}

/* --- the copy --------------------------------------------------------------- */

/* rbp's pixels for this cell, from the page into the destination. `page` is a view of
 * the framebuffer whose origin is already the cell's top-left and whose size is dst's,
 * so this is a straight point-for-point move through both strides.
 *
 * NO PAGE MEANS NO COPY AND NOT A FILL. `page` is NULL when the destination IS rbp's own
 * pixels -- the page route, and every MENU_PLANE=0 machine -- and then the only thing to
 * write is the two marks. Filling here would be the worst bug this module could have: it
 * would black out the operator's own BPM/msec/BEAT readout, which is the one thing the
 * HUD exists to leave visible.
 *
 * A MISMATCHED PAGE FILLS, and that is the opposite case: it can only happen on a plane
 * route (there is a page to mismatch with), the buffer would otherwise be shown holding
 * whatever the kernel left in it, and a black cell the operator sees at once is a better
 * failure than uninitialised pixels. It cannot arise from menu_draw.c, which derives both
 * views from fxpad_paint_w/h. */
static void fxp_copy(const struct menu_view *dst, const struct menu_view *page)
{
    int x, y;

    if (!page)
        return;

    if (page->bpp != dst->bpp || page->dw != dst->dw || page->dh != dst->dh) {
        unsigned int bed = menu_pixel(dst->bpp, MENU_FILL);

        for (y = 0; y < dst->dh; y++)
            for (x = 0; x < dst->dw; x++)
                fxp_set(dst, x, y, bed);
        return;
    }

    for (y = 0; y < dst->dh; y++)
        for (x = 0; x < dst->dw; x++)
            fxp_set(dst, x, y, fxp_get(page, x, y));
}

/* --- the mark --------------------------------------------------------------- */

/* THE FRAME IS GONE, and it is the operator's own verdict that removed it: *"you don't need
 * to draw the white box outline, it flashes so its too distracting"* (2026-10-07, on the
 * glass). It was a one-pixel ring of MENU_BORDER drawn on the cell's own outermost row and
 * column -- which are rbp's 8,8,8 border -- and rbp repaints that border itself, so the two
 * writes land on the same pixels at different rates and the ring strobed. The dot is what
 * the pad actually needs (the level has no readout anywhere else on the panel); the
 * rectangle was decoration, and it cost a flicker on a screen the operator watches for
 * hours. The cell's own border is left exactly as rbp drew it. */

/* The dot's radius in framebuffer pixels, and the ONE place it is computed: fxpad_paint_ok()
 * has to be able to ask whether the dot fits, and the answer it gets has to be the answer
 * fxp_dot() will draw. A floor of 3 because below it a "dot" is a single pixel that reads
 * as dirt on rbp's ink rather than as a position. */
static int fxp_dot_r(const struct menu_view *v)
{
    int r = (FXPAD_DOT_R * v->dw) / FXPAD_W;

    return r < 3 ? 3 : r;
}

/* The dot, at the finger (or, while the restore is running, at the value on its way
 * home -- fxpad_zone.h's fxpad_mark()). A filled disc of MENU_FX_SEL with a ring of
 * MENU_BORDER round it: the blue alone would vanish against rbp's white ink, which is
 * exactly what the middle of this cell is full of, and the ring is what keeps the dot
 * legible over the two numerals it will spend most of its life on.
 *
 * The position is fxpad_mark()'s and is already clamped into the cell; a pixel that still
 * lands outside the view is dropped by fxp_set(). */
static void fxp_dot(const struct menu_view *v, int mx, int my)
{
    unsigned int face = menu_pixel(v->bpp, MENU_FX_SEL);
    unsigned int ring = menu_pixel(v->bpp, MENU_BORDER);
    int cx = fxp_fx(v, mx), cy = fxp_fy(v, my);
    int r  = fxp_dot_r(v);
    int ro = r + 1, x, y;

    for (y = -ro; y <= ro; y++) {
        for (x = -ro; x <= ro; x++) {
            int d2 = x * x + y * y;

            if (d2 <= r * r)
                fxp_set(v, cx + x, cy + y, face);
            else if (d2 <= ro * ro)
                fxp_set(v, cx + x, cy + y, ring);
        }
    }
}

/* --- the public surface ----------------------------------------------------- */

int fxpad_paint_w(int page_dw)
{
    int w = (FXPAD_W * page_dw) / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int fxpad_paint_h(int page_dh)
{
    int h = (FXPAD_H * page_dh) / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int fxpad_paint_ok(const struct menu_view *v)
{
    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    /* Room for the mark. The dot is fxp_dot_r() scaled twice over plus its ring, and it has
     * to be able to sit anywhere in the cell -- so the cell must have room for it across
     * BOTH axes, or the two ends of an axis would be the same picture. (This bound was
     * written for the frame as well, which needed two rows and two columns; the frame is
     * gone and the dot's bound is the stricter of the two, so it is kept unchanged.) A page
     * that small is a page this shim's band is already refusing (menu_view_ok() asks the
     * same kind of question), so this is a refusal in the same family and not a new limit. */
    {
        int room = 2 * fxp_dot_r(v) + 3;

        if (v->dw < room || v->dh < room)
            return 0;
    }
    return 1;
}

void fxpad_paint(const struct menu_view *dst, const struct menu_view *page,
                 int mark_on, int mx, int my)
{
    if (!fxpad_paint_ok(dst))
        return;

    fxp_copy(dst, page);
    if (mark_on)
        fxp_dot(dst, mx, my);
}
