/*
 * test_menu.c -- the top menu's unit test: no Pi, no device, no rbp, no screen.
 *
 * It links the PRODUCTION menu_zone.c, menu_paint.c and menu_font.h, for the
 * reason test_cursor.c links the production glyph: the rules pinned here are
 * exactly the kind that get quietly rewritten in a copy. Its surface is a local
 * array -- there is no framebuffer, no thread and no device anywhere in this file.
 *
 * Four things are pinned, and each has a failure it is standing in front of.
 *
 * 1. The geometry. The seven columns have to tile 1280 logical px with no gap (a
 *    gap is a dead seam under the operator's finger) and no overlap (an overlap
 *    makes the hit test depend on which check ran first). Both ends inclusive,
 *    because an off-by-one here moves every button one pixel and nothing would
 *    ever say so.
 *
 * 2. THE MIRROR. pointsrc.c reflects x at the wire (1279 - x) *after* this module
 *    has spoken, so the module is fed logical, pre-reflection points -- the rule
 *    touch_zone.h:36-40 states for the same reason. Get it wrong and every tap
 *    lands on the button mirroring the one the operator aimed at: a fault that
 *    reads as "the buttons are all swapped" and is invisible in any test that only
 *    ever feeds one side of the screen. Also checked here: the QUANTIZE box's
 *    point (55,755) and its mirror (1224,755) both hit nothing, so the menu can
 *    never steal the deck tap that test_point.c pins.
 *
 * 3. The gesture, clause by clause, including the two latching invariants -- a
 *    press that starts in the strip is swallowed for its whole life, and a press
 *    that starts outside is never touched even when it wanders in. The latch is
 *    what keeps rbp's touch stream balanced: tscfake_emit() dedups and tracks its
 *    own last_down, so a lone swallowed release would strand rbp's touch thread in
 *    the down state.
 *
 * 4. The image. That painting leaves no pixel of the panel unwritten (the whole
 *    scheme rests on it -- the panel is drawn over whatever rbp has there), that it
 *    writes nothing OUTSIDE the panel rect (rbp's screen is not ours to scribble
 *    on), that a second identical paint changes nothing, that the pressed image
 *    differs only inside the pressed button, and that no label pixel ever lands
 *    outside its own column -- at every panel size, not just at 1280x800.
 *
 * 5. The press-and-hold, which is the button's SECOND meaning and the only two
 *    things in this module that look at how long a finger was down: menu_hold_fires()
 *    and menu_hold_pending(), and the two constants the first splits, against the
 *    measurement they were chosen from (rbp's own timer accepts a hold between 300
 *    and 400 ms). The failure this stands in front of is a retune: drop the
 *    synthesized key under rbp's threshold and a long press on MENU quietly stops
 *    being UTILITY, with nothing else in the tree to say so. Also pinned: the gesture
 *    a hold ends on is the gesture a tap ends on, so a long finger cannot fire a
 *    *different* button or survive the slide-off-to-cancel rule -- and, for the
 *    mid-press answer, that it closes the panel at the threshold rather than at the
 *    lift, that it fires the button only once, and that the release still to come is
 *    the silent swallow it has always been.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_menu test_menu.c menu_zone.c
 *            menu_paint.c   &&   qemu-arm ./test_menu
 */
#define _GNU_SOURCE
#include "menu_zone.h"
#include "menu_paint.h"
#include "menu_font.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* A value no class in the palette can produce, so a pixel still holding it was
 * never written. 0xa5a5 is a mid grey-magenta in RGB565 and is not in the
 * palette; the 32bpp run uses its own sentinel below. */
#define SENTINEL16 0xa5a5u
#define SENTINEL32 0xa5a5a5a5u

/* The biggest panel any test here asks about, 16 bpp, one flat array with pitch =
 * width. 1920x1080 is 4 MB of .bss, which is nothing for a host run. */
#define FB_MAX_W 1920
#define FB_MAX_H 1080
static unsigned short fb16[FB_MAX_W * FB_MAX_H];

/* One panel's worth of image at the largest size above -- what the incremental
 * repaint test renders into. The panel is MZ_PANEL_H logical rows of MZ_LOGICAL_H, so
 * at 1080 rows it is 78 (151 when the panel was 112 rows). */
#define IMG_MAX (FB_MAX_W * ((MZ_PANEL_H * FB_MAX_H) / MZ_LOGICAL_H))

/* A logical row inside the button band, DERIVED, and used by every test that aims
 * at a button. It was the literal 50 -- and on 2026-09-29 that literal cost thirty
 * checks at once: 50 is inside the band only while the band is 96 rows tall, so
 * when the operator asked for half the height (MZ_BTN_Y1 103 -> 47) each of those
 * tests aimed below the band and read "not a button". A literal cannot track the
 * geometry it is a row of; the band's own middle can. */
#define BTN_ROW ((MZ_BTN_Y0 + MZ_BTN_Y1) / 2)

/* The panel sizes these tests walk, and which of them this port is actually run on.
 * `usable` is not a preference, it is the answer menu_view_ok() must give -- and it
 * is ASSERTED rather than assumed, because a real panel that silently stops being
 * usable is a bar that silently stops being drawn: menu_draw.c logs "leaves no room
 * for the panel" and draws nothing at all, which a test that simply skipped the
 * refused sizes could never see.
 *
 * 480x320 is the entry that moved. It was usable while the panel was 112 rows; at
 * 56 it is 22 rows there, the button band 16, and the witness's band rows would
 * land on label ink -- so the size is refused, and what it is refused FOR is the
 * rule that keeps the witness off the glyphs (menu_paint.c's menu_view_ok()). */
struct tsize { int w, h, usable; };
static const struct tsize TSIZES[] = {
    { 1280, 800, 1 }, { 1280, 720, 1 }, { 1024, 768, 1 }, { 1024, 600, 1 },
    {  800, 480, 1 }, {  800, 600, 1 }, {  640, 480, 1 }, {  480, 320, 0 },
    { 1920, 1080, 1 }, { 1366, 768, 1 }
};
#define TSIZE_N ((int)(sizeof TSIZES / sizeof TSIZES[0]))

static struct menu_view mkview(int fw, int fh)
{
    struct menu_view v;

    memset(&v, 0, sizeof v);
    v.pix = fb16;
    v.pitch = fw;
    v.fb_w = fw;
    v.fb_h = fh;
    v.bpp = 16;
    v.bx = 0;
    v.by = 0;
    v.dw = fw;
    v.dh = fh;
    return v;
}

static void fill16(int fw, int fh, unsigned short v)
{
    int i;

    for (i = 0; i < fw * fh; i++)
        fb16[i] = v;
}

/* A view over any buffer, so a test can render two images side by side in memory
 * and diff them -- which is the only way to pin an INCREMENTAL redraw: what it has
 * to produce is an image, and the image it has to match is another drawing. */
static struct menu_view mkview_pix(int fw, int fh, void *pix)
{
    struct menu_view v = mkview(fw, fh);

    v.pix = pix;
    return v;
}

/* Which button a panel pixel belongs to, asked through the production classifier
 * rather than through a copy of the column arithmetic: a pixel belongs to button i
 * exactly when classifying with i pressed reports it as pressed. Returns 0 for a
 * pixel that belongs to no button (fill, border, seam, or outside). */
/* WHICH CELL OWNS THIS PIXEL, asked the only way that cannot be fooled: paint the
 * pixel as if each cell in turn were pressed and see which one changes it. That is
 * what makes it a test of the drawing rather than a copy of the geometry. The loop is
 * the seven labelled columns because they are the whole panel's width again -- when
 * the menu had an eighth cell its mark was ink and had to be asked too, or every pixel
 * of it would have been reported as belonging to no cell at all. */
static int owner_of(const struct menu_view *v, int fx, int fy)
{
    int i;

    for (i = 0; i < MZ_COLS; i++) {
        int c = menu_class_at(v, fx, fy, i + 1);
        if (c == MENU_BTN_PRESSED || c == MENU_LABEL_PRESSED)
            return i + 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 1 + 2. Geometry and the mirror.
 * ------------------------------------------------------------------------- */
static void test_geometry(void)
{
    int i, x;

    CHECK(menu_button_x0(0) == 0, "column 0 does not start at 0 (got %d)",
          menu_button_x0(0));
    /* THE SEVEN REACH THE PANEL'S EDGE AGAIN, because the menu's eighth cell went with
     * the browser (menu_zone.h's block). The panel's right edge is the LAST COLUMN's
     * now, and that is what this asserts -- a menu that still stopped the columns at
     * 1232 would leave a 48 px dead strip where the globe used to be, and the operator
     * would see it as a gap rather than as a bug. */
    CHECK(menu_button_x1(MZ_COLS - 1) == MZ_LOGICAL_W - 1,
          "the last column does not end at %d (got %d)", MZ_LOGICAL_W - 1,
          menu_button_x1(MZ_COLS - 1));
    /* ...and the widths are NOT all equal, which is worth pinning rather than
     * glossing: 1280 does not divide by 7, so menu_button_x0/x1 give 182, 184...184,
     * 183. The tiling below is the property that matters; a test that asserted
     * MZ_BTN_W % MZ_COLS == 0 as well would be asserting something untrue. */

    /* Tile exactly: each column starts one past the last one's end, and the widths
     * add up to MZ_BTN_W. */
    for (i = 0; i < MZ_COLS; i++) {
        int w = menu_button_x1(i) - menu_button_x0(i) + 1;

        CHECK(w > 0, "column %d is empty", i);
        if (i > 0)
            CHECK(menu_button_x0(i) == menu_button_x1(i - 1) + 1,
                  "column %d starts at %d, one past column %d's end at %d",
                  i, menu_button_x0(i), i - 1, menu_button_x1(i - 1));
    }
    /* THE COLUMNS COVER THE WHOLE BAND, spelled off MZ_COLS rather than off a number.
     * This is the canary for "the column count changed and nothing else was updated":
     * the total above is 1280 at seven columns, 1280 at six and 1280 at eight, but the
     * pair of bounds that produce it move with the count, so a stale literal -- 1279
     * from the six-column band, or 1231 from the retile that made room for the globe --
     * fails here.
     *
     * AND THEY ARE NOT ALL THE SAME WIDTH, which is the other half of the canary: 1280
     * does not divide by 7, so they are 182, 184, 184, 184, 184, 184, 183. An exact
     * tiling is what the seam rule in class_in_layout() and the masked repaint in
     * menu_paint_cols() both rest on; equal widths would be nicer and are not on offer
     * at this width, and a test that asked for them would be asking for a lie. */
    CHECK(menu_button_x0(0) == 0 && menu_button_x1(MZ_COLS - 1) == MZ_LOGICAL_W - 1 &&
          menu_button_x1(MZ_COLS - 1) - menu_button_x0(0) + 1 == MZ_BTN_W,
          "the columns span %d..%d, not 0..%d",
          menu_button_x0(0), menu_button_x1(MZ_COLS - 1), MZ_BTN_W - 1);
    {
        int lo = menu_button_x1(0) - menu_button_x0(0) + 1;
        int hi = lo;

        for (i = 1; i < MZ_COLS; i++) {
            int w = menu_button_x1(i) - menu_button_x0(i) + 1;

            if (w < lo) lo = w;
            if (w > hi) hi = w;
        }
        CHECK(hi - lo <= 1, "the columns run from %d to %d px", lo, hi);
    }

    /* Inclusive at both ends, and one pixel outside on each side is the neighbour
     * -- not a gap, and not this column. */
    for (i = 0; i < MZ_COLS; i++) {
        CHECK(menu_button_at(menu_button_x0(i), BTN_ROW) == i + 1,
              "the left edge of column %d is not in it", i);
        CHECK(menu_button_at(menu_button_x1(i), BTN_ROW) == i + 1,
              "the right edge of column %d is not in it", i);
        if (i > 0)
            CHECK(menu_button_at(menu_button_x0(i) - 1, BTN_ROW) == i,
                  "the pixel left of column %d is not column %d", i, i - 1);
        if (i < MZ_COLS - 1)
            CHECK(menu_button_at(menu_button_x1(i) + 1, BTN_ROW) == i + 2,
                  "the pixel right of column %d is not column %d", i, i + 2);
    }
    CHECK(menu_button_at(-1, BTN_ROW) == 0, "x=-1 hit a button");
    CHECK(menu_button_at(MZ_LOGICAL_W, BTN_ROW) == 0, "x=1280 hit a button");

    /* The button band's own edges. */
    for (x = 0; x < MZ_LOGICAL_W; x += 97) {
        CHECK(menu_button_at(x, MZ_BTN_Y0) != 0, "the band's top row is dead");
        CHECK(menu_button_at(x, MZ_BTN_Y1) != 0, "the band's bottom row is dead");
        CHECK(menu_button_at(x, MZ_BTN_Y0 - 1) == 0, "a pixel above the band hit");
        CHECK(menu_button_at(x, MZ_BTN_Y1 + 1) == 0, "a pixel below the band hit");
    }

    /* THE MIRROR. A logical point on the left is the operator's left, and the
     * reflection happens after this module. Aim at column 1 and its mirror is
     * column 6 -- not column 1. */
    CHECK(menu_button_at(100, BTN_ROW) == 1, "logical x=100 is not column 1");
    CHECK(menu_button_at(MZ_LOGICAL_W - 1 - 100, BTN_ROW) == MZ_COLS,
          "the mirror of column 1 at x=100 is not column %d", MZ_COLS);
    CHECK(menu_button_at(300, BTN_ROW) == 2, "logical x=300 is not column 2");
    CHECK(menu_button_at(MZ_LOGICAL_W - 1 - 300, BTN_ROW) == MZ_COLS - 1,
          "the mirror of column 2 is not column %d", MZ_COLS - 1);

    /* The deck QUANTIZE box's point, its mirror, and the strip above it: the menu
     * must hit nothing at any of them, or it would steal the tap that
     * test_point.c's test_quantize_zone() pins. */
    CHECK(menu_button_at(55, 755) == 0, "the QUANTIZE point hit a menu button");
    CHECK(menu_button_at(MZ_LOGICAL_W - 1 - 55, 755) == 0,
          "the QUANTIZE point's mirror hit a menu button");
    CHECK(menu_button_at(695, 755) == 0, "deck 2's QUANTIZE point hit a button");

    /* Labels: all of them, distinct, non-empty, and in the font. */
    for (i = 1; i <= MZ_COLS; i++) {
        const char *s = menu_label(i);
        int j, n = 0;

        CHECK(s != NULL, "button %d has no label", i);
        if (!s)
            continue;
        while (s[n])
            n++;
        CHECK(n > 0, "button %d's label is empty", i);
        for (j = 0; j < n; j++) {
            CHECK(s[j] == ' ' || menu_font_index((unsigned char)s[j]) >= 0,
                  "button %d's label has a character the font cannot draw: '%c'",
                  i, s[j]);
            CHECK(!(s[j] >= 'a' && s[j] <= 'z'),
                  "button %d's label is lowercase: the panel is set in capitals, and"
                  " the atlas has carried lowercase since the window's keyboard"
                  " needed it, so this is a style pin now and no longer a font"
                  " limitation", i);
        }
        for (j = 1; j < i; j++)
            CHECK(strcmp(menu_label(j), s) != 0, "buttons %d and %d share a label",
                  j, i);
    }
    CHECK(menu_label(0) == NULL && menu_label(MZ_COLS + 1) == NULL,
          "menu_label() answered outside 1..%d", MZ_COLS);

    /* Out-of-range columns clamp rather than reading past the table. */
    CHECK(menu_button_x0(-3) == menu_button_x0(0), "x0(-3) did not clamp");
    CHECK(menu_button_x1(99) == menu_button_x1(MZ_COLS - 1), "x1(99) did not clamp");
}

/* ---------------------------------------------------------------------------
 * The font itself.
 *
 * The table stops being a hand-written bitmap here and becomes the device's own
 * typeface, rasterised at build time (menu_font.h is generated by
 * work/bake_menu_font.py). What the tests can still pin without re-rasterising it:
 * that the table is well formed, that every character any label uses has ink, that
 * the advances are the ones the layout will centre with, and -- the one with teeth
 * -- that no glyph's ink leaves MENU_FONT_INK_TOP..MENU_FONT_INK_BOT, because the
 * damage witness's sample rows are chosen against that band.
 *
 * The set is no longer just the panel's labels. Lowercase and ten punctuation marks
 * were appended on 2026-10-04 for the browser window's URL bar and its popup
 * keyboard, and two of the pins below moved with them: 'Z' is no longer the last
 * glyph and 'a' is no longer absent. The append-only rule is what keeps the rest of
 * this file -- and the operator's seven felt buttons -- untouched, and the keyboard's
 * own alphabet is now pinned here too, so a key the atlas cannot draw is caught on
 * the host rather than found as a blank cap on the glass.
 * ------------------------------------------------------------------------- */
static void test_font(void)
{
    int i, j, k;

    CHECK(menu_font_selfcheck(),
          "the font table is malformed: a metrics run does not match the atlas");

    /* The glyph metrics the label layout depends on. */
    CHECK(menu_text_width("") == 0, "the empty string has a width");
    CHECK(menu_text_width(NULL) == 0, "a NULL string has a width");
    CHECK(menu_text_width("A") == (int)menu_font_glyph('A')->adv,
          "one glyph is not its own advance wide");
    CHECK(menu_text_width("AB") == (int)menu_font_glyph('A')->adv +
                                    (int)menu_font_glyph('B')->adv,
          "two glyphs are not the sum of their advances");
    /* An unsupported character is a gap of the space's advance, not a zero-width
     * hole and not a smear -- menu_draw.h's labels are all in the set, but a
     * future label with a character the font lacks should look like a space. */
    CHECK(menu_text_width("~") == menu_text_width(" "),
          "an unsupported character is not a gap the width of a space");

    /* Every advance is positive: a zero advance would let two glyphs overlap. */
    for (i = 0; i < MENU_FONT_GLYPHS; i++)
        CHECK(menu_font_glyphs[i].adv >= 1, "glyph %d has advance %d",
              i, menu_font_glyphs[i].adv);

    /* Space is blank everywhere, and every drawn glyph has ink -- a glyph with no
     * ink at all is a label that silently disappears. The unit here is coverage,
     * not cells: the atlas is 8-bit (menu_font.h), so "has ink" means the coverage
     * sums to something, and the two bounds catch the two ways a bake goes wrong --
     * a glyph that rasterised to nothing, and one that rasterised to a solid block.
     * The bounds are loose against the measured table (the thinnest glyph, 'I', has
     * 22 cells at >50% coverage and the heaviest, 'B', averages 48% of its box), so
     * they fail on a defect and not on a different face. */
    {
        const struct menu_glyph *sp = menu_font_glyph(' ');

        CHECK(sp->w == 0 && sp->h == 0, "space has a %dx%d ink box", sp->w, sp->h);
    }
    for (k = 'A'; k <= 'Z'; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)k);
        long sum = 0;
        int cells = 0;

        CHECK(g->w > 0 && g->h > 0, "'%c' has no ink box", k);
        for (j = 0; j < g->h; j++)
            for (i = 0; i < g->w; i++) {
                int c = menu_font_cov(g, i, j);

                CHECK(c >= 0 && c <= 255, "'%c' coverage at (%d,%d) is %d",
                      k, i, j, c);
                sum += c;
                if (c > 127)
                    cells++;
            }
        CHECK(cells >= 10, "'%c' has only %d solid cells", k, cells);
        CHECK(sum <= (long)g->w * g->h * 255L * 80 / 100,
              "'%c' is %ld/%d coverage, which is a blob", k, sum,
              g->w * g->h * 255);
    }
    for (k = '0'; k <= '9'; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)k);
        int cells = 0;

        for (j = 0; j < g->h; j++)
            for (i = 0; i < g->w; i++)
                if (menu_font_cov(g, i, j) > 127)
                    cells++;
        CHECK(cells >= 10, "'%c' has only %d solid cells", k, cells);
    }

    /* Every character in every label has ink, so no label renders as a gap. */
    for (i = 1; i <= MZ_COLS; i++) {
        const char *s = menu_label(i);
        int c;

        for (c = 0; s && s[c]; c++) {
            const struct menu_glyph *g;
            int ink = 0;

            if (s[c] == ' ')
                continue;
            g = menu_font_glyph((unsigned char)s[c]);
            for (j = 0; j < g->h; j++)
                for (k = 0; k < g->w; k++)
                    ink += menu_font_cov(g, k, j);
            CHECK(ink > 0, "'%c' in \"%s\" has no ink", s[c], s);
        }
    }

    /* Out of range is blank, not a read past the atlas. */
    CHECK(menu_font_cov(menu_font_glyph('A'), -1, 0) == 0, "col -1 has ink");
    CHECK(menu_font_cov(menu_font_glyph('A'), 999, 0) == 0, "a far column has ink");
    CHECK(menu_font_cov(menu_font_glyph('A'), 0, -1) == 0, "row -1 has ink");
    CHECK(menu_font_cov(menu_font_glyph('A'), 0, 999) == 0, "a far row has ink");
    CHECK(menu_font_cov(NULL, 0, 0) == 0, "a NULL glyph has ink");
    CHECK(menu_font_cov(menu_font_glyph('~'), 1, 1) == 0,
          "an unsupported character has ink");
    CHECK(menu_font_index(' ') == 0, "' ' is not the first glyph");
    CHECK(menu_font_index('Z') == 26 + 10, "'Z' is not where the digit run ends");
    CHECK(menu_font_index('a') == MENU_FONT_GLYPHS - 26 - strlen(MENU_FONT_PUNCT),
          "'a' does not start exactly where the punctuation's run begins");
    CHECK(menu_font_index('~') < 0, "the font claims a glyph for '~'");
    CHECK(menu_font_glyph('~')->adv == menu_font_glyph(' ')->adv,
          "an unsupported character does not resolve to a space");

    /* THE KEYBOARD'S ALPHABET IS IN THE ATLAS. menu_keyboard.h's caps name these
     * characters and the URL bar is typed with them; a character the atlas lacks
     * draws a gap, which on a key cap is a blank button the operator cannot tell
     * from a broken one. bake_menu_font.py checks the same string at bake time, so
     * this is the belt to that braces -- it catches a keyboard that names a
     * character the font deliberately never carried. */
    {
        static const char *keys =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            "./-_:?=&@#";

        for (i = 0; keys[i]; i++) {
            const struct menu_glyph *g = menu_font_glyph((unsigned char)keys[i]);
            int ink = 0;

            CHECK(menu_font_index((unsigned char)keys[i]) >= 0,
                  "the keyboard's '%c' has no glyph in the atlas", keys[i]);
            for (j = 0; j < g->h; j++)
                for (k = 0; k < g->w; k++)
                    ink += menu_font_cov(g, k, j);
            CHECK(ink > 0, "the keyboard's '%c' is blank", keys[i]);
        }
    }

    /* THE INK BAND, walked out of the table rather than trusted: no glyph's ink
     * leaves MENU_FONT_INK_TOP..MENU_FONT_INK_BOT, both ends of the band are really
     * used, and the band is MENU_FONT_INK_H rows of it. The witness's sample rows
     * are chosen to miss the ink, and this is where the band it misses is checked.
     *
     * IT IS A BAND AND NOT A GLYPH. It was three ways of saying one thing while
     * every glyph was a capital or a digit with the same 11-row box; lowercase
     * brought descenders and the two came apart, so what is asserted now is the
     * union -- the top row is the dot of an 'i' and the bottom row is the
     * underscore, and no single glyph touches both. */
    {
        int top = 999, bot = -1, tallest = 0;

        for (i = 0; i < MENU_FONT_GLYPHS; i++) {
            const struct menu_glyph *g = &menu_font_glyphs[i];

            if (g->h == 0)
                continue;
            if (g->h > tallest)
                tallest = g->h;
            if (g->top < top)
                top = g->top;
            if (g->top + g->h > bot)
                bot = g->top + g->h;
        }
        CHECK(top == MENU_FONT_INK_TOP, "the ink starts at row %d, not %d",
              top, MENU_FONT_INK_TOP);
        CHECK(bot - 1 == MENU_FONT_INK_BOT, "the ink ends at row %d, not %d",
              bot - 1, MENU_FONT_INK_BOT);
        CHECK(MENU_FONT_INK_BOT - MENU_FONT_INK_TOP + 1 == MENU_FONT_INK_H,
              "the ink band and the ink height disagree");
        CHECK(tallest <= MENU_FONT_INK_H,
              "a %d-row glyph does not fit the %d-row band", tallest,
              MENU_FONT_INK_H);
        CHECK(MENU_FONT_INK_TOP >= 0, "the ink band starts above the line box");
        CHECK(MENU_FONT_INK_BOT < MENU_FONT_LINE,
              "the ink runs past the line box (%d rows)", MENU_FONT_LINE);
        /* The band now reaches BELOW the baseline: that is what a descender is, and
         * the underscore is the deepest of them. It is still inside the line box,
         * which is the extent the layout centres in -- so an undersized band is
         * still refused by menu_view_ok() on the same rule as before. */
        CHECK(MENU_FONT_INK_BOT >= MENU_FONT_ASCENT - 1,
              "the ink band ends at row %d, above the baseline at %d",
              MENU_FONT_INK_BOT, MENU_FONT_ASCENT);
    }
}

/* ---------------------------------------------------------------------------
 * 3. The gesture.
 * ------------------------------------------------------------------------- */
/* THE MIDDLE OF A COLUMN, ASKED OF THE COLUMN. These were literals and they went
 * stale: X3_ read 533, which was the middle of column 3 when the band had SIX
 * columns, and it stayed 533 through the seventh column's arrival -- where it was
 * still inside column 3 by luck of the wider tiling -- and then landed in column 4
 * the moment the web cell retiled the seven to 176 px. Four gesture tests failed on
 * that one number and every one of them looked like a gesture bug. Deriving them
 * means the next retile moves the tests with the geometry instead of under it. */
#define X1_ ((menu_button_x0(0) + menu_button_x1(0)) / 2)   /* the middle of column 1 */
#define X3_ ((menu_button_x0(2) + menu_button_x1(2)) / 2)   /* the middle of column 3 */
#define X7_ ((menu_button_x0(MZ_COLS - 1) + menu_button_x1(MZ_COLS - 1)) / 2)  /* the middle of MENU */
#define ENTRY_X 639                          /* the middle of the entry zone */
#define OPEN_Y BTN_ROW                       /* in the button band */
#define STRIP_Y 20                           /* in the strip */
#define AWAY_Y 400                           /* below the panel */

/* Swipe the panel open from the strip. Every report of the gesture must be
 * swallowed, so this also asserts that -- and that lifting the finger leaves the
 * panel up, which is the toggle the operator asked for. Returns with the panel
 * open and no finger down.
 *
 * It starts at ENTRY_X and not at X1_, and that is the entry zone doing its job:
 * a swipe may only begin in the middle third, so column 1's centre (x 100) is no
 * longer a place a swipe can start from at all.
 *
 * MZ_FEED_TAKEN and not merely non-zero: the swipe's release must NOT be the
 * MZ_FEED_TAP answer, or a swipe would replay rbp the button it just covered. */
static void open_panel(void)
{
    int b = -1;

    menu_reset();
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "the swipe's press was not swallowed");
    CHECK(b == 0, "the swipe's press fired a button");
    CHECK(!menu_is_open(), "the panel opened on the down edge");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + 30, &b) == MZ_FEED_TAKEN, "the swipe's move was not swallowed");
    CHECK(!menu_is_open(), "the panel opened before MZ_SWIPE_PX");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN,
          "the swipe's opening move was not swallowed");
    CHECK(menu_is_open(), "the panel did not open at MZ_SWIPE_PX");
    CHECK(b == 0, "opening the panel fired a button");
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN,
          "the swipe's release was not swallowed, or was handed back as a tap");
    CHECK(b == 0, "the swipe that opened the panel fired a button");
    CHECK(menu_is_open(), "the panel closed when the swipe's finger lifted");
    CHECK(menu_pressed() == 0, "the swipe left a button highlighted");
}

static void test_gesture(void)
{
    int b, tx, ty;

    menu_reset();

    /* A press that starts outside the strip is not ours, end to end. */
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == 0, "a press in open space was swallowed");
    CHECK(menu_feed(1, 600, AWAY_Y - 20, &b) == 0, "a move in open space was swallowed");
    CHECK(menu_feed(0, 600, AWAY_Y - 20, &b) == 0, "a release in open space was swallowed");
    CHECK(!menu_is_open(), "a press in open space opened the panel");

    /* ...and it never becomes ours, even when it is dragged up into the strip. */
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == 0, "a press in open space was swallowed");
    CHECK(menu_feed(1, 600, STRIP_Y, &b) == 0, "a drag into the strip was swallowed");
    CHECK(menu_feed(1, 600, STRIP_Y - 10, &b) == 0, "a drag into the strip was swallowed");
    CHECK(menu_feed(0, 600, STRIP_Y - 10, &b) == 0, "its release was swallowed");
    CHECK(!menu_is_open(), "a drag from open space opened the panel");

    /* A tap in the entry zone does not open the panel -- there is no travel, so there
     * is no swipe -- and it is not discarded either: it comes back as MZ_FEED_TAP, the
     * one report the menu takes and gives back, for the caller to replay to rbp at
     * the point the finger landed (pointsrc.c's menu_replay_tap -- which is not
     * host-testable, so this is where the answer itself is pinned). */
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "a tap in the entry zone was not swallowed");
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAP, "its release was not handed back");
    CHECK(b == 0, "a tap in the entry zone fired a button");
    CHECK(!menu_is_open(), "a tap in the entry zone opened the panel");

    /* The strip's own edge: the last dead row is dead, the next row is rbp's. */
    CHECK(menu_feed(1, 600, MZ_STRIP_Y1, &b) == MZ_FEED_TAKEN, "the strip's last row was not dead");
    CHECK(menu_feed(0, 600, MZ_STRIP_Y1, &b) == MZ_FEED_TAP, "its release was not handed back");
    CHECK(menu_feed(1, 600, MZ_STRIP_Y1 + 1, &b) == 0, "row 56 was swallowed");
    CHECK(menu_feed(0, 600, MZ_STRIP_Y1 + 1, &b) == 0, "row 56's release was swallowed");
    CHECK(!menu_is_open(), "a press at row 56 opened the panel");

    /* THE MIDDLE THIRD, and both of its edges, because this is the boundary the
     * operator's INFO button depends on being outside. Inclusive at both ends: x 426
     * and x 852 are ours, 425 and 853 are rbp's. */
    CHECK(menu_feed(1, MZ_ENTRY_X0, STRIP_Y, &b) == MZ_FEED_TAKEN, "the entry zone's left edge was not ours");
    CHECK(menu_feed(0, MZ_ENTRY_X0, STRIP_Y, &b) == MZ_FEED_TAP, "its release was not handed back");
    CHECK(menu_feed(1, MZ_ENTRY_X1, STRIP_Y, &b) == MZ_FEED_TAKEN, "the entry zone's right edge was not ours");
    CHECK(menu_feed(0, MZ_ENTRY_X1, STRIP_Y, &b) == MZ_FEED_TAP, "its release was not handed back");
    CHECK(menu_feed(1, MZ_ENTRY_X0 - 1, STRIP_Y, &b) == 0, "the pixel left of the entry zone was swallowed");
    CHECK(menu_feed(0, MZ_ENTRY_X0 - 1, STRIP_Y, &b) == 0, "its release was swallowed");
    CHECK(menu_feed(1, MZ_ENTRY_X1 + 1, STRIP_Y, &b) == 0, "the pixel right of the entry zone was swallowed");
    CHECK(menu_feed(0, MZ_ENTRY_X1 + 1, STRIP_Y, &b) == 0, "its release was swallowed");
    CHECK(!menu_is_open(), "a press beside the entry zone opened the panel");

    /* And the whole point of the bounds: rbp's own INFO button -- x 1183..1258,
     * rows 12..35, measured off the glass -- is not merely replayed but never
     * swallowed at all. This is the operator's fourth finding, pinned as arithmetic:
     * the touch reaches rbp on the frame it happened, with no 45 ms dwell. */
    CHECK(menu_feed(1, 1211, 25, &b) == MZ_FEED_NONE, "the INFO button's press was swallowed");
    CHECK(menu_feed(0, 1211, 25, &b) == MZ_FEED_NONE, "the INFO button's release was swallowed");
    CHECK(b == 0, "the INFO button fired a menu button");
    CHECK(!menu_is_open(), "the INFO button opened the panel");
    /* ...including the left third, which is BROWSE's sidebar cells and the title
     * bands: rbp's, first frame, on every screen. */
    CHECK(menu_feed(1, 49, 25, &b) == MZ_FEED_NONE, "the sidebar cell's press was swallowed");
    CHECK(menu_feed(0, 49, 25, &b) == MZ_FEED_NONE, "the sidebar cell's release was swallowed");

    /* A swipe started outside the middle third is not a swipe and never becomes one:
     * it is rbp's press for its whole life, however far down it travels. */
    CHECK(menu_feed(1, 100, STRIP_Y, &b) == 0, "a left-third press was swallowed");
    CHECK(menu_feed(1, 100, STRIP_Y + MZ_SWIPE_PX, &b) == 0, "a left-third swipe was swallowed");
    CHECK(!menu_is_open(), "a press in the left third opened the panel");
    CHECK(menu_feed(0, 100, STRIP_Y + MZ_SWIPE_PX, &b) == 0, "its release was swallowed");
    CHECK(menu_feed(1, 1211, STRIP_Y, &b) == 0, "a right-third press was swallowed");
    CHECK(menu_feed(1, 1211, STRIP_Y + MZ_SWIPE_PX, &b) == 0, "a right-third swipe was swallowed");
    CHECK(!menu_is_open(), "a press in the right third opened the panel");
    CHECK(menu_feed(0, 1211, STRIP_Y + MZ_SWIPE_PX, &b) == 0, "its release was swallowed");

    /* WHILE THE PANEL IS OPEN THE X BOUNDS DO NOT APPLY -- they are the entry's, not
     * the panel's. The panel's leftmost column is SOURCE at x 0..212 and its
     * rightmost is MENU at x 1067..1279, so an open-state press that fell through to
     * rbp at either end would be a dead button under the operator's finger. */
    open_panel();
    CHECK(menu_feed(1, 20, OPEN_Y, &b) == MZ_FEED_TAKEN, "an open press on column 1 was not swallowed");
    CHECK(menu_pressed() == 1, "the highlight is on button %d, not 1", menu_pressed());
    CHECK(menu_feed(0, 20, OPEN_Y, &b) == MZ_FEED_TAKEN, "its release was not swallowed");
    CHECK(b == 1, "the leftmost column fired %d, not 1", b);
    /* The RIGHTMOST CELL is MENU again, now that the eighth cell has gone: x 1279 is
     * inside the seventh column, and this block covers the panel's ends -- the last
     * pixel of the panel is the last pixel of a real button, which is the property the
     * globe's 48 px broke and the removal restores. */
    open_panel();
    CHECK(menu_feed(1, 1279, OPEN_Y, &b) == MZ_FEED_TAKEN, "an open press on the last cell was not swallowed");
    CHECK(menu_pressed() == MZ_COLS,
          "the highlight is on cell %d, not %d", menu_pressed(), MZ_COLS);
    CHECK(menu_feed(0, 1279, OPEN_Y, &b) == MZ_FEED_TAKEN, "its release was not swallowed");
    CHECK(b == MZ_COLS, "the rightmost cell fired %d, not %d", b, MZ_COLS);
    open_panel();
    CHECK(menu_feed(1, X7_, OPEN_Y, &b) == MZ_FEED_TAKEN, "an open press on MENU was not swallowed");
    CHECK(menu_pressed() == MZ_COLS, "the highlight is on button %d, not %d", menu_pressed(), MZ_COLS);
    CHECK(menu_feed(0, X7_, OPEN_Y, &b) == MZ_FEED_TAKEN, "its release was not swallowed");
    CHECK(b == MZ_COLS, "MENU fired %d, not %d", b, MZ_COLS);
    /* ...and while open, a press in the strip above the buttons is still swallowed
     * (and closes), because the panel covers that band and dismissing it must not
     * press what is under it. */
    open_panel();
    CHECK(menu_feed(1, 1211, 4, &b) == MZ_FEED_TAKEN, "an open press in the right of the strip was not swallowed");
    CHECK(menu_feed(0, 1211, 4, &b) == MZ_FEED_TAKEN, "its release was not swallowed");
    CHECK(b == 0, "a press on the panel's border fired a button");
    CHECK(!menu_is_open(), "a press on the panel's border left the panel open");

    /* A drag ALONG the strip is not a swipe -- and so it is a tap, which is the one
     * place the drift matters: the replay goes to where the finger LANDED, not
     * where it lifted, because rbp never saw the movement and the operator aimed at
     * the thing it started on.
     *
     * It also leaves the entry zone on the way, which is deliberate: the latch is
     * taken at the down edge, so a press that starts in the middle third is ours
     * even when it wanders into the left third. */
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "the drag's press was not swallowed");
    CHECK(menu_feed(1, 500, STRIP_Y + 5, &b) == MZ_FEED_TAKEN, "the drag was not swallowed");
    CHECK(menu_feed(1, 300, STRIP_Y + 5, &b) == MZ_FEED_TAKEN, "the drag was not swallowed");
    CHECK(!menu_is_open(), "a drag along the strip opened the panel");
    CHECK(menu_feed(0, 300, STRIP_Y + 5, &b) == MZ_FEED_TAP, "the drag's release was not handed back");
    CHECK(b == 0, "a drag along the strip fired a button");
    menu_tap_point(&tx, &ty);
    CHECK(tx == ENTRY_X && ty == STRIP_Y,
          "the replay point is (%d,%d), not the start of the press", tx, ty);

    /* Nor is a swipe that is more sideways than down, even when it travels far
     * enough: 60 down and 200 across is a drag across the strip. */
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "press");
    CHECK(menu_feed(1, ENTRY_X - 200, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the diagonal was not swallowed");
    CHECK(!menu_is_open(), "a mostly-horizontal drag opened the panel");
    CHECK(menu_feed(0, ENTRY_X - 200, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAP, "release");
    CHECK(b == 0, "a mostly-horizontal drag fired a button");

    /* The swipe opens, and then a tap fires the button it is on. */
    open_panel();
    CHECK(menu_feed(1, X3_, OPEN_Y, &b) == 1, "a press on a button was not swallowed");
    CHECK(menu_pressed() == 3, "the highlight is on button %d, not 3", menu_pressed());
    CHECK(b == 0, "the press fired before the release");
    CHECK(menu_feed(0, X3_, OPEN_Y, &b) == 1, "the button's release was not swallowed");
    CHECK(b == 3, "the release fired %d, not 3", b);
    CHECK(!menu_is_open(), "the panel stayed open after a button fired");
    CHECK(menu_pressed() == 0, "the highlight survived the release");

    /* Every cell, by its own centre -- the seven labelled columns, which are now every
     * cell the panel has. Spelled off MZ_COLS so a change to the count moves this loop
     * with the geometry rather than under it. */
    {
        int cell;

        for (cell = 1; cell <= MZ_COLS; cell++) {
            int cx = (menu_button_x0(cell - 1) + menu_button_x1(cell - 1)) / 2;

            open_panel();
            CHECK(menu_feed(1, cx, OPEN_Y, &b) == 1, "press on cell %d", cell);
            CHECK(menu_pressed() == cell, "cell %d highlighted %d", cell, menu_pressed());
            CHECK(menu_feed(0, cx, OPEN_Y, &b) == 1, "release on cell %d", cell);
            CHECK(b == cell, "cell %d fired %d", cell, b);
            CHECK(!menu_is_open(), "the panel stayed open after cell %d", cell);
        }
    }

    /* Sliding off a button cancels it: the press started on one button and ended
     * on another, so nothing fires. This is what a button is expected to do, and
     * it is why the fire is on the release rather than the down edge. */
    open_panel();
    CHECK(menu_feed(1, X1_, OPEN_Y, &b) == 1, "press");
    CHECK(menu_pressed() == 1, "the highlight is not on button 1");
    CHECK(menu_feed(1, X3_, OPEN_Y, &b) == 1, "the slide was not swallowed");
    CHECK(menu_pressed() == 3, "the highlight did not follow the finger");
    CHECK(menu_feed(0, X3_, OPEN_Y, &b) == 1, "release");
    CHECK(b == 0, "a slide from button 1 to button 3 fired %d", b);
    CHECK(!menu_is_open(), "the panel stayed open");

    /* Sliding off the panel entirely cancels it too. */
    open_panel();
    CHECK(menu_feed(1, X1_, OPEN_Y, &b) == 1, "press");
    CHECK(menu_feed(1, X1_, AWAY_Y, &b) == 1, "the slide out was not swallowed");
    CHECK(menu_pressed() == 0, "a finger below the panel still highlights a button");
    CHECK(menu_feed(0, X1_, AWAY_Y, &b) == 1, "release");
    CHECK(b == 0, "a slide out of the panel fired %d", b);
    CHECK(!menu_is_open(), "the panel stayed open");

    /* A tap on the panel's own border, off the buttons, closes and does nothing. */
    open_panel();
    CHECK(menu_feed(1, X1_, MZ_BTN_Y0 - 1, &b) == 1, "a press on the border was not swallowed");
    CHECK(menu_pressed() == 0, "the border highlighted a button");
    CHECK(menu_feed(0, X1_, MZ_BTN_Y0 - 1, &b) == 1, "the border's release was not swallowed");
    CHECK(b == 0, "the border fired %d", b);
    CHECK(!menu_is_open(), "a press on the border did not close the panel");

    /* A tap AWAY, anywhere on the screen, closes the panel and does nothing else:
     * the press that dismisses the panel must not also press what is under it. */
    open_panel();
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == 1, "a tap away was not swallowed");
    CHECK(menu_feed(0, 600, AWAY_Y, &b) == 1, "a tap away's release was not swallowed");
    CHECK(b == 0, "a tap away fired %d", b);
    CHECK(!menu_is_open(), "a tap away did not close the panel");

    /* A swipe UP closes, from anywhere -- the operator must be able to travel up
     * without having started on the panel. */
    open_panel();
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == 1, "the closing swipe's press was not swallowed");
    CHECK(menu_feed(1, 600, AWAY_Y - MZ_CLOSE_PX, &b) == 1,
          "the closing swipe was not swallowed");
    CHECK(!menu_is_open(), "a swipe up did not close the panel");
    CHECK(b == 0, "the closing swipe fired %d", b);
    CHECK(menu_feed(0, 600, AWAY_Y - MZ_CLOSE_PX, &b) == 1, "its release was not swallowed");
    CHECK(b == 0, "the closing swipe's release fired %d", b);

    /* A sideways drag while open does not close it. */
    open_panel();
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == 1, "press");
    CHECK(menu_feed(1, 600 + MZ_CLOSE_PX, AWAY_Y, &b) == 1, "the sideways drag was not swallowed");
    CHECK(menu_is_open(), "a sideways drag closed the panel");
    CHECK(menu_feed(0, 600 + MZ_CLOSE_PX, AWAY_Y, &b) == 1, "release");

    /* A swipe up that STARTS ON A BUTTON closes and fires nothing, even though the
     * finger is still over a button when it closes. This is the one that would
     * otherwise turn "dismiss the panel" into "press whatever you were on". */
    open_panel();
    CHECK(menu_feed(1, X1_, MZ_BTN_Y1, &b) == 1, "press");
    CHECK(menu_pressed() == 1, "the highlight is not on button 1");
    CHECK(menu_feed(1, X1_, MZ_BTN_Y1 - MZ_CLOSE_PX, &b) == 1, "the upward drag was not swallowed");
    CHECK(!menu_is_open(), "a swipe up from a button did not close the panel");
    CHECK(menu_feed(0, X1_, MZ_BTN_Y1 - MZ_CLOSE_PX, &b) == 1, "release");
    CHECK(b == 0, "a swipe up from a button fired %d", b);

    /* THE EDGE, DRIVEN REPEATEDLY. Twenty identical down reports are one press:
     * the panel opens once, the state does not multiply, and nothing fires. */
    {
        int i, opens = 0;

        menu_reset();
        CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == 1, "press");
        for (i = 0; i < 20; i++) {
            CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == 1,
                  "repeat %d was not swallowed", i);
            CHECK(b == 0, "repeat %d fired %d", i, b);
            if (menu_is_open())
                opens++;
        }
        CHECK(opens == 20, "the panel was open for %d of 20 repeats", opens);
        CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == 1, "release");
        CHECK(menu_is_open(), "the panel closed when the repeated press lifted");
        CHECK(b == 0, "the repeated press fired %d", b);
    }

    /* The other latch: a press that starts in the strip is ours for its whole
     * life, wherever it goes -- including far below the panel. */
    menu_reset();
    CHECK(menu_feed(1, 600, STRIP_Y, &b) == 1, "the strip press was not swallowed");
    CHECK(menu_feed(1, 600, 700, &b) == 1, "a move far below the panel escaped");
    CHECK(menu_feed(1, 900, 780, &b) == 1, "a move to the corner escaped");
    CHECK(menu_feed(0, 900, 780, &b) == 1, "the release escaped");
    CHECK(b == 0, "a strip press that wandered fired %d", b);

    /* A release with no press is not ours: we never saw the press, so we have no
     * claim on the release. */
    menu_reset();
    CHECK(menu_feed(0, 600, AWAY_Y, &b) == 0, "a stray release was swallowed");
    CHECK(menu_feed(0, 600, STRIP_Y, &b) == 0, "a stray release in the strip was swallowed");

    /* Two presses in a row without a release: still one press. */
    menu_reset();
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == 1, "press");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == 1, "the swipe");
    CHECK(menu_is_open(), "the panel did not open");
    CHECK(menu_feed(1, X3_, OPEN_Y, &b) == 1, "a second down edge without a release");
    CHECK(menu_pressed() == 3, "the highlight did not follow");
    CHECK(b == 0, "a second down edge fired %d", b);

    /* menu_reset() -- the device was unplugged mid-swipe. The panel must not be
     * left open by a pointer that no longer exists, and the next press must be a
     * first press. */
    open_panel();
    CHECK(menu_is_open(), "the panel did not open before the reset");
    menu_reset();
    CHECK(!menu_is_open(), "reset() left the panel open");
    CHECK(menu_pressed() == 0, "reset() left a highlight");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == 1, "a strip press after reset was not swallowed");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == 1, "the swipe after reset");
    CHECK(menu_is_open(), "the panel did not open after a reset");
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == 1, "release");

    menu_reset();
}

/* ---------------------------------------------------------------------------
 * 3b. The tap the menu gives back.
 *
 * MZ_FEED_TAP is the one answer that is not "rbp gets it" and not "rbp does not":
 * the menu took the press, has no use for it, and hands it back for the caller to
 * replay. Everything that matters about it is a *negative* here -- it is answered
 * once, only at the release edge, never for a press that opened the panel, and
 * never for one that did not begin in the strip -- because a false positive is an
 * INFO press the operator never asked for.
 * ------------------------------------------------------------------------- */
static void test_strip_tap(void)
{
    int b, tx, ty;

    /* Answered at the release and not before: the down edge cannot know whether
     * the press is a swipe. */
    menu_reset();
    CHECK(menu_feed(1, 640, STRIP_Y, &b) == MZ_FEED_TAKEN, "the down edge was not swallowed");
    CHECK(menu_feed(1, 640, STRIP_Y + MZ_SWIPE_PX - 1, &b) == MZ_FEED_TAKEN,
          "a move one px short of the swipe was not swallowed");
    CHECK(!menu_is_open(), "the panel opened one px early");
    CHECK(menu_feed(0, 640, STRIP_Y + MZ_SWIPE_PX - 1, &b) == MZ_FEED_TAP,
          "a press one px short of a swipe was not handed back as a tap");

    /* And the point is the press start, not where the finger gave up. */
    menu_tap_point(&tx, &ty);
    CHECK(tx == 640 && ty == STRIP_Y, "the tap point is (%d,%d), not the press start", tx, ty);

    /* One answer per press: the release consumed the state, so a second release is
     * a stray release and must not replay a second INFO. */
    CHECK(menu_feed(0, 640, STRIP_Y, &b) == MZ_FEED_NONE, "a stray release was answered");
    CHECK(!menu_is_open(), "a stray release opened the panel");

    /* A press that begins outside the strip is never a tap, whichever way it goes. */
    menu_reset();
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == MZ_FEED_NONE, "a press in open space");
    CHECK(menu_feed(0, 600, AWAY_Y, &b) == MZ_FEED_NONE, "its release was answered");
    CHECK(menu_feed(1, 600, MZ_STRIP_Y1 + 1, &b) == MZ_FEED_NONE,
          "a press one row below the strip");
    CHECK(menu_feed(0, 600, MZ_STRIP_Y1 + 1, &b) == MZ_FEED_NONE,
          "its release was handed back as a tap");

    /* A strip press that DID open the panel is a swipe, not a tap -- the one
     * positive case that must stay swallowed, since the replay would fire rbp's
     * INFO button under the panel the gesture just opened. */
    menu_reset();
    CHECK(menu_feed(1, 640, STRIP_Y, &b) == MZ_FEED_TAKEN, "press");
    CHECK(menu_feed(1, 640, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the swipe");
    CHECK(menu_is_open(), "the panel did not open");
    CHECK(menu_feed(0, 640, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN,
          "the swipe that opened the panel was handed back as a tap");
    CHECK(menu_is_open(), "the panel closed when the swipe's finger lifted");

    /* Nor is a press on an open panel's button: it fires, and firing is the whole
     * answer. */
    CHECK(menu_feed(1, X1_, OPEN_Y, &b) == MZ_FEED_TAKEN, "a press on a button");
    CHECK(menu_feed(0, X1_, OPEN_Y, &b) == MZ_FEED_TAKEN, "a fired button was handed back");
    CHECK(b == 1, "the button fired %d, not 1", b);

    /* Nor a press that dismisses the panel by swiping up in it. */
    open_panel();
    CHECK(menu_feed(1, 600, AWAY_Y, &b) == MZ_FEED_TAKEN, "the dismissing press");
    CHECK(menu_feed(1, 600, AWAY_Y - MZ_CLOSE_PX, &b) == MZ_FEED_TAKEN, "the dismissal");
    CHECK(!menu_is_open(), "the panel survived the dismissal");
    CHECK(menu_feed(0, 600, AWAY_Y - MZ_CLOSE_PX, &b) == MZ_FEED_TAKEN,
          "the dismissing press was handed back as a tap");

    /* And menu_reset() clears the point with everything else, so a tap point read
     * after a device loss is not a stale coordinate from the last press. */
    menu_reset();
    menu_tap_point(&tx, &ty);
    CHECK(tx == 0 && ty == 0, "reset() left a tap point of (%d,%d)", tx, ty);
}

/* ---------------------------------------------------------------------------
 * 4. The image.
 * ------------------------------------------------------------------------- */

/* Paint once over a sentinel-filled panel and check the whole scheme's premise:
 * every pixel of the panel is written, and nothing outside it is. */
static void test_paint_covers(void)
{
    struct menu_view v = mkview(1280, 800);
    int x0, y0, x1, y1, fx, fy, outside = 0, unwritten = 0;

    menu_panel_rect(&v, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0, "the panel is not at the picture's top-left");
    CHECK(x1 == 1279, "the panel is %d px wide, not 1280", x1 - x0 + 1);
    CHECK(y1 - y0 + 1 == MZ_PANEL_H, "the panel is %d px tall, not %d",
          y1 - y0 + 1, MZ_PANEL_H);

    fill16(1280, 800, SENTINEL16);
    menu_paint(&v, 0);

    for (fy = 0; fy < 800; fy++) {
        for (fx = 0; fx < 1280; fx++) {
            int in = (fx >= x0 && fx <= x1 && fy >= y0 && fy <= y1);
            unsigned short p = fb16[(size_t)fy * 1280 + fx];

            if (in) {
                if (p == SENTINEL16)
                    unwritten++;
            } else if (p != SENTINEL16) {
                outside++;
            }
        }
    }
    CHECK(unwritten == 0, "%d panel pixels were never written", unwritten);
    CHECK(outside == 0, "the panel wrote %d pixels outside its own rect", outside);

    /* And the same for 32 bpp, on a smaller panel, because the palette and the
     * pixel cursor are two separate pieces of arithmetic and only one of them has
     * been exercised above. */
    {
        static unsigned int fb32[800 * 480];
        struct menu_view w;
        int n = 800 * 480;
        int i, unwritten32 = 0, outside32 = 0;

        memset(&w, 0, sizeof w);
        w.pix = fb32;
        w.pitch = 800;
        w.fb_w = 800;
        w.fb_h = 480;
        w.bpp = 32;
        w.dw = 800;
        w.dh = 480;
        for (i = 0; i < n; i++)
            fb32[i] = SENTINEL32;
        menu_panel_rect(&w, &x0, &y0, &x1, &y1);
        menu_paint(&w, 0);
        for (i = 0; i < n; i++) {
            int fx2 = i % 800, fy2 = i / 800;
            int in = (fx2 >= x0 && fx2 <= x1 && fy2 >= y0 && fy2 <= y1);

            if (in && fb32[i] == SENTINEL32)
                unwritten32++;
            if (!in && fb32[i] != SENTINEL32)
                outside32++;
        }
        CHECK(unwritten32 == 0, "16 bpp was fine and 32 bpp left %d pixels unwritten",
              unwritten32);
        CHECK(outside32 == 0, "32 bpp wrote %d pixels outside the panel", outside32);
        CHECK(menu_pixel(32, MENU_FILL) != menu_pixel(16, MENU_FILL),
              "the two depths produce the same pixel value");
    }

    /* A letterboxed picture: the bar sits at the top of the PICTURE, not of the
     * framebuffer, so it does not run out into the letterbox. */
    {
        struct menu_view w = mkview(1280, 800);

        w.by = 40;
        w.dh = 720;
        fill16(1280, 800, SENTINEL16);
        menu_panel_rect(&w, &x0, &y0, &x1, &y1);
        CHECK(y0 == 40, "the panel did not start at the picture's top (got %d)", y0);
        CHECK(y1 == 40 + (MZ_PANEL_H * 720) / 800 - 1,
              "the panel's height does not follow the picture's (got %d rows)",
              y1 - y0 + 1);
        menu_paint(&w, 0);
        for (fy = 0; fy < 800; fy++)
            for (fx = 0; fx < 1280; fx++) {
                int in = (fy >= y0 && fy <= y1);
                unsigned short p = fb16[(size_t)fy * 1280 + fx];

                if (!in && p != SENTINEL16) {
                    outside++;
                    if (outside < 3)
                        printf("  letterbox: (%d,%d) written outside the panel\n", fx, fy);
                }
            }
        CHECK(outside == 0, "a letterboxed panel wrote into the letterbox");
    }
}

static void test_paint_image(void)
{
    struct menu_view v = mkview(1280, 800);
    int x0, y0, x1, y1, fx, fy, i;
    static unsigned short before[1280 * MZ_PANEL_H];
    int changed, bad = 0;

    /* Idempotence: a second identical paint changes no pixel. */
    fill16(1280, 800, 0);
    menu_paint(&v, 0);
    memcpy(before, fb16, sizeof before);
    menu_paint(&v, 0);
    CHECK(memcmp(before, fb16, sizeof before) == 0,
          "a second identical paint changed the image");

    /* The pressed image differs from the unpressed one, and ONLY inside the
     * pressed button. Both halves matter: a highlight that leaks outside its
     * button is a smeared screen, and one that does not change anything is a
     * button that does not respond to a finger. */
    menu_panel_rect(&v, &x0, &y0, &x1, &y1);
    fill16(1280, 800, 0);
    menu_paint(&v, 0);
    memcpy(before, fb16, sizeof before);

    for (i = 1; i <= MZ_COLS; i++) {
        menu_paint(&v, i);
        changed = 0;
        for (fy = y0; fy <= y1; fy++) {
            for (fx = x0; fx <= x1; fx++) {
                unsigned short a = before[(size_t)fy * 1280 + fx];
                unsigned short b2 = fb16[(size_t)fy * 1280 + fx];
                int mine = (owner_of(&v, fx, fy) == i);

                if (a == b2)
                    continue;
                changed++;
                if (!mine) {
                    bad++;
                    if (bad < 4)
                        printf("  pressed %d changed (%d,%d), which is button %d\n",
                               i, fx, fy, owner_of(&v, fx, fy));
                }
            }
        }
        CHECK(changed > 0, "pressing button %d changed nothing", i);
        CHECK(bad == 0, "the pressed image leaked outside button %d", i);
        menu_paint(&v, 0);                 /* back to the plain image */
    }

    /* Every button is made of both a background and a label, and every label has
     * ink -- a button whose label silently vanished would still pass a test that
     * only counted the background. */
    for (i = 1; i <= MZ_COLS; i++) {
        int bg = 0, label = 0;

        for (fy = y0; fy <= y1; fy++)
            for (fx = x0; fx <= x1; fx++) {
                int c = menu_class_at(&v, fx, fy, 0);

                if (owner_of(&v, fx, fy) != i)
                    continue;
                if (c == MENU_BTN)
                    bg++;
                else if (c == MENU_LABEL)
                    label++;
            }
        CHECK(bg > 100, "button %d's background is only %d pixels", i, bg);
        CHECK(label > 20, "button %d's label is only %d pixels of ink", i, label);
    }

    /* The frame, the fill and the seams are all present -- a panel that drew only
     * buttons would look like three hundred lines of nothing. */
    {
        int border = 0, fill = 0, div = 0;

        for (fy = y0; fy <= y1; fy++)
            for (fx = x0; fx <= x1; fx++) {
                int c = menu_class_at(&v, fx, fy, 0);

                if (c == MENU_BORDER) border++;
                else if (c == MENU_FILL) fill++;
                else if (c == MENU_DIV) div++;
            }
        CHECK(border > 100, "the frame is only %d pixels", border);
        CHECK(fill > 100, "the fill is only %d pixels", fill);
        CHECK(div == (MZ_COLS - 1) * MZ_BTN_H,
              "the seams are %d pixels, not %d full-height columns",
              div, MZ_COLS - 1);
    }

    CHECK(menu_class_at(&v, 640, 400, 0) == MENU_NONE,
          "a pixel far below the panel is not NONE");
    CHECK(menu_pixel(16, MENU_NONE) == 0, "NONE is not the zero pixel");

    /* A view that cannot be drawn into is refused rather than written through. */
    {
        struct menu_view w = mkview(1280, 800);

        CHECK(menu_view_ok(&v), "a good view was refused");
        w.pix = NULL;
        CHECK(!menu_view_ok(&w), "a view with no pixels was accepted");
        w = mkview(1280, 800);
        w.bpp = 24;
        CHECK(!menu_view_ok(&w), "a 24 bpp view was accepted");
        w = mkview(1280, 800);
        w.dh = 900;
        CHECK(!menu_view_ok(&w), "a picture taller than the framebuffer was accepted");
        w = mkview(1280, 800);
        w.bx = 100;                        /* 100 + 1280 > 1280 */
        CHECK(!menu_view_ok(&w), "a picture past the framebuffer's right edge was accepted");
        CHECK(menu_class_at(&w, 0, 0, 0) == MENU_NONE,
              "an invalid view was classified instead of refused");
        CHECK(menu_value_at(&w, 0, 0, 0) == 0,
              "an invalid view has a pixel value");
        fill16(1280, 800, SENTINEL16);
        menu_paint(&w, 0);
        CHECK(fb16[0] == SENTINEL16, "an invalid view was painted");
    }
}

/* The labels have to fit their columns at every panel size the port can be run
 * at, not just the one on the operator's desk -- and the failure mode is a word
 * that runs into the neighbouring word, which is unreadable rather than obviously
 * broken. With one baked size and no scale to choose, the fit is now a property of
 * the panel rather than a decision: the labels are centred and CLIPPED to their own
 * column (menu_label_cov()), so the guarantee this test pins is that no label pixel
 * is ever outside its own button, at any size -- including sizes where the label
 * cannot fit and the clip is all that saves it. */
static void test_labels_fit(void)
{
    unsigned int s;

    for (s = 0; s < (unsigned)TSIZE_N; s++) {
        int fw = TSIZES[s].w, fh = TSIZES[s].h;
        struct menu_view v = mkview(fw, fh);
        int x0, y0, x1, y1, fx, fy, i, bad = 0;
        int seen[MZ_COLS];        /* one counter per column */

        CHECK(menu_view_ok(&v) == TSIZES[s].usable,
              "%dx%d is %s, and this test says %s", fw, fh,
              menu_view_ok(&v) ? "usable" : "refused",
              TSIZES[s].usable ? "usable" : "refused");
        if (!menu_view_ok(&v))
            continue;

        fill16(fw, fh, 0);
        menu_paint(&v, 0);
        menu_panel_rect(&v, &x0, &y0, &x1, &y1);
        for (i = 0; i < MZ_COLS; i++)
            seen[i] = 0;

        for (fy = y0; fy <= y1; fy++) {
            for (fx = x0; fx <= x1; fx++) {
                int c = menu_class_at(&v, fx, fy, 0);
                int own = owner_of(&v, fx, fy);
                int cx0, cx1;

                if (c != MENU_LABEL)
                    continue;
                if (own < 1) {
                    bad++;
                    if (bad < 3)
                        printf("  %dx%d: label ink at (%d,%d) is in no cell\n",
                               fw, fh, fx, fy);
                    continue;
                }
                seen[own - 1]++;
                /* ...and the pixel really is inside that cell's own column, which is
                 * the bound menu_label_cov() clips at -- a label drawn past its column
                 * is a truncated word, not an overlap, and this is what says so. */
                cx0 = menu_button_x0(own - 1) * fw / MZ_LOGICAL_W;
                cx1 = menu_button_x1(own - 1) * fw / MZ_LOGICAL_W;
                if (fx < cx0 || fx > cx1)
                    bad++;
            }
        }
        CHECK(bad == 0, "%dx%d has %d label pixels outside their own cell", fw, fh, bad);
        /* Every column drew SOME ink: a size where a label is entirely clipped away is
         * a button with no words on it, which is a different defect from a label that
         * spills. */
        for (i = 0; i < MZ_COLS; i++)
            CHECK(seen[i] > 0, "%dx%d drew nothing for cell %d", fw, fh, i + 1);
    }

    /* The panels this port is actually run on: the widest label has to fit the
     * narrowest of these columns whole. Measured for the baked table -- at 16 px the
     * widest is now "USB STOP" at 65 px (the six browse words are 57..59), and at
     * 1280x800 that sits in a 182 px column with the whole of its margin spare. The
     * tightest width above, 480, gives columns of 68 px, so this assertion is 3 px
     * from failing -- deliberately, because it is the one place the label table and
     * the column count are checked against each other and a silently truncated label
     * would be far worse than a red test. If a future label needs more, the fix is a
     * wider drawn band (menu_zone.h's MZ_COLS note), not a looser check. */
    {
        static const int widths[] = { 1280, 1024, 800, 640, 480, 1920, 1366 };
        unsigned int i;
        int widest = 0, j;

        for (i = 1; i <= MZ_COLS; i++) {
            int w = menu_text_width(menu_label(i));

            if (w > widest)
                widest = w;
        }
        for (j = 0; j < (int)(sizeof widths / sizeof widths[0]); j++) {
            struct menu_view v = mkview(widths[j], 800);
            int col = (menu_button_x1(0) + 1) * v.dw / MZ_LOGICAL_W;

            CHECK(widest <= col,
                  "the widest label (%d px) does not fit a %d px column at %d wide",
                  widest, col, widths[j]);
        }
    }
}

/* The painter IS the classifier: every pixel menu_paint() writes has to be the
 * pixel menu_value_at() says it is -- the class, the palette and, on a glyph edge,
 * the blend -- at every panel size and every pressed state.
 *
 * Two things rest on this. One is the cache in menu_draw.c: it renders the panel
 * into a buffer whose origin is its own top-left corner and blits the result onto
 * the page, so the painter's output has to be the whole truth about the image --
 * there is no second opinion anywhere in that path. The other is any shortcut
 * inside the classifier itself: the guards that keep a per-glyph walk out of most
 * of the label band's pixels are only allowed to be a shortcut if the image does
 * not change, and this is where that is checked rather than argued.
 *
 * menu_value_at() is not a re-implementation of the painter for this test -- it is
 * the function menu_paint() itself calls per pixel, split out so that a caller
 * asking "what value goes here?" gets the production answer.
 *
 * One CHECK per (size, pressed) and a count of the differing pixels, not one
 * assertion per pixel: the count says more than a pass/fail, and the output stays
 * readable when it fails. */
static void test_paint_matches_value(void)
{
    static const struct { int w, h; } sizes[] = {
        { 1280, 800 }, { 1024, 600 }, { 800, 480 }, { 1920, 1080 }, { 640, 480 }
    };
    int s, pressed, fx, fy, bad, x0, y0, x1, y1;
    long blended = 0, blended_wrong = 0;

    for (s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++) {
        struct menu_view v = mkview(sizes[s].w, sizes[s].h);

        menu_panel_rect(&v, &x0, &y0, &x1, &y1);
        for (pressed = 0; pressed <= MZ_COLS; pressed++) {
            fill16(sizes[s].w, sizes[s].h, SENTINEL16);
            menu_paint(&v, pressed);
            bad = 0;
            for (fy = y0; fy <= y1; fy++)
                for (fx = x0; fx <= x1; fx++) {
                    unsigned int want = menu_value_at(&v, fx, fy, pressed);

                    if (fb16[(size_t)fy * sizes[s].w + fx] != (unsigned short)want)
                        bad++;
                }
            CHECK(bad == 0, "%dx%d pressed=%d: %d pixels differ from the value",
                  sizes[s].w, sizes[s].h, pressed, bad);
        }
    }

    /* The labels really are antialiased, and only the labels are. Every pixel of the
     * 1280x800 panel is compared against the palette value its class would give: the
     * ones that differ are the glyph edges, and they must ALL be label pixels. A
     * blend that leaked onto a border or a button background would put a stray colour
     * on the panel; an image with no differing pixel at all would mean the labels
     * went back to being hard-edged blocks, which is what the operator complained
     * about and would otherwise pass every other test here. */
    {
        struct menu_view v = mkview(1280, 800);

        menu_panel_rect(&v, &x0, &y0, &x1, &y1);
        fill16(1280, 800, 0);
        menu_paint(&v, 0);
        for (fy = y0; fy <= y1; fy++)
            for (fx = x0; fx <= x1; fx++) {
                unsigned short got = fb16[(size_t)fy * 1280 + fx];
                int cls = menu_class_at(&v, fx, fy, 0);

                if (got == (unsigned short)menu_pixel(v.bpp, cls))
                    continue;
                blended++;
                if (cls != MENU_LABEL)
                    blended_wrong++;
            }
        CHECK(blended > 100, "only %ld pixels are blended: the labels are not"
              " antialiased", blended);
        CHECK(blended_wrong == 0, "%ld blended pixels are not label ink",
              blended_wrong);
    }

    /* The ends of the coverage range are exact palette values, which is what the
     * witness in menu_draw.c compares against. Coverage 255 can only be reached
     * through menu_value_at()'s blend path being skipped, so the check is that a
     * solidly covered pixel of a glyph is EXACTLY the label colour: the interior of
     * a letter's stem must not be a near-miss blend. */
    {
        struct menu_view v = mkview(1280, 800);
        int bx0, by0, bx1, by1, n = 0;

        menu_panel_rect(&v, &bx0, &by0, &bx1, &by1);
        for (fy = by0; fy <= by1; fy++)
            for (fx = bx0; fx <= bx1; fx++) {
                int cls = menu_class_at(&v, fx, fy, 0);

                if (cls != MENU_LABEL)
                    continue;
                if (menu_value_at(&v, fx, fy, 0) == menu_pixel(v.bpp, MENU_LABEL))
                    n++;
            }
        CHECK(n > 50, "only %d label pixels are the exact label colour: the"
              " coverage never reaches 255", n);
    }
}

/* The damage witness's sample points must never land on ink.
 *
 * menu_draw.c's menu_intact() compares the page against an EXACT palette value,
 * which is only valid where no glyph is drawn -- a partly covered glyph pixel is a
 * blend, and a witness that sampled one would read "damaged" on every tick, repaint
 * every frame and never converge. Its points come from menu_paint.c's
 * menu_witness_point(), so this asserts the property against the production list
 * rather than against a copy of the arithmetic: change the font, the size or the
 * label band, and this fails before the unit sees a repaint storm. */
static void test_witness_points_are_clear(void)
{
    int s, i, n = menu_witness_count();

    CHECK(n == 15, "the witness changed from 15 points to %d", n);
    for (s = 0; s < TSIZE_N; s++) {
        struct menu_view v = mkview(TSIZES[s].w, TSIZES[s].h);

        CHECK(menu_view_ok(&v) == TSIZES[s].usable,
              "%dx%d usability changed under the witness test",
              TSIZES[s].w, TSIZES[s].h);
        if (!menu_view_ok(&v))
            continue;
        for (i = 0; i < n; i++) {
            int fx, fy, pressed;

            menu_witness_point(&v, i, &fx, &fy);
            for (pressed = 0; pressed <= MZ_COLS; pressed++) {
                int cls = menu_class_at(&v, fx, fy, pressed);

                CHECK(cls != MENU_LABEL && cls != MENU_LABEL_PRESSED,
                      "%dx%d witness point %d at (%d,%d) is on label ink (pressed=%d)",
                      TSIZES[s].w, TSIZES[s].h, i, fx, fy, pressed);
                /* ...and the value the witness expects really is the palette
                 * value, which is the other half of what makes its comparison
                 * exact. */
                CHECK(menu_value_at(&v, fx, fy, pressed) ==
                      menu_pixel(v.bpp, cls),
                      "%dx%d witness point %d at (%d,%d) is a blended pixel",
                      TSIZES[s].w, TSIZES[s].h, i, fx, fy);
            }
        }
    }
}

/* The floor under the panel's size, and it is a floor rather than a squashed
 * drawing on purpose.
 *
 * The witness's band rows are the button band's own edges, one row in (see
 * menu_paint.c's menu_witness_point()); that is outside the label's ink only while
 * the band can host the font's line box, so a band shorter than MENU_FONT_LINE is
 * refused outright. Refusing is the cheap half of the trade: the alternative is a
 * bar drawn with its labels filling the band and a witness that reads "damaged" on
 * every tick, which repaints the band continuously into the page rbp is drawing
 * into, on rbp's own render thread.
 *
 * What this pins is the SHAPE of the rule and not the number 384, because the
 * number is integer division's business and would fail confusingly the next time
 * the band's rows move. Monotone in the picture's height, never usable on a band
 * the line box does not fit, and the two sizes that bracket the operator's own
 * glass and the one the port is not run on. */
static void test_panel_size_floor(void)
{
    int dh, prev_usable = 0, usable_n = 0, refused_n = 0, inverted = 0;
    struct menu_view v;

    for (dh = 1; dh <= 800; dh++) {
        int ok;

        v = mkview(1280, dh);
        ok = menu_view_ok(&v);

        if (ok)
            usable_n++;
        else
            refused_n++;
        /* Once a picture is tall enough it stays tall enough: a usable size above a
         * refused one would mean the height arithmetic is not monotone, which no
         * amount of spot-checking would reveal. */
        if (!ok && prev_usable)
            inverted++;
        prev_usable = ok;
    }
    CHECK(usable_n > 0 && refused_n > 0, "every height was %s",
          usable_n ? "usable" : "refused");
    CHECK(inverted == 0, "%d heights were refused above a height that was usable",
          inverted);

    v = mkview(1280, 800);
    v.pix = NULL;
    CHECK(!menu_view_ok(&v), "a view with no pixels was usable");
    v = mkview(1280, 320);
    CHECK(!menu_view_ok(&v), "320 rows was usable");
    v = mkview(1280, 480);
    CHECK(menu_view_ok(&v), "480 rows was refused");
    v = mkview(640, 480);
    CHECK(menu_view_ok(&v), "640x480 was refused");
    v = mkview(1920, 1080);
    CHECK(menu_view_ok(&v), "1920x1080 was refused");

    /* And the boundary is real, not a rounding artefact: the refused heights are
     * contiguous up to the first usable one, and that one gives a panel taller than
     * its own border. The height itself is reported rather than asserted, because
     * the number is integer division's business and would fail confusingly the next
     * time the band's rows move -- the SHAPE is what must hold. */
    {
        int last_refused = 0, first_usable = 0;

        for (dh = 1; dh <= 800; dh++) {
            v = mkview(1280, dh);
            if (menu_view_ok(&v)) {
                first_usable = dh;
                break;
            }
            last_refused = dh;
        }
        CHECK(first_usable > 0, "no usable height at all");
        CHECK(last_refused == first_usable - 1,
              "the refused heights are not contiguous up to the first usable one");
        v = mkview(1280, first_usable);
        {
            int x0, y0, x1, y1;

            menu_panel_rect(&v, &x0, &y0, &x1, &y1);
            CHECK(y1 - y0 + 1 >= MENU_FONT_LINE,
                  "the smallest usable picture has a %d-row panel", y1 - y0 + 1);
        }
    }
}

/* THE INCREMENTAL REDRAW, which is what menu_draw.c does instead of reclassifying
 * the whole panel every time a finger moves from one button to the next.
 *
 * The claim is one line of arithmetic and it is worth pinning because a violation of
 * it is invisible on the unit: the only pixels whose value depends on `pressed` are
 * inside the pressed button's own column (class_in_layout() reaches its pressed test
 * only after the column's x range has decided which button a pixel belongs to), so
 * the image for button b is the image for button a with exactly the columns {a, b}
 * redrawn. If that ever stops being true -- a class that reads `pressed` before the
 * column loop, a highlight that bleeds a pixel into its neighbour -- the panel keeps a
 * pixel of the old image and nothing anywhere says so. So it is pinned byte for byte,
 * for every ordered pair of images, at three panel sizes including one that scales.
 *
 * Two ends of the range are pinned as well. A full mask IS menu_paint(), byte for
 * byte, which is what makes the column loop a rewrite of the whole-panel loop rather
 * than a second implementation of it that could drift. And a mask of 0 writes nothing:
 * an incremental redraw that touched a pixel it was not asked for would be a scribble
 * on rbp's screen. */
static void test_partial_repaint(void)
{
    /* The pair sweep is the expensive half (49 ordered pairs, three drawings each),
     * so it runs at three sizes; the two ends run at four. */
    static const struct { int w, h; } pairs[] = {
        { 1280, 800 }, { 1024, 600 }, { 1366, 768 }
    };
    static const struct { int w, h; } sizes[] = {
        { 1280, 800 }, { 1024, 600 }, { 1366, 768 }, { 1920, 1080 }
    };
    static unsigned short img[IMG_MAX];
    static unsigned short ref[IMG_MAX];
    unsigned int s;

    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int w = sizes[s].w, h = sizes[s].h;
        int ph = (MZ_PANEL_H * h) / MZ_LOGICAL_H;
        size_t bytes, cells, k;
        struct menu_view vi, vf;
        long nz;

        if (ph < 1)
            ph = 1;
        cells = (size_t)w * (size_t)ph;
        bytes = cells * sizeof img[0];
        vi = mkview_pix(w, h, img);
        vf = mkview_pix(w, h, fb16);

        CHECK(menu_view_ok(&vi), "%dx%d is not a usable view", w, h);

        /* A full mask is the whole-panel paint -- and a full mask is EIGHT bits now,
         * because the web cell is a cell: a seven-bit mask leaves its cell holding
         * whatever was in the buffer, which is exactly what this check exists to
         * catch and exactly what it caught when the retile landed. */
        memset(img, 0, bytes);
        memset(fb16, 0, bytes);
        menu_paint_cols(&vi, 3, (1u << MZ_COLS) - 1u);
        menu_paint(&vf, 3);
        CHECK(memcmp(img, fb16, bytes) == 0,
              "%dx%d: painting every column is not painting the panel", w, h);

        /* ...and a mask of 0 writes nothing at all. */
        memset(img, 0, bytes);
        menu_paint_cols(&vi, 5, 0u);
        nz = 0;
        for (k = 0; k < cells; k++)
            if (img[k])
                nz++;
        CHECK(nz == 0, "%dx%d: a mask of 0 wrote %ld pixels", w, h, nz);
    }

    for (s = 0; s < sizeof pairs / sizeof pairs[0]; s++) {
        int w = pairs[s].w, h = pairs[s].h;
        int ph = (MZ_PANEL_H * h) / MZ_LOGICAL_H;
        size_t bytes, cells, k;
        struct menu_view vi, vr;
        int a, b;

        if (ph < 1)
            ph = 1;
        cells = (size_t)w * (size_t)ph;
        bytes = cells * sizeof img[0];
        vi = mkview_pix(w, h, img);
        vr = mkview_pix(w, h, ref);

        /* Every image, a = 0 (no cell) through every column: a finger arriving on or
         * leaving any cell is a transition whose mask must name it, and the sweep is
         * over all pairs rather than over the neighbours because an incremental
         * rebuild that is wrong by one cell is wrong in a way only a pair catches. */
        for (a = 0; a <= MZ_COLS; a++) {
            for (b = 0; b <= MZ_COLS; b++) {
                unsigned int mask = 0;
                long bad = 0, first_x = -1, first_y = -1;

                /* The image for `a`, then only the columns that differ repainted
                 * for `b` -- exactly what menu_draw.c's menu_build() does. */
                memset(img, 0, bytes);
                menu_paint_cols(&vi, a, (1u << MZ_COLS) - 1u);
                if (a > 0)
                    mask |= 1u << (a - 1);
                if (b > 0)
                    mask |= 1u << (b - 1);
                menu_paint_cols(&vi, b, mask);

                /* ...against the image for `b`, drawn whole. */
                memset(ref, 0, bytes);
                menu_paint(&vr, b);

                for (k = 0; k < cells; k++)
                    if (img[k] != ref[k]) {
                        if (!bad) {
                            first_x = (long)(k % (size_t)w);
                            first_y = (long)(k / (size_t)w);
                        }
                        bad++;
                    }
                CHECK(bad == 0, "%dx%d: repainting columns %#x to go from image %d"
                      " to image %d left %ld pixels wrong (first at %ld,%ld)",
                      w, h, mask, a, b, bad, first_x, first_y);
            }
        }
    }
}

/* menu_class_in() with a layout built once is menu_class_at() -- which is what lets
 * menu_draw.c's damage witness build the panel's geometry once per tick instead of
 * fifteen times, on both callers, at two thousand ticks a second.
 *
 * Pinned rather than argued because the two are separate functions now and the
 * witness compares page pixels against EXACT palette values: a prepared-layout path
 * that classified even one pixel differently would make the witness read "damaged"
 * every tick, repaint every frame, and never converge -- a cost, not a corruption,
 * which is exactly the kind of fault that survives a glance. */
static void test_layout_hoist(void)
{
    static const struct { int w, h; } sizes[] = {
        { 1280, 800 }, { 1024, 600 }, { 800, 480 }, { 1920, 1080 }
    };
    unsigned int s;

    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        struct menu_view v = mkview(sizes[s].w, sizes[s].h);
        struct menu_layout L;
        int x0, y0, x1, y1, fx, fy, step, pressed, bad;

        if (!menu_view_ok(&v))
            continue;
        menu_panel_rect(&v, &x0, &y0, &x1, &y1);
        menu_layout_make(&v, &L);

        /* Every pixel of the panel at the shipped size, and a stride elsewhere so
         * the sweep stays a few seconds under qemu-arm. A stride cannot hide a
         * disagreement of the kind this pins: a class that differs at one pixel
         * differs at every pixel of the boundary it sits on, and the boundaries here
         * are hundreds of pixels long. */
        step = (sizes[s].w == 1280 && sizes[s].h == 800) ? 1 : 5;
        for (pressed = 0; pressed <= MZ_COLS; pressed++) {
            bad = 0;
            for (fy = y0 - 2; fy <= y1 + 2; fy += step)
                for (fx = x0 - 2; fx <= x1 + 2; fx += step)
                    if (menu_class_in(&L, fx, fy, pressed) !=
                        menu_class_at(&v, fx, fy, pressed))
                        bad++;
            CHECK(bad == 0, "%dx%d pressed=%d: %d pixels classify differently"
                  " through a prepared layout",
                  sizes[s].w, sizes[s].h, pressed, bad);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 5. The press-and-hold.
 *
 * The operator put a finger on the shipped panel and asked for the button's second
 * meaning: *"if i long press menu it should go to utility"*. rbp implements that
 * itself, on the same keycode, with its own timer -- press is the menu, hold is
 * UTILITY, and the threshold is measured between 300 and 400 ms (menu_zone.h's
 * MZ_HOLD_FINGER_MS). So what this module owns is one comparison, and what the
 * constants have to keep is the relationship between them.
 *
 * Every check here is a negative or a boundary: the rule is one line, and the way
 * to get it wrong is to move it by a millisecond or to retune a number past rbp's
 * threshold.
 * ------------------------------------------------------------------------- */
/* The three reports that bring the panel out. Every case below has to start with
 * one, and the finger has to come OFF before the next press is a press -- feeding a
 * press while another is still down is the same press at a new position, and reads
 * as a move (the note in test_hold's own body is that lesson, learned the hard way). */
static void swipe_panel_open(void)
{
    int b;

    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "the swipe's press was not swallowed");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the swipe did not open the panel");
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the swipe's release was not swallowed");
    CHECK(menu_is_open(), "the panel is not open after a swipe");
}

static void test_hold(void)
{
    int b;
    int mx = (menu_button_x0(MZ_COLS) + menu_button_x1(MZ_COLS)) / 2;

    /* The boundary, exactly: the threshold itself is a hold, one ms under is not. */
    CHECK(menu_hold_fires(MZ_HOLD_FINGER_MS, MZ_HOLD_FINGER_MS) == 1,
          "a finger of exactly %d ms is not a hold", MZ_HOLD_FINGER_MS);
    CHECK(menu_hold_fires(MZ_HOLD_FINGER_MS - 1, MZ_HOLD_FINGER_MS) == 0,
          "a finger one ms short of the threshold is a hold");

    /* A press the caller never measured is a tap: held_ms comes from the loop that
     * saw the press, and the relative loop has no stamp for a report that is not a
     * release. Getting this wrong would fire a hold on every report. */
    CHECK(menu_hold_fires(-1, MZ_HOLD_FINGER_MS) == 0,
          "an unmeasured press is a hold");

    /* threshold <= 0 is the A/B lever the bench knob pulls, and it must disable the
     * hold whatever the finger did. */
    CHECK(menu_hold_fires(60000, 0) == 0, "a threshold of 0 did not turn the hold off");
    CHECK(menu_hold_fires(60000, -5) == 0, "a negative threshold did not turn the hold off");

    /* The two short presses the shim itself makes, and the fastest human tap: none
     * of them can become a hold by accident. 45 is POINT_MENU_TAP_DEFAULT_MS (the
     * dwell of a replayed strip tap), 150 is poke.py's own default finger dwell. */
    CHECK(menu_hold_fires(45, MZ_HOLD_FINGER_MS) == 0,
          "the replay's own 45 ms dwell is a hold");
    CHECK(menu_hold_fires(150, MZ_HOLD_FINGER_MS) == 0, "a 150 ms tap is a hold");

    /* And the two numbers against the measurement they came from. MZ_HOLD_KEY_MS is
     * the one that matters: it is what rbp's timer has to see, and 400 ms is the
     * measured upper bound of its threshold -- below that, a hold stops reaching
     * UTILITY and no test, doc or log anywhere else would notice. */
    CHECK(MZ_HOLD_KEY_MS > 400,
          "the synthesized key (%d ms) is inside rbp's measured 300..400 ms threshold",
          MZ_HOLD_KEY_MS);
    CHECK(MZ_HOLD_FINGER_MS < MZ_HOLD_KEY_MS,
          "the finger's threshold (%d ms) is not below the key it sends (%d ms)",
          MZ_HOLD_FINGER_MS, MZ_HOLD_KEY_MS);
    CHECK(MZ_HOLD_FINGER_MS > 150,
          "the finger's threshold (%d ms) is inside the range of an ordinary tap",
          MZ_HOLD_FINGER_MS);

    /* The gesture is the same one a tap ends on -- `held_ms` is the caller's number
     * and this module never sees it -- so the release must fire the button the finger
     * is on, and only that. A hold that fired a different button, or that survived
     * sliding off, would be a bug the duration would get blamed for. */
    menu_reset();
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "the swipe in was not swallowed");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the swipe did not open the panel");
    CHECK(menu_is_open(), "the panel is not open for the hold test");
    /* The finger has to come OFF the swipe before the next press is a press: a
     * press fed while one is still down is the same press at a new position, and it
     * reads as a move. (Measured the hard way -- this test's first version fed the
     * MENU press with the swipe's finger still down, so the release found
     * started_open == 0, fired nothing, and left the panel open.) */
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the swipe's release was not swallowed");
    CHECK(menu_is_open(), "the panel closed when the swipe's finger lifted");
    CHECK(menu_feed(1, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the press on MENU was not swallowed");
    CHECK(menu_pressed() == MZ_COLS, "a press on MENU highlighted button %d", menu_pressed());
    CHECK(menu_feed(0, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the release on MENU was not swallowed");
    CHECK(b == MZ_COLS, "the release fired button %d, not MENU (%d)", b, MZ_COLS);
    CHECK(!menu_is_open(), "the panel stayed open after a button fired");

    /* Held long, then slid off: still nothing. The slide-off rule is not a function
     * of how long the finger was there -- and the release's own point is not what
     * decides it, the button the finger *tracked* onto is: that is what `cur_btn` is
     * for, and it is why the slide has to be fed as a move. A release fed straight at
     * another column, with no move to get there, still fires MENU, which is the
     * highlight rule and not a bug. */
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y, &b) == MZ_FEED_TAKEN, "the second swipe's press was not swallowed");
    CHECK(menu_feed(1, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the second swipe did not open the panel");
    CHECK(menu_feed(0, ENTRY_X, STRIP_Y + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN, "the second swipe's release was not swallowed");
    CHECK(menu_feed(1, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the held press on MENU was not swallowed");
    CHECK(menu_feed(1, mx, BTN_ROW + 1, &b) == MZ_FEED_TAKEN, "the held press's move was not swallowed");
    CHECK(menu_pressed() == MZ_COLS, "the held press left MENU after a move inside it");
    /* menu_button_x0(1) is the SECOND column's left edge -- i is 0-based here. */
    CHECK(menu_feed(1, menu_button_x0(1) + 5, BTN_ROW, &b) == MZ_FEED_TAKEN, "the slide onto the second column was not swallowed");
    CHECK(menu_pressed() == 2, "the slide left the highlight on button %d", menu_pressed());
    CHECK(menu_feed(0, menu_button_x0(1) + 5, BTN_ROW, &b) == MZ_FEED_TAKEN, "the slid-off release was not swallowed");
    CHECK(b == 0, "a hold that slid to another column fired button %d", b);
    CHECK(!menu_is_open(), "the panel stayed open after a slid-off release");

    /* ---------------------------------------------------------------------
     * The same hold asked WHILE the finger is still down -- the operator's
     * follow-up on the same button (2026-10-04): *"for holding the MENU to get
     * utility, after two seconds the menu should disappear and it should just go to
     * utility by itself"*. The comparison is the one pinned above; what is new is
     * WHEN it is asked, so what these have to catch is a mid-press answer that
     * leaves the panel standing, fires twice, or lets the release fire as well.
     * ------------------------------------------------------------------- */
    menu_reset();
    swipe_panel_open();
    CHECK(menu_feed(1, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the held press on MENU was not swallowed");

    /* One millisecond short is not a hold, and must not touch the panel on the way
     * to deciding -- this is the check an off-by-one in the threshold would fail. */
    CHECK(menu_hold_pending(MZ_HOLD_FINGER_MS - 1, MZ_HOLD_FINGER_MS) == 0,
          "a finger one ms short of the threshold became a hold mid-press");
    CHECK(menu_is_open(), "the panel closed short of the hold's threshold");
    CHECK(menu_pressed() == MZ_COLS, "the short-of-threshold report left the highlight on %d", menu_pressed());

    /* The threshold itself: the button comes back and the panel is already gone, in
     * the same call -- the caller's very next act is to send the key, and it must not
     * be sending it over a panel that is still on the glass. */
    CHECK(menu_hold_pending(MZ_HOLD_FINGER_MS, MZ_HOLD_FINGER_MS) == MZ_COLS,
          "the threshold did not fire MENU mid-press");
    CHECK(!menu_is_open(), "the panel stayed open after a mid-press hold");
    CHECK(menu_pressed() == 0, "a button is still highlighted after a mid-press hold");

    /* Once, and only once: the tick that asked the first time asks again 20 ms later. */
    CHECK(menu_hold_pending(MZ_HOLD_FINGER_MS + 5000, MZ_HOLD_FINGER_MS) == 0,
          "the mid-press hold fired a second time");

    /* And the release still to come -- the finger is on the glass throughout -- is the
     * silent swallow it has always been. A second MENU here would be a second key. */
    CHECK(menu_feed(0, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the release after a mid-press hold was not swallowed");
    CHECK(b == 0, "the release after a mid-press hold fired button %d", b);
    CHECK(!menu_is_open(), "the panel came back after a mid-press hold's release");

    /* The slide-off rule, asked one press earlier: a finger that wanders onto the
     * next column is not holding the button it started on, however long it stays. */
    swipe_panel_open();
    CHECK(menu_feed(1, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the slid press on MENU was not swallowed");
    CHECK(menu_feed(1, menu_button_x0(1) + 5, BTN_ROW, &b) == MZ_FEED_TAKEN, "the slide off MENU was not swallowed");
    CHECK(menu_hold_pending(60000, MZ_HOLD_FINGER_MS) == 0,
          "a finger that slid off MENU fired a mid-press hold");
    CHECK(menu_is_open(), "the panel closed on a mid-press hold that had slid off its button");
    CHECK(menu_feed(0, menu_button_x0(1) + 5, BTN_ROW, &b) == MZ_FEED_TAKEN, "the slid press's release was not swallowed");

    /* A press the menu owns but no button does -- the panel's top border row, above the
     * buttons -- can no more become a hold than the swipe can, because `press_btn` is 0
     * for it. STRIP_Y is not that row: the panel is drawn over the strip, so the row a
     * swipe starts in when the panel is CLOSED is the fourth button when it is open. */
    swipe_panel_open();
    CHECK(menu_button_at(ENTRY_X, MZ_PANEL_Y0) == 0, "the panel's border row is a button");
    CHECK(menu_feed(1, ENTRY_X, MZ_PANEL_Y0, &b) == MZ_FEED_TAKEN, "the press on the border was not swallowed");
    CHECK(menu_hold_pending(60000, MZ_HOLD_FINGER_MS) == 0,
          "a press that landed on no button fired a mid-press hold");
    CHECK(menu_is_open(), "the panel closed on a press that was not on a button");
    CHECK(menu_feed(0, ENTRY_X, MZ_PANEL_Y0, &b) == MZ_FEED_TAKEN, "the border press's release was not swallowed");

    /* The A/B lever, pulled mid-press as well as at the release: a threshold of 0 is
     * "the hold is off", and it has to mean the panel never dismisses itself. */
    swipe_panel_open();
    CHECK(menu_feed(1, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the last press on MENU was not swallowed");
    CHECK(menu_hold_pending(60000, 0) == 0, "a threshold of 0 did not turn the mid-press hold off");
    CHECK(menu_hold_pending(60000, -5) == 0, "a negative threshold did not turn the mid-press hold off");
    CHECK(menu_is_open(), "the panel closed with the hold turned off");
    CHECK(menu_feed(0, mx, BTN_ROW, &b) == MZ_FEED_TAKEN, "the last press's release was not swallowed");
    CHECK(!menu_is_open(), "the panel stayed open after an ordinary release");
}

int main(void)
{
    test_geometry();
    test_font();
    test_gesture();
    test_strip_tap();
    test_hold();
    test_paint_covers();
    test_paint_matches_value();
    test_paint_image();
    test_partial_repaint();
    test_layout_hoist();
    test_panel_size_floor();
    test_labels_fit();
    test_witness_points_are_clear();

    printf("test_menu: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
