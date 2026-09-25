/*
 * rbp_led.h -- the LED bridge's interface to the rest of the shim.
 *
 * Only the three symbols ctrlshim.c needs: the two threads, the LED_SWEEP flag
 * they share, and the loop-in latch that the key handler sets and the LED
 * refresh consumes.
 */
#ifndef RBLIVE4_RBP_LED_H
#define RBLIVE4_RBP_LED_H

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

#endif /* RBLIVE4_RBP_LED_H */

