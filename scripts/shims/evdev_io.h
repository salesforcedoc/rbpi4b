/*
 * evdev_io.h -- the non-MIDI event source: /dev/input/event*.
 *
 * A keyboard, a mouse and a trackball are not MIDI devices: /dev/input/event*
 * carries EV_KEY and EV_REL, not sequencer events, and no amount of sequencer
 * configuration will make a keyboard appear as a client. A surface that wants
 * one therefore says so through `struct ctrl_map`'s devices()/input() pair
 * (ctrl_map.h) and the front end starts this reader for it.
 *
 * What this module deliberately does NOT know:
 *
 *   - any keycode. It hands the map the raw (type, code, value) triple the
 *     kernel delivered and nothing else, so "which key is PLAY" stays a table in
 *     a map -- the same rule that keeps note numbers out of the bridge.
 *   - which devices are wanted. It opens every /dev/input/event* node that has
 *     EV_KEY: that is the keyboard *and* the mouse, and the mouse is where
 *     BTN_RIGHT and REL_WHEEL come from. Nothing here needs to tell them apart,
 *     because the map does not.
 *
 * It is a sibling of fbshim.so's pointsrc.c, not a share of it: that file is in
 * a different shared object, discovers its own device by capability to serve
 * rbp's fake tsc2007, and has its own state. Two readers of one evdev node are
 * fine -- both get every event -- which is why the mouse can drive the pointer
 * and BTN_RIGHT at the same time.
 */
#ifndef RBLIVE4_EVDEV_IO_H
#define RBLIVE4_EVDEV_IO_H

/*
 * Start the reader thread. Returns 0 if it was created, -1 otherwise (and then
 * nothing is read). Idempotent: a second call is a no-op and returns 0.
 *
 * `on_event` is called once per event, from that thread, with the kernel's own
 * triple. It is called for every device the reader has open, so a map cannot
 * tell from the arguments (and must not care) which node an event came from.
 *
 * The device set is discovered rather than configured: every /dev/input/event*
 * with EV_KEY, re-scanned whenever one goes away, so unplugging and replugging
 * a keyboard or a mouse needs no restart. KBD_DEV pins the reader to one node
 * instead of scanning (bring-up aid; with it set, a keyboard pinned means the
 * mouse is not read at all, and so BTN_RIGHT and the wheel are not either).
 */
int evdev_start(void (*on_event)(int type, int code, int value));

#endif /* RBLIVE4_EVDEV_IO_H */
