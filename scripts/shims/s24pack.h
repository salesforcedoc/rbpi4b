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
 * The default path needs none of this. AUDIO_DEV is a `plughw:` device, so ALSA's
 * plug chain converts S24_LE to whatever the card wants and we hand over rbp's
 * own words untouched. These exist for the escape hatch — a `hw:` device with an
 * explicit AUDIO_FMT, where nothing converts for us and the bytes must be exactly
 * right.
 *
 * Pure: no ALSA, no allocation, no globals. That is what lets test_audio.c run
 * the real functions under qemu-arm with no card attached.
 */
#ifndef RBLIVE4_S24PACK_H
#define RBLIVE4_S24PACK_H

#include <stdint.h>
#include <stddef.h>

/* One entry per format we can pack to. S24_LE is the container rbp already
 * writes, so it is the identity case and the default. */
enum {
    AUDIO_FMT_S24_LE = 0,   /* 4 bytes, value in the low 3 — rbp's own layout */
    AUDIO_FMT_S24_3LE,      /* 3 bytes, packed (the DDJ-FLX4's likely native)   */
    AUDIO_FMT_S16_LE        /* 2 bytes, low 8 bits dropped                      */
};

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

#endif /* RBLIVE4_S24PACK_H */
