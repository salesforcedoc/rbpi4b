/*
 * test_vnc_rfb.c -- the RFB byte shapes, pinned.
 *
 * The point of this file is the rectangle framing. Everything else here is a
 * table that would be caught by an eye; the compact length would not. Get it wrong
 * and the client does not complain -- it reads the next message out of the middle
 * of this one and renders garbage, or waits forever, and the bug looks like a
 * capture problem three modules away. So it is asserted rather than reasoned about.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <string.h>

#include "vnc_rfb.h"

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

static void eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (want == NULL ? got != NULL : (!got || strcmp(got, want) != 0)) {
        fails++;
        printf("  FAIL %s: got %s, want %s\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

static void eq_mem(const void *got, const void *want, size_t n, const char *what)
{
    checks++;
    if (memcmp(got, want, n) != 0) {
        size_t i;
        fails++;
        printf("  FAIL %s: got", what);
        for (i = 0; i < n; i++) printf(" %02X", ((const unsigned char *)got)[i]);
        printf(", want");
        for (i = 0; i < n; i++) printf(" %02X", ((const unsigned char *)want)[i]);
        printf("\n");
    }
}

static void test_version(void)
{
    int maj = 0, min = 0;

    ok(vnc_rfb_parse_version("RFB 003.008\n", &maj, &min), "3.8 parses");
    eq_int(maj, 3, "3.8 major");
    eq_int(min, 8, "3.8 minor");

    /* Apple's Screen Sharing says this. It must parse, and it must not be mistaken
     * for 8 -- the effective version is min(889,8) at the call site, and the raw
     * value is what gets logged. */
    ok(vnc_rfb_parse_version("RFB 003.889\n", &maj, &min), "3.889 parses");
    eq_int(min, 889, "3.889 minor is 889, not 8");

    ok(vnc_rfb_parse_version("RFB 003.003\n", &maj, &min), "3.3 parses");
    eq_int(min, 3, "3.3 minor");

    ok(!vnc_rfb_parse_version("VFU 003.008\n", &maj, &min), "bad magic refused");
    ok(!vnc_rfb_parse_version("RFB 003.008X\n", &maj, &min), "bad terminator refused");
    ok(!vnc_rfb_parse_version("RFB 00X.008\n", &maj, &min), "non-digit refused");
    ok(!vnc_rfb_parse_version("RFB 003-008\n", &maj, &min), "bad separator refused");
}

static void test_encoding_names(void)
{
    eq_str(vnc_rfb_encoding_name(VNC_ENC_RAW),  "Raw",   "0");
    eq_str(vnc_rfb_encoding_name(VNC_ENC_TIGHT), "Tight", "7 is the one that matters");
    eq_str(vnc_rfb_encoding_name(1234), NULL, "unknown positive has no name");
    eq_str(vnc_rfb_encoding_name(-32), NULL, "a pseudo-encoding is not a real one");
}

static void test_pseudo_names(void)
{
    eq_str(vnc_rfb_pseudo_name(VNC_ENC_LASTRECT),      "LastRect",             "-224");
    eq_str(vnc_rfb_pseudo_name(VNC_ENC_QUALITY_0),     "QualityLevel0",        "-32");
    eq_str(vnc_rfb_pseudo_name(VNC_ENC_FINE_QUALITY_0),"FineQualityLevel0",    "-512");
    eq_str(vnc_rfb_pseudo_name(VNC_ENC_SUBSAMP_1X),    "Subsamp1X",            "-768");
    eq_str(vnc_rfb_pseudo_name(VNC_ENC_COMPRESS_0),    "CompressLevel0",       "-256");
    eq_str(vnc_rfb_pseudo_name(0), NULL, "a real encoding is not a pseudo-encoding");
    eq_str(vnc_rfb_pseudo_name(-9999), NULL, "unknown negative has no name");
}

static void test_quality_predicate(void)
{
    /* The whole ladder, at both bases, and the values just outside it. A client that
     * sends one of these has asked for a *quality level* -- and since the spec forbids
     * JPEG unless it has, this predicate is now the second half of the go/no-go for the
     * hardware encoder rather than a word on a log line. So the tops of both ladders are
     * pinned: a rung this misses is a client needlessly denied the encoder. */
    eq_int(vnc_rfb_pseudo_is_quality(-32), 1, "QualityLevel0");
    eq_int(vnc_rfb_pseudo_is_quality(-29), 1, "QualityLevel3");
    eq_int(vnc_rfb_pseudo_is_quality(-23), 1, "QualityLevel9");
    eq_int(vnc_rfb_pseudo_is_quality(-22), 0, "one past QualityLevel9");
    eq_int(vnc_rfb_pseudo_is_quality(-33), 0, "one below QualityLevel0");
    eq_int(vnc_rfb_pseudo_is_quality(-512), 1, "FineQualityLevel0");
    eq_int(vnc_rfb_pseudo_is_quality(-497), 1, "FineQualityLevel15");
    eq_int(vnc_rfb_pseudo_is_quality(-413), 1, "FineQualityLevel99");
    eq_int(vnc_rfb_pseudo_is_quality(-412), 1, "FineQualityLevel100, the top rung");
    eq_int(vnc_rfb_pseudo_is_quality(-411), 0, "one past the fine ladder");
    eq_int(vnc_rfb_pseudo_is_quality(-513), 0, "one below the fine ladder");
    eq_int(vnc_rfb_pseudo_is_quality(-224), 0, "LastRect is not a quality");
    eq_int(vnc_rfb_pseudo_is_quality(7), 0, "Tight is not a quality");
}

static void test_security_result_rule(void)
{
    /* THE BUG THIS PINS COST THE OPERATOR A SPINNING WINDOW. The rule was written
     * down in three places as "3.3 has no SecurityResult" -- which is what RFC 6143
     * appendix A.1 says about the *None* path, read as if it were about 3.3. For
     * VNC auth, every version sends the word, and a client that never receives it
     * does not error: it waits. macOS's Screen Sharing answered the challenge, got
     * "password accepted" in the log, and then sat silent until the server's own
     * fifteen-second client-init timeout fired.
     *
     * So the version boundary is pinned on BOTH axes. Testing one axis is how the
     * original mistake stayed invisible: 3.3-with-None and 3.3-with-VNC-auth have
     * different answers, and only the second is a live path here, because the 3.3
     * branch offers VNC auth and never None. */
    eq_int(vnc_rfb_sends_security_result(3, VNC_SEC_VNC), 1,
           "3.3 + VNC auth DOES send a SecurityResult -- the whole bug");
    eq_int(vnc_rfb_sends_security_result(7, VNC_SEC_VNC), 1, "3.7 + VNC auth");
    eq_int(vnc_rfb_sends_security_result(8, VNC_SEC_VNC), 1, "3.8 + VNC auth");
    eq_int(vnc_rfb_sends_security_result(889, VNC_SEC_VNC), 1,
           "3.889 + VNC auth, clamped by the caller to 8");
    eq_int(vnc_rfb_sends_security_result(3, VNC_SEC_NONE), 0,
           "3.3 + None proceeds straight to the initialization messages");
    eq_int(vnc_rfb_sends_security_result(7, VNC_SEC_NONE), 0,
           "3.7 + None, same rule as 3.3");
    eq_int(vnc_rfb_sends_security_result(8, VNC_SEC_NONE), 1,
           "3.8 + None is the one that DOES send it -- 7.1.3 makes no exception");
}

static void test_pixel_format(void)
{
    uint8_t wire[16], again[16];
    struct vnc_pixel_format pf;
    static const uint8_t want[16] = {
        16, 16, 0, 1,
        0, 31, 0, 63, 0, 31,
        11, 5, 0,
        0, 0, 0
    };
    char desc[160];

    vnc_pf_write(wire, &vnc_pf_rgb565);
    eq_mem(wire, want, 16, "RGB565 on the wire");

    vnc_pf_read(wire, &pf);
    ok(vnc_pf_is_rgb565_le(&pf), "round trip reads back as fb0's format");
    vnc_pf_write(again, &pf);
    eq_mem(again, wire, 16, "write/read/write is stable");

    /* Every field is load-bearing for the memcpy fast path. */
    pf = vnc_pf_rgb565; pf.bits_per_pixel = 32;
    ok(!vnc_pf_is_rgb565_le(&pf), "32bpp is not the fast path");
    pf = vnc_pf_rgb565; pf.red_shift = 10;
    ok(!vnc_pf_is_rgb565_le(&pf), "a different red shift is not the fast path");
    pf = vnc_pf_rgb565; pf.green_max = 31;
    ok(!vnc_pf_is_rgb565_le(&pf), "555 is not 565");
    pf = vnc_pf_rgb565; pf.big_endian = 1;
    ok(!vnc_pf_is_rgb565_le(&pf), "big-endian is not the fast path");
    pf = vnc_pf_rgb565; pf.depth = 24;
    ok(!vnc_pf_is_rgb565_le(&pf), "depth 24 is not the fast path");

    vnc_pf_describe(&vnc_pf_rgb565, desc, sizeof desc);
    ok(strstr(desc, "16bpp") != NULL, "describe names the bpp");
    ok(strstr(desc, "shift 11/5/0") != NULL, "describe names the shifts");
}

static void test_compact_len(void)
{
    uint8_t b[8];

    /* One byte: 0..127. */
    eq_int(vnc_rfb_write_compact_len(b, 0), 1, "0 is one byte");
    eq_int(b[0], 0x00, "0");
    eq_int(vnc_rfb_write_compact_len(b, 1), 1, "1 is one byte");
    eq_int(b[0], 0x01, "1");
    eq_int(vnc_rfb_write_compact_len(b, 0x7F), 1, "127 is one byte");
    eq_int(b[0], 0x7F, "127");

    /* Two bytes: 128..16383. Bit 7 of the first byte means "another follows". */
    eq_int(vnc_rfb_write_compact_len(b, 0x80), 2, "128 is two bytes");
    eq_int(b[0], 0x80, "128 low byte");
    eq_int(b[1], 0x01, "128 high nibble");
    eq_int(vnc_rfb_write_compact_len(b, 0x3FFF), 2, "16383 is two bytes");
    eq_int(b[0], 0xFF, "16383 low");
    eq_int(b[1], 0x7F, "16383 high");

    /* Three bytes: 16384 and up. The THIRD byte carries a full 8 bits, so the
     * encoding is only 7-bit for the first two -- this is the detail that is easy to
     * get wrong and impossible to notice. */
    eq_int(vnc_rfb_write_compact_len(b, 0x4000), 3, "16384 is three bytes");
    eq_int(b[0], 0x80, "16384 low");
    eq_int(b[1], 0x80, "16384 continue");
    eq_int(b[2], 0x01, "16384 third byte is 1, not 0");

    eq_int(vnc_rfb_write_compact_len(b, 0x3FFFFF), 3, "the maximum");
    eq_int(b[0], 0xFF, "max low");
    eq_int(b[1], 0xFF, "max mid");
    eq_int(b[2], 0xFF, "max third byte uses all eight bits");

    /* The worked example from the header, which is what a real JPEG looks like. */
    eq_int(vnc_rfb_write_compact_len(b, 40000), 3, "40000 length is three bytes");
    {
        static const uint8_t want[3] = { 0xC0, 0xB8, 0x02 };
        eq_mem(b, want, 3, "40000 -> C0 B8 02");
    }
}

static void test_jpeg_header(void)
{
    uint8_t h[8];

    /* 0x90, exactly: rfbTightJpeg << 4. The decoder peels the low nibble off and
     * compares the result BY EQUALITY against 0x09, so 0x09 here would fail. */
    eq_int(vnc_rfb_jpeg_header(h, 40000), 4, "40000 needs four bytes of header");
    {
        static const uint8_t want[4] = { 0x90, 0xC0, 0xB8, 0x02 };
        eq_mem(h, want, 4, "the 40000-byte JPEG header is 90 C0 B8 02");
    }

    eq_int(vnc_rfb_jpeg_header(h, 1000), 3, "1000 needs three bytes");
    {
        static const uint8_t want[3] = { 0x90, 0xE8, 0x07 };
        eq_mem(h, want, 3, "1000 -> 90 E8 07");
    }

    eq_int(vnc_rfb_jpeg_header(h, 5), 2, "5 needs two bytes");
    {
        static const uint8_t want[2] = { 0x90, 0x05 };
        eq_mem(h, want, 2, "5 -> 90 05");
    }
}

static void test_tight_zlib_header(void)
{
    uint8_t h[8];

    /* 0x00: basic compression, zlib stream 0, and all four reset bits clear.
     *
     * The reset bits being clear is the load-bearing half. A Tight client keeps one
     * inflate stream per stream index for the life of the connection, and this server
     * -- like libvncserver, whose 0x00 this is -- keeps its deflate stream alive to
     * match. Set bit 0 and the client throws its stream away and reads the zlib header
     * of a stream that is mid-flight, which decodes as garbage.
     *
     * And it is NOT 0xA0: rfbTightNoZlib says the opposite, that this rectangle's bytes
     * are raw pixels with no zlib stream involved. That byte with these bytes after it
     * is how a client ends up drawing compressed data as pixels. */
    eq_int(vnc_rfb_tight_zlib_header(h, 40000), 4, "40000 needs four bytes of header");
    {
        static const uint8_t want[4] = { 0x00, 0xC0, 0xB8, 0x02 };
        eq_mem(h, want, 4, "the 40000-byte zlib header is 00 C0 B8 02");
    }

    eq_int(vnc_rfb_tight_zlib_header(h, 1000), 3, "1000 needs three bytes");
    {
        static const uint8_t want[3] = { 0x00, 0xE8, 0x07 };
        eq_mem(h, want, 3, "1000 -> 00 E8 07");
    }

    /* The two headers differ in exactly one nibble, which is worth asserting rather
     * than reading: they are written by two functions two screens apart and a copy
     * -paste between them would look right and be a desync. */
    {
        uint8_t j[4], z[4];
        vnc_rfb_jpeg_header(j, 40000);
        vnc_rfb_tight_zlib_header(z, 40000);
        eq_int(j[0], 0x90, "JPEG control byte");
        eq_int(z[0], 0x00, "zlib control byte");
        eq_mem(j + 1, z + 1, 3, "the compact length after each is identical");
    }
}

/* Encoding 6, "zlib" -- the encoding macOS's Screen Sharing actually offers, and the
 * one whose absence kept that client on 4 MB Raw frames.
 *
 * IT IS DELIBERATELY NOT SHAPED LIKE THE TWO ABOVE. Tight and Tight-JPEG both begin with
 * a control byte and then a compact length; encoding 6 has only a plain 4-byte
 * big-endian length, and the whole point of these checks is that a reader who assumes
 * otherwise -- or a copy-paste between the three functions -- produces bytes that
 * desync instead of bytes that fail. */
static void test_zlib_rect_header(void)
{
    uint8_t h[4];

    eq_int(vnc_rfb_zlib_rect_header(h, 40000), 4,
           "encoding 6's header is always exactly four bytes");

    /* 40000 = 0x00009C40. The bytes are 00 00 9C 40 and NOT 40 9C 00 00: the length is
     * big-endian, which is RFB's general rule for every multi-byte integer except the
     * pixel values themselves. Little-endian here would be a plausible-looking 40 9C
     * 00 00 that decodes as a 1084-megabyte rectangle. */
    {
        static const uint8_t want[4] = { 0x00, 0x00, 0x9C, 0x40 };
        eq_mem(h, want, 4, "40000 -> 00 00 9C 40, high byte first");
    }

    /* The two bounds of the field, because they are where a 24-bit assumption breaks.
     * A client that reads only three bytes -- or a writer that wrote a compact length --
     * is right for every size a screen update ever takes and wrong here. */
    vnc_rfb_zlib_rect_header(h, 0u);
    {
        static const uint8_t want[4] = { 0x00, 0x00, 0x00, 0x00 };
        eq_mem(h, want, 4, "an empty rectangle is four zero bytes, not one");
    }
    vnc_rfb_zlib_rect_header(h, 0xFFFFFFFFu);
    {
        static const uint8_t want[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        eq_mem(h, want, 4, "the field is a full 32 bits");
    }

    /* ...and the one fact that separates the three encodings on the wire, asserted
     * rather than assumed: encoding 6's header shares no prefix with Tight's. If this
     * ever passes with equal first bytes, someone has wired the wrong encoder in. */
    {
        uint8_t t[4];
        vnc_rfb_tight_zlib_header(t, 40000);
        vnc_rfb_zlib_rect_header(h, 40000);
        eq_int(t[0], 0x00, "Tight's first byte is the control byte");
        eq_int(h[0], 0x00, "encoding 6's first byte is the length's high byte");
        /* Both happen to be 0x00 for a length under 16 MB, so the FIRST byte cannot
         * tell them apart -- which is exactly why the rectangle's encoding field, and
         * nothing in the rectangle's payload, is what a client dispatches on. */
        eq_int(t[1], 0xC0, "Tight's second byte is the compact length's low 7 bits");
        eq_int(h[1], 0x00, "encoding 6's second byte is still the length");
    }
}

/* The conversion only ever runs because a client asked for something other than
 * what we advertised, so the cases that matter are the ones a client actually sends.
 * Apple's Screen Sharing asks for 32 bpp depth 24 little-endian with 8-bit channels
 * -- BGRA in memory -- and the first three checks below are that format. The rest
 * pin the two ways it can be wrong in silence: a channel that never reaches its
 * maximum (the picture is dark, and looks like a capture problem) and a channel in
 * the wrong byte (red and blue swapped, which looks like a byte-order bug in the
 * panel rather than here). */
static void test_conv(void)
{
    struct vnc_pixel_format pf;
    struct vnc_conv cv;
    uint16_t src[4];
    uint8_t dst[16];

    /* 32bpp BGRA: red at 16, green at 8, blue at 0, all 255. */
    memset(&pf, 0, sizeof pf);
    pf.bits_per_pixel = 32; pf.depth = 24; pf.true_colour = 1;
    pf.red_max = pf.green_max = pf.blue_max = 255;
    pf.red_shift = 16; pf.green_shift = 8; pf.blue_shift = 0;

    eq_int(vnc_conv_init(&cv, &pf), 0, "32bpp BGRA is a format we can produce");

    src[0] = 0xFFFF;            /* white */
    src[1] = 0xF800;            /* pure red   (r=31 g=0 b=0) */
    src[2] = 0x07E0;            /* pure green (r=0  g=63 b=0) */
    src[3] = 0x001F;            /* pure blue  (r=0  g=0  b=31) */
    vnc_conv_rows(dst, src, 4, &cv);

    {
        static const uint8_t want[16] = {
            0xFF, 0xFF, 0xFF, 0x00,
            0x00, 0x00, 0xFF, 0x00,
            0x00, 0xFF, 0x00, 0x00,
            0xFF, 0x00, 0x00, 0x00
        };
        eq_mem(dst, want, 16, "white/red/green/blue convert to BGRA at full range");
    }
    /* The dark-picture trap: 31 of 31 must reach 255, not 248. */
    eq_int(dst[6], 0xFF, "the reddest red reaches the client's red_max, not 248");

    /* 16bpp big-endian RGB565 with a different channel order: the same format as
     * fb0 but byte-swapped, which a big-endian client would ask for. */
    memset(&pf, 0, sizeof pf);
    pf.bits_per_pixel = 16; pf.depth = 16; pf.true_colour = 1; pf.big_endian = 1;
    pf.red_max = 31; pf.green_max = 63; pf.blue_max = 31;
    pf.red_shift = 11; pf.green_shift = 5; pf.blue_shift = 0;

    eq_int(vnc_conv_init(&cv, &pf), 0, "16bpp RGB565 big-endian is producible");
    src[0] = 0xF800;
    vnc_conv_rows(dst, src, 1, &cv);
    {
        static const uint8_t want[2] = { 0xF8, 0x00 };
        eq_mem(dst, want, 2, "big-endian 16bpp puts the high byte first");
    }

    /* 8bpp of a 3-3-2 kind: three channels sharing one byte, scaled to 7/7/3. */
    memset(&pf, 0, sizeof pf);
    pf.bits_per_pixel = 8; pf.depth = 8; pf.true_colour = 1;
    pf.red_max = 7; pf.green_max = 7; pf.blue_max = 3;
    pf.red_shift = 5; pf.green_shift = 2; pf.blue_shift = 0;
    eq_int(vnc_conv_init(&cv, &pf), 0, "8bpp 3-3-2 is producible");
    src[0] = 0xFFFF;
    vnc_conv_rows(dst, src, 1, &cv);
    eq_int(dst[0], 0xFF, "8bpp white fills every bit");

    /* The refusals. A colour-mapped client has not asked for our pixels at all;
     * a 4bpp one has no packing here; and 16bpp with 8-bit channels would drop the
     * top of every value if it were answered rather than refused. */
    memset(&pf, 0, sizeof pf);
    pf.true_colour = 0; pf.bits_per_pixel = 32;
    eq_int(vnc_conv_init(&cv, &pf), -1, "a colour-mapped format is refused");

    memset(&pf, 0, sizeof pf);
    pf.true_colour = 1; pf.bits_per_pixel = 4;
    eq_int(vnc_conv_init(&cv, &pf), -1, "4bpp is refused, not rounded to nothing");

    memset(&pf, 0, sizeof pf);
    pf.bits_per_pixel = 16; pf.depth = 24; pf.true_colour = 1;
    pf.red_max = pf.green_max = pf.blue_max = 255;
    pf.red_shift = 16; pf.green_shift = 8; pf.blue_shift = 0;
    eq_int(vnc_conv_init(&cv, &pf), -1, "16bpp carrying 8-bit channels is refused");

    /* Zero pixels is a legal call -- a fully clipped plane produces one -- and must
     * not write anything. */
    eq_int(vnc_conv_init(&cv, &vnc_pf_rgb565), 0, "our own format converts trivially");
    dst[0] = 0x5A;
    vnc_conv_rows(dst, src, 0, &cv);
    eq_int(dst[0], 0x5A, "converting no pixels writes no pixels");
    src[0] = 0x0000;
    vnc_conv_rows(dst, src, 1, &cv);
    eq_int(dst[0], 0x00, "RGB565 to RGB565 little-endian is the low byte first");
}

int main(void)
{
    printf("test_vnc_rfb\n");
    test_version();
    test_encoding_names();
    test_pseudo_names();
    test_quality_predicate();
    test_security_result_rule();
    test_pixel_format();
    test_compact_len();
    test_jpeg_header();
    test_tight_zlib_header();
    test_zlib_rect_header();
    test_conv();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
