/*
 * util_zone.h -- touch inside rbp's own UTILITY screen: drag to scroll the list, tap a
 * row to Enter it, and drag again to change the value of an item already in edit mode.
 *
 * THE FEATURE, in the operator's words: *"in the utility menu give the ability to
 * scroll up and down items and tap to click enter on an item (only do this in this
 * menu)"*, disambiguated as *"the utility menu where the need lock is"*. rbp's
 * UTILITY screen is opened by HOLDING the shim's MENU control (menu_zone.h's
 * MZ_HOLD_FINGER_MS) and is driven entirely by K_SELECTOR: OP_ROTATE moves the
 * highlight, OP_PRESS/OP_RELEASE is Enter. rbp binds NO touch to it -- docs/07-touch.md
 * has said so since the screen was mapped, and has listed this as real work nobody
 * had started.
 *
 * This is menu_zone.h's and side_zone.h's sibling and is written to the same rule:
 * pure C, no rbp address, no env, no I/O, no clock. It says WHICH rotation count and
 * whether an Enter follows; sending them is the caller's (pointsrc.c).
 *
 * ---------------------------------------------------------------------------
 * WHAT rbp ACTUALLY DOES, because every number here comes from it. Measured on the
 * unit 2026-10-05 by injecting evdev keys into the panel's own keyboard node and
 * reading the state back (work/keysend.py + work/utilprobe.py); the full record is
 * the memory file rb-utility-list-internals.
 *
 * THE GATE is one read. rbp gates its whole UTILITY key handling on
 * `getBrowseMode() == 7` -- `UiBrowse_SetDispUtilityList` @0x13cfc8 opens the screen
 * with `setBrowseMode(7)` and `Ui_BrowseCommTask` @0x1351a8 compares against the
 * literal 7 at 0x139124/0x139180/0x1391a4/0x1391bc. The mode is the first word of
 * the `uiBrowse` singleton at a FIXED address, so there is no pointer chase and
 * nothing to guard -- which is why this module is handed the answer rather than
 * fetching it: `mode` arrives in struct util_state, exactly as menu_zone.h's
 * gestures are handed the caller's `allow_menu`.
 *
 * THE WINDOW AND THE CURSOR. The list is 33 items drawn 12 rows at a time. A rotate
 * moves the selected ABSOLUTE index by exactly one, every time: the cursor walks
 * 0..11 and then, at the edge, HOLDS at 11 while `initialNo` -- the window's first
 * item -- increments. Fourteen steps took initial 7 -> 21 with the cursor pinned at
 * 11, and stopped at absolute 32, the last of the 33. Rotating back up walks the
 * cursor down inside the window before the window itself moves.
 *
 * That is what makes a tap cheap: the target the operator touched is `initialNo + row`
 * and the cursor is already on `initialNo + cursorNo`, so the travel is
 * **row - cursorNo** and the window position cancels. Reading initialNo would buy
 * nothing, and this module deliberately does not ask for it.
 *
 * ENTER IS NOT HARMLESS, and this is the one real hazard. A selector press TOGGLES
 * rbp's edit mode: `activeList` (uiBrowse+0x14) goes 0 -> 1 and the item's value
 * becomes rotatable. Measured on a live unit: one rotate while editing turned LOAD
 * LOCK from UNLOCK to LOCK. So a rotation sent while an item is in edit mode does not
 * scroll -- it rewrites a setting.
 *
 * THE FIRST RELEASE GOT THIS WRONG, and the correction is the operator's. It refused
 * every rotation while editing, on the reasoning that a silent rewrite is worse than a
 * dead gesture. But a dead gesture is exactly what they reported back:
 *
 *     *"selecting items in utility menu also works, only issue is that i have no way of
 *      changing the values once selected"*
 *
 * -- the refusal let them ENTER a value row and never change it. And the trade is not
 * ours to make, because rbp decides what a rotation means from its own cursor mode:
 * on the RX3 one encoder both moves the highlight and edits the value, and no control
 * scrolls a list while an item is in edit mode. So the rule is now the operator's own,
 * and it is the whole of the gesture:
 *
 *     a tap enters edit mode; a drag changes the value; a tap leaves.
 *
 * The tap's own travel count still stays zero in that state, so the Enter that leaves
 * edit mode cannot also drag the highlight onto the row the finger happened to be over.
 */
#ifndef RBPI4B_UTIL_ZONE_H
#define RBPI4B_UTIL_ZONE_H

/* rbp's browse mode for this screen -- UiBrowse_SetDispUtilityList @0x13cfc8. Kept
 * as this module's own literal rather than including rbp_abi.h, so the header stays
 * dependency-free the way menu_zone.h and side_zone.h are; test_util.c includes both
 * and pins that the two numbers agree. */
#define UTIL_MODE_UTILITY 7

/* ---------------------------------------------------------------------------
 * Geometry, in the 1280x800 logical px every zone in the shim works in. All of it
 * measured off /dev/fb0 with the screen up (2026-10-05), not eyeballed:
 *
 *   row 0 spans y  50..101      the first item, directly under the UTILITY title
 *   a 2 px separator, then every row is 52 px on from the last
 *   row 11 spans y 622..673     the twelfth and last DRAWN row
 *   row n centres on y 76.5 + 52n
 *
 * Below y 674 is a gap and then rbp's own deck strip at ~712, which is not ours and
 * must keep working untouched. TWELVE rows, not thirteen: rbp's `stUtilityObjects`
 * window has Obj_CTRL_UTILITY_GRP_LEFT_LINE1..13, but only twelve are ever drawn,
 * and rbp's own setCursor @0x113c24 clamps the cursor to 0..11 -- the same twelve.
 *
 * The name cell is x 0..729 and the VALUE cell x 730..1279, but they are one row, so
 * the whole width is one target and there is no column rule here at all.
 * ------------------------------------------------------------------------- */
#define UTIL_ROW0_Y   50    /* the top edge of row 0 */
#define UTIL_ROW_H    52    /* row pitch, separator included */
#define UTIL_ROWS     12    /* drawn rows; rbp's setCursor clamps 0..11 */
#define UTIL_LIST_X0  0
#define UTIL_LIST_X1  1279
#define UTIL_LIST_Y1  (UTIL_ROW0_Y + UTIL_ROWS * UTIL_ROW_H - 1)   /* 673 */

/* A press is a TAP if it never wandered further than this from where it landed, in
 * either axis. 24 px is a little under half a row, so a finger that has crossed into
 * the next row has already stopped being a tap by the time a row of travel is
 * registered -- the two tests agree, which is the point. It is also well inside the
 * 52 px a real finger needs to aim at a row, so a deliberate tap cannot be missed by
 * wobble. */
#define UTIL_TAP_SLOP 24

/* The most rotations this module will ever hand back in ONE answer. A tap needs at
 * most 11 (the cursor's own 0..11 range), so this only ever bites on a drag, where
 * it matches the precedent map_kbd.c:184-191 sets for a wheel that arrives with a
 * dozen notches at once -- a burst must not become a dozen keycodes in one breath. */
#define UTIL_ROT_MAX  16

/* What the caller must DO with a swallowed report. Cleared on every call that is not
 * a TAKEN report, the same discipline side_zone.c's SZ_ACT_* keeps.
 *
 * Both actions carry a SIGNED rotation count in *value, applied in the order given:
 *
 *   UTIL_ACT_SCROLL   rotate *value steps and stop. A drag that is still moving.
 *   UTIL_ACT_TAP      rotate *value steps, then send Enter (K_SELECTOR press and
 *                     release). *value is 0 when the row touched is already the
 *                     highlighted one -- and also, always, while editing, where the
 *                     rotation is skipped on purpose (see the header note).
 *
 * Sending is the caller's, and the count is bounded so it does not have to re-bound
 * it; the caller must not sleep between steps (rbp's rotate is a posted message, and
 * a per-step wait would starve the shared input loop this runs on). */
#define UTIL_ACT_NONE   0
#define UTIL_ACT_SCROLL 1
#define UTIL_ACT_TAP    2

/* rbp's UTILITY state, as the caller reads it. `cursor` is the cursor of the list
 * rbp currently has ACTIVE -- getCursorNo(getActiveList()) -- because that is the one
 * a rotation will move, and it is 0..11 (rbp clamps it).
 *
 * `mode` is getBrowseMode(), `editing` is `activeList != 0`, and `calibrating` is
 * IsUtilityCalibrationOn() -- a sub-screen reached by Entering TOUCH DISPLAY
 * CALIBRATION, which is rbp's own and must not be swallowed (it is a touch screen,
 * and this module would otherwise cover the rows it draws its marks in). */
struct util_state {
    int mode;
    int editing;
    int calibrating;
    int cursor;
};

/* Which drawn row a logical y is on, or -1 if it is not on the list at all. The
 * mapping is fixed in screen space: it is the WINDOW's rows, so it does not depend on
 * initialNo, and a row keeps its y as the list scrolls its contents through it. */
int util_row(int y);

/* Is this point on the list? x in [UTIL_LIST_X0, UTIL_LIST_X1] and y on a row. The
 * caller's ladder gives the band and the drawers first refusal, so the list may
 * overlap their entry columns without this module having to know they exist. */
int util_in_list(int x, int y);

/* The gesture. Returns menu_zone.h's three-answer vocabulary, so pointsrc.c's funnel
 * is written once -- though this module only ever uses two of them:
 *
 *   MZ_FEED_NONE   not ours: rbp gets this report unchanged, *act is UTIL_ACT_NONE.
 *   MZ_FEED_TAKEN  swallowed, and *act / *value say what to send.
 *
 * It answers NOTHING unless `st->mode == UTIL_MODE_UTILITY` and the calibration
 * sub-screen is not up -- the operator's *"only do this in this menu"*, and the whole
 * reason this module can cover a band of the glass that rbp draws for itself.
 *
 * A press is swallowed for its whole life or not at all (latched at the down edge),
 * so a press that starts off the list never becomes ours when it wanders on. The
 * rules in full:
 *
 *   down on the list        -- swallowed and latched; the landing point is the anchor
 *                              and the row under it is remembered. Nothing is sent
 *                              yet, so a tap costs one report even if it never moves.
 *   while down              -- coalesced rotations: one per UTIL_ROW_H of travel from
 *                              the ANCHOR, minus what this press has already sent, so
 *                              a resting finger re-sends nothing and a direction
 *                              change reverses by exactly the difference. Bounded by
 *                              UTIL_ROT_MAX per answer. What rbp DOES with those
 *                              rotations depends on its own state and not on this
 *                              module: !editing they scroll the list, editing they
 *                              change the highlighted item's value. It is the same
 *                              number on the same wire either way, and that is why
 *                              there is no separate "edit" action here.
 *   release, wandered       -- swallowed, nothing sent: a drag is not a tap.
 *   release, on the anchor  -- UTIL_ACT_TAP with the travel to the row TOUCHED:
 *                              `row_at_press - cursor`, clamped to the cursor's own
 *                              range. Zero rows is a legitimate answer and means
 *                              "Enter the row you are already on". While editing the
 *                              count is 0 and the Enter alone goes out, which is what
 *                              leaves edit mode and hands the screen back to this
 *                              gesture.
 *
 * and one refusal that is not about the gesture at all: if the screen has LEFT
 * UTILITY by the time a report arrives -- the FLX4's BACK, which reaches rbp
 * without ever touching this module -- the press is dropped and that report is
 * MZ_FEED_NONE, on the move path and on the release. rbp never saw the press, so
 * it is owed no release; more to the point, an Enter or a rotation must never land
 * on a screen this module knows nothing about. */
int util_feed(const struct util_state *st, int down, int x, int y,
              int *act, int *value);

/* Forget the press in progress. Called where the touch device is lost (beside
 * menu_reset() and side_reset_all()): a latched press whose finger went away with the
 * device would otherwise make the NEXT press look like a continuation of this one. */
void util_reset(void);

#endif /* RBPI4B_UTIL_ZONE_H */
