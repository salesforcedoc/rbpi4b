/*
 * fx_paint.c -- the effect picker's image: rbp's own BEAT FX plate, redrawn over itself,
 * holding fourteen of his cells.
 *
 * The layout is fx_zone.h's arithmetic and is not repeated here; this file only draws
 * what that geometry says. THE STYLE IS NOT THIS FILE'S OWN, and this is the one
 * surface in the shim that is a COPY OF RBP'S DRAWING rather than a member of the
 * band's family. The box is rbp's panel on the right of the glass drawn in its own
 * rectangle, over it, and the operator asked for it to "look visually similar (size, color, font)" --
 * so the bed is his plate colour, a row is his black effect-name cell at the same 160
 * px width and with no outline, because his cells have none, and the ink is his white.
 * The three colours are sampled from a live /dev/fb0 capture and live in menu_paint.h
 * with the rest of the palette.
 *
 * THE ROW UNDER THE FINGER WEARS THE COLOUR RBP FILLS A SELECTED BOX WITH, and that is
 * the one thing not simply copied: his panel has no "pressed" state to copy, and this
 * is the colour his own CH SELECT box turns when it is the live one -- so a finger on a
 * row looks like a choice on that panel rather than like a foreign highlight. Nothing
 * else here varies with state: every row is live (a row IS the answer, so there is
 * nothing to arm and nothing to draw dead), and fx_zone.h records why the live effect
 * is not marked.
 *
 * See fx_paint.h for the contract. The ~10 duplicated lines below (the pixel accessor,
 * the fill, the frame, the glyph walk and the blend) are duplicated in prompt_paint.c,
 * side_paint.c and menu_window_paint.c for the reason menu_paint.h records: exporting
 * them would mean an accessor that has to know three module-internal notions to save
 * ten lines. What is NOT duplicated is the palette -- menu_pixel() is public, so this
 * box, the band, the drawers, the window and the USB chooser are the same colours by
 * construction.
 *
 * EVERY PIXEL IS WRITTEN ON EVERY CALL. The box's plane is single-buffered, so the
 * whole picture is filled first and every mark goes on top; nothing here reads the
 * buffer, so a second paint of the same state leaves the same image and the plane
 * cannot tear between states.
 */
#include "fx_paint.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, NULL */

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void fp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
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

static void fp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            fp_set(v, x, y, pv);
}

/* There is no fp_frame() here, and its absence is the point: this box outlines nothing.
 * fp_row()'s comment records the measurement -- rbp's effect-name cell has no edge of
 * its own, so a framed row would be the one mark on the box the panel underneath it is
 * without. prompt_paint.c, side_paint.c and menu_window_paint.c all keep theirs. */

/* --- logical (box-local) to framebuffer ------------------------------------
 *
 * The view's picture rect IS the box (fx_paint.h), so the two scales are the box's own:
 * FX_W logical px across `dw` framebuffer px, FX_H down `dh`. The origin subtracted
 * first is FX_X0/FX_Y0, so the arithmetic is in box-local coordinates and a caller that
 * moves the box on the page moves `bx`/`by` and nothing else. */
static int fp_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - FX_X0) * v->dw / FX_W;
}

static int fp_fy(const struct menu_view *v, int ly)
{
    return v->by + (ly - FX_Y0) * v->dh / FX_H;
}

/* A half-open rect in box-local coordinates: (lx0,ly0) included, (lx1,ly1) NOT -- which
 * is what the fill loops want, and NOT the inclusive rect fx_zone.c's hit test uses.
 * The two are one pixel apart by construction and every caller below adds the 1 back at
 * the call site, so the pairing is visible rather than buried. */
static void fp_rect(const struct menu_view *v, int lx0, int ly0, int lx1, int ly1,
                    int *fx0, int *fy0, int *fx1, int *fy1)
{
    *fx0 = fp_fx(v, lx0);
    *fy0 = fp_fy(v, ly0);
    *fx1 = fp_fx(v, lx1);
    *fy1 = fp_fy(v, ly1);
    /* The far edges are the NEXT pixel's origin and must be at least one past the near
     * ones, or a logical rect narrower than a framebuffer pixel would vanish. */
    if (*fx1 <= *fx0) *fx1 = *fx0 + 1;
    if (*fy1 <= *fy0) *fy1 = *fy0 + 1;
}

/* --- ink --------------------------------------------------------------------- */

/* `ink` over `base` at coverage `cov`/255. Coverage 0 and 255 return one argument
 * unchanged, so the ends of the range are palette values and only the glyph edges
 * blend -- the rule menu_paint.c, side_paint.c and prompt_paint.c all keep, and the
 * reason a second paint of the same image writes the same bytes. */
static unsigned int fp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;

    if (cov <= 0)
        return base;
    if (cov >= 255)
        return ink;
    if (bpp == 32) {
        br = (int)((base >> 16) & 0xffu); bg = (int)((base >> 8) & 0xffu);
        bb = (int)(base & 0xffu);
        ir = (int)((ink >> 16) & 0xffu);  ig = (int)((ink >> 8) & 0xffu);
        ib = (int)(ink & 0xffu);
        return (unsigned int)(((br * (255 - cov) + ir * cov + 127) / 255) << 16 |
                              ((bg * (255 - cov) + ig * cov + 127) / 255) << 8 |
                              ((bb * (255 - cov) + ib * cov + 127) / 255));
    }
    br = (int)((base >> 11) & 0x1fu) * 255 / 31;
    bg = (int)((base >> 5) & 0x3fu) * 255 / 63;
    bb = (int)(base & 0x1fu) * 255 / 31;
    ir = (int)((ink >> 11) & 0x1fu) * 255 / 31;
    ig = (int)((ink >> 5) & 0x3fu) * 255 / 63;
    ib = (int)(ink & 0x1fu) * 255 / 31;
    return (unsigned int)(((((br * (255 - cov) + ir * cov + 127) / 255) & 0xf8) << 8) |
                          ((((bg * (255 - cov) + ig * cov + 127) / 255) & 0xfc) << 3) |
                          ((((bb * (255 - cov) + ib * cov + 127) / 255) & 0xf8) >> 3));
}

/* One glyph, pen at `pen_fx`, the top of the LINE BOX at `line_fy`, clipped to
 * [cx0, cx1) so text can never run out of the row it is drawn into. */
static void fp_glyph(const struct menu_view *v, int pen_fx, int line_fy,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    int col, row;

    for (row = 0; row < (int)g->h; row++)
        for (col = 0; col < (int)g->w; col++) {
            int fx = pen_fx + g->left + col;
            int cov = menu_font_cov(g, col, row);

            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            fp_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : fp_blend(v->bpp, base, ink, cov));
        }
}

static void fp_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        fp_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* A string centred in a box-local rect, by the LINE BOX -- the band's, the drawer's and
 * the USB chooser's rule, so a descender cannot nudge a cap off centre. The line box is
 * MENU_FONT_LINE DEVICE pixels and is not scaled: the atlas is drawn at 1:1 and only
 * the rectangles scale. */
static void fp_text_center(const struct menu_view *v, int lx0, int ly0, int lx1, int ly1,
                           const char *s, unsigned int base, unsigned int ink)
{
    int fx0, fy0, fx1, fy1, w, line_fy;

    fp_rect(v, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    w = menu_text_width(s);
    line_fy = fy0 + ((fy1 - fy0) - MENU_FONT_LINE) / 2;
    fp_text(v, (fx0 + fx1 - w) / 2, line_fy, s, base, ink, fx0, fx1);
}

/* --- the pieces ------------------------------------------------------------- */

/* One row: face and centred label, over the row's own inclusive logical rect. `pressed`
 * is the finger; there is nothing else, because every row is live and no row is the
 * current effect (fx_zone.h).
 *
 * NO FRAME, and that is copied rather than omitted: rbp draws his own effect-name cell
 * as plain black on the plate with no outline round it (measured -- a horizontal scan
 * across the cell at y 118 reads BLACK for x 1100..1259 with plate either side and not
 * one pixel of anything between them). An outline here would be the one mark on the box
 * that the panel underneath it does not have. */
static void fp_row(const struct menu_view *v, int row, int pressed)
{
    int fx0, fy0, fx1, fy1;
    unsigned int face, ink;

    face = menu_pixel(v->bpp, pressed ? MENU_FX_SEL : MENU_FILL);
    ink  = menu_pixel(v->bpp, MENU_LABEL);

    fp_rect(v, FXLIST_ROW_X0, FXLIST_ROW_Y0(row - 1),
            FXLIST_ROW_X1 + 1, FXLIST_ROW_Y1(row - 1) + 1, &fx0, &fy0, &fx1, &fy1);
    fp_fill(v, fx0, fy0, fx1, fy1, face);
    fp_text_center(v, FXLIST_ROW_X0, FXLIST_ROW_Y0(row - 1),
                   FXLIST_ROW_X1 + 1, FXLIST_ROW_Y1(row - 1) + 1,
                   fxlist_row_label(row), face, ink);
}

/* Does every row hold its own label whole, at this view's scale? menu_labels_fit() and
 * pp_labels_fit() asked the same question for their own controls, and for the same
 * reason: a page small enough scales the box down until a label would be clipped by the
 * frame it is drawn inside, and a clipped label is a row whose name the operator cannot
 * read. The two columns of the frame are the whole of the margin the ink has, because
 * fp_text_center() centres in the rect the frame is drawn inside. */
static int fp_labels_fit(const struct menu_view *v)
{
    int row;

    for (row = 1; row <= FXLIST_ROWS; row++) {
        int x0 = fp_fx(v, FXLIST_ROW_X0);
        int x1 = fp_fx(v, FXLIST_ROW_X1 + 1) - 1;

        if (x1 - x0 + 1 < 2 + menu_text_width(fxlist_row_label(row)))
            return 0;
    }
    return 1;
}

int fxlist_paint_w(int page_dw)
{
    int w = (FX_W * page_dw) / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int fxlist_paint_h(int page_dh)
{
    int h = (FX_H * page_dh) / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int fxlist_paint_ok(const struct menu_view *v)
{
    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    /* Every row has to host the font's line box after scaling, or the labels collide
     * with the frames around them, and every row has to hold its own label whole. Both
     * are properties of the SIZE and not of the state, which is why they are asked once
     * by the caller rather than per pixel here. */
    if ((FX_ROW_H * v->dh) / FX_H < MENU_FONT_LINE + 2)
        return 0;
    return fp_labels_fit(v);
}

void fxlist_paint(const struct menu_view *v, int pressed_row)
{
    int row;

    if (!fxlist_paint_ok(v))
        return;
    if (pressed_row < 0 || pressed_row > FXLIST_ROWS)
        pressed_row = 0;

    /* The bed first, so every pixel of the box is written by this call, and the bed IS
     * the whole box -- there is no frame round it and, since the operator struck the
     * caption, nothing on it either. It is rbp's plate colour; the rows go on top. */
    fp_fill(v, 0, 0, v->dw, v->dh, menu_pixel(v->bpp, MENU_FX_PLATE));

    for (row = 1; row <= FXLIST_ROWS; row++)
        fp_row(v, row, row == pressed_row);
}
