/*
 * menu_zone.h -- the swipe-down top menu's geometry and gesture, and the one
 * question that matters to the rest of the shim: does rbp get to see this touch?
 *
 * Second of the shim's two pure modules (touch_zone.h is the first, for the deck
 * QUANTIZE box whose widget rbp binds no touch to). The rules there are the rules
 * here, and for the same reason -- test_menu.c has to be able to pin the gesture
 * on the host, with no rbp, no framebuffer and no touch panel in the loop:
 * pure C, no rbp address, no env, no I/O, no clock.
 *
 * WHAT THIS IS FOR. Seven functions. Six of them are reachable on this unit only
 * from a keyboard (map_kbd.c's digit row) or from the FLX4 (SHIFT+browse for
 * SOURCE alone); the seventh, USB STOP, is reachable from nowhere else at all --
 * it is the safe eject, the FLX4 has no button for it, and pulling the stick
 * instead is what risks the operator's media. The operator's hands are on an
 * absolute touch panel, so the panel is what makes them reachable: swipe down from
 * the top edge and the buttons unroll; tap one and it fires; tap away or swipe up
 * and it goes.
 *
 * THE COST, STATED PLAINLY, AND IT IS NOW A DELAY RATHER THAN A LOSS, OVER A
 * NARROWER BAND THAN IT USED TO BE: while the panel is CLOSED, a touch that starts
 * in the entry zone -- the top MZ_STRIP_Y1 rows AND within MZ_ENTRY_X0..MZ_ENTRY_X1,
 * the middle third of the width -- is swallowed and then, if it never became a swipe
 * and never opened the panel, handed back to the caller to REPLAY to rbp as a press
 * at the point the finger landed (MZ_FEED_TAP below). So a control rbp draws inside
 * that zone still answers a tap; what it does not get is the moment it happened.
 * Outside the zone -- the left and right thirds of the band -- nothing is swallowed
 * at all: rbp sees the touch itself, first frame, and the strip costs those columns
 * nothing. That is where rbp's own `INFO` button lives (x 1183..1258, measured off
 * the glass), so it is a native control again rather than a replayed one, and the
 * BROWSE sidebar's first cell and the title bands in the left third come back with
 * it. The TIME/REMAIN bar, which overlaps into the middle third, is the one control
 * the entry zone can still take -- and it is the one that does not answer a tap
 * anywhere, measured with this shim out of the path entirely (docs/13 S3.5), so
 * nothing is lost there. While the panel is OPEN every touch is swallowed outright,
 * at the full width, and nothing is replayed, because a touch that dismisses the
 * panel must not also press the thing underneath it. The strip is kept as short, and
 * now as narrow, as a swipe can honestly start from: see the literals below for what
 * it takes and what it does not.
 *
 * COORDINATES ARE LOGICAL, PRE-REFLECTION -- touch_zone.h:36-40's rule verbatim.
 * pointsrc.c reflects x at the wire (`tscfake_wire_x()`, 1279-x) after this module
 * has spoken, so feeds get logical points and the hit test is in the same space as
 * the operator's finger. The paint module works in fb pixels instead (so a glyph is
 * never resampled on a non-1280x800 panel); the two boundaries can disagree by a
 * pixel on such a panel, which is invisible and documented rather than papered
 * over.
 */
#ifndef RBLIVE4_MENU_ZONE_H
#define RBLIVE4_MENU_ZONE_H

/* ---------------------------------------------------------------------------
 * Geometry. Literals, in touch_zone.c:50-60's style, each with its measurement.
 *
 * MZ_STRIP_Y1 -- the band rbp does not see *as it happens*. Rows 0..55 inclusive.
 * Short on purpose: the cost is a tap's 45 ms (the caller replays a non-swipe), so
 * it is sized to the shortest swipe that an operator can honestly be asked to
 * start rather than to a loss budget. Measured
 * against this repo's own captured frames in work/unit/ (see work/uimap.py's
 * sidebar cells): on
 * BROWSE it takes the sidebar's first cell (USB1, x 8..100, y 8..50) and the
 * title/header band (y 12..35); the sidebar's next cell, PLAYLIST (y 52..118),
 * is NOT taken -- 118 px would have swallowed it, and those left-hand icons were
 * already the subject of one complaint. PERFORMANCE's title band (y 6..40) goes
 * too. Nothing in this band is a control the operator has needed.
 *
 * MZ_PANEL_Y1 -- what is DRAWN. It was built deliberately taller than the strip
 * (112 rows against a 56-row runway, on the "the panel unrolls below the swipe"
 * metaphor); the operator, having used it, asked for half the height, so it is now
 * the strip's own rows exactly. That makes the two answers coincide, which is worth
 * knowing rather than glossing: the rows the closed gesture takes and the rows the
 * open panel covers are the same 56. The border is kept at 8 px at each end -- it
 * is the frame that makes the band read as a panel, and halving it would leave a
 * 40-row button band wearing a hairline -- so it is the BUTTON band that halves,
 * 96 rows to 40. 56 rows of 800 is 7% of the screen, and only while it is open.
 *
 * At 40 rows the label still sits inside its button with room, and the arithmetic
 * was checked against the glass rather than trusted: with the panel open on the
 * operator's own unit (2026-09-29, a raw dump of rows 0..119) the row profile is
 * frame 0..1, fill 2..7, BUTTONS 8..47, fill 48..53, frame 54..55, rbp's own black
 * at 56 -- so the drawn band is exactly these 56 rows and not one more. The label
 * ink is the CAPITALS' ink box -- the seven labels are capital text, digits and a
 * space and nothing else -- and at the shipped 19 px atlas (menu_font.h) it is 14
 * rows inside a 24-row line box, rows 5..18 of it. The band's rows 8..47 put that
 * line box at 16, so the ink is rows 21..34 in a 40-row button band. (It was rows
 * 23..33 in the 20-row line box of the 16 px atlas: the size grew, the band did
 * not.) Every character the seven labels use is in that box -- menu_font.h's set has
 * since grown lowercase and punctuation, which reach above and below it, but a label
 * is capitals, digits and a space and the LAYOUT is the line box, so nothing here
 * moved with them. The damage witness's points are the buttons' own OUTLINE rows
 * now, the band's top and bottom (menu_paint.c's menu_witness_point(), moved there
 * by the 2026-10-06 restyle when the band's frame became a ring round each button):
 * rows 8 and 47 at the button's own centre x, with one further point in the padding
 * between two buttons at row 9. They are clear of the ink by construction -- the ink
 * is centred in the button inside a MENU_FONT_LINE line box, and the outline rows
 * are the band's own edges -- and it is menu_paint.c's line-box rule, not this
 * paragraph, that bounds it. It used
 * to be 5/16 and 11/16 of the PANEL, which is a proportion, and a proportion is
 * exactly what a fixed-height font cannot follow: at 112 rows those were rows 34
 * and 76 (safe), at 56 they are 17 and 37 (still safe here, but only by luck of
 * this size) and at 480x320 both land on glyphs, which is a witness that reads
 * damaged every tick. So the points now come from the band itself, and
 * menu_paint.c's menu_view_ok() refuses a panel whose band cannot host the line box
 * -- which is what makes the property structural instead of arithmetic that happens
 * to work out. test_menu.c asserts both, at ten panel sizes.
 * ------------------------------------------------------------------------- */
#define MZ_STRIP_Y1   55     /* last dead row, inclusive */
#define MZ_PANEL_Y0    0     /* panel rows, inclusive both ends */
#define MZ_PANEL_Y1   55     /* 56 rows: 8 border, 40 button, 8 border */
#define MZ_BTN_Y0      8     /* buttons, inclusive */
#define MZ_BTN_Y1     47     /* 40 rows tall */
/* Seven, not six: the six browse/selector keys plus USB STOP, the safe eject
 * (rbp_abi.h's K_USBSTOP). It is here at the operator's own ask -- "add a USB STOP
 * button to the menu" -- because the FLX4 has no such button and the alternative,
 * pulling the stick, is the thing that risks the media. Nothing about the column
 * arithmetic below is six-specific; at 1280 logical px a column is 182 px and the
 * widest label, "USB STOP", is 76 px at the shipped atlas (menu_font.h), so the
 * seventh costs every label 42 px of a 182 px column and none of them notices. */
#define MZ_COLS        7     /* SOURCE BROWSE TAG LIST PLAYLIST SEARCH MENU USB STOP */

/* THE EIGHTH CELL IS GONE, AND WHY IT WENT RATHER THAN CHANGED ITS MARK.
 *
 * It was the web cell: a mark in the panel's last 48 px that opened the deck's browser
 * window -- the operator's own asks, "a > button for an extended menu and have it open
 * up a browser window" and then "change the chevron to a web icon". The browser was
 * ABANDONED 2026-10-04 ("ok, you can abandon the exercise, i don't need a browser"),
 * which left the cell opening a window with no browser behind it: a live button that
 * does nothing, sitting in the one band the operator looks at most. So the cell is
 * REMOVED, not re-marked -- restoring the ">" would only make a dead button look like
 * the one that used to work.
 *
 * WHAT THAT GIVES BACK. The seven were narrowed 182 -> 176 px to make the cell's 48,
 * and removing it returns all six pixels to each of them. 1280 does NOT divide by 7
 * (182.857 a column), so the columns are no longer equal -- menu_button_x0/x1 tile
 * them exactly regardless, with no gap and no overlap, and that is the property
 * menu_button_at() and menu_paint_cols() rest on; test_menu.c asserts it at every
 * column rather than asserting a width. The widest label, "USB STOP" at 76 px, has
 * more than 100 px of slack in a 1280-px column either way, so no label moves; it is
 * a NARROW picture, not the seventh column, that the atlas size is measured against
 * -- menu_paint.c refuses one whose narrowest column cannot hold its own label whole,
 * and at 19 px that floor is a 535-px-wide picture.
 *
 * WHAT IS NOT GONE: the window module itself still ships and still works
 * (menu_window.c, menu_window_paint.c, browser_link.c) -- the operator abandoned the
 * browser, not the code -- and menu_draw.c's MENU_WINDOW=1 opener still opens it. What
 * is gone is the menu's door to it, because a door to nowhere is worse than no door. */
#define MZ_BTN_W        MZ_LOGICAL_W   /* 1280: what the seven tile on their own */

/* The touch space, in logical px. 1280x800 on every panel this port runs on --
 * the numbers fb_cursor.c calls POINT_LOGICAL_W/POINT_LOGICAL_H. Literals here
 * rather than an include, so this module keeps touch_zone.h:38-41's "no
 * dependencies" rule; test_menu.c asserts the columns tile MZ_LOGICAL_W
 * exactly, with no gap and no overlap. menu_paint.c maps these onto whatever the
 * real framebuffer turns out to be. */
#define MZ_LOGICAL_W 1280
#define MZ_LOGICAL_H  800

/* MZ_ENTRY_X0..MZ_ENTRY_X1 -- the middle third, and the swipe may only START here
 * while the panel is closed. Rows tell you how far down the strip reaches; this
 * tells you how far across it is still ours, and the two are separate answers on
 * purpose.
 *
 * WHY THE MIDDLE THIRD. Because of what rbp draws in the other two. Its `INFO`
 * button sits wholly in the right third (x 1183..1258, rows 12..35, measured off
 * the glass) and is the operator's own example of what the strip was taking; with
 * these bounds a tap there is not swallowed at all, so it is rbp's native control
 * and not a replayed one. The left third is where BROWSE's sidebar cells and every
 * screen's title band start. Making the entry a third rather than a fixed pixel
 * width means it stays centred whatever the panel reports as its logical width.
 *
 * WHY NOT THE DRAWN PANEL TOO. The panel stays the full width, because seven
 * buttons in a third of the screen would be a worse target than the band they are
 * in -- 182 px each is the narrowest this feature has drawn, and a third of the
 * screen would cut that to 61 -- and because narrowing the drawn band would not buy
 * what the operator hoped for: the
 * shimmer is rbp's own repaint erasing the band in the shared page, and its cost is
 * the repair copy of the band's area, not of the gesture's. Nothing about where a
 * swipe may start changes how often rbp paints.
 *
 * The bounds are inclusive, and are the only place the gesture's x extent is
 * written: menu_feed()'s closed down-edge arm is the single reader. */
#define MZ_ENTRY_X0   (MZ_LOGICAL_W / 3)          /* 426 */
#define MZ_ENTRY_X1  ((2 * MZ_LOGICAL_W) / 3 - 1) /* 852 */

#define MZ_ENTRY_IN(x) ((x) >= MZ_ENTRY_X0 && (x) <= MZ_ENTRY_X1)

#define MZ_PANEL_H   (MZ_PANEL_Y1 - MZ_PANEL_Y0 + 1)   /* 56 */
#define MZ_BTN_H     (MZ_BTN_Y1 - MZ_BTN_Y0 + 1)       /* 40 */

/* The gesture distances, in logical px (~1.5-2 cm of glass on this panel).
 * MZ_SWIPE_PX opens; MZ_CLOSE_PX dismisses, measured from wherever the closing
 * press started, so a finger can travel up without having to start on the panel. */
#define MZ_SWIPE_PX   56
#define MZ_CLOSE_PX   56

/* The press-and-hold: the same button's second meaning, and rbp implements it
 * itself, on one keycode. A *press* of the MENU key is the menu; a *hold* is
 * UTILITY. Measured 2026-09-29 through the keyboard path, which sends the panel's
 * own keycode (vkeyd's KEY_0 -> map_kbd.c's K_MENU 0x206 on CH_GLOBAL): 200 and
 * 300 ms change nothing (587 px of drift), 400 ms and up reach rbp's UTILITY
 * screen (~804 000 px, whole screen), and the 16-band md5 sequence at ~200 ms
 * resolution shows ONE whole-screen change at t ~ 455 ms with no menu before it --
 * so rbp runs its own timer on the key and its threshold is between 300 and
 * 400 ms.
 *
 * The panel therefore needs to know nothing about UTILITY. It answers one
 * question -- was this finger held, or tapped? -- and then holds the key down long
 * enough that rbp's own timer fires. Two numbers, and they are different on
 * purpose:
 *
 * MZ_HOLD_FINGER_MS -- what the OPERATOR's finger must do. 350 ms is a deliberate
 * hold and well clear of a tap (a human tap is 60-120 ms, and the panel's own
 * dwell 45), so no ordinary tap can become a hold by accident. The injection
 * boundary on the real path reads 340.9 ms -> tap and 360.3 ms -> hold, so the
 * constant behaves as written -- and the finger that has to do it has since
 * confirmed it by feel: asked whether the delay was right, the operator said "yes
 * right amount of delay" (2026-09-29). It ships as measured and felt.
 *
 * MZ_HOLD_KEY_MS -- how long the KEY is held, and it is a synthesized number, not
 * the finger's own duration. What rbp's timer has to see is a key-down at least as
 * long as its threshold, and a finger that qualifies as a hold here may clear
 * MZ_HOLD_FINGER_MS by a single millisecond -- which is *inside* rbp's measured
 * 300..400 ms band -- so the finger's duration would reach UTILITY by luck. A fixed
 * span that clears the band's upper bound with room does it every time, and costs
 * the caller one bounded sleep per hold -- the same trade the tap replay makes for
 * its 45 ms. test_menu.c asserts both inequalities, so a retune cannot quietly land
 * under rbp's threshold.
 *
 * (The old reason this read differently has gone: the key used to go out at the
 * release, so firing at the down edge would have sent rbp keys during a press that
 * turned out to be the dismiss-swipe. The swipe cannot fire at all now -- the mid-
 * press rule below requires the press to have started on a button -- and the key
 * goes out at the threshold with the finger still on the glass.)
 *
 * MZ_HOLD_FINGER_MS <= 0 turns the hold off: menu_hold_fires() is then always 0
 * and every press is a plain tap again. That is the A/B lever, and the bench knob
 * POINT_MENU_HOLD_MS is how it is turned. */
#define MZ_HOLD_FINGER_MS 350
#define MZ_HOLD_KEY_MS    500

/* Column i spans [menu_button_x0(i), menu_button_x1(i)], inclusive, and between
 * them they tile x 0..1279 with no gap and no overlap -- because a one-pixel gap
 * would be a
 * dead seam under the operator's finger and a one-pixel overlap would make the
 * hit test depend on which check ran first. i is 0..MZ_COLS-1; anything else is
 * clamped rather than being a read past the end. */
int menu_button_x0(int i);
int menu_button_x1(int i);

/* Label for button 1..MZ_COLS, or NULL. The painter centres these; the width
 * comes from menu_text_width(). */
const char *menu_label(int button);

/* Which cell a logical point is over: 1..MZ_COLS for a labelled button, or 0 for
 * none. A point in the panel but in the border, or in the strip above the buttons,
 * is 0 -- "not a cell" and "outside the panel" are the same answer to the only
 * question the gesture asks, which is why there is no further value. Bounds are
 * inclusive. The seven columns tile the whole logical width, so every x that is in
 * the button band answers with a column; 0 is reached by y alone. */
int menu_button_at(int x, int y);

/* What menu_feed() decided about one report, in full. The caller's skeleton is:
 *
 *     int v = menu_feed(down, x, y, &btn);
 *     if (v == MZ_FEED_TAP)   replay the press from menu_tap_point();
 *     else if (v)             if (btn) fire the key;   -- and emit nothing
 *     else                    forward the report to rbp;
 */
#define MZ_FEED_NONE   0   /* not ours: rbp gets this report unchanged */
#define MZ_FEED_TAKEN  1   /* swallowed: rbp must not see this report at all */
#define MZ_FEED_TAP    2   /* swallowed, and it was a press the menu has no use
                            * for -- the caller replays the whole press itself,
                            * from menu_tap_point(). See the tap rule below. */

/* The gesture. Sets *button to the button to fire (1..MZ_COLS) or 0 -- it is only
 * ever non-zero on a release that both started and ended on the same button, so a
 * swipe that opens the panel and slides onto a button cannot fire it, and *button
 * is cleared on every call that is not such a release.
 *
 * Swallowed reports are never emitted at all -- whole presses, never a lone
 * release -- which is what keeps rbp's stream balanced (tscfake_emit() dedups and
 * tracks its own last_down, so an unbalanced stream would strand rbp's touch
 * thread in the down state). MZ_FEED_TAP is the one exception, and it is spelled
 * out as a separate answer for exactly that reason: there the caller must emit,
 * and must emit *both* edges.
 *
 * The rules, in full, because this is the whole feature:
 *
 *   down edge, closed, in the entry    -- swallowed and armed for a swipe. The
 *   zone (y <= MZ_STRIP_Y1 and x in       entry zone is the strip's rows crossed
 *   MZ_ENTRY_X0..MZ_ENTRY_X1)             with its middle third -- see above.
 *   down edge, closed, anywhere else   -- NOT ours: not swallowed, not tracked,
 *                                          and returned untouched end to end. The
 *                                          left and right thirds of the band are
 *                                          rbp's own controls again, on the first
 *                                          frame and with no replay: that is its
 *                                          `INFO` button on the right and BROWSE's
 *                                          sidebar cells on the left.
 *   down edge, open                    -- swallowed, and remembers which button it
 *                                          started on.
 *   while down, armed and closed       -- opens on dy >= MZ_SWIPE_PX AND dy > |dx|:
 *                                          predominantly vertical, so a drag along
 *                                          the strip is not a swipe.
 *   while down, open                   -- the highlight tracks the button under the
 *                                          finger; an upward MZ_CLOSE_PX from the
 *                                          press start closes it, same verticality
 *                                          test.
 *   while down, open, and this         -- once the finger has been down
 *   press started on a button             MZ_HOLD_FINGER_MS, the panel closes itself
 *                                         and the button fires, the finger still on
 *                                         the glass: menu_hold_pending() below.
 *                                         The one rule here that is the caller's
 *                                         CLOCK rather than a position, and it is
 *                                         why the key can now be held for as long
 *                                         as rbp's own timer needs -- see below.
 *   release, started open               -- fires *button if it started and ended
 *                                          on the same button, then closes. A
 *                                          release on the background, off a
 *                                          button, or outside the panel closes and
 *                                          does nothing. Fire-on-release rather
 *                                          than touch_zone.c's fire-on-down
 *                                          because the shim owns the highlight
 *                                          here, and slide-off-to-cancel is what a
 *                                          button is expected to do -- and because
 *                                          firing on the down edge would send
 *                                          rbp's key during a press that turns out
 *                                          to be the dismiss-swipe. This module
 *                                          says WHICH button; how long the finger
 *                                          was down is the caller's clock, and
 *                                          whether that makes it a hold is
 *                                          menu_hold_fires() below.
 *   release, started closed            -- the panel STAYS OPEN if the press opened
 *                                          it. That is the toggle the operator
 *                                          asked for: the swipe brings the panel
 *                                          out and lifting the finger does not
 *                                          take it away. Nothing fires either way
 *                                          -- a swipe in the strip is not a press
 *                                          on a button, however far it travelled.
 *   release, started closed, and the
 *   panel is CLOSED                    -- MZ_FEED_TAP. The press began in the strip,
 *                                          never travelled far enough to become a
 *                                          swipe, and so is a plain tap: the one
 *                                          report the menu takes and gives back.
 *                                          Not swallowed *and* not rbp's either --
 *                                          see the replay rule below.
 *
 * A press is swallowed for its whole life or not at all (latched at the down
 * edge): a press that starts outside never becomes ours when it wanders in, and a
 * press that starts inside never escapes back to rbp when it wanders out.
 *
 * A tap in the entry zone does not open the panel -- there is no vertical travel, so
 * there is no swipe -- and it is not discarded either: MZ_FEED_TAP hands it back and
 * the caller replays the whole press at the point the finger landed. That covers the
 * controls rbp draws *inside* the entry zone and nowhere else: the middle third of
 * its own top bar, which on PERFORMANCE is the middle of the track/waveform header
 * and on BROWSE the middle of the title band. The two controls this feature was
 * first accused of taking are no longer among them -- `INFO` is in the right third
 * and the TIME/REMAIN bar starts in the left. The panel staying shut keeps the strip
 * honest about what it is for: a swipe's runway, not a button of its own.
 *
 * `!open_state` IS the whole of "and never opened the panel", which is worth stating
 * because it does not look like it. A press that did not start open can have
 * open_state set only by its own swipe, and cleared again only by the dismissal rule
 * above -- which needs `start_y - y >= MZ_CLOSE_PX` with `start_y <= MZ_STRIP_Y1`
 * (55) and y in the logical space, where y >= 0. A press that began in the strip
 * therefore cannot satisfy it, so the two conditions agree today. If the dismissal
 * rule ever gains a downward gesture, this clause has to become an explicit "this
 * press opened the panel" flag: a dither that opened and re-closed the panel within
 * one press would otherwise replay rbp an INFO it was never asked for. */
int menu_feed(int down, int x, int y, int *button);

/* Does a press that lasted held_ms fire as a HOLD? The clock is the caller's -- this
 * module has none, by the no-I/O rule at the top -- but the *rule* lives here so it
 * can be pinned on the host instead of in an untested line of pointsrc.c, and so the
 * two constants above sit next to the comparison that uses them. threshold_ms comes
 * from the caller (the bench knob over MZ_HOLD_FINGER_MS); <= 0 means "hold off".
 * held_ms < 0 means the caller has no measurement for this press, which is a tap. */
int menu_hold_fires(int held_ms, int threshold_ms);

/* The same hold, asked WHILE the finger is still down. Returns the button that has
 * just become a hold -- having closed the panel itself -- or 0.
 *
 * menu_hold_fires() answers "was this press long enough" for a press that is over.
 * This answers it for one that is not, and it exists because the operator said the
 * difference out loud (2026-10-04): *"for holding the MENU to get utility, after two
 * seconds the menu should disappear and it should just go to utility by itself"*.
 * rbp's timer runs on the KEY and does not care when the panel lifts, so firing the
 * hold only at the release cost nothing functionally and everything in feel: the
 * panel sat there showing no reaction for as long as the finger stayed down, and
 * only the lift told the operator anything. The threshold is the *same* number and
 * is shared with the release path -- MZ_HOLD_FINGER_MS, which they had already
 * approved by feel -- so this changes only WHEN the same hold is answered, never
 * what counts as one. It is not a second, longer press to learn.
 *
 * Called on a press that is still down, once per tick of the caller's clock. The
 * first call that sees held_ms >= threshold_ms closes the panel and returns the
 * button; every later call returns 0, because a closed panel has no button to hold
 * and because the press stays swallowed to its end -- the release that follows is
 * the MZ_FEED_TAKEN it already was, so the caller must not treat that release as a
 * second fire. */
int menu_hold_pending(int held_ms, int threshold_ms);

/* Where the press that menu_feed() just answered MZ_FEED_TAP for began, in logical
 * px -- the point the replay is emitted at, and the point the operator actually
 * aimed at. Read only immediately after that answer, which is also the only moment
 * the state it reads is meaningful; the caller emits both edges there rather than
 * at wherever the finger was at the release, because rbp never saw the movement and
 * a drifted finger is still a tap on the thing it started on. */
void menu_tap_point(int *x, int *y);

/* State, read by the painter (menu_draw.c) and by pointsrc_status(). */
int menu_is_open(void);
int menu_pressed(void);        /* button currently under the finger, 0 for none */

/* Forget everything. Called from pointsrc.c where the touch device is lost
 * (beside touch_zone_reset()), so a panel that was unplugged mid-swipe cannot
 * leave a menu that nothing can dismiss. */
void menu_reset(void);

#endif /* RBLIVE4_MENU_ZONE_H */
