/*
 * fx_zone.h -- the BEAT FX panel's three touch controls: the CH SELECT cycle, the
 * effect picker, and the power toggle. PURE.
 *
 * WHY IT EXISTS. rbp's performance screen draws a BEAT FX panel on the right of the
 * deck, and its own `ui::touch_panel` family contains `BeatFxAndXPad`, `BeatFxMode_*`,
 * `BeatFxSelectItem1..4` and the in-panel quantize -- a panel/mode toggle, the X-PAD
 * and the quantize, and NOTHING that picks the effect, assigns its channel or powers it.
 * Those three reach rbp only as keycodes: today the FLX4's FX SELECT button steps the
 * type, its CH SELECT lever assigns the channel and its FX ON/OFF button toggles the
 * effect (map_flx4.c). The operator's ask, 2026-10-06:
 *
 *     "when i touch the ch select in performance view let it toggle from ch 1 -> ch 2
 *      -> master, when i touch the beat fx have a popup menu with all the fx available
 *      so i can select"
 *
 * and the power toggle is their follow-up of 2026-10-07, once the first two were on the
 * glass and confirmed ("yep the beat fx is good too"):
 *
 *     "when i press the beat fx label have the menu popup but if i press the actual
 *      label of the beat fx (eg delay) enable the beat fx and disable if i press it
 *      again"
 *
 * So all three controls are re-homed onto rbp's OWN drawing, which the operator chose
 * over shim-owned cells: a finger lands where the label already is. All three taps are a
 * WIRING job, not a discovery one -- every wire form is proven in this tree
 * (`send_rx_key(K_BFXCH 0x448c, OP_VALUE, CH_GLOBAL, want)` is map_flx4.c:493,
 * `send_rx_key(K_BFXTYPE 0x448b, OP_VALUE, CH_GLOBAL, pos)` is map_flx4.c:520, and the
 * toggle is `K_BFX 0x448d`, `add_note(CH_FXA, N_FX_ONOFF, K_BFX, CH_GLOBAL)` at
 * map_flx4.c:1087 -- the FLX4's own FX ON/OFF button, so a tap here is one toggle and
 * the shim keeps no state to desync).
 *
 * THE THREE RECTS ARE MEASURED OFF THE GLASS, and they are rbp's INK, not the panel.
 * The panel's grey plate is x 1090..1269; at its top rbp draws a grey "BEAT FX" header
 * bar, and below it a black effect-name cell and a filled CH SELECT value box, BOTH of
 * those inset from the plate by ten pixels on either side:
 *
 *     BEAT FX header   x 1090..1269   y  57..85    <- FX_HIT_HEADER (the full plate width)
 *     effect-name cell x 1100..1259   y  98..137   <- FX_HIT_NAME
 *     CH SELECT box    x 1100..1259   y 168..203   <- FX_HIT_CH
 *
 * A FOURTH rbp-drawn rect sits below those three and is deliberately NOT one of them:
 * the black BPM-detail cell (x 1100..1259, y 216..352), which reads
 * "125.0 BPM / 480 msec / 1 BEAT / QUANTIZE" and wears the same ten-pixel inset. It has
 * no binding here because it belongs to the momentary X/Y pad, not to this panel's three
 * controls -- see fxpad_zone.h, which carries the rect, the two mappings and the same
 * measured-swallow evidence. The two rect sets do not overlap: FX_HIT_CH ends at y 203
 * and the cell begins at y 216.
 *
 * The header bar and the y values of the two boxes come from the committed capture
 * work/fb_fx2.raw (2026-09-30); the x values of the boxes were corrected on 2026-10-06
 * against the LIVE frame, and the header's own bounds were re-measured off the live
 * framebuffer on 2026-10-07 (the grey (48,48,48) bar runs y 57..85 across the full
 * 1090..1269, above the plate's (32,32,32) which starts at y 86). Re-deriving all six is
 * one command against an image -- the plan of record (jiggly-puzzling-corbato.md) names
 * it -- and a layout change in rbp is the one thing this port cannot control; the mode
 * gate below is what stops a moved panel firing.
 *
 * THE SWALLOW WAS MEASURED BEFORE A LINE OF THIS WAS WRITTEN. An idle capture diff at
 * both boxes, tapping each twice with the shim doing nothing, changed **0 px in the
 * panel and 0 px in the pad strip**, while the positive control (the segmented
 * STATUS/BEAT FX toggle) moved 7098 px and 77921 px. The HEADER bar was measured the
 * same way on 2026-10-07 and needs no qualification: a tap at its centre changed **0 px
 * in the whole panel**, while the CH box tapped in the SAME run moved 726 px (rbp's
 * `1`->`2`). So rbp binds nothing to any of the three and withholding the reports costs
 * the operator nothing.
 *
 * PURE, LIKE prompt_zone.c AND util_zone.c. Nothing here reaches rbp, the framebuffer
 * or a clock of its own: the time arrives as an argument and both answers ("which
 * channel" and "which effect") leave as an `act` the caller sends. That is what lets
 * test_fx.c pin the geometry, the latch, the picker and the timeout on a host with no
 * Pi, no panel and no player -- and it is why the caller, not this file, is the one
 * that sends.
 */
#ifndef RBPI4B_FX_ZONE_H
#define RBPI4B_FX_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* --------------------------------------------------------------- the three rects
 *
 * Inclusive at both ends, in the menu's own 1280x800 logical space, and written as
 * literals rather than derived -- they are a measurement of rbp's drawing, and a
 * derived number would only hide which one moved when rbp's layout changes. The
 * comment above is the provenance; test_fx.c asserts each sits inside the plate and
 * that none overlaps another.
 */
#define FX_PANEL_X0  1090          /* the grey plate, margins included */
#define FX_PANEL_X1  1269
#define FX_PANEL_Y0    47
#define FX_PANEL_Y1   490

#define FX_HEADER_X0 1090          /* the grey "BEAT FX" header bar: the FULL plate width, */
#define FX_HEADER_X1 1269          /* unlike the two inset boxes below it -- rbp's own ink */
#define FX_HEADER_Y0   57
#define FX_HEADER_Y1   85

#define FX_NAME_X0   1100          /* the black effect-name cell rbp draws */
#define FX_NAME_X1   1259
#define FX_NAME_Y0     98
#define FX_NAME_Y1    137

#define FX_CH_X0     1100          /* the filled CH SELECT value box */
#define FX_CH_X1     1259
#define FX_CH_Y0      168
#define FX_CH_Y1      203

/* 160 x 40 logical px for each INSET box -- a comfortable finger target -- with 30 rows
 * of clear plate between them; the header bar is the full 180 x 29 above both. All
 * three are separated by plate, so none can be confused for another. Deliberately NOT
 * padded out to the plate: the drawn box is the visual promise, and growing a rect
 * towards a neighbour would let a tap near the boundary fire the wrong control. */

/* ---------------------------------------------------------------- the answers */
enum {
    FX_HIT_NONE = 0,        /* none of the three */
    FX_HIT_CH,              /* the CH SELECT value box: step the target */
    FX_HIT_HEADER,          /* the BEAT FX header bar: raise the picker */
    FX_HIT_NAME             /* the effect-name cell: toggle the Beat FX on/off */
};

enum {
    FX_ACT_NONE = 0,        /* no answer: the press missed or slid off */
    FX_ACT_CH,              /* cycle rbp's Beat FX channel */
    FX_ACT_PICK,            /* raise the effect picker */
    FX_ACT_POWER            /* toggle rbp's Beat FX on/off */
};

/* --------------------------------------------------------------- the picker box
 *
 * ONE COLUMN, FOURTEEN ROWS, which is the operator's own answer of 2026-10-06
 * ("One column, 14 rows") -- the same shape the FLX4's FX SELECT button steps, so the
 * order on the glass is the order their muscle memory already has.
 *
 * AND IT IS DRAWN OVER THE BEAT FX PANEL ITSELF, IN THE PANEL'S OWN RECTANGLE. That is
 * the operator's ask of the same day, made after seeing the first build centred on the
 * screen -- and corrected once, the second sentence arriving while the first was being
 * built:
 *
 *     "have the beatfx menu display on the left side with the same dimensions of the
 *      beatfx box and have it look visually similar (size, color, font). you don't
 *      need a title menu saying beat fx either"
 *     "shit, i meant on the right side not the left"
 *
 * So the box IS the plate's rectangle -- x 1090..1269, y 47..490, the same 180 x 444 --
 * and every number below is derived from FX_PANEL_* rather than typed, so rbp moving his
 * panel moves this box with it. It COVERS the panel while it is up, and that is what
 * "same dimensions" comes to: the box reads as the panel turning into a list, and the
 * control the finger just left is behind it rather than beside it.
 *
 *     +-------------------------------+  FX_Y0 == FX_PANEL_Y0   (MENU_FX_PLATE)
 *     |  FX_EDGE_Y                    |
 *     |   +-----------------------+   |  row 0    FX_ROW_H     (MENU_FILL, no frame)
 *     |   +-----------------------+   |  row 1    FX_ROW_GAP between
 *     |             ...               |  ...
 *     |   +-----------------------+   |  row 13
 *     |  FX_EDGE_Y                    |
 *     +-------------------------------+  FX_Y1 == FX_PANEL_Y1
 *
 * FX_EDGE_X is TEN and not a round number of the shim's own, because it is RBP'S: his
 * black effect-name cell is inset ten pixels from the plate on either side, so a row
 * here is 160 px wide and lands on exactly the same width as the cell it replaces.
 * FX_EDGE_Y is six because fourteen cells have to fit where rbp puts two -- 2*6 + 14*29
 * + 13*2 = 444 exactly, which test_fx.c asserts rather than trusts.
 *
 * THE ROWS WEAR RBP'S OWN COLOURS (menu_paint.h): the bed is his plate, a row is his
 * black cell with his white ink, and the row under the finger takes the colour he fills
 * a SELECTED box with. Nothing here draws a frame round a row, because his cells have
 * none -- the box's whole style is "what the panel on the right looks like", which is
 * what the operator asked for in one word: "similar".
 *
 * THERE IS NO TITLE AND NO RULE. The operator struck the "BEAT FX" caption, so there is
 * no header band and the rows start FX_EDGE_Y below the top edge; the panel this box
 * mirrors is already labelled on the glass, and a box that repeats the label of the
 * control that raised it has nothing to say about itself.
 *
 * AND IT COVERS THE PANEL, WHICH IS THE ONLY THING ON THE GLASS IT CAN COVER. That is
 * what the operator asked for, and it costs nothing: the two hit rects this module owns
 * are inside the plate, and the picker is asked FIRST in pointsrc.c's ladder, so while
 * the box is up every report over the panel is the picker's and neither of rbp's
 * controls underneath can fire. Its first nine rows also sit under the swipe-down band's
 * strip -- inherited, because rbp's own plate starts at y 47 too -- and the RIGHT-hand
 * drawer is the same 180 px column at the same edge. Neither can be open at the same
 * time: pointsrc.c's FX rung refuses while any drawer or the band is open, and it is
 * asked before either of those can open on a report the picker is holding. Nothing in
 * THIS file enforces any of that.
 */
#define FXLIST_ROWS     14

/* THE SAME RECTANGLE AS THE PLATE, spelled as the plate's own bounds rather than as
 * four literals so it cannot drift from them: this box is drawn over that panel, and a
 * shim-side copy of its geometry would be the third place rbp's layout is written down.
 * The width and height fall out of the bounds and are not used to derive them. */
#define FX_W        (FX_PANEL_X1 - FX_PANEL_X0 + 1)      /* 180 */
#define FX_H        (FX_PANEL_Y1 - FX_PANEL_Y0 + 1)      /* 444 */
#define FX_X0       FX_PANEL_X0                          /* 1090 */
#define FX_Y0       FX_PANEL_Y0                          /*   47 */
#define FX_X1       FX_PANEL_X1                          /* 1269 */
#define FX_Y1       FX_PANEL_Y1                          /*  490 */

#define FX_EDGE_X   10          /* the plate down each side of a row: RBP's inset, not ours */
#define FX_EDGE_Y   6           /* top and bottom, so fourteen rows fit the plate's height */
#define FX_ROW_H    29          /* MENU_FONT_LINE is 24 rows at this atlas's 19 px, so a
                                 * row keeps 5 px of air and 28 would be the floor --
                                 * fx_paint.c's ok() asks exactly this question */
#define FX_ROW_GAP  2

/* The rows' shared rect: the columns span this and every row is the same width, so a
 * row's hit target and the ink it wears are the same pixels -- 160 px, rbp's own cell. */
#define FXLIST_ROW_X0   (FX_X0 + FX_EDGE_X)              /* 1100 */
#define FXLIST_ROW_X1   (FX_X1 - FX_EDGE_X)              /* 1259 */

/* Row i (0-based) top -- the arithmetic in ONE place, so the painter, the hit test and
 * the test cannot disagree about where a row is. */
#define FXLIST_ROW_TOP(i) (FX_Y0 + FX_EDGE_Y + (i) * (FX_ROW_H + FX_ROW_GAP))
#define FXLIST_ROW_Y0(i)  FXLIST_ROW_TOP(i)
#define FXLIST_ROW_Y1(i)  (FXLIST_ROW_Y0(i) + FX_ROW_H - 1)

/* The box goes away on its own, the same rule and the same number as the USB chooser's
 * (prompt_zone.h's PR_TIMEOUT_MS). The picker covers rbp's pad rows while it is up, and
 * nothing about choosing an effect is urgent enough to be worth being modal over. */
#define FX_TIMEOUT_MS 10000

/* ------------------------------------------------------------------- the input */

/* Which of the three rects a logical point is in, or FX_HIT_NONE for the plate between
 * them, the plate's margins, or anywhere else on the screen. */
int fx_hit(int x, int y);

/* Feed one pointer report. Returns MZ_FEED_TAKEN for a press that began inside any of
 * the three rects -- INCLUDING its release, and including a press that then slid off,
 * because a control that swallowed the press must see the release or it goes deaf (the
 * rule declined-press-must-still-see-release records) -- and MZ_FEED_NONE everywhere
 * else, so rbp keeps every report this module is not about.
 *
 * THE EFFECT-NAME CELL FIRES ON THE PRESS, NOT ON THE RELEASE -- the operator's own
 * answer, 2026-10-07 ("when i press the actual label of the beat fx (eg delay) enable
 * the beat fx"), and the same edge the HOT CUE pads moved to. A power toggle is a
 * button: it acts the moment the finger goes down, so *act is FX_ACT_POWER on the
 * UP->DOWN EDGE that lands on the name cell, and the release answers nothing (it is
 * swallowed, and answers FX_ACT_NONE). A run of downs fires nothing a second time.
 *
 * THE HEADER BAR AND THE CH BOX KEEP THE FIRE-ON-RELEASE ANCHOR RULE. A RELEASE ANSWERS
 * FOR THE RECT THE PRESS BEGAN ON, AND ONLY IF THE RELEASE IS STILL ON IT, with the
 * rect taken from the RELEASE's own coordinates rather than from the tracked one: a fast
 * flick can lift at a moved position with no move report between, and a release that
 * moved must not answer for the box the finger has already left. That is
 * prompt_zone.c:256-285's anchor rule, and it is what stops a drag that drifted onto the
 * panel from firing -- and, on the name cell, what stops a SLIDE from the header onto
 * the name cell from toggling.
 *
 * WHAT A QUALIFYING RELEASE ANSWERS is the caller's to send: FX_ACT_CH for the CH
 * SELECT box, FX_ACT_PICK for the header bar. Every other release -- a miss, a slide
 * off, or the release of the name cell (which fired on its press) -- answers FX_ACT_NONE
 * and is still swallowed. */
int fx_feed(int down, int x, int y, int *act);

/* -------------------------------------------------------------------- the picker
 *
 * The state machine is prompt_zone.c's, deliberately and down to the field names,
 * because that shape has already been through a real drill on this unit. The rules it
 * carries, restated because each one is load-bearing:
 *
 *   - It is asked FIRST in pointsrc.c's ladder, above the band, the drawers and rbp,
 *     so while it is up it owns EVERY report wherever it lands -- a press that
 *     dismisses must not also press whatever is underneath it.
 *   - NOTHING IS INHERITED. It opens with no press behind it (the raising tap fired on
 *     a release) and with no row armed.
 *   - It FIRES ON RELEASE, and only when the release's own coordinates are on the row
 *     the press anchored to.
 *   - A MISS, A SLIDE, OR A TAP OUTSIDE CLOSES IT AND SENDS NOTHING -- an honest no-op
 *     rather than a silent action.
 *   - IT NEVER EXPIRES WHILE A FINGER IS DOWN, so rbp always gets the release for a
 *     press the box swallowed.
 *
 * THERE IS NO "CURRENT EFFECT" HIGHLIGHT, and that is a measurement and not an
 * omission. rbp's type is read by `DjEngineIF::getBeatEffectType @0x4d514` ->
 * `ME_SINGLETON` -> `MixerEngine+0x58` -> `BeatEffectManager` -> `[+0x50]`, and that
 * word was measured on the unit on 2026-10-06 reading **0 at every switch position**
 * while the panel showed SLIP ROLL, REVERB and VINYL BRAKE in turn. The FLX4's own
 * cursor falls back for the same reason (map_flx4.c's seed, visible in the log as a
 * first step of "type position 1"). A light driven by a word that is always 0 would sit
 * on DELAY forever, which is worse than no light at all. So the picker opens with
 * nothing marked, and the operator's own FLX4 panel remains the honest witness for
 * which effect is live.
 */

/* Raise the box, or re-raise it if it is already up (which restarts the clock).
 * `now_ms` is the caller's monotonic milliseconds; this module keeps no clock. A press
 * in flight is NOT inherited and no row is armed. */
void fxlist_open(unsigned long long now_ms);

/* Put it away. Safe on a shut box, and safe from the tick as well as the touch thread
 * -- see fxlist_expire(). */
void fxlist_close(void);

int fxlist_is_open(void);

/* The wall-clock milliseconds at which the box closes itself. Only meaningful while
 * fxlist_is_open(). */
unsigned long long fxlist_deadline(void);

/* The self-dismissal, asked from wherever the clock is watched. Returns 1 on the call
 * that closed it and 0 otherwise, so a caller can log the transition once.
 *
 * IT IS NOT ONLY THE TOUCH THREAD THAT ASKS: pointsrc.c's read loop otherwise wakes
 * only on an event, so it waits in slices while the box is up -- exactly as it already
 * does for the USB chooser -- and asks this at each one. That keeps ONE writer of this
 * state (the touch thread) and needs no lock: menu_draw.c only ever READS it. */
int fxlist_expire(unsigned long long now_ms);

/* Drop everything, including a press in flight. pointsrc.c calls this when the pointer
 * device goes away, beside fx_reset() and the rest. */
void fxlist_reset(void);

/* The row (1..FXLIST_ROWS) under a logical point, or 0 for a gap, the plate's margin,
 * or off the box entirely. */
int fxlist_row_at(int x, int y);

/* The row the finger is on right now, or 0 -- the LIVE row, not the anchored one: a
 * finger that has slid to another row highlights where it is, while what it will
 * answer is still where it started (fxlist_feed()'s fire-on-release rule). */
int fxlist_pressed(void);

/* Row `row`'s inclusive rect, the same rect fxlist_row_at() tests, so the ink and the
 * hit target are the same pixels. Out of range answers an empty rect. */
void fxlist_row_rect(int row, int *x0, int *y0, int *x1, int *y1);

/* The box's own inclusive rect. */
void fxlist_box_rect(int *x0, int *y0, int *x1, int *y1);

/* Row `row`'s label, 1..FXLIST_ROWS, or NULL out of range. The table is MEASURED, not
 * derived -- see fx_zone.c -- and it is data: a wrong name is a one-line fix here and
 * never a logic change. */
const char *fxlist_row_label(int row);

/* Feed one pointer report, exactly as pointsrc.c's funnel hands them out.
 *
 * WHILE THE BOX IS UP IT OWNS EVERY REPORT, wherever it lands, so the return is
 * MZ_FEED_TAKEN for everything while open and MZ_FEED_NONE the moment it is shut --
 * rbp's stream is untouched everywhere else by construction, not by a rectangle test.
 * A release with no press behind it is taken too but does NOT close the box: nothing
 * was dismissed, so there is nothing to dismiss.
 *
 * `*act` is the row's SWITCH POSITION (0..FXLIST_ROWS-1) when a qualifying release
 * lands on a row, and -1 for anything else -- the caller sends it as
 * `send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, pos)`. It is a position and not a name
 * by design: fx_zone.c's table is what puts a word on the glass, and the wire value is
 * the position rbp's own switch uses. */
int fxlist_feed(int down, int x, int y, int *act);

/* --------------------------------------------------------------- the whole zone */

/* Drop the zone's own latch as well as the picker's. pointsrc.c calls this from both
 * of its reset blocks, beside menu_reset() and the rest, so neither a half-typed
 * channel cycle nor a box can survive a pointer device that went away. */
void fx_reset(void);

#endif /* RBPI4B_FX_ZONE_H */
