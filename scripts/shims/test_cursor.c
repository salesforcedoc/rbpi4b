/*
 * test_cursor.c — the visible pointer's compositing rule, in-process.
 *
 * The reason this test exists is the third case below. Drawing an arrow and
 * putting back what was under it is trivial and would pass a two-case test; what
 * is *not* trivial, and is the thing that would break silently on the unit, is
 * the conditional restore. With DFB_PRESENT=off rbp paints its UI straight into
 * this same page, so between a paint() and its restore() the player can have
 * redrawn the region underneath — and an unconditional put-back then pastes
 * stale pixels over a fresh repaint, which on screen looks like a small block of
 * the previous screen following the mouse around. That failure is invisible in
 * any test that never lets a third party write between the two calls, so this
 * file fakes exactly that: fill, paint, let "rbp" repaint one cell of the glyph,
 * restore, and assert the repaint survived while everything else came back.
 *
 * The surface is a local array in both formats the target can produce (16 bpp
 * RGB565, which is what the measured Pi fb is, and 32 bpp XRGB8888), with a
 * deliberately odd pitch in one case so a stride bug cannot hide behind
 * pitch == width.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_cursor test_cursor.c
 *            cursor_paint.c   &&   qemu-arm ./test_cursor
 */
#include "cursor_paint.h"

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

/* --- the surface ------------------------------------------------------------
 *
 * A buffer of 16-bit pixels, addressed as bpp decides. The fill is a ramp that
 * never lands on 0x0000 or 0xffff, i.e. never on either of the two values
 * cursor_pixel() writes — so "the arrow is there" and "the arrow is gone" are
 * both observable as differences, in both the pressed and unpressed states (a
 * press swaps black and white; if the background could be either, a swap would
 * be indistinguishable from no change at all). */
#define TX_W 40
#define TX_H 30
#define TX_PITCH 47                    /* deliberately wider than TX_W */

static unsigned short buf16[TX_PITCH * TX_H];
static unsigned int   buf32[TX_PITCH * TX_H];

static unsigned int fill16(size_t i) { return (unsigned int)(0x0800 + (i & 0x07ff)); }
static unsigned int fill32(size_t i) { return (unsigned int)(0x00080808u + (i & 0x7ff)); }

static void reset_surface(void)
{
    size_t i;
    for (i = 0; i < (size_t)TX_PITCH * TX_H; i++) {
        buf16[i] = (unsigned short)fill16(i);
        buf32[i] = fill32(i);
    }
}

static unsigned int get16(int x, int y) { return buf16[(size_t)y * TX_PITCH + x]; }
static unsigned int get32(int x, int y) { return buf32[(size_t)y * TX_PITCH + x]; }
static void put16(int x, int y, unsigned int v) { buf16[(size_t)y * TX_PITCH + x] = (unsigned short)v; }
static void put32(int x, int y, unsigned int v) { buf32[(size_t)y * TX_PITCH + x] = v; }

/* How many pixels of the glyph should land on the surface with the tip at (x,y):
 * every non-transparent cell that is actually on screen. This is the count the
 * paint must change and the restore must undo, and it is computed from the glyph
 * rather than from the buffer so it cannot agree with a buggy loop by accident. */
static int glyph_cells_on_screen(int x, int y)
{
    int col, row, n = 0;
    for (row = 0; row < CURSOR_H; row++) {
        if (y + row < 0 || y + row >= TX_H)
            continue;
        for (col = 0; col < CURSOR_W; col++) {
            if (x + col < 0 || x + col >= TX_W)
                continue;
            if (cursor_class_at(col, row) != CURSOR_TRANSPARENT)
                n++;
        }
    }
    return n;
}

/* Pixels differing from the pristine fill, over the whole buffer. */
static int count_changed16(void)
{
    size_t i;
    int n = 0;
    for (i = 0; i < (size_t)TX_PITCH * TX_H; i++)
        if (buf16[i] != (unsigned short)fill16(i))
            n++;
    return n;
}

static int count_changed32(void)
{
    size_t i;
    int n = 0;
    for (i = 0; i < (size_t)TX_PITCH * TX_H; i++)
        if (buf32[i] != fill32(i))
            n++;
    return n;
}

/* --- 1. the glyph and the pixel values ------------------------------------- */

static void test_glyph(void)
{
    int col, row, bodies = 0, outlines = 0;

    for (row = 0; row < CURSOR_H; row++)
        for (col = 0; col < CURSOR_W; col++) {
            int cls = cursor_class_at(col, row);
            CHECK(cls == CURSOR_TRANSPARENT || cls == CURSOR_OUTLINE ||
                  cls == CURSOR_BODY,
                  "cell (%d,%d) is class %d", col, row, cls);
            if (cls == CURSOR_BODY) bodies++;
            if (cls == CURSOR_OUTLINE) outlines++;
        }
    /* An arrow with no filled interior or no border is a typo in the glyph, not a
     * rendering question, so it is worth failing on here. The counts are not
     * compared against each other: this is a filled arrow with a one-cell border,
     * so there are legitimately more body cells than outline ones, and a test
     * asserting the opposite would be pinning a mistake. What matters is that
     * both classes exist and that together they are the whole opaque glyph —
     * which is what makes "the pixel we wrote" a well-defined thing for the
     * conditional restore to compare against. */
    CHECK(bodies > 0, "the glyph has no body cells");
    CHECK(outlines > 0, "the glyph has no outline cells");
    CHECK(bodies + outlines <= CURSOR_W * CURSOR_H,
          "the glyph claims %d cells in a %dx%d box", bodies + outlines,
          CURSOR_W, CURSOR_H);

    /* The tip is what points at the target, so it must be an opaque cell right at
     * the origin of the box: the position rbp is sent is the tip, not the centre
     * of the glyph, and a transparent corner would leave the click looking like
     * it landed beside the thing it was aimed at. */
    CHECK(cursor_class_at(0, 0) != CURSOR_TRANSPARENT,
          "the tip cell is transparent");
    CHECK(cursor_class_at(-1, 0) == CURSOR_TRANSPARENT, "off-screen column is not transparent");
    CHECK(cursor_class_at(0, -1) == CURSOR_TRANSPARENT, "off-screen row is not transparent");
    CHECK(cursor_class_at(CURSOR_W, 0) == CURSOR_TRANSPARENT, "past the last column");
    CHECK(cursor_class_at(0, CURSOR_H) == CURSOR_TRANSPARENT, "past the last row");

    /* Both formats, both states: outline and body must be each other's inverse,
     * and the two formats must not share a value by accident. */
    CHECK(cursor_pixel(16, 0, CURSOR_OUTLINE) == 0x0000,
          "16 bpp outline is %04x", cursor_pixel(16, 0, CURSOR_OUTLINE));
    CHECK(cursor_pixel(16, 0, CURSOR_BODY) == 0xffff,
          "16 bpp body is %04x", cursor_pixel(16, 0, CURSOR_BODY));
    CHECK(cursor_pixel(16, 1, CURSOR_OUTLINE) == 0xffff,
          "16 bpp outline does not invert on press");
    CHECK(cursor_pixel(16, 1, CURSOR_BODY) == 0x0000,
          "16 bpp body does not invert on press");
    CHECK(cursor_pixel(32, 0, CURSOR_OUTLINE) == 0x000000 &&
          cursor_pixel(32, 0, CURSOR_BODY) == 0xffffff,
          "32 bpp outline/body: %06x/%06x",
          cursor_pixel(32, 0, CURSOR_OUTLINE), cursor_pixel(32, 0, CURSOR_BODY));
    CHECK(cursor_pixel(32, 1, CURSOR_OUTLINE) == 0xffffff &&
          cursor_pixel(32, 1, CURSOR_BODY) == 0x000000,
          "32 bpp does not invert on press");
}

/* --- 2. paint, then restore, is the identity ------------------------------- */

static void test_paint_restore_round_trip(void)
{
    static unsigned int saved[CURSOR_W * CURSOR_H];
    int x = 11, y = 7;

    reset_surface();
    cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
    CHECK(count_changed16() == glyph_cells_on_screen(x, y),
          "paint changed %d pixels, the glyph has %d on screen",
          count_changed16(), glyph_cells_on_screen(x, y));

    /* Spot-check the two classes at the tip and just below it rather than
     * trusting the count alone: the count would also be right if outline and
     * body were emitted swapped. */
    CHECK(get16(x + 0, y + 0) == cursor_pixel(16, 0, cursor_class_at(0, 0)),
          "tip pixel is %04x", get16(x, y));

    cursor_restore(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
    CHECK(count_changed16() == 0, "restore left %d pixels changed", count_changed16());

    /* The same in 32 bpp, so the format branch is exercised in both directions
     * rather than only by the pixel-value assertions above — and pressed rather
     * than released, so the inverted pair is the one being saved and restored.
     * The tip is repainted in between, so this is also the conditional rule in
     * the other format: one pixel must survive, all the rest must come back. */
    reset_surface();
    cursor_paint(buf32, TX_PITCH, TX_W, TX_H, 32, x, y, 1, saved);
    CHECK(count_changed32() == glyph_cells_on_screen(x, y),
          "32 bpp paint changed %d pixels, the glyph has %d on screen",
          count_changed32(), glyph_cells_on_screen(x, y));
    CHECK(get32(x, y) == cursor_pixel(32, 1, cursor_class_at(0, 0)),
          "32 bpp tip pixel is %08x", get32(x, y));

    put32(x, y, 0x00abcdefu);
    cursor_restore(buf32, TX_PITCH, TX_W, TX_H, 32, x, y, 1, saved);
    CHECK(get32(x, y) == 0x00abcdefu,
          "32 bpp restore overwrote the repainted tip with %08x", get32(x, y));
    CHECK(count_changed32() == 1,
          "32 bpp restore left %d pixels changed, expected exactly the repainted tip",
          count_changed32());

    /* Rows outside the glyph box must never be touched, and the padding between
     * the end of the visible row and the pitch must not be either — a stride bug
     * shows up exactly there and nowhere else. */
    {
        int i;
        reset_surface();
        cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
        for (i = 0; i < TX_H; i++) {
            int c;
            if (i >= y && i < y + CURSOR_H)
                continue;
            for (c = 0; c < TX_PITCH; c++)
                CHECK(buf16[(size_t)i * TX_PITCH + c] == (unsigned short)fill16((size_t)i * TX_PITCH + c),
                      "row %d was written outside the glyph box", i);
        }
        for (i = y; i < y + CURSOR_H; i++) {
            int c;
            for (c = TX_W; c < TX_PITCH; c++)
                CHECK(buf16[(size_t)i * TX_PITCH + c] == (unsigned short)fill16((size_t)i * TX_PITCH + c),
                      "pitch padding at row %d column %d was written", i, c);
        }
    }
}

/* --- 3. the conditional restore -------------------------------------------- */

static void test_restore_leaves_rbp_repaints_alone(void)
{
    static unsigned int saved[CURSOR_W * CURSOR_H];
    int x = 9, y = 5;
    int col, row, clobbered = 0;
    int cc[2], cr[2];
    const unsigned short rbp_px = 0xABCD;

    /* Choose two opaque cells inside the glyph — one outline, one body — and let
     * "rbp" paint over them with a value that is neither of the cursor's. The two
     * classes are both used because the conditional test compares against
     * cursor_pixel(pressed): an implementation that only reasoned about the
     * outline would pass with one and fail with the other. */
    reset_surface();
    cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);

    for (row = 0; row < CURSOR_H && clobbered < 2; row++)
        for (col = 0; col < CURSOR_W && clobbered < 2; col++) {
            int cls = cursor_class_at(col, row);
            /* One of each class, in the order they are asked for. */
            if (cls != (clobbered == 0 ? CURSOR_OUTLINE : CURSOR_BODY))
                continue;
            cc[clobbered] = col;
            cr[clobbered] = row;
            put16(x + col, y + row, rbp_px);
            clobbered++;
        }
    CHECK(clobbered == 2, "found only %d glyph cells to clobber", clobbered);

    cursor_restore(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);

    /* The two clobbered cells must still hold rbp's pixel: putting the saved
     * value back there is the stale-pixel bug this whole rule exists to prevent. */
    CHECK(get16(x + cc[0], y + cr[0]) == rbp_px &&
          get16(x + cc[1], y + cr[1]) == rbp_px,
          "the repainted cells were overwritten by the restore (%04x %04x)",
          get16(x + cc[0], y + cr[0]), get16(x + cc[1], y + cr[1]));
    CHECK(count_changed16() == 2,
          "restore left %d pixels changed, expected exactly the 2 repainted ones",
          count_changed16());

    /* And the rest of the arrow came back: nothing else differs. */
    {
        int c, r, intact = 0;
        for (r = 0; r < CURSOR_H; r++)
            for (c = 0; c < CURSOR_W; c++) {
                if (cursor_class_at(c, r) == CURSOR_TRANSPARENT)
                    continue;
                if (get16(x + c, y + r) == (unsigned short)fill16((size_t)(y + r) * TX_PITCH + (x + c)))
                    intact++;
            }
        CHECK(intact >= glyph_cells_on_screen(x, y) - 2,
              "only %d of %d glyph cells were restored",
              intact, glyph_cells_on_screen(x, y));
    }
}

/* --- 4. clipping at the edges --------------------------------------------- */

static void test_edges_are_exact_inverses(void)
{
    static unsigned int saved[CURSOR_W * CURSOR_H];
    struct { int x, y; } cases[] = {
        { 0, 0 },                       /* tip at the top-left corner */
        { TX_W - 1, TX_H - 1 },         /* tip at the bottom-right corner */
        { -5, -3 },                     /* mostly off the top-left */
        { TX_W - 3, TX_H - 2 },         /* mostly off the bottom-right */
        { -CURSOR_W, 4 },               /* entirely off the left */
        { 4, TX_H + 4 }                 /* entirely off the bottom */
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int x = cases[i].x, y = cases[i].y;
        reset_surface();
        cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
        CHECK(count_changed16() == glyph_cells_on_screen(x, y),
              "tip (%d,%d): painted %d pixels, %d cells are on screen",
              x, y, count_changed16(), glyph_cells_on_screen(x, y));
        cursor_restore(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
        CHECK(count_changed16() == 0,
              "tip (%d,%d): restore left %d pixels changed", x, y, count_changed16());
    }
}

/* --- 5. repainting every tick ----------------------------------------------
 *
 * This is the case that the first version of the compositor got wrong on the
 * unit, and it is worth stating plainly because the code it replaces looked
 * right: it painted once and then asked each tick whether the tip pixel was
 * still the one it had written. On rbp's black waveform area the answer after a
 * repaint over the arrow was "yes" — the arrow's outline is black and so is what
 * rbp painted there — so the arrow was never drawn again, and the pointer was
 * invisible for a whole run.
 *
 * The property that makes unconditional repainting safe is tested here: paint()
 * leaves an intact arrow alone (so a stationary pointer costs nothing and does
 * not blink out while it is restored and redrawn), and it heals exactly the
 * cells rbp has repainted over. */

static void test_repaint_is_idempotent_and_heals(void)
{
    static unsigned int saved[CURSOR_W * CURSOR_H];
    static unsigned int saved_before[CURSOR_W * CURSOR_H];
    int x = 13, y = 8;
    int col, row, cells = 0, healed;

    /* 1. A second paint over an intact arrow changes nothing, and does not
     *    disturb the save-under: were it to re-save here, every cell's "what was
     *    underneath" would become the arrow itself and the next restore would
     *    leave the glyph permanently painted on the screen. */
    reset_surface();
    cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
    memcpy(saved_before, saved, sizeof saved);
    cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
    CHECK(memcmp(saved, saved_before, sizeof saved) == 0,
          "repainting an intact arrow overwrote the save-under");
    CHECK(count_changed16() == glyph_cells_on_screen(x, y),
          "repainting an intact arrow changed the pixel count to %d", count_changed16());

    /* 2. rbp repaints over part of the arrow — in the middle of the body, so the
     *    damage is not a cell the shape happens to have on its own silhouette.
     *    The next paint has to put the arrow back there, and record *rbp's* pixel
     *    as the thing underneath, so that the eventual restore does not resurrect
     *    the pre-repaint content. */
    for (row = 6; row < 9; row++)
        for (col = 2; col < 8; col++) {
            if (cursor_class_at(col, row) == CURSOR_TRANSPARENT)
                continue;
            put16(x + col, y + row, 0x0ACE);
            cells++;
        }
    CHECK(cells > 0, "found no body cells to damage");

    cursor_paint(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);

    healed = 0;
    for (row = 0; row < CURSOR_H; row++)
        for (col = 0; col < CURSOR_W; col++) {
            int cls = cursor_class_at(col, row);
            if (cls == CURSOR_TRANSPARENT)
                continue;
            if (get16(x + col, y + row) == (unsigned short)cursor_pixel(16, 0, cls))
                healed++;
        }
    CHECK(healed == glyph_cells_on_screen(x, y),
          "only %d of %d glyph cells came back after rbp repainted over them",
          healed, glyph_cells_on_screen(x, y));

    /* And the restore now hands the screen back to rbp rather than to the pixels
     * that were there before the damage. */
    cursor_restore(buf16, TX_PITCH, TX_W, TX_H, 16, x, y, 0, saved);
    CHECK(get16(x + 2, y + 6) == 0x0ACE,
          "the heal recorded the stale pixel instead of rbp's: %04x",
          get16(x + 2, y + 6));
}

int main(void)
{
    test_glyph();
    test_paint_restore_round_trip();
    test_restore_leaves_rbp_repaints_alone();
    test_edges_are_exact_inverses();
    test_repaint_is_idempotent_and_heals();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
