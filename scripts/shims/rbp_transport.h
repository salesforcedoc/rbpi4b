/*
 * rbp_transport.h -- the transport LED state that the LED thread and the edge
 * drawers share.
 *
 * WHY THIS IS ITS OWN OBJECT, AND ITS OWN FILE, AND NOT PART OF rbp_led.c.
 *
 * The state is WRITTEN by rbp_led.c's LED thread, which lives in knobshim.so, and
 * READ by the edge drawers, which are painted by fbshim.so. Those are two different
 * libraries, and rb.conf's RB_LD_PRELOAD loads them in that order -- fbshim.so
 * BEFORE knobshim.so -- so fbshim cannot call into knobshim: at fbshim's relocation
 * the name does not exist yet. Measured the hard way, on the unit: the first build
 * of this crossed the boundary by declaring the two accessors in rbp_led.h and rbp
 * died at launch with
 *
 *     /root/pdj/rbp: symbol lookup error: /usr/lib/fbshim.so:
 *         undefined symbol: rbp_led_transport_want
 *
 * and moved the failure three seconds in, past the point the panel opens -- a
 * startup crash whose message names the shim and not the drawer.
 *
 * The library that would have supplied the symbol could not have, either: every
 * object in knobshim's CTRL_OBJS list is compiled -fvisibility=hidden, so rbp_led.o
 * exported nothing at all. Two independent reasons, one symptom.
 *
 * The arrangement this file follows is the one the Makefile already documents for
 * state two preloaded libraries share (fader_state.o): ONE object, built WITHOUT
 * -fvisibility=hidden, deliberately in neither object list, and linked into BOTH.
 * A default-visibility symbol defined in two preloaded libraries resolves for BOTH
 * of them to the first in the search order, so there is one instance and both shims
 * see it -- and the arrangement keeps working if the order is ever changed.
 *
 * THIS OBJECT MUST STAY PURE: no syscall, no clock, no rbp address, no allocation.
 * It is the state and the accessors only. Deciding what the bits should BE stays in
 * rbp_led.c, where rbp's LedStat table and play engine are; that is also what lets
 * test_menu_dev link the real object and drive it directly.
 */
#ifndef RBPI4B_RBP_TRANSPORT_H
#define RBPI4B_RBP_TRANSPORT_H

/*
 * The three states travel as ONE PACKED INT and not as three fields or a struct, so
 * that the publish is a single aligned store and the read a single aligned load:
 * three separate words could be read across an update and hand the drawer a SYNC
 * from one tick beside a PLAY from the next, which is a deck rbp never had. Same
 * reason menu_draw.c packs its repaint key.
 */
#define RBP_TRANSPORT_SYNC 1
#define RBP_TRANSPORT_CUE  2
#define RBP_TRANSPORT_PLAY 4

/*
 * This deck's three, as rbp has them right now, ORed from the RBP_TRANSPORT_* bits
 * above. `deck` is 0 for deck 1, 1 for deck 2 -- the same index rbp_led.c uses for
 * everything else, and side_zone.h's SZ_LEFT/SZ_RIGHT by construction
 * (side_channel() is deck+1).
 *
 * Returns -1 when nothing has been read yet (rbp's play engine is not up, or the LED
 * thread has not run). That is the caller's "I do not know" and emphatically not 0,
 * which is the real and common "all three dark".
 *
 * `on` is the answer AT THE INSTANT OF THE READ, not "rbp asked for a blink": rbp
 * sets State 2 and leaves it, and the cadence is its own period in the same entry
 * (rbp_abi.h's LED_ENTRY_OFF_PERIOD), so a blink has to be resolved against a clock
 * somewhere. Resolving it in the LED thread, once per tick, is what makes the drawer
 * and the controller flash TOGETHER; a "blinking" flag handed to two painters would
 * flash them at the same RATE and at whatever phase each painter's own clock happened
 * to be in. The cost is that the drawer's flash is quantised to the LED tick (20 Hz),
 * which a 250 ms period divides evenly.
 */
int rbp_transport_get(int deck);

/* Publish one deck's reading. Called once per deck per LED tick, by the one thread
 * that reads rbp. */
void rbp_transport_set(int deck, int bits);

/* "Nothing is known" -- rbp's play engine is not up. Back to -1 for every deck, which
 * is what a started-but-not-yet-running player should look like on a drawer. */
void rbp_transport_clear(void);

/*
 * Raise this while a drawer is open, lower it when the last one shuts.
 *
 * It exists for ONE case: no controller attached, so midi_out_ready() is false and
 * led_refresh() would return before ever taking rbp's LED table. That is exactly the
 * unit the drawers are for, so without this they would be dark precisely when they
 * are load-bearing. The read it buys is the 1.5 ms two-sample LedStat snapshot
 * (led_table.h), taken at most once per 50 ms tick, and only while a drawer is out.
 */
void rbp_transport_want(int on);
int  rbp_transport_wanted(void);

#endif /* RBPI4B_RBP_TRANSPORT_H */
