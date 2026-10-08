/*
 * s24pack.h — output sample packing, as pure functions.
 *
 * rbp hands every output stream to snd_pcm_writei() as 32-bit words holding a
 * 24-bit sample right-justified in the low three bytes with the top byte zero, so
 * the raw word for -1 is the *positive* 0x00ffffff.
 *
 * The packed formats are insensitive to that: S24_3LE and S16_LE read only bits
 * 23..0, which are the three bytes rbp wrote, so the sign-extension audioshim.c
 * applies before its gains makes no difference to the bytes they produce. This is
 * worth stating plainly because the opposite is the intuitive reading — 24-bit
 * two's complement -1 *is* 0xffffff, so it packs to ff ff ff either way.
 *
 * S24_LE is the exception and is not a packer at all: it is the container rbp
 * already writes, so it copies the whole 4-byte word including the top byte, and
 * the extension *is* visible in its output. audioshim.c fills its buffer with
 * extended values, so the card receives the extended form.
 *
 * What the extension is genuinely load-bearing for is the arithmetic: a gain
 * applied to the *raw* word gives 0x00ffffff * 0.5, full-scale positive, where
 * -1 * 0.5 is silence. test_audio.c pins both halves of this.
 *
 * This is the default path, not an escape hatch. AUDIO_DEV is a `hw:` device, so
 * nothing between the shim and the card converts and the bytes have to be exactly
 * right; `rb.conf` sets AUDIO_FMT=s24_3le, which is what the FLX4 accepts and
 * therefore what these produce. They are only bypassed when AUDIO_DEV names a
 * *plug* device, where ALSA's plug chain does the conversion and rbp's own words
 * go over untouched — which is also the one case where leaving AUDIO_FMT unset is
 * right, since an unset name parses to S24_LE and hands rbp's words through
 * unchanged.
 *
 * AUDIO_FMT_SUBFRAME_LE is the fourth case and the odd one out: it is not a
 * container rbp writes, it is not the FLX4's, and it is not one a plug chain will
 * make either — the vc4 HDMI PCMs offer it and *nothing else* (measured, and it
 * is in the driver: `drivers/gpu/drm/vc4/vc4_hdmi.c` gives its CPU DAI
 * `.formats = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE` alone, so the card's mask is
 * the intersection with hdmi-codec's list and collapses to that one name). It is
 * a genuine IEC 60958 subframe rather than a 4-byte container, and the position
 * of the sample is the whole of it:
 *
 *     bits 0-3   preamble
 *         4-27   24-bit sample, LSB at bit 4
 *         28     validity
 *         29     user data
 *         30     channel status
 *         31     parity (even over bits 4..30)
 *
 * That is alsa-lib's layout, taken from the encoder every working setup on this
 * hardware goes through (`src/pcm/pcm_iec958.c`): it is handed the sample
 * left-justified and immediately does `data >>= 4; data &= ~0xf;`. The Pi
 * hardware "does almost no repacking between the FIFO submission and the wire",
 * so those bits are what the sink reads — which is why the *wrong* reading looks
 * so specific. Putting the sample at bits 8..31 (the obvious reading of "24-bit
 * sample left-justified in a 32-bit word", and what this file first shipped) puts
 * it 4 bits high: the sink reads bits 4..27, finds the sample's low 20 bits
 * promoted to the top, and plays a wrapped, aliased waveform at full level. On
 * the unit that is loud and *very distorted* and nothing else — the picture, the
 * FLX4 and every counter stay perfect, so it is an ears-only defect, and S4.6 is
 * the row that found it. The parity bit is set so each word is a valid subframe;
 * the preamble and V/U/C are left zero because the hardware signals Z/B itself
 * (the driver sets its b-frame identifier to 8 to match ALSA's Z) and because an
 * all-zero preamble demonstrably does not stop the sink locking — it was audible,
 * just wrong, which is the measurement that says the preamble is not the bug.
 * See raspberrypi/linux issues #4654 and #4193.
 *
 * Pure: no ALSA, no allocation, no globals. That is what lets test_audio.c run
 * the real functions under qemu-arm with no card attached — and it is also why
 * the preamble is not synthesised here: a correct preamble needs a 192-subframe
 * block counter, i.e. state, which would cost this file the property that makes
 * it testable.
 */
#ifndef RBPI4B_S24PACK_H
#define RBPI4B_S24PACK_H

#include <stdint.h>
#include <stddef.h>

/* One entry per format we can pack to. S24_LE is the container rbp already
 * writes, so it is the identity case — and on this target it is NOT the default:
 * the FLX4 accepts S24_3LE (measured: `aplay --dump-hw-params` lists S16_LE and
 * S24_3LE, and no 4-byte 24-bit format at all), so `rb.conf` selects s24_3le. */
enum {
    AUDIO_FMT_S24_LE = 0,   /* 4 bytes, value in the low 3 — rbp's own layout */
    AUDIO_FMT_S24_3LE,      /* 3 bytes, packed — the DDJ-FLX4's native format */
    AUDIO_FMT_S16_LE,       /* 2 bytes, low 8 bits dropped                    */
    AUDIO_FMT_SUBFRAME_LE   /* 4 bytes, value in the HIGH 3 — the vc4 HDMI DMA */
};

/* The sample domain: what rbp's words mean once audioshim.c has sign-extended
 * them, and therefore the rails a value has to be inside to be a sample at all.
 * Full scale is 2^23-1, the bottom is -2^23.
 *
 * The packers below do NOT apply this, and that is a decision rather than an
 * omission. They read bits 23..0 and encode what they are given, which is what
 * makes them insensitive to the missing sign extension in rbp's raw word — the
 * property the opening paragraphs of this header rest on and test_audio.c pins.
 * A clamp here would destroy exactly that: rbp's raw word for -1 is the positive
 * 0x00ffffff, i.e. 16 777 215 counts past full scale, so a clamp would turn a
 * quiet -1 into a full-scale positive sample on a 24-in-32 card that reads the low
 * three bytes. That is the "aliased waveform at full level" class this port has
 * already been bitten by once (S4.6), reached from the other side.
 *
 * So the domain is defined here, and it is ENFORCED at the one place on this port
 * that can leave it: the HDMI mirror's gain, which is the only gain that is
 * allowed past unity (mirror_saturate() in audioshim.c, counting what it pulled
 * back as `clips=`). Every other sample the shim packs is inside the domain by
 * construction — the buffers are sign-extended and the master's gains are
 * clamp01()'d — which is why the packers can go on being modular. */
#define S24PACK_SAMPLE_MAX  8388607
#define S24PACK_SAMPLE_MIN  (-8388608)

/* Saturate one sample to that domain. Out of range is not a quiet condition on
 * this port: a word even one count past full scale loses its top byte to the
 * S24_LE container, aliases into a different sample in S24_3LE and the IEC958
 * subframe, and wraps to the opposite rail in S16_LE — which is why the mirror
 * asks before it hands a lifted sample on. */
static inline int32_t s24pack_clamp(int32_t v)
{
    if (v > S24PACK_SAMPLE_MAX)
        return S24PACK_SAMPLE_MAX;
    if (v < S24PACK_SAMPLE_MIN)
        return S24PACK_SAMPLE_MIN;
    return v;
}

/* Bytes per sample, or 0 for an unknown format. */
int s24pack_bytes(int fmt);

/* Parse a format name, case-insensitively. Returns -1 when the name is not one
 * we know, so the caller can report the name it did not understand instead of
 * silently doing something else. NULL or "" is the default, not an error. */
int s24pack_parse(const char *name);

/* Pack `frames` frames of `ch` interleaved channels out of `src` into `dst`,
 * which holds at most `dst_cap` bytes.
 *
 * Returns the number of bytes written, or 0 for an unknown format or a
 * destination too small to hold the result — in which case nothing is written,
 * so a caller that gets 0 must not hand `dst` to the device. `frames` must be
 * non-zero (0 frames is reported as 0, i.e. as failure; callers return early on
 * an empty block anyway). */
unsigned s24pack(int fmt, const int32_t *src, unsigned frames, unsigned ch,
                 void *dst, unsigned dst_cap);

/* Worst-case bytes a format can need for a block: the 4-byte container, which is
 * what S24_LE produces and is larger than every other format here. One number to
 * size any output scratch buffer with, so a format change can never overflow it. */
static inline unsigned s24pack_worst(unsigned frames, unsigned ch)
{
    return frames * ch * (unsigned)sizeof(int32_t);
}

#endif /* RBPI4B_S24PACK_H */
