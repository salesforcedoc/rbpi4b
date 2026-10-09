/*
 * test_vnc_diff.c -- the band rules, pinned against the shape of rbp's own screen.
 *
 * The failure this guards is quiet and specific: a band that is dropped from the list
 * leaves a stripe of the screen that is NEVER resent, so the client sits looking at a
 * picture that is right everywhere except one horizontal slice, forever. Truncating
 * the band list when there are more bands than the caller's array holds would do
 * exactly that, which is why the function reports the true count and the caller sends
 * one whole-frame rectangle instead.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <string.h>

#include "vnc_diff.h"

static int fails, checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

static void eq_int(long got, long want, const char *what)
{
    checks++;
    if (got != want) { fails++; printf("  FAIL %s: got %ld, want %ld\n", what, got, want); }
}

#define W 16
#define H 40

static uint16_t a[H * W], b[H * W];

/* Mark rows [y0, y1) as changed, everything else as identical. */
static void paint(int y0, int y1)
{
    int y, x;
    memset(b, 0, sizeof b);
    memset(a, 0, sizeof a);
    for (y = y0; y < y1; y++)
        for (x = 0; x < W; x++)
            b[y * W + x] = (uint16_t)(0x1234 + y);
}

static void test_nothing_moved(void)
{
    struct vnc_band out[VNC_BAND_MAX];
    long dirty = -1;

    paint(0, 0);
    eq_int(vnc_diff_same(a, b, W, H), 1, "identical frames compare equal");

    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, &dirty), 0,
           "identical frames have no bands");
    eq_int(dirty, 0, "and no dirty pixels");
}

static void test_same_is_a_cheap_lie_detector(void)
{
    paint(0, 0);                          /* two identical frames */
    b[20 * W + 0] = 0x1234;               /* ONE pixel, in the first column */
    eq_int(vnc_diff_same(a, b, W, H), 0,
           "a single changed pixel in the first column is enough to differ");
    b[20 * W + 0] = 0;                    /* and put it back */
    eq_int(vnc_diff_same(a, b, W, H), 1, "and putting it back makes them equal again");
}

static void test_one_band(void)
{
    struct vnc_band out[VNC_BAND_MAX];

    paint(5, 12);
    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, NULL), 1,
           "one run of changed rows is one band");
    eq_int(out[0].y0, 5, "and it starts where the change does");
    eq_int(out[0].y1, 12, "and ends one past the last changed row (half-open)");
}

static void test_the_gap_is_absorbed(void)
{
    struct vnc_band out[VNC_BAND_MAX];
    int y, x;

    /* Two runs separated by exactly VNC_BAND_GAP clean rows: one band. */
    paint(0, 0);
    for (y = 0; y < 4; y++) for (x = 0; x < W; x++) b[y * W + x] = 0x1234;
    for (y = 4 + VNC_BAND_GAP; y < 12; y++) for (x = 0; x < W; x++) b[y * W + x] = 0x1234;

    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, NULL), 1,
           "a gap of exactly the tolerance is bridged into one band");
    eq_int(out[0].y0, 0, "which starts at the first run");
    eq_int(out[0].y1, 12, "and ends at the last");

    /* One more clean row and it is two bands. */
    paint(0, 0);
    for (y = 0; y < 4; y++) for (x = 0; x < W; x++) b[y * W + x] = 0x1234;
    for (y = 5 + VNC_BAND_GAP; y < 12; y++) for (x = 0; x < W; x++) b[y * W + x] = 0x1234;
    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, NULL), 2,
           "one clean row more and it is two bands");
    eq_int(out[0].y0, 0,  "first band starts at 0");
    eq_int(out[0].y1, 4,  "first band stops at the last changed row, clean rows excluded");
    eq_int(out[1].y0, 5 + VNC_BAND_GAP, "second band starts at the second run");
    eq_int(out[1].y1, 12, "second band ends at the bottom of the change");
}

static void test_a_band_that_reaches_the_bottom(void)
{
    struct vnc_band out[VNC_BAND_MAX];

    paint(H - 3, H);
    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, NULL), 1,
           "a change that reaches the last row is one band");
    eq_int(out[0].y1, H, "and it is closed at the frame's height, not left open");
}

static void test_too_many_bands_is_reported_not_truncated(void)
{
    struct vnc_band out[3];
    int y, x, n;

    paint(0, 0);
    for (y = 0; y < H; y += 2)
        for (x = 0; x < W; x++) b[y * W + x] = (uint16_t)(0x100 + y);

    n = vnc_diff_bands(a, b, W, H, 0, out, 3, NULL);
    eq_int(n, H / 2, "every other row is a band of its own, and all of them are counted");
    ok(n > 3, "the count exceeds the array the caller offered");
    eq_int(out[0].y0, 0, "the bands it did write are the first ones");
    eq_int(out[2].y0, 4, "and they are in order");
}

static void test_the_dirty_count(void)
{
    struct vnc_band out[VNC_BAND_MAX];
    long dirty = -1;
    int y, x;

    paint(0, 0);
    for (y = 2; y < 5; y++)
        for (x = 0; x < 3; x++)                    /* three pixels a row, five rows */
            b[y * W + x] = (uint16_t)(0x9000 + y * W + x);

    eq_int(vnc_diff_bands(a, b, W, H, VNC_BAND_GAP, out, VNC_BAND_MAX, &dirty), 1,
           "three changed pixels a row across three rows is one band");
    eq_int(dirty, 3 * 3, "and nine dirty pixels");
}

int main(void)
{
    printf("test_vnc_diff\n");
    test_nothing_moved();
    test_same_is_a_cheap_lie_detector();
    test_one_band();
    test_the_gap_is_absorbed();
    test_a_band_that_reaches_the_bottom();
    test_too_many_bands_is_reported_not_truncated();
    test_the_dirty_count();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
