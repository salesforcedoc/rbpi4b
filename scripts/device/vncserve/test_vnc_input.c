/*
 * test_vnc_input.c -- the bytes, the mapping, and the hold.
 *
 * Three things are pinned here, and they are the three that would fail silently on
 * the unit rather than loudly:
 *
 *   1. THE BYTES. This program's whole claim is that it writes what work/poke.py
 *      writes. poke.py has been pressing this glass since September and its report
 *      layout is `struct.pack("<iiHHi", 0, 0, type, code, value)` -- sixteen bytes, and
 *      the test says so twice over, once as an explicit little-endian packer and once
 *      as a raw literal, so that neither the implementation's indexing nor this file's
 *      own helper can drift alone.
 *
 *   2. THE MAPPING, AS A PROPERTY RATHER THAN A TABLE. The forward transform is
 *      measured and documented (docs/07-touch.md): `logical = raw * 1280 / 1921`, with
 *      the `+1` that `point_xform_abs` puts in `range = max - min + 1`. The inverse is
 *      therefore pinned by demanding the round trip -- for every logical x and y on the
 *      screen, the raw point this program would aim at must come back as the same
 *      logical point. That is the test the floor fails and the ceiling passes, and it
 *      is the one the docs' measurement ("a fader asked for value 0 landed on 2, and
 *      logical x 1279 came back as 1278") is a symptom of.
 *
 *   3. THE HOLD. A release that arrives sooner than VNC_INPUT_MIN_PRESS_MS is held back
 *      and never extended past it -- the difference between a Mac's quick click
 *      registering as a tap and it being swallowed, and the difference between a real
 *      hold staying a hold and becoming a tap instead.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "vnc_input.h"

static int fails, checks;

static void eq_int(long got, long want, const char *what)
{
    checks++;
    if (got != want) { fails++; printf("  FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("  FAIL %s: got %s, want %s\n", what, got ? got : "(null)", want);
    }
}

/* --- an independent writer for the expected bytes --------------------------------- *
 * Deliberately NOT vnc_input.c's put_ev: separate code, so a mistake in one is not
 * mirrored in the other. */
static void pack16(uint8_t *p, int32_t sec, int32_t usec, uint16_t type, uint16_t code,
                   int32_t value)
{
    p[0] = (uint8_t)sec;         p[1] = (uint8_t)(sec >> 8);
    p[2] = (uint8_t)(sec >> 16); p[3] = (uint8_t)(sec >> 24);
    p[4] = (uint8_t)usec;        p[5] = (uint8_t)(usec >> 8);
    p[6] = (uint8_t)(usec >> 16);p[7] = (uint8_t)(usec >> 24);
    p[8]  = (uint8_t)type;       p[9]  = (uint8_t)(type >> 8);
    p[10] = (uint8_t)code;       p[11] = (uint8_t)(code >> 8);
    p[12] = (uint8_t)value;      p[13] = (uint8_t)(value >> 8);
    p[14] = (uint8_t)(value >> 16); p[15] = (uint8_t)(value >> 24);
}

/* poke.py's `ev()`: ev(type, code, value). Four events per line keeps the callers
 * reading like the sequence they are checking. */
static size_t ev(uint8_t *p, int type, int code, int value)
{
    pack16(p, 0, 0, (uint16_t)type, (uint16_t)code, (int32_t)value);
    return 16;
}

#define EV_ABS  0x03
#define EV_KEY  0x01
#define EV_SYN  0x00
#define MT_SLOT 0x2f
#define MT_X    0x35
#define MT_Y    0x36
#define MT_ID   0x39
#define ABS_X   0x00
#define ABS_Y   0x01
#define BTN_TOUCH 0x14a
#define SYN_REPORT 0

/* The 16-byte literal, for one event, spelled out. Everything below builds on this. */
static void check_the_event_layout(void)
{
    static const uint8_t want[16] = {
        0, 0, 0, 0,   /* tv_sec           */
        0, 0, 0, 0,   /* tv_usec          */
        0x03, 0x00,   /* EV_ABS, 16-bit LE */
        0x2f, 0x00,   /* ABS_MT_SLOT       */
        0, 0, 0, 0    /* the value         */
    };
    uint8_t buf[VNC_INPUT_REPORT_MAX];
    size_t n;

    n = vnc_input_build_syn(buf, sizeof buf, 16);
    eq_int((long)n, 16, "a 16-byte SYN_REPORT is one record");
    checks++;
    if (memcmp(buf, (uint8_t[16]){0}, 16) != 0) {
        fails++; printf("  FAIL a SYN_REPORT carries no type, no code and a zero value\n");
    }

    /* Rebuild the first event of the prime sequence a second way and compare against a
     * hand-written literal -- this is the check that would notice a byte order slip. */
    memset(buf, 0xaa, sizeof buf);
    n = vnc_input_build_prime(buf, sizeof buf, 16, 1884, 541);
    eq_int((long)n, 112, "prime() is seven 16-byte records, as poke.py writes it");
    checks++;
    if (memcmp(buf, want, 16) != 0) {
        fails++; printf("  FAIL prime()'s first record is not ev(3, 0x2f, 0)\n");
    }

    /* And the 24-byte layout, which is what a 64-bit reader gets: the same three tail
     * fields, eight bytes further in. Nothing else about it is used. */
    memset(buf, 0xaa, sizeof buf);
    n = vnc_input_build_syn(buf, sizeof buf, 24);
    eq_int((long)n, 24, "a 24-byte SYN_REPORT is one record");
    {
        int z = 1, i;
        for (i = 0; i < 16; i++)
            if (buf[i] != 0) z = 0;
        eq_int(z, 1, "the 24-byte record's two timestamp fields are zero");
    }
    eq_int(buf[16], 0, "its type is at offset 16");
    eq_int(buf[24], 0xaa, "and byte 24 begins the next record, not this one");
    eq_int((long)vnc_input_build_syn(buf, 8, 24), 0,
           "a buffer too small for the record is refused, not truncated");
}

static void check_prime(void)
{
    uint8_t got[VNC_INPUT_REPORT_MAX], want[VNC_INPUT_REPORT_MAX];
    uint8_t *w = want;
    size_t n, wn = 0;

    n = vnc_input_build_prime(got, sizeof got, 16, 1884, 541);

    wn += ev(w + wn, EV_ABS, MT_SLOT, 0);   wn += ev(w + wn, EV_ABS, MT_ID, -1);
    wn += ev(w + wn, EV_ABS, MT_X, 1884);   wn += ev(w + wn, EV_ABS, MT_Y, 541);
    wn += ev(w + wn, EV_ABS, ABS_X, 1884);  wn += ev(w + wn, EV_ABS, ABS_Y, 541);
    wn += ev(w + wn, EV_SYN, SYN_REPORT, 0);

    eq_int((long)n, (long)wn, "prime()'s length");
    checks++;
    if (memcmp(got, want, wn) != 0) {
        fails++;
        printf("  FAIL prime() is not poke.py's prime(): the sequence or a code "
               "differs\n");
    }
}

static void check_press_and_release(void)
{
    uint8_t got[VNC_INPUT_REPORT_MAX], want[VNC_INPUT_REPORT_MAX];
    uint8_t *w = want;
    size_t n, wn = 0;

    n = vnc_input_build_press(got, sizeof got, 16, 1884, 541);
    wn += ev(w + wn, EV_ABS, MT_SLOT, 0);   wn += ev(w + wn, EV_ABS, MT_ID, 1);
    wn += ev(w + wn, EV_ABS, MT_X, 1884);   wn += ev(w + wn, EV_ABS, MT_Y, 541);
    wn += ev(w + wn, EV_ABS, ABS_X, 1884);  wn += ev(w + wn, EV_ABS, ABS_Y, 541);
    wn += ev(w + wn, EV_KEY, BTN_TOUCH, 1); wn += ev(w + wn, EV_SYN, SYN_REPORT, 0);
    eq_int((long)n, (long)wn, "press()'s length");
    checks++;
    if (memcmp(got, want, wn) != 0) {
        fails++; printf("  FAIL press() is not poke.py's press\n");
    }

    /* The release does NOT repeat the position, and that is deliberate: a release with
     * a position on it is a report pointsrc's relative-mouse path never makes, and the
     * up-edge is all rbp needs. */
    wn = 0;
    n = vnc_input_build_release(got, sizeof got, 16);
    wn += ev(w + wn, EV_ABS, MT_ID, -1);
    wn += ev(w + wn, EV_KEY, BTN_TOUCH, 0);
    wn += ev(w + wn, EV_SYN, SYN_REPORT, 0);
    eq_int((long)n, (long)wn, "release()'s length");
    checks++;
    if (memcmp(got, want, wn) != 0) {
        fails++; printf("  FAIL release() is not poke.py's release\n");
    }

    wn = 0;
    n = vnc_input_build_move(got, sizeof got, 16, 961, 541);
    wn += ev(w + wn, EV_ABS, MT_X, 961); wn += ev(w + wn, EV_ABS, MT_Y, 541);
    wn += ev(w + wn, EV_ABS, ABS_X, 961); wn += ev(w + wn, EV_ABS, ABS_Y, 541);
    wn += ev(w + wn, EV_SYN, SYN_REPORT, 0);
    eq_int((long)n, (long)wn, "move()'s length");
    checks++;
    if (memcmp(got, want, wn) != 0) {
        fails++; printf("  FAIL move() is not a press's position events alone\n");
    }
}

/* --- the mapping ------------------------------------------------------------------ */

/* `point_xform_abs()`, docs/07-touch.md: range = max - min + 1. */
static int forward(int r, int lspan, int rspan)
{
    return r * lspan / rspan;
}

static void check_mapping(void)
{
    static const int taps[3][2] = { { 12, 7 }, { 954, 635 }, { 1874, 1248 } };
    static const int tapsy[3][2] = { { 9, 6 }, { 477, 353 }, { 1071, 792 } };
    int i, l;

    /* The operator's own three taps of 2026-09-27, from the table in docs/07-touch.md.
     * First the table itself is checked against this file's model of the forward
     * transform -- if the two disagree, the model is wrong and everything below it is
     * measuring the wrong thing. Then the inverse: aiming at the logical point a tap
     * emitted must give a raw point that reads back as that same logical point, which
     * is the only thing that matters, since the forward map is not injective. */
    for (i = 0; i < 3; i++) {
        char what[64];
        int rx, ry;
        snprintf(what, sizeof what, "the docs' tap %d, raw x %d -> logical x",
                 i, taps[i][0]);
        eq_int(forward(taps[i][0], 1280, 1921), taps[i][1], what);
        snprintf(what, sizeof what, "the docs' tap %d, raw y %d -> logical y",
                 i, tapsy[i][0]);
        eq_int(forward(tapsy[i][0], 800, 1081), tapsy[i][1], what);

        rx = vnc_input_raw_of_logical(taps[i][1], 1280, 1921);
        ry = vnc_input_raw_of_logical(tapsy[i][1], 800, 1081);
        snprintf(what, sizeof what, "measured tap %d, x %d -> raw -> x", i, taps[i][1]);
        eq_int(forward(rx, 1280, 1921), taps[i][1], what);
        snprintf(what, sizeof what, "measured tap %d, y %d -> raw -> y", i, tapsy[i][1]);
        eq_int(forward(ry, 800, 1081), tapsy[i][1], what);
    }

    /* THE PROPERTY. Every logical point on rbp's screen must survive the round trip.
     * The floor fails this at x 1279 alone; the ceiling passes it everywhere. */
    {
        int bad = 0;
        for (l = 0; l < 1280; l++)
            if (forward(vnc_input_raw_of_logical(l, 1280, 1921), 1280, 1921) != l)
                bad++;
        eq_int(bad, 0, "every logical x survives the round trip");
        bad = 0;
        for (l = 0; l < 800; l++)
            if (forward(vnc_input_raw_of_logical(l, 800, 1081), 800, 1081) != l)
                bad++;
        eq_int(bad, 0, "every logical y survives the round trip");
    }

    /* The two edges, in raw units, because these are the numbers a drill reads. */
    eq_int(vnc_input_raw_of_logical(0, 1280, 1921), 0, "logical x 0 is raw x 0");
    eq_int(vnc_input_raw_of_logical(1279, 1280, 1921), 1920,
           "logical x 1279 is raw 1920, not 1919 -- the last column is reachable");
    eq_int(vnc_input_raw_of_logical(799, 800, 1081), 1080, "logical y 799 is raw 1080");
    eq_int(vnc_input_raw_of_logical(1255, 1280, 1921), 1884,
           "the seventh menu column's centre is raw x 1884 -- USB STOP");

    /* Off the edge is clamped, not wrapped: a client that sends x 5000 because it has
     * a stale width must not land the pointer somewhere on the other side. */
    eq_int(vnc_input_raw_of_logical(-5, 1280, 1921), 0, "a negative x clamps to the left");
    eq_int(vnc_input_raw_of_logical(99999, 1280, 1921), 1920, "a huge x clamps to the right");
    eq_int(vnc_input_raw_of_logical(99999, 800, 1081), 1080, "and so does y");

    /* THE MAP ITSELF IS NOT REFLECTED. If anybody ever "fixes" this by putting
     * tscfake's 1279 - x here, the left edge would come back as the right one -- and
     * this is the check that says which of the two the picture's left edge is. */
    {
        int rx = -1, ry = -1;
        vnc_input_map(0, 0, 1280, 800, 1921, 1081, &rx, &ry);
        eq_int(rx, 0, "the picture's top-left corner is the panel's top-left corner");
        eq_int(ry, 0, "in x and in y");
        vnc_input_map(1279, 799, 1280, 800, 1921, 1081, &rx, &ry);
        eq_int(rx, 1920, "the picture's bottom-right corner is the panel's bottom-right");
        eq_int(ry, 1080, "in x and in y");
    }
}

/* --- the switch's text ------------------------------------------------------------ */

static void accept(const char *text, int want, const char *what)
{
    int got = -1;
    eq_int(vnc_input_parse(text, &got), 0, what);
    eq_int(got, want, what);
}

static void reject(const char *text, const char *what)
{
    int got = -1;
    checks++;
    if (vnc_input_parse(text, &got) == 0) {
        fails++;
        printf("  FAIL %s: \"%s\" was accepted as %s\n", what, text, vnc_input_name(got));
    }
}

static void check_switch_text(void)
{
    eq_str(vnc_input_name(VNC_INPUT_ON), "on", "on's name");
    eq_str(vnc_input_name(VNC_INPUT_OFF), "off", "off's name");
    eq_str(vnc_input_name(999), "off",
           "anything not exactly on reads as off -- the safe direction for this one");

    accept("on", VNC_INPUT_ON, "on");
    accept("off", VNC_INPUT_OFF, "off");
    accept("on\n", VNC_INPUT_ON, "on with the newline the writer adds");
    accept("1", VNC_INPUT_ON, "1");
    accept("0", VNC_INPUT_OFF, "0");
    accept("YES", VNC_INPUT_ON, "yes, in capitals");
    accept("  true  ", VNC_INPUT_ON, "true, surrounded by blanks");
    accept("\tFALSE\t\n", VNC_INPUT_OFF, "false, with tabs and a newline");
    accept("Off\r\n", VNC_INPUT_OFF, "CRLF, as a file edited over SMB arrives");

    /* A control file that lets a machine press a live player's buttons is the last
     * place to be generous about what a word means. */
    reject("", "an empty file");
    reject("\n", "a newline and nothing else");
    reject("toggle", "the word the page uses for its button");
    reject("onn", "a longer word starting with on");
    reject("no way", "two words");
    reject("enable", "a synonym nobody agreed on");
    reject("2", "a number that is neither");

    {
        struct vnc_input_switch s;
        char out[64];
        vnc_input_switch_init(&s, "/nonexistent/vnc.input", VNC_INPUT_OFF);
        eq_int(s.on, VNC_INPUT_OFF, "input starts off when nothing says otherwise");
        eq_int(vnc_input_switch_get(&s), VNC_INPUT_OFF,
               "and a missing file leaves it there");
        eq_int(vnc_input_switch_note(&s, out, sizeof out), 0, "an empty note reports nothing");
        snprintf(s.note, sizeof s.note, "turned on");
        eq_int(vnc_input_switch_note(&s, out, sizeof out), 1, "a note is handed over");
        eq_str(out, "turned on", "the note's text arrives intact");
        eq_int(vnc_input_switch_note(&s, out, sizeof out), 0, "and having been read, it is gone");
    }
}

/* --- the hold, driven through a pipe ---------------------------------------------- */

static size_t drain(int fd, uint8_t *out, size_t cap)
{
    size_t n = 0;
    ssize_t r;

    for (;;) {
        if (n == cap) break;
        r = read(fd, out + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    return n;
}

static void check_the_hold(void)
{
    int p[2];
    struct vnc_input in;
    uint8_t got[VNC_INPUT_REPORT_MAX * 8];
    size_t n;

    if (pipe(p) != 0) { fails++; printf("  FAIL pipe()\n"); return; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);

    vnc_input_init(&in, 1280, 800, 1921, 1081);
    vnc_input_set_fd(&in, p[1], 16);
    eq_int(in.evsize, 16, "the test drives the 16-byte layout");
    eq_int(in.raw_w, 1921, "and the panel's own raw width");

    /* A move with no button held writes nothing at all: motion is published only while
     * pressed, which is pointsrc's own rule for a mouse. */
    vnc_input_pointer(&in, 0, 640, 400, 1000);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "a hover writes nothing");
    eq_int(vnc_input_is_down(&in), 0, "and leaves nothing down");

    /* A right-click writes nothing either. The panel has one contact and no buttons. */
    vnc_input_pointer(&in, 2, 640, 400, 1000);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "a right-click writes nothing");
    eq_int(in.presses, 0, "and is not counted as a press");

    /* A press writes a prime and a press: 7 + 8 records. */
    vnc_input_pointer(&in, 1, 640, 400, 1000);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 15 * 16, "a press is a prime and a press");
    eq_int(vnc_input_is_down(&in), 1, "and it is down");
    eq_int(in.presses, 1, "counted once");

    /* A drag within the press: one move, and only when the raw point actually moves. */
    vnc_input_pointer(&in, 1, 640, 400, 1010);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "a press with no movement writes nothing");
    vnc_input_pointer(&in, 1, 700, 400, 1020);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 5 * 16, "a drag writes the position events and a SYN, and no more");
    eq_int(in.moves, 1, "counted once");

    /* THE HOLD: let go at 1050, 50 ms into a 150 ms floor. Nothing goes out yet. */
    vnc_input_pointer(&in, 0, 700, 400, 1050);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "a release under the floor is held back, not sent");
    eq_int(vnc_input_is_down(&in), 1, "so the finger is still down");
    eq_int(in.releases, 0, "and nothing has been released");

    /* ...and the tick sends it the moment the press has lasted long enough. */
    vnc_input_tick(&in, 1100);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "and still not a millisecond early");
    vnc_input_tick(&in, 1150);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 3 * 16, "at the floor the release goes out");
    eq_int(vnc_input_is_down(&in), 0, "the finger is up");
    eq_int(in.releases, 1, "counted once");
    vnc_input_tick(&in, 1200);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "and a tick with nothing pending writes nothing");

    /* A LONG PRESS IS NOT EXTENDED. A press the client held for 400 ms already outlasts
     * the floor, so it goes out at once -- and stays a 400 ms hold, which the menu reads
     * as a hold rather than a tap. */
    vnc_input_pointer(&in, 1, 200, 200, 2000);
    drain(p[0], got, sizeof got);
    vnc_input_pointer(&in, 0, 200, 200, 2400);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 3 * 16, "a press past the floor is released at once");
    eq_int(vnc_input_is_down(&in), 0, "and is up");

    /* A SECOND CLICK INSIDE THE FLOOR. The client pressed again while the first
     * release was still held back, at a different place. It must become two presses
     * and not one drag -- merged, rbp would see a single long press whose finger
     * wandered, which on the menu means the second button never fires and the first
     * fires twice -- and it must not become a second press until the first has lasted
     * long enough to be a tap. So it waits, and vnc_input_tick() finishes the first
     * press before starting the second. */
    vnc_input_pointer(&in, 1, 300, 300, 3000);
    drain(p[0], got, sizeof got);
    vnc_input_pointer(&in, 0, 300, 300, 3010);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "the first release is held back");
    vnc_input_pointer(&in, 1, 400, 400, 3020);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "and the second press writes nothing either");
    eq_int(in.presses, 3, "it is not a press yet");
    vnc_input_tick(&in, 3100);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "still inside the floor");
    vnc_input_tick(&in, 3160);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, (3 + 15) * 16, "at the floor: the release, then the queued press");
    eq_int(vnc_input_is_down(&in), 1, "the second press is down");
    eq_int(in.presses, 4, "and now it counts");

    /* THE THREE WAYS A FINGER MUST NEVER OUTLIVE ITS PRESS: the client going away, the
     * switch being turned off, and shutdown. All three come through here. */
    vnc_input_release(&in);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 3 * 16, "an explicit release writes the up-edge");
    eq_int(vnc_input_is_down(&in), 0, "and puts the finger up");
    vnc_input_release(&in);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "releasing a finger that is already up writes nothing");

    /* A pending release is not a down that has been forgotten: releasing during the
     * hold-back still puts the finger up, immediately. */
    vnc_input_pointer(&in, 1, 500, 500, 4000);
    drain(p[0], got, sizeof got);
    vnc_input_pointer(&in, 0, 500, 500, 4010);
    drain(p[0], got, sizeof got);
    vnc_input_release(&in);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 3 * 16, "a held-back release can still be cut short by release()");
    eq_int(vnc_input_is_down(&in), 0, "and the finger is up");

    /* ...and a queued press is dropped rather than fired when the client goes away
     * during the hold-back. */
    vnc_input_pointer(&in, 1, 600, 600, 5000);
    drain(p[0], got, sizeof got);
    vnc_input_pointer(&in, 0, 600, 600, 5010);
    drain(p[0], got, sizeof got);
    vnc_input_pointer(&in, 1, 700, 700, 5020);
    drain(p[0], got, sizeof got);
    vnc_input_release(&in);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 3 * 16, "a queued press is dropped when the client goes away");
    vnc_input_tick(&in, 6000);
    n = drain(p[0], got, sizeof got);
    eq_int((long)n, 0, "and it does not fire afterwards");

    /* With no panel there is nothing to write and nothing to crash. */
    vnc_input_close(&in);
    eq_int(in.fd, -1, "closing drops the fd");
    vnc_input_pointer(&in, 1, 100, 100, 9000);
    vnc_input_tick(&in, 9000);
    vnc_input_release(&in);
    eq_int(in.presses, 6, "and a press with no panel is counted nowhere");

    close(p[0]);
    close(p[1]);
}

int main(void)
{
    printf("test_vnc_input\n");

    check_the_event_layout();
    check_prime();
    check_press_and_release();
    check_mapping();
    check_switch_text();
    check_the_hold();

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
