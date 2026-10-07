/*
 * fxpad_zone.h -- the BPM detail cell becomes a MOMENTARY X/Y pad for the Beat FX.
 * PURE.
 *
 * WHY IT EXISTS. Below the BEAT FX panel's CH SELECT box rbp draws a black cell that
 * reads `125.0 BPM / 480 msec / 1 BEAT / QUANTIZE` -- the effect's time, its beat count
 * and the quantize state. rbp binds NO touch to any of it (measured -- see THE SWALLOW
 * below), and the operator's ask of 2026-10-07 makes that dead square the one place on
 * the panel where the effect's two continuous values can be flown by hand:
 *
 *     "in the beat fx window where the BPM detail is displayed allow it to be a x/y pad
 *      to control the beat fx x for level and y is for how many beats, engage when
 *      pressed down and allow dragging within the square to work and then when you
 *      release set it back to what the beat and level was before you pressed it."
 *
 * So: X is the level (the effect's depth), Y is how many beats, it is engaged while the
 * finger is down, and letting go puts all of it back exactly where it was.
 *
 * THE THREE ANSWERS BEHIND THE ONE SENTENCE are the operator's own, given when asked,
 * and they are decisions rather than defaults:
 *
 *   - **Y drives the discrete BEAT LADDER** (`BEAT <` / `BEAT >`: halve / double), not
 *     the continuous TIME knob. The cell already prints the beat count, so the ladder is
 *     the value the operator can read back while dragging.
 *   - **The press also turns the Beat FX ON**, and the release puts the on/off back
 *     too. A momentary pad that changed the level of a switched-off effect would move
 *     nothing audible, which is not what "control the beat fx" means.
 *   - **The pad draws a frame and a finger dot** -- because the LEVEL has no readout
 *     anywhere on rbp's panel (only the beat count and the time are printed), so without
 *     a dot X is flown blind.
 *
 * THE CELL IS RBP'S OWN INK, and it was measured off a live /dev/fb0 capture on
 * 2026-10-07 (rbp pid 16934, the BEAT FX panel on the glass):
 *
 *     BPM detail cell   x 1100..1259   y 216..352      <- 160 x 137
 *
 * -- the SAME ten-pixel inset from the grey plate (x 1090..1269) that the effect-name
 * cell and the CH SELECT box wear, and clear of all three of fx_zone.h's rects: the CH
 * box ends at y 203 and this cell begins at y 216, with thirteen rows of plate between
 * them. Row y 352 is rbp's own 8,8,8 border, so the rectangle stops there rather than
 * running into the plate grey below it. The `QUANTIZE` word inside the cell is rbp's
 * already-measured-dead `Shortcut_EffectQuantize_On/Off`, so nothing under this rect is
 * anything the operator can lose.
 *
 * THE SWALLOW WAS MEASURED BEFORE A LINE OF THIS WAS WRITTEN, with its own positive
 * control in the same run (2026-10-07, the shim doing nothing, work/tap.py writing
 * records straight into rbp's pipe). Counting changed pixels INSIDE the panel only --
 * a whole-frame count is useless here, because rbp's deck animation moves ~46,000 px
 * on its own, all of it outside x 1090..1269:
 *
 *     region                        idle   tap cell   tap cell   tap toggle
 *     BPM cell (1100..1259/216..352)   0         0          0           0
 *     STATUS/BEAT FX toggle            0         0          0        7098
 *     whole panel (1090..1269)         0         0          0        8178
 *
 * Two taps in the cell: **0 px**, twice. The control -- the segmented STATUS/BEAT FX
 * toggle in the same run -- moved **7098 px**. So rbp binds nothing here and withholding
 * these reports costs the operator nothing.
 *
 * THE STATE THIS PAD MOVES IS READ, NOT INFERRED, and the read is a pointer chase
 * `pointsrc.c` already performs. Disassembled out of `extracted/XDJRX3/pdj/rbp` on
 * 2026-10-07 and then read live through `/proc/16934/mem`, with two of the numbers
 * checked against the screen's own text:
 *
 *     ME_SINGLETON 0x011493c0 -> MixerEngine
 *       MixerEngine +0x58       -> BeatEffectManager        <- fx_ch_read() is already here
 *         BeatEffectManager +0x00  int   select channel     = 5    (screen: MASTER)
 *         BeatEffectManager +0x08  ptr   BeatEffect*        <- the CURRENT effect object
 *         BeatEffectManager +0x50  int   the effect TYPE     <- 0 IS the Off effect
 *           BeatEffect +0x20  float  level / depth          = 0.409
 *           BeatEffect +0x24  long   time, msec             = 480  (screen: "480 msec")
 *           BeatEffect +0x3c  byte   effect ON/OFF          = 0    (isBeatEffectOn)
 *           BeatEffect +0x44  long   beat button (label)    = 5    (screen: "1 BEAT")
 *           BeatEffect +0x48  long   beat button max        = 9
 *           BeatEffect +0x4c  long   beat button min        = 0
 *
 * Every accessor behind those is a two-instruction load, so this is a plain chase with
 * NO function call into rbp -- the same shape and the same risk as `fx_ch_read()`, with
 * both interior pointers NULL-checked. The pad refuses to engage when any link of the
 * chase is missing: a pad that cannot read the old value cannot put it back, and a
 * momentary pad that cannot restore is a pad that loses the operator's setting.
 *
 * AND ONE OF THOSE TWO FIELDS CANNOT BE BELIEVED ON ITS OWN. `+0x3c` says ON on an
 * effect that is switched off, so the pad reads `+0x50` as well and treats ON as
 * `+0x50 != 0 && +0x3c != 0`. The reason is not a rounding detail -- it is the
 * operator's own bug report of 2026-10-07, *"when i switch an effect and then press the
 * x/y pad it doesn't engage, but it does after i turn the newly selected effect
 * on/off"*, and it is measured rather than argued:
 *
 *   - Type 0 is the OFF state, not "nothing selected" -- it is the one entry of rbp's
 *     14-position effect table that maps to no position. Once the manager has been
 *     driven, the object it leaves at +0x08 for it is a `BeatEffectOff`, and that class
 *     has every control the pad drives compiled out -- `changeEffectStatusToOn`
 *     @0x8b0b8, `changeEffectStatusToOff` @0x8b0b4, `changeLevelDepthValue` @0x8b0ac and
 *     `changeTimeValue` @0x8b0b0 are all a bare `bx lr`.
 *   - So nothing maintains that object's `+0x3c`, and on a switched-off effect it reads
 *     **1**. Measured: moving the effect selector ONE POSITION while the effect is off
 *     leaves the type at 0 and flips `+0x3c` from 0 to 1. Turning the effect off with
 *     `K_BFX` reaches the same state. (A freshly started rbp that has never had its Beat
 *     FX touched also reads type 0, but with a stale REAL object at +0x08 whose own class
 *     maintains the flag -- so that one reads 0, and is right. It is a CONJUNCTION, not an
 *     equivalence, and the conjunction answers "off" in both cases.)
 *   - The pad then read `on`, wanted `on`, agreed -- and sent no toggle at all, on a
 *     `BeatEffectOff` that ignores the level and the rung as well. Nothing happened
 *     anywhere: "it doesn't engage". Pressing rbp's own ON/OFF button re-points +0x08
 *     at the newly selected type's real object, where `+0x3c` IS maintained -- which is
 *     why the operator's "turn it on/off" makes the pad work again, and why the fix is
 *     to stop believing `+0x3c` alone rather than to change anything the pad sends.
 *
 * rbp_abi.h carries the measured table this rests on, including the cold-start row that
 * makes it an inequality rather than a plain rule.
 *
 * THE WIRE FORMS ARE ALL PROVEN ELSEWHERE IN THIS TREE. Nothing here is a discovery:
 *
 *     level   send_rx_key_f(K_DEPTH 0x448f, OP_VALUE, CH_GLOBAL, v, v/1023.0f)
 *             -- map_flx4.c:1095-1096, exactly what flx4_cc_abs() puts on the wire
 *     beats   send_rx_key_fl(K_BEATNEXT 0x4491 / K_BEATPREV 0x4490, OP_PRESS,
 *             CH_GLOBAL, d, 0.0f, d) with d = +1 / -1
 *             -- map_flx4.c:531-541, map_jp21.c:542-546. `OP_PRESS` is REQUIRED:
 *             asEventCode gates 0x4490/0x4491 on (op & 0xf) == 0
 *     on/off  fx_power_send(): K_BFX 0x448d PRESS then RELEASE -- pointsrc.c:1007-1011
 *
 * AND IT WAS PUT ON THE WIRE, on this unit, before this module was trusted with it. On
 * 2026-10-07 the four forms were sent through the shim's own sequencer port (seqinject2),
 * which runs `map_flx4.c`'s real handlers -- whose last hop IS the two calls above -- and
 * rbp's words were read back out of /proc/<rbp>/mem after each one:
 *
 *     K_DEPTH  cc 100 -> BeatEffect+0x20 = 0.7879 (806/1023); cc 20 -> 0.1574 (161/1023)
 *              ABSOLUTE and exact, so one send lands the level and one send puts it back.
 *     K_BEATNEXT  rung 5 -> 6 -> 7            K_BEATPREV  rung 7 -> 6
 *              one rung per press, and the DIRECTIONS ARE MEASURED rather than inferred
 *              from the button's name -- which is the whole reason the climb below can
 *              trust "next is up".
 *     K_BFX    not re-sent: it is the audible one and the operator's own hand has already
 *              proven it. +0x3c read 1 throughout the run, so the effect was ENGAGED while
 *              those levels moved -- and every one of them was put back, exactly.
 *
 * rbp REPAINTED THE CELL ITSELF through all of it: at 125.0 BPM the same square went
 * `480 msec / 1 BEAT` -> `960 / 2 BEAT` -> `1920 / 4 BEAT`, following +0x24 and +0x44.
 * So the panel is a live readout of this struct, and +0x24 is rbp's own recomputation
 * (rung 5 is "1 BEAT", 6 "2 BEAT", 7 "4 BEAT" -- the halving/doubling ladder, visible).
 * That is the fact the HUD rests on: rbp will be painting these pixels underneath, so
 * the dot must be drawn OVER A COPY of them and never in place of them.
 *
 * WHAT THIS MODULE DOES NOT DO. It never reads rbp, the framebuffer or a clock, and it
 * never sends. rbp's current state arrives as a `struct fxpad_live` the caller filled,
 * and this tick's answer leaves as a `struct fxpad_out` the caller sends. That is what
 * lets test_fxpad.c pin the two mappings, the snapshot, the closed-loop ladder and the
 * restore on a host with no Pi, no panel and no player.
 */
#ifndef RBPI4B_FXPAD_ZONE_H
#define RBPI4B_FXPAD_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* --------------------------------------------------------------- the cell
 *
 * Inclusive at both ends, in the menu's own 1280x800 logical space, and written as
 * literals rather than derived -- they are a measurement of rbp's drawing, and a
 * derived number would only hide which one moved when rbp's layout changes. The
 * comment above is the provenance; test_fxpad.c asserts the cell sits inside the plate
 * and overlaps none of fx_zone.h's three rects.
 */
#define FXPAD_X0 1100          /* rbp's black BPM cell, measured 2026-10-07 */
#define FXPAD_X1 1259
#define FXPAD_Y0  216
#define FXPAD_Y1  352

/* 160 x 137 logical px. The X span is rbp's own cell width and does not need to be
 * anything else; the Y span is the drawn cell's full height because the ladder has only
 * beat_min..beat_max rungs to spread over it and a short pad would make two adjacent
 * rungs hard to hit. */
#define FXPAD_W  (FXPAD_X1 - FXPAD_X0 + 1)     /* 160 */
#define FXPAD_H  (FXPAD_Y1 - FXPAD_Y0 + 1)     /* 137 */

/* The level's full-scale value on the wire. rbp's K_DEPTH takes the same 0..1023 the
 * FLX4's own knob is scaled to (map_flx4.c's cc_to_10bit), and the float it carries is
 * that number over 1023. Named here rather than typed twice, because the mapping and
 * its inverse must agree. */
#define FXPAD_DEPTH_MAX 1023

/* The on/off axis's give-up, and the reason it needs one at all: K_BFX is a TOGGLE, so
 * the only way to know whether the effect is on is to read rbp back -- and a toggle that
 * rbp does not answer must not be repeated forever, or it would flip the effect on and
 * off under the operator's hand for as long as they held the pad. So: ask, wait up to
 * FXPAD_ON_WAIT ticks for rbp's answer to agree, ask again (the answer is read, so a
 * second ask is a correction and not a blind repeat), and stop after FXPAD_ON_TRIES
 * attempts. The wait is in TICKS of the pad's own loop -- pointsrc.c's
 * POINT_MENU_HOLD_TICK_MS, 20 ms -- so six of them is 120 ms, comfortably longer than
 * rbp takes to apply a key and short enough that a real drop is corrected before the
 * operator has let go.
 *
 * Note what the bound costs when everything works: nothing. rbp answers on the next
 * tick, so a working toggle is one send and one read and never reaches the wait. */
#define FXPAD_ON_WAIT   6
#define FXPAD_ON_TRIES  3

/* --------------------------------------------------- what rbp says right now
 *
 * Filled by the caller from the chase in the header comment -- nothing in this file
 * goes and looks. `beat_min`/`beat_max` are rbp's own limits for the CURRENT effect
 * type, and the ladder between them is NOT uniformly spaced (the rungs halve and
 * double), which is why the target below is approached by climbing rather than
 * computed as a step count. Any field the caller could not read must be answered by
 * NOT engaging at all rather than by a made-up value.
 *
 * `type` is here for one reason and it is not decoration: `on` CANNOT BE BELIEVED when
 * `type` is 0, because type 0 is the `BeatEffectOff` class and that class never
 * maintains the flag (see the header comment, and the operator's bug it caused). The
 * two are read from the same `BeatEffectManager`, so asking for both costs the caller
 * nothing, and the module -- not the caller -- owns the rule that joins them, so
 * test_fxpad.c can pin it.
 */
struct fxpad_live {
    int type;                  /* BeatEffectManager+0x50: 0 IS the Off effect */
    int on;                    /* BeatEffect+0x3c: 0 off, 1 on -- MEANINGLESS if type==0 */
    int depth;                 /* BeatEffect+0x20 as 0..FXPAD_DEPTH_MAX */
    int beat;                  /* BeatEffect+0x44: the current rung */
    int beat_min;              /* BeatEffect+0x4c */
    int beat_max;              /* BeatEffect+0x48 */
};

/* ------------------------------------------------------ what to send this tick
 *
 * The caller sends these in the order they are declared, back to back and with no
 * sleep: a wait inside the shared input loop starves the band, the window, the drawers
 * and all three boxes (docs/07-touch.md TRAP 2).
 */
struct fxpad_out {
    int want_on;               /* -1 no change, 0 turn off, 1 turn on */
    int depth;                 /* -1 none, else 0..FXPAD_DEPTH_MAX */
    int beat_step;             /* -1 one rung down, 0 none, +1 one rung up */
};

/* ------------------------------------------------------------------- the input */

/* IS THE EFFECT RUNNING, as far as rbp can be believed? `type != 0 && on` -- the two
 * words are read together for the reason in the header comment, and this is the ONLY
 * place the rule is written down. Exposed so pointsrc.c's own log line can print what
 * the pad concluded without restating it (a second copy is a copy that can drift), and
 * so test_fxpad.c can pin the conjunction directly rather than through a whole gesture. */
int fxpad_live_on(const struct fxpad_live *live);

/* Is a logical point inside the cell? Exposed so the caller can tell a press it must
 * withhold from one rbp should keep, before it has committed to anything. */
int fxpad_hit(int x, int y);

/* The cell's inclusive rect. */
void fxpad_rect(int *x0, int *y0, int *x1, int *y1);

/* Feed one pointer report, exactly as pointsrc.c's funnel hands them out.
 *
 * A PRESS INSIDE THE CELL ENGAGES IT, and the press point sets BOTH axes immediately --
 * that is what makes this an X/Y pad rather than a knob (the alternative, "the first
 * move sets the value", would be a one-line change here). Every report until the release
 * is then TAKEN, including one that has slid off the cell: a feeder that swallowed the
 * press must see the release or it goes deaf, and the record for that is
 * declined-press-must-still-see-release. A finger that began OUTSIDE the cell is rbp's
 * for the whole of its gesture, however far it then travels across the panel.
 *
 * A RELEASE ENDS THE ENGAGEMENT and starts the restore; it answers nothing itself. A
 * release this module never saw a press for is not ours and goes back to rbp.
 *
 * Returns MZ_FEED_TAKEN for every report this module owns and MZ_FEED_NONE for the
 * rest, so the caller's ladder can do the swallowing. It is deliberately the same
 * contract as fx_zone.c's fx_feed() and hc_zone.c's hc_feed().
 *
 * It does NOT check whether rbp's state could be read: that is the caller's gate, asked
 * BEFORE the first feed of a gesture, and the reason is in the header comment -- a pad
 * that cannot read the old value must not engage.
 */
int fxpad_feed(int down, int x, int y);

/* ------------------------------------------------------------------- the tick
 *
 * One step of the pad's state machine. `live` is rbp as of this moment and may be NULL
 * (which answers three no-ops and changes nothing). `out` is filled with what to send
 * now; a field left at its "no change" value means exactly that.
 *
 * THE SNAPSHOT IS TAKEN ON THE FIRST TICK AFTER THE PRESS -- the only moment at which
 * rbp's `on`, `depth` and `beat` are still "what they were before the finger touched".
 * That snapshot IS the whole of the operator's "set it back to what the beat and level
 * was before you pressed it".
 *
 * THE THREE AXES ARE DRIVEN DIFFERENTLY, because they are three different kinds of
 * value:
 *
 *   - **Level (X).** `depth = (x - FXPAD_X0) / (FXPAD_X1 - FXPAD_X0) * 1023`, rounded,
 *     and sent WHENEVER IT DIFFERS FROM WHAT WAS LAST SENT. Absolute, so it needs no
 *     feedback loop and restores in one send.
 *   - **Beats (Y).** The top of the cell is the MOST beats. `beat_target` is
 *     `beat_min + (FXPAD_Y1 - y) / (FXPAD_Y1 - FXPAD_Y0) * (beat_max - beat_min)`, and
 *     it is reached by HILL-CLIMBING ON RBP'S OWN ANSWER -- at most one rung per tick,
 *     in the direction that closes the gap, and NO second step until rbp has moved off
 *     the rung the last step was taken from. That needs no knowledge of the ladder's
 *     spacing, and it cannot overshoot or double-step when rbp applies a key a tick
 *     late.
 *   - **On/off.** The desired state is ON while the finger is down (the operator's
 *     "engage when pressed down" -- the effect has to be running for the level to be
 *     heard) and back to the snapshot's own state once it is not. It is a TOGGLE on the
 *     wire, so it is ASKED FOR AND THEN WATCHED: the module reads rbp's answer back, and
 *     asks again -- up to FXPAD_ON_TRIES, FXPAD_ON_WAIT ticks apart -- while the answer
 *     still disagrees. While the finger is down that also RE-ARMS an effect rbp has
 *     switched off underneath the operator, which is the case the operator reported:
 *     *"when i change effects it doesn't seem to remember to trigger when i press until
 *     i turn it on off"* (2026-10-07).
 *
 *     Reading the answer is safe here for a reason the level's axis does not share: a
 *     flip that rbp HAS applied is one the read agrees with, so the loop stops there.
 *     A blind repeat would be the dangerous version, and this is not that.
 *
 *     AND WHAT IT READS IS `type != 0 && on`, not `on`. This is the axis that was
 *     silently dead, and it was dead because the answer it was watching can lie on a
 *     switched-off effect -- the header comment has the measurement and the operator's
 *     report. With the type in the test the pad now asks for the effect on in the one
 *     case it used to sit still for, which is the case the operator was in.
 *
 * A RELEASE FLIPS IT TO RESTORING with the targets set back to the snapshot values, and
 * the same drive loop above puts them home and then goes idle. The restore cannot live
 * in the release report: there is no report after a release, and rbp applies keys
 * asynchronously, so climbing on rbp's own answer is the only honest way to land on the
 * exact rung again.
 *
 * IT IS HOME WHEN THE VALUES HAVE ARRIVED, NOT WHEN THEY HAVE BEEN ASKED FOR. The level
 * is the one exception and only because it is absolute -- asking for it IS putting it
 * there. The ladder is on the rung, and the on/off has read back as the state the
 * operator had; each of the two may instead have given up, which is what keeps a restore
 * that rbp will not answer from holding the pad busy forever. The upshot is a witness
 * line -- `beat fx pad unwound` -- that is rbp's answer and not the shim's intent.
 *
 * IT GIVES UP RATHER THAN SPINS. If the ladder does not answer a step within
 * `2 * (beat_max - beat_min) + 4` ticks, the beat axis stops asking for the rest of the
 * gesture -- a rung rbp will not move off is a plateau, not a reason to send a key
 * forever. A new beat target clears the giving-up and tries again.
 *
 * A TAP THAT PRESSES AND RELEASES BETWEEN TWO TICKS sends nothing at all -- no tick
 * ever saw it engaged, so there is nothing to restore -- and the tick that finds itself
 * restoring with no snapshot simply goes idle. That is not a special case bolted on: it
 * is the same rule the whole module rests on, that the only thing worth undoing is
 * something that was actually sent.
 */
void fxpad_tick(const struct fxpad_live *live, struct fxpad_out *out);

/* --------------------------------------------------------------- the whole pad */

/* Engaged (finger down) OR restoring -- i.e. the caller must keep ticking. This is what
 * keeps the input loop's tick block alive with the finger held still, which is what lets
 * the ladder converge, and after the release, which is what puts it back. */
int fxpad_busy(void);

/* The finger is down on the cell right now. */
int fxpad_engaged(void);

/* Where to draw the dot, and whether to draw one at all: 1 while engaged (the finger's
 * position, clamped into the cell) and while restoring (where the value is going home
 * to), 0 when idle. The caller draws it whenever fxpad_busy(). */
int fxpad_mark(int *mx, int *my);

/* Drop everything, including a gesture in flight. pointsrc.c calls this from both of
 * its reset blocks, beside fx_reset() and the rest, so a pointer device that went away
 * cannot leave a pad engaged forever. */
void fxpad_reset(void);

#endif /* RBPI4B_FXPAD_ZONE_H */
