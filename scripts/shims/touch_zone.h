/*
 * touch_zone.h -- the one part of the panel that means something to the shim
 * rather than to rbp.
 *
 * Almost every touch belongs to rbp: pointsrc hands the record to tscfake_emit()
 * and rbp decides what it hit. The two deck QUANTIZE boxes are the exception --
 * on the PERFORMANCE screen. rbp draws them there, but its own touch classes do
 * not bind them: measured on the unit (2026-09-28), taps on the widget at both x
 * conventions left the frame pixel-identical, while a positive control (a tap on
 * INFO) toggled the screen in the same run -- so the instrument reaches rbp's
 * handlers and the tap is simply not wired to an on/off at all. rbp has a touch
 * class for the *Beat FX* quantize and only value key handlers
 * (`UiKey_Shortcut_QuantizeValue{1,1_2,1_4,1_8}`) for the deck's.
 *
 * So the shim supplies the missing binding: a press that starts inside one of
 * the two rectangles below is turned into the keycode rbp already has for that
 * button -- K_QUANTIZE (rbp_abi.h), which is what a hardware press sends -- and
 * rbp does the rest, screen included.
 *
 * THE RECTANGLE IS NOT FREE ON EVERY SCREEN, and that is measured rather than
 * hoped for. On the BROWSE screen rbp binds a control of its own over the same
 * band: the deck strip's time display, where a touch toggles TIME/REMAIN (and
 * the deck selector's brackets with it). Two identical injections at the
 * widget's value field flipped that label and flipped it back again within one
 * run, and an injection on the label row just above did nothing -- so it is the
 * injection driving an rbp touch class, not a stray finger, and the class starts
 * somewhere between those two rows. A press there on the browse screen therefore
 * does both things: rbp's own time-mode toggle, and this keycode.
 *
 * The overlap cannot be narrowed away. The widget is a label row (the word
 * QUANTIZE) over the value field the operator aims at, and the browse screen's
 * live band begins immediately below the label row -- so every rectangle that
 * covers the field covers rbp's control too, and every rectangle that misses
 * rbp's control is off the field the operator is aiming at. RB_POINT_QUANTIZE_TAP=0
 * is what turns this module off; the collision is written down in docs/07-touch.md
 * and carries a drill row in docs/13-raspberrypi4.md.
 *
 * This module is pure: no rbp address, no environment, no I/O, no clock. That is
 * what lets test_point.c pin it on the host, and it is the same rule
 * point_xform.c follows for the same reason. pointsrc.c is the only thing that
 * has to know how to send a key, and it is the only caller.
 *
 * COORDINATES ARE LOGICAL, PRE-REFLECTION. The caller passes the position a
 * finger is at -- point_xform_abs/rel's output -- and not the reflected value
 * tscfake_emit() puts on the wire. rbp's UI and the operator's hand agree on
 * logical space; the reflection exists for rbp's benefit alone (see tscfake.h).
 */
#ifndef RBLIVE4_TOUCH_ZONE_H
#define RBLIVE4_TOUCH_ZONE_H

/* One call per pointer report, in the order the events happened.
 *
 *   down  the button/touch state the report carries (the reader's own `down`)
 *   x, y  the logical position
 *
 * Returns 0 when the report means nothing to us, or the deck number (1 or 2)
 * whose QUANTIZE box was just pressed.
 *
 * The press is recognized on the DOWN EDGE alone and is complete at that
 * moment: the caller sends the key's press and its release together, exactly as
 * aloop_apply() does with a pad, so rbp's KeyManager bookkeeping is left clean
 * and the toggle happens where a hardware press would put it -- under the
 * operator's finger, not on lift. That also means there is no armed deck to
 * remember and no release position to trust: a resting finger is a stream of
 * identical down reports and only the first one is an edge, a drag that passes
 * through the box never started there, and a device that reports a zeroed or
 * stale coordinate on release cannot silently complete a gesture.
 *
 * It is a TOGGLE and rbp reads its own state, so nothing here computes the new
 * value; two presses mean two toggles, as two hardware presses would. */
int touch_zone_feed(int down, int x, int y);

/* Forget the edge state. pointsrc calls this when the reader closes its device
 * and clears st_down: without it, a pointer unplugged while down would leave
 * the next device's first press looking like a continuation, and that tap would
 * be swallowed -- once, and then again on every replug. There is no key to
 * release here (the gesture is already complete), so this cannot strand one. */
void touch_zone_reset(void);

#endif /* RBLIVE4_TOUCH_ZONE_H */
