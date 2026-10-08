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
#ifndef RBPI4B_CTRL_MAP_H
#define RBPI4B_CTRL_MAP_H

#include <sound/asequencer.h>   /* struct snd_seq_event */

/* ---- the binding tables -------------------------------------------------- */

/* Big enough for the largest map with room to spare, and the size is
 * load-bearing in a way that is easy to miss: add_note()/add_abs() DROP a row
 * once the table is full, silently -- the binding simply does not exist, no
 * message is printed, and the control reads as an unmapped event. The FLX4 map
 * is the one that finds this ceiling: it reaches 99 rows (test_flx4.c pins the
 * count), and the previous 96 silently swallowed its last three -- a mixer note
 * and both Beat FX notes. The next obvious change to that map is binding the
 * pads' +SHIFT layer, which is another 64 rows; 192 covers it. */
#define CTRL_NKEYS 192
#define CTRL_NABS  40

/* Button: a note on a receive channel that means an rbp keycode. `key` 0 marks
 * a control rbp has no code for, which is logged and otherwise ignored. `repeat`
 * marks the handful of rbp handlers that need a second edge before the release
 * (see rbp_abi.h's OP_REPEAT) -- add_note() leaves it 0, so this is invisible to
 * every row that does not ask for it. */
struct note_ctrl {
     int rch;              /* receive channel (0-based sequencer channel) */
     int note;
     int key;              /* rbp keycode, 0 = log-only */
     int sch;              /* send channel for sendKey */
     int pressed;
     int repeat;           /* send OP_REPEAT between the press and the release */
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

/* The global FX group's rows, in the order rbp_led.c pairs them with rbp's
 * LedStat ids. Named rather than positional so a map can fill the array with
 * designated initializers and a reorder cannot silently swap two LEDs. */
enum led_fx {
     LED_FX_BFX_ONOFF = 0,
     LED_FX_CFX_FILTER,
     LED_FX_CFX_DUBECHO,
     LED_FX_CFX_NOISE,
     LED_FX_CFX_SWEEP,
     LED_FX_COUNT
};

/* How a pad's stored 0..255 RGB becomes a Note On velocity. One function per
 * value, because the encoding is a property of the SURFACE and not of the bridge:
 * the SC Live 4 uses Engine OS's 6-bit layout, the FLX4's is not assumed to be
 * the same, and a surface whose pads are not RGB at all says NONE. The FLX4 is
 * that surface, and it is a measured answer rather than a stand-in for one
 * nobody has taken: Pioneer's own MIDI list gives all 350 of that unit's LED
 * rows as a plain `OFF=0x00, ON=0x7F`, and the only row in the document carrying
 * a value RANGE is the CH LEVEL METER, which this header models as a meter
 * instead. NONE means the RGB is read, discarded, and any non-zero velocity
 * lights the pad. */
enum led_enc {
     LED_ENC_NONE = 0,     /* plain on/off: the pad takes any non-zero velocity */
     LED_ENC_PRIME_6BIT,   /* bits 4-5 red, 2-3 green, 0-1 blue (Engine OS Prime) */
};

/* How a meter LEVEL becomes a CC value. Not an led_enc: a meter is a level, not
 * a colour and not a state, and the two panels that have one disagree about the
 * shape of the number. An encoding this build does not know is refused rather
 * than guessed at, for the same reason as led_enc -- a wrong-but-plausible meter
 * is indistinguishable from a right one at the panel. */
enum meter_enc {
     METER_ENC_NONE = 0,
     METER_ENC_PRIME_SEGMENTS,  /* (1 << n) - 1; the Prime's meter is 6 segments */
     METER_ENC_FLX4_LEVEL,      /* 0x26 bottom .. 0x7F top; below 0x26 is dark */
};

/* ---- the panel LEDs ------------------------------------------------------
 *
 * A surface's ILLUMINATION note numbers. Same rule as every other table here:
 * these are the panel's numbers, and nothing in them is an rbp address. Which
 * rbp state each LED mirrors, and the LedStat ids it is read from, are rbp's and
 * stay in rbp_led.c/rbp_abi.h -- the map only says which note that surface lights
 * for it, and on which channel.
 *
 * The shape is flat, one field per control, because that is what a panel is: one
 * note per button. `-1` means THIS SURFACE HAS NO SUCH LED, and it is the
 * load-bearing value in the whole struct -- the reason the table exists is that
 * one target's notes must never be transmitted at a panel that does not have
 * them (driving the SC Live 4's numbers at an FLX4 lands on the Beat FX section,
 * where notes 16/17 are the FX CH SELECT legs). So an absent or unmeasured LED is
 * -1 and sends nothing, and a neighbouring surface's number is never substituted
 * for it. `0` is a legal note, so the "no LED" value cannot be zero.
 *
 * Both surfaces this build knows send a deck's LEDs on one channel per deck and
 * its pads on another. Only the DECK LEDs are evenly spaced, so those are a base
 * and a stride; the pads are two channels spelled out, because they are not (see
 * pad_ch below). */
struct led_notes {
     int deck_ch;          /* deck 1's send channel; deck 2 is +1 */

     /* one note per deck LED, in the order rbp_led.c reads them */
     int n_sync, n_cue, n_play, n_keylock, n_vinyl, n_slip;
     int n_loopin, n_loopout, n_autoloop;

     /* The four pad-mode buttons' LEDs, indexed by RBP'S OWN MODE VALUE --
      * 0 HOT CUE / 1 AUTO BEAT LOOP / 2 SLIP BEAT LOOP / 3 BEAT JUMP -- on the
      * deck channel like the rest of the deck group. -1 = no such LED.
      *
      * Indexed by mode rather than tracked from the last keycode on purpose:
      * the light means "this is the mode rbp is in", and rbp changes mode from
      * its own screen too. rbp_led.c reads the mode back instead of remembering
      * the last key it saw, so the two agree however the mode moved.
      *
      * These are the ONE LED group on this surface that cannot come from rbp's
      * LedStat table. Measured 2026-10-01: the whole 45-entry table was dumped
      * either side of each of the four modes (the four keycodes injected through
      * the shim's own port, deck 1's mode read back 0/1/2/3) and nothing in it
      * changed but the pad grid's RGB, ids 18..25. There is no entry to read,
      * so the mode comes from rbp's own reader (UI_GET_PADMODE, rbp_abi.h).
      *
      * `0` is a legal note, so -1 is the only usable "none" -- and that is a
      * trap for a map that simply omits this field, because zero-init reads as
      * "note 0". Every LED row sets all four. */
     int n_mode[4];

     /* ---- the pads ---------------------------------------------------------
      *
      * Two channels rather than the `base + deck` every other channel group
      * here uses, because the two surfaces disagree about the STRIDE: the SC
      * Live 4's pads sit on the channel after its deck LEDs (4 and 5), and the
      * FLX4's do not (7 and 9). On the FLX4 8 and 10 are the same eight pads'
      * +SHIFT layer, so a `+deck` rule would have sent deck 2's pads to the
      * shift channel -- a bug this field exists to make unstateable. `-1` on
      * the second is a surface whose deck 2 has no pads. */
     int pad_ch;           /* pads: send channel, deck 1; -1 = no pad LEDs */
     int pad_ch2;          /* pads: send channel, deck 2 */

     /* The pads' LED NOTE BASE, indexed by RBP'S OWN MODE VALUE -- the same
      * four and the same index as n_mode above: 0 HOT CUE / 1 AUTO BEAT LOOP /
      * 2 SLIP BEAT LOOP / 3 BEAT JUMP. Pad p's note is `base + p`. -1 = none.
      *
      * An array and not the single base this struct used to carry, because on
      * the FLX4 THE BASE MOVES WITH THE MODE: the unit re-addresses its own
      * eight pads when a mode button is pressed. Measured on the panel
      * 2026-10-01, all four bases, every pad pressed -- HOT CUE sends 0..7 on
      * ch 7, PAD FX 1 sends 16..23, the BEAT JUMP button 32..39 and SAMPLER
      * 48..55. A light sent to a note the panel has moved away from addresses
      * nothing, so the base has to follow rbp's mode the same way the surface's
      * pads do, from the same reader n_mode already uses.
      *
      * A surface whose pads keep their notes across modes sets all four entries
      * to that one base -- and that is a statement about that surface rather
      * than a formality: it is exactly what the `n_pad_first` field it replaces
      * already asserted, so the SC Live 4's row says the same thing it said
      * before in the shape this one needs.
      *
      * `0` is a legal base, so -1 is the only usable "none" -- the same trap as
      * n_mode above, and every LED row sets all four. */
     int n_pad_base[4];

     int pad_enc;          /* how an RGB value becomes a velocity (enum led_enc) */

     int strip_ch_first;   /* mixer strip m: send channel strip_ch_first + m */
     int strip_count;      /* how many strips this surface has; 0 = no strip LEDs */
     int n_strip_cue;      /* the strip CUE note; -1 = none */

     int master_ch_first;  /* master CUE goes to a run of channels, ... */
     int master_ch_count;  /* ... this many of them, all on the one note */
     int n_master_cue;     /* the master CUE note; -1 = none */

     int fx_ch;            /* the global FX group's send channel */
     int n_fx[LED_FX_COUNT];   /* indexed by enum led_fx; -1 = no such LED */

     /* ---- the meter --------------------------------------------------------
      *
      * Not a note and not a state, which is why it is at the bottom here rather
      * than among the LEDs above: a meter carries a LEVEL, arrives at the panel
      * as a CC rather than a Note On, and is written by a different file
      * (rbp_vu.c reads rbp's own meter segments and sends this). It is in this
      * struct anyway because it is a property of the SURFACE, which is the whole
      * point of the struct -- and because the FLX4 and the SC Live 4 disagree
      * about all four of the numbers below, so a hardcoded one is wrong for one
      * of them by construction.
      *
      * `n_meter_cc < 0` means THIS SURFACE HAS NO METER, and then nothing is
      * sent and rbp's segments are not even read -- the same rule as the notes,
      * for the same reason. */
     int meter_ch_first;   /* deck d (0-based) sends on meter_ch_first + d,
                            * the same shape as strip_ch_first above */
     int n_meter_cc;       /* the CC carrying the level; -1 = no channel meter */
     int meter_enc;        /* how a level becomes a CC value (enum meter_enc) */
     int meter_pre_fader;  /* 1 = the panel meters BEFORE the fader, as rbp does;
                            * 0 = after it, so rbp's own value must be attenuated
                            * by the channel fader before it is sent */
     int meter_master_ch;  /* master meter channel; -1 = no master meter */
     int n_meter_master_l; /* master meter CCs, left then right; the FLX4 has
                            * neither and leaves both at -1 */
     int n_meter_master_r;
};

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
/* Same row, plus OP_REPEAT on the press. Only for a key whose rbp handler gates
 * on the browse-key record reaching state 3 -- today that is K_TRACKFILTER and
 * nothing else (rbp_abi.h's OP_REPEAT has the measurement). A row added with
 * this and a handler that does not need it is a change in rbp's behaviour, not
 * a no-op, so it is a separate function rather than a fifth argument. */
void add_note_repeat(int rch, int note, int key, int sch);
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

     /* The panel LEDs this surface can light, or NULL when it has none to drive
      * (a keyboard has no LEDs; `none` has no surface at all). Optional, and NULL
      * is a real answer rather than an omission: it means "send nothing", which
      * is what keeps a selection with no LED table from being driven by one it
      * never published. */
     const struct led_notes *leds;
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

/* The selected MIDI surface's LED table, or NULL if it has none to drive. Defined
 * in ctrlshim.c because that is the only file that knows the selection, and asked
 * of the MIDI side alone: the LED panel belongs to the controller, so lighting
 * nothing under MIDI_MAP=kbd is a selection rather than an omission. A caller
 * that gets NULL sends nothing -- it must not fall back to another map's notes. */
const struct led_notes *ctrl_sel_leds(void);

/* Has the MIDI surface been selected yet? The front end picks it on midi_thread,
 * and every thread is started together from the constructor, so a thread that
 * needs the surface's table *at start-up* can ask before there is an answer.
 * ctrl_sel_leds() says NULL for both "not yet" and "this surface has no LEDs",
 * and a caller that must tell those apart has to ask this as well -- vu_thread
 * is the one that does, because it decides whether to patch rbp's machine code
 * for a meter at all and it got that answer wrong on its first run (see the
 * comment at the call site in rbp_vu.c). */
int ctrl_sel_ready(void);

#endif /* RBPI4B_CTRL_MAP_H */
