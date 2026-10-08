/*
 * rbp_led.h -- the LED bridge's interface to the rest of the shim.
 *
 * The three symbols ctrlshim.c needs -- the two threads, the LED_SWEEP flag they
 * share, and the loop-in latch the key handler sets and the LED refresh consumes.
 *
 * The transport state the edge drawers read is NOT here. It lives in
 * rbp_transport.h, in an object of its own linked into both shims, because the
 * drawers are painted by fbshim.so and this file is compiled into knobshim.so,
 * which fbshim is loaded BEFORE -- so fbshim cannot call into this file's symbols
 * even if this object exported them, which it does not (-fvisibility=hidden over
 * CTRL_OBJS). rbp_transport.h carries the whole account; do not reintroduce the
 * dependency by adding those accessors back here.
 */
#ifndef RBPI4B_RBP_LED_H
#define RBPI4B_RBP_LED_H

/* Reads rbp's LedStat table and mirrors it onto the panel. LED_DUMP=1 dumps the
 * table instead of driving, LED_SWEEP=1 runs the note sweep. */
void *led_thread(void *arg);

/* LED_SWEEP=1 only: sweeps every note in the map so a panel can be identified
 * without a dump. */
void *led_sweep_thread(void *arg);

/* LED_SWEEP. Written by ctrlshim.c's constructor before either thread starts,
 * and again by led_thread. */
extern int led_sweep;

/* "loop-in armed" per deck (0 = deck 1). The key handler sets it on LOOP IN and
 * the LED refresh turns it into the panel's blink; it is shared because the two
 * are in different modules. */
extern int led_loop_armed[2];

#endif /* RBPI4B_RBP_LED_H */

