/*
 * s24pack.c — see s24pack.h. Two loops and a memcpy; the value is in being
 * separable, so the byte-level contract can be tested without a sound card.
 */
#define _GNU_SOURCE
#include "s24pack.h"

#include <string.h>
#include <strings.h>

int s24pack_bytes(int fmt)
{
    switch (fmt) {
    case AUDIO_FMT_S24_LE:  return 4;
    case AUDIO_FMT_S24_3LE: return 3;
    case AUDIO_FMT_S16_LE:  return 2;
    default:                return 0;
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
    return -1;
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
        /* The identity case: the scratch buffer already holds exactly these
         * bytes, because that is the layout rbp wrote into it. */
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

    default:
        return 0;
    }
    return need;
}
