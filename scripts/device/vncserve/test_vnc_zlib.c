/*
 * test_vnc_zlib.c -- does the stream this produces actually inflate back?
 *
 * THIS IS THE TEST THAT MAKES THE HAND-TRANSCRIBED z_stream SAFE. On the unit there is
 * no zlib.h, so vnc_zlib.c declares z_stream, deflateInit2_ and friends itself. Here
 * there IS a zlib.h -- the Mac has one -- so the test holds the library's own idea of
 * the struct and inflates what the module produced with it. A transcription that got
 * a field wrong, or fed the stream the wrong pixels, fails here rather than on the
 * operator's screen as a picture that is correct for one stripe and garbage for the
 * next.
 *
 * It also pins the property the wire format depends on: chunks fed to ONE never-reset
 * stream, sync-flushed between them, must each be decodable IN ORDER by one inflate
 * stream that is never reset either. That is exactly what a Tight client does with the
 * rectangles of an update, and it is the whole reason vnc_deflate has no reset path.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZLIB_CONST 1
#include <zlib.h>

#include "vnc_zlib.h"

static int fails, checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

/* A frame that looks like rbp's: mostly one flat colour with a few bands of detail, so
 * the ratio assertions below are about something real rather than about noise. */
#define W 128
#define H 96
#define N (W * H)

static void make_frame(uint16_t *px, int variant)
{
    int y, x;
    memset(px, 0, (size_t)N * 2);
    for (y = 8; y < 40; y++)
        for (x = 0; x < W; x++)
            px[y * W + x] = (uint16_t)(0x8410 + ((x + variant) & 0x1F));
    for (y = 60; y < 70; y++)
        for (x = 0; x < W; x++)
            px[y * W + x] = (uint16_t)(0xF800 ^ (uint16_t)(y * 7));
}

/* Feed `nchunks` chunks of `rows` rows each through one stream, inflating each chunk
 * back into `back` as it arrives, and check every byte. Returns the total compressed
 * bytes, or 0 on failure. */
static unsigned long round_trip(const uint16_t *px, int nchunks, int rows,
                                uint8_t *back, const char *what)
{
    struct vnc_deflate *d;
    z_stream in;
    unsigned long total = 0;
    size_t out_cap;
    uint8_t *out;
    int c;

    d = vnc_deflate_new(1, (size_t)W * H * 4);
    if (!d) {
        ok(0, "a deflate stream could be made");
        printf("       (%s)\n", vnc_deflate_error() ? vnc_deflate_error() : "no reason given");
        return 0;
    }
    out = malloc((size_t)W * H * 4 + 65536);
    if (!out) { vnc_deflate_free(d); return 0; }
    out_cap = (size_t)W * H * 4 + 65536;

    /* ONE inflate stream, never reset -- the client's side of the contract. */
    memset(&in, 0, sizeof in);
    if (inflateInit(&in) != Z_OK) { ok(0, "inflateInit"); free(out); vnc_deflate_free(d); return 0; }

    for (c = 0; c < nchunks; c++) {
        const uint8_t *z;
        size_t zlen;
        int r;

        vnc_deflate_begin(d);
        for (r = 0; r < rows; r++) {
            const uint16_t *row = px + (size_t)(c * rows + r) * W;
            if (vnc_deflate_feed(d, (const uint8_t *)row, (size_t)W * 2) != 0) {
                ok(0, "vnc_deflate_feed");
                printf("       (%s)\n", vnc_deflate_error() ? vnc_deflate_error() : "");
                goto done;
            }
        }
        z = vnc_deflate_end(d, &zlen);
        if (!z) {
            ok(0, "vnc_deflate_end");
            printf("       (%s)\n", vnc_deflate_error() ? vnc_deflate_error() : "");
            goto done;
        }
        ok(zlen > 0, "a chunk produced some bytes");
        total += zlen;

        /* Decode just this chunk, into its own place in the buffer. */
        in.next_in  = z;
        in.avail_in = (uInt)zlen;
        in.next_out = back + (size_t)c * rows * W * 2;
        in.avail_out = (uInt)(out_cap);
        {
            int rc = inflate(&in, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                ok(0, "inflate of a chunk");
                printf("       (chunk %d: inflate -> %d, %u bytes in, %u out)\n",
                       c, rc, (unsigned)zlen, (unsigned)(out_cap - in.avail_out));
                goto done;
            }
            ok(in.avail_in == 0, "the chunk was consumed whole");
        }
    }

    /* And now the bytes, all of them. */
    {
        size_t want = (size_t)nchunks * rows * W * 2;
        ok(memcmp(back, px, want) == 0, what);
    }

done:
    inflateEnd(&in);
    free(out);
    vnc_deflate_free(d);
    return total;
}

static void test_round_trip(void)
{
    uint16_t *px = malloc((size_t)N * 2);
    uint8_t *back = malloc((size_t)N * 2);
    unsigned long z, raw = (unsigned long)N * 2;

    make_frame(px, 0);
    z = round_trip(px, H / 8, 8, back, "eight 8-row chunks inflate back to the frame");
    if (z) {
        printf("       whole frame: %lu -> %lu bytes (%.1fx)\n", raw, z,
               (double)raw / (double)z);
        /* Not a ratio assertion -- that belongs in the measurement, not a test, and it
         * depends on the frame. What is asserted is that the thing compresses at all:
         * a stream that emitted its input verbatim would pass every check above. */
        ok(z < raw, "a mostly-flat frame compresses");
    }

    make_frame(px, 3);
    z = round_trip(px, 1, H, back, "one chunk covering the whole frame inflates back");
    ok(z > 0, "and produced bytes");

    make_frame(px, 7);
    z = round_trip(px, H, 1, back, "a chunk PER ROW inflates back, in order");
    ok(z > 0, "and produced bytes");

    free(px);
    free(back);
}

/* A run of identical frames must keep compressing to almost nothing on the same
 * stream: that is the dictionary the never-reset stream carries, and it is the reason
 * a never-reset stream is worth having at all. */
static void test_the_stream_keeps_its_history(void)
{
    uint16_t *px = malloc((size_t)N * 2);
    uint8_t *back = malloc((size_t)N * 2);
    struct vnc_deflate *d = vnc_deflate_new(1, (size_t)N * 4);
    size_t first = 0, later = 0;
    int i;

    ok(d != NULL, "a stream for the history test");
    make_frame(px, 0);
    for (i = 0; i < 2; i++) {
        const uint8_t *z;
        size_t zlen;
        vnc_deflate_begin(d);
        ok(vnc_deflate_feed(d, (const uint8_t *)px, (size_t)N * 2) == 0, "feed the frame");
        z = vnc_deflate_end(d, &zlen);
        ok(z != NULL, "flush the frame");
        if (i == 0) first = zlen; else later = zlen;
    }
    printf("       the same frame twice: %zu bytes then %zu\n", first, later);
    ok(later < first, "the second time round the stream pays much less");

    vnc_deflate_free(d);
    free(px);
    free(back);
}

static void test_failure_is_soft(void)
{
    /* A level zlib will not take. deflateInit2_ refuses it, and this module's answer
     * is a NULL stream and a reason -- never a crash and never a corrupt stream. The
     * session treats that as "this client gets Raw", which is the whole fallback. */
    struct vnc_deflate *d = vnc_deflate_new(99, 1024);
    if (d) {
        ok(0, "a level of 99 should not have produced a stream");
        vnc_deflate_free(d);
    } else {
        ok(vnc_deflate_error() != NULL, "and it says why");
        printf("       (%s)\n", vnc_deflate_error());
    }
}

int main(void)
{
    printf("test_vnc_zlib  (linked against zlib %s)\n", zlibVersion());
    test_round_trip();
    test_the_stream_keeps_its_history();
    test_failure_is_soft();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
