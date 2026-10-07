/*
 * hc_zone.h -- the HOT CUE pad row: a touch on a pad triggers its cue, and ONLY when
 * a cue is registered on it. PURE.
 *
 * WHY IT EXISTS. rbp's performance screen draws a HOT CUE grid across the bottom of
 * each deck -- four cells to a row, two rows, eight pads, lettered A..D and E..H. The
 * operator's ask, 2026-10-06:
 *
 *     "also on the performance screen, make hotcues activatable with touch.  when i
 *      press a hotcue, but only if one is registered to it.  if there is no active
 *      hotcue do nothing and do not create a hotcue."
 *
 * THE SECOND SENTENCE IS THE WHOLE FEATURE, and the reason it is written as a gate and
 * not as a nicety. On real Pioneer gear an unlit HOT CUE pad STORES a cue at the
 * current position. So the failure this module exists to prevent is not "the tap did
 * nothing" -- it is "the tap created a hot cue on a pad the operator thought was
 * empty", on their own track, mid-set. Every read below therefore fails towards NO,
 * and test_hc.c pins that direction by name.
 *
 * THE SWALLOW WAS MEASURED BEFORE A LINE OF THIS WAS WRITTEN, and the run carried its
 * own positive control (2026-10-06, shim doing nothing, `work/tap.py` writing records
 * straight into rbp's pipe):
 *
 *     INFO control (control)  549,690 px changed, all 16 bands   -- twice, open+shut
 *     all 16 pad cells          416 px -- the idle blinker, i.e. NO response
 *
 * The control is rbp's own `◈ INFO` at logical (1211,25); the 416 px is the four small
 * red marks at x 32..725, y 605..625 that toggle on their own, so a run showing only
 * those has shown nothing. All eight cells on BOTH decks, in BOTH rows, were tapped.
 * rbp binds nothing to the pad row, so withholding these reports costs the operator
 * nothing -- which is the same evidence the BEAT FX zone rests on, with a control that
 * this time actually fired.
 *
 * THE TWENTY-FOUR NUMBERS ARE A MEASUREMENT, not a derivation. Re-derived from a live
 * /dev/fb0 capture on 2026-10-06 by scanning the pad strip for its drawn edges:
 *
 *     HOT CUE label row     y 499..509        (rbp's own caption, above the grid)
 *     row 1 cells           y 518..537        20 rows
 *     gap                   y 538..547        10 rows
 *     row 2 cells           y 548..567        20 rows
 *     DECK strips           y 581..
 *
 *     deck 1 cells  x  11..158  168..315  325..472  482..629      first cell at 11
 *     deck 2 cells  x 651..798  808..955  965..1112 1122..1269    first cell at 651
 *
 * 148 px wide, 9 px of gutter, pitch 157; the screen's margins are 11 left and 10
 * right, and 21 px separate the decks. docs/07-touch.md's performance-screen table
 * carries the same rows and agrees, and that is the independent check: one source
 * would only be this file's opinion restated.
 *
 * 20 ROWS IS A THIN TARGET and it is deliberately left thin. The drawn cell is the
 * visual promise (the rule fx_zone.h states for its two boxes), and padding these
 * rects out to fill the gutter would make a tap in the 10-row gap -- visibly neither
 * pad -- fire one of them. A touch that lands in the gutter does nothing at all.
 *
 * PURE, LIKE fx_zone.c AND prompt_zone.c. Nothing here reads rbp, the framebuffer or a
 * clock: the three gate values arrive as arguments and the answer leaves as a pad
 * number the caller sends. test_hc.c pins the geometry, the latch, the slide rule and
 * the gate on a host with no Pi, no panel and no player.
 */
#ifndef RBPI4B_HC_ZONE_H
#define RBPI4B_HC_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* ------------------------------------------------------------- the drawn grid
 *
 * Inclusive at both ends, in the menu's own 1280x800 logical space, and written as
 * literals rather than derived -- they are a measurement of rbp's drawing, and a
 * derived number would only hide which one moved when rbp's layout changes. The
 * comment above is the provenance; test_hc.c asserts that the sixteen cells tile
 * their two spans exactly, with no overlap and no double-counted gutter.
 */
#define HC_CELL_W       148     /* a drawn cell */
#define HC_CELL_PITCH   157     /* cell + the 9 px gutter between */
#define HC_COLS           4     /* per row, per deck */

#define HC_ROW1_Y0      518
#define HC_ROW1_Y1      537     /* A..D */
#define HC_ROW2_Y0      548
#define HC_ROW2_Y1      567     /* E..H */

#define HC_DECK1_X0      11     /* deck 1's first cell */
#define HC_DECK2_X0     651     /* deck 2's first cell */

/* The two decks' cells do not meet: deck 1 ends at 629 and deck 2 starts at 651, so
 * 630..650 is nobody's. This is the split the lookup uses, and it sits inside that gap
 * -- but the within-deck test below is what actually rejects a point there, so a split
 * anywhere in 630..650 gives the same answer. */
#define HC_DECK_SPLIT   640

#define HC_PADS           8     /* per deck: pads 1..8, rbp's own numbering */
#define HC_ROWS           2

/* --------------------------------------------------------------- rbp's own gate
 *
 * HOT CUE is pad mode **0** and that is rbp's own number, not a guess: rbp_abi.h's
 * UI_GET_PADMODE carries the derivation (UiGetPadMode @0xfd3cc returns 0 HOT CUE, 1
 * AUTO BEAT LOOP, 2 SLIP BEAT LOOP, 3 BEAT JUMP) and the measurement -- injecting ch0
 * notes 27/30/32/34 through the shim's own sequencer port moved deck 1's value
 * 3 -> 0, 1, 3, 2 exactly, with deck 2's held at 0.
 *
 * THE MODE GATE IS NOT DECORATION. rbp's eight pad keycodes mean whatever the CURRENT
 * pad mode makes them mean -- K_PAD1+p is a hot cue in HOT CUE, a 1/8 beat loop in
 * AUTO BEAT LOOP, a beat jump in BEAT JUMP -- so a tap on a cell while the deck is in
 * any other mode would send that mode's action. Engaging a beat loop because a finger
 * landed on a drawn pad is exactly as unwanted as creating a cue, and it is the same
 * fix. A deck not in HOT CUE mode gets nothing from this module, and neither does one
 * whose mode cannot be read.
 */
#define HC_PAD_MODE_HOT   0

/* THE WHOLE RULE, as a pure function so test_hc.c can pin it -- and pinned in both
 * directions, because "fires when it should not" is the defect that costs the operator
 * a cue and "does not fire when it should" is the one they would report.
 *
 *   browse_mode  getBrowseMode()      BROWSE_MODE_PLAY (1) means the performance screen
 *   pad_mode     UiGetPadMode(deck)   0 means HOT CUE; -1 means the read failed
 *   registered   isRegisteredHotCue() non-zero means a cue IS on that pad
 *
 * ALL THREE TERMS MUST HOLD, and every failure lands on 0: a pad mode that could not be
 * read is -1, and a registration query that could not be made is 0. There is no fourth
 * branch and no default that fires. */
int hc_may_fire(int browse_mode, int pad_mode, int registered);

/* ------------------------------------------------------------------- the input */

/* Which cell a logical point is on: 1 and *deck 0/1 with *pad 1..8, or 0 for the
 * gutter, the gap between the decks, the label row, or anywhere off the grid. */
int hc_cell(int x, int y, int *deck, int *pad);

/* Cell `pad` (1..8) of `deck` (0/1), inclusive, as the same rect hc_cell() tests --
 * so the ink, the hit target and the test cannot disagree. Out of range answers an
 * empty rect. */
void hc_cell_rect(int deck, int pad, int *x0, int *y0, int *x1, int *y1);

/* rbp's keycode for pad `pad` (1..8), or 0 out of range.
 *
 * IT IS A FUNCTION BECAUSE OF THE OFF-BY-ONE. rbp's arithmetic is
 * `pad = keycode - 0x4116`, read straight off the instruction stream of
 * ui::Player::onHotCueEvent @0x2f5720 (`sub sl, r8, #16640` / `sub sl, sl, #22`, with
 * the range test on `keycode - 0x4117` in 0..7), so pad 1 is K_PAD1 0x4117 and pad 8 is
 * 0x411e. Getting this wrong by one does not produce a pad that does nothing: it
 * triggers the NEIGHBOURING cue, and on an empty neighbour it CREATES one -- which is
 * the exact harm the whole feature is gated against, arriving through the arithmetic
 * instead of through the gate. test_hc.c pins all eight. */
int hc_pad_keycode(int pad);

/* Feed one pointer report. Returns MZ_FEED_TAKEN for a press that began on a cell --
 * INCLUDING its release, and including a press that then slid off, because a control
 * that swallowed the press must see the release or it goes deaf (the rule
 * declined-press-must-still-see-release records) -- and MZ_FEED_NONE everywhere else,
 * so rbp keeps every report this module is not about.
 *
 * A PAD FIRES ON THE PRESS, NOT ON THE LIFT. A hot cue is a jump: on real gear the pad
 * acts the moment it goes down, so *deck and *pad are filled on the UP->DOWN EDGE that
 * lands on a cell, and the release answers -1 -- it is swallowed, never fired. The
 * operator's own correction, 2026-10-07: "it should trigger on the press not the
 * release". A run of downs while the finger stays down fires NOTHING: the press it
 * belongs to already did, and a second fire per finger is exactly the double-trigger
 * the edge rule exists to prevent.
 *
 * AND THE ANCHOR IS SET ON THE UP->DOWN EDGE ONLY. A touch panel sends a RUN of down
 * reports while a finger is on the glass, so a second `down` is the same finger moving,
 * not a second press -- re-anchoring on it would let a drag across the pad row fire
 * whatever it crossed. A gesture that BEGINS off the grid is rbp's for its whole life,
 * including a slide that crosses the pads: it is never adopted.
 *
 * *deck and *pad are filled (0/1 and 1..8) on the firing press and left at -1
 * otherwise, so "the module took the report" and "the caller should send a pad key"
 * are two separate questions -- a run, a swallowed slide and a miss are all taken or
 * not taken on their own terms, and none of them fires.
 *
 * WHETHER IT MAY ACTUALLY FIRE IS NOT DECIDED HERE. This is geometry and a latch; the
 * three gate reads are the caller's, because they need rbp. */
int hc_feed(int down, int x, int y, int *deck, int *pad);

/* Is a press currently latched? The caller needs this because a press it took must be
 * handed its release even after rbp has left the performance screen -- the screen is
 * re-read on every report (rbp can leave it under a finger that is down), so without
 * this the release would reach rbp as an up for a down it never saw. */
int hc_latched(void);

/* Drop the latch. pointsrc.c calls this from both of its reset blocks, beside
 * fx_reset() and the rest, so a pad cannot stay latched across a pointer device that
 * went away. */
void hc_reset(void);

#endif /* RBPI4B_HC_ZONE_H */
