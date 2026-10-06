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

/* The frame around the bar, in fb px. Two pixels reads as a frame at any size this
 * port runs at without eating a meaningful part of a small panel. */
#define MENU_BORDER_PX 2

/* There is no padding constant any more, and its absence is deliberate: the old
 * layout picked a glyph scale by asking how much room a label needed with a margin
 * on each side, and the answer could be "too small to read". The labels are now set
 * in the device's own type at one fixed size (menu_font.h) and centred in their
 * column, and the only constraint left is the clip that keeps a label inside its own
 * button -- which menu_label_cov() enforces on panels too small to hold it. */

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
    case MENU_FILL:          return MENU_TRIP(bpp, 20, 22, 26);   /* near-black blue-grey */
    case MENU_BORDER:        return MENU_TRIP(bpp, 200, 204, 210);
    case MENU_DIV:           return MENU_TRIP(bpp, 70, 74, 80);
    case MENU_BTN:           return MENU_TRIP(bpp, 40, 44, 52);
    case MENU_BTN_PRESSED:   return MENU_TRIP(bpp, 0, 160, 255);  /* the accent */
    case MENU_LABEL:         return MENU_TRIP(bpp, 240, 242, 246);
    case MENU_LABEL_PRESSED: return MENU_TRIP(bpp, 0, 20, 40);    /* dark on the accent */
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
    /* Room for the frame and something inside it. */
    if (v->dw < 2 * MENU_BORDER_PX + 2)
        return 0;
    if ((MZ_PANEL_H * v->dh) / MZ_LOGICAL_H < 2 * MENU_BORDER_PX + 2)
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
 * The button's own x range is the outer bound. On a panel narrow enough that not
 * even the narrowest label fits, the centring would put the text past the column and
 * a label would spill into its neighbour's; clipping here makes that a truncated
 * word instead of two overlapping ones, and it is what makes "no label pixel is
 * outside its own button" true at every panel size rather than at the sizes anyone
 * happened to try. */
static int menu_label_cov(const struct menu_layout *L, int i, int fx, int fy)
{
    const char *s = menu_label(i + 1);
    int dx, dy, k;

    if (!s || L->ln[i] <= 0)
        return 0;
    if (fx < L->bx0[i] || fx > L->bx1[i])
        return 0;
    dx = fx - L->lx[i];
    dy = fy - L->ly;
    if (dx < 0 || dy < 0 || dy >= MENU_FONT_LINE)
        return 0;
    for (k = 0; k < L->ln[i]; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)s[k]);

        if (dx < (int)g->adv)
            return menu_font_cov(g, dx - g->left, dy - g->top);
        dx -= g->adv;
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
    if (fx < L->x0 + MENU_BORDER_PX || fx > L->x1 - MENU_BORDER_PX ||
        fy < L->y0 + MENU_BORDER_PX || fy > L->y1 - MENU_BORDER_PX)
        return MENU_BORDER;
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
    /* The seam belongs to the column on its right, inside that column's own hit
     * rectangle -- the columns tile with no gap, so a divider drawn between them
     * would have to take a pixel from one of them anyway. One pixel of a 182 px
     * target is not a target the operator can miss. */
    if (i > 0 && fx == L->bx0[i])
        return MENU_DIV;
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

/* The sample points the damage witness uses, as panel-relative fractions in
 * sixteenths. The mix is deliberate and menu_draw.c explains the failure each part
 * of it guards against; what matters here is that all of them are glyph-free, so
 * menu_value_at() at one of them is an exact palette value and a witness comparison
 * against menu_pixel() is valid. test_menu.c asserts that rather than trusting it.
 *
 * The six frame points are corners and border midpoints; the nine band points are on
 * the column centres and the seams, at 5/16 and 11/16 of the panel's height -- above
 * and below the label ink, which sits where menu_layout_make() puts it. */
static const unsigned char wpx16[] = {
    0, 16,  0, 16,  8,  8,
    1,  4,  7,  9, 12, 15,
    1,  8, 15
};
/* WHICH ROW, and this is not a fraction of the panel any more. It was 0/16 for the
 * frame and 5/16, 11/16 for the band, which was safe at the 112-row panel the
 * feature was built for and stopped being safe the moment the operator asked for
 * half the height: the band is a PROPORTION of the panel while the label is a fixed
 * 20-row line box, so halving the panel took the button band from 96 rows to 40 at
 * 1280x800 and to 16 rows at 480x320 -- where the ink fills the band and both band
 * fractions land on glyphs. That is a witness that reads "damaged" on every tick,
 * i.e. a repaint storm, and it would have shipped silently.
 *
 * So the band's rows are the band's own EDGES, one row inside: always inside the
 * band (so a clobbered band still reads damaged), and always outside the ink,
 * because the ink is centred in the band inside a MENU_FONT_LINE-tall line box and
 * menu_view_ok() refuses a band too short to host that box. The property is now
 * structural -- there is no panel size at which a witness point can be a blend --
 * and test_menu.c asserts it against this list at ten sizes rather than trusting
 * this paragraph. */
enum { WY_PANEL_TOP = 0, WY_PANEL_BOT, WY_BAND_TOP, WY_BAND_BOT };
static const unsigned char wpy[] = {
    WY_PANEL_TOP, WY_PANEL_TOP, WY_PANEL_BOT, WY_PANEL_BOT,
    WY_PANEL_TOP, WY_PANEL_BOT,
    WY_BAND_TOP,  WY_BAND_TOP,  WY_BAND_TOP,  WY_BAND_TOP,  WY_BAND_TOP,
    WY_BAND_TOP,
    WY_BAND_BOT,  WY_BAND_BOT,  WY_BAND_BOT
};

int menu_witness_count(void)
{
    return (int)(sizeof wpx16 / sizeof wpx16[0]);
}

void menu_witness_point(const struct menu_view *v, int i, int *fx, int *fy)
{
    int x0, y0, x1, y1, by0, by1, y;

    menu_panel_rect(v, &x0, &y0, &x1, &y1);
    menu_band_rows(v, &by0, &by1);
    if (i < 0 || i >= menu_witness_count()) {
        if (fx) *fx = x0;
        if (fy) *fy = y0;
        return;
    }
    switch (wpy[i]) {
    case WY_PANEL_BOT:  y = y1;      break;
    case WY_BAND_TOP:   y = by0 + 1; break;
    case WY_BAND_BOT:   y = by1 - 1; break;
    case WY_PANEL_TOP:
    default:            y = y0;      break;
    }
    /* A band of one row has by0 + 1 == by1 - 1 == by1; the clamps keep every point
     * inside the panel even for a view the caller should have refused. */
    if (y < y0) y = y0;
    if (y > y1) y = y1;
    if (fx) *fx = x0 + (x1 - x0) * wpx16[i] / 16;
    if (fy) *fy = y;
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
