/*
 * rbp_vu.h -- the meter bridge's interface to the rest of the shim.
 *
 * The fader positions live here rather than with the handler that writes them
 * because rbp meters pre-fader and the panel meters post-fader: the fader is an
 * input to the meter scaling, so it belongs with the meter code.
 */
#ifndef RBLIVE4_RBP_VU_H
#define RBLIVE4_RBP_VU_H

/* Rescales rbp's meter bitmask (and the audioshim's cue/master gains) onto the
 * selected panel's CC-driven meters, in the shape that panel's map asks for
 * (channel base, CC, value encoding, pre/post-fader, master address -- see
 * `struct led_notes` in ctrl_map.h).
 *
 * A surface with no meter row of its own, and a run with LED_VU=0, both skip
 * install_meter_hook() and the whole send loop, so rbp's machine code is not
 * patched for a meter nothing will read -- but either still waits for rbp's
 * mixer and asserts the master cue and stereo cue mode. Those are engine
 * defaults rather than meter features: rbp has no PFL keycode, so the cue bus is
 * the shim's to assert on every target, and it is a startup value the surface
 * may then toggle (the FLX4's MASTER CUE button does, read from
 * me_get_master_cue(); the SC Live 4 has no such button, and its strips 3/4 PFL
 * toggles it instead). VU_DEBUG=1 logs, VU_TEST=1 sweeps the panel's segments,
 * and LED_VU_SEGMENTS (default 11) is the segment count the rescale assumes
 * rbp's own meter has. */
void *vu_thread(void *arg);

/* Last channel-fader position (10-bit; index 1/2 = deck 1/2), written by the CC
 * handler of the surface in use (map_*.c), read by the meter scaling above.
 * 1023 = at the top, so the meters read full until the panel reports a real
 * position. */
extern int g_fader[3];
extern int g_fader_seen[3];   /* set once the panel has reported a fader */

#endif /* RBLIVE4_RBP_VU_H */

