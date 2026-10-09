/*
 * vnc_rfb.c -- the RFB wire format. Pure; see vnc_rfb.h for why that matters.
 */
#include "vnc_rfb.h"

#include <stdio.h>
#include <string.h>

/* --- encodings ------------------------------------------------------------ */

const char *vnc_rfb_encoding_name(int32_t enc)
{
    switch (enc) {
    case VNC_ENC_RAW:      return "Raw";
    case VNC_ENC_COPYRECT: return "CopyRect";
    case VNC_ENC_RRE:      return "RRE";
    case VNC_ENC_CORRE:    return "CoRRE";
    case VNC_ENC_HEXTILE:  return "Hextile";
    case VNC_ENC_ZLIB:     return "zlib";
    case VNC_ENC_TIGHT:    return "Tight";
    case VNC_ENC_ZLIBHEX:  return "zlibhex";
    case VNC_ENC_TRLE:     return "TRLE";
    case VNC_ENC_ZRLE:     return "ZRLE";
    case VNC_ENC_ZYWRLE:   return "ZYWRLE";
    default:               return NULL;
    }
}

const char *vnc_rfb_pseudo_name(int32_t enc)
{
    switch (enc) {
    case VNC_ENC_XCURSOR:        return "XCursor";
    case VNC_ENC_RICHCURSOR:     return "RichCursor";
    case VNC_ENC_POINTERPOS:     return "PointerPos";
    case VNC_ENC_LASTRECT:       return "LastRect";
    case VNC_ENC_NEWFBSIZE:      return "NewFBSize";
    case VNC_ENC_EXT_DESKTOP:    return "ExtendedDesktopSize";
    case VNC_ENC_QUALITY_0:      return "QualityLevel0";
    case VNC_ENC_COMPRESS_0:     return "CompressLevel0";
    case VNC_ENC_FINE_QUALITY_0: return "FineQualityLevel0";
    case VNC_ENC_SUBSAMP_1X:     return "Subsamp1X";
    default:                     return NULL;
    }
}

/* Did the client ask for a JPEG quality level -- that is, did it opt in to a LOSSY
 * Tight payload?
 *
 * THE WIDTHS MATTER NOW, which they did not when this only decorated a log line. The
 * spec makes the answer a precondition for sending JPEG at all (see client_jpeg_ok in
 * vnc_session.c), so a rung this misses is a client that is needlessly denied the
 * hardware encoder. Both ladders are contiguous upward from their base and the bases
 * are far apart, so a range test cannot confuse them: ten ordinary levels off -32, and
 * the fine-grained ladder's ONE HUNDRED AND ONE rungs off -512 -- not sixteen, which
 * is what this said while it did not matter. */
int vnc_rfb_pseudo_is_quality(int32_t enc)
{
    if (enc >= VNC_ENC_QUALITY_0      && enc <= VNC_ENC_QUALITY_0 + 9)   return 1;
    if (enc >= VNC_ENC_FINE_QUALITY_0 && enc <= VNC_ENC_FINE_QUALITY_0 + 100) return 1;
    return 0;
}

/* See the header for the three rules and where each one is written down. Written as
 * a predicate rather than inline at the two call sites because the failure it guards
 * is a silent wait on the client, which no amount of testing the happy path finds. */
int vnc_rfb_sends_security_result(int minor, int sec_type)
{
    if (sec_type == VNC_SEC_VNC)
        return 1;
    return minor >= 8;
}

/* --- version -------------------------------------------------------------- */

static int three_digits(const char *p)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9')
        return -1;
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

int vnc_rfb_parse_version(const char raw[VNC_RFB_VERSION_LEN], int *major, int *minor)
{
    int maj, min;

    if (memcmp(raw, "RFB ", 4) != 0) return 0;
    if (raw[7] != '.' || raw[11] != '\n') return 0;
    maj = three_digits(raw + 4);
    min = three_digits(raw + 8);
    if (maj < 0 || min < 0) return 0;

    if (major) *major = maj;
    if (minor) *minor = min;
    return 1;
}

/* --- pixel format --------------------------------------------------------- */

const struct vnc_pixel_format vnc_pf_rgb565 = {
    .bits_per_pixel = 16,
    .depth          = 16,
    .big_endian     = 0,
    .true_colour    = 1,
    .red_max        = 31,
    .green_max      = 63,
    .blue_max       = 31,
    .red_shift      = 11,
    .green_shift    = 5,
    .blue_shift     = 0,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);   /* RFB fields are big-endian on the wire, always,
                                 * whichever way the pixels underneath are ordered. */
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

void vnc_pf_write(uint8_t out[16], const struct vnc_pixel_format *pf)
{
    out[0] = pf->bits_per_pixel;
    out[1] = pf->depth;
    out[2] = pf->big_endian;
    out[3] = pf->true_colour;
    put16(out + 4,  pf->red_max);
    put16(out + 6,  pf->green_max);
    put16(out + 8,  pf->blue_max);
    out[10] = pf->red_shift;
    out[11] = pf->green_shift;
    out[12] = pf->blue_shift;
    out[13] = out[14] = out[15] = 0;   /* padding[3] */
}

void vnc_pf_read(const uint8_t in[16], struct vnc_pixel_format *pf)
{
    pf->bits_per_pixel = in[0];
    pf->depth          = in[1];
    pf->big_endian     = in[2];
    pf->true_colour    = in[3];
    pf->red_max        = get16(in + 4);
    pf->green_max      = get16(in + 6);
    pf->blue_max       = get16(in + 8);
    pf->red_shift      = in[10];
    pf->green_shift    = in[11];
    pf->blue_shift     = in[12];
}

void vnc_pf_describe(const struct vnc_pixel_format *pf, char *buf, size_t n)
{
    snprintf(buf, n,
             "%ubpp depth %u %s %s r%ug%ub%u (max %u/%u/%u, shift %u/%u/%u)",
             pf->bits_per_pixel, pf->depth,
             pf->big_endian ? "BE" : "LE",
             pf->true_colour ? "truecolour" : "colourmap",
             pf->red_max, pf->green_max, pf->blue_max,
             pf->red_max, pf->green_max, pf->blue_max,
             pf->red_shift, pf->green_shift, pf->blue_shift);
}

int vnc_pf_is_rgb565_le(const struct vnc_pixel_format *pf)
{
    return pf->bits_per_pixel == 16 &&
           pf->depth          == 16 &&
           pf->big_endian     == 0  &&
           pf->true_colour    == 1  &&
           pf->red_max        == 31 &&
           pf->green_max      == 63 &&
           pf->blue_max       == 31 &&
           pf->red_shift      == 11 &&
           pf->green_shift    == 5  &&
           pf->blue_shift     == 0;
}

/* --- Tight JPEG framing --------------------------------------------------- */

int vnc_rfb_write_compact_len(uint8_t *out, uint32_t len)
{
    int n = 0;

    out[n++] = (uint8_t)(len & 0x7F);
    if (len > 0x7F) {
        out[n - 1] |= 0x80;
        out[n++] = (uint8_t)((len >> 7) & 0x7F);
        if (len > 0x3FFF) {
            out[n - 1] |= 0x80;
            out[n++] = (uint8_t)((len >> 14) & 0xFF);   /* a FULL byte, not 7 bits */
        }
    }
    return n;
}

int vnc_rfb_jpeg_header(uint8_t *out, uint32_t jpeg_len)
{
    out[0] = (uint8_t)(VNC_TIGHT_JPEG << 4);
    return 1 + vnc_rfb_write_compact_len(out + 1, jpeg_len);
}

int vnc_rfb_tight_zlib_header(uint8_t *out, uint32_t zlen)
{
    out[0] = VNC_TIGHT_STREAM0;
    return 1 + vnc_rfb_write_compact_len(out + 1, zlen);
}

int vnc_rfb_zlib_rect_header(uint8_t *out, uint32_t zlen)
{
    /* Four bytes, big-endian, and nothing else -- no control byte, no compact
     * length. See the note in vnc_rfb.h. */
    out[0] = (uint8_t)(zlen >> 24);
    out[1] = (uint8_t)(zlen >> 16);
    out[2] = (uint8_t)(zlen >> 8);
    out[3] = (uint8_t)zlen;
    return 4;
}

/* --- pixel conversion ----------------------------------------------------- */

/* Stretch a v-bit field across the client's 0..max, and put it where the client
 * wants it. Integer arithmetic, and the rounding is the point: (v * max) / (2^bits
 * - 1) rounds down, so the top value lands on max exactly and every other value is
 * at most one step low. Scaling the other way -- shifting the 5-bit value up by 3,
 * which is the tempting one-liner -- leaves the brightest red at 248 of 255 and the
 * whole picture visibly dark. */
static uint32_t scale_shift(unsigned v, unsigned vmaxbits, unsigned cmax, unsigned shift)
{
    unsigned vmax = (1u << vmaxbits) - 1u;
    uint32_t scaled = (uint32_t)((v * cmax + vmax / 2) / vmax);
    return scaled << shift;
}

/* Does a channel's maximum fit in the bytes left above its shift?
 *
 * The `bits >= 32` guard is not decoration. The natural way to write this is
 * `cmax > (1u << (8*bytes - shift)) - 1u`, and for the commonest client format there
 * is -- 32 bpp with blue at shift 0 -- that evaluates `1u << 32`, which is undefined
 * behaviour and in practice a 1, so the check became `255 > 0` and every 32-bpp
 * client was refused before its first frame. */
static int channel_fits(unsigned cmax, unsigned shift, int bytes)
{
    unsigned bits = (unsigned)(8 * bytes) - shift;
    if (bits >= 32)
        return 1;
    return cmax <= ((1u << bits) - 1u);
}

int vnc_conv_init(struct vnc_conv *cv, const struct vnc_pixel_format *pf)
{
    unsigned i;

    if (!pf->true_colour)
        return -1;
    switch (pf->bits_per_pixel) {
    case 8:  cv->bytes = 1; break;
    case 16: cv->bytes = 2; break;
    case 24: cv->bytes = 3; break;
    case 32: cv->bytes = 4; break;
    default: return -1;
    }
    /* A pixel must fit in the bytes carrying it, or we would silently drop the top
     * channel. 16bpp with red_max 255 is not a format anyone should ask for, and
     * answering it with a wrong picture is worse than refusing. */
    if (!channel_fits(pf->red_max,   pf->red_shift,   cv->bytes)) return -1;
    if (!channel_fits(pf->green_max, pf->green_shift, cv->bytes)) return -1;
    if (!channel_fits(pf->blue_max,  pf->blue_shift,  cv->bytes)) return -1;

    cv->big_endian = pf->big_endian ? 1 : 0;
    for (i = 0; i < 32; i++)
        cv->lut_r[i] = scale_shift(i, 5, pf->red_max, pf->red_shift);
    for (i = 0; i < 64; i++)
        cv->lut_g[i] = scale_shift(i, 6, pf->green_max, pf->green_shift);
    for (i = 0; i < 32; i++)
        cv->lut_b[i] = scale_shift(i, 5, pf->blue_max, pf->blue_shift);
    return 0;
}

void vnc_conv_rows(uint8_t *dst, const uint16_t *src, size_t n, const struct vnc_conv *cv)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint16_t px = src[i];
        uint32_t v = cv->lut_r[(px >> 11) & 0x1F] |
                     cv->lut_g[(px >> 5)  & 0x3F] |
                     cv->lut_b[px & 0x1F];
        switch (cv->bytes) {
        case 1: dst[i] = (uint8_t)v; break;
        case 2:
            if (cv->big_endian) { dst[2*i] = (uint8_t)(v >> 8); dst[2*i+1] = (uint8_t)v; }
            else                { dst[2*i] = (uint8_t)v; dst[2*i+1] = (uint8_t)(v >> 8); }
            break;
        case 3:
            if (cv->big_endian) {
                dst[3*i] = (uint8_t)(v >> 16); dst[3*i+1] = (uint8_t)(v >> 8); dst[3*i+2] = (uint8_t)v;
            } else {
                dst[3*i] = (uint8_t)v; dst[3*i+1] = (uint8_t)(v >> 8); dst[3*i+2] = (uint8_t)(v >> 16);
            }
            break;
        default:
            if (cv->big_endian) {
                dst[4*i] = (uint8_t)(v >> 24); dst[4*i+1] = (uint8_t)(v >> 16);
                dst[4*i+2] = (uint8_t)(v >> 8); dst[4*i+3] = (uint8_t)v;
            } else {
                dst[4*i] = (uint8_t)v; dst[4*i+1] = (uint8_t)(v >> 8);
                dst[4*i+2] = (uint8_t)(v >> 16); dst[4*i+3] = (uint8_t)(v >> 24);
            }
            break;
        }
    }
}
