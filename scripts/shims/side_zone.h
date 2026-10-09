/*
 * side_zone.h -- the left/right edge drawers' geometry, hit test and gesture.
 *
 * THE FEATURE. The operator asked for one drawer per edge carrying that deck's
 * transport and that channel's fader: *"can you add a side swipe on each side with
 * cue/play buttons for each deck and a fader control for each channel? deck 1 on
 * the left, deck 2 on the right"*. It exists because CUE, PLAY and the channel
 * faders are otherwise reachable only from the FLX4, and rbp builds its channel
 * faders at ZERO -- so a machine with no FLX4 has no transport and, worse, no way
 * to open a channel at all (docs/09-audio.md:667-714).
 *
 * This is menu_zone.h's sibling and is written to the same rule: pure C, no rbp
 * address, no env, no I/O, no clock. Everything it answers can be pinned on the
 * host, and it says WHICH control; how the key or the fader value reaches rbp is
 * the caller's (pointsrc.c).
 *
 * COORDINATES ARE PANEL-LOCAL, AND THAT IS THE WHOLE MIRROR. The two panels are
 * mirror images, so rather than writing every rectangle twice this module works in
 * a frame whose origin is the panel's OUTER edge: local x = 0 is the screen edge
 * the drawer slides in from and local x grows inward. For the left panel
 * `lx == x`; for the right one `lx == MZ_LOGICAL_W - 1 - x` (side_local_x()).
 * Every rule below -- the entry column, "inward" for the opening swipe, "outward"
 * for the dismissal, the drawn rectangles -- is then written ONCE and holds for
 * both edges. menu_zone.h's coordinates are logical and absolute, pre-reflection;
 * these are local to a panel, and side_abs_x() is the one place that becomes an
 * absolute logical x again.
 *
 * BOTH DRAWERS AT ONCE, which is what the operator asked for one round on -- *"allow
 * for both side panels to be visable at the same time and to accept input"*. Each
 * edge has its own plane (drmband.c holds the card and master once and refcounts
 * them), so the two are independent: either, both, or neither. What is still
 * exclusive is the top BAND's plane, which overlaps a drawer at the corners of the
 * glass -- opening a drawer tears the band's plane down and the last drawer to close
 * gives it back (menu_draw.c's side_plane_sync()). A consequence the operator will
 * see and should not read as a bug.
 *
 * WHAT A PRESS IS OFFERED TO, now that two panels can be out: the drawer whose panel
 * contains it, then the entry column of a SHUT drawer (so the second drawer can be
 * swiped out while the first is up). Anything else -- the glass in the middle, which
 * is rbp's own screen -- is handed straight back. There used to be a third step, an
 * "away" dismissal that closed every open drawer; the operator reversed it (*"the
 * side bar should stay up until i swipe them away"*), so it is gone.
 */
#ifndef RBPI4B_SIDE_ZONE_H
#define RBPI4B_SIDE_ZONE_H

#include "menu_zone.h"       /* MZ_LOGICAL_W, MZ_LOGICAL_H, MZ_STRIP_Y1, MZ_FEED_* */

/* Which edge. The values are also the deck/channel number minus one and the array
 * index in this module's own state, which is why they are 0/1 and not 1/2. */
#define SZ_LEFT   0
#define SZ_RIGHT  1

/* ---------------------------------------------------------------------------
 * TWO CONTACTS, BECAUSE THE TWO CHANNEL FADERS ARE THE ONE CONTROL PAIR A DJ RIDES
 * WITH BOTH HANDS. The operator, on the glass: *"if i try to drag both volume meters
 * it gets confused and only one of them changes"* (2026-10-06).
 *
 * Every press in this module belongs to a POINTER, and every press's state is keyed
 * by (side, pointer) -- so two fingers on the two faders are two presses that never
 * touch each other's bookkeeping. `SZ_PTR_MAIN` is the primary contact, the one the
 * whole rest of the shim is built around (the band, the browser window, the USB STOP
 * chooser and rbp's own pointer path all read it and only it); `SZ_PTR_ALT` exists
 * for the drawers alone.
 *
 * WHY IT IS NOT JUST A SECOND SIDE. Until this change the panel's second contact was
 * indistinguishable from the first: the reader took whichever slot's
 * ABS_MT_POSITION_* arrived last, so dragging the RIGHT finger moved the LEFT
 * drawer's fader, and -- because a latched press owns every report of its own life --
 * the right channel never moved at all. Measured on the unit before the fix: slot 0
 * on the left lane, slot 1 dragged alone on the right, and the only line the log
 * produced was `side left fader ch1 -> 198`.
 *
 * WHAT THE ALT POINTER STILL CANNOT DO, stated: it is offered to the DRAWERS and to
 * nothing else. rbp's own pointer path, the band, the browser window, the USB STOP
 * chooser and the UTILITY gesture are single-pointer by construction and stay that
 * way -- a second contact that lands anywhere but a drawer is dropped, never
 * forwarded, because rbp has one pointer and a second press would be a button it
 * cannot interpret.
 * ------------------------------------------------------------------------- */
#define SZ_PTRS      2
#define SZ_PTR_MAIN  0
#define SZ_PTR_ALT   1

/* ---------------------------------------------------------------------------
 * Geometry, in logical px -- the 1280x800 space every other zone in the shim
 * uses (menu_zone.h). 180 px is ~14% of the width: a real target for a fat finger
 * with 920 px of centre glass left uncovered.
 *
 * The x values below are PANEL-LOCAL (`lx`): 0 is the outer edge on BOTH sides, so
 * "16" is the same inset whether the drawer is left or right. The only absolute
 * numbers are SZ_RX0, which is left-panel arithmetic, and SZ_ENTRY_W, which is
 * quoted against the RIGHT entry column in the note under it.
 * ------------------------------------------------------------------------- */
#define SZ_W      180   /* panel width  */
#define SZ_H      MZ_LOGICAL_H      /* full height: the operator's "full-height drawer" */

/* The right panel's absolute left edge. Its x1 is MZ_LOGICAL_W - 1 by
 * construction, and SZ_RX0 == MZ_LOGICAL_W - SZ_W; test_side.c asserts both. */
#define SZ_RX0    (MZ_LOGICAL_W - SZ_W)

#define SZ_PAD    16    /* the inner inset from the outer edge, for the title */

/* One line of the text atlas, in logical px. A LITERAL here rather than
 * `#include "menu_font.h"`, so this module keeps menu_zone.h's "no dependencies"
 * shape -- menu_zone.h spells MZ_LOGICAL_W as 1280 for exactly this reason, and
 * test_side.c asserts the two numbers agree. The layout is the LINE BOX and not the
 * cap box (menu_font.h's rule), so nothing here follows the ink. */
#define SZ_LINE       24

/* Title (the channel label). Rows are one line box tall and 12 rows in from the
 * panel's top; test_side.c pins the text against the width it is drawn into, so a
 * label can never overflow the panel it names. */
#define SZ_TITLE_X0    SZ_PAD
#define SZ_TITLE_X1    (SZ_W - 1 - SZ_PAD)
#define SZ_TITLE_Y0    12
#define SZ_TITLE_Y1    (SZ_TITLE_Y0 + SZ_LINE - 1)   /* 35 */

/* ---------------------------------------------------------------------------
 * THE LAYOUT, TOP TO BOTTOM, and every row of it is the operator's own order: a
 * SYNC button at the TOP of the strip, the +/- nudge pair under it, the channel
 * fader and its value, and CUE and PLAY at the BOTTOM -- *"add a beat sync button
 * at the top of the strip"*, *"move the play/cue button to the bottom"*, *"add +/-
 * buttons to nudge the track"*. The bottom is the thumb's natural resting place on a
 * panel held at the edge of the glass, which is the whole reason PLAY moved there.
 *
 *   12.. 35   CH n        the title
 *   44..115   SYNC        full width
 *  128..163   -  |  +     the nudge pair, side by side, HALF SZ_BTN_H
 *  206..229   value       the fader's readout
 *  240..600   fader       the track, top = loud
 *  628..699   CUE         full width
 *  712..783   PLAY        full width
 *
 * Every block is a multiple of SZ_LINE (24) plus its padding, so the panel reads as
 * a column rather than as five unrelated numbers, and the whole stack ends at 783 --
 * SZ_PAD rows clear of the bottom edge, the same margin the title leaves at the top.
 *
 * THE NUDGE PAIR IS THE ONE BLOCK THAT IS NOT SZ_BTN_H -- the operator asked for it
 * at half height (*"the +/- buttons should be half of their current height"*,
 * 2026-10-09), so it keeps its top edge and gives up its bottom half. The rows BELOW
 * it do not move: the space it gave up is now a gap between the nudge pair and the
 * fader's readout, which is where a gap reads as separation between two groups. That
 * was chosen over pulling the readout and the fader up, which would have grown the
 * fader's own 360 px of travel -- a change to how the fader you are riding FEELS, and
 * not something this request asked for.
 * ------------------------------------------------------------------------- */

/* The common button box: full inner width, and SZ_BTN_H tall. 72 rows is the same
 * target menu_zone.h gives a strip button (40) at nearly twice the height, which is
 * what a thumb on a vertical panel wants, and it leaves room for six stacked things
 * in 800 rows. */
#define SZ_BTN_X0      SZ_PAD                        /* 16 */
#define SZ_BTN_X1      (SZ_W - 1 - SZ_PAD)           /* 163 */
#define SZ_BTN_H       72

/* BEAT SYNC, at the top -- the operator's *"add a beat sync button at the top of the
 * strip"*. It sits under the title and above everything else because it is the
 * control a hand reaches for between two tracks, and because on the hardware it is
 * above the transport too. Full width, like CUE and PLAY: "SYNC" is four glyphs
 * against an atlas whose worst case is eight, so it has room to spare. */
#define SZ_SYNC_Y0     44
#define SZ_SYNC_Y1     (SZ_SYNC_Y0 + SZ_BTN_H - 1)    /* 115 */

/* THE NUDGE PAIR, side by side and full width between them, AND HALF THE HEIGHT of
 * every other button. Two cells of (SZ_BTN_X1 - SZ_BTN_X0 + 1 - SZ_NUDGE_GAP) / 2 = 70
 * px -- the height does not touch their width -- which is wide enough for a thumb and
 * for the drawn bar; the 8 px seam between them is what stops a press that straddles
 * the middle from being ambiguous. The marks are DRAWN, not set: the atlas carries '-'
 * (menu_font.h) and has no '+' at all, and a pair where one sign came from the font and
 * the other from two rectangles would not match -- side_paint.c draws both as bars for
 * exactly that reason.
 *
 * ITS CELL IS NOW THE PANEL'S TIGHTEST VERTICAL TARGET, which is what keeps
 * SP_SIGN_ARM down to a mark that fits in it (side_paint.h): at 36 logical rows a
 * 1280x800 panel gives the cell 36 device rows, so the drawn cross is the thing that
 * decides whether side_paint_ok() will draw the drawer at all on a SHORT panel. */
#define SZ_NUDGE_GAP   8
#define SZ_NUDGE_H     36     /* HALF of SZ_BTN_H -- the operator's ask */
#define SZ_NUDGE_Y0    128
#define SZ_NUDGE_Y1    (SZ_NUDGE_Y0 + SZ_NUDGE_H - 1) /* 163 */
#define SZ_NUDGE_M_X0  SZ_BTN_X0                      /* 16  */
#define SZ_NUDGE_M_X1  ((SZ_BTN_X0 + SZ_BTN_X1 - SZ_NUDGE_GAP) / 2)   /* 85  */
#define SZ_NUDGE_P_X0  (SZ_NUDGE_M_X1 + 1 + SZ_NUDGE_GAP)             /* 94  */
#define SZ_NUDGE_P_X1  SZ_BTN_X1                                      /* 163 */
#define SZ_NUDGE_M_X0  SZ_BTN_X0                      /* 16  */
#define SZ_NUDGE_M_X1  ((SZ_BTN_X0 + SZ_BTN_X1 - SZ_NUDGE_GAP) / 2)   /* 85  */
#define SZ_NUDGE_P_X0  (SZ_NUDGE_M_X1 + 1 + SZ_NUDGE_GAP)             /* 94  */
#define SZ_NUDGE_P_X1  SZ_BTN_X1                                      /* 163 */

/* The fader. The TRACK is what is drawn; the GRAB LANE is wider and taller on
 * purpose so a fat finger lands on it -- it carries SZ_FADER_PAD px of slack either
 * side of the track's own 32 px width, and it reaches UP to the readout's first row
 * so touching the number moves the fader (the promise this header has always made)
 * and DOWN 8 rows past the track for the same reason at the loud end.
 *
 * ITS ENDS ARE DISJOINT FROM EVERY BUTTON: the lane's top (206) is below the nudge
 * pair's last row (163 -- it was 199 until the pair went to half height, so the gap the
 * operator's change opened is 42 rows rather than 6) and its bottom (608) is above
 * CUE's first (628), so a drag can never begin a button press and a button press can
 * never start a drag. That is the same guarantee the old layout made with PLAY, kept by
 * arithmetic: test_side.c asserts both gaps.
 *
 * VERTICAL, track at the top = loud, which is rbp_vu.h's convention (1023 = at the
 * top) and the hardware's. */
#define SZ_FADER_X0     74     /* the drawn track, 32 px wide */
#define SZ_FADER_X1     105
#define SZ_FADER_Y0     240    /* top  = 1023 */
#define SZ_FADER_Y1     600    /* bottom = 0   */
#define SZ_FADER_TRAVEL (SZ_FADER_Y1 - SZ_FADER_Y0)   /* 360 px over 1024 steps */
#define SZ_FADER_PAD    30     /* the grab lane's slack beyond the track */
#define SZ_FADER_GX0    (SZ_FADER_X0 - SZ_FADER_PAD)  /* 44  */
#define SZ_FADER_GX1    (SZ_FADER_X1 + SZ_FADER_PAD)  /* 135 */
#define SZ_FADER_GY0    SZ_READ_Y0                    /* 206: the readout is a grab */
#define SZ_FADER_GY1    (SZ_FADER_Y1 + 8)             /* 608: 8 rows below the track */

/* The value readout, ABOVE the fader now (it was below when PLAY was below). 0..100
 * as a percentage: nothing else tells the operator where the fader is, and it makes a
 * screenshot self-proving. */
#define SZ_READ_X0      66
#define SZ_READ_X1      113
#define SZ_READ_Y0      206
#define SZ_READ_Y1      (SZ_READ_Y0 + SZ_LINE - 1)   /* 229 */

/* CUE and PLAY, at the BOTTOM and full width, in the operator's order: CUE above
 * PLAY, as on the hardware and as the fingers expect. Stacked rather than side by
 * side for the reason the old layout stacked them -- the inner width is
 * SZ_W - 2*SZ_PAD = 148 px, and two 74 px cells would put "PLAY" against the atlas's
 * own 65 px worst case ("USB STOP", menu_zone.h) with nothing to spare. */
#define SZ_CUE_Y0      628
#define SZ_CUE_Y1      (SZ_CUE_Y0 + SZ_BTN_H - 1)     /* 699 */
#define SZ_PLAY_Y0     712
#define SZ_PLAY_Y1     (SZ_PLAY_Y0 + SZ_BTN_H - 1)    /* 783 */

/* ---------------------------------------------------------------------------
 * The entry column, AND THE y GATE UNDER IT IS LOAD-BEARING.
 *
 * A swipe may only START in the outer SZ_ENTRY_W px of the glass while the drawer
 * is shut. 56 px matches MZ_SWIPE_PX/MZ_CLOSE_PX, so the runway is exactly one
 * swipe long.
 *
 * SZ_ENTRY_Y0 IS NOT TIDINESS. The right entry column, local x 0..55, is absolute
 * x 1224..1279 -- EXACTLY the band's seventh column, whose USB STOP cell sits at
 * y 8..47 (menu_zone.h's MZ_BTN_Y0/Y1). A drawer that armed there would swallow a
 * press aimed at the safe eject, and that press STOPS THE OPERATOR'S MEDIA. The
 * gate keeps the two entry zones disjoint in BOTH axes: the band's is x 426..852
 * with y <= MZ_STRIP_Y1 (55), the drawers' is the outer 56 px with y >= 56. So one
 * latched press can arm at most one surface, and the collision cannot return by
 * accident -- but any change to MZ_STRIP_Y1 reopens it and must reopen this gate.
 * ------------------------------------------------------------------------- */
#define SZ_ENTRY_W    56
#define SZ_ENTRY_Y0   (MZ_STRIP_Y1 + 1)   /* 56: below every band row */

#define SZ_SWIPE_PX   56   /* travel inward, dominant axis, that opens */
#define SZ_CLOSE_PX   56   /* travel outward, dominant axis, that dismisses */

/* What a logical point is over, in PANEL-LOCAL terms. SZ_HIT_NONE and SZ_HIT_BG
 * are different answers on purpose: NONE is "outside the panel" (or on a control's
 * own gap) and BG is "inside the panel and not on a control" -- and it is only the
 * second that can start a dismissal swipe.
 *
 * SZ_HIT_NUDGE_M AND _P ARE THE '-' AND '+' CELLS, in the operator's left-to-right
 * reading order: MINUS is the left cell, PLUS the right one. They are not "back" and
 * "forward" in this module -- that mapping is a fact about rbp's tempo slider, not
 * about the panel, and it lives in the caller's send. */
#define SZ_HIT_NONE    0   /* not on the panel */
#define SZ_HIT_SYNC    1
#define SZ_HIT_NUDGE_M 2   /* the '-' cell: move the tempo back   */
#define SZ_HIT_NUDGE_P 3   /* the '+' cell: move the tempo forward */
#define SZ_HIT_CUE     4
#define SZ_HIT_PLAY    5
#define SZ_HIT_FADER   6   /* the grab lane */
#define SZ_HIT_BG      7   /* inside the panel, on no control */

/* What the caller must DO with a swallowed report, beside swallowing it. Cleared on
 * every call that is not a TAKEN report, exactly as menu_feed() clears its button.
 * There is no OPEN/CLOSE action: whether the drawer is out is state the driver
 * reads through side_is_open(), and the caller does not act on it.
 *
 * THE NUDGE IS THREE ACTIONS BECAUSE THE PRESS MOVES THE TEMPO AND ONLY THE LIFT PUTS
 * IT BACK. The pair is a hand on the tempo slider -- pointsrc.c sends
 * K_TEMPO_SLIDER/OP_VALUE, the same message the FLX4's own pitch fader sends, with the
 * fader's position moved by +-SIDE_NUDGE_PCT percent of its travel -- so the pair needs
 * a move down and a move up, and ONE restore that is direction-agnostic because putting
 * the fader back does not care which cell was held. That restore must be emitted on the
 * RELEASE edge, on a slide-off, on the drawer closing and on the touch device vanishing:
 * every one of those is a finger that stopped existing with the tempo still moved, and
 * nothing on the glass could then put it back. That is why side_nudging() exists to
 * reconcile it rather than four call sites each remembering. */
#define SZ_ACT_NONE        0
#define SZ_ACT_SYNC        1   /* send K_SYNC press+release on this side's channel */
#define SZ_ACT_NUDGE_REV   2   /* the '-' cell: move the tempo slider back */
#define SZ_ACT_NUDGE_FWD   3   /* the '+' cell: move the tempo slider forward */
#define SZ_ACT_NUDGE_STOP  4   /* the lift: put the tempo slider back */
#define SZ_ACT_CUE         5   /* send K_CUE press+release on this side's channel */
#define SZ_ACT_PLAY        6   /* send K_PLAY press+release on this side's channel */
#define SZ_ACT_FADER       7   /* send K_FADER/OP_VALUE = *value on this side's channel */

/* ---------------------------------------------------------------------------
 * The coordinate transform. Local x 0 is the OUTER edge on both sides, so the two
 * panels are one set of rules; these are the only two places the mirror exists.
 * ------------------------------------------------------------------------- */
static inline int side_local_x(int side, int x)
{
    return side == SZ_RIGHT ? (MZ_LOGICAL_W - 1 - x) : x;
}

static inline int side_abs_x(int side, int lx)
{
    return side == SZ_RIGHT ? (MZ_LOGICAL_W - 1 - lx) : lx;
}

/* The channel this side drives: the deck number rbp's send_rx_key() wants as its
 * channel argument (1 = deck 1, 2 = deck 2 -- map_flx4.c:910-913). Left is deck 1. */
static inline int side_channel(int side)
{
    return side == SZ_RIGHT ? 2 : 1;
}

/* What a logical point is over. `x` is ABSOLUTE logical (the space pointsrc.c works
 * in); the mirror is applied here so no caller has to remember which edge it is on.
 * Bounds are inclusive at both ends, as menu_button_at()'s are. */
int side_hit(int side, int x, int y);

/* Is a logical point in the entry column with the drawer shut? Absolute x, and
 * y >= SZ_ENTRY_Y0 is the USB STOP gate above. */
int side_entry_in(int side, int x, int y);

/* The fader's maths, exact ints so the boundaries are testable, exposed because the
 * painter needs the inverse to place the handle.

 *   side_fader_v(y)    clamps y to [SZ_FADER_Y0, SZ_FADER_Y1] and maps it to
 *                      0..1023 with 1023 at the top (y = SZ_FADER_Y0).
 *   side_fader_y(v)    the inverse; v is clamped to 0..1023, and v = 1023 gives
 *                      back SZ_FADER_Y0. Round-trips at both extremes.
 *   side_fader_pct(v)  the readout: 0..100, v clamped.
 *
 * 360 px of travel over 1024 steps is finer than a finger can aim, so the mapping
 * is not the limiting quantiser -- the dedup in the caller is. */
int side_fader_v(int y);
int side_fader_y(int v);
int side_fader_pct(int v);

/* The gesture. Returns menu_zone.h's MZ_FEED_* vocabulary -- the same three
 * answers, so pointsrc.c's funnel is written once -- and sets *act / *value for a
 * TAKEN report only:

 *   MZ_FEED_NONE   not ours: rbp gets this report unchanged, *act is SZ_ACT_NONE.
 *   MZ_FEED_TAKEN  swallowed. *act may be CUE, PLAY or FADER (with *value the
 *                  0..1023 position), or NONE.
 *   MZ_FEED_TAP    swallowed, and it was a press the drawer has no use for -- the
 *                  caller replays the whole press from side_tap_point(). Only ever
 *                  answered for a press that began in the entry column, never
 *                  travelled far enough to be a swipe, and left the drawer shut --
 *                  the mirror of menu_zone.h's tap rule, and it exists for the same
 *                  reason: a control rbp draws in the entry column is DELAYED, never
 *                  dropped.
 *
 * The rules, in local coordinates (lx), in full because this is the whole feature:

 *   down, shut, in the entry         -- swallowed and armed for an inward swipe.
 *   column (lx <= SZ_ENTRY_W-1 and
 *   y >= SZ_ENTRY_Y0)
 *   down, shut, anywhere else        -- NOT ours end to end: rbp keeps the whole
 *                                       press natively, with no replay.
 *   down, out                        -- swallowed; remembers which control it began
 *                                       on. A press on the FADER lane jumps to the
 *                                       landing y and sends that value AT ONCE (the
 *                                       operator's "jump to where you touch"); a
 *                                       press on either nudge cell moves the tempo
 *                                       slider in that cell's direction.
 *   while down, armed & shut         -- opens on inward travel dx >= SZ_SWIPE_PX AND
 *                                       dx > |dy|: predominantly horizontal, so a
 *                                       vertical drag from the edge is not a swipe
 *                                       and rbp's own vertical gestures are
 *                                       untouched.
 *   while down, out, press began     -- the drag: recompute the value and send only
 *   on the fader lane                   when it CHANGES (the caller's dedup, the
 *                                       same one map_flx4.c:623-625 makes).
 *   while down, out, press began on  -- a swipe of SZ_CLOSE_PX OUTWARD toward its
 *   the background                      own edge, same dominant-axis test, closes
 *                                       it mid-press.
 *   release, started out             -- fires SYNC, CUE or PLAY only if the press
 *                                       began AND ended on the same box (slide-off
 *                                       cancels, menu_zone.h's fire-on-release
 *                                       rule). A press that began on a NUDGE cell puts
 *                                       the tempo back (SZ_ACT_NUDGE_STOP) whether or
 *                                       not the finger is still on the cell, and does
 *                                       NOT fire the button a slide-off landed on --
 *                                       a nudge that wandered is not a button press.
 *                                       The ONE exception to fire-on-release is a press
 *                                       that began on the FADER lane: the value has
 *                                       already been sent.
 *
 *   THE DRAWER STAYS OUT IN EVERY ONE OF THOSE CASES. It used to close on any
 *   release but the fader's; with a SYNC button, a nudge pair and the operator's
 *   *"allow for both side panels to be visable at the same time"*, a self-closing
 *   panel is unusable -- SYNC then NUDGE then PLAY would be three swipes. What closes
 *   a drawer is now only ever ONE act: a swipe outward from its own background (above).
 *   A press anywhere else, including the glass rbp draws for itself, is not this
 *   module's at all and comes back MZ_FEED_NONE on both edges.

 *   release, started shut, opened    -- the drawer STAYS OPEN: lifting the finger
 *                                       does not take it away.
 *   release, started shut, never     -- MZ_FEED_TAP; the caller replays.
 *   opened
 *
 * A press is swallowed for its whole life or not at all (latched at the down edge):
 * a press that starts outside never becomes ours when it wanders in.
 *
 * ONE SURFACE AT A TIME, and that is now the BAND against the DRAWERS rather than one
 * drawer against the other. The caller's funnel is window -> band -> side, and this
 * module's closed-entry arm is gated on `!menu_is_open()` alone: with two drawers able
 * to be out together, a drawer may be swiped out while the other is already up, so a
 * gate on `side_any_open()` here would make the second panel unreachable -- which is
 * exactly the feature the operator asked for. The band's own arm keeps its
 * `&& !side_any_open()`, and band-first is still deliberate: an open band swallows at
 * full width and must answer first, and this module's `!menu_is_open()` gate is what
 * stops a drawer stealing a dismissing tap at the left edge (y >= SZ_ENTRY_Y0) before
 * the band can close itself. The browser WINDOW's gate is the caller's (`&&
 * !menu_window_is_open()` in pointsrc.c's funnel), because the window is opaque and
 * first in the ladder and this module has no business knowing it exists.
 *
 * `ptr` is WHICH FINGER (SZ_PTR_MAIN / SZ_PTR_ALT). It selects one press's own
 * bookkeeping and nothing else -- the gesture rules above are identical for both, so
 * the second contact gets the whole drawer: it can ride a fader, press SYNC, hold a
 * nudge cell, swipe a shut drawer out and sweep one away. Only the state is separate. */
int side_feed(int side, int ptr, int down, int x, int y, int *act, int *value);

/* THE NUDGE, RECONCILED RATHER THAN REMEMBERED. Returns what THIS side's nudge is
 * doing right now: -1 while the '-' cell is held, +1 while the '+' cell is, 0 when
 * neither is.
 *
 * It exists because the moved tempo is state rbp holds rather than this process, so
 * "a cell is held" can be lost -- by a slide-off, by the panel closing, or by the touch
 * device vanishing with a finger still down -- and a tempo left moved has nothing on
 * the glass able to put it back. The caller calls this on every report path and once in
 * the device-loss path (beside side_reset_all()) and turns any change into the matching
 * send, so there is exactly one place that can be wrong instead of four. side_feed()'s
 * SZ_ACT_NUDGE_* answers are the edges of the same state and are how a caller that acts
 * only on reports gets told; this is how a caller that has to reconcile gets the
 * truth. */
int side_nudging(int side);

/* The funnel's ONE call: consult the drawers and report which one answered.
 *
 * WHICH ONE IS NOT THE CALLER'S PROBLEM, and getting it wrong is not subtle. The
 * order is fixed and each step is load-bearing:
 *
 *   1. the drawer whose OPEN PANEL contains the point -- its own controls are
 *      offered first, and it is the only step that can fire one;
 *   2. failing that, the ENTRY COLUMN of a SHUT drawer, left then right -- so the
 *      second panel can be swiped out while the first is already up. (While both are
 *      shut the two columns are disjoint and the order cannot matter; while one is
 *      open its column still lies outside the other's panel, so the columns stay
 *      disjoint in every state. The open drawer is simply not a candidate at this
 *      step, which is what makes the check one `if` per side rather than a tangle.)
 *   3. failing that, NOTHING HERE CLAIMS IT. A press on the glass rbp draws for
 *      itself -- the centre of the screen, most of what the operator touches -- is
 *      handed back untouched, on its down edge and on its release alike, so rbp's
 *      own UI keeps working with a drawer out. Nothing latches it, so nothing has to
 *      unlatch it and a device that vanishes mid-press cannot leave this module stuck.
 *      (There used to be an "away" dismissal here that closed every open drawer. The
 *      operator reversed it: the drawers stay up until they are swiped away.)
 *
 * Sets *side_out to SZ_LEFT or SZ_RIGHT for a report either drawer took, or -1 for
 * MZ_FEED_NONE; *act / *value are side_feed()'s, and are cleared on every path that
 * is not that drawer's TAKEN answer.
 *
 * `ptr` is which finger, exactly as in side_feed(). Each pointer runs this ladder
 * over its OWN latched state, so the second contact's reports are never routed by
 * the first contact's press -- which is the defect this parameter exists to remove. */
int side_feed_any(int ptr, int down, int x, int y, int *side_out, int *act,
                  int *value);

/* Is this drawer out? Read by the painter and by the driver's plane sync. */
int side_is_open(int side);

/* Is EITHER drawer out? This is the gate menu_zone.c's closed-entry arm needs --
 * the band must not arm a swipe while a drawer owns the plane. One call rather than
 * two so the gate cannot be written with a side missing. */
int side_any_open(void);

/* The control currently under the finger, SZ_HIT_* -- what the painter highlights.
 * SZ_HIT_NONE when no press of ours is down. BOTH POINTERS ARE ASKED, the primary
 * first, because a highlight is a fact about the panel and either hand can put one
 * there; asking only the primary would leave the second finger's press invisible. */
int side_pressed(int side);

/* Where the press that side_feed() just answered MZ_FEED_TAP for began, in ABSOLUTE
 * logical px. Read only immediately after that answer, the only moment the state it
 * reads is meaningful; the caller emits both edges there rather than at the release,
 * because rbp never saw the movement and a drifted finger is still a tap on the
 * thing it started on. ABSOLUTE, not local -- the caller feeds it straight to
 * tscfake_emit(), which works in the logical space. */
void side_tap_point(int side, int *x, int *y);

/* Forget everything for one side, and for both. Called where the touch device is
 * lost (beside menu_reset()): a drawer left open by a finger that went away with the
 * device would be a drawer nothing can dismiss. */
void side_reset(int side);
void side_reset_all(void);

#endif /* RBPI4B_SIDE_ZONE_H */
