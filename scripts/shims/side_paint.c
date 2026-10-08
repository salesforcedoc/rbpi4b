/*
 * side_paint.c -- the edge drawers' image: title, SYNC, the nudge pair, the fader and
 * its readout, then CUE and PLAY at the bottom -- on a black bed with no frame around
 * the panel (2026-10-06; every control still frames itself).
 *
 * THE ~10 DUPLICATED LINES, again and deliberately. menu_paint.c's pixel accessors
 * are static there and menu_window_paint.c duplicates them for the same reason
 * (menu_paint.c's header records the decision): the alternative is exporting an
 * accessor that must know three module-internal notions to save ten lines. What is
 * NOT duplicated is the palette -- menu_pixel() is public, so the drawer, the band
 * and the window are the same colours by construction.
 *
 * EVERY PIXEL IS WRITTEN ON EVERY CALL. The drawer's plane is single-buffered
 * (side_paint.h), so the panel's rectangle is filled first and every mark goes on
 * top; nothing here reads the buffer, so a second paint of the same state leaves the
 * same image and the plane cannot tear between states.
 */
#include "side_paint.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, NULL */
#include <stdio.h>           /* snprintf, for the fader readout */

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void sp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
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

static void sp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            sp_set(v, x, y, pv);
}

/* A one-pixel outline, drawn inside the rect so a box's mark and its hit target are
 * the same pixels -- side_zone.c's hit test uses the inclusive rect, not the ink. */
static void sp_frame(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    sp_fill(v, x0, y0, x1, y0 + 1, pv);
    sp_fill(v, x0, y1 - 1, x1, y1, pv);
    sp_fill(v, x0, y0, x0 + 1, y1, pv);
    sp_fill(v, x1 - 1, y0, x1, y1, pv);
}

/* --- logical (panel-local) to framebuffer, with the mirror ----------------- */

/* One local logical x to a framebuffer column. The mirror is here and nowhere else:
 * the view's buffer is placed at the panel's SCREEN rectangle, so the right drawer's
 * local 0 sits at the buffer's RIGHT edge. */
static int sp_x(const struct menu_view *v, int side, int lx)
{
    if (side == SZ_RIGHT)
        lx = SZ_W - 1 - lx;
    return (lx * v->dw) / SZ_W;
}

static int sp_y(const struct menu_view *v, int y)
{
    return (y * v->dh) / SZ_H;
}

/* An inclusive local rect to a half-open framebuffer rect. The two x ends are sorted
 * because the mirror makes the first the larger one on the right drawer. */
static void sp_rect(const struct menu_view *v, int side, int lx0, int ly0,
                    int lx1, int ly1, int *fx0, int *fy0, int *fx1, int *fy1)
{
    int a = sp_x(v, side, lx0), b = sp_x(v, side, lx1);

    *fx0 = a < b ? a : b;
    *fx1 = (a < b ? b : a) + 1;
    *fy0 = sp_y(v, ly0);
    *fy1 = sp_y(v, ly1) + 1;
}

/* --- text, from the band's own atlas --------------------------------------- */

/* menu_paint.c's menu_blend() is static there, so this is it again -- the same
 * deliberate duplication as sp_set(), for the same reason. */
static unsigned int sp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
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

/* One glyph, pen at (pen_fx) and the top of the LINE BOX at line_fy, clipped to
 * [cx0, cx1) so text can never run off the panel it is drawn into. */
static void sp_glyph(const struct menu_view *v, int pen_fx, int line_fy,
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
            /* 0 and 255 are exact -- the base and the ink themselves -- so only the
             * glyph's edges blend and the paint stays idempotent. */
            sp_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : sp_blend(v->bpp, base, ink, cov));
        }
}

static void sp_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        sp_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* A string centred in a LOCAL rect, by the LINE BOX -- the band's rule, so a
 * descender cannot nudge a cap off centre. The rect is scaled and mirrored first,
 * which is why the centring happens on the framebuffer rect and not on the logical
 * one: on the right drawer the logical ends run backwards. The line box is
 * MENU_FONT_LINE DEVICE pixels and not the scaled one, because the atlas is drawn
 * at 1:1 -- only the rectangles scale (menu_paint.c:294-299 does the same). */
static void sp_text_center(const struct menu_view *v, int side, int lx0, int ly0,
                           int lx1, int ly1, const char *s,
                           unsigned int base, unsigned int ink)
{
    int fx0, fy0, fx1, fy1, w, line_fy;

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    w = menu_text_width(s);
    line_fy = fy0 + (fy1 - fy0 - MENU_FONT_LINE) / 2;
    sp_text(v, (fx0 + fx1 - w) / 2, line_fy, s, base, ink, fx0, fx1);
}

/* --- the pieces ------------------------------------------------------------ */

/* A button box: face, frame, centred label, over an explicit LOCAL x range. The
 * range is a parameter rather than SZ_BTN_X0/X1 because the nudge pair is two boxes
 * in one row and they share this code -- a second near-identical function for a
 * half-width button would be the same 8 lines with two constants changed. The pressed
 * pair is the band's own (MENU_BTN_PRESSED / MENU_LABEL_PRESSED), so a press reads the
 * same here as it does on the strip. */
/* A button that rbp has LIT. The face stays the dark MENU_BTN and the accent goes
 * into the FRAME AND THE LABEL -- a backlit control, which is what the three it is
 * used for are: rbp lights a PLAY, a CUE and a SYNC and the hardware beside this
 * panel lights the same three. The alternative, filling the face with the accent,
 * was rejected because it is the same picture as "a finger is on me"
 * (MENU_BTN_PRESSED) and would have made the two indistinguishable -- and because
 * PLAY is lit for as long as a deck is playing, which is most of the time.
 *
 * A BLINK is not a third appearance: the caller resolves it and passes 1 or 0
 * (side_paint.h), so a flashing button is this button alternating with the one
 * below, at rbp's own period. Nothing here keeps a clock, and that is what keeps
 * every paint a pure function of its arguments. */
static void sp_button_x(const struct menu_view *v, int side, int lx0, int lx1,
                        int ly0, int ly1, const char *label, int pressed, int lit)
{
    int fx0, fy0, fx1, fy1;
    unsigned int face = menu_pixel(v->bpp, pressed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED
                                      : (lit ? MENU_BTN_PRESSED : MENU_LABEL));

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, face);
    sp_frame(v, fx0, fy0, fx1, fy1, ink);
    sp_text_center(v, side, lx0, ly0, lx1, ly1, label, face, ink);
}

static void sp_button(const struct menu_view *v, int side, int ly0, int ly1,
                      const char *label, int pressed, int lit)
{
    sp_button_x(v, side, SZ_BTN_X0, SZ_BTN_X1, ly0, ly1, label, pressed, lit);
}

/* THE NUDGE MARK, DRAWN RATHER THAN SET -- and it has to be, because the atlas has no
 * '+'. menu_font.h carries space, 0-9, A-Z, a-z and `.` `/` `-` `_` `:` `?` `=` `&` `@`
 * `#`; '-' is there and '+' is not, and a pair where the minus came from the font and
 * the plus from two rectangles would be visibly two different marks at the same weight.
 * So BOTH are bars: a horizontal one for '-', and the same bar with a vertical one
 * through its middle for '+'. `arm` is the bar's half-length, `thick` its weight --
 * the same 1:1 DEVICE pixels as the glyphs beside them, not the scaled rectangle, so
 * the mark matches the labels on any panel size.
 *
 * Drawn as an even-armed cross so the two cells read as a pair at a glance, which is
 * the whole job of the mark. */
static void sp_sign(const struct menu_view *v, int side, int lx0, int lx1,
                    int ly0, int ly1, int plus, int pressed)
{
    int fx0, fy0, fx1, fy1, cx, cy;
    int arm = SP_SIGN_ARM, thick = SP_SIGN_THICK;
    unsigned int ink = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED : MENU_LABEL);

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    cx = (fx0 + fx1) / 2;
    cy = (fy0 + fy1) / 2;
    sp_fill(v, cx - arm, cy - thick / 2, cx + arm + 1, cy - thick / 2 + thick, ink);
    if (plus)
        sp_fill(v, cx - thick / 2, cy - arm, cx - thick / 2 + thick, cy + arm + 1, ink);
}

/* The fader: the well, then the cap, then the value. The well is MENU_DIV -- the
 * band's own seam colour, a step behind the panel fill, which is what a groove is --
 * and the cap is a MENU_BTN box framed in MENU_LABEL, so it reads as a knob rather
 * than as a lighter patch of track. `grabbed` lights it in the pressed pair. */
static void sp_fader(const struct menu_view *v, int side, int fader_v, int grabbed)
{
    int fx0, fy0, fx1, fy1, cy, hy0, hy1;
    unsigned int well = menu_pixel(v->bpp, MENU_DIV);
    unsigned int face = menu_pixel(v->bpp, grabbed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, grabbed ? MENU_LABEL_PRESSED : MENU_LABEL);

    sp_rect(v, side, SZ_FADER_X0, SZ_FADER_Y0, SZ_FADER_X1, SZ_FADER_Y1,
            &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, well);

    /* The cap's centre, clamped so its own half-height stays inside the well at both
     * ends: at v = 1023 the cap's top edge is the well's top edge and not above it. */
    cy = side_fader_y(fader_v);
    if (cy < SZ_FADER_Y0 + SP_HANDLE_H / 2)
        cy = SZ_FADER_Y0 + SP_HANDLE_H / 2;
    if (cy > SZ_FADER_Y1 - SP_HANDLE_H / 2)
        cy = SZ_FADER_Y1 - SP_HANDLE_H / 2;
    hy0 = cy - SP_HANDLE_H / 2;
    hy1 = hy0 + SP_HANDLE_H - 1;

    sp_rect(v, side, SZ_FADER_X0, hy0, SZ_FADER_X1, hy1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, face);
    sp_frame(v, fx0, fy0, fx1, fy1, ink);
}

/* The value, ABOVE the fader (it sits between the nudge pair and the fader now that
 * PLAY has moved to the bottom). 0..100. Digits only: the atlas carries no '%'
 * (menu_font.h's punctuation run), and a number under a fader needs no unit. */
static void sp_readout(const struct menu_view *v, int side, int fader_v)
{
    char buf[8];

    snprintf(buf, sizeof buf, "%d", side_fader_pct(fader_v));
    sp_text_center(v, side, SZ_READ_X0, SZ_READ_Y0, SZ_READ_X1, SZ_READ_Y1, buf,
                   menu_pixel(v->bpp, MENU_FILL), menu_pixel(v->bpp, MENU_LABEL));
}

int side_paint_ok(const struct menu_view *v)
{
    int min;

    if (!v || !v->pix)
        return 0;
    if (v->pitch <= 0 || v->fb_w <= 0 || v->fb_h <= 0)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw <= 0 || v->dh <= 0)
        return 0;
    if (v->bx < 0 || v->by < 0)
        return 0;
    if (v->bx + v->dw > v->fb_w || v->by + v->dh > v->fb_h)
        return 0;
    /* Room for the frame and something inside it. The plane IS the panel here, so
     * both extents are the view's own. */
    if (v->dw < 2 * SP_BORDER + 2 || v->dh < 2 * SP_BORDER + 2)
        return 0;
    /* AND ROOM FOR THE LABELS, which is menu_view_ok()'s rule one level up: the
     * button boxes have to host the font's line box and its widest label. Both are
     * drawn at 1:1 DEVICE pixels -- the atlas is not scaled, only the rectangles are
     * -- so it is the SCALED box that has to be big enough, exactly as the band asks
     * only that its own band host the line box and not that the page be 1280 wide. */
    min = ((SZ_CUE_Y1 - SZ_CUE_Y0 + 1) * v->dh) / SZ_H;
    if (min < MENU_FONT_LINE)
        return 0;
    min = (SZ_BTN_X1 - SZ_BTN_X0 + 1) * v->dw / SZ_W;
    if (min < menu_text_width("PLAY"))
        return 0;
    /* ...and the NARROWEST box in the panel, which is a nudge cell, has to host the
     * DRAWN cross: 2*SP_SIGN_ARM+1 device px wide and as many tall. It is the tightest
     * constraint here and the only one that would silently clip a mark rather than a
     * word, because a bar drawn past the edge is simply cut with no trace. */
    if ((SZ_NUDGE_M_X1 - SZ_NUDGE_M_X0 + 1) * v->dw / SZ_W < 2 * SP_SIGN_ARM + 1)
        return 0;
    min = ((SZ_NUDGE_Y1 - SZ_NUDGE_Y0 + 1) * v->dh) / SZ_H;
    if (min < 2 * SP_SIGN_ARM + 1)
        return 0;
    return 1;
}

void side_paint(const struct menu_view *v, int side, int pressed_hit, int fader_v,
                int sync, int cue, int play)
{
    int fx0, fy0, fx1, fy1;
    unsigned int fill  = menu_pixel(v->bpp, MENU_FILL);
    unsigned int ink   = menu_pixel(v->bpp, MENU_LABEL);
    char title[8];

    if (!side_paint_ok(v))
        return;
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return;
    if (fader_v < 0)
        fader_v = 0;
    if (fader_v > 1023)
        fader_v = 1023;
    /* One bit each: the caller's values are booleans, and a value that is merely
     * TRUTHY must not change what is drawn (sp_button_x tests it with a ?:). */
    sync = sync ? 1 : 0;
    cue  = cue  ? 1 : 0;
    play = play ? 1 : 0;

    /* THE BED, AND NOTHING AROUND IT. *"you don't need a white border on the side swipe
     * menus"* (2026-10-06) -- so the panel's own edge is the black MENU_FILL and the
     * only outlines on the glass are the ones each control draws for itself
     * (sp_button/sp_button_x's sp_frame, the fader cap's, the well's). That is the same
     * shape the band and the USB STOP chooser were restyled to the day before, and it
     * is deliberately all four edges: a frame that survived on one side would be the
     * mirror showing through, which is the one thing this panel's drawing can get
     * wrong without any hit test noticing. */
    sp_rect(v, side, 0, 0, SZ_W - 1, SZ_H - 1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, fill);

    snprintf(title, sizeof title, "CH %d", side_channel(side));
    sp_text_center(v, side, SZ_TITLE_X0, SZ_TITLE_Y0, SZ_TITLE_X1, SZ_TITLE_Y1,
                   title, fill, ink);

    sp_button(v, side, SZ_SYNC_Y0, SZ_SYNC_Y1, "SYNC",
              pressed_hit == SZ_HIT_SYNC, sync);

    /* The nudge pair: two boxes sharing one row, each with its DRAWN mark rather than
     * a label. The '+' cell is the right one -- the operator's own order, left to
     * right, and the caller is what decides which of rbp's two bend directions each
     * means (side_zone.h's SZ_HIT_NUDGE_*). Neither has a state to show: rbp has no
     * LED for a nudge, and the bend is a thing you do and not a thing you are. */
    sp_button_x(v, side, SZ_NUDGE_M_X0, SZ_NUDGE_M_X1, SZ_NUDGE_Y0, SZ_NUDGE_Y1,
                "", pressed_hit == SZ_HIT_NUDGE_M, 0);
    sp_sign(v, side, SZ_NUDGE_M_X0, SZ_NUDGE_M_X1, SZ_NUDGE_Y0, SZ_NUDGE_Y1, 0,
            pressed_hit == SZ_HIT_NUDGE_M);
    sp_button_x(v, side, SZ_NUDGE_P_X0, SZ_NUDGE_P_X1, SZ_NUDGE_Y0, SZ_NUDGE_Y1,
                "", pressed_hit == SZ_HIT_NUDGE_P, 0);
    sp_sign(v, side, SZ_NUDGE_P_X0, SZ_NUDGE_P_X1, SZ_NUDGE_Y0, SZ_NUDGE_Y1, 1,
            pressed_hit == SZ_HIT_NUDGE_P);

    sp_readout(v, side, fader_v);
    sp_fader(v, side, fader_v, pressed_hit == SZ_HIT_FADER);

    sp_button(v, side, SZ_CUE_Y0, SZ_CUE_Y1, "CUE", pressed_hit == SZ_HIT_CUE, cue);
    sp_button(v, side, SZ_PLAY_Y0, SZ_PLAY_Y1, "PLAY", pressed_hit == SZ_HIT_PLAY, play);
}
