/*
 * test_audio.c — the audio output path's byte-level contract, with no card.
 *
 * There is a DDJ-FLX4 on the other end of this and no way to attach it from here,
 * so the parts of the audio path that *are* checkable are pinned hard: the exact
 * bytes s24pack() produces for each format, the sign of small negatives, the
 * extremes, and the refusal to write when the destination cannot hold the result.
 *
 * The one that matters most is `test_packing_and_the_extension` at the bottom, and
 * it is worth reading before trusting the rest. rbp's samples are right-justified
 * 24-bit values in a 32-bit word with the top byte zero, so the raw word for -1 is
 * 0x00ffffff — a *positive* number. That does not, as it first appears, mean the
 * packer mis-encodes it: 24-bit two's complement -1 *is* 0xffffff, so the packed
 * formats read bits 23..0 and come out right either way. The extension is
 * load-bearing in the *mixing arithmetic* (0x00ffffff * 0.5 is full-scale positive
 * where -1 * 0.5 is silence) and in the S24_LE container, which copies the top byte
 * too. The test pins both sides of that distinction rather than asserting a rule
 * that is only true of some formats.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "s24pack.h"

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

/* The mapping rbp's samples need, written without the signed right shift so the
 * test does not agree with the implementation by construction: if a compiler's
 * >> were logical rather than arithmetic, both would be wrong together and only
 * a test that spells the intent out independently can catch it. */
static int32_t sext24(int32_t raw)
{
    uint32_t v = (uint32_t)raw & 0x00ffffffu;
    return (v & 0x800000u) ? (int32_t)(v | 0xff000000u) : (int32_t)v;
}

static void test_parse_and_sizes(void)
{
    CHECK(s24pack_bytes(AUDIO_FMT_S24_LE) == 4, "S24_LE is a 4-byte container");
    CHECK(s24pack_bytes(AUDIO_FMT_S24_3LE) == 3, "S24_3LE is packed to 3 bytes");
    CHECK(s24pack_bytes(AUDIO_FMT_S16_LE) == 2, "S16_LE is 2 bytes");
    CHECK(s24pack_bytes(99) == 0, "an unknown format has no size");

    /* NULL and "" are the default, not an error: start-rb.sh exports AUDIO_FMT=""
     * when rb.conf does not set it, so an error here would abort every default
     * run. */
    CHECK(s24pack_parse(NULL) == AUDIO_FMT_S24_LE, "NULL format is not the default");
    CHECK(s24pack_parse("") == AUDIO_FMT_S24_LE, "empty format is not the default");
    CHECK(s24pack_parse("s24_le") == AUDIO_FMT_S24_LE, "s24_le not recognised");
    CHECK(s24pack_parse("S24_LE") == AUDIO_FMT_S24_LE, "format names are not case-insensitive");
    CHECK(s24pack_parse("s24le") == AUDIO_FMT_S24_LE, "s24le not recognised");
    CHECK(s24pack_parse("s24_3le") == AUDIO_FMT_S24_3LE, "s24_3le not recognised");
    CHECK(s24pack_parse("S24_3LE") == AUDIO_FMT_S24_3LE, "S24_3LE (upper) not recognised");
    CHECK(s24pack_parse("s24_3") == AUDIO_FMT_S24_3LE, "s24_3 not recognised");
    CHECK(s24pack_parse("s16_le") == AUDIO_FMT_S16_LE, "s16_le not recognised");
    CHECK(s24pack_parse("S16") == AUDIO_FMT_S16_LE, "S16 not recognised");
    /* A name we cannot pack must be reported, not silently replaced: the caller
     * logs the name it did not understand. */
    CHECK(s24pack_parse("s32_le") == -1, "an unknown format name was accepted");
    CHECK(s24pack_parse("flac") == -1, "a nonsense format name was accepted");

    /* Whatever format is chosen, the worst-case sizing must cover it — that is
     * the invariant the output scratch buffer's size rests on. */
    CHECK(s24pack_worst(1024, 8) == 1024 * 8 * 4, "worst-case size is not 4 bytes/sample");
    for (int fmt = 0; fmt < 3; fmt++)
        CHECK((unsigned)s24pack_bytes(fmt) <= sizeof(int32_t),
              "format %d needs more than the worst-case allowance", fmt);
}

static void test_s24_le_identity(void)
{
    int32_t in[4] = { -1, 0x123456, (int32_t)0xff800000, 0 };
    unsigned char out[16];

    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_S24_LE, in, 2, 2, out, sizeof out) == 16,
          "S24_LE did not write 16 bytes for 2x2");
    CHECK(memcmp(out, in, sizeof out) == 0,
          "S24_LE is not the identity copy; it must hand rbp's own words over");
}

static void test_s24_3le(void)
{
    /* ch=1 so sample i lands at byte 3i and the expected bytes can be written
     * out by hand rather than computed. */
    int32_t in[4] = { 1, -1, 0x7fffff, -0x800000 };
    unsigned char out[12];
    static const unsigned char want[12] = {
        0x01, 0x00, 0x00,   /* +1                  */
        0xff, 0xff, 0xff,   /* -1                  */
        0xff, 0xff, 0x7f,   /* +8388607 (full scale)*/
        0x00, 0x00, 0x80,   /* -8388608 (negative full scale) */
    };

    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_S24_3LE, in, 4, 1, out, sizeof out) == 12,
          "S24_3LE did not write 12 bytes for 4 mono frames");
    CHECK(memcmp(out, want, sizeof want) == 0,
          "S24_3LE bytes differ: got %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x",
          out[0], out[1], out[2], out[3], out[4], out[5],
          out[6], out[7], out[8], out[9], out[10], out[11]);
}

static void test_s16_le(void)
{
    int32_t in[4] = { 1, -1, 0x7fffff, -0x800000 };
    unsigned char out[8];
    static const unsigned char want[8] = {
        0x00, 0x00,   /* +1, truncated to 0 */
        0xff, 0xff,   /* -1                 */
        0xff, 0x7f,   /* +32767             */
        0x00, 0x80,   /* -32768             */
    };

    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_S16_LE, in, 4, 1, out, sizeof out) == 8,
          "S16_LE did not write 8 bytes for 4 mono frames");
    CHECK(memcmp(out, want, sizeof want) == 0,
          "S16_LE bytes differ: got %02x%02x %02x%02x %02x%02x %02x%02x",
          out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
}

static void test_interleave(void)
{
    /* Two frames of stereo, packed to 3 bytes: the channel order within a frame
     * must survive, because a swapped pair puts the master on the headphone jack
     * and is otherwise indistinguishable from a wiring fault at the other end. */
    int32_t in[4] = { 0x010203, 0x040506, 0x070809, 0x0a0b0c };
    unsigned char out[16];   /* two frames of stereo is 16 bytes in S24_LE */
    static const unsigned char want[12] = {
        0x03, 0x02, 0x01,   /* frame 0 left   */
        0x06, 0x05, 0x04,   /* frame 0 right  */
        0x09, 0x08, 0x07,   /* frame 1 left   */
        0x0c, 0x0b, 0x0a,   /* frame 1 right  */
    };

    unsigned written;

    memset(out, 0xAA, sizeof out);
    written = s24pack(AUDIO_FMT_S24_3LE, in, 2, 2, out, sizeof out);
    CHECK(written == 12, "interleaved S24_3LE wrote %u bytes, expected 12", written);
    CHECK(memcmp(out, want, sizeof want) == 0, "channel/frame interleave was reordered");

    /* The same test in the identity format: bytes for frame 0 come before frame 1,
     * left before right. */
    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_S24_LE, in, 2, 2, out, sizeof out) == 16,
          "interleaved S24_LE wrote the wrong byte count");
    CHECK(memcmp(out, in, 16) == 0, "S24_LE reordered the interleave");
}

static void test_refusals(void)
{
    /* Four mono frames to S24_3LE needs 12 bytes; give it 11 and require that
     * nothing at all is written. A partial pack would hand the device a truncated
     * block, which is worse than handing it nothing. */
    int32_t in[4] = { 1, 2, 3, 4 };
    unsigned char out[16];
    unsigned i, touched = 0;

    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_S24_3LE, in, 4, 1, out, 11) == 0,
          "S24_3LE packed into a destination one byte too small");
    for (i = 0; i < sizeof out; i++)
        if (out[i] != 0xAA)
            touched++;
    /* One check, not sixteen: the property is "it wrote nothing", and reporting
     * it per byte would drown the count that the summary line prints. */
    CHECK(touched == 0, "%u bytes were written into an over-small destination", touched);

    CHECK(s24pack(AUDIO_FMT_S24_3LE, in, 4, 1, out, 12) == 12,
          "S24_3LE refused a destination that is exactly big enough");
    CHECK(s24pack(99, in, 4, 1, out, sizeof out) == 0,
          "an unknown format wrote something");
    CHECK(s24pack(AUDIO_FMT_S24_LE, in, 0, 2, out, sizeof out) == 0,
          "an empty block was reported as a successful write");
}

/* --- what the packer does, and does not, depend on -------------------------- */

static void test_packing_and_the_extension(void)
{
    /* rbp's raw word for a sample of -1 is 0x00ffffff — the 24-bit value in the
     * low three bytes with the top byte zero, which as a 32-bit integer is the
     * *positive* 16777215.
     *
     * The packed formats read only bits 23..0, i.e. the three bytes rbp wrote, so
     * that missing sign-extension makes no difference to their output. Worth
     * pinning, because it is the reason a level that is wrong can never be the
     * packer's doing, and the search for it belongs in the arithmetic instead:
     * 0x00ffffff * 0.5 is full-scale positive, where -1 * 0.5 is silence.
     *
     * The first version of this test asserted the opposite — that the raw word
     * would pack to ff ff 00 and the extended one to ff ff ff — and was wrong:
     * 24-bit two's complement -1 *is* 0xffffff, so both pack to the same three
     * bytes. Packing is modular, and the extension only changes bits 31..24. */
    static const int packed[2] = { AUDIO_FMT_S24_3LE, AUDIO_FMT_S16_LE };
    int32_t raw = 0x00ffffff;         /* rbp's -1, unextended */
    int32_t ext = sext24(raw);        /* the same sample, sign-extended */
    unsigned char a[4], b[4];
    int i;

    CHECK(ext == -1, "sext24(0x00ffffff) is %d, expected -1", ext);

    for (i = 0; i < 2; i++) {
        unsigned na, nb;

        memset(a, 0xAA, sizeof a);
        memset(b, 0xAA, sizeof b);
        na = s24pack(packed[i], &raw, 1, 1, a, sizeof a);
        nb = s24pack(packed[i], &ext, 1, 1, b, sizeof b);
        CHECK(na == nb && na > 0,
              "format %d packed %u bytes for the raw word and %u for the extended one",
              packed[i], na, nb);
        CHECK(na > 0 && memcmp(a, b, na) == 0,
              "format %d packed the raw and the extended -1 differently; it reads no "
              "bit above 23, so this must not be able to happen", packed[i]);
    }

    /* S24_LE is the exception and deliberately so: it copies the whole 4-byte
     * word, top byte included, so the extension *is* visible in its output. That
     * is the contract the default path rests on — audioshim.c fills its buffer
     * with sign-extended values, so the card receives the extended form, which is
     * what the SC Live 4's hardware was given. Hence both words must pass through
     * byte-for-byte, and the two top bytes must differ, which is what makes the
     * extension observable at all. */
    memset(b, 0xAA, sizeof b);
    CHECK(s24pack(AUDIO_FMT_S24_LE, &raw, 1, 1, b, sizeof b) == 4, "S24_LE raw failed");
    CHECK(memcmp(b, &raw, 4) == 0, "S24_LE did not copy the raw word verbatim");
    CHECK(b[3] == 0x00, "the raw word's top byte is %02x, not 0 — the premise of "
          "every format in this file", b[3]);

    memset(b, 0xAA, sizeof b);
    CHECK(s24pack(AUDIO_FMT_S24_LE, &ext, 1, 1, b, sizeof b) == 4, "S24_LE ext failed");
    CHECK(memcmp(b, &ext, 4) == 0, "S24_LE did not copy the extended word verbatim");
    CHECK(b[3] == 0xff, "the extended -1's top byte is %02x, not ff", b[3]);

    /* And the bytes for -1 in every format, so the value is pinned as well as the
     * insensitivity: full-scale negative must not come out as anything else. */
    memset(a, 0xAA, sizeof a);
    CHECK(s24pack(AUDIO_FMT_S24_3LE, &ext, 1, 1, a, sizeof a) == 3, "S24_3LE -1 failed");
    CHECK(a[0] == 0xff && a[1] == 0xff && a[2] == 0xff,
          "S24_3LE packed -1 as %02x %02x %02x, expected ff ff ff", a[0], a[1], a[2]);
    memset(a, 0xAA, sizeof a);
    CHECK(s24pack(AUDIO_FMT_S16_LE, &ext, 1, 1, a, sizeof a) == 2, "S16_LE -1 failed");
    CHECK(a[0] == 0xff && a[1] == 0xff,
          "S16_LE packed -1 as %02x %02x, expected ff ff", a[0], a[1]);
}

int main(void)
{
    test_parse_and_sizes();
    test_s24_le_identity();
    test_s24_3le();
    test_s16_le();
    test_interleave();
    test_refusals();
    test_packing_and_the_extension();

    printf("test_audio: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
