/*
 * controllers.h -- one row per control surface, and it is the only place any of
 * this port's facts about a surface is written down.
 *
 * WHY THIS EXISTS. One physical device -- the DDJ-FLX4 -- used to be named by
 * four unrelated tokens in four places that never referenced each other:
 *
 *   the map        rb.conf's RB_MIDI_MAP          -> ctrlshim.c's default_midi_map
 *   how it is FOUND rb.conf's RB_MIDI_IN/OUT_MATCH -> midi_io.c's "FLX4" literal
 *   its sound card rb.conf's RB_AUDIO_DEV         -> audioshim.c's AUDIO_DEV_DEFAULT
 *   its USB id     docs/13 only                   -> nothing at all
 *
 * Change surface and you have to edit all four correctly. Miss one and it drifts
 * in silence: RB_AUDIO_DEV (DDJFLX4) is completely independent of RB_MIDI_MAP, so
 * selecting MIDI_MAP=jp21 on a bench leaves the audio pointed at the FLX4 and
 * nothing says so. The two defaults are here so that the literals are in ONE
 * place; rb.conf's settings stay as the operator's overrides (docs/15 tells the
 * operator to change the audio device there), and doctor.sh compares the two.
 *
 * The shape is the sibling port's `rx3-handoff/controllers.py` -- one dict keyed
 * by controller id, feeding detection, doctor and the install so they cannot
 * disagree. docs/17 §2.4 records what that buys them. This is the same idea in
 * this tree's idiom: a pure module, a struct in, no address of rbp's, no I/O, no
 * globals, no clock -- so test_controllers.c can link the production source and
 * pin every token.
 *
 * WHAT A ROW IS NOT. The note/CC bindings and the LED tables are NOT here. Those
 * are already one coherent file per surface (map_flx4.c, map_jp21.c), and the
 * sibling does not centralise them either. `kbd` and `none` are MAPS and have no
 * row: a keyboard is not a controller and neither is nothing. A surface with no
 * row keeps working -- the maps are selected by name, not through this table --
 * it just cannot be detected or described.
 */
#ifndef RBPI4B_CONTROLLERS_H
#define RBPI4B_CONTROLLERS_H

/* The controller a shim falls back to when the environment says nothing or says
 * something this build has never heard of. It is the surface this port targets
 * and the value rb.conf ships, so a shim started by hand with no environment and
 * a typo'd name both end up where they would have been anyway. The previous
 * target is one word away (MIDI_MAP=jp21); the point of the fallback is that a
 * mistake is LOUD, not that it is impossible. */
#define CONTROLLERS_DEFAULT_ID "flx4"

/* The JP21 protocol surface, named for the one caller that has to reach a
 * specific row rather than the selected one: midi_io.c's led_query_absolute()
 * sends this surface's absolute-value query, and that message only ever goes out
 * on the rawmidi route -- which is the JP21's route and the reason the lookup is
 * by id rather than by whatever the environment selected. Here, beside the
 * default, so that the two ids a caller may name are in the header that owns
 * them: a rename is then one edit and a compile error, not a lookup that quietly
 * returns NULL. */
#define JP21_CONTROLLER_ID "jp21"

struct controller {
     const char *id;          /* "flx4"  -- the MIDI_MAP value and the CLI key.
                               * Case-sensitive, like the map selection it names. */
     const char *name;        /* "DDJ-FLX4" -- human label for logs and doctor */
     const char *usb;         /* "2b73:0045" -- the unit's USB id, lowercase hex
                               * `vvvv:pppp`. Detection only; this port has no
                               * udev rules and matches the card and the
                               * sequencer port, not the USB id. NULL = not
                               * known, and the CLI prints it as unknown. */
     const char *alsa_hint;   /* "FLX4"  -- the ALSA sequencer PORT-NAME
                               * substring, which is how the surface is actually
                               * found (midi_io.c's find_surface). Matched
                               * case-insensitively and as a substring, so it is
                               * "FLX4" and not "DDJ-FLX4 MIDI 1". NULL or empty
                               * = not reachable by name; see
                               * controllers_by_hint(). */
     const char *card_id;     /* "DDJFLX4" -- the ALSA card id, as it appears in
                               * `hw:CARD=<id>,DEV=<n>`. NULL = not known. */
     const char *map_name;    /* "flx4"  -- the ctrl_map this row selects. It is
                               * the link from "which controller" to "which
                               * binding table", and it equals `id` for every row
                               * today; test_controllers.c pins that, so a row
                               * that renames one and not the other is caught. */
     /* The surface's OWN SysEx message, or NULL. The shim decides when it goes
      * out, and two surfaces want opposite things from it:
      *
      *   - the FLX4 has none (NULL). What keeps it talking is `keepalive`,
      *     below, and that is a poll rather than a greeting.
      *   - the JP21's is an absolute-value QUERY: Engine OS sends it at startup
      *     and this port re-asks it whenever rbp's fader positions are unknown
      *     (rbp_vu.c's calls to led_query_absolute()). It went out only on the
      *     rawmidi route even before this table existed, because the sequencer
      *     route carries three-byte LED/meter messages and nothing else.
      */
     const unsigned char *init;
     unsigned init_len;
     /* The vendor's keepalive, repeated every `keepalive_ms`, or NULL/0 for a
      * surface that does not want one. See controller_keepalive_due() for when
      * it is due and docs/17 §2.4 for where the FLX4's value comes from -- and
      * for why "the FLX4 needs one every 200 ms" is the sibling's claim and not
      * a measurement this port has taken.
      *
      * NOTE for the reader of a log: `keepalive_ms` is a PERIOD, so 0 is a real
      * way to say "never" and is not a fast poll. */
     const unsigned char *keepalive;
     unsigned keepalive_len;
     unsigned keepalive_ms;

     /* ---- mixers: rows that are about DETECTION, not about a binding table ----------
      *
      * `mixer` marks an EXTERNAL DIGITAL MIXER -- a device with channel strips of its
      * own, which is the only thing RB_MIXER_MODE=external is for. Such a row is NOT a
      * control surface: no map (`map_name` is NULL, and the maps are selected by name
      * anyway), no SysEx, no keepalive. What it buys is recognition, and recognition is
      * the whole feature: external routing APPLIES only while one of these is on the USB
      * bus, and the config page does not offer the setting while none is (usb_devices.h
      * asks the question, for both of them).
      */
     int mixer;

     /* A MODEL NAME, matched against what the kernel says the device is -- its USB
      * product string or its ALSA card id. Both sides are normalised before the
      * comparison (see controllers_mixer_by_product), so the spellings that differ only
      * in punctuation all reach one token. NULL for a row reached by its USB id alone,
      * and for every control surface. */
     const char *product;
};

/* The table. `at()` is for walking it (the CLI's detect, the tests); `find()`
 * and `by_hint()` are the two real lookups. */
unsigned controllers_count(void);
const struct controller *controllers_at(unsigned i);

/* By id, case-sensitively -- the same comparison ctrlshim.c's pick_map() makes
 * against a map's name, so "which controller" and "which map" cannot resolve
 * differently. NULL when the id is not a controller this build has, which is the
 * case for `kbd`, `none`, and for a typo. */
const struct controller *controllers_find(const char *id);

/* By a real ALSA port name, as find_surface() sees it: case-INSENSITIVE and a
 * SUBSTRING, mirroring midi_io.c's name_has() exactly -- this is not a second
 * matching rule, it is the same one. Rows whose `alsa_hint` is NULL or empty are
 * skipped, and both halves of that guard are load-bearing: an empty needle
 * matches every haystack, and a NULL one is dereferenced by strcasestr(). So a
 * hint-less surface (the JP21) cannot be reached by name, and no port at all can
 * reach a row that has no hint. NULL when nothing matches. */
const struct controller *controllers_by_hint(const char *port_name);

/* Rows whose `usb` equals `usb` (lowercase hex, "vvvv:pppp"), or NULL. This is
 * the CLI's `detect`; the comparison is done here so the CLI does not grow a
 * second spelling of the format. */
const struct controller *controllers_by_usb(const char *usb);

/* The fallback row, and never NULL: it is CONTROLLERS_DEFAULT_ID's row. The
 * table cannot be empty and the default cannot be missing without failing to
 * build, so a caller does not have to carry a NULL arm for it. */
const struct controller *controllers_default(void);

/* Is this row an external digital mixer rather than a control surface? NULL-safe, and 0
 * for NULL: a caller that has just failed a lookup should not have to carry an arm. */
int controller_is_mixer(const struct controller *c);

/* A MIXER row whose USB id matches ("vvvv:pppp"), or NULL. Restricted to mixer rows on
 * purpose -- a DDJ-FLX4 is in this table too, and it must never be the thing that turns
 * external routing on, because it has no channel strips to hand the mixing to. */
const struct controller *controllers_mixer_by_usb(const char *usb);

/* A MIXER row whose `product` token appears in `text`, or NULL.
 *
 * THE COMPARISON IS NORMALISED, and that is the point of it: one model is written three
 * ways by three sources -- the USB product string `DJM-900NXS2`, the ALSA card id
 * `DJM900NXS2`, and the model name `DJM-900NXS2` -- so both sides are lowercased with
 * everything outside [a-z0-9] dropped before the substring test. A matcher that only
 * knows one of those spellings fails on a real unit, and fails silently, which is the
 * shape of failure this whole port keeps paying for.
 *
 * TABLE ORDER DECIDES TIES, first match wins, so a family token (`DJM-900NXS`, which is
 * a substring of `DJM-900NXS2`) must be listed after the specific row it would otherwise
 * swallow. An empty token never matches -- the rule controllers_by_hint states, for the
 * same reason: an empty needle matches every haystack. */
const struct controller *controllers_mixer_by_product(const char *text);

/* Is the keepalive due at `now_ms`, given it last went out at `last_ms`?
 *
 * Pure, and it takes its clock as an argument, because the comparison is the
 * only place this feature can go quietly wrong. Both ends are
 * `unsigned long long` MILLISECONDS on purpose: on this target `long` is four
 * bytes, so a millisecond CLOCK_MONOTONIC in one wraps every 49.7 days and a
 * nanosecond one every 4.29 s -- and this tree has already paid for the second
 * once (audioshim's clock). The subtraction is unsigned, so `now_ms` below
 * `last_ms` underflows to a very large elapsed and reads as due: a wrapped clock
 * costs one early keepalive instead of a silence. See the definition for why
 * that is the arithmetic rather than an extra branch.
 *
 * Returns 0 for a row with no keepalive (NULL or keepalive_ms == 0), so the tick
 * thread needs no arm of its own for a surface that does not want one.
 *
 * `last_ms` is the caller's "never sent" sentinel: pass 0 and the first tick is
 * due, which is what the FLX4 wants (the poll should start with the shim, not
 * 200 ms later). */
int controller_keepalive_due(const struct controller *c,
                             unsigned long long now_ms,
                             unsigned long long last_ms);

#endif /* RBPI4B_CONTROLLERS_H */
