/*
 * s24pack.c — see s24pack.h. Two loops and a memcpy; the value is in being
 * separable, so the byte-level contract can be tested without a sound card.
 * What these deliberately do NOT do is clamp: they encode whatever value they are
 * given, which is what makes them insensitive to a raw word's missing sign
 * extension — and the saturation the mirror's level needs lives at the mirror's
 * gain instead, on the argument the header's S24PACK_SAMPLE_* comment sets out.
 */
#define _GNU_SOURCE
#include "s24pack.h"

#include <string.h>
#include <strings.h>

int s24pack_bytes(int fmt)
{
    switch (fmt) {
    case AUDIO_FMT_S24_LE:      return 4;
    case AUDIO_FMT_S24_3LE:     return 3;
    case AUDIO_FMT_S16_LE:      return 2;
    case AUDIO_FMT_SUBFRAME_LE: return 4;
    default:                    return 0;
    }
}

int s24pack_parse(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return AUDIO_FMT_S24_LE;
    if (strcasecmp(name, "s24_le") == 0 || strcasecmp(name, "s24le") == 0)
        return AUDIO_FMT_S24_LE;
    if (strcasecmp(name, "s24_3le") == 0 || strcasecmp(name, "s24_3") == 0)
        return AUDIO_FMT_S24_3LE;
    if (strcasecmp(name, "s16_le") == 0 || strcasecmp(name, "s16") == 0)
        return AUDIO_FMT_S16_LE;
    /* The vc4 HDMI DMA's format. "subframe" because that is what the device asks
     * for (IEC958_SUBFRAME_LE); the iec958 spelling is accepted because that is
     * the name an operator is likelier to arrive with. */
    if (strcasecmp(name, "subframe_le") == 0 || strcasecmp(name, "subframe") == 0 ||
        strcasecmp(name, "iec958_subframe_le") == 0)
        return AUDIO_FMT_SUBFRAME_LE;
    return -1;
}

/* Even parity over bits 4..30 of a subframe — the count that bit 31 completes,
 * so that the whole word carries an even number of set bits. This is the same
 * sum alsa-lib's iec958_parity() takes, and computing it is what makes the word
 * a *valid* IEC958 subframe rather than a container with a sample somewhere in
 * it. Branchless XOR fold; not __builtin_parity, which on ARMv7 is a libgcc
 * call on a per-sample path. */
static int subframe_parity(uint32_t v)
{
    v &= 0x7ffffff0U;
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1U);
}

unsigned s24pack(int fmt, const int32_t *src, unsigned frames, unsigned ch,
                 void *dst, unsigned dst_cap)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned i, n = frames * ch, need;

    if (n == 0)
        return 0;
    need = n * (unsigned)s24pack_bytes(fmt);
    if (need == 0 || need > dst_cap)
        return 0;

    switch (fmt) {
    case AUDIO_FMT_S24_LE:
        /* The identity case: the scratch buffer already holds exactly these bytes,
         * because that is the layout rbp wrote into it. Verbatim, and deliberately
         * not clamped like the three below — see the S24PACK_SAMPLE_* comment in the
         * header for what a clamp here would do to an unextended word. */
        memcpy(dst, src, need);
        break;

    case AUDIO_FMT_S24_3LE:
        for (i = 0; i < n; i++) {
            uint32_t v = (uint32_t)src[i];
            d[i * 3 + 0] = (unsigned char)(v & 0xff);
            d[i * 3 + 1] = (unsigned char)((v >> 8) & 0xff);
            d[i * 3 + 2] = (unsigned char)((v >> 16) & 0xff);
        }
        break;

    case AUDIO_FMT_S16_LE:
        /* Drop the bottom 8 bits, i.e. truncate toward zero — not round. Rounding
         * would need the discarded bits and would still be inaudible; truncation
         * is what keeps this a shift rather than a branch in a per-sample loop. */
        for (i = 0; i < n; i++) {
            uint32_t v = (uint32_t)src[i];
            d[i * 2 + 0] = (unsigned char)((v >> 8) & 0xff);
            d[i * 2 + 1] = (unsigned char)((v >> 16) & 0xff);
        }
        break;

    case AUDIO_FMT_SUBFRAME_LE:
        /* A real IEC958 subframe, not a container: the sample goes at bits 4..27.
         * That is what alsa-lib's own encoder does — it is handed the sample
         * left-justified and its first two lines are `data >>= 4; data &= ~0xf;`
         * — and it is why a sample placed at bits 8..31 is not merely shifted:
         * the receiver reads 4..27, gets this sample's low 20 bits promoted to
         * the top, and plays a wrapped, aliased version of the waveform at full
         * level. Measured on the unit as exactly that: loud and very distorted,
         * which is how S4.6 found it. The parity bit is then set so the word is
         * a valid subframe; the preamble (bits 0..3) and V/U/C (28..30) are left
         * zero, which is a decision the header explains. Written per byte rather
         * than as a 32-bit store because the buffer is assembled into a byte
         * array and this target is little-endian with no alignment promise. */
        for (i = 0; i < n; i++) {
            uint32_t v = ((uint32_t)src[i] << 4) & 0x0ffffff0U;
            if (subframe_parity(v))
                v |= 0x80000000U;
            d[i * 4 + 0] = (unsigned char)(v & 0xff);
            d[i * 4 + 1] = (unsigned char)((v >> 8) & 0xff);
            d[i * 4 + 2] = (unsigned char)((v >> 16) & 0xff);
            d[i * 4 + 3] = (unsigned char)((v >> 24) & 0xff);
        }
        break;

    default:
        return 0;
    }
    return need;
}
