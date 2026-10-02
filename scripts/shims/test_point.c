/*
 * test_point.c — the pointer path's unit test: no Pi, no device, no rbp.
 *
 * Three things are pinned here, and they are pinned for different reasons.
 *
 * 1. The record stream rbp reads. Everything about the fake tsc2007 is
 *    unverifiable without the player, so the parts that *are* verifiable — the
 *    byte layout, the duplicate suppression, the two-frame burst on a press, and
 *    the x reflection — are asserted against a real pipe through the real
 *    tscfake_emit(). A future change to the emit rules has to break this test out
 *    loud instead of silently making the first tap of every gesture invisible, or
 *    moving every tap to the opposite side of the screen.
 *
 * 2. The coordinate algebra. The repo's own two touch implementations disagreed
 *    about it, so this asserts the *old* SC Live 4 result is reproducible from
 *    the new flags (swap=1, range 0..2047, no inversions) — that is the check
 *    that the generalization did not change the algebra it generalizes.
 *
 * 3. The deck QUANTIZE boxes. Their rectangles are a measurement off a captured
 *    frame and their offset from deck 1 to deck 2 is +640 px, not a reflection —
 *    two facts that a finger would take weeks to falsify and a test falsifies
 *    here. test_point does not link pointsrc.c, so what is pinned is the
 *    decision; the one line that sends the keycode lives in that file and is
 *    rbp's side of the boundary.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_point test_point.c tscfake.c
 *            point_xform.c touch_zone.c   &&   qemu-arm ./test_point
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "point_xform.h"
#include "touch_zone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

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

/* tscfake_open() starts the pointer source; this test drives tscfake_emit()
 * directly, so the source is a no-op. It is the only reason this file mentions
 * pointsrc at all: the test is about what rbp receives, not where it came from. */
int pointsrc_start(void) { return 0; }

/* And the same for the visible pointer, which tscfake_open() also starts. It is
 * stubbed rather than linked for the same reason as the line above: this test is
 * about the bytes rbp reads, and the compositor would drag a framebuffer, a
 * thread and pthread into a fixture that is deliberately none of those things.
 * Its own rule is pinned in test_cursor.c. */
int fb_cursor_start(void) { return 0; }

/* And the same again for the top menu's panel, which tscfake_open() now starts
 * alongside the arrow. The gesture and the swallow that go with it ARE pinned
 * here, but through menu_zone.c, which this test links as production code -- the
 * compositor itself is another thread and another framebuffer and belongs to
 * test_menu.c's paint cases, not to a fixture about the bytes rbp reads. */
int menu_draw_start(void) { return 0; }

/* --- 1. the record layout --------------------------------------------------- */

static void test_record_layout(void)
{
    unsigned char b[TSC_RECORD_LEN];

    tscfake_record(1, 0x1234, 0x5678, b);
    CHECK(b[0] == 1 && b[1] == 0, "press flag/pad: got %02x %02x", b[0], b[1]);
    CHECK(b[2] == 0x34 && b[3] == 0x12, "x not little-endian: %02x %02x", b[2], b[3]);
    CHECK(b[4] == 0x78 && b[5] == 0x56, "y not little-endian: %02x %02x", b[4], b[5]);

    tscfake_record(0, 0, 0, b);
    CHECK(b[0] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0,
          "released zero record not zeroed");

    /* The logical coordinates rbp is sent routinely exceed TSC_MAX_X (3): the
     * ioctl-reported maximum is rbp's calibration scale, not the record's. If
     * anyone ever "fixes" that mismatch, this is the assertion that stops them. */
    tscfake_record(1, 1279, 799, b);
    CHECK(b[2] == 0xff && b[3] == 0x04, "x=1279 encoded as %02x %02x", b[2], b[3]);
    CHECK(b[4] == 0x1f && b[5] == 0x03, "y=799 encoded as %02x %02x", b[4], b[5]);
}

/* --- 2. the ioctl contract ------------------------------------------------- */

static void test_tsc_ioctls(void)
{
    unsigned int mx = 0;
    unsigned short my = 0;

    /* These literals are rbp's side of the contract: it was compiled against a
     * tsc2007 driver on the RX3 and issues exactly these requests. They are
     * written out here rather than compared against TSC_MAX_X so that editing
     * the header fails the test instead of moving the goalposts with it. */
    CHECK(TSC_MAX_X == 3, "TSC_MAX_X is rbp ABI; the original shim reported 3");
    CHECK(TSC_MAX_Y == 3900, "TSC_MAX_Y is rbp ABI; the original shim reported 3900");
    CHECK(strcmp(TSC_DEVICE_PATH, "/dev/tsc2007_2-0048") == 0,
          "the device path rbp opens is ABI, not a preference");

    CHECK(tscfake_ioctl(0x80046b00, &mx) == 1, "_IOR(0x6b,0,4) not claimed");
    CHECK(mx == 3, "max X ioctl answered %u, expected 3", mx);
    CHECK(tscfake_ioctl(0x40046b00, &mx) == 1, "_IOW(0x6b,0,4) not claimed");
    CHECK(tscfake_ioctl(0x80026b01, &my) == 1, "_IOR(0x6b,1,2) not claimed");
    CHECK(my == 3900, "max Y ioctl answered %u, expected 3900", my);
    CHECK(tscfake_ioctl(0x40026b01, &my) == 1, "_IOW(0x6b,1,2) not claimed");

    /* Anything else in the 0x6b family must fall through to the kernel, and a
     * NULL argument must be tolerated rather than dereferenced. */
    CHECK(tscfake_ioctl(0x80046b02, &mx) == 0, "an unknown 0x6b request was claimed");
    CHECK(tscfake_ioctl(0x80046b00, NULL) == 1, "a NULL argument crashed the max-X ioctl");
    CHECK(tscfake_ioctl(0x80026b01, NULL) == 1, "a NULL argument crashed the max-Y ioctl");
}

/* --- 3. what tscfake_emit() puts on the wire -------------------------------- */

static int read_exact(int fd, unsigned char *buf, int want)
{
    int got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, (size_t)(want - got));
        if (n <= 0)
            break;
        got += (int)n;
    }
    return got;
}

/* The wire, opened non-blocking -- every time, including for the sections that do
 * not care. read_exact() asking for more bytes than are there would otherwise BLOCK
 * on an empty pipe rather than return short, so an arithmetic mistake in a test
 * would hang the whole suite instead of failing it; with O_NONBLOCK a short read is
 * a failed CHECK, which is the whole difference between a test and a trap. Nothing
 * here relies on the blocking behaviour: every write tscfake_emit() makes is
 * synchronous, so the bytes a test expects are already buffered when it reads. */
static int open_wire(void)
{
    int fd = tscfake_open();
    int fl;

    if (fd >= 0 && (fl = fcntl(fd, F_GETFL, 0)) >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

/* The process's FIRST emit, which is a state that exists exactly once per process
 * and is the one the operator meets after every start.
 *
 * The behaviour under test is a `static` inside tscfake_emit(), so the only
 * honest way to get a process that has never published a pointer state is to be
 * a different process: fork() here in a child that inherits the pristine statics,
 * so every emit it makes lands in its own copy and the tests below still see a
 * fresh one. `primed` was added with the fix, and the two children pin both
 * halves of it:
 *
 *   A: the first emit is a DOWN. rbp's touch stream starts released, so this is a
 *      real up->down transition and must be the two-frame burst. It was one frame
 *      while the statics opened with `last_down = -1` (truthy, so `down &&
 *      !last_down` was false), and one frame is invisible: rbp's
 *      TouchAdValueHysteresis discards the first frame after a gap, so the first
 *      touch after rbp starts did nothing. Measured on the unit -- work/menu17.sh's
 *      press #1, the only one of seven that moved nothing.
 *   B: the first emit is a RELEASE at exactly (0,0), the position the statics are
 *      initialised to. It must still be published: before the first emit there is
 *      nothing to dedup against, and a `primed` that let the initial (0,0,0) match
 *      it would swallow a real record. */
static void test_first_emit(void)
{
    int st, which;

    for (which = 0; which < 2; which++) {
        pid_t pid = fork();

        CHECK(pid >= 0, "fork failed");
        if (pid == 0) {
            unsigned char got[2 * TSC_RECORD_LEN];
            int fd = open_wire();
            int want;

            if (fd < 0)
                _exit(2);
            if (which == 0) {
                tscfake_emit(1, 100, 200);          /* A: first emit is a press */
                want = 2 * TSC_RECORD_LEN;
            } else {
                tscfake_emit(0, 0, 0);              /* B: a release at the origin */
                want = TSC_RECORD_LEN;
            }
            /* Reading `want` and then one more asks the pipe whether anything
             * followed, so a single-frame press cannot pass by having the read
             * land short of a burst it never sent. */
            _exit(read_exact(fd, got, want) == want
                  && read_exact(fd, got, TSC_RECORD_LEN) == 0 ? 0 : 1);
        }
        st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "%s: the first emit of a fresh process was wrong (child exit %d)",
              which == 0 ? "a press must be a burst of 2"
                         : "a release at 0,0 must be published",
              WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
}

static void test_emit_stream(void)
{
    /* Four records are expected out of these five calls; the two no-ops
     * contribute nothing, which is the assertion that matters most here — a
     * change that makes the dedup stop working shows up as extra bytes. */
    static const int seq[][3] = {
        { 0, 100, 200 },   /* first report, released -> 1 record */
        { 0, 100, 200 },   /* unchanged -> nothing */
        { 1, 100, 200 },   /* press -> a BURST of 2 */
        { 1, 100, 200 },   /* unchanged while held -> nothing */
        { 1, 300, 200 },   /* moved while held -> 1 record */
    };
    unsigned char expect[4 * TSC_RECORD_LEN];
    unsigned char got[4 * TSC_RECORD_LEN];
    int fd, n, i;

    fd = open_wire();
    CHECK(fd >= 0, "tscfake_open failed");
    if (fd < 0)
        return;
    CHECK(tscfake_is_fd(fd) == 1, "tscfake_is_fd says no for a just-opened fd");

    for (i = 0; i < 5; i++)
        tscfake_emit(seq[i][0], seq[i][1], seq[i][2]);

    /* The x the seq asks for is 100 and 300; the x on the wire is their
     * reflection, 1179 and 979, because rbp acts at `1279 - x`.  Written as
     * literals rather than recomputed here, so that a change to the reflection
     * fails this assertion instead of moving it along with the code -- the same
     * discipline as the TSC_MAX_X literals in section 2.  The law and its
     * measurement are pinned point by point in test_emit_mirrors_x() below. */
    tscfake_record(seq[0][0], 1179, seq[0][2], expect);
    tscfake_record(1, 1179, 200, expect + TSC_RECORD_LEN);          /* burst, frame 1 */
    tscfake_record(1, 1179, 200, expect + 2 * TSC_RECORD_LEN);      /* burst, frame 2 */
    tscfake_record(seq[4][0], 979, seq[4][2], expect + 3 * TSC_RECORD_LEN);

    n = read_exact(fd, got, (int)sizeof expect);
    CHECK(n == (int)sizeof expect, "expected %d bytes on the pipe, got %d",
          (int)sizeof expect, n);
    if (n == (int)sizeof expect)
        CHECK(memcmp(got, expect, sizeof expect) == 0, "record stream differs");

    /* A release is one record, not a burst: the burst is what gets past rbp's
     * debounce, and the debounce only discards frames after a gap on the way
     * down. */
    tscfake_emit(0, 300, 200);
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "release produced %d bytes, expected %d",
          n, TSC_RECORD_LEN);
    if (n == TSC_RECORD_LEN) {
        unsigned char rel[TSC_RECORD_LEN];
        tscfake_record(0, 979, 200, rel);
        CHECK(memcmp(got, rel, TSC_RECORD_LEN) == 0, "release record differs");
    }

    CHECK(tscfake_close(fd) == 0, "tscfake_close failed");
    CHECK(tscfake_is_fd(fd) == 0, "tscfake_is_fd still true after close");
}

/* --- 3b. the x reflection, which is the one measured quirk ------------------- */

/* rbp acts at `POINT_LOGICAL_W - 1 - x` of the record it reads.  The evidence is
 * the framebuffer, not the code: records written into the pipe rbp holds both
 * ways (work/tap.py) made it select a sidebar cell drawn at x 8..50 for x=1229,
 * load the deck-2 button drawn at 1152..1272 for x=90, and hit INFO drawn at
 * 1180..1275 for x=42, while x=50 fell through the sidebar column (0..100) into
 * the list and x=200 fell outside the deck-2 button.  Slope -1, intercept 1279.
 *
 * And the panel side is honest in the same session: with POINT_DEBUG=1 the
 * operator's finger at raw (1874,1071) emitted logical (1248,792), the
 * bottom-right corner -- docs/07's S3.2 table, whose "no mirroring" conclusion is
 * about point_xform_abs() and is still true.  So the reflection is rbp's, it is
 * undone in tscfake_emit(), and the two literals below are what stops anyone
 * reading S3.2 alone from "fixing" it back.
 *
 * Each point is emitted as a release followed by a press, so both directions of
 * the wire are covered (a release is one record, a press is the burst of two),
 * and each gets its own y so the dedup cannot hide a record from the count. */
static void test_emit_mirrors_x(void)
{
    /* x handed to tscfake_emit(), x that must appear on the wire. */
    static const int pair[][2] = {
        {    0, 1279 },   /* the UI's far left is the wire's highest x */
        { 1279,    0 },
        {   50, 1229 },   /* a sidebar cell is DRAWN here ... */
        { 1229,   50 },   /* ... and was REACHED at only from here */
        {   90, 1189 },   /* the deck-2 LOAD button, drawn 1152..1272 */
    };
    unsigned char got[3 * TSC_RECORD_LEN];
    unsigned char exp[TSC_RECORD_LEN];
    size_t i;
    int fd;

    fd = open_wire();
    CHECK(fd >= 0, "tscfake_open failed for the reflection test");
    if (fd < 0)
        return;

    for (i = 0; i < sizeof pair / sizeof pair[0]; i++) {
        int in = pair[i][0], wire = pair[i][1], y = 100 + (int)i;

        tscfake_emit(0, in, y);                        /* release: 1 record */
        tscfake_emit(1, in, y);                        /* press: a burst of 2 */
        CHECK(read_exact(fd, got, (int)sizeof got) == (int)sizeof got,
              "x=%d: expected %d bytes on the wire", in, (int)sizeof got);

        tscfake_record(0, wire, y, exp);
        CHECK(memcmp(got, exp, TSC_RECORD_LEN) == 0,
              "x=%d: the release carries %d, expected %d", in,
              got[2] | (got[3] << 8), wire);
        tscfake_record(1, wire, y, exp);
        CHECK(memcmp(got + TSC_RECORD_LEN, exp, TSC_RECORD_LEN) == 0,
              "x=%d: burst frame 1 is not the reflection", in);
        CHECK(memcmp(got + 2 * TSC_RECORD_LEN, exp, TSC_RECORD_LEN) == 0,
              "x=%d: burst frame 2 is not the reflection", in);
    }

    CHECK(tscfake_close(fd) == 0, "tscfake_close failed after the reflection test");
}

/* --- 3c. the replay of a swallowed strip tap -------------------------------- */

/* pointsrc.c hands rbp a whole press for a tap the menu took and had no use for
 * (menu_zone.h's MZ_FEED_TAP): the down edge, a dwell, then the up, all at the
 * point the finger landed. The sequence itself lives in pointsrc.c, which owns a
 * thread and a real evdev read and is not host-testable -- but the property it needs
 * from tscfake is, and this is where that is pinned.
 *
 * The one that matters is that the pair LEAVES THE WIRE RELEASED. rbp's touch thread
 * is stranded in the down state by a stream that ends pressed, and the replay is the
 * only place in this shim that emits a press it never received -- so if the up were
 * deduped away the operator's next touch would arrive as a drag, for good. And the
 * up is a real hazard rather than a hypothetical one: tscfake_emit() drops any record
 * identical to the last one it sent, and an up carries exactly the down's x and y.
 *
 * The fd comes from open_wire(), so a missing record fails a CHECK rather than
 * blocking the read for ever. */
static void test_tap_replay_stream(void)
{
    unsigned char got[4 * TSC_RECORD_LEN];
    unsigned char exp[TSC_RECORD_LEN];
    int fd, n;

    fd = open_wire();
    CHECK(fd >= 0, "tscfake_open failed for the replay test");
    if (fd < 0)
        return;

    /* THE REPLAY'S OWN PRECONDITION, and it is not housekeeping: tscfake's dedup
     * state is process-wide and the previous test leaves the wire PRESSED (every
     * point in test_emit_mirrors_x ends on a press), while the burst rbp's debounce
     * needs is emitted only on an up->down transition. So a replay onto a wire that
     * is already down would be a single-frame press -- exactly the frame rbp
     * discards. Production cannot reach that: the menu swallows whole presses and
     * the panel is single-touch, so the report before any replay is a release. The
     * release below is that precondition, made explicit rather than inherited. */
    tscfake_emit(0, 999, 999);
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "releasing the wire produced %d bytes, not 1 record", n);

    /* The replay, exactly as menu_replay_tap() emits it: two calls at one point,
     * the dwell between them touching nothing. */
    tscfake_emit(1, 1211, 25);
    tscfake_emit(0, 1211, 25);

    /* Three records: a burst of two for the down (rbp's hysteresis discards the
     * first frame after a gap, which is why the burst exists) and one for the up.
     * 68 on the wire is 1279 - 1211 -- the reflection -- and (1211,25) is rbp's own
     * INFO button, the control the operator reported as dead under the strip. */
    tscfake_record(1, 68, 25, exp);
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "the replay's down frame produced %d bytes", n);
    if (n == TSC_RECORD_LEN)
        CHECK(memcmp(got, exp, TSC_RECORD_LEN) == 0, "the replay's down frame differs");
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "the replay's down was not a burst: %d bytes for frame 2", n);
    if (n == TSC_RECORD_LEN)
        CHECK(memcmp(got, exp, TSC_RECORD_LEN) == 0, "the replay's second frame differs");

    tscfake_record(0, 68, 25, exp);
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "the replay produced no release: %d bytes", n);
    if (n == TSC_RECORD_LEN)
        CHECK(memcmp(got, exp, TSC_RECORD_LEN) == 0, "the replay's release differs");
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == 0, "the replay emitted %d bytes more than a press and a release", n);

    /* THE ASSERTION THIS TEST EXISTS FOR: a press elsewhere, afterwards, must still
     * be a burst of two -- which it can only be if the replay left tscfake's
     * last_down at 0. */
    tscfake_emit(1, 400, 300);
    n = read_exact(fd, got, 2 * TSC_RECORD_LEN);
    CHECK(n == 2 * TSC_RECORD_LEN,
          "a press after the replay produced %d bytes, not a burst of 2 records", n);
    tscfake_emit(0, 400, 300);
    n = read_exact(fd, got, TSC_RECORD_LEN);
    CHECK(n == TSC_RECORD_LEN, "the press after the replay did not release");

    /* And two taps of the same button in a row are two presses: the second must not
     * dedup against the first. INFO tapped twice is INFO twice. */
    tscfake_emit(1, 1211, 25);
    tscfake_emit(0, 1211, 25);
    n = read_exact(fd, got, 3 * TSC_RECORD_LEN);
    CHECK(n == 3 * TSC_RECORD_LEN,
          "a second tap at the same point produced %d bytes, not a fresh press", n);

    CHECK(tscfake_close(fd) == 0, "tscfake_close failed after the replay test");
}

/* --- 4. the coordinate algebra --------------------------------------------- */

static void set_flag(const char *name, const char *value)
{
    /* "" is what start-rb.sh exports for an unset variable, and it must mean
     * "unset" rather than "present". Testing the empty string is the point. */
    if (value == NULL)
        unsetenv(name);
    else
        setenv(name, value, 1);
}

static void test_abs_sc_live4(void)
{
    struct point_xform x;
    int lx, ly;

    /* The SC Live 4's configuration: portrait panel, 11-bit controller, no
     * inversions. swap_xy is what carries logical_x from the device's Y axis. */
    set_flag("POINT_SWAP_XY", "1");
    set_flag("POINT_INVERT_X", NULL);
    set_flag("POINT_INVERT_Y", NULL);
    point_xform_init(&x);
    x.raw_min_x = 0; x.raw_max_x = 2047;
    x.raw_min_y = 0; x.raw_max_y = 2047;

    CHECK(x.swap_xy == 1, "POINT_SWAP_XY=1 did not take effect");
    CHECK(x.invert_x == 0 && x.invert_y == 0, "inversions defaulted on");

    point_xform_abs(&x, 0, 0, &lx, &ly);
    CHECK(lx == 0 && ly == 0, "raw (0,0) -> (%d,%d), old code gives (0,0)", lx, ly);

    point_xform_abs(&x, 0, 2047, &lx, &ly);
    CHECK(lx == 1279 && ly == 0, "raw (0,2047) -> (%d,%d), old code gives (1279,0)", lx, ly);

    point_xform_abs(&x, 2047, 0, &lx, &ly);
    CHECK(lx == 0 && ly == 799, "raw (2047,0) -> (%d,%d), old code gives (0,799)", lx, ly);

    point_xform_abs(&x, 2047, 2047, &lx, &ly);
    CHECK(lx == 1279 && ly == 799,
          "raw (2047,2047) -> (%d,%d), old code gives (1279,799)", lx, ly);

    /* Out of range is clamped, not wrapped: a noisy panel must not be able to
     * hand rbp a coordinate outside its own UI. */
    point_xform_abs(&x, 99999, -99999, &lx, &ly);
    CHECK(lx >= 0 && lx <= 1279 && ly >= 0 && ly <= 799,
          "out-of-range clamped to (%d,%d)", lx, ly);
}

static void test_abs_monitor(void)
{
    struct point_xform x;
    int lx, ly;

    /* A landscape touchscreen as it would be on the Pi: no swap, and a device
     * range that is not the SC Live 4's. */
    set_flag("POINT_SWAP_XY", "0");
    set_flag("POINT_INVERT_X", "1");
    set_flag("POINT_INVERT_Y", "1");
    point_xform_init(&x);
    x.raw_min_x = 0; x.raw_max_x = 4095;
    x.raw_min_y = 0; x.raw_max_y = 4095;

    point_xform_abs(&x, 0, 0, &lx, &ly);
    CHECK(lx == 1279 && ly == 799, "inverted origin -> (%d,%d), expected (1279,799)", lx, ly);

    point_xform_abs(&x, 4095, 4095, &lx, &ly);
    CHECK(lx == 0 && ly == 0, "inverted far corner -> (%d,%d), expected (0,0)", lx, ly);

    point_xform_abs(&x, 2048, 2048, &lx, &ly);
    CHECK(lx == 639 && ly == 399, "inverted centre -> (%d,%d), expected (639,399)", lx, ly);

    /* A range that does not start at zero (a signed digitizer range). The
     * normalization is what makes the whole dimension reachable. */
    x.raw_min_x = -2048; x.raw_max_x = 2047;
    x.invert_x = 0; x.invert_y = 0;
    x.raw_min_y = -2048; x.raw_max_y = 2047;
    point_xform_abs(&x, -2048, -2048, &lx, &ly);
    CHECK(lx == 0 && ly == 0, "signed-range min -> (%d,%d), expected (0,0)", lx, ly);
    point_xform_abs(&x, 2047, 2047, &lx, &ly);
    CHECK(lx == 1279 && ly == 799, "signed-range max -> (%d,%d), expected (1279,799)", lx, ly);
}

static void test_abs_degenerate(void)
{
    struct point_xform x;
    int lx, ly;

    set_flag("POINT_SWAP_XY", "0");
    set_flag("POINT_INVERT_X", NULL);
    set_flag("POINT_INVERT_Y", NULL);
    point_xform_init(&x);
    /* What a kernel reports for an axis it has no calibration for. Dividing by
     * this range must not fault and must still land inside the logical space. */
    x.raw_min_x = 5; x.raw_max_x = 5;
    x.raw_min_y = 5; x.raw_max_y = 5;
    point_xform_abs(&x, 5, 5, &lx, &ly);
    CHECK(lx >= 0 && lx <= 1279 && ly >= 0 && ly <= 799,
          "degenerate range -> (%d,%d)", lx, ly);
}

static void test_rel(void)
{
    struct point_xform x;
    int cx, cy;

    set_flag("POINT_SWAP_XY", NULL);
    set_flag("POINT_INVERT_X", NULL);
    set_flag("POINT_INVERT_Y", NULL);
    /* Pinned so the assertions do not depend on the default speed. */
    set_flag("POINT_MOUSE_SPEED", "1");
    point_xform_init(&x);

    cx = POINT_LOGICAL_W / 2;
    cy = POINT_LOGICAL_H / 2;
    point_xform_rel(&x, 100, 50, &cx, &cy);
    CHECK(cx == 740 && cy == 450, "rel +100,+50 from centre -> (%d,%d), expected (740,450)",
          cx, cy);

    /* Accumulating past the edge clamps; it must not wrap or go negative. */
    point_xform_rel(&x, 100000, 100000, &cx, &cy);
    CHECK(cx == 1279 && cy == 799, "rel past the edge -> (%d,%d), expected (1279,799)", cx, cy);
    point_xform_rel(&x, -100000, -100000, &cx, &cy);
    CHECK(cx == 0 && cy == 0, "rel past the origin -> (%d,%d), expected (0,0)", cx, cy);

    /* Inversion applies to the *motion* for a relative device: reflecting a
     * cursor position would teleport the pointer instead of reversing it. */
    set_flag("POINT_INVERT_X", "1");
    point_xform_init(&x);
    cx = 640; cy = 400;
    point_xform_rel(&x, 100, 0, &cx, &cy);
    CHECK(cx == 540, "rel with INVERT_X and dx=+100 -> %d, expected 540", cx);

    /* The empty string is what start-rb.sh exports for a variable the operator
     * did not set, so "" has to read as "off" — a truthiness test on presence
     * would enable swap and invert for everybody. */
    set_flag("POINT_MOUSE_SPEED", "");
    set_flag("POINT_SWAP_XY", "1");
    point_xform_init(&x);
    CHECK(x.swap_xy == 1, "POINT_SWAP_XY=1 read as off");
    set_flag("POINT_SWAP_XY", "");
    point_xform_init(&x);
    CHECK(x.swap_xy == 0, "POINT_SWAP_XY='' must mean unset, not enabled");

    set_flag("POINT_MOUSE_SPEED", NULL);
    set_flag("POINT_INVERT_X", NULL);
    set_flag("POINT_SWAP_XY", NULL);
}

/* --- 5. the fit rectangle (point_fit) --------------------------------------- */

/* Where the UI lands inside a panel that is not the logical 1280x800.  The
 * arithmetic is a SECOND COPY of the present path's fbdev_present_fit() in
 * work/dfb-src/systems/fbdev/fbdev.c -- a different build, no shared header --
 * so these numbers are pinned here to make a drift a failing test rather than a
 * cursor that draws in the wrong place.  The expected values are also the worked
 * table in the plan and what docs/13's S10 rows quote. */
static void test_fit(void)
{
    int dw, dh, bx, by;

    /* A panel that IS the logical size: the identity, which is the case the unit
     * runs today and must stay byte-for-byte what it was. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 1280, 800, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 1280 && dh == 800 && bx == 0 && by == 0,
          "identity fit -> %dx%d at %d,%d", dw, dh, bx, by);

    /* 720p, the panel most likely to be swapped in: 0.9x, 64 columns each side.
     * Here the HEIGHT binds and must come out exactly 720 -- a floored second
     * rounding gives 719, which is one row of the UI not shown and a one-pixel
     * bar on the bottom edge only. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 1280, 720, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 1152 && dh == 720 && bx == 64 && by == 0,
          "720p fit -> %dx%d at %d,%d, expected 1152x720 at 64,0", dw, dh, bx, by);

    /* The logical 1.6 aspect at 0.75: it fills exactly, so no bars at all. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 960, 600, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 960 && dh == 600 && bx == 0 && by == 0,
          "960x600 fit -> %dx%d at %d,%d, expected the whole panel", dw, dh, bx, by);

    /* 4:3, where the WIDTH binds and the bars are horizontal. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 800, 600, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 800 && dh == 500 && bx == 0 && by == 50,
          "800x600 fit -> %dx%d at %d,%d, expected 800x500 at 0,50", dw, dh, bx, by);

    /* An upscale, same code path -- and the one a 1080p TV takes. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 1920, 1080, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 1728 && dh == 1080 && bx == 96 && by == 0,
          "1080p fit -> %dx%d at %d,%d, expected 1728x1080 at 96,0", dw, dh, bx, by);

    /* 16:10 is the logical aspect, so an upscale that fills both axes. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 1920, 1200, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 1920 && dh == 1200 && bx == 0 && by == 0,
          "1920x1200 fit -> %dx%d at %d,%d, expected the whole panel", dw, dh, bx, by);

    /* stretch ignores the aspect and fills both axes, so there is no bar by
     * construction.  It is also what makes a same-size panel the identity. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 1280, 720, 1, &dw, &dh, &bx, &by);
    CHECK(dw == 1280 && dh == 720 && bx == 0 && by == 0,
          "stretch fit -> %dx%d at %d,%d, expected the whole panel", dw, dh, bx, by);

    /* A degenerate panel must not divide by zero, and must not hand back a
     * negative width: a fb read while its mode is being torn down can look like
     * this, and a negative dw would make the cursor paint walk backwards. */
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, 0, 0, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 0 && dh == 0 && bx == 0 && by == 0,
          "zero fb -> %dx%d at %d,%d", dw, dh, bx, by);
    point_fit(0, 0, 1280, 800, 0, &dw, &dh, &bx, &by);
    CHECK(dw == 0 && dh == 0 && bx == 0 && by == 0,
          "zero source -> %dx%d at %d,%d", dw, dh, bx, by);

    /* The invariant the floored step exists to guarantee, swept rather than
     * spot-checked: the rectangle may never be wider or taller than the fb (the
     * present path refuses such a blit outright, and the cursor would paint past
     * the end of its mapping), may never be empty, and must sit inside the fb.
     * Aggregated into one CHECK so the sweep does not inflate the check count. */
    {
        int fw, fh, bad = 0;
        char msg[160] = "";

        for (fw = 320; fw <= 2600 && !bad; fw += 7) {
            for (fh = 200; fh <= 1600; fh += 11) {
                point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, fw, fh, 0, &dw, &dh, &bx, &by);
                if (dw < 1 || dh < 1 || dw > fw || dh > fh || bx < 0 || by < 0 ||
                    bx + dw > fw || by + dh > fh) {
                    snprintf(msg, sizeof msg, "fb %dx%d -> %dx%d at %d,%d",
                             fw, fh, dw, dh, bx, by);
                    bad = 1;
                    break;
                }
            }
        }
        CHECK(bad == 0, "fit escaped its framebuffer: %s", msg);
    }
}

/* --- 6. the QUANTIZE boxes (touch_zone) ------------------------------------- */

/* The geometry is a measurement off a captured frame, so the literals below are
 * that measurement written down twice -- once in touch_zone.c, once here. That
 * is deliberate: a zone that quietly moved (a margin widened, an offset that
 * became a mirror) would still "work" in the sense that a tap somewhere would
 * toggle something, and the operator would have to find out with their finger.
 * The box is x 26..88 / 666..728, y 736..776, plus a 4 px margin; the deck-2
 * offset is +640 and emphatically NOT a reflection. */
static void test_quantize_zone(void)
{
    /* Every geometry probe below is its own touch. It has to be: a press counts
     * only on the edge, so a probe run after another press would be testing the
     * edge detector instead of the rectangle. */
#define TAP(x, y) (touch_zone_reset(), touch_zone_feed(1, (x), (y)))

    /* The widget's own extremes, and the margin's: the four corners of the
     * finger target, all inclusive. */
    CHECK(TAP(22, 732) == 1, "top-left of deck 1's target missed");
    CHECK(TAP(92, 780) == 1, "bottom-right of deck 1's target missed");
    CHECK(TAP(26, 736) == 1, "the label's top-left missed");
    CHECK(TAP(88, 776) == 1, "the value field's corner missed");
    CHECK(TAP(56, 755) == 1, "the middle of deck 1's widget missed");

    /* One pixel outside on every side is outside. The y band is the one that
     * matters most: above it is the deck's own control row, below it the empty
     * strip that runs to the panel rule at y 789. */
    CHECK(TAP(21, 755) == 0, "x=21 is inside the target");
    CHECK(TAP(93, 755) == 0, "x=93 is inside the target");
    CHECK(TAP(55, 731) == 0, "y=731 is inside the target");
    CHECK(TAP(55, 781) == 0, "y=781 is inside the target");

    /* Deck 2 is 640 px right of deck 1. The point a reflection would have put
     * deck 1's box at -- 1279-55 -- is 1224, and must hit nothing at all: a
     * reflection here would toggle one deck's quantize from the other's box and
     * would read as a mis-tap rather than as a bug. */
    CHECK(TAP(662, 755) == 2, "deck 2's target's left edge missed");
    CHECK(TAP(732, 755) == 2, "deck 2's target's right edge missed");
    CHECK(TAP(695, 736) == 2, "deck 2's label missed");
    CHECK(TAP(661, 755) == 0, "x=661 is inside deck 2's target");
    CHECK(TAP(733, 755) == 0, "x=733 is inside deck 2's target");
    CHECK(TAP(1224, 755) == 0, "the mirrored point hit a deck");

    /* Between them, and nowhere near them, is nothing. */
    CHECK(TAP(400, 755) == 0, "the middle of the screen hit a deck");
    CHECK(TAP(55, 400) == 0, "the upper screen hit a deck");

    /* The edge detector. A press emits a burst and a resting finger a stream of
     * identical reports (tscfake.c's duplicate suppression is on the wire, not
     * on this side), so only the transition into "down" may toggle -- otherwise
     * one touch would toggle quantize as many times as the panel sent reports. */
    touch_zone_reset();
    CHECK(touch_zone_feed(1, 55, 755) == 1, "a press was not recognized");
    CHECK(touch_zone_feed(1, 55, 755) == 0, "the same press toggled twice");
    CHECK(touch_zone_feed(1, 55, 756) == 0, "a jittering press toggled twice");
    CHECK(touch_zone_feed(0, 55, 755) == 0, "a release toggled");
    CHECK(touch_zone_feed(1, 55, 755) == 1, "the next press was swallowed");

    /* A press that starts outside and is dragged in is not a press on the box,
     * and one that leaves the box while down does not press the other deck. */
    CHECK(touch_zone_feed(0, 55, 755) == 0, "release");
    touch_zone_reset();
    CHECK(touch_zone_feed(1, 400, 400) == 0, "a press in open space hit a deck");
    CHECK(touch_zone_feed(1, 55, 755) == 0, "a drag into the box pressed it");
    CHECK(touch_zone_feed(1, 695, 755) == 0, "a drag across to deck 2 pressed it");
    CHECK(touch_zone_feed(0, 695, 755) == 0, "release");

    /* An unplugged pointer is the case reset() exists for: the device that was
     * down is gone, and the next device's first press has to be a first press.
     * Without this, one tap after every replug would be silently swallowed. */
    touch_zone_reset();
    CHECK(touch_zone_feed(1, 695, 755) == 2, "deck 2's press was not recognized");
    touch_zone_reset();
    CHECK(touch_zone_feed(1, 695, 755) == 2, "the press after a replug was lost");
    touch_zone_reset();
#undef TAP
}

int main(void)
{
    /* FIRST, AND IT IS NOT OPTIONAL: test_first_emit() forks a child to observe
     * what tscfake_emit() does in a process that has never published a pointer
     * state, and fork() copies whatever statics the parent has by then. Run after
     * any other test and the child would inherit a primed tscfake and the test
     * would pass without testing anything. */
    test_first_emit();
    test_record_layout();
    test_tsc_ioctls();
    test_emit_stream();
    test_emit_mirrors_x();
    test_tap_replay_stream();
    test_abs_sc_live4();
    test_abs_monitor();
    test_abs_degenerate();
    test_rel();
    test_fit();
    test_quantize_zone();

    printf("test_point: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
