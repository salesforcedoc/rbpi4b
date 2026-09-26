/*
 * ctrl_map.h -- what a control surface is, in one struct.
 *
 * The shim's front end (ctrlshim.c) is deliberately ignorant of what is plugged
 * in: it reads the environment, starts the threads, hands each sequencer event
 * to the selected map, and nothing else knows a note number. Everything that is
 * a particular surface -- which note is PLAY, which CC pair is the jog, what a
 * pad's colour byte looks like -- lives in a map, chosen by MIDI_MAP.
 *
 * A map gets its meaning from rbp through rbp_bridge.h's named functions
 * (send_rx_key(), me_set_cue(), me_set_master_cue(), ...), so a map never
 * contains an rbp address, and a bridge never learns a note number. The reverse
 * of that rule is what makes the FLX4 work a table edit rather than a rewrite.
 *
 * The binding tables below are shared storage rather than per-map state: every
 * surface is "some buttons and some absolute controls", the dispatcher has to
 * be able to invalidate the absolute cache, and two maps with two copies of that
 * bookkeeping is how they drift apart.
 *
 * Not every surface is MIDI, though. A keyboard is an evdev device, so a map
 * says whether it wants a non-MIDI source through devices()/input() at the end
 * of struct ctrl_map.
 *
 * TWO MAPS RUN AT ONCE, on two independent selections: MIDI_MAP picks the
 * sequencer surface and EVDEV_MAP picks the non-MIDI one, so the FLX4 and a
 * keyboard drive rbp together with no configuration. The three consequences a
 * reader of this file needs:
 *
 *  - `devices()`/`input()` still describe ONE surface's need for a non-MIDI
 *    source; the front end asks the evdev map, not the MIDI one. A MIDI-only map
 *    leaves both NULL and that is not a gap -- nothing on that side wants them.
 *  - The two selections may name the SAME map (MIDI_MAP=kbd), and then it is
 *    built, started and ticked once rather than twice.
 *  - `build()` is ADDITIVE. ctrl_bindings_reset() is called by the front end,
 *    exactly once, before either build -- a build that reset the shared tables
 *    would wipe the other surface's bindings, and map_kbd.c's leaves them empty
 *    on purpose. See the note on ctrl_bindings_reset() below.
 */
#ifndef RBLIVE4_CTRL_MAP_H
#define RBLIVE4_CTRL_MAP_H

#include <sound/asequencer.h>   /* struct snd_seq_event */

/* ---- the binding tables -------------------------------------------------- */

#define CTRL_NKEYS 96
#define CTRL_NABS  40

/* Button: a note on a receive channel that means an rbp keycode. `key` 0 marks
 * a control rbp has no code for, which is logged and otherwise ignored. */
struct note_ctrl {
     int rch;              /* receive channel (0-based sequencer channel) */
     int note;
     int key;              /* rbp keycode, 0 = log-only */
     int sch;              /* send channel for sendKey */
     int pressed;
};
extern struct note_ctrl note_map[];
extern int note_map_n;

/* Absolute control: a CC whose 7-bit value is a position. `last` is the 10-bit
 * value last sent, so a CC that repeats is dropped; -1 means "unknown", which
 * is what makes the panel's reply to led_query_absolute() re-apply. */
struct abs_ctrl {
     int rch, cc, key, sch, last;
};
extern struct abs_ctrl abs_map[];
extern int abs_map_n;

/* There are two kinds of row and no third. In particular a CC cannot be a
 * BUTTON: a row here is always a position, so a control that a surface reports
 * as a CC toggling at some threshold (>= 64 is the usual convention) has no
 * entry kind to live in. That was expected to be needed for the FLX4 and is
 * not: Pioneer's list for that unit puts every toggle on a note and every CC
 * on a 14-bit position pair or a relative encoder, so map_flx4.c needs nothing
 * here and this header is unchanged by it. A surface that did carry such a
 * switch would need a decision, not a hack -- the two options are a flag on
 * struct abs_ctrl, or a third table with a threshold -- and until one exists,
 * the honest answer at a call site is a comment saying so. */

/* Empty both tables. Called by the front end exactly once, before any build():
 * two maps can be live at once, so a build that reset them would wipe the other
 * surface's bindings -- and map_kbd.c's build leaves both tables empty on
 * purpose, so the wipe would be of the FLX4's bindings by a map that put nothing
 * in their place. A build() must therefore ADD to the tables as it finds them.
 *
 * Kept here rather than inlined at the call site because the absolute cache and
 * the bindings have to be emptied together. */
void ctrl_bindings_reset(void);
void add_note(int rch, int note, int key, int sch);
void add_abs(int rch, int cc, int key, int sch);

/* Forget every "last value sent". Called after the panel has been asked to
 * re-report its positions: it answers with the same physical values, so an
 * intact cache would drop the reply. */
void ctrl_abs_invalidate(void);

/* ---- the map ------------------------------------------------------------- */

struct ctrl_map {
     const char *name;      /* the value MIDI_MAP selects it by */

     /* Fill the binding tables, and read whatever environment this surface
      * needs (jog resolution, polarity, ...). Runs once, before any event. */
     void (*build)(void);

     /* rbp-side start-up that only makes sense with this surface attached.
      * Runs once, after rbp's KeyManager exists and before the first event is
      * dispatched, so anything sent here is not lost. */
     void (*startup)(void);

     /* One sequencer event. rbp's KeyManager is known to exist. */
     void (*event)(const struct snd_seq_event *ev);

     /* ~20 ms heartbeat: button-hold timeouts and idle state, the two things
      * that are about time rather than about an event. Optional. */
     void (*tick)(void);

     /* ---- non-MIDI sources -------------------------------------------------
      *
      * A map is not always a MIDI surface: a keyboard and a mouse are
      * /dev/input/event* devices, and the sequencer will never show them. The
      * pair below is how such a map asks for its events, and it is deliberately
      * as small as this: the front end asks one question (does this surface want
      * a non-MIDI source at all?) and then hands over raw triples.
      *
      * The question is asked of the EVDEV_MAP selection and never of the
      * MIDI_MAP one, which is what lets a MIDI controller and a keyboard be live
      * together (see the header). A map that leaves both members NULL is
      * MIDI-only, and that no longer means "no keyboard" -- it means "not this
      * map's business", because the other side of the selection is asked
      * separately.
      *
      * devices() answers that question with a count rather than a flag, because
      * the caller's line reads better with one: anything above zero makes the
      * front end start the evdev reader against input(). */
     int (*devices)(void);

     /* One raw evdev event, (type, code, value) exactly as the kernel delivered
      * it, on the reader's thread. The map decides what it means -- the codes
      * are just integers, and there is no keyboard-specific hook -- so the
      * reader never learns a keycode. Only ever called when devices() > 0, and
      * only once rbp's KeyManager exists. */
     void (*input)(int type, int code, int value);
};

/* "Nothing on this side." The absence of a surface is a real selection and not a
 * missing one: EVDEV_MAP=none is the way back to a MIDI controller with no
 * keyboard and no mouse buttons, without a rebuild and without the silent
 * fallback to a map the operator did not ask for. */
extern const struct ctrl_map map_none;

/* The SC Live 4 / Prime GO surface, which is what this shim grew up on. */
extern const struct ctrl_map map_jp21;

/* The keyboard fallback: no controller, no MIDI, playable with an empty USB bus. */
extern const struct ctrl_map map_kbd;

/* The DDJ-FLX4, the surface this port targets. Its tables come from Pioneer's
 * published DDJ-FLX4 MIDI message list and are unverified until a dump from the
 * unit confirms them -- see the provenance block in map_flx4.c and docs/15. */
extern const struct ctrl_map map_flx4;

#endif /* RBLIVE4_CTRL_MAP_H */
