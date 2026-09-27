/*
 * test_point.c — the pointer path's unit test: no Pi, no device, no rbp.
 *
 * Two things are pinned here, and they are pinned for different reasons.
 *
 * 1. The record stream rbp reads. Everything about the fake tsc2007 is
 *    unverifiable without the player, so the parts that *are* verifiable — the
 *    byte layout, the duplicate suppression, and the two-frame burst on a press
 *    — are asserted against a real pipe through the real tscfake_emit(). A
 *    future change to the emit rules has to break this test out loud instead of
 *    silently making the first tap of every gesture invisible.
 *
 * 2. The coordinate algebra. The repo's own two touch implementations disagreed
 *    about it, so this asserts the *old* SC Live 4 result is reproducible from
 *    the new flags (swap=1, range 0..2047, no inversions) — that is the check
 *    that the generalization did not change the algebra it generalizes.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_point test_point.c tscfake.c
 *            point_xform.c   &&   qemu-arm ./test_point
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "point_xform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    fd = tscfake_open();
    CHECK(fd >= 0, "tscfake_open failed");
    if (fd < 0)
        return;
    CHECK(tscfake_is_fd(fd) == 1, "tscfake_is_fd says no for a just-opened fd");

    for (i = 0; i < 5; i++)
        tscfake_emit(seq[i][0], seq[i][1], seq[i][2]);

    tscfake_record(seq[0][0], seq[0][1], seq[0][2], expect);
    tscfake_record(1, 100, 200, expect + TSC_RECORD_LEN);          /* burst, frame 1 */
    tscfake_record(1, 100, 200, expect + 2 * TSC_RECORD_LEN);      /* burst, frame 2 */
    tscfake_record(seq[4][0], seq[4][1], seq[4][2], expect + 3 * TSC_RECORD_LEN);

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
        tscfake_record(0, 300, 200, rel);
        CHECK(memcmp(got, rel, TSC_RECORD_LEN) == 0, "release record differs");
    }

    CHECK(tscfake_close(fd) == 0, "tscfake_close failed");
    CHECK(tscfake_is_fd(fd) == 0, "tscfake_is_fd still true after close");
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

int main(void)
{
    test_record_layout();
    test_tsc_ioctls();
    test_emit_stream();
    test_abs_sc_live4();
    test_abs_monitor();
    test_abs_degenerate();
    test_rel();
    test_fit();

    printf("test_point: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
