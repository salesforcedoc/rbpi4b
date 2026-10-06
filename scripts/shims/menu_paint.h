/*
 * menu_paint.h -- what the top menu looks like, as pure pixel work.
 *
 * The shim's second drawing module (cursor_paint.c draws the mouse arrow) and it
 * keeps that module's central rule: every pixel written for the panel is a PURE
 * FUNCTION OF THE LAYOUT AND THE PALETTE -- never of what was already there -- so
 * the image is the same every time it is drawn and painting it twice is painting it
 * once. No destination is ever read, and there is no alpha.
 *
 * THE LABELS ARE ANTIALIASED, and that is the one place this needs stating
 * carefully, because the first version of this module said "no blending, no close
 * enough" and meant it: the labels were a 5x7 bitmap drawn as integer-scaled blocks,
 * so every pixel was one of eight palette values. They are now the device's own
 * typeface at 16 px (menu_font.h), which means a glyph edge is *partly* covered and
 * has to be blended. The rule above survives that intact, and the coverage is where
 * it lives: what a pixel gets is (bpp, class, coverage), where coverage comes from
 * the glyph's own atlas and the layout's arithmetic and not from the page. So
 *
 *   - coverage 0 writes EXACTLY the button colour and coverage 255 writes EXACTLY
 *     the label colour -- the ends of the range are exact values, unchanged from
 *     before, and only the edges in between are a blend;
 *   - the in-between is a per-channel blend in the bpp's own geometry, so it is a
 *     deterministic integer function with no rounding that could drift between two
 *     draws of the same image;
 *   - nothing is read. A pixel already holding the right blend is written again with
 *     the same value.
 *
 * The witness in menu_draw.c compares page pixels against exact palette values, so
 * this matters to it: its 15 sample points are chosen to be glyph-free, and
 * test_menu.c asserts that against the production point list rather than trusting
 * the arithmetic to stay clear of the labels.
 *
 * WHY THERE IS NO SAVE-UNDER, which is the other place this module departs from
 * cursor_paint.c and is worth stating. The arrow is small, it sits on top of
 * whatever rbp drew at that spot, and it comes and goes while rbp is idle -- so it
 * saves the pixels it covers and puts them back. The panel does not need any of
 * that, because rbp repaints its ENTIRE frame about 60 times a second
 * (docs/13-raspberrypi4.md:540-584: one fb page, RB_DFB_PRESENT=off, no damage
 * tracking, no UpdateRegion hook). The panel is painted in the gap between two of
 * those frames, and rbp's very next frame erases it. The restore is rbp's own
 * repaint; saving the under-image here would not just be wasted work, it would be
 * *wrong*: after a second of rbp drawing other things, slots captured before the
 * panel opened hold a UI that no longer exists, and restoring them would paint
 * stale pixels. So: paint the whole panel, exact values, every frame while it is
 * open, and do nothing at all when it is closed. (Each pixel is written
 * UNCONDITIONALLY; the first version compared first and wrote only on a
 * difference, and measurement on the unit killed that -- see menu_paint.c's
 * comment for the numbers. The comparison was free and the read was not.)
 *
 * This also disposes of the ghost bug that the save-under design has to be
 * careful about (highlighting the button under the finger changes the image, so
 * the slot captured for a pixel would hold the panel's own old value rather than
 * rbp's). With a full exact repaint there is no slot to capture and no ghost to
 * avoid -- the pressed image is simply a different whole image. If the assumption
 * is ever wrong on the unit -- a ghost survives the panel's close -- then rbp's
 * repaint is not full-frame after all, and the fallback is cursor_paint.c's
 * conditional restore driven from a thread; that is a fallback, not the design,
 * and this module is written so it could take one.
 *
 * FB PIXELS, NOT LOGICAL. The class of a pixel is computed in framebuffer pixels,
 * scaled from menu_zone.c's logical geometry by the picture rectangle the caller
 * measured with point_fit() -- so a glyph is never resampled on a panel that is
 * not 1280x800, and the letterforms stay crisp at any size. The hit test is
 * logical (where the finger is); the two disagree by at most a pixel on a scaled
 * panel, which is invisible and documented rather than papered over.
 */
#ifndef RBLIVE4_MENU_PAINT_H
#define RBLIVE4_MENU_PAINT_H

/* For MZ_COLS, which sizes struct menu_layout's arrays. The dependency runs one way
 * -- menu_zone.h is pure geometry with no includes of its own -- and it is the honest
 * direction: the panel's shape is menu_zone.c's, and the painter derives from it
 * rather than carrying a second copy of the column count. */
#include "menu_zone.h"

/* The panel, in the classes a pixel can be. The palette is a pure function of
 * (bpp, class) -- menu_pixel() -- exactly as cursor_pixel() is. */
enum {
    MENU_NONE = 0,      /* not the panel's pixel at all */
    MENU_FILL,          /* the bar's background, between the border and a button */
    MENU_BORDER,        /* the frame around the whole bar */
    MENU_DIV,           /* the one-pixel seam between two columns */
    MENU_BTN,           /* a button's background */
    MENU_BTN_PRESSED,   /* ...while a finger is on it */
    MENU_LABEL,         /* a glyph on a button */
    MENU_LABEL_PRESSED  /* ...on the pressed button (dark on the accent) */
};

/* Everything the painter needs to know about the framebuffer, measured by the
 * caller (menu_draw.c) with real_open/real_ioctl -- bypassing the shim's own lie
 * about the geometry, the fb_cursor.c:100-196 pattern. `bx/by/dw/dh` are the
 * picture rectangle that point_fit() chose inside the framebuffer; the panel is
 * drawn across the top of it, so on a letterboxed panel the bar does not run out
 * into the letterbox.
 *
 * pitch is in PIXELS per row, as fb_cursor.c counts it, not bytes. */
struct menu_view {
    void *pix;
    int   pitch;
    int   fb_w, fb_h;
    int   bpp;
    int   bx, by, dw, dh;
};

/* The layout: the panel's rectangle, the MZ_COLS button rects, where each label starts
 * and the line box the labels sit in, all in fb px, all computed once per image.
 * Every rect is inclusive at both ends, matching menu_zone.c's hit test.
 *
 * It is published because a caller that reads many pixels of ONE image should not pay
 * for the geometry per pixel. menu_draw.c's damage witness is that caller: fifteen
 * spread-out points per tick, two thousand ticks a second, and each point used to
 * rebuild all of this through menu_class_at(). The fields are the panel's geometry
 * and there is no reason for a caller to touch them. */
struct menu_layout {
    int x0, y0, x1, y1;          /* the panel */
    int bx0[MZ_COLS], bx1[MZ_COLS];
    int by0, by1;                /* the button band, shared by every column */
    int lx[MZ_COLS];             /* where each label starts */
    int lx1[MZ_COLS];            /* ...and where it ends, inclusive */
    int ly;                      /* the top of the line box, shared by every label */
    int ln[MZ_COLS];             /* label length, so the per-pixel path does no strlen */
    /* The seven column rects above tile the whole panel: MZ_BTN_W is MZ_LOGICAL_W
     * since the menu's eighth cell went (menu_zone.h's block). There is no cell
     * geometry beyond them. */
};

/* Sanity: the view can be drawn into at all. Callers check this once rather than
 * every frame; a view that fails it must never reach menu_paint(). */
int menu_view_ok(const struct menu_view *v);

/* Build the layout for one view. Cheap arithmetic, but not free: it is 6 columns of
 * scaled geometry and a walk of each label's advances, which is why a caller with
 * more than one pixel to classify builds it once. */
void menu_layout_make(const struct menu_view *v, struct menu_layout *L);

/* The class of one framebuffer pixel for a given image. Out of the panel is
 * MENU_NONE. `pressed` is the button 1..MZ_COLS currently under the finger, or 0.
 *
 * This is the production geometry -- menu_paint() classifies through this same
 * layout -- so test_menu.c pins what actually gets drawn, not a copy of it. */
int menu_class_at(const struct menu_view *v, int fx, int fy, int pressed);

/* ...and the same question with the layout already in hand, for a caller classifying
 * more than one pixel of the same image. Identical arithmetic; test_menu.c asserts
 * the two agree at every pixel of the panel, so the prepared-layout path cannot drift
 * away from the production one. The view must be one menu_layout_make() accepted --
 * this does not re-test it, and menu_class_at() does. */
int menu_class_in(const struct menu_layout *L, int fx, int fy, int pressed);

/* The VALUE menu_paint() writes at one pixel for one image: what menu_class_at()
 * says, run through menu_pixel(), and blended when the pixel is a partly covered
 * glyph edge.
 *
 * This exists because the class alone stopped being the whole answer when the
 * labels became antialiased -- a glyph edge is a blend of the button and label
 * colours at a coverage the class does not carry -- and the test that pins the
 * painter ("every pixel written is the pixel the classifier calls for") must
 * compare against the production function rather than re-implement the blend. It
 * is the same code path: menu_paint() is a loop over this function. */
unsigned int menu_value_at(const struct menu_view *v, int fx, int fy, int pressed);

/* The panel's rectangle in the framebuffer, inclusive both ends. */
void menu_panel_rect(const struct menu_view *v, int *x0, int *y0, int *x1, int *y1);

/* The damage witness's sample points -- menu_draw.c's menu_intact() -- as
 * panel-relative fractions in sixteenths, so they land in the same classes on a
 * panel that is not 1280x800. They are published here, next to the layout that
 * defines them, because the property that matters is a property of the IMAGE: every
 * one of them is glyph-free at every panel size, which is what lets the witness
 * compare the page against an exact palette value. menu_draw.c walks them and
 * test_menu.c asserts that property against this same list, so the two cannot
 * disagree about where the witness looks. */
int menu_witness_count(void);
void menu_witness_point(const struct menu_view *v, int i, int *fx, int *fy);

/* Draw the image: every pixel of the panel rect, exact values. */
void menu_paint(const struct menu_view *v, int pressed);

/* Draw only the columns whose bit is set in `mask` -- bit i is button i+1, so
 * (1u << MZ_COLS) - 1 covers all seven, which is the whole band, and 0 writes
 * nothing. THE REST OF THE PANEL IS LEFT ALONE, and that is the point of it.
 *
 * The only pixels whose value depends on `pressed` are inside the pressed button's own
 * column: class_in_layout() reaches its pressed test only after the column's x range
 * has decided which button a pixel belongs to (menu_paint.c). So repainting the
 * columns {old, new} over a copy of the image for `old` yields exactly the image for
 * `new` -- which is what lets menu_draw.c redraw two columns (2 x 182 x 96 cells,
 * ~2.4 ms) instead of reclassifying the whole panel (~7.4 ms) when a finger moves from
 * one button to the next. test_menu.c pins that identity pixel for pixel, at four
 * panel sizes, for every (old, new) pair.
 *
 * The columns and the web cell tile the panel exactly, so a full mask is a full paint and
 * no cell can be addressed twice or missed. Bits above MZ_COLS are ignored. */
void menu_paint_cols(const struct menu_view *v, int pressed, unsigned int mask);

/* One framebuffer pixel, as menu_paint() would read it. Exposed for the damage
 * witness in menu_draw.c, which has to answer "is the image still there?" without
 * paying for a full pass: same format knowledge, one module, so the two cannot
 * disagree about where a pixel lives. `fx`/`fy` are framebuffer pixels and must be
 * inside the view; out-of-range returns 0, which no class in the palette is. */
unsigned int menu_get(const struct menu_view *v, int fx, int fy);

/* The palette. 16 bpp is RGB565 and 32 bpp is XRGB8888, as cursor_pixel() has it;
 * an unknown depth is treated as 16 bpp, which is what this port's panel is. */
unsigned int menu_pixel(int bpp, int cls);

#endif /* RBLIVE4_MENU_PAINT_H */
