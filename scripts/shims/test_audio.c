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
 * The `test_mirror_*` cases at the bottom pin the HDMI mirror's decisions
 * (mirror_policy.c), which are the half of that feature which is arithmetic. The
 * half that is not — the fill inside the shim's per-frame loop — cannot be reached
 * from here at all, and is covered by the drill in docs/13-raspberrypi4.md.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "s24pack.h"
#include "mirror_policy.h"

#include <errno.h>
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

static void test_subframe_le(void)
{
    /* The mirror's format on the vc4 HDMI PCMs: a real IEC958 subframe, so the
     * sample lives at bits 4..27 and bit 31 is the parity of bits 4..30. ch=1, so
     * sample i is at byte 4i and the expected words can be written by hand.
     *
     * This test used to pin the OTHER reading — the sample at bits 8..31, which is
     * what "24-bit sample left-justified in a 32-bit word" sounds like — and said
     * so, noting that a nibble shift would tell the two apart. It was the wrong
     * one, and the ears row (S4.6) is how that was found: the sink read bits 4..27,
     * got the sample's low 20 bits promoted, and played a wrapped version of the
     * waveform, loud and very distorted, with every counter in the log perfect.
     * The expected words below are therefore alsa-lib's encoder's output, and the
     * round trip at the end checks them against alsa-lib's *decoder* instead of
     * against this file's own arithmetic. */
    int32_t in[4] = { 1, -1, 0x7fffff, -0x800000 };
    unsigned char out[16], packed[12];
    static const unsigned char want[16] = {
        0x10, 0x00, 0x00, 0x80,   /* +1:               1 bit set  -> odd -> parity set    */
        0xf0, 0xff, 0xff, 0x0f,   /* -1:               24 bits    -> even -> parity clear */
        0xf0, 0xff, 0xff, 0x87,   /* +8388607:         23 bits    -> odd  -> parity set   */
        0x00, 0x00, 0x00, 0x88,   /* -8388608:         1 bit      -> odd  -> parity set   */
    };
    unsigned i;

    memset(out, 0xAA, sizeof out);
    CHECK(s24pack(AUDIO_FMT_SUBFRAME_LE, in, 4, 1, out, sizeof out) == 16,
          "SUBFRAME_LE did not write 16 bytes for 4 mono frames");
    (void)s24pack(AUDIO_FMT_S24_3LE, in, 4, 1, packed, sizeof packed);
    CHECK(memcmp(out, want, sizeof want) == 0,
          "SUBFRAME_LE bytes differ: got %02x%02x%02x%02x %02x%02x%02x%02x "
          "%02x%02x%02x%02x %02x%02x%02x%02x",
          out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7],
          out[8], out[9], out[10], out[11], out[12], out[13], out[14], out[15]);

    for (i = 0; i < 4; i++) {
        uint32_t w = (uint32_t)out[i * 4 + 0] |
                     ((uint32_t)out[i * 4 + 1] << 8) |
                     ((uint32_t)out[i * 4 + 2] << 16) |
                     ((uint32_t)out[i * 4 + 3] << 24);
        uint32_t dec = (w & 0x0ffffff0U) >> 4;

        /* alsa-lib's decoder, verbatim: `data &= ~0xf; data <<= 4;`. It has to
         * recover the sample left-justified in 32 bits, which is the sample
         * scaled by 256 — the same value the encoder was handed, shifted. If the
         * packer and alsa-lib disagreed by a nibble this is what fails. */
        CHECK((w & ~0xfU) << 4 == (uint32_t)(in[i] << 8),
              "alsa-lib's IEC958 decoder does not recover sample %u: got %08x",
              i, (w & ~0xfU) << 4);

        /* Both output formats must carry the same audio. The subframe's bits
         * 4..27 shifted back down are exactly the three bytes S24_3LE writes for
         * the same sample; the old version of this test asserted the byte-level
         * relation instead, which was the wrong reading's fingerprint and passed
         * for as long as the bug lived. */
        CHECK((dec & 0xff) == packed[i * 3 + 0] &&
              ((dec >> 8) & 0xff) == packed[i * 3 + 1] &&
              ((dec >> 16) & 0xff) == packed[i * 3 + 2],
              "SUBFRAME_LE and S24_3LE disagree about sample %u", i);

        /* Even parity, checked the way a receiver checks it: over bits 4..31 of the
         * word as it arrives, parity bit included, so the whole thing has an even
         * number of set bits. Masking bit 31 off (as this check first did) throws
         * away the very bit the packer writes and then complains that the 4..30
         * sum is odd on any sample with an odd count — which is the packer being
         * *right*, so the mask, not the packer, was the bug. Note the two masks
         * are a nibble apart: 0xf0 is bits 4..31, 0x70 is 4..30. */
        CHECK(__builtin_parity(w & 0xfffffff0U) == 0,
              "sample %u's subframe has odd parity", i);

        /* Bits 0..3 are the preamble and 28..30 validity/user/status: neither may
         * carry audio. A zero preamble is a decision (s24pack.h), but audio
         * leaking into the status nibble is the defect this whole test is about,
         * so pin that it cannot. */
        CHECK((w & 0xfU) == 0, "sample %u put something in the preamble", i);
        CHECK((w & 0x70000000U) == 0,
              "sample %u set a validity/user/channel-status bit", i);
    }

    CHECK(s24pack_bytes(AUDIO_FMT_SUBFRAME_LE) == 4,
          "SUBFRAME_LE's sample is not 4 bytes wide");
    CHECK(s24pack_parse("subframe_le") == AUDIO_FMT_SUBFRAME_LE,
          "subframe_le did not parse to the subframe format");
    CHECK(s24pack_parse("iec958_subframe_le") == AUDIO_FMT_SUBFRAME_LE,
          "the device's own name for the format did not parse");
    CHECK(s24pack_worst(4, 2) >= 16,
          "the worst-case buffer is smaller than one 4-byte block");
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

/*
 * The HDMI mirror's policy, which is the half of the mirror that can be checked
 * without a sink plugged into anything.
 *
 * These verdicts are not style: each one is a way the mirror could report itself
 * healthy while emitting nothing, or give up on a sink that was only busy. The
 * two the header calls load-bearing are pinned first.
 */
static void test_mirror_verdict(void)
{
    /* Nothing consumed. A non-blocking writei returns 0 when the ring is full,
     * and counting that as a write is precisely how "up and silently dead" gets
     * logged as working. */
    CHECK(mirror_verdict(0, 64) == MIRROR_FULL, "0 frames wrote is not a full ring");

    CHECK(mirror_verdict(64, 64) == MIRROR_WROTE, "a whole block is not a write");
    /* Some of it went out. Not an error, and not a full ring either: the tail of
     * the block is lost and must be counted as short, not as a write. */
    CHECK(mirror_verdict(32, 64) == MIRROR_SHORT, "a partial write is not short");
    CHECK(mirror_verdict(1, 64) == MIRROR_SHORT, "one frame is not short");

    CHECK(mirror_verdict(-EAGAIN, 64) == MIRROR_FULL, "-EAGAIN is not a full ring");
    CHECK(mirror_verdict(-EWOULDBLOCK, 64) == MIRROR_FULL,
          "-EWOULDBLOCK is not a full ring");

    /* A broken stream on a device that is still there: worth one prepare. */
    CHECK(mirror_verdict(-EPIPE, 64) == MIRROR_RETRY, "-EPIPE is not worth a retry");
    CHECK(mirror_verdict(-ESTRPIPE, 64) == MIRROR_RETRY,
          "-ESTRPIPE is not worth a retry");
    CHECK(mirror_verdict(-EBADFD, 64) == MIRROR_RETRY, "-EBADFD is not worth a retry");

    /* Busy is not full. Classing it as full keeps the handle open forever,
     * un-counted and un-recovered, in a log that reads as healthy. */
    CHECK(mirror_verdict(-EBUSY, 64) == MIRROR_DOWN, "-EBUSY is not a down device");
    CHECK(mirror_verdict(-ENODEV, 64) == MIRROR_DOWN, "-ENODEV is not a down device");
    CHECK(mirror_verdict(-ENXIO, 64) == MIRROR_DOWN, "-ENXIO is not a down device");
    CHECK(mirror_verdict(-EIO, 64) == MIRROR_DOWN, "-EIO is not a down device");
    CHECK(mirror_verdict(-EINVAL, 64) == MIRROR_DOWN, "-EINVAL is not a down device");
    CHECK(mirror_verdict(-9999, 64) == MIRROR_DOWN, "an unknown errno is not a loss");
    CHECK(mirror_verdict(128, 64) == MIRROR_DOWN,
          "writing more frames than were asked for is not a write");

    /* Every verdict has a name, because every one of them reaches the log. */
    static const char *names[] = { "wrote", "short", "full", "retry", "down" };
    for (int v = 0; v <= 4; v++)
        CHECK(strcmp(mirror_verdict_name((enum mirror_verdict)v), names[v]) == 0,
              "verdict %d has the wrong log name", v);
}

static void test_mirror_candidates(void)
{
    char got[MIRROR_CAND_MAX][MIRROR_NAME_MAX];
    int dropped, n;

    /* Absent is off, and off is not an error: this is how RB_AUDIO_MIRROR_DEV=""
     * turns the whole feature off. */
    dropped = -1;
    CHECK(mirror_candidates(NULL, got, MIRROR_CAND_MAX, &dropped) == 0,
          "a NULL list is not empty");
    CHECK(dropped == 0, "a NULL list dropped something");
    CHECK(mirror_candidates("", got, MIRROR_CAND_MAX, &dropped) == 0,
          "an empty list is not empty");
    CHECK(mirror_candidates("  \t \n ", got, MIRROR_CAND_MAX, &dropped) == 0,
          "a whitespace-only list is not empty");
    CHECK(dropped == 0, "a whitespace-only list dropped something");

    /* A lone hw: entry yields itself and its plug twin, in that order. */
    n = mirror_candidates("hw:CARD=vc4hdmi0,DEV=0", got, MIRROR_CAND_MAX, &dropped);
    CHECK(n == 2, "a hw: entry produced %d candidates, not 2", n);
    CHECK(n == 2 && strcmp(got[0], "hw:CARD=vc4hdmi0,DEV=0") == 0,
          "the hw: form is not tried first");
    CHECK(n == 2 && strcmp(got[1], "plughw:CARD=vc4hdmi0,DEV=0") == 0,
          "the plug fallback is not tried second");
    CHECK(dropped == 0, "a plain entry dropped something");

    /* A twin of a twin is the same device twice, so only hw: gets one. */
    n = mirror_candidates("plughw:CARD=vc4hdmi0,DEV=0", got, MIRROR_CAND_MAX, &dropped);
    CHECK(n == 1, "a plughw: entry produced %d candidates, not 1", n);
    CHECK(n == 1 && strcmp(got[0], "plughw:CARD=vc4hdmi0,DEV=0") == 0,
          "a plughw: entry was not kept verbatim");

    /* The shipped default: both HDMI ports, in order, each with its twin. The
     * second port matters because moving the cable is a thing an operator does. */
    n = mirror_candidates("hw:CARD=vc4hdmi0,DEV=0 hw:CARD=vc4hdmi1,DEV=0",
                          got, MIRROR_CAND_MAX, &dropped);
    CHECK(n == 4, "both cards produced %d candidates, not 4", n);
    CHECK(n == 4 && strcmp(got[2], "hw:CARD=vc4hdmi1,DEV=0") == 0,
          "the second card is not third, so the walk order is wrong");
    CHECK(n == 4 && strcmp(got[3], "plughw:CARD=vc4hdmi1,DEV=0") == 0,
          "the second card's twin is wrong");

    /* Separators: any run of whitespace, and a trailing one is not an entry. */
    n = mirror_candidates("  hw:CARD=a,DEV=0\t\thw:CARD=b,DEV=1 \n", got,
                          MIRROR_CAND_MAX, &dropped);
    CHECK(n == 4, "mixed whitespace produced %d candidates, not 4", n);

    /* A repeat is one attempt, not two. This is also what keeps an operator who
     * writes both forms by hand from trying the same device twice. */
    n = mirror_candidates("hw:CARD=a,DEV=0 hw:CARD=a,DEV=0", got, MIRROR_CAND_MAX,
                          &dropped);
    CHECK(n == 2, "a repeated entry produced %d candidates, not 2", n);
    CHECK(dropped == 2, "a repeated entry dropped %d, not 2", dropped);
    n = mirror_candidates("hw:CARD=a,DEV=0 plughw:CARD=a,DEV=0", got,
                          MIRROR_CAND_MAX, &dropped);
    CHECK(n == 2, "both spellings of one device produced %d candidates, not 2", n);

    /* A name too long for the buffer is dropped and counted, never truncated: a
     * truncated device name is a name that does not exist, and the open failure
     * would read as "the sink refused us". */
    n = mirror_candidates("hw:CARD=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                          "aaaaaaaaaaaaaaaaaaaaaaaa,DEV=0", got, MIRROR_CAND_MAX,
                          &dropped);
    CHECK(n == 0, "an over-long name produced %d candidates", n);
    CHECK(dropped == 1, "an over-long name dropped %d, not 1", dropped);

    /* A list longer than the array is truncated and counted. */
    n = mirror_candidates("a b c d e f g h i j k", got, 4, &dropped);
    CHECK(n == 4, "a long list stored %d, not 4", n);
    CHECK(dropped == 7, "a long list dropped %d, not 7", dropped);
    /* and with nowhere to put anything, it still counts rather than writing. */
    n = mirror_candidates("hw:CARD=a,DEV=0 b", got, 0, &dropped);
    CHECK(n == 0, "a zero-capacity list stored %d", n);
    CHECK(dropped == 3, "a zero-capacity list dropped %d, not 3", dropped);
}

/* Count the retries a down mirror makes over `ticks` milliseconds of audio, one
 * tick being one candidate instant to retry at. The shim's floor (500,
 * MIRROR_REOPEN_MIN_MS) and the default ceiling (5000, RB_AUDIO_MIRROR_REOPEN_MS)
 * are literals here because audioshim.o is deliberately not linkable.
 *
 * `restamp` is the caller's behaviour and the whole point of the exercise: 1 is
 * what the shim does (the stamp moves to the last attempt), 0 is the bug it had
 * (the stamp stays on the loss and never moves again). The predicate cannot tell
 * the two apart — only a caller can — so the caller is what is modelled. */
static int mirror_count_attempts(int ticks, int restamp)
{
    /* Down since the first tick: the loss stamped it, once. */
    unsigned long long last = 1;
    unsigned long backoff = 500;
    int attempts = 0, t;

    for (t = 0; t < ticks; t++) {
        if (!mirror_reopen_due((unsigned long long)t, last, backoff, 5000))
            continue;
        if (restamp)
            last = (unsigned long long)t;
        backoff = mirror_next_backoff(backoff, 5000);
        attempts++;
    }
    return attempts;
}

static void test_mirror_backoff(void)
{
    /* A mirror that has never gone down is never due, however long the process
     * has run. down_since 0 is that state, and now_ms() must not be able to
     * reach it. */
    CHECK(mirror_reopen_due(100000, 0, 500, 5000) == 0,
          "a mirror that never went down is due for a retry");
    /* The first attempt waits its floor. */
    CHECK(mirror_reopen_due(1000, 900, 500, 5000) == 0,
          "a retry fired 100ms into a 500ms floor");
    CHECK(mirror_reopen_due(1400, 900, 500, 5000) == 1,
          "a retry did not fire at the floor");
    /* RB_AUDIO_MIRROR_REOPEN_MS=0 is "never retry", which is a different thing
     * from a backoff of 0 — that would retry on every single block. */
    CHECK(mirror_reopen_due(99999, 900, 500, 0) == 0,
          "retries fired with the interval set to 0");
    /* A stamp from the future is a clock problem, not a due retry. */
    CHECK(mirror_reopen_due(100, 900, 500, 5000) == 0,
          "a stamp ahead of now fired a retry");

    /* Double to the ceiling and stop there: the ceiling is what bounds the cost
     * of a retry on rbp's audio thread. */
    CHECK(mirror_next_backoff(500, 5000) == 1000, "the backoff did not double");
    CHECK(mirror_next_backoff(1000, 5000) == 2000, "the backoff did not double");
    CHECK(mirror_next_backoff(4000, 5000) == 5000, "the backoff passed its ceiling");
    CHECK(mirror_next_backoff(5000, 5000) == 5000, "the backoff grew past its ceiling");
    CHECK(mirror_next_backoff(0, 5000) == 5000, "a zero backoff did not take the ceiling");
    CHECK(mirror_next_backoff(500, 0) == 0, "a disabled interval produced a backoff");
    /* The ceiling is not required to be a power-of-two multiple of the floor, and
     * a doubling that passed it must not wrap. */
    CHECK(mirror_next_backoff(700, 900) == 900, "the backoff overshot a non-multiple");
    CHECK(mirror_next_backoff(600, 700) == 700, "the backoff overshot");

    /* And now the loop rather than the predicate — because every check above passes
     * with a caller that stamps the outage once and never again, and that caller is
     * the defect this unit actually measured. 60 s of audio at ~1.45 ms per
     * 64-frame block, so 60,000 ticks is about a minute of playback.
     *
     * A restamping caller gets 500 ms, 1 s, 2 s, 4 s, then the 5 s ceiling: a
     * dozen-odd attempts in a minute, and the cost of each one on rbp's audio
     * thread stays bounded. */
    CHECK(mirror_count_attempts(60000, 1) <= 20,
          "a restamping caller retried far more often than the backoff allows");
    CHECK(mirror_count_attempts(60000, 1) >= 10,
          "the backoff stopped retrying altogether");
    /* The frozen stamp, pinned so the reason this test is a loop is on the record:
     * the predicate still answers correctly at every call, and the retry still runs
     * on essentially every tick once the interval has elapsed — measured on the
     * unit as ~2000 opens and closes a second, 121k log lines in 28 s, from a
     * configuration whose floor and ceiling were 500 ms and 5 s. */
    CHECK(mirror_count_attempts(60000, 0) > 10000,
          "the frozen-stamp caller this test exists to catch did not flood");
}

/* A minute of the mirror's ring, in the shim's own units: one block of 64 frames
 * per tick, the sink draining 10 milli-frames per block more than the source feeds.
 * That 10/1000 of a frame is not a made-up number — it is the drift this unit
 * measured, 6.9 frames a second at ~689 blocks a second (docs/13-raspberrypi4.md,
 * the S4.7 row), and it is the whole reason mirror_pad() exists.
 *
 * `use_pad` 0 is the mirror as it was before the correction: the prefill, then
 * nothing. Returns the lowest level seen, and reports the pads and whether the ring
 * ever reached empty — which is what an XRUN is, and what a stream restart follows.
 * The level starts at the prefill (half the ring), like the real one does. */
static long mirror_run_ring(int blocks, int use_pad, unsigned long *pads, int *starved)
{
    const long buf = 1024;
    long level = buf / 2;
    long debt = 0, lowest = level;
    int t;

    *pads = 0;
    *starved = 0;
    for (t = 0; t < blocks; t++) {
        level += 64;                 /* the master's block arrives */
        debt += 10;                  /* the sink's extra hundredth of a frame */
        level -= 64 + debt / 1000;
        debt %= 1000;
        if (use_pad) {
            unsigned long p = mirror_pad_frames(buf - level, (unsigned long)buf,
                                                (unsigned long)buf / 2, 64);
            if (p)
                (*pads)++;
            level += (long)p;
        } else if (level > buf) {
            level = buf;             /* no pad, no drop: the ring simply holds it */
        }
        if (level < 0) {
            *starved = 1;
            level = 0;               /* the stream would have been restarted here */
        }
        if (level < lowest)
            lowest = level;
    }
    return lowest;
}

static void test_mirror_pad(void)
{
    unsigned long pads;
    int starved;

    /* The decision, case by case. `avail` is free space, so a pad appears when there
     * is MORE room than the target implies — the ring is emptying. A ring *above* its
     * target is therefore one with LESS avail than half the buffer (324 here is 700
     * frames held), and it gets nothing: the drop side is the corrector for that. */
    CHECK(mirror_pad_frames(512, 1024, 512, 64) == 0, "a ring at its target was padded");
    CHECK(mirror_pad_frames(324, 1024, 512, 64) == 0, "a ring above its target was padded");
    CHECK(mirror_pad_frames(513, 1024, 512, 64) == 1, "a one-frame deficit did not pad by one");
    CHECK(mirror_pad_frames(520, 1024, 512, 64) == 8, "a deficit was padded by the wrong amount");
    CHECK(mirror_pad_frames(1024, 1024, 512, 64) == 64, "an empty ring was not topped up at all");
    CHECK(mirror_pad_frames(1000, 1024, 512, 8) == 8, "one correction passed its ceiling");
    /* Readings that are not a level — an unresolved symbol, a broken stream, a device
     * that granted no geometry. None of them may become a burst of silence into a
     * live device, which is what a "pad everything" reading of these would do. */
    CHECK(mirror_pad_frames(-EPIPE, 1024, 512, 64) == 0, "an -EPIPE reading produced padding");
    CHECK(mirror_pad_frames(-1, 1024, 512, 64) == 0, "a negative reading produced padding");
    CHECK(mirror_pad_frames(0, 0, 0, 64) == 0, "an unset geometry produced padding");
    CHECK(mirror_pad_frames(0, 1024, 512, 0) == 0, "a zero ceiling produced padding");
    /* A target outside the ring is clamped, not honoured: it would otherwise ask for
     * more silence in one block than the ring can hold. */
    CHECK(mirror_pad_frames(100, 100, 200, 50) == 50, "a target past the ring was not clamped");
    /* And an avail reported larger than the ring — a plug chain accounting for its own
     * buffer — is an empty ring, not a negative level. */
    CHECK(mirror_pad_frames(4096, 1024, 512, 64) == 64, "an oversize avail went wrong");

    /* Now the loop, which is where the number came from. A minute and a half of audio
     * is enough for the un-corrected ring to run dry: 512 frames of lead at 6.9
     * frames a second is ~74 s. Both halves are pinned, because either alone is a
     * test that passes for the wrong reason — the pad has to be there, and it has to
     * be the thing that stops the starvation. */
    mirror_run_ring(120000, 0, &pads, &starved);
    CHECK(starved == 1, "the un-padded ring never ran dry, so this test proves nothing");
    mirror_run_ring(120000, 1, &pads, &starved);
    CHECK(starved == 0, "the padded ring ran dry: the correction does not hold the level");
    /* And it holds it by the *rate of the drift*, not by keeping the ring full: 120000
     * blocks of 64 frames is ~174 s of playback, the deficit is one frame per 100
     * blocks, and each correction restores exactly that frame — so ~1200 corrections
     * of one frame each, which is 6.9 frames a second and 23 us per correction. */
    CHECK(pads >= 1000 && pads <= 1400, "the pad fired %lu times in 120000 blocks", pads);
}

int main(void)
{
    test_parse_and_sizes();
    test_s24_le_identity();
    test_s24_3le();
    test_subframe_le();
    test_s16_le();
    test_interleave();
    test_refusals();
    test_packing_and_the_extension();
    test_mirror_verdict();
    test_mirror_candidates();
    test_mirror_backoff();
    test_mirror_pad();

    printf("test_audio: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
