/*
 * hc_zone.c -- see hc_zone.h. Geometry, the latch, and the one rule, all pure.
 */
#include "hc_zone.h"
#include "rbp_abi.h"        /* BROWSE_MODE_PLAY, and nothing else is taken from it */

/* --------------------------------------------------------------------- the gate */

int hc_may_fire(int browse_mode, int pad_mode, int registered)
{
    return browse_mode == BROWSE_MODE_PLAY
        && pad_mode == HC_PAD_MODE_HOT
        && registered > 0;
}

/* ---------------------------------------------------------------- the keycodes */

int hc_pad_keycode(int pad)
{
    if (pad < 1 || pad > HC_PADS)
        return 0;
    return K_PAD1 + (pad - 1);          /* pad = keycode - 0x4116; see hc_zone.h */
}

/* ---------------------------------------------------------------- the geometry */

/* Deck `deck`'s cell `col` (0-based) starts here. Deck 1's first cell is at 11 and
 * deck 2's at 651; both then step by the same pitch, which is what makes the two rows
 * of a deck line up with each other on the glass. */
#define HC_COL_X0(deck, col) ((deck) ? HC_DECK2_X0 + (col) * HC_CELL_PITCH \
                                     : HC_DECK1_X0 + (col) * HC_CELL_PITCH)

void hc_cell_rect(int deck, int pad, int *x0, int *y0, int *x1, int *y1)
{
    int row, col;

    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = -1;
    if (y1) *y1 = -1;
    if (deck < 0 || deck > 1 || pad < 1 || pad > HC_PADS)
        return;

    row = (pad - 1) / HC_COLS;              /* 0 -> row 1 (A..D), 1 -> row 2 (E..H) */
    col = (pad - 1) % HC_COLS;

    if (x0) *x0 = HC_COL_X0(deck, col);
    if (y0) *y0 = row ? HC_ROW2_Y0 : HC_ROW1_Y0;
    if (x1) *x1 = HC_COL_X0(deck, col) + HC_CELL_W - 1;
    if (y1) *y1 = row ? HC_ROW2_Y1 : HC_ROW1_Y1;
}

int hc_cell(int x, int y, int *deck, int *pad)
{
    int row, d, col, dx;

    if (y >= HC_ROW1_Y0 && y <= HC_ROW1_Y1)
        row = 0;
    else if (y >= HC_ROW2_Y0 && y <= HC_ROW2_Y1)
        row = 1;
    else
        return 0;                       /* the label row, the gutter, the deck strip */

    d = (x < HC_DECK_SPLIT) ? 0 : 1;
    dx = x - (d ? HC_DECK2_X0 : HC_DECK1_X0);
    if (dx < 0)
        return 0;                       /* left of the deck's first cell (only reachable
                                         * for deck 2, i.e. the gap at 630..650) */
    col = dx / HC_CELL_PITCH;
    if (col >= HC_COLS)
        return 0;                       /* right of the deck's last cell */
    if (dx - col * HC_CELL_PITCH >= HC_CELL_W)
        return 0;                       /* the 9 px gutter between two cells */

    if (deck) *deck = d;
    if (pad)  *pad = row * HC_COLS + col + 1;
    return 1;
}

/* --------------------------------------------------------------------- the latch */

/* THE ANCHOR IS SET ON THE UP->DOWN EDGE AND ON NOWHERE ELSE, which is fx_zone.c's
 * `was_down` rule and the reason it is a rule. A touch panel does not send "a press"
 * and then "moves": it sends a run of DOWN reports, one per sample, for as long as the
 * finger is on the glass. So a second `down` is not a second press -- it is the SAME
 * finger, and re-anchoring on it would make the anchor track the finger, which turns
 * "the cell the press began on" into "the cell the finger was over when it lifted" and
 * fires the pad the finger merely crossed on its way. That is not a hypothetical: the
 * first draft of this file re-anchored, and test_hc.c's slide case failed on it.
 *
 * `anchor_deck` is -1 both when no gesture is in flight and when the gesture began
 * off the grid -- and in the second case it stays -1 for the gesture's whole life, so a
 * finger that starts on the glass and slides across the pad row is rbp's from its first
 * report to its last. Adopting it on the slide would let a drag over the pads trigger
 * whatever it ended on. */
static int was_down;
static int anchor_deck = -1;

int hc_latched(void)
{
    return was_down && anchor_deck >= 0;
}

void hc_reset(void)
{
    was_down = 0;
    anchor_deck = -1;
}

int hc_feed(int down, int x, int y, int *deck, int *pad)
{
    int d, p;

    if (deck) *deck = -1;
    if (pad)  *pad = 0;

    if (!down) {
        /* A release we never saw the press for is not ours, and neither is the lift of
         * a gesture that began off the grid. rbp may keep both. */
        if (!was_down || anchor_deck < 0) {
            was_down = 0;
            anchor_deck = -1;
            return MZ_FEED_NONE;
        }
        /* THE LIFT FIRES NOTHING AND IS STILL OURS. The press it belongs to fired
         * already (below), so this is a release with nothing left to do but be
         * swallowed -- handing rbp a bare up would be an event it never had a down for
         * (declined-press-must-still-see-release). *deck and *pad stay -1: the caller must
         * send no key here, and that is the assertion, not a side effect. */
        was_down = 0;
        anchor_deck = -1;
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        /* THE UP->DOWN EDGE, AND THE ONLY PLACE A PAD EVER FIRES. A hot cue is a jump:
         * on real gear the pad acts the moment it goes down, so the cell is answered
         * HERE and not at the lift, and this is the operator's own correction of the
         * first release-firing build ("it should trigger on the press not the release",
         * 2026-10-07). */
        was_down = 1;
        if (hc_cell(x, y, &d, &p)) {
            anchor_deck = d;
            if (deck) *deck = d;
            if (pad)  *pad = p;
            return MZ_FEED_TAKEN;
        }
        /* A press that began outside every cell is NOT this module's, and `anchor_deck`
         * stays -1 for the WHOLE gesture -- it is not cleared until the release. */
        anchor_deck = -1;
        return MZ_FEED_NONE;
    }

    /* A run: the finger is still down. Movement does not move the anchor, the reports
     * stay swallowed so the release lands on a feeder that still knows a press is in
     * flight (declined-press-must-still-see-release), and NOTHING here fires -- the
     * press this run belongs to already did, and a second fire per finger is the
     * double-trigger the edge rule exists to prevent. */
    if (anchor_deck < 0)
        return MZ_FEED_NONE;            /* still a gesture that began elsewhere */
    return MZ_FEED_TAKEN;
}
