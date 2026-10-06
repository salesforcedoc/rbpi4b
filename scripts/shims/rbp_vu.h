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
 * handler of the surface in use (map_*.c) and by the touch drawers (pointsrc.c),
 * read by the meter scaling above. 1023 = at the top, so the meters read full until
 * a surface reports a real position.
 *
 * DEFINED IN fader_state.c, not here and not in rbp_vu.c: the two arrays must exist
 * exactly once across the whole shim, and that object is linked into fbshim.so with
 * default visibility so knobshim's copy is the same memory. fader_state.c says why. */
extern int g_fader[3];
extern int g_fader_seen[3];   /* set once a surface has reported a fader */

/* Record a fader position, and that one was reported -- the two are one fact, so
 * they are written together. `ch` is 1 or 2; anything else is ignored, and `v` is
 * clamped to 0..1023. */
void fader_state_set(int ch, int v);

#endif /* RBLIVE4_RBP_VU_H */

