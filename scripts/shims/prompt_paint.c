/*
 * prompt_paint.c -- the USB STOP chooser's image: title, rule, two outlined buttons.
 *
 * NO FRAME ROUND THE BOX, at the operator's ask of 2026-10-06, who had just had the
 * swipe-down band restyled the same way: "the drop down doesn't need the white border
 * around the endire thing, it only needs a thin white border around each button with
 * some padding in between and a black background. model it this way for the USB stop
 * menu as well". So the box is a black bed with a title, a rule and two buttons that
 * each wear their own one-pixel outline -- which the buttons already did (pp_cell()),
 * in their own ink colour; what went was only the PR_BORDER frame that used to enclose
 * the lot. PR_BORDER itself stays, as the box's margin: it is what keeps the title and
 * the buttons off the bed's edge and it is what keeps the hit test and the ink the same
 * pixels (prompt_zone.h's diagram is now the same picture with the outer rule struck
 * out).
 *
 * TWO BUTTONS IN ONE ROW, since 2026-10-07: the second line (OK and CANCEL) is gone
 * with the two-tap gesture it answered, and with it the `selected` argument this file
 * used to carry. What replaces it is the FLASH -- a hold in progress draws its button
 * PRESSED for half a period and its normal live face for the other half, so the
 * operator can see the three seconds running. Read prompt_zone.h's header before
 * changing either the layout or the gesture. Nothing in this file decides any of that;
 * it only draws whatever cells it is told, in whichever of the three faces it is told.
 *
 * See prompt_paint.h for the contract and side_paint.c for the family this belongs
 * to. The ~10 duplicated lines below (the pixel accessor, the fill, the frame, the
 * glyph walk and the blend) are duplicated there and in menu_window_paint.c for the
 * reason menu_paint.h records: exporting them would mean an accessor that has to
 * know three module-internal notions to save ten lines. What is NOT duplicated is the
 * palette -- menu_pixel() is public, so this box, the band, the drawers and the window
 * are the same colours by construction. The two OFF classes the box needs are in it
 * for the same reason (menu_paint.h).
 *
 * EVERY PIXEL IS WRITTEN ON EVERY CALL. The box's plane is single-buffered, so the
 * whole picture is filled first and every mark goes on top; nothing here reads the
 * buffer, so a second paint of the same state leaves the same image and the plane
 * cannot tear between states.
 */
#include "prompt_paint.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, NULL */

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void pp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
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

static void pp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            pp_set(v, x, y, pv);
}

/* A one-pixel outline, drawn inside the rect so a box's mark and its hit target are
 * the same pixels -- prompt_zone.c's hit test uses the inclusive rect, not the ink. */
static void pp_frame(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    pp_fill(v, x0, y0, x1, y0 + 1, pv);
    pp_fill(v, x0, y1 - 1, x1, y1, pv);
    pp_fill(v, x0, y0, x0 + 1, y1, pv);
    pp_fill(v, x1 - 1, y0, x1, y1, pv);
}

/* --- logical (box-local) to framebuffer ------------------------------------
 *
 * The view's picture rect IS the box (prompt_paint.h), so the two scales are the
 * box's own: PR_W logical px across `dw` framebuffer px, PR_H down `dh`. The origin
 * subtracted first is PR_X0/PR_Y0, so the arithmetic is in box-local coordinates and
 * a caller that moves the box on the page moves `bx`/`by` and nothing else. */
static int pp_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - PR_X0) * v->dw / PR_W;
}

static int pp_fy(const struct menu_view *v, int ly)
{
    return v->by + (ly - PR_Y0) * v->dh / PR_H;
}

/* A half-open rect in box-local coordinates: (lx0,ly0) included, (lx1,ly1) NOT --
 * which is what the fill loops want, and NOT the inclusive rect prompt_zone.c's hit
 * test uses. The two are one pixel apart by construction and every caller below adds
 * the 1 back at the call site, so the pairing is visible rather than buried. */
static void pp_rect(const struct menu_view *v, int lx0, int ly0, int lx1, int ly1,
                    int *fx0, int *fy0, int *fx1, int *fy1)
{
    *fx0 = pp_fx(v, lx0);
    *fy0 = pp_fy(v, ly0);
    *fx1 = pp_fx(v, lx1);
    *fy1 = pp_fy(v, ly1);
    /* The far edges are the NEXT pixel's origin and must be at least one past the
     * near ones, or a logical rect narrower than a framebuffer pixel would vanish. */
    if (*fx1 <= *fx0) *fx1 = *fx0 + 1;
    if (*fy1 <= *fy0) *fy1 = *fy0 + 1;
}

/* --- ink --------------------------------------------------------------------- */

/* `ink` over `base` at coverage `cov`/255. Coverage 0 and 255 return one argument
 * unchanged, so the ends of the range are palette values and only the glyph edges
 * blend -- the rule menu_paint.c and side_paint.c both keep, and the reason a second
 * paint of the same image writes the same bytes. */
static unsigned int pp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
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
 * [cx0, cx1) so text can never run out of the box it is drawn into. */
static void pp_glyph(const struct menu_view *v, int pen_fx, int line_fy,
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
            pp_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : pp_blend(v->bpp, base, ink, cov));
        }
}

static void pp_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        pp_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* A string centred in a box-local rect, by the LINE BOX -- the band's and the
 * drawer's rule, so a descender cannot nudge a cap off centre. The line box is
 * MENU_FONT_LINE DEVICE pixels and is not scaled: the atlas is drawn at 1:1, and only
 * the rectangles scale (menu_paint.c and side_paint.c do the same). */
static void pp_text_center(const struct menu_view *v, int lx0, int ly0, int lx1, int ly1,
                           const char *s, unsigned int base, unsigned int ink)
{
    int fx0, fy0, fx1, fy1, w, line_fy;

    pp_rect(v, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    w = menu_text_width(s);
    line_fy = fy0 + ((fy1 - fy0) - MENU_FONT_LINE) / 2;
    pp_text(v, (fx0 + fx1 - w) / 2, line_fy, s, base, ink, fx0, fx1);
}

/* --- the pieces ------------------------------------------------------------- */

/* One button: face, frame and centred label, over the cell's own inclusive logical
 * rect. `pressed` is the finger (or the lit half of a hold's flash, which is the same
 * pair of pixels -- prompt_zone.c hands both to it as one cell) and `live` is rbp's
 * answer about that cell. Pressed beats `live`, deliberately: a finger on a dim cell
 * has to see that it landed somewhere (prompt_paint.h), and the cell whose hold is
 * running is by construction a live one (prompt_zone.c refuses to start a hold on a
 * device rbp reports gone). */
static void pp_cell(const struct menu_view *v, const struct prompt_state *S, int cell,
                    int live, int pressed)
{
    char text[PR_LABEL_MAX + 1];
    int fx0, fy0, fx1, fy1;
    unsigned int face, ink;

    if (pressed) {
        face = menu_pixel(v->bpp, MENU_BTN_PRESSED);
        ink  = menu_pixel(v->bpp, MENU_LABEL_PRESSED);
    } else if (!live) {
        face = menu_pixel(v->bpp, MENU_BTN_OFF);
        ink  = menu_pixel(v->bpp, MENU_LABEL_OFF);
    } else {
        face = menu_pixel(v->bpp, MENU_BTN);
        ink  = menu_pixel(v->bpp, MENU_LABEL);
    }

    pp_rect(v, PR_CELL_X0(cell), PR_CELL_Y0(cell),
            PR_CELL_X1(cell) + 1, PR_CELL_Y1(cell) + 1, &fx0, &fy0, &fx1, &fy1);
    pp_fill(v, fx0, fy0, fx1, fy1, face);
    pp_frame(v, fx0, fy0, fx1, fy1, ink);

    /* THE LABEL IS CUT TO THE CELL, HERE, and this is the one place a name the operator
     * chose meets a rectangle this file owns. The budget is the cell's own inner width
     * less PR_LABEL_MARGIN on each side -- the same arithmetic prompt_paint_ok() below
     * refuses a view for, asked of the string that is actually going to be drawn rather
     * than of the cell's default. Nothing is drawn when it comes back empty, which at
     * an accepted size cannot happen: the refusal has already promised room for the
     * default label, which is ten characters. */
    prompt_text_clip(prompt_cell_text(S, cell), (fx1 - fx0) - 2 * PR_LABEL_MARGIN,
                     text, (int)sizeof text);
    pp_text_center(v, PR_CELL_X0(cell), PR_CELL_Y0(cell),
                   PR_CELL_X1(cell) + 1, PR_CELL_Y1(cell) + 1, text, face, ink);
}

/* Does every button hold its CELL'S OWN LABEL whole, at this view's scale? The band's
 * menu_labels_fit() asked the same question the day the labels grew, and for the same
 * reason: a button is now half a row wide, so a page small enough scales the box down
 * until a label would be clipped by the frame it is drawn inside -- and a clipped
 * label is a button whose name the operator cannot read, not a cosmetic fault. The two
 * columns of the frame are the whole of the margin the ink has (pp_text_center centres
 * in the rect the frame is drawn inside). */
static int pp_labels_fit(const struct menu_view *v)
{
    int c;

    /* THE DEFAULT, NOT THE DEVICE'S NAME, and the difference is the operator's own
     * sentence on the glass: a button reads "HOLD RBOX USB" where the cell's own label
     * is "HOLD USB 1". A name is up to five characters longer than that and a long one
     * is cut to fit (pp_cell()), so measuring the name here would refuse a view the box
     * can perfectly well draw -- and it would refuse it as a BLANK BOX, since a failed
     * prompt_paint_ok() paints nothing at all. What this refusal means is "this cell is
     * too narrow for a button to be legible on", and the label every cell is guaranteed
     * to have is the honest yardstick for that. PR_LABEL_MARGIN carries why the margin
     * is two and not the frame's one. */
    for (c = 1; c <= PROMPT_CELLS; c++) {
        int x0 = pp_fx(v, PR_CELL_X0(c));
        int x1 = pp_fx(v, PR_CELL_X1(c) + 1) - 1;

        if (x1 - x0 + 1 < 2 * PR_LABEL_MARGIN + menu_text_width(prompt_cell_label(c)))
            return 0;
    }
    return 1;
}

int prompt_paint_w(int page_dw)
{
    int w = (PR_W * page_dw) / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int prompt_paint_h(int page_dh)
{
    int h = (PR_H * page_dh) / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int prompt_paint_ok(const struct menu_view *v)
{
    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    /* Every row has to host the font's line box after scaling, or the labels collide
     * with the frames around them, and every button has to hold its CELL'S OWN LABEL
     * whole. AND BOTH ARE STILL PROPERTIES OF THE SIZE AND NOT OF THE STATE, which is
     * why they are asked once by the caller rather than per pixel here -- the device's
     * own name is clipped to the cell inside prompt_paint() instead of being allowed to
     * refuse the view (pp_labels_fit() says why). */
    if ((PR_ROW_H * v->dh) / PR_H < MENU_FONT_LINE + 2)
        return 0;
    return pp_labels_fit(v);
}

void prompt_paint(const struct menu_view *v, const struct prompt_state *S,
                  int pressed_cell, int flash_cell)
{
    unsigned int fill, rule, ink;
    int c;

    if (!prompt_paint_ok(v))
        return;
    if (pressed_cell < 0 || pressed_cell > PROMPT_CELLS)
        pressed_cell = 0;
    if (flash_cell < 0 || flash_cell > PROMPT_CELLS)
        flash_cell = 0;

    fill = menu_pixel(v->bpp, MENU_FILL);
    rule = menu_pixel(v->bpp, MENU_DIV);
    ink  = menu_pixel(v->bpp, MENU_LABEL);

    /* The bed first, so every pixel of the box is written by this call -- AND THE BED
     * IS THE WHOLE BOX. There used to be a PR_BORDER-thick MENU_BORDER frame drawn
     * round the box here; it is gone (this file's header), and what it covered is bed
     * like the rest, which is why the box's own four edges need no code of their own
     * any more. */
    pp_fill(v, 0, 0, v->dw, v->dh, fill);

    /* The title, across the whole inner width and centred on the box -- not on the
     * rows' narrower rect, because it is the box's caption and not a fourth row. */
    pp_text_center(v, PR_X0 + PR_BORDER, PR_TITLE_Y0,
                   PR_X1 + 1 - PR_BORDER, PR_TITLE_Y1 + 1,
                   prompt_title(), fill, ink);

    /* The rule under it, one logical px, across the inner width. */
    {
        int rx0, ry0, rx1, ry1;

        pp_rect(v, PR_X0 + PR_BORDER, PR_RULE_Y,
                PR_X1 + 1 - PR_BORDER, PR_RULE_Y + 1, &rx0, &ry0, &rx1, &ry1);
        pp_fill(v, rx0, ry0, rx1, ry1, rule);
    }

    /* THE TWO CELLS, and the second argument is the same face as the first: a cell is
     * drawn pressed when the finger is on it OR when it is the lit half of the hold's
     * flash, and prompt_zone.c is careful never to report the same cell as both
     * (prompt_pressed() answers 0 for the cell whose hold is running), so the blink has
     * somewhere to blink to. */
    for (c = 1; c <= PROMPT_CELLS; c++)
        pp_cell(v, S, c, prompt_cell_live(S, c),
                c == pressed_cell || c == flash_cell);
}
