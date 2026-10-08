/*
 * menu_paint.c -- the top menu's image, as exact pixel writes.
 *
 * See menu_paint.h for why there is no save-under here and why the class of a
 * pixel is computed in framebuffer pixels. This file is the arithmetic: a layout
 * computed once from the view, and one pure classification of a pixel per class.
 */
#include "menu_paint.h"
#include "menu_zone.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, for the pixel cursor */

/* THE BUTTON'S OWN OUTLINE, AND IT REPLACED THE PANEL'S FRAME. The operator's ask,
 * 2026-10-06, on the band and then on the USB STOP chooser:
 *
 *     "the drop down doesn't need the white border around the endire thing, it only
 *      needs a thin white border around each button with some padding in between and
 *      a black background. model it this way for the USB stop menu as well"
 *
 * So the two pixels of MENU_BORDER that used to go round the whole bar now go round
 * each of the seven buttons instead, one pixel thick because the ask was for THIN,
 * and the bar's background is the black bed MENU_FILL paints.
 *
 * BOTH CONSTANTS LIVE IN menu_paint.h, because they are the arithmetic test_menu.c
 * pins its restyle checks against and a second copy here would be the drift the
 * header's own comments spend so long warning about. */

/* The padding between a column's edge and its button, in fb px -- so the black gap
 * between two neighbouring buttons is twice this less the single seam pixel the two
 * columns share, 7 px at 1280. This is what replaced the one-pixel MENU_DIV seam:
 * the operator asked for padding BETWEEN the buttons rather than a line drawn down
 * between them. The padding columns stay TAPPABLE -- menu_zone.c's hit test is the
 * full column and did not move -- which is deliberate: a 7 px gap is not a control
 * and a finger that lands in one is aiming at a button.
 *
 * The vertical counterpart is the band's own 8-row margin at each end (MZ_BTN_Y0),
 * so the buttons are 40 rows tall inside a 56-row panel and the black bed shows
 * above and below them. That margin was the frame's padding when there was a frame
 * and it reads as the same thing now, which is why nothing about the band moved. */

/* There is no LABEL margin constant, and its absence is deliberate: the old layout
 * picked a glyph scale by asking how much room a label needed with a margin on each
 * side, and the answer could be "too small to read". The labels are now set in the
 * device's own type at one fixed size (menu_font.h) and centred in their button, and
 * the only constraint left is the clip that keeps a label inside it -- which
 * menu_label_cov() enforces, and which menu_labels_fit() refuses a picture outright
 * for rather than letting it draw a truncated word. */

/* An 8-bit triple to this port's two framebuffer depths. The palette is written
 * in the triples rather than in magic numbers, so a colour can be read and changed
 * without a converter in the loop -- the numbers are the source of truth and the
 * shift arithmetic is the only thing that could be wrong, once. */
#define RGB565(r, g, b) ((unsigned int)((((r) & 0xf8) << 8) | \
                                        (((g) & 0xfc) << 3) | \
                                        (((b) & 0xf8) >> 3)))
#define XRGB8888(r, g, b) ((unsigned int)(((r) << 16) | ((g) << 8) | (b)))

/* An 8-bit triple to whichever depth this is, in one place: menu_pixel() and the
 * label blend both go through this, so a blended pixel and a solid one cannot be
 * encoded differently. */
#define MENU_TRIP(bpp, r, g, b) ((bpp) == 32 ? XRGB8888(r, g, b) : RGB565(r, g, b))

/* The layout: the panel rect, the MZ_COLS button rects, where each label starts and the
 * line box the labels sit in, all in fb px, all computed once per paint. Every rect
 * is inclusive at both ends, matching menu_zone.c's hit test.
 *
 * struct menu_layout itself lives in menu_paint.h now, because menu_draw.c's damage
 * witness classifies fifteen points of one image per tick and must not rebuild the
 * geometry for each of them. menu_layout_make() below is the only thing that fills it
 * in, and test_menu.c asserts menu_class_in() against menu_class_at() at every panel
 * pixel so the prepared-layout path cannot drift from the production one. */

/* The palette, as 8-bit triples. The depth is applied here and in menu_blend()
 * through the same macro, so a solid pixel and a blended one cannot come out of two
 * different encoders. An unknown depth is RGB565, which is what this port's panel
 * is -- menu_paint.h says why. */
unsigned int menu_pixel(int bpp, int cls)
{
    switch (cls) {
    /* BLACK, at the operator's ask of 2026-10-06 ("a black background"). It was a
     * near-black blue-grey, and the change reaches every surface that shares this
     * palette -- the two drawers and the window's bed as well as this bar and the USB
     * STOP chooser. That is the point of the palette being in one place: "a button"
     * and "a background" cannot come to mean two different things on two panels.
     *
     * IT IS ALSO THE ZERO PIXEL, in both depths, which menu_pixel(MENU_NONE) already
     * was. Nothing here reads a pixel to decide anything, so the collision costs
     * nothing -- but the damage witness's one background point now compares against a
     * value rbp's own UI can hold, which is why the other fourteen sample the white
     * button outline instead (menu_witness_point() below). */
    case MENU_FILL:          return MENU_TRIP(bpp, 0, 0, 0);      /* the black bed */
    case MENU_BORDER:        return MENU_TRIP(bpp, 200, 204, 210);/* a button's outline */
    case MENU_DIV:           return MENU_TRIP(bpp, 70, 74, 80);   /* not this bar's any more */
    case MENU_BTN:           return MENU_TRIP(bpp, 40, 44, 52);
    case MENU_BTN_PRESSED:   return MENU_TRIP(bpp, 0, 160, 255);  /* the accent */
    case MENU_LABEL:         return MENU_TRIP(bpp, 240, 242, 246);
    case MENU_LABEL_PRESSED: return MENU_TRIP(bpp, 0, 20, 40);    /* dark on the accent */
    /* A button that cannot be pressed: the same two colours pulled down far enough
     * that the ink is unmistakably present and unmistakably not a choice. The label
     * is the one that carries it -- a dim face with bright ink would read as a
     * rendering fault rather than as a refusal -- so BTN_OFF moves only a little off
     * MENU_BTN and LABEL_OFF moves most of the way to it. */
    case MENU_BTN_OFF:       return MENU_TRIP(bpp, 28, 30, 36);
    case MENU_LABEL_OFF:     return MENU_TRIP(bpp, 118, 122, 130);
    /* SAMPLED OFF RBP'S OWN BEAT FX PANEL, 2026-10-06, from a /dev/fb0 capture of the
     * live performance screen: the plate is (32,32,32) across 15 543 of its pixels, and
     * the CH SELECT value box -- the one thing on that panel rbp fills to mean
     * "selected" -- is (0,125,222) across 5 703. menu_paint.h says why they are here. */
    case MENU_FX_PLATE:      return MENU_TRIP(bpp, 32, 32, 32);
    case MENU_FX_SEL:        return MENU_TRIP(bpp, 0, 125, 222);
    default:                 return 0;
    }
}

/* A palette value back to the 8-bit triple it was made from, so the blend below can
 * be done where the arithmetic is obvious rather than in a packed-domain trick.
 * RGB565's channels expand by floor(v * 255 / 31) and floor(v * 255 / 63); the two
 * ends of a blend do not depend on this being a faithful round trip -- menu_blend()
 * returns the palette values themselves for coverage 0 and 255 -- only the edges in
 * between do, and there determinism is what is wanted. */
static void menu_trip_of(int bpp, unsigned int v, int *r, int *g, int *b)
{
    if (bpp == 32) {
        *r = (int)((v >> 16) & 0xffu);
        *g = (int)((v >> 8) & 0xffu);
        *b = (int)(v & 0xffu);
    } else {
        *r = (int)((v >> 11) & 0x1fu) * 255 / 31;
        *g = (int)((v >> 5) & 0x3fu) * 255 / 63;
        *b = (int)(v & 0x1fu) * 255 / 31;
    }
}

/* `ink` over `base` at coverage `cov`/255 -- what a partly covered glyph pixel gets.
 * Coverage 0 and 255 return `base` and `ink` themselves, so the panel's exactness
 * rule is enforced rather than hoped for: a glyph's edge is the only blended pixel
 * on the panel, and both ends of it are palette values.
 *
 * Per channel, in 8-bit, rounded to nearest. A pure integer function of its
 * arguments, which is what "painting it twice is painting it once" needs: two draws
 * of the same image cannot disagree about a pixel. */
static unsigned int menu_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;

    if (cov <= 0)
        return base;
    if (cov >= 255)
        return ink;
    menu_trip_of(bpp, base, &br, &bg, &bb);
    menu_trip_of(bpp, ink, &ir, &ig, &ib);
    return MENU_TRIP(bpp,
                     (br * (255 - cov) + ir * cov + 127) / 255,
                     (bg * (255 - cov) + ig * cov + 127) / 255,
                     (bb * (255 - cov) + ib * cov + 127) / 255);
}

/* The button band's rows, from the view alone. ONE implementation, because
 * menu_layout_make() centres the labels inside these rows and menu_witness_point()
 * picks its sample rows from these same edges -- two copies that drifted by a pixel
 * would put a witness point on a row the layout never drew, and the property test
 * would go on checking arithmetic nobody ran. */
static void menu_band_rows(const struct menu_view *v, int *by0, int *by1)
{
    *by0 = v->by + (MZ_BTN_Y0 * v->dh) / MZ_LOGICAL_H;
    *by1 = v->by + ((MZ_BTN_Y1 + 1) * v->dh) / MZ_LOGICAL_H - 1;
    if (*by1 < *by0)
        *by1 = *by0;
}

static int menu_band_h(const struct menu_view *v)
{
    int by0, by1;

    menu_band_rows(v, &by0, &by1);
    return by1 - by0 + 1;
}

/* Does every label fit the BUTTON it is centred in, at this panel width?
 *
 * The painter CLIPS at the button bound -- menu_label_cov() stops at the button's
 * inner rect -- so a button narrower than its label is not a squeeze, it is a word
 * with its end cut off: the widest label, "USB STOP", becomes "USB STO", in the one
 * band the operator reads most and by a defect that looks like nothing at all. A
 * picture too narrow for its own words is a picture this feature declines to draw
 * on, which is the same answer menu_view_ok() already gives a picture too short for
 * its line box; the caller logs the refusal and draws nothing.
 *
 * The bound is the button's INNER rect and not the column's: the padding and the
 * outline are drawn over the ends of the column (class_in_layout() below), so a
 * label that only just fitted the column would be painted down the white border.
 * The two are 2 * (MENU_BTN_PAD_PX + MENU_BTN_BORDER_PX) = 10 px apart at every
 * size, which is why the restyle moved the width floor up by that much.
 *
 * The bounds are menu_layout_make()'s own expressions for L->bx0/L->bx1, because a
 * second opinion about where a column is would be a check on arithmetic nobody ran.
 * This is what moves when the ATLAS grows: "USB STOP" is 65 px at 16 px and 76 at
 * 19, and a 480-px-wide picture gives 68-px columns, so the size-up costs that one
 * panel shape its band -- and keeps it on 640 wide (91 px columns, 81 px inner) and
 * up. */
static int menu_labels_fit(const struct menu_view *v)
{
    int i;

    for (i = 0; i < MZ_COLS; i++) {
        int x0 = v->bx + (menu_button_x0(i) * v->dw) / MZ_LOGICAL_W;
        int x1 = v->bx + ((menu_button_x1(i) + 1) * v->dw) / MZ_LOGICAL_W - 1;
        int inner = (x1 - x0 + 1) - 2 * (MENU_BTN_PAD_PX + MENU_BTN_BORDER_PX);

        if (inner < menu_text_width(menu_label(i + 1)))
            return 0;
    }
    return 1;
}

int menu_view_ok(const struct menu_view *v)
{
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
    /* Room for a button: its two paddings, its two outline pixels and something
     * inside them. The panel has no frame of its own any more (MENU_BTN_BORDER_PX),
     * so this is the narrowest a picture may be before the per-button rule is what
     * refuses it -- and menu_labels_fit() below refuses a far wider one, so this is a
     * floor under the arithmetic rather than the floor anyone meets. */
    if (v->dw < 2 * (MENU_BTN_PAD_PX + MENU_BTN_BORDER_PX) + 2)
        return 0;
    if ((MZ_PANEL_H * v->dh) / MZ_LOGICAL_H < 2 * MENU_BTN_BORDER_PX + 2)
        return 0;
    /* AND ROOM FOR THE LABELS. The button band has to host the font's line box,
     * because the damage witness samples the row one inside each edge of that band
     * (menu_witness_point()) -- the only rows that are inside the band and outside
     * the ink at every size, which is what makes its exact-value comparison valid.
     * A band shorter than the line box can only be had on a panel where nothing here
     * is legible, and drawing there would mean a witness that read "damaged" on
     * every tick: ten thousand repaints a second of a band rbp is drawing into, on
     * rbp's own render thread. So it is refused, and the caller logs what it refused
     * and draws nothing (menu_draw.c's menu_fb_open_unlocked()). */
    if (menu_band_h(v) < MENU_FONT_LINE)
        return 0;
    /* And room for the words themselves -- see menu_labels_fit(). This is the rule
     * the ATLAS is measured against rather than the panel: growing the font raises
     * the width it needs, so it is the widest label and not a constant that decides
     * how narrow a picture may be. */
    if (!menu_labels_fit(v))
        return 0;
    return 1;
}

void menu_panel_rect(const struct menu_view *v, int *x0, int *y0, int *x1, int *y1)
{
    int ph;

    if (x0) *x0 = v->bx;
    if (y0) *y0 = v->by;
    ph = (MZ_PANEL_H * v->dh) / MZ_LOGICAL_H;
    if (ph < 1) ph = 1;
    if (x1) *x1 = v->bx + v->dw - 1;
    if (y1) *y1 = v->by + ph - 1;
}

void menu_layout_make(const struct menu_view *v, struct menu_layout *L)
{
    int i, ph, bh, col_w, w;

    L->x0 = v->bx;
    L->y0 = v->by;
    ph = (MZ_PANEL_H * v->dh) / MZ_LOGICAL_H;
    if (ph < 1)
        ph = 1;
    L->x1 = v->bx + v->dw - 1;
    L->y1 = v->by + ph - 1;

    menu_band_rows(v, &L->by0, &L->by1);
    bh = L->by1 - L->by0 + 1;

    for (i = 0; i < MZ_COLS; i++) {
        const char *s = menu_label(i + 1);
        int n = 0;

        L->bx0[i] = v->bx + (menu_button_x0(i) * v->dw) / MZ_LOGICAL_W;
        L->bx1[i] = v->bx + ((menu_button_x1(i) + 1) * v->dw) / MZ_LOGICAL_W - 1;
        if (L->bx1[i] < L->bx0[i])
            L->bx1[i] = L->bx0[i];
        while (s && s[n])
            n++;
        L->ln[i] = n;
    }

    /* The line box, not the ink: MENU_FONT_LINE is the font's own ascent + descent
     * (menu_font.h), and the ink sits inside it at its own offsets. Centring the
     * LINE is what makes all the labels share a baseline -- centring each label's
     * ink box would put the ones with and without descenders at different heights,
     * and "TAG LIST" would ride a pixel higher than "BROWSE". */
    L->ly = L->by0 + (bh - MENU_FONT_LINE) / 2;
    /* A button band shorter than the line box can only happen on a panel small
     * enough that nothing here is legible anyway; keep the text inside the band
     * rather than letting the centring push its first row up into the fill. */
    if (L->ly < L->by0)
        L->ly = L->by0;
    for (i = 0; i < MZ_COLS; i++) {
        col_w = L->bx1[i] - L->bx0[i] + 1;
        w = menu_text_width(menu_label(i + 1));
        L->lx[i] = L->bx0[i] + (col_w - w) / 2;
        L->lx1[i] = L->lx[i] + w - 1;
    }

}


/* How much ink covers this framebuffer pixel of button i's label, 0..255.
 *
 * The pen walks the label's advances -- they are per glyph now, not one fixed cell
 * (menu_font.h), so which glyph a pixel belongs to is a walk and not a division --
 * and each pixel takes the atlas's coverage at the cell it lands on. The glyph's own
 * ink box is offset from the pen origin by its `left` and from the line box top by
 * its `top`, and menu_font_cov() answers 0 outside the box, so the "outside the ink"
 * cases need no test here.
 *
 * The button's own INNER rect is the outer bound -- inside the padding and inside
 * the one-pixel outline, so ink can never run down the white border. On a panel
 * narrow enough that not even the narrowest label fits, the centring would put the
 * text past the button and a label would spill into its neighbour's; clipping here
 * makes that a truncated word instead of two overlapping ones, and it is what makes
 * "no label pixel is outside its own button" true at every panel size rather than at
 * the sizes anyone happened to try. menu_labels_fit() is the other half: a picture
 * where this clip would have to truncate is refused outright, so the clip is the
 * guarantee and not the everyday path. */
static int menu_label_cov(const struct menu_layout *L, int i, int fx, int fy)
{
    const char *s = menu_label(i + 1);
    int dx, dy, k;

    if (!s || L->ln[i] <= 0)
        return 0;
    if (fx < L->bx0[i] + MENU_BTN_PAD_PX + MENU_BTN_BORDER_PX ||
        fx > L->bx1[i] - MENU_BTN_PAD_PX - MENU_BTN_BORDER_PX)
        return 0;
    dx = fx - L->lx[i];
    dy = fy - L->ly;
    if (dx < 0 || dy < 0 || dy >= MENU_FONT_LINE)
        return 0;
    for (k = 0; k < L->ln[i]; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)s[k]);
        int adv = menu_font_adv((unsigned char)s[k], (unsigned char)s[k + 1]);

        if (dx < adv)
            return menu_font_cov(g, dx - g->left, dy - g->top);
        dx -= adv;
    }
    return 0;
}

/* The class of one pixel, with the layout already in hand: the shape both
 * menu_class_at() and menu_paint() need, so the per-pixel path never rebuilds the
 * layout. `cov`, when not NULL, gets the label's coverage at that pixel -- which is
 * what menu_paint() blends with; the class alone cannot say how much ink is on a
 * pixel, and a glyph edge is a partial one.
 *
 * THIS IS THE ONLY PLACE `pressed` IS READ, and where it is read is what
 * menu_paint_cols() rests on: both pressed-dependent returns below come after the
 * column loop has decided which button a pixel belongs to, so a pixel's dependence on
 * `pressed` is confined to the pressed button's own column. */
static int class_in_layout(const struct menu_layout *L, int fx, int fy, int pressed,
                           int *cov)
{
    int i, btn = 0;

    if (cov)
        *cov = 0;
    if (fx < L->x0 || fx > L->x1 || fy < L->y0 || fy > L->y1)
        return MENU_NONE;
    /* Above and below the buttons is the bed. There is no frame here any more: the
     * white goes round each button instead, and the panel's own outline is nothing
     * (MENU_BTN_BORDER_PX). */
    if (fy < L->by0 || fy > L->by1)
        return MENU_FILL;
    for (i = 0; i < MZ_COLS; i++) {
        if (fx >= L->bx0[i] && fx <= L->bx1[i]) {
            btn = i + 1;
            break;
        }
    }
    /* No column claims this pixel. The seven tile the whole logical width, so inside
     * the band this cannot happen -- it is reached only through the rounding in
     * menu_layout_make() on a framebuffer narrower than the logical width, where the
     * scaled column rects can leave a pixel of fill between two of them. */
    if (!btn)
        return MENU_FILL;
    /* THE PADDING, which is what stands between two buttons now. It is the column's
     * own edge pixels, so it is painted by the same column that owned the seam, and
     * it is the bed colour -- which is what makes the gap read as a gap rather than
     * as a line drawn down it (MENU_BTN_PAD_PX). */
    if (fx < L->bx0[i] + MENU_BTN_PAD_PX || fx > L->bx1[i] - MENU_BTN_PAD_PX)
        return MENU_FILL;
    /* The button's outline, inside the padding on all four sides. It is tested
     * BEFORE the pressed class, so a finger on a button turns its face and its label
     * and leaves the outline white -- which is also what keeps every pixel a witness
     * samples independent of `pressed`, and so what keeps the witness's expected
     * value stable while a button is held. */
    if (fx == L->bx0[i] + MENU_BTN_PAD_PX || fx == L->bx1[i] - MENU_BTN_PAD_PX ||
        fy == L->by0 || fy == L->by1)
        return MENU_BORDER;
    /* The label band is the only place a glyph can be, and the guard is exact --
     * menu_label_cov() rejects everything outside it itself, including a label the
     * centring pushed past its button. Without it, EVERY pixel of the band pays the
     * per-glyph walk to be told it is not ink, and fewer than one in ten is ever
     * inside a label's box. */
    if (fy >= L->ly && fy < L->ly + MENU_FONT_LINE &&
        fx >= L->lx[i] && fx <= L->lx1[i]) {
        int c = menu_label_cov(L, i, fx, fy);

        if (c > 0) {
            if (cov)
                *cov = c;
            return (btn == pressed) ? MENU_LABEL_PRESSED : MENU_LABEL;
        }
    }
    return (btn == pressed) ? MENU_BTN_PRESSED : MENU_BTN;
}

int menu_class_at(const struct menu_view *v, int fx, int fy, int pressed)
{
    struct menu_layout L;

    if (!menu_view_ok(v))
        return MENU_NONE;
    menu_layout_make(v, &L);
    return class_in_layout(&L, fx, fy, pressed, NULL);
}

int menu_class_in(const struct menu_layout *L, int fx, int fy, int pressed)
{
    return class_in_layout(L, fx, fy, pressed, NULL);
}

/* THE SAMPLE POINTS THE DAMAGE WITNESS USES, and the restyle moved every one of
 * them. menu_draw.c explains the failure the mix guards against; what matters here
 * is that all of them are glyph-free AND are exact palette values, so the witness's
 * comparison against menu_pixel() is valid. test_menu.c asserts that rather than
 * trusting it.
 *
 * THEY USED TO BE FRACTIONS OF THE PANEL, four corners and two mid-border points of
 * the frame plus nine spread over the button band. The frame is gone (the white goes
 * round each button now) and so is the seam, so the six frame points would all have
 * landed on the BLACK BED -- which is the one colour the comment in menu_draw.c
 * warns about: a witness that sampled only the bed could be fooled for the life of
 * the session by rbp happening to draw black there, and black is a colour rbp's own
 * UI is full of.
 *
 * So fourteen of the fifteen points sample the BUTTON OUTLINES -- the top edge row
 * of each button and the bottom edge row of each button, at that button's own centre
 * x. White at fourteen spread-out points is a false "intact" rbp cannot produce, and
 * the outline is one pixel inside the band and outside the ink by the same argument
 * the old band rows used: the ink is centred in the button inside a MENU_FONT_LINE
 * line box and menu_view_ok() refuses a band too short to host it. The fifteenth
 * point samples the BED, in the padding between two buttons at a row one inside the
 * band's top -- so the background and the gap are witnessed too, and a witness point
 * there can only add a "damaged", never hide one.
 *
 * NONE OF THE FIFTEEN DEPENDS ON `pressed`: class_in_layout() tests the outline
 * before it tests the pressed class, and the bed is outside the columns' buttons
 * entirely. test_menu.c asserts both properties against this list at ten sizes. */
enum { WY_BTN_TOP = 0, WY_BTN_BOT, WY_BED };
/* Which button (0-based column) each point samples, and what of it. Seven top edges,
 * seven bottom edges, then the one bed point. */
static const unsigned char wcol[] = {
    0, 1, 2, 3, 4, 5, 6,
    0, 1, 2, 3, 4, 5, 6,
    3
};
static const unsigned char wkind[] = {
    WY_BTN_TOP, WY_BTN_TOP, WY_BTN_TOP, WY_BTN_TOP,
    WY_BTN_TOP, WY_BTN_TOP, WY_BTN_TOP,
    WY_BTN_BOT, WY_BTN_BOT, WY_BTN_BOT, WY_BTN_BOT,
    WY_BTN_BOT, WY_BTN_BOT, WY_BTN_BOT,
    WY_BED
};

int menu_witness_count(void)
{
    return (int)(sizeof wcol / sizeof wcol[0]);
}

/* A column's centre x in framebuffer pixels, in the same expression
 * menu_layout_make() uses for the column's own bounds -- the centre of a rect is not
 * an edge, so scaling the logical centre instead of the two edges cannot disagree
 * with the layout by more than the rounding, and a button is 175 px wide at 1280. */
static int menu_col_centre(const struct menu_view *v, int i)
{
    int cxl = (menu_button_x0(i) + menu_button_x1(i) + 1) / 2;

    return v->bx + (cxl * v->dw) / MZ_LOGICAL_W;
}

void menu_witness_point(const struct menu_view *v, int i, int *fx, int *fy)
{
    int x0, y0, x1, y1, by0, by1, y, col;

    menu_panel_rect(v, &x0, &y0, &x1, &y1);
    menu_band_rows(v, &by0, &by1);
    if (i < 0 || i >= menu_witness_count()) {
        if (fx) *fx = x0;
        if (fy) *fy = y0;
        return;
    }
    col = wcol[i];
    switch (wkind[i]) {
    case WY_BTN_BOT: y = by1;      break;
    case WY_BED:     y = by0 + 1;  break;
    case WY_BTN_TOP:
    default:         y = by0;      break;
    }
    /* The clamps keep every point inside the panel even for a view the caller should
     * have refused -- and inside the band's own rows for the two that name them. */
    if (y < by0) y = by0;
    if (y > by1) y = by1;
    if (y < y0) y = y0;
    if (y > y1) y = y1;
    /* The bed point is the SEAM between columns 2 and 3 -- the first pixel of the
     * fourth column, by the same expression menu_layout_make() gives L->bx0[3], which
     * that column's own padding rule has already turned to bed (class_in_layout()).
     * The other fourteen are their column's centre, which is inside that column's
     * button at every size: the button is the column less MENU_BTN_PAD_PX at each end
     * and the padding is under half a column. */
    if (fx) *fx = (wkind[i] == WY_BED)
                   ? v->bx + (menu_button_x0(col) * v->dw) / MZ_LOGICAL_W
                   : menu_col_centre(v, col);
    if (fy) *fy = y;
    /* ...and keep x inside the panel too, for the same reason the rows are clamped. */
    if (fx) {
        if (*fx < x0) *fx = x0;
        if (*fx > x1) *fx = x1;
    }
}

/* The value one pixel gets. Split out of menu_paint() so that the loop and
 * menu_value_at() are the same arithmetic -- the test compares the page against
 * this function, which is only worth anything if this function is what paints. */
static unsigned int pixel_value(const struct menu_view *v, const struct menu_layout *L,
                                const unsigned int *pal, int fx, int fy, int pressed)
{
    int cov = 0;
    int cls = class_in_layout(L, fx, fy, pressed, &cov);

    /* A glyph edge, and only a glyph edge: the label colour over the button colour,
     * in the label's own pressed or unpressed form. Coverage 255 (and every
     * non-label class) takes pal[cls] and skips this, which is what keeps the ends
     * of the coverage range exact palette values. */
    if (cov > 0 && cov < 255) {
        if (cls == MENU_LABEL)
            return menu_blend(v->bpp, pal[MENU_BTN], pal[MENU_LABEL], cov);
        if (cls == MENU_LABEL_PRESSED)
            return menu_blend(v->bpp, pal[MENU_BTN_PRESSED],
                              pal[MENU_LABEL_PRESSED], cov);
    }
    return pal[cls];
}

unsigned int menu_value_at(const struct menu_view *v, int fx, int fy, int pressed)
{
    struct menu_layout L;
    unsigned int pal[8];
    int i;

    if (!menu_view_ok(v))
        return 0;
    menu_layout_make(v, &L);
    for (i = 0; i < 8; i++)
        pal[i] = menu_pixel(v->bpp, i);
    return pixel_value(v, &L, pal, fx, fy, pressed);
}

/* The same read and write cursor_paint.c uses, indexed in PIXELS (y * pitch + x)
 * with pitch the row stride in pixels -- so this works on a panel whose stride is
 * not exactly its width, which is the case this port ships (2560 bytes = 1280
 * pixels at 16 bpp). Duplicated rather than shared because cursor_paint.c's are
 * static and a header of two four-line functions is not worth a module. */
static unsigned int px_get(const void *pix, int pitch, int bpp, int x, int y)
{
    if (bpp == 32) {
        const unsigned int *p = (const unsigned int *)pix;
        return p[(size_t)y * pitch + x];
    }
    {
        const unsigned short *p = (const unsigned short *)pix;
        return p[(size_t)y * pitch + x];
    }
}

static void px_set(void *pix, int pitch, int bpp, int x, int y, unsigned int v)
{
    if (bpp == 32) {
        unsigned int *p = (unsigned int *)pix;
        p[(size_t)y * pitch + x] = v;
    } else {
        unsigned short *p = (unsigned short *)pix;
        p[(size_t)y * pitch + x] = (unsigned short)v;
    }
}

unsigned int menu_get(const struct menu_view *v, int fx, int fy)
{
    if (!v || !v->pix || fx < 0 || fy < 0 || fx >= v->fb_w || fy >= v->fb_h)
        return 0;
    return px_get(v->pix, v->pitch, v->bpp, fx, fy);
}

/* BLIND WRITES, and the read this loop used to do is gone. The first version read
 * each cell and wrote it only when it differed -- fb_cursor.c's rule, and it reads
 * as free because the comparison is cheap. Measured on the unit on 2026-09-29 it
 * was the opposite of free: passes took 26,496, 32,566, 32,845 and 29,341 us, i.e.
 * two whole frames. The comparison is cheap; the READ is not. work/fbwrite.c
 * measures the difference directly on this glass -- 143,360 16-bit reads from
 * /dev/fb0 (the 112-row band; it is 71,680 now) take 27,192 us (189 ns a cell, an
 * uncached load stalling for its full latency) while the same cells in 16-bit
 * stores take 178 us (1 ns, posted and merged). Writing every cell takes that read
 * out of the loop and the pass with
 * it, and the image is identical either way: menu_paint.h's "painting it twice is
 * painting it once" is a property of the values written, not of the read.
 *
 * THOSE FIGURES ARE THE 112-ROW BAND. The panel was halved to 56 rows on
 * 2026-09-29, so the cell counts and byte counts here are historical; the per-cell
 * costs carry over, because what this paragraph rests on (an uncached read costs
 * ~190x a store on this mapping) does not depend on how many of them there are.
 *
 * WHAT IS LEFT IS THE CLASSIFICATION, not the memory -- the same benchmark's
 * cached-stores control says a 143,360-pixel loop costs 209 us, and this pass
 * still costs ~7 ms, so ~44 ns a cell is class_in_layout()'s branches and
 * mispredicts. That is why menu_draw.c caches the rendered image and blits it: the
 * steady state is one band-sized memcpy per rbp frame (286 KB in 164 us, taken at
 * the 112-row band; it is 143 KB now), and this loop runs only when the picture
 * actually changes -- the panel opening, or a different button under the finger.
 *
 * THE LABEL BLEND is why pixel_value() asks class_in_layout() for the coverage as
 * well as the class. It is one extra int out through a pointer that is usually
 * ignored, and it keeps the per-pixel path to a single walk of the label -- asking
 * twice would walk the advances twice for every pixel of a label band that is mostly
 * not ink.
 *
 * COLUMNS, NOT ROWS, and that is the whole difference between this and the loop it
 * replaced. A masked paint is what makes an incremental redraw possible -- see
 * menu_paint.h's menu_paint_cols() for the invariant it rests on -- and the mask is
 * applied by walking the MZ_COLS columns instead of the panel's rows. With every column
 * set the two cover exactly the same pixels (the columns tile x0..x1 with no gap and
 * no overlap), and test_menu.c asserts that rather than trusting it. */
void menu_paint_cols(const struct menu_view *v, int pressed, unsigned int mask)
{
    struct menu_layout L;
    unsigned int pal[8];
    int fx, fy, i;

    if (!menu_view_ok(v))
        return;
    menu_layout_make(v, &L);

    /* The palette is a pure function of (bpp, class); resolving it once per paint
     * keeps a switch out of the per-pixel path. */
    for (i = 0; i < 8; i++)
        pal[i] = menu_pixel(v->bpp, i);

    mask &= (1u << MZ_COLS) - 1u;

    /* One iteration per cell, and the cells are the seven labelled columns; one loop
     * rather than a second pass so a column cannot be painted twice or missed by a
     * mask that names it. */
    for (i = 0; i < MZ_COLS; i++) {
        int cx0, cx1;

        if (!(mask & (1u << i)))
            continue;
        cx0 = L.bx0[i];
        cx1 = L.bx1[i];
        for (fy = L.y0; fy <= L.y1; fy++)
            for (fx = cx0; fx <= cx1; fx++)
                px_set(v->pix, v->pitch, v->bpp, fx, fy,
                       pixel_value(v, &L, pal, fx, fy, pressed));
    }
}

void menu_paint(const struct menu_view *v, int pressed)
{
    menu_paint_cols(v, pressed, (1u << MZ_COLS) - 1u);
}
