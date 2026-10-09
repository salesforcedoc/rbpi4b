/*
 * test_vnc_compose.c -- the plane composite's clipping and z-order.
 *
 * Every plane this puts on screen is clipped by its own edges at least once per
 * animation: the drawers slide in from off the left and right edges, and the USB-STOP
 * chooser rises from off the bottom. A compositor that gets the clip wrong does not
 * fail loudly -- it draws the plane at the wrong offset, or scribbles past the end of
 * the frame -- so the cases below are the ones that would actually happen.
 *
 * make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <string.h>

#include "vnc_compose.h"

static int fails, checks;

#define FW 8
#define FH 4

static void check_pixel(const uint16_t *f, int x, int y, uint16_t want, const char *what)
{
    checks++;
    if (f[y * FW + x] != want) {
        fails++;
        printf("  FAIL %s: (%d,%d) is 0x%04X, want 0x%04X\n", what, x, y, f[y * FW + x], want);
    }
}

static void test_inside(void)
{
    uint16_t f[FW * FH], src[6];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    for (i = 0; i < 6; i++) src[i] = (uint16_t)(i + 1);

    p.dst_x = 2; p.dst_y = 1; p.w = 3; p.h = 2;
    p.src = src; p.src_stride_px = 3; p.zpos = 0;
    vnc_compose_blit(f, FW, FH, &p);

    /* The rectangle lands where it was asked to, and nothing around it moves. */
    check_pixel(f, 2, 1, 1, "inside: top-left");
    check_pixel(f, 3, 1, 2, "inside: top-middle");
    check_pixel(f, 4, 1, 3, "inside: top-right");
    check_pixel(f, 2, 2, 4, "inside: bottom-left");
    check_pixel(f, 4, 2, 6, "inside: bottom-right");
    check_pixel(f, 1, 1, 0xFFFF, "inside: left neighbour untouched");
    check_pixel(f, 5, 1, 0xFFFF, "inside: right neighbour untouched");
    check_pixel(f, 2, 0, 0xFFFF, "inside: row above untouched");
    check_pixel(f, 2, 3, 0xFFFF, "inside: row below untouched");
}

static void test_clip_left(void)
{
    uint16_t f[FW * FH], src[3];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    src[0] = 10; src[1] = 11; src[2] = 12;

    p.dst_x = -1; p.dst_y = 0; p.w = 3; p.h = 1;
    p.src = src; p.src_stride_px = 3; p.zpos = 0;
    vnc_compose_blit(f, FW, FH, &p);

    /* One column is off the left edge, so the first source column is skipped. */
    check_pixel(f, 0, 0, 11, "clip left: first visible pixel is src[1]");
    check_pixel(f, 1, 0, 12, "clip left: second visible pixel is src[2]");
    check_pixel(f, 2, 0, 0xFFFF, "clip left: the plane is only three wide");
}

static void test_clip_right(void)
{
    uint16_t f[FW * FH], src[3];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    src[0] = 20; src[1] = 21; src[2] = 22;

    p.dst_x = FW - 2; p.dst_y = 0; p.w = 3; p.h = 1;
    p.src = src; p.src_stride_px = 3; p.zpos = 0;
    vnc_compose_blit(f, FW, FH, &p);

    check_pixel(f, FW - 2, 0, 20, "clip right: first visible pixel is src[0]");
    check_pixel(f, FW - 1, 0, 21, "clip right: second visible pixel is src[1]");
    /* src[2] has nowhere to go, and -- the bug this catches -- must not be written
     * one row down, which is what a stride-blind clip does. */
    check_pixel(f, 0, 1, 0xFFFF, "clip right: src[2] did not wrap to the next row");
}

static void test_clip_bottom(void)
{
    uint16_t f[FW * FH], src[6];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    for (i = 0; i < 6; i++) src[i] = (uint16_t)(30 + i);

    p.dst_x = 0; p.dst_y = FH - 1; p.w = 2; p.h = 2;
    p.src = src; p.src_stride_px = 2; p.zpos = 0;
    vnc_compose_blit(f, FW, FH, &p);

    check_pixel(f, 0, FH - 1, 30, "clip bottom: the last row takes the first source row");
    check_pixel(f, 1, FH - 1, 31, "clip bottom: ... and its second pixel");
    check_pixel(f, 0, 0, 0xFFFF, "clip bottom: the frame above is untouched");
}

static void test_offscreen(void)
{
    uint16_t f[FW * FH], src[4];
    struct vnc_plane p;
    int i;

    src[0] = src[1] = src[2] = src[3] = 0x1234;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    p.w = 2; p.h = 2; p.src = src; p.src_stride_px = 2; p.zpos = 0;

    /* Fully off each edge. None of these may write a single pixel. */
    p.dst_x = -2; p.dst_y = 0;
    vnc_compose_blit(f, FW, FH, &p);
    p.dst_x = FW; p.dst_y = 0;
    vnc_compose_blit(f, FW, FH, &p);
    p.dst_x = 0; p.dst_y = -2;
    vnc_compose_blit(f, FW, FH, &p);
    p.dst_x = 0; p.dst_y = FH;
    vnc_compose_blit(f, FW, FH, &p);

    for (i = 0; i < FW * FH; i++) {
        checks++;
        if (f[i] != 0xFFFF) {
            fails++;
            printf("  FAIL offscreen: index %d was written (0x%04X)\n", i, f[i]);
            break;
        }
    }
}

static void test_stride(void)
{
    uint16_t f[FW * FH], src[10];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    for (i = 0; i < 10; i++) src[i] = 0xDEAD;   /* padding, and bait */

    /* A real framebuffer's pitch is wider than the plane. Two rows of three, five
     * apart -- so the second source row starts at src[5], not src[3]. */
    src[0] = 1; src[1] = 2; src[2] = 3;
    src[5] = 4; src[6] = 5; src[7] = 6;

    p.dst_x = 1; p.dst_y = 0; p.w = 3; p.h = 2;
    p.src = src; p.src_stride_px = 5; p.zpos = 0;
    vnc_compose_blit(f, FW, FH, &p);

    check_pixel(f, 1, 0, 1, "stride: row 0 pixel 0");
    check_pixel(f, 3, 0, 3, "stride: row 0 pixel 2");
    check_pixel(f, 1, 1, 4, "stride: row 1 reads from src[5], not src[3]");
    check_pixel(f, 3, 1, 6, "stride: row 1 pixel 2");
}

static void test_z_order(void)
{
    uint16_t f[FW * FH], below[4], above[4];
    struct vnc_plane planes[2];
    int i;

    below[0] = below[1] = below[2] = below[3] = 100;
    above[0] = above[1] = above[2] = above[3] = 200;

    /* `below` covers x 0..1, `above` covers x 1..2, so they overlap at x 1. */
    planes[0].dst_x = 0; planes[0].dst_y = 0; planes[0].w = 2; planes[0].h = 2;
    planes[0].src = below; planes[0].src_stride_px = 2; planes[0].zpos = 1;

    planes[1].dst_x = 1; planes[1].dst_y = 0; planes[1].w = 2; planes[1].h = 2;
    planes[1].src = above; planes[1].src_stride_px = 2; planes[1].zpos = 2;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    vnc_compose_planes(f, FW, FH, planes, 2);
    check_pixel(f, 0, 0, 100, "z: the lower plane shows where nothing covers it");
    check_pixel(f, 1, 0, 200, "z: the higher plane wins the overlap");
    check_pixel(f, 2, 0, 200, "z: the higher plane shows on its own side");

    /* Same two planes, opposite order in the array: the sort must still put the
     * higher one last. A compositor that trusts the caller's order draws whichever
     * the kernel happened to list first. */
    {
        struct vnc_plane rev[2];
        rev[0] = planes[1];
        rev[1] = planes[0];
        for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
        vnc_compose_planes(f, FW, FH, rev, 2);
        check_pixel(f, 1, 0, 200, "z: order in the array does not decide the winner");
    }
}

static void test_degenerate(void)
{
    uint16_t f[FW * FH], src[4];
    struct vnc_plane p;
    int i;

    for (i = 0; i < FW * FH; i++) f[i] = 0xFFFF;
    src[0] = src[1] = src[2] = src[3] = 0x4321;

    /* Each of these must be a no-op rather than a crash or a write. */
    p.dst_x = 0; p.dst_y = 0; p.h = 2; p.src = src; p.src_stride_px = 2; p.zpos = 0;

    p.w = 0;  vnc_compose_blit(f, FW, FH, &p);
    p.w = -3; vnc_compose_blit(f, FW, FH, &p);
    p.w = 2; p.h = 0; vnc_compose_blit(f, FW, FH, &p);
    p.w = 2; p.h = 2; p.src = NULL; vnc_compose_blit(f, FW, FH, &p);

    /* An empty plane list is a valid frame -- the deck with no drawers open. */
    vnc_compose_planes(f, FW, FH, NULL, 0);

    for (i = 0; i < FW * FH; i++) {
        checks++;
        if (f[i] != 0xFFFF) {
            fails++;
            printf("  FAIL degenerate: index %d was written (0x%04X)\n", i, f[i]);
            break;
        }
    }
}

int main(void)
{
    printf("test_vnc_compose\n");
    test_inside();
    test_clip_left();
    test_clip_right();
    test_clip_bottom();
    test_offscreen();
    test_stride();
    test_z_order();
    test_degenerate();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
