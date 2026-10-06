/*
 * midi_io.h -- the sequencer and the panel's MIDI output.
 *
 * In:  the ALSA sequencer, /dev/snd/seq. The control surface is found by name
 *      (MIDI_IN_MATCH) rather than assumed at a fixed client:port, because the
 *      client number depends on enumeration order and because the shim starts
 *      before the controller is necessarily connected.
 *
 * Out: whichever route to the surface's receive side is available, chosen at
 *      runtime and always logged -- the sequencer (a second local port
 *      subscribed to the surface's receive side) or a rawmidi node as the
 *      fallback. midi_out_ready() says whether either is up; LED and meter
 *      output is written through midi_note()/midi_cc() either way, so the
 *      modules that produce it never learn which route they are on.
 *
 * The absolute-control dedupe cache used to live here (as `struct abs_ctrl
 * abs_map[]`); it is in ctrl_map.h now, with the binding tables it belongs to.
 */
#ifndef RBPI4B_MIDI_IO_H
#define RBPI4B_MIDI_IO_H

#include <sound/asequencer.h>   /* struct snd_seq_event */

/* First candidate for the rawmidi route. The card number is not stable across
 * boots on a Pi -- HDMI audio, the controller's own USB audio and whatever else
 * is plugged in all take card indices -- so this node is tried first and then
 * midiC0D0..midiC7D0 in turn. */
#define MIDI_LED_DEV       "/dev/snd/midiC0D0"

/* The rawmidi route's fd, -1 when the sequencer route is in use or no device
 * was found. Read by the LED/VU paths only through midi_out_ready(). */
extern int led_fd;

/* The ALSA sequencer fd, and seq_setup() which opens it, creates our two ports
 * and looks for the surface. seq_fd < 0 means setup failed and the caller
 * should give up. */
extern int seq_fd;
void seq_setup(void);

/* Read sequencer events until the process dies, calling on_event for each. Does
 * not return. Retries the surface lookup on every idle timeout, so a controller
 * plugged in after the shim starts is picked up. */
void midi_poll_forever(void (*on_event)(const struct snd_seq_event *ev));

/* Panel output: three-byte Note On and control change, and the query that makes
 * the panel report its current absolute values. Every note goes out as a Note On
 * -- velocity 0 is OFF, not a Note Off, because the FLX4's LEDs ignore a real
 * Note Off (measured; see midi_io.c). */
int midi_note(int midi_ch, int note, int val);
int midi_cc(int midi_ch, int cc, int val);
/* 1 if the query actually went out (only the rawmidi route carries it, and only
 * a JP21-protocol panel answers), 0 if this surface cannot be asked where its
 * absolute controls are. The 0 is load-bearing: rbp's mixer builds with its
 * channel faders at zero, so a surface that cannot answer leaves rbp silent
 * until something seeds them -- see rbp_vu.c's vu_thread. */
int led_query_absolute(void);

/* 1 once some route to the surface exists. The LED and meter threads wait for
 * this instead of for a device node, because which device (if any) is the route
 * is decided here. */
int midi_out_ready(void);

#endif /* RBPI4B_MIDI_IO_H */
