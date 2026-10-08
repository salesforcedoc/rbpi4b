/*
 * test_controllers.c -- the per-controller table, pinned. No Pi, no rbp, no
 * address of rbp's anywhere in this file.
 *
 * It links the PRODUCTION controllers.c. The module is pure on purpose -- a
 * lookup in, a row out -- which is what lets the whole anti-drift claim be
 * checked on the host instead of by a drill on the unit. What is pinned:
 *
 * 1. THE FOUR TOKENS, TOGETHER. One physical device used to be named by four
 *    unrelated strings in four files. The point of the table is that they are
 *    now in one row and agree, so the FLX4's are asserted here as a set -- and
 *    asserted against the MEASURED values (2b73:0045 is docs/13's lsusb line,
 *    FLX4 is what find_surface() actually matched, DDJFLX4 is what /proc/asound
 *    reported, flx4 is the map that ran). A test that only said "non-NULL" would
 *    pass on a table that had quietly swapped the card for the map.
 *
 * 2. THE UNKNOWNS STAY UNKNOWN. The JP21's USB id, card id and port-name hint
 *    are NULL, and the tests make that a requirement rather than a gap: an empty
 *    string, or another row's value, fails. A guessed card id sends audioshim
 *    looking for a device that cannot exist, and a guessed hint makes the shim
 *    bind the LED route to the wrong surface -- both silently.
 *
 * 3. THE EMPTY NEEDLE. `controllers_by_hint()` walks a table where one row has
 *    no hint at all, and under strcasestr() an empty needle matches EVERY
 *    haystack -- so the hint-less row would be returned for the first port the
 *    kernel lists. That is the same trap midi_io.c's name_has() refuses, and it
 *    is pinned from both sides here.
 *
 * 4. THE KEEPALIVE'S CLOCK. controller_keepalive_due() takes its clock as an
 *    argument and both ends are 64-bit MILLISECONDS, because on this target
 *    `long` is four bytes: a nanosecond CLOCK_MONOTONIC in one wraps every
 *    4.29 s and this tree has already been bitten by exactly that. The wrap is
 *    pinned, at the boundary and either side of it.
 *
 * WHAT THIS FILE CANNOT CHECK, and it is worth being precise about: whether a
 * row's `map_name` really is a map the BUILD has. The maps' names live inside
 * `struct ctrl_map` initializers in map_*.c, and the registry that resolves them
 * is in ctrlshim.c -- neither can be linked into a host test, so the list below
 * is a COPY. That means this catches a controller row naming a map that does not
 * exist (the direction that silently breaks MIDI_MAP), and does NOT catch a map
 * being renamed out from under a correct row. The runtime half of that pair is
 * ctrlshim.c's pick_map(), which logs "<name> is not a map this build has" --
 * so a rename is loud in the log either way.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "controllers.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* The maps this build has, as ctrlshim.c's midi_maps[]/evdev_maps[] spell them.
 * A COPY, and the header above says which direction that leaves unguarded. */
static const char *const maps_the_build_has[] = {
    "flx4", "jp21", "kbd", "none",
};

static int is_a_map(const char *name)
{
    unsigned i;

    for (i = 0; i < sizeof maps_the_build_has / sizeof maps_the_build_has[0]; i++)
        if (strcmp(maps_the_build_has[i], name) == 0)
            return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 1. The shape of the table
 */

static void test_shape(void)
{
    const struct controller *c;
    unsigned i, j;

    CHECK(controllers_count() >= 2,
          "the table must carry at least the two surfaces this port has rows "
          "for, not %u", controllers_count());

    for (i = 0; i < controllers_count(); i++) {
        c = controllers_at(i);
        CHECK(c != NULL, "row %u must exist", i);
        if (!c)
            continue;

        CHECK(c->id && c->id[0], "row %u must have an id", i);
        CHECK(c->name && c->name[0], "row %s must have a human name", c->id);
        CHECK(c->map_name && c->map_name[0],
              "row %s must name the map it selects", c->id);
        if (c->map_name)
            CHECK(is_a_map(c->map_name),
                  "row %s selects map '%s', which this build does not have",
                  c->id, c->map_name);

        /* No field is the EMPTY STRING. NULL means "not known" and is written
         * as NULL; "" is a value that reads like one and matches every needle
         * in a substring search. The two must not be interchangeable, or the
         * empty-needle guard is the only thing standing between a missing hint
         * and a shim bound to the wrong surface. */
        CHECK(!c->usb || c->usb[0], "row %s: usb is \"\" rather than NULL", c->id);
        CHECK(!c->alsa_hint || c->alsa_hint[0],
              "row %s: alsa_hint is \"\" rather than NULL", c->id);
        CHECK(!c->card_id || c->card_id[0],
              "row %s: card_id is \"\" rather than NULL", c->id);

        /* Every SysEx is well-formed: a message that does not start with F0 or
         * end with F7 is a truncated paste, and the surface answers nothing.
         * Cheap, and it is the only structural check a byte blob can have. */
        if (c->init) {
            CHECK(c->init_len >= 2 && c->init[0] == 0xF0 &&
                  c->init[c->init_len - 1] == 0xF7,
                  "row %s: its SysEx must be F0..F7, %u byte(s)", c->id,
                  c->init_len);
        } else {
            CHECK(c->init_len == 0,
                  "row %s: NULL SysEx with a non-zero length", c->id);
        }
        if (c->keepalive) {
            CHECK(c->keepalive_len >= 2 && c->keepalive[0] == 0xF0 &&
                  c->keepalive[c->keepalive_len - 1] == 0xF7,
                  "row %s: its keepalive must be F0..F7, %u byte(s)", c->id,
                  c->keepalive_len);
        } else {
            CHECK(c->keepalive_len == 0,
                  "row %s: NULL keepalive with a non-zero length", c->id);
            CHECK(c->keepalive_ms == 0,
                  "row %s: a NULL keepalive cannot have a period", c->id);
        }

        /* Ids are the CLI's keys and MIDI_MAP's values, so a duplicate makes
         * one row unreachable and the other ambiguous. */
        for (j = i + 1; j < controllers_count(); j++) {
            const struct controller *o = controllers_at(j);
            CHECK(o && strcmp(c->id, o->id) != 0,
                  "two rows share the id '%s'", c->id);
        }
    }

    CHECK(controllers_at(controllers_count()) == NULL,
          "a row past the end must be NULL");
    CHECK(controllers_at(controllers_count() + 100) == NULL,
          "a row far past the end must be NULL");
}

/* ---------------------------------------------------------------------------
 * 2. The FLX4's four measured tokens, and the map link
 */

static void test_the_measured_row(void)
{
    const struct controller *f = controllers_find("flx4");

    CHECK(f != NULL, "the FLX4 must have a row");
    if (!f)
        return;

    /* The four tokens this table exists to hold together. Each is a MEASURED
     * value, not a plausible one: the lsusb id from docs/13, the port-name
     * substring find_surface() actually matched, the ALSA card id
     * `hw:CARD=DDJFLX4,DEV=0` was built from, and the map that ran. */
    CHECK(f->usb && strcmp(f->usb, "2b73:0045") == 0,
          "the FLX4's USB id must be 2b73:0045, not '%s'",
          f->usb ? f->usb : "(null)");
    CHECK(f->alsa_hint && strcmp(f->alsa_hint, "FLX4") == 0,
          "the FLX4's port-name hint must be FLX4, not '%s'",
          f->alsa_hint ? f->alsa_hint : "(null)");
    CHECK(f->card_id && strcmp(f->card_id, "DDJFLX4") == 0,
          "the FLX4's card id must be DDJFLX4, not '%s'",
          f->card_id ? f->card_id : "(null)");
    CHECK(f->map_name && strcmp(f->map_name, "flx4") == 0,
          "the FLX4's map must be flx4, not '%s'",
          f->map_name ? f->map_name : "(null)");

    /* id and map_name are the same word for every row today. Pinned so that a
     * row which renames one and not the other is a test failure rather than a
     * shim that selects a map nobody can ask for by controller name. */
    CHECK(strcmp(f->id, f->map_name) == 0,
          "the FLX4's id ('%s') and map_name ('%s') must not drift apart",
          f->id, f->map_name);
}

/* ---------------------------------------------------------------------------
 * 3. The JP21's unknowns, and the empty needle
 */

static void test_the_unknowns_stay_unknown(void)
{
    const struct controller *j = controllers_find("jp21");
    const struct controller *f = controllers_find("flx4");

    CHECK(j != NULL, "the JP21 must have a row");
    if (!j)
        return;

    CHECK(j->map_name && strcmp(j->map_name, "jp21") == 0,
          "the JP21's map must be jp21, not '%s'",
          j->map_name ? j->map_name : "(null)");
    CHECK(strcmp(j->id, j->map_name) == 0,
          "the JP21's id and map_name must not drift apart");

    /* NOT KNOWN means NULL, and the tests say so -- an empty string would be a
     * value that matches every needle, and another row's token would be a
     * guess presented as a measurement. */
    CHECK(j->usb == NULL, "the JP21's USB id is not recorded here and must stay "
                          "NULL rather than guessed (found '%s')",
          j->usb ? j->usb : "(null)");
    CHECK(j->alsa_hint == NULL, "the JP21 is not named in the sequencer list, so "
                                "its hint must stay NULL (found '%s')",
          j->alsa_hint ? j->alsa_hint : "(null)");
    CHECK(j->card_id == NULL, "the JP21's card id is not recorded here and must "
                              "stay NULL (found '%s')",
          j->card_id ? j->card_id : "(null)");

    /* And no row borrowed the other's tokens. */
    if (f) {
        CHECK(!j->usb || !f->usb || strcmp(j->usb, f->usb) != 0,
              "the JP21 must not borrow the FLX4's USB id");
        CHECK(!j->card_id || !f->card_id || strcmp(j->card_id, f->card_id) != 0,
              "the JP21 must not borrow the FLX4's card id");
        CHECK(!j->alsa_hint || !f->alsa_hint ||
              strcmp(j->alsa_hint, f->alsa_hint) != 0,
              "the JP21 must not borrow the FLX4's port-name hint");
    }

    /* THE EMPTY NEEDLE. A hint-less row must be unreachable by name from both
     * sides: neither an empty/absent port name, nor a real one that does not
     * contain the hint. Without the guard, strcasestr(hay, "") returns the
     * haystack and this returns the JP21 for every port in the system. */
    CHECK(controllers_by_hint(NULL) == NULL, "a NULL port name matches nothing");
    CHECK(controllers_by_hint("") == NULL, "an empty port name matches nothing");
    CHECK(controllers_by_hint("Midi Through") == NULL,
          "a port that is neither surface matches nothing");
    CHECK(controllers_by_hint("SC Live 4 MIDI 1") == NULL,
          "the JP21 has no hint, so its own port name must NOT find it by "
          "name -- it is reached on the rawmidi route");
    CHECK(controllers_by_hint("DDJ-400 MIDI 1") == NULL,
          "a controller this build has no row for matches nothing");
}

/* ---------------------------------------------------------------------------
 * 4. The two real lookups
 */

static void test_find_and_hint(void)
{
    const struct controller *f = controllers_find("flx4");

    /* find() is case-SENSITIVE, because it is the same comparison ctrlshim.c's
     * pick_map() makes against a map's name -- so "which controller" and "which
     * map" cannot resolve differently. `MIDI_MAP=FLX4` is a typo and the shim
     * warns about it; this lookup must not quietly accept it. */
    CHECK(f != NULL && f == controllers_at(0),
          "find(\"flx4\") must be the table's first row");
    CHECK(controllers_find("FLX4") == NULL,
          "find() must be case-sensitive: MIDI_MAP=FLX4 is a typo, not a hit");
    CHECK(controllers_find("") == NULL, "an empty id is not a controller");
    CHECK(controllers_find(NULL) == NULL, "a NULL id is not a controller");
    CHECK(controllers_find("Flx4") == NULL, "find() must be case-sensitive");
    CHECK(controllers_find("ddj-flx4") == NULL,
          "the USB product name is not the id");

    /* kbd and none are MAPS with no row: they must not be findable, because a
     * caller that got a row back would take the FLX4's card and port facts for
     * a keyboard. */
    CHECK(controllers_find("kbd") == NULL, "kbd is a map, not a controller");
    CHECK(controllers_find("none") == NULL, "none is a map, not a controller");

    /* by_hint() mirrors name_has(): case-insensitive SUBSTRING, so a real port
     * name like "DDJ-FLX4 MIDI 1" finds the row. All three spellings of the one
     * rule are checked, because a rule that is only substring would pass the
     * first and a rule that is only case-insensitive would pass the second. */
    CHECK(controllers_by_hint("FLX4") == f, "the hint itself must match");
    CHECK(controllers_by_hint("flx4") == f, "the match must be case-insensitive");
    CHECK(controllers_by_hint("DDJ-FLX4 MIDI 1") == f,
          "a real ALSA port name must match on the substring");
    CHECK(controllers_by_hint("ddj-flx4 midi 1") == f,
          "the port-name match must be case-insensitive too");
    CHECK(controllers_by_hint("DDJ-FLX4:DDJ-FLX4 MIDI 1") == f,
          "the doubled client:port spelling must match as well");
    CHECK(controllers_by_hint("flx") == NULL,
          "the hint is the needle and the port name the haystack, so a port "
          "name shorter than the hint is not a match");

    /* by_usb() is exact and lowercase, because the format is one the caller
     * formats -- "2B73:0045" is the same device in a spelling this table does
     * not use, and quietly accepting it would hide the difference. */
    CHECK(f && controllers_by_usb("2b73:0045") == f,
          "the FLX4's USB id must find its row");
    CHECK(controllers_by_usb("2B73:0045") == NULL,
          "the USB match must be exact and lowercase");
    CHECK(controllers_by_usb(NULL) == NULL, "a NULL USB id matches nothing");
    CHECK(controllers_by_usb("") == NULL, "an empty USB id matches nothing");
    CHECK(controllers_by_usb("0000:0000") == NULL,
          "an unknown USB id must not match the row whose usb is NULL");
}

static void test_default(void)
{
    const struct controller *d = controllers_default();

    CHECK(d != NULL, "the default controller must never be NULL");
    CHECK(d == controllers_find(CONTROLLERS_DEFAULT_ID),
          "the default must be CONTROLLERS_DEFAULT_ID's own row -- a second "
          "default would be the drift this table removes");
    CHECK(d == controllers_at(0),
          "and it is the table's first row, so the fallback in "
          "controllers_default() and the table agree");
    CHECK(strcmp(CONTROLLERS_DEFAULT_ID, "flx4") == 0,
          "the shipped default is the surface this port targets: flx4");
}

/* ---------------------------------------------------------------------------
 * 5. The two SysEx blobs, byte for byte
 */

static void test_sysex(void)
{
    const struct controller *f = controllers_find("flx4");
    const struct controller *j = controllers_find("jp21");

    /* The FLX4's keepalive, as the sibling port records it. Pinned byte for byte
     * because a mistyped payload is a message the surface silently ignores --
     * and because 200 is a PERIOD, so 0 would mean "never" rather than "fast". */
    {
        static const unsigned char want[12] = {
            0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x05,
            0x00, 0x50, 0x02, 0xF7
        };

        CHECK(f && f->keepalive != NULL, "the FLX4 must carry a keepalive");
        if (f && f->keepalive) {
            CHECK(f->keepalive_len == sizeof want,
                  "the FLX4's keepalive is %u bytes, not %u",
                  f->keepalive_len, (unsigned)sizeof want);
            CHECK(f->keepalive_len == sizeof want &&
                  memcmp(f->keepalive, want, sizeof want) == 0,
                  "the FLX4's keepalive payload must be the sibling's twelve "
                  "bytes");
        }
        CHECK(f && f->keepalive_ms == 200,
              "the FLX4's keepalive period must be 200 ms, not %u",
              f ? f->keepalive_ms : 0);
        CHECK(f && f->init == NULL && f->init_len == 0,
              "the FLX4 has no SysEx of its own; a poll is not a greeting");
    }

    /* The JP21's absolute-value query: this port's own bytes, moved here out of
     * midi_io.c so there is one copy. led_query_absolute() reads them from this
     * row, and that is what makes the two unable to drift. */
    {
        static const unsigned char want[10] = {
            0xF0, 0x00, 0x02, 0x0B, 0x7F, 0x12, 0x04, 0x00, 0x00, 0xF7
        };

        CHECK(j && j->init != NULL, "the JP21 must carry its absolute-value query");
        if (j && j->init) {
            CHECK(j->init_len == sizeof want,
                  "the JP21's query is %u bytes, not %u",
                  j->init_len, (unsigned)sizeof want);
            CHECK(j->init_len == sizeof want &&
                  memcmp(j->init, want, sizeof want) == 0,
                  "the JP21's absolute-value query must be the bytes midi_io.c "
                  "used to carry inline");
        }
        CHECK(j && j->keepalive == NULL && j->keepalive_len == 0 &&
              j->keepalive_ms == 0,
              "the JP21 wants no keepalive");
    }

    /* No two surfaces share a blob: a copy-paste that left the FLX4's keepalive
     * on the JP21's row would otherwise read as correct here. */
    CHECK(!f || !j || !f->keepalive || !j->init ||
          f->keepalive_len != j->init_len ||
          memcmp(f->keepalive, j->init, f->keepalive_len) != 0,
          "the two surfaces must not share a SysEx payload");
}

/* ---------------------------------------------------------------------------
 * 6. The keepalive's clock
 */

static void test_keepalive_due(void)
{
    const struct controller *f = controllers_find("flx4");
    const struct controller *j = controllers_find("jp21");
    unsigned long long t = 1000000ULL;      /* an arbitrary "already running" ms */

    /* A surface that wants no keepalive is never due, whichever way it is
     * absent -- so the tick thread needs no arm of its own. */
    CHECK(controller_keepalive_due(NULL, t, 0) == 0,
          "no row, no keepalive");
    CHECK(controller_keepalive_due(j, t, 0) == 0,
          "the JP21 wants no keepalive and must never be due");
    CHECK(controller_keepalive_due(j, 0, 0) == 0,
          "and not even at the clock's origin");

    if (!f)
        return;

    /* The first tick after the shim starts sends: `last` of 0 is the caller's
     * "never sent", and the poll should begin with the shim rather than 200 ms
     * later. */
    CHECK(controller_keepalive_due(f, t, 0) == 1,
          "the first tick must be due -- `last` 0 means never sent");

    /* Both sides of the period, so `>=` cannot be read as `>` and a 200 ms
     * period cannot become a 201 ms one. */
    CHECK(controller_keepalive_due(f, 1000200ULL, 1000000ULL) == 1,
          "exactly one period later must be due -- the bound is inclusive");
    CHECK(controller_keepalive_due(f, 1000199ULL, 1000000ULL) == 0,
          "one millisecond short must NOT be due");
    CHECK(controller_keepalive_due(f, 1000000ULL, 1000000ULL) == 0,
          "the same instant is not due twice");
    CHECK(controller_keepalive_due(f, 1000400ULL, 1000000ULL) == 1,
          "two periods late is still due -- the deadline does not drift");

    /* THE WRAP, and it is not hypothetical: a monotonic millisecond clock that
     * goes backwards has wrapped, and on this target `long` is four bytes -- a
     * nanosecond CLOCK_MONOTONIC in one wraps every 4.29 s, which this tree has
     * already paid for once (audioshim's clock). A wrap must cost ONE early
     * keepalive, never a silence for the next 49.7 days, so ANY backwards step
     * is treated as due and not only a big one. */
    CHECK(controller_keepalive_due(f, 256ULL, 0xFFFFFF00ULL) == 1,
          "a wrap across the 32-bit millisecond boundary must be due");
    CHECK(controller_keepalive_due(f, 100ULL, 0xFFFFFFFFFFFFFF00ULL) == 1,
          "and a wrap across the 64-bit boundary must be due as well");
    CHECK(controller_keepalive_due(f, 999999ULL, 1000000ULL) == 1,
          "even a one-millisecond backwards step is a wrap: a monotonic clock "
          "cannot do that, and sending one early beats going silent");
    CHECK(controller_keepalive_due(f, 0xFFFFFFFFFFFFFFFFULL, 0ULL) == 1,
          "the widest forward count is due -- the comparison must not overflow");
}

int main(void)
{
    test_shape();
    test_the_measured_row();
    test_the_unknowns_stay_unknown();
    test_find_and_hint();
    test_default();
    test_sysex();
    test_keepalive_due();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks,
           failures);
    return failures ? 1 : 0;
}
