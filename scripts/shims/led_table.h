/*
 * led_table.h -- the settled view of rbp's LED table, and the blink it asks for.
 *
 * rbp's ui::PanelComController::timerCallback rebuilds uif::LedStat every 20 ms.
 * A read taken while that is happening can show half of one rebuild and half of
 * the next -- a State from after and a colour from before, or an entry caught
 * mid-clear -- and every one of those is a state rbp never had. This tree has
 * already been bitten by the class once (a pad record read while rbp was
 * clearing it), which is why the reading is filtered rather than trusted.
 *
 * THE FILTER, and why this one. Two reads of the whole table a short interval
 * apart that AGREE describe a table nobody was writing while either was taken:
 * whatever changed in between changed in the first one too, so the agreement is
 * evidence and not luck. Only the entries that agree are acted on. An entry that
 * disagrees is in flux -- it keeps the value it had in the previous settled
 * table, so a momentary disagreement cannot blink an LED dark -- and an entry in
 * flux with no history is simply not there this tick, which is the same answer
 * as "rbp has no such LED" and is what every reader here already handles.
 *
 * The sibling port filters the same hazard the same way (Rx3-flx4,
 * control-shim.c:94: "publish only a state seen twice in a row"), which is
 * where this tree read the idea; the interval and the carry-forward are this
 * port's own choice.
 *
 * It is a separate module from rbp_led.c because it is PURE: buffers in, a
 * buffer out, no address of rbp's anywhere. rbp_led.c owns the reads (the
 * pointer chain, the interval, the sleep) and this owns the decision, so the
 * decision can be tested with a fabricated torn table -- see test_led_table.c.
 */
#ifndef RBLIVE4_LED_TABLE_H
#define RBLIVE4_LED_TABLE_H

#include "rbp_abi.h"   /* LED_ENTRY_SIZE, LED_DUMP_MAX: the table's shape */

/* The cadence for a blink rbp does NOT ask for -- this port's own rendering
 * choice, and unchanged from what it replaces. `blink = (led_tick & 8)` on the
 * bridge's 50 ms tick was 400 ms on and 400 ms off, so the fallback is that
 * same cycle spelled as a period. Keeping it means the only LEDs whose cadence
 * moves when the firmware's period is adopted are the ones rbp actually asks to
 * blink, which is what makes a drill's before/after readable.
 *
 * The engaged beat-loop pad (State 3) is the case that needs it: rbp marks that
 * pad with a state and a colour and NO period, because its intent is dimmer and
 * not faster, and a panel with only off and on has to say it somehow. */
#define LED_BLINK_FALLBACK_MS 800

/* Entries of `a` and `b` that read identically, copied to `out`; an entry that
 * changed in between carries what it said in `prev` (the previous settled
 * table, matched by (id, channel) and not by position, because the order is
 * rbp's). `prev` may be NULL with prev_count 0, which is the first tick.
 * Returns the number of entries written, which is never more than
 * LED_DUMP_MAX. */
unsigned int led_merge_settled(const unsigned char *a, const unsigned char *b,
                               unsigned int count,
                               const unsigned char *prev, unsigned int prev_count,
                               unsigned char *out);

/* The entry for (id, channel) in a table of `count` entries, or NULL. The
 * reader that wants State, colour, dim and period wants one lookup, not four
 * scans of the same table. */
const unsigned char *led_find(const unsigned char *tbl, unsigned int count,
                              unsigned int id, unsigned int ch);

/* On for the first half of the cycle, off for the second -- the half-duty blink
 * the sibling port uses and the only one an off/on-only panel can express.
 * `period_ms` is rbp's own when it asked for a blink and 0 when it did not, in
 * which case the caller's fallback cadence is used instead; a period below two
 * milliseconds is not a duty cycle at all and reads as on rather than as a
 * division this module would have to invent an answer for. */
int led_blink_on(unsigned long long now_ms, unsigned int period_ms,
                 unsigned int fallback_ms);

#endif /* RBLIVE4_LED_TABLE_H */
