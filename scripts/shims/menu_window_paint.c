/*
 * menu_window_paint.c -- the window's chrome, the URL and the keyboard, drawn over
 * the page.
 *
 * THE ~10 DUPLICATED LINES. menu_paint.c's px_set()/px_get() are static there, and
 * cursor_paint.c duplicates them for the same reason: the alternative is exporting
 * a pixel accessor that has to know about three module-internal notions (the view's
 * pitch, its depth, and where the picture rect sits inside the page) to save ten
 * lines. menu_paint.h already records that decision. What is NOT duplicated is the
 * palette: menu_pixel() is public, so the chrome and the panel are the same
 * colours by construction rather than by two tables agreeing.
 *
 * THE X AND THE MINIMIZE ARE STROKES. The atlas is generated and carries no 'x', so
 * an 'x' would mean re-baking the font for one mark. The keyboard's DEL and SHIFT
 * caps are WORDS instead, because the atlas does carry capitals and a word is
 * legible where a 4-px arrow at this size is not.
 */
#include "menu_window_paint.h"
#include "menu_window.h"
#include "menu_keyboard.h"
#include "menu_font.h"

#include <stddef.h>            /* size_t, NULL -- menu_paint.h carries neither,
                                * and this file is deliberately header-light */

/* --- pixels, in the view's own coordinates --------------------------------- */

static void wp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
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

static void wp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            wp_set(v, x, y, pv);
}

/* A one-pixel outline, drawn inside the rect so a box's mark and its hit target
 * are the same pixels -- menu_window.c's hit test uses the rect, not the ink. */
static void wp_frame(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    wp_fill(v, x0, y0, x1, y0 + 1, pv);
    wp_fill(v, x0, y1 - 1, x1, y1, pv);
    wp_fill(v, x0, y0, x0 + 1, y1, pv);
    wp_fill(v, x1 - 1, y0, x1, y1, pv);
}

/* --- the chrome's own marks ------------------------------------------------ */

/* The X: two diagonals of the box, inset so the mark reads as a mark and not as
 * the box's own border. */
static void wp_cross(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    int n = x1 - x0, i;

    if (n <= 6)
        return;
    for (i = 3; i < n - 3; i++) {
        wp_set(v, x0 + i, y0 + i, pv);
        wp_set(v, x0 + i, y1 - 1 - i, pv);
    }
}

/* The minimize: one horizontal stroke, in the same inset so the two boxes' marks
 * are the same weight. */
static void wp_minus(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    int n = x1 - x0, y = (y0 + y1) / 2, x;

    if (n <= 6)
        return;
    for (x = x0 + 3; x < x1 - 3; x++) {
        wp_set(v, x, y, pv);
        wp_set(v, x, y + 1, pv);
    }
}

/* The back and forward marks: a chevron, two diagonals meeting at a point, `dir`
 * negative for the one that points left. Strokes and not glyphs for wp_cross()'s
 * reason -- the atlas is generated and carries no arrow -- and a chevron rather
 * than a solid triangle because at 22 px a filled head reads as a smudge and a
 * chevron reads as an arrow. */
static void wp_chevron(const struct menu_view *v, int x0, int y0, int x1, int y1,
                       unsigned int pv, int dir)
{
    int n = x1 - x0, h = y1 - y0, cy = (y0 + y1) / 2, i, x;

    if (n <= 8 || h <= 8)
        return;
    for (i = 0; i <= h / 2 - 4; i++) {
        x = dir < 0 ? x0 + 4 + i : x1 - 5 - i;
        wp_set(v, x, cy - i, pv);
        wp_set(v, x, cy + i, pv);
    }
}

static void wp_box(const struct menu_view *v, int x0, int y0, int x1, int y1,
                   int pressed, int is_close){
    unsigned int face = menu_pixel(v->bpp, pressed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED : MENU_LABEL);

    /* Translated into the view's own origin: the window's rect is in the menu's
     * logical space and the view's buffer starts at the window's top-left. */
    x0 -= MW_X; x1 -= MW_X; y0 -= MW_Y; y1 -= MW_Y;
    wp_fill(v, x0, y0, x1, y1, face);
    wp_frame(v, x0, y0, x1, y1, ink);
    if (is_close)
        wp_cross(v, x0, y0, x1, y1, ink);
    else
        wp_minus(v, x0, y0, x1, y1, ink);
}

/* The two nav boxes, built like the title ones so a press reads the same way in
 * either row -- same face, same frame, only the mark differs. They sit in the URL
 * row, whose own fill is MENU_FILL, so their MENU_BTN face is what makes them read
 * as buttons on the field rather than as part of it. */
static void wp_nav_box(const struct menu_view *v, int x0, int y0, int x1, int y1,
                       int pressed, int dir)
{
    unsigned int face = menu_pixel(v->bpp, pressed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED : MENU_LABEL);

    x0 -= MW_X; x1 -= MW_X; y0 -= MW_Y; y1 -= MW_Y;
    wp_fill(v, x0, y0, x1, y1, face);
    wp_frame(v, x0, y0, x1, y1, ink);
    wp_chevron(v, x0, y0, x1, y1, ink, dir);
}

/* --- text, from the same atlas the panel's labels use ---------------------- */

/* menu_paint.c's menu_blend() is static there, so this is it again -- the same
 * deliberate duplication as wp_set() above, and for the same reason. What is NOT
 * copied is the palette: menu_pixel() is public and both files read it. */
static unsigned int wp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
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

/* One glyph, its pen origin at (pen_x) and the top of the LINE BOX at line_top --
 * the two coordinates menu_font.h's metrics are relative to. Clipped to [cx0, cx1)
 * so a URL longer than its field runs off the END OF THE FIELD and not across the
 * window's chrome. */
static void wp_glyph(const struct menu_view *v, int pen_x, int line_top,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    int col, row;

    for (row = 0; row < (int)g->h; row++)
        for (col = 0; col < (int)g->w; col++) {
            int fx = pen_x + g->left + col;
            int cov = menu_font_cov(g, col, row);

            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            /* Coverage 0 and 255 are exact -- the base and the ink themselves -- so
             * only a glyph's edges blend and the chrome stays idempotent. */
            wp_set(v, fx, line_top + g->top + row,
                   cov >= 255 ? ink : wp_blend(v->bpp, base, ink, cov));
        }
}

/* A string, left-aligned at pen_x, its line box starting at line_top. */
static void wp_text(const struct menu_view *v, int pen_x, int line_top,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        wp_glyph(v, pen_x, line_top, (unsigned char)*s, base, ink, cx0, cx1);
        pen_x += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* A string centred in a rect, by the LINE BOX -- menu_paint.c's rule for the panel's
 * labels, so a descender cannot nudge a cap off centre. */
static void wp_text_center(const struct menu_view *v, int x0, int y0, int x1, int y1,
                           const char *s, unsigned int base, unsigned int ink)
{
    wp_text(v, (x0 + x1 - menu_text_width(s)) / 2,
            y0 + (y1 - y0 - MENU_FONT_LINE) / 2, s, base, ink, x0, x1);
}

/* --- the URL field --------------------------------------------------------- */

/* The field's text, and the caret after it. The caret is a solid bar and not a
 * blinking one: the painter is pure (menu_window_paint.h) and a blink is a clock,
 * which would put the blink's phase in the paint thread's hands and make "painting
 * it twice paints it once" false on the tick the phase changed. A bar that is always
 * there says where the next character goes, which is the whole of what a caret is
 * for. */
static void wp_url_field(const struct menu_view *v, int x0, int y0, int x1, int y1)
{
    const char *s = menu_window_field();
    int sel = menu_window_field_sel();
    int editing = menu_window_target() == MW_EDIT_URL;
    unsigned int base = menu_pixel(v->bpp, sel ? MENU_BTN_PRESSED : MENU_FILL);
    unsigned int ink  = menu_pixel(v->bpp, sel ? MENU_LABEL_PRESSED : MENU_LABEL);
    int tx = x0 - MW_X + MW_URL_TEXT_X, line_top, w;

    line_top = y0 - MW_Y + (y1 - y0 - MENU_FONT_LINE) / 2;
    /* A selected field is filled in the accent and its text drawn dark -- the
     * operator's whole address is one tap from being replaced, and the inverted
     * field is the only thing on the glass that says so. */
    if (sel)
        wp_fill(v, x0 - MW_X, y0 - MW_Y, x1 - MW_X, y1 - MW_Y, base);
    wp_text(v, tx, line_top, s, base, ink, x0 - MW_X, x1 - MW_X);

    w = menu_text_width(s);
    if (editing && !sel) {
        wp_fill(v, tx + w + 1, line_top + MENU_FONT_INK_TOP,
                tx + w + 3, line_top + MENU_FONT_INK_TOP + MENU_FONT_INK_H, ink);
    }
}

/* --- the keyboard ---------------------------------------------------------- */

/* The whole popup, into the window's own coordinates. The bed is filled first and
 * the caps are drawn inset into it, so the gaps between them are the bed and not the
 * page showing through -- a keyboard whose keys were separated by the page would
 * read as a hole in the window.
 *
 * THE ONE PRESSED CAP is `key`, and the SHIFT cap is lit by its own STATE rather
 * than by the finger: an armed shift that looked unarmed would make the next
 * character a surprise. */
static void wp_keyboard(const struct menu_view *v, int key)
{
    int x0, y0, x1, y1, i, n = menu_keyboard_count();
    unsigned int bed   = menu_pixel(v->bpp, MENU_FILL);
    unsigned int face  = menu_pixel(v->bpp, MENU_BTN);
    unsigned int press = menu_pixel(v->bpp, MENU_BTN_PRESSED);
    unsigned int edge  = menu_pixel(v->bpp, MENU_DIV);
    unsigned int ink   = menu_pixel(v->bpp, MENU_LABEL);
    unsigned int inkp  = menu_pixel(v->bpp, MENU_LABEL_PRESSED);

    if (!menu_keyboard_is_up())
        return;
    menu_keyboard_rect(&x0, &y0, &x1, &y1);
    x0 -= MW_X; x1 -= MW_X; y0 -= MW_Y; y1 -= MW_Y;
    wp_fill(v, x0, y0, x1, y1, bed);

    for (i = 0; i < n; i++) {
        const struct menu_key *k = menu_keyboard_key(i);
        char cap[8];
        int on, kx0, ky0, kx1, ky1;

        if (!k)
            continue;
        on = (i == key) || (k->act == MK_SHIFT && menu_keyboard_shift());
        menu_keyboard_cap(i, cap, (int)sizeof cap);
        menu_keyboard_key_rect(i, &kx0, &ky0, &kx1, &ky1);
        kx0 -= MW_X; kx1 -= MW_X; ky0 -= MW_Y; ky1 -= MW_Y;
        /* A two-pixel inset: the bed shows between the caps, which is what makes
         * fifty cells read as fifty keys rather than as one slab. */
        kx0 += 2; ky0 += 2; kx1 -= 2; ky1 -= 2;
        wp_fill(v, kx0, ky0, kx1, ky1, on ? press : face);
        wp_frame(v, kx0, ky0, kx1, ky1, on ? inkp : edge);
        wp_text_center(v, kx0, ky0, kx1, ky1, cap,
                       on ? press : face, on ? inkp : ink);
    }
    /* The seam along the top, so the keyboard reads as a thing that came up over
     * the page rather than as part of it. */
    wp_fill(v, x0, y0 - 1, x1, y0, edge);
}

void menu_window_paint_chrome(const struct menu_view *v, int hit, int key)
{
    int w, h, x0, y0, x1, y1;
    unsigned int border = menu_pixel(v->bpp, MENU_BORDER);
    unsigned int bar    = menu_pixel(v->bpp, MENU_BTN);
    unsigned int field  = menu_pixel(v->bpp, MENU_FILL);
    unsigned int div    = menu_pixel(v->bpp, MENU_DIV);

    if (v->pix == NULL || v->dw <= 0 || v->dh <= 0)
        return;
    if (!menu_window_rect(NULL, NULL, &w, &h))
        return;

    /* The title bar across the full width, and the divider under the url field --
     * both clipped by wp_set, so a minimized window (h = MW_TITLE_H) draws its bar
     * and nothing below it without a second code path. */
    wp_fill(v, 0, 0, w, MW_TITLE_H, bar);
    if (h > MW_TITLE_H) {
        wp_fill(v, 0, MW_TITLE_H, w, MW_CONTENT_Y, field);
        wp_fill(v, 0, MW_CONTENT_Y - 1, w, MW_CONTENT_Y, div);
    }

    menu_window_close_box(&x0, &y0, &x1, &y1);
    wp_box(v, x0, y0, x1, y1, hit == MW_HIT_CLOSE, 1);
    menu_window_min_box(&x0, &y0, &x1, &y1);
    wp_box(v, x0, y0, x1, y1, hit == MW_HIT_MIN, 0);

    /* The field's text, and then the keyboard over the page area. Both are drawn
     * before the window's own edge so nothing can spill past it. */
    if (h > MW_TITLE_H) {
        menu_window_url_rect(&x0, &y0, &x1, &y1);
        wp_url_field(v, x0, y0, x1, y1);
        /* AFTER THE FIELD, so they sit on it rather than under it: a selected field
         * fills its whole row in the accent, and these two are inside that row. */
        menu_window_back_box(&x0, &y0, &x1, &y1);
        wp_nav_box(v, x0, y0, x1, y1, hit == MW_HIT_BACK, -1);
        menu_window_fwd_box(&x0, &y0, &x1, &y1);
        wp_nav_box(v, x0, y0, x1, y1, hit == MW_HIT_FORWARD, 1);
        wp_keyboard(v, key);
    }

    /* The window's own edge, last so it sits over the bars' ends. */
    wp_frame(v, 0, 0, w, h, border);
}

void menu_window_paint(const struct menu_view *v, int hit, int key)
{
    menu_window_paint_chrome(v, hit, key);
}
