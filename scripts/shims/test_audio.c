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
 * (mirror_policy.c), which are the half of that feature which is arithmetic — how
 * much the drift correction inserts (test_mirror_pad) and what it inserts
 * (test_mirror_hold, a repeat of the last delivered frame rather than silence).
 * The half that is not — which frame counts as "last" and the fill inside the
 * shim's per-frame loop — cannot be reached from here at all, and is covered by
 * the drill in docs/13-raspberrypi4.md.
 *
 * `test_pace_deadline` pins the block clock a cardless unit runs on
 * (pace_policy.c) — the rate itself, which no counter in the shim reports, by way
 * of the mirror's drift correction, which is the one number in the log that does.
 *
 * `test_mirror_boost_gain` and `test_s24pack_clamp` are the pair behind the
 * mirror's level: the first is the dB-to-multiplier law the HDMI out is lifted by,
 * the second is the sample domain that lift has to stay inside. They are together
 * because they are one decision — the gain is allowed past unity ONLY because
 * something saturates on the way out, and the test at the bottom of the second one
 * pins that the saturation did not move inside the packers, where it would invert
 * a raw word instead of clipping it.
 *
 * The `test_master_*` cases pin the same two things for the master
 * (master_policy.c): which devices its chain may contain, and what one writei()
 * return means. They exist because both halves were wrong on the unit on
 * 2026-09-27, with no FLX4 attached — the chain ended at a bare `default` that
 * opened and then refused every write, and the shim believed it was up because it
 * had a handle. Neither mistake is visible in a log that reads healthy, which is
 * the whole argument for pinning them here.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "s24pack.h"
#include "mirror_policy.h"
#include "master_policy.h"
#include "pace_policy.h"

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

/* The pad's content: a repeat of the last delivered frame, where it used to be a run
 * of silence.
 *
 * This is the part of the correction the operator asked for by name, and the reason is
 * in test_mirror_pad above: the pads do not arrive one frame at a time, they arrive 34
 * to 38 at a time every five seconds, and 0.8 ms of silence in the middle of a track
 * is an envelope notch. What is pinned here is that the hold repeats the frame the
 * caller handed it, at the frame's own stride, and that it can never write past the
 * buffer it was given — the one place in the correction that touches a caller-sized
 * buffer, and an overrun there would read as a mixer fault rather than as a mirror
 * one.
 *
 * The choice of WHICH frame is held (the last one the stream delivered, not the one
 * about to be written) is made in audioshim.c and cannot be reached from here; that
 * half is the drill in docs/13-raspberrypi4.md. */
static void test_mirror_hold(void)
{
    unsigned char out[64];
    const unsigned char f4[4] = { 0x11, 0x22, 0x33, 0x44 };   /* one s16 stereo frame */
    const unsigned char f6[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };  /* s24_3le */

    /* The ordinary case: N copies of the frame, byte for byte, and nothing left in
     * the buffer past them. A hold is a copy of a *sample*, so the bytes go out as
     * they arrived — there is no interpretation of them here and must not be. */
    memset(out, 0x7f, sizeof out);
    CHECK(mirror_hold_fill(f4, sizeof f4, 5, out, sizeof out) == 5,
          "five frames were not placed");
    CHECK(memcmp(out, "\x11\x22\x33\x44\x11\x22\x33\x44\x11\x22\x33\x44\x11\x22\x33\x44"
                      "\x11\x22\x33\x44", 20) == 0,
          "the held frames are not repeats of the frame handed in");
    CHECK(out[20] == 0x7f, "the hold wrote past the frames it reported");

    /* A negative sample is still just bytes: 24-bit -1 is 0xffffff, and the hold must
     * copy that rather than anything that looks like it. This is the same trap
     * test_packing_and_the_extension pins for the packer. */
    {
        const unsigned char neg[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        memset(out, 0x00, sizeof out);
        CHECK(mirror_hold_fill(neg, sizeof neg, 3, out, sizeof out) == 3,
              "a negative frame was not held");
        CHECK(out[17] == 0xff && out[18] == 0x00,
              "the held frame is not followed by the buffer's own bytes");
    }

    /* The stride is the frame's, not a container width: a 6-byte frame repeated 4
     * times is 24 bytes, and a version that assumed 4 would land the second copy in
     * the middle of the first. */
    memset(out, 0x00, sizeof out);
    CHECK(mirror_hold_fill(f6, sizeof f6, 4, out, sizeof out) == 4,
          "four 6-byte frames were not placed");
    CHECK(memcmp(out + 6, f6, sizeof f6) == 0, "the second copy is not on the frame stride");
    CHECK(out[24] == 0x00, "a 6-byte hold overran its 24 bytes");

    /* The clamp: a request larger than the buffer is cut to what fits, not honoured.
     * The caller's own ceiling is MIRROR_PAD_MAX, so this is defensive — and defensive
     * is exactly what a buffer whose size is computed at the call site needs. */
    memset(out, 0x7f, sizeof out);
    CHECK(mirror_hold_fill(f4, sizeof f4, 1000, out, sizeof out) == 16,
          "an oversize hold was not clamped to the buffer");
    CHECK(out[63] == 0x44, "the clamped hold did not fill the buffer exactly");
    CHECK(mirror_hold_fill(f6, sizeof f6, 100, out, 12) == 2,
          "a 12-byte buffer did not take exactly two 6-byte frames");

    /* Nothing to hold, and nowhere to put it: all of these are the caller's silence
     * fallback, so all of them answer 0 rather than a partial or bogus count. */
    CHECK(mirror_hold_fill(NULL, 4, 4, out, sizeof out) == 0, "a NULL frame was held");
    CHECK(mirror_hold_fill(f4, 0, 4, out, sizeof out) == 0, "a zero-width frame was held");
    CHECK(mirror_hold_fill(f4, 4, 0, out, sizeof out) == 0, "a zero-length hold was placed");
    CHECK(mirror_hold_fill(f4, 4, 4, NULL, sizeof out) == 0, "a NULL destination was written");
    CHECK(mirror_hold_fill(f4, 4, 4, out, 3) == 0, "a buffer too small for one frame took one");

    /* The widest frame this shim can produce — S24_LE stereo, two 4-byte containers —
     * is the one the hold buffer is sized for, so it has to be the one that fits
     * exactly. */
    {
        const unsigned char f8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        memset(out, 0x7f, sizeof out);
        CHECK(mirror_hold_fill(f8, sizeof f8, 3, out, 24) == 3,
              "three 8-byte frames did not fit a 24-byte buffer");
        CHECK(memcmp(out + 16, f8, sizeof f8) == 0, "an 8-byte hold is not on the frame stride");
        CHECK(out[24] == 0x7f, "an 8-byte hold wrote past its 24 bytes");
    }
}

/* The mirror's level lift, which is the one gain on this port allowed past unity.
 *
 * The value that matters is +4 dB, because that is what the unit runs — the
 * operator's own choice, and this is where the number is pinned rather than in the
 * shim's arithmetic. The exact multiplier is 1.5848931924611136; asserting it to
 * 12 places is what stops a later edit from quietly redefining the dB as a power
 * ratio, or 20 as 10. */
static void test_mirror_boost_gain(void)
{
    /* A tolerance, because pow(10.0, 0.2) is not exactly representable and the point
     * here is the law, not the last bit of libm. */
    double g;

    g = mirror_boost_gain(4.0);
    CHECK(g > 1.5848931924611130 && g < 1.5848931924611140,
          "+4 dB is x%.17g, expected 1.5848931924611136", g);

    /* The identity, which is also the escape hatch: 0 dB is no change at all, and it
     * is exactly 1.0 rather than a very small power of ten away from it. */
    CHECK(mirror_boost_gain(0.0) == 1.0, "0 dB is x%.17g, not exactly 1.0",
          mirror_boost_gain(0.0));

    /* Below unity it attenuates, the same law read backwards: -6.02 dB is half. */
    g = mirror_boost_gain(-6.020599913279624);
    CHECK(g > 0.4999999 && g < 0.5000001, "-6.02 dB is x%.17g, expected 0.5", g);

    /* Half a dB is a meaningful step to an operator and must survive the law. */
    g = mirror_boost_gain(0.5);
    CHECK(g > 1.0592 && g < 1.0593, "+0.5 dB is x%.17g, expected 1.05925", g);

    /* The window. A mis-keyed order of magnitude (+40 for +4.0) is what it is there
     * for, so the clamp has to be a rail and not a pass-through. */
    CHECK(mirror_boost_gain(MIRROR_BOOST_DB_MAX) == mirror_boost_gain(40.0),
          "+40 dB was not clamped to the window");
    CHECK(mirror_boost_gain(MIRROR_BOOST_DB_MAX) > mirror_boost_gain(4.0),
          "the window's top is not above +4 dB — the guard rail would be the limit");
    CHECK(mirror_boost_gain(-1e6) == mirror_boost_gain(MIRROR_BOOST_DB_MIN),
          "a huge negative was not clamped to the window");
    CHECK(mirror_boost_gain(MIRROR_BOOST_DB_MIN) > 0.0,
          "the window's floor mutes the mirror instead of attenuating it");

    /* Non-finite input answers unity. NaN is the one that matters: it would multiply
     * into every frame of the mirror for the rest of the process's life, and the only
     * symptom would be silence with every counter healthy. */
    {
        double nan = 0.0, inf = 1.0;
        nan = nan / nan;
        inf = inf / 0.0;
        CHECK(mirror_boost_gain(nan) == 1.0, "NaN did not answer unity");
        CHECK(mirror_boost_gain(inf) == mirror_boost_gain(MIRROR_BOOST_DB_MAX),
              "+infinity was not clamped to the window's top");
        CHECK(mirror_boost_gain(-inf) == mirror_boost_gain(MIRROR_BOOST_DB_MIN),
              "-infinity was not clamped to the window's floor");
    }
}

/* The sample domain, and the one thing on this port that has to ask about it.
 *
 * s24pack_clamp() is what the mirror's gain saturates with, and it is NOT applied
 * inside the packers — they stay modular, which is what test_packing_and_the_
 * extension above pins. Keeping those two facts in one place is the point of this
 * test: the rails are the samples' own, and the reason the packers do not use them
 * is that a raw word is already outside them by a whole convention. */
static void test_s24pack_clamp(void)
{
    /* The rails are exact and inclusive: full scale is a valid sample, and the
     * bottom is one count lower, because 24-bit two's complement is asymmetric. */
    CHECK(s24pack_clamp(0) == 0, "zero was moved");
    CHECK(s24pack_clamp(S24PACK_SAMPLE_MAX) == S24PACK_SAMPLE_MAX,
          "full scale was clamped — the top of the domain is a legal sample");
    CHECK(s24pack_clamp(S24PACK_SAMPLE_MIN) == S24PACK_SAMPLE_MIN,
          "the bottom of the domain was clamped");
    CHECK(s24pack_clamp(1) == 1 && s24pack_clamp(-1) == -1, "a small sample was moved");
    CHECK(s24pack_clamp(S24PACK_SAMPLE_MAX + 1) == S24PACK_SAMPLE_MAX,
          "one count past full scale was not pulled back to the rail");
    CHECK(s24pack_clamp(S24PACK_SAMPLE_MIN - 1) == S24PACK_SAMPLE_MIN,
          "one count below the bottom was not pulled back to the rail");

    /* The extremes a boosted sample can actually reach: the loudest 24-bit sample
     * times the window's top (+12 dB, 3.98x) is 33.4M, well inside int32, so the
     * float-to-int conversion at the gain never overflows — this is the arithmetic
     * that says so. */
    CHECK(s24pack_clamp(33400000) == S24PACK_SAMPLE_MAX, "+12 dB's peak was not clamped");
    CHECK(s24pack_clamp(-33400000) == S24PACK_SAMPLE_MIN,
          "-12 dB's peak was not clamped");
    CHECK(s24pack_clamp(2147483647) == S24PACK_SAMPLE_MAX, "INT32_MAX was not clamped");
    CHECK(s24pack_clamp(-2147483647 - 1) == S24PACK_SAMPLE_MIN,
          "INT32_MIN was not clamped");

    /* And the property the packers rely on staying OUT of them: a raw word is
     * clamped by this helper (it is many counts past full scale as a 32-bit value)
     * while the packer still encodes it verbatim, which is the difference between a
     * -1 and a full-scale blast on a card that reads the low three bytes. */
    {
        int32_t raw = 0x00ffffff;          /* rbp's -1, unextended */
        unsigned char a[4], b[4];
        int32_t ext = sext24(raw);

        CHECK(ext == -1, "the premise: the extended raw word is -1");
        CHECK(s24pack_clamp(raw) == S24PACK_SAMPLE_MAX,
              "the clamp left a raw word alone; it is 16.7M as a 32-bit value");

        memset(a, 0xAA, sizeof a);
        memset(b, 0xAA, sizeof b);
        CHECK(s24pack(AUDIO_FMT_S24_3LE, &raw, 1, 1, a, sizeof a) == 3 &&
              s24pack(AUDIO_FMT_S24_3LE, &ext, 1, 1, b, sizeof b) == 3,
              "S24_3LE refused a one-sample block");
        CHECK(a[0] == 0xff && a[1] == 0xff && a[2] == 0xff && memcmp(a, b, 3) == 0,
              "S24_3LE no longer encodes a raw word as -1: it produced %02x %02x %02x, "
              "so something started clamping inside the packer", a[0], a[1], a[2]);
    }
}

/* Feed the mirror's ring from a block clock, and count what the drift correction
 * has to put back — the operator's symptom, as arithmetic.
 *
 * `work_ns` is everything a block costs besides its own time: the wakeup out of the
 * sleep, the staging, the packing, the mirror. `deadline_paced` 0 is the
 * sleep-per-block clock pace_policy.c replaced (usleep(period), then the work).
 *
 * The sink is 155 ppm faster than the feed, which is the drift the correction
 * exists for, and the level is held near half a ring like the real one. Reports the
 * frames of silence injected and the elapsed time, so the two clocks can be compared
 * as rates instead of as arithmetic. */
static void mirror_run_clock(int blocks, unsigned long period_ns,
                             unsigned long work_ns, int deadline_paced,
                             unsigned long *padframes, unsigned long long *elapsed_ns)
{
    const long buf = 1024;
    const double sink = 44100.0 * (1.0 + 155e-6) / 1e9;   /* frames per ns */
    double level = buf / 2.0;
    unsigned long long now = 0, dl = 0;
    int t;

    *padframes = 0;
    for (t = 0; t < blocks; t++) {
        unsigned long long after;

        if (deadline_paced) {
            dl = pace_next_deadline(now, period_ns, dl);
            after = dl + work_ns;             /* wake at the deadline, then work */
        } else {
            after = now + period_ns + work_ns;   /* usleep(period), then work */
        }

        level -= (double)(after - now) * sink;    /* what the sink took meanwhile */
        level += 64.0;                            /* what the block fed it */
        if (level < 0.0)
            level = 0.0;                  /* an XRUN: the stream restarts here */
        now = after;

        {
            unsigned long p = mirror_pad_frames(buf - (long)level, (unsigned long)buf,
                                                (unsigned long)buf / 2, 64);
            if (p) {
                *padframes += p;
                level += (double)p;
            }
        }
    }
    *elapsed_ns = now;
}

/* The block clock (pace_policy.c), and what a wrong one costs the monitor.
 *
 * This is the second half of the cardless story. The first half — three sleeps per
 * block, a third of real time — is pinned by pace_secondary()'s counter rule, which
 * is three lines of arithmetic inside the shim and cannot be reached from here. The
 * half that is here is the rate itself: a clock is not content, so not one counter
 * in the shim reports it, and both rounds of this defect were found only because
 * the HDMI mirror's *drift correction* counts the silence it injects, which is the
 * deficit. */
static void test_pace_deadline(void)
{
    const unsigned long period = 1451247;   /* 64 frames at 44100, in ns */
    const unsigned long work = 167000;      /* measured on the unit, cardless */
    const int blocks = 35000;               /* ~58 s, the length of the windows */
    unsigned long p_sleep, p_dead;
    unsigned long long el_sleep, el_dead;

    /* The rule, case by case, with `now` the moment inside a block whose deadline
     * is 1000 — the wait is the returned deadline minus now, because that is what
     * the caller sleeps and therefore what sets the rate. */
    CHECK(pace_next_deadline(1000, period, 0) == 1000 + period,
          "the first block was not given a period of its own");
    CHECK(pace_next_deadline(1000, period, 1000) - 1000 == period,
          "a block that had done none of its work was not given a whole period");
    /* The defect, in one line: a block that has already spent half its period on
     * its own work waits the other half. A sleep of a whole period always waited a
     * whole period, which is how the work got charged to the clock. */
    CHECK(pace_next_deadline(1000 + period / 2, period, 1000)
              - (1000 + period / 2) == period - period / 2,
          "a block that had done half its work did not wait the remainder");
    /* A block that overran its period is written off, not repaid: the deadline
     * restarts a period from now, so the blocks after it do not run fast. Running
     * fast is the worse artefact of the two — it is the ring overflowing instead
     * of draining, and no amount of silence can correct that. */
    CHECK(pace_next_deadline(1000 + 3 * period, period, 1000)
              - (1000 + 3 * period) == period,
          "a late block was repaid as a burst of fast blocks");
    /* A clock that stepped backwards restarts the same way, rather than being slept
     * through as a stall of the length of the step. */
    CHECK(pace_next_deadline(1000, period, 1000 + 5 * period) - 1000 == period,
          "a backwards clock step became a long sleep");
    /* And a zero period is not a division: nothing to wait for. */
    CHECK(pace_next_deadline(1000, 0, 777) == 1000, "a zero period produced a deadline");

    /* The two clocks over the same 35000 blocks, as the mirror's own counters saw
     * them with the FLX4 away. */
    mirror_run_clock(blocks, period, work, 0, &p_sleep, &el_sleep);
    mirror_run_clock(blocks, period, work, 1, &p_dead, &el_dead);

    /* The deadline clock costs the block clock exactly: the elapsed time is the
     * blocks' own periods plus the last block's work, and no more. */
    CHECK(el_dead == (unsigned long long)blocks * period + work,
          "the deadline clock took %llu ns for %d blocks, not %lu",
          el_dead, blocks, (unsigned long)blocks * period + work);
    /* The sleep clock paid the overhead on every block, which is the whole defect:
     * 35000 x 167 us of it, 89.7 % of real time. */
    CHECK(el_sleep == (unsigned long long)blocks * (period + work),
          "the sleep clock's per-block cost did not add up");
    CHECK(el_sleep > el_dead, "the two clocks are not distinguishable at all");

    /* And this is what the operator heard. The ring needs ~6.9 frames a second of
     * correction (the two crystals, 155 ppm); the sleep clock made the mirror
     * inject 7.38 frames of silence into every 64-frame block — 660x the drift,
     * 10.3 % of the timeline, in holes of ~0.17 ms about a hundred times a second
     * — and the deadline clock puts it back at the floor. Both halves are pinned,
     * because either alone passes for the wrong reason. */
    CHECK(p_dead > 0, "the drift correction never fired, so this proves nothing");
    CHECK(p_dead * 1000 / blocks < 30,           /* 0.01 frames/block, not 7.38 */
          "the deadline clock needed %lu frames of silence in %d blocks", p_dead, blocks);
    CHECK(p_sleep / blocks >= 7 && p_sleep / blocks <= 8,
          "the sleep clock cost %lu frames of silence per block", p_sleep / blocks);
    CHECK(p_sleep > 500 * p_dead,
          "the two clocks were not far apart: %lu frames against %lu", p_sleep, p_dead);
}

/* ---- the master's policy ---------------------------------------------------- */

static void test_master_candidates(void)
{
    char got[MASTER_CAND_MAX][MASTER_NAME_MAX];
    int dropped, n;

    /* The chain the unit has to run on. Two entries — the configured card and its
     * plug twin — and the second check is the one that matters: this is the ONLY
     * test that fails if the bare name `default` is ever put back into the chain.
     * It is not a style check. `default` on this unit is card 0, bcm2835
     * Headphones, which opens and then refuses every write with -EINVAL; that is
     * the failure this whole module exists for, and a chain that names it is a
     * chain that can take the player's UI down with it. */
    dropped = -1;
    n = master_candidates("hw:CARD=DDJFLX4,DEV=0", "plughw:CARD=DDJFLX4,DEV=0",
                          got, MASTER_CAND_MAX, &dropped);
    CHECK(n == 2, "the shipped device produced %d candidates, not 2", n);
    CHECK(n == 2 && strcmp(got[0], "hw:CARD=DDJFLX4,DEV=0") == 0,
          "the configured device is not tried first");
    CHECK(n == 2 && strcmp(got[1], "plughw:CARD=DDJFLX4,DEV=0") == 0,
          "the plug twin is not tried second");
    CHECK(dropped == 0, "the shipped device dropped something");

    /* The device named by AUDIO_DEV is always in the chain, whatever it is. This
     * is what keeps RB_AUDIO_DEV=default usable as the explicit opt-in it is (and
     * as the drill's way to stage a refusing device on purpose) — the rule the
     * chain follows is "only what AUDIO_DEV names", not "never this string". */
    n = master_candidates("default", "", got, MASTER_CAND_MAX, &dropped);
    CHECK(n == 1, "an explicitly named default produced %d candidates, not 1", n);
    CHECK(n == 1 && strcmp(got[0], "default") == 0, "an explicit default was not kept");
    CHECK(dropped == 0, "an explicit default dropped something");

    /* No twin for a non-hw: device, and none for one already through the plug
     * layer: a twin of a twin is the same device twice. The caller passes "" for
     * those, which is also what an absent AUDIO_DEV would produce. */
    n = master_candidates("plughw:CARD=DDJFLX4,DEV=0", "", got, MASTER_CAND_MAX,
                          &dropped);
    CHECK(n == 1, "a plughw: device produced %d candidates, not 1", n);
    CHECK(n == 1 && strcmp(got[0], "plughw:CARD=DDJFLX4,DEV=0") == 0,
          "a plughw: device was not kept verbatim");

    /* Nothing to add is not a dropped candidate; two names for one device is. */
    n = master_candidates("hw:CARD=a,DEV=0", "", got, MASTER_CAND_MAX, &dropped);
    CHECK(n == 1 && dropped == 0, "an absent twin was counted as a dropped name");
    n = master_candidates("hw:CARD=a,DEV=0", "hw:CARD=a,DEV=0", got, MASTER_CAND_MAX,
                          &dropped);
    CHECK(n == 1, "the same device twice produced %d candidates, not 1", n);
    CHECK(dropped == 1, "the same device twice dropped %d, not 1", dropped);

    /* An empty or absent AUDIO_DEV is not a candidate — and not a dropped one
     * either. (load_config() has already replaced an empty one with the default,
     * so this is the module's own contract; the shim logs `candidates=0`, which is
     * the shape of a bug worth seeing rather than a name that went missing.) */
    dropped = -1;
    CHECK(master_candidates(NULL, NULL, got, MASTER_CAND_MAX, &dropped) == 0,
          "a NULL device produced a candidate");
    CHECK(dropped == 0, "an absent device was counted as a dropped name");
    CHECK(master_candidates("", "", got, MASTER_CAND_MAX, &dropped) == 0,
          "an empty device produced a candidate");
    CHECK(dropped == 0, "an empty device was counted as a dropped name");

    /* A name too long for the array is dropped and counted, never truncated: a
     * truncated device name is a name that does not exist, and the open failure
     * would read as "the card refused us". Built rather than written out so the
     * length is a number and not something to count by eye — and checked, so the
     * test cannot quietly stop testing anything if an edit shortens it. */
    {
        char long_name[512];
        char edge_name[MASTER_NAME_MAX];

        memset(long_name, 'a', sizeof long_name);
        memcpy(long_name, "hw:CARD=", 8);
        long_name[8 + 170] = '\0';
        CHECK(strlen(long_name) >= MASTER_NAME_MAX, "the over-long name is not over-long");
        n = master_candidates(long_name, "", got, MASTER_CAND_MAX, &dropped);
        CHECK(n == 0, "an over-long name produced %d candidates", n);
        CHECK(dropped == 1, "an over-long name dropped %d, not 1", dropped);

        /* One byte shorter is a name, and it is kept whole rather than refused. */
        memset(edge_name, 'b', sizeof edge_name);
        edge_name[MASTER_NAME_MAX - 1] = '\0';
        n = master_candidates(edge_name, "", got, MASTER_CAND_MAX, &dropped);
        CHECK(n == 1, "a name exactly at the limit produced %d candidates", n);
        CHECK(n == 1 && strcmp(got[0], edge_name) == 0,
              "a name exactly at the limit was altered");
    }

    /* Nowhere to put anything still counts rather than writing: the capacity test
     * has to come before the store, or this is a buffer overflow. */
    n = master_candidates("hw:CARD=a,DEV=0", "plughw:CARD=a,DEV=0", got, 0, &dropped);
    CHECK(n == 0, "a zero-capacity chain stored %d", n);
    CHECK(dropped == 2, "a zero-capacity chain dropped %d, not 2", dropped);

    /* dropped may be NULL; the shim always passes one, but the contract says so. */
    CHECK(master_candidates("hw:CARD=a,DEV=0", "", got, MASTER_CAND_MAX, NULL) == 1,
          "a NULL dropped out-parameter broke the count");
}

/* What rbp's write on the master means. The errnos are the ones a writei() really
 * returns, and the measured one is first. */
static void test_master_verdict(void)
{
    /* The measurement this module exists for: `default` (card 0, bcm2835
     * Headphones) with no FLX4 on the bus answers -EINVAL on every write, and the
     * shim — which acted only on -ENODEV — kept the handle and spun, taking the
     * player's UI down with it. Every hard failure must land here. */
    CHECK(master_verdict(-EINVAL, 64) == MASTER_DOWN, "-EINVAL is not a dead handle");
    CHECK(master_verdict(-ENODEV, 64) == MASTER_DOWN, "-ENODEV is not a dead handle");
    CHECK(master_verdict(-EBUSY, 64) == MASTER_DOWN, "-EBUSY is not a dead handle");
    CHECK(master_verdict(-ENXIO, 64) == MASTER_DOWN, "-ENXIO is not a dead handle");
    CHECK(master_verdict(-EIO, 64) == MASTER_DOWN, "-EIO is not a dead handle");
    CHECK(master_verdict(-9999, 64) == MASTER_DOWN, "an unknown errno is not a loss");

    /* Audio landing is what makes a handle live — including a partial block. */
    CHECK(master_verdict(64, 64) == MASTER_WROTE, "a whole block is not a write");
    CHECK(master_verdict(32, 64) == MASTER_SHORT, "a partial write is not short");
    CHECK(master_verdict(1, 64) == MASTER_SHORT, "one frame is not short");

    /* A full ring is NOT a fault and must never be counted as one. rbp's
     * non-blocking bit survives onto this handle (open_real_device clears only
     * SND_PCM_ASYNC), so this is a state a healthy card really produces; counting
     * it would retire the FLX4 on its first full ring. */
    CHECK(master_verdict(-EAGAIN, 64) == MASTER_FULL, "-EAGAIN is not a full ring");
    CHECK(master_verdict(-EWOULDBLOCK, 64) == MASTER_FULL,
          "-EWOULDBLOCK is not a full ring");

    /* A broken stream on a device that is still there: one prepare, one rewrite.
     * A master_verdict() call sees the FINAL outcome, so what reaches here has
     * already been retried once — see the counter's comment below. */
    CHECK(master_verdict(-EPIPE, 64) == MASTER_RETRY, "-EPIPE is not worth a retry");
    CHECK(master_verdict(-ESTRPIPE, 64) == MASTER_RETRY,
          "-ESTRPIPE is not worth a retry");
    CHECK(master_verdict(-EBADFD, 64) == MASTER_RETRY, "-EBADFD is not worth a retry");

    /* More frames than were asked for is not a delivery. */
    CHECK(master_verdict(128, 64) == MASTER_DOWN,
          "writing more frames than were asked for is not a write");

    /* Nothing delivered is not a delivery either, and this is the ONE reading that
     * deliberately differs from mirror_verdict(): the mirror calls 0 a full ring
     * ("nothing consumed, but nothing is wrong, do not log a write"), while for
     * the master only a delivered frame proves the handle is carrying the stream.
     * The two are pinned against each other below so the divergence stays
     * deliberate rather than becoming a bug. */
    CHECK(master_verdict(0, 64) == MASTER_DOWN,
          "writing no frames at all is not a dead handle");
    CHECK(mirror_verdict(0, 64) == MIRROR_FULL,
          "the mirror stopped reading 0 as a full ring");

    /* Every verdict has a name, because every one of them reaches the log. */
    {
        static const char *names[] = { "wrote", "short", "full", "retry", "down" };
        int v;

        for (v = 0; v <= 4; v++)
            CHECK(strcmp(master_verdict_name((enum master_verdict)v), names[v]) == 0,
                  "verdict %d has the wrong log name", v);
    }

    /* The cross-check, so an edit made to one classifier and not the other is
     * caught here. Every errno either of them knows must be classified the same
     * way, hard-vs-retryable-vs-full; written == 0 is the only case where they
     * are allowed to disagree, and it is asserted above. */
    {
        static const int errnos[] = { EINVAL, ENODEV, EBUSY, ENXIO, EIO, EPIPE,
                                      ESTRPIPE, EBADFD, EAGAIN, EWOULDBLOCK, 9999 };
        static const long written[] = { 1, 32, 64, 128 };
        size_t i;

        for (i = 0; i < sizeof errnos / sizeof errnos[0]; i++) {
            enum master_verdict m = master_verdict(-errnos[i], 64);
            enum mirror_verdict r = mirror_verdict(-errnos[i], 64);

            CHECK((m == MASTER_DOWN) == (r == MIRROR_DOWN),
                  "the two classifiers disagree about -%d as a hard failure",
                  errnos[i]);
            CHECK((m == MASTER_RETRY) == (r == MIRROR_RETRY),
                  "the two classifiers disagree about -%d as retryable", errnos[i]);
            CHECK((m == MASTER_FULL) == (r == MIRROR_FULL),
                  "the two classifiers disagree about -%d as a full ring", errnos[i]);
        }
        for (i = 0; i < sizeof written / sizeof written[0]; i++) {
            enum master_verdict m = master_verdict(written[i], 64);
            enum mirror_verdict r = mirror_verdict(written[i], 64);

            CHECK((m == MASTER_WROTE) == (r == MIRROR_WROTE),
                  "the two classifiers disagree about %ld frames as a write",
                  written[i]);
            CHECK((m == MASTER_SHORT) == (r == MIRROR_SHORT),
                  "the two classifiers disagree about %ld frames as short",
                  written[i]);
            CHECK((m == MASTER_DOWN) == (r == MIRROR_DOWN),
                  "the two classifiers disagree about %ld frames as a loss",
                  written[i]);
        }
    }
}

/* The counter that retires a handle which opens and refuses, and what keeps it
 * from retiring a card that is merely slow. */
static void test_master_fail_counter(void)
{
    unsigned long long n = 0;
    int i;

    /* Anything that delivered audio — or was consumed by a working ring — clears
     * the count. This is why a card that delivers one block in seven hundred is
     * never retired. */
    CHECK(master_fail_frames_next(4096, 64, MASTER_WROTE) == 0,
          "a delivered block did not reset the failure count");
    CHECK(master_fail_frames_next(4096, 64, MASTER_SHORT) == 0,
          "a partial delivery did not reset the failure count");
    CHECK(master_fail_frames_next(4096, 64, MASTER_FULL) == 0,
          "a full ring did not reset the failure count");

    /* A failure counts by the frames it did not carry, not by the attempt. */
    CHECK(master_fail_frames_next(0, 64, MASTER_DOWN) == 64, "a failed block did not count");
    CHECK(master_fail_frames_next(64, 64, MASTER_DOWN) == 128, "failures did not accumulate");
    CHECK(master_fail_frames_next(0, 64, MASTER_RETRY) == 64,
          "a prepared-and-still-failed block did not count");
    /* -ENODEV never reaches the counter in the shim (it is acted on immediately),
     * but it must not be treated as a delivery if it ever does. */
    CHECK(master_fail_frames_next(0, 64, master_verdict(-ENODEV, 64)) == 64,
          "-ENODEV reset the failure count");

    /* Saturating, so a failure long enough to overflow comes back as "still dead"
     * rather than wrapping round to healthy. */
    CHECK(master_fail_frames_next(~0ULL, 64, MASTER_DOWN) == ~0ULL,
          "the failure count wrapped instead of saturating");
    CHECK(master_fail_frames_next(~0ULL - 32, 64, MASTER_DOWN) == ~0ULL,
          "a near-overflow failure count wrapped");

    /* The limit, and the two ways it is meant to be inert. 0 disables the rule —
     * the same idiom as an interval of 0 in the mirror's backoff. */
    CHECK(master_is_dead(0, 44100) == 0, "an untouched handle was dead");
    CHECK(master_is_dead(44099, 44100) == 0, "a handle one frame short was dead");
    CHECK(master_is_dead(44100, 44100) == 1, "a handle at the limit was not dead");
    CHECK(master_is_dead(1000000, 44100) == 1, "a handle past the limit was not dead");
    CHECK(master_is_dead(999999, 0) == 0, "a disabled limit still retired a handle");

    /* 44100 frames of audio that never landed, in the blocks rbp writes. This is
     * the measured case's arithmetic: -EINVAL on every block, both attempts, and
     * the counter runs out on the 690th — 690 x 64 = 44160. */
    for (i = 1; i <= 2000; i++) {
        n = master_fail_frames_next(n, 64, MASTER_DOWN);
        if (master_is_dead(n, MASTER_DEAD_FRAMES))
            break;
    }
    CHECK(i == 690, "the count ran out on block %d, not 690", i);
    CHECK(n == 44160, "the count ran out at %llu frames, not 44160", n);
    CHECK(master_is_dead(689 * 64ULL, MASTER_DEAD_FRAMES) == 0,
          "the 689th failed block retired the handle");
    CHECK(master_is_dead(690 * 64ULL, MASTER_DEAD_FRAMES) == 1,
          "the 690th failed block did not retire the handle");

    /* And the false positive this whole design is arranged to avoid: a card that
     * is slow or briefly stuck, but is still delivering. Half a limit of failures,
     * one block delivered, half a limit again — nowhere near dead. A counter that
     * did not reset would have retired it on the 690th of the second run. */
    n = 0;
    for (i = 0; i < 300; i++)
        n = master_fail_frames_next(n, 64, MASTER_DOWN);
    n = master_fail_frames_next(n, 64, MASTER_WROTE);
    for (i = 0; i < 300; i++) {
        n = master_fail_frames_next(n, 64, MASTER_DOWN);
        CHECK(!master_is_dead(n, MASTER_DEAD_FRAMES),
              "a delivering card was retired after %d failed blocks", i + 1);
    }
    CHECK(n == 300 * 64ULL, "the delivered block did not reset the count cleanly");
}

/* How often a device that opens and refuses every block is re-opened, in the
 * shim's own accounting: one cycle is an open, then a second of audio that never
 * lands, then the retire — after which the next open waits g_reopen_backoff_ms.
 *
 * The failing blocks are absorbed at the rate the unit measured (~29k a second
 * during the -EINVAL wedge), so the 690 blocks that reach MASTER_DEAD_FRAMES cost
 * about 24 ms of wall clock; the wait is what dominates, and the wait is the only
 * thing that changes between the two callers below.
 *
 * `reset_on_open` is the caller's behaviour and the whole point of the exercise:
 * 1 is the bug the shim had (the wait and the failure count were cleared the
 * moment the open returned, before any frame had landed), 0 is what it does now
 * (they are cleared by the first DELIVERED frame, which a refusing device never
 * produces). master_policy.c cannot tell the two apart — only a caller can — so
 * the caller is what is modelled. The floor and the ceiling (500, 5000,
 * REOPEN_MIN_MS/REOPEN_MAX_MS in the shim) are literals here because audioshim.o
 * is deliberately not linkable. */
static int master_count_opens(int ms_total, int reset_on_open)
{
    const int dead_ms = 24;
    const unsigned long ceiling = 5000;
    unsigned long backoff = 500;
    int t = 0, opens = 0;

    while (t < ms_total) {
        opens++;
        if (reset_on_open)
            backoff = 500;
        t += dead_ms + (int)backoff;
        if (backoff < ceiling) {
            backoff *= 2;
            if (backoff > ceiling)
                backoff = ceiling;
        }
    }
    return opens;
}

static void test_master_reopen_backoff(void)
{
    int fixed = master_count_opens(60000, 0);
    int buggy = master_count_opens(60000, 1);

    /* A minute of a device that opens and refuses: 500 ms, 1 s, 2 s, 4 s, then the
     * 5 s ceiling — about fifteen opens, and the cost of each on rbp's audio
     * thread stays bounded. */
    CHECK(fixed >= 10, "the backoff stopped retrying altogether (%d opens)", fixed);
    CHECK(fixed <= 20, "the backoff retried far more often than it allows (%d opens)",
          fixed);

    /* The frozen-floor caller, pinned so the reason this test is a loop is on the
     * record: clearing the wait at the open means every cycle costs the same
     * 500 ms, and the same device is opened and closed about eight times as often
     * for as long as the player runs. That is defect 5 of the change this test
     * came with, and it is invisible to every check above. */
    CHECK(buggy > 100, "the frozen-floor caller this test exists to catch did not churn");
    CHECK(buggy > 4 * fixed, "clearing the wait at the open cost nothing at all");
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
    test_mirror_hold();
    test_mirror_boost_gain();
    test_s24pack_clamp();
    test_pace_deadline();
    test_master_candidates();
    test_master_verdict();
    test_master_fail_counter();
    test_master_reopen_backoff();

    printf("test_audio: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
