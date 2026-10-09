/*
 * vnc_rfb.h -- the RFB (VNC) wire format, as bytes.
 *
 * This module knows the protocol and nothing else: no sockets, no /dev, no
 * allocation. Every function is a pure function of its arguments writing into a
 * caller-supplied buffer, which is what lets the whole of it be pinned by a host
 * test (test_vnc_rfb.c) rather than by watching a client.
 *
 * THAT MATTERS MORE THAN USUAL HERE. RFB has two classes of mistake. The first is
 * loud: a bad magic, a refused security type, a client that hangs up. The second is
 * silent -- a length field written with the wrong shape desynchronises the stream,
 * and the client then decodes the *next* message out of the middle of this one. It
 * does not error; it renders garbage, or waits forever. The rectangle framing below
 * was got wrong once on the way to this file (see vnc_rfb_jpeg_header) precisely
 * because it had been reasoned about rather than read, so it is read out of
 * libvncserver's own encoder and decoder and pinned by a test.
 */
#ifndef RBPI4B_VNC_RFB_H
#define RBPI4B_VNC_RFB_H

#include <stddef.h>
#include <stdint.h>

/* --- the version string ----------------------------------------------------
 *
 * The SERVER SPEAKS FIRST in RFB: it sends these 12 bytes, and only then does the
 * client answer with its own. We offer 3.8 -- the last version whose security
 * negotiation is the one implemented here (a type list, a chosen type, a
 * SecurityResult), and the version every current client settles on even when it
 * names something higher.
 *
 * macOS's Screen Sharing answers "RFB 003.889\n". 889 > 8, so the *effective*
 * protocol is 3.8 and it is served here without special-casing -- but the raw
 * string is kept for the log, because the probe's whole job is to see what Apple
 * actually says rather than what it is documented to say. */
#define VNC_RFB_VERSION     "RFB 003.008\n"
#define VNC_RFB_VERSION_LEN 12

/* Security types. None (1) is unauthenticated and macOS's Screen Sharing refuses it
 * outright -- it reads the list, finds nothing it will take, and closes the socket.
 * VNC auth (2) is the standard DES challenge/response and is what that client will
 * accept. Apple's own schemes are 30/33/35 and are not implemented here. */
#define VNC_SEC_NONE 1
#define VNC_SEC_VNC  2

/* --- encodings -------------------------------------------------------------
 *
 * The positive numbers are real encodings and the negatives are pseudo-encodings
 * (capabilities and knobs, not pixel formats). They are written as signed int32
 * because that is how they appear on the wire inside SetEncodings, and the two
 * ranges cannot collide. */
#define VNC_ENC_RAW      0
#define VNC_ENC_COPYRECT 1
#define VNC_ENC_RRE      2
#define VNC_ENC_CORRE    4
#define VNC_ENC_HEXTILE  5
#define VNC_ENC_ZLIB     6
#define VNC_ENC_TIGHT    7
#define VNC_ENC_ZLIBHEX  8
#define VNC_ENC_TRLE     15
#define VNC_ENC_ZRLE     16
#define VNC_ENC_ZYWRLE   17

#define VNC_ENC_QUALITY_0      (-32)  /* .. -23, ten JPEG quality levels        */
#define VNC_ENC_COMPRESS_0     (-256) /* .. -247, ten zlib levels               */
#define VNC_ENC_FINE_QUALITY_0 (-512) /* .. -412, the fine-grained JPEG ladder:  */
                                      /* 101 rungs, 0..100, not sixteen           */
#define VNC_ENC_SUBSAMP_1X     (-768) /* .. -767, chroma subsampling            */
#define VNC_ENC_XCURSOR        (-240)
#define VNC_ENC_RICHCURSOR     (-239)
#define VNC_ENC_POINTERPOS     (-232)
#define VNC_ENC_LASTRECT       (-224)
#define VNC_ENC_NEWFBSIZE      (-223)
#define VNC_ENC_EXT_DESKTOP    (-308)

/* --- Tight rectangles' compression-control byte -----------------------------
 *
 * rfbproto.h's own comment block, which is the authority here -- RFC 6143 does NOT
 * specify Tight at all, listing encoding 7 as a historic assignment and stopping:
 *
 *   bit 0:    if 1, then compression stream 0 should be reset;
 *   bit 1:    if 1, then compression stream 1 should be reset;
 *   bit 2:    if 1, then compression stream 2 should be reset;
 *   bit 3:    if 1, then compression stream 3 should be reset;
 *   bits 7-4: if 1000 (0x08), then the compression type is "fill",
 *             if 1001 (0x09), then the compression type is "jpeg",
 *             if 1010 (0x0A), then the compression type is "basic" and no Zlib
 *               compression was used,
 *             if 0xxx, then the compression type is "basic" and Zlib compression
 *               was used,
 *   and for 0xxx:  bits 5-4 are the zlib stream index, bit 6 says a filter-id byte
 *                  follows.
 *
 * So the JPEG byte below, 0x09 << 4 = 0x90, is confirmed by the document rather than
 * by the reasoning that first produced it -- and the stream-0 zlib byte is 0x00. */
#define VNC_TIGHT_FILL  0x08
#define VNC_TIGHT_JPEG  0x09
#define VNC_TIGHT_NOZLIB 0x0A

/* Basic compression, zlib stream 0, NO RESET.
 *
 * The reset bits are deliberately left clear. A Tight client keeps one inflate
 * stream per stream index for the life of the connection, and libvncserver -- the
 * server half of the x11vnc sessions macOS Screen Sharing joins every day -- never
 * sets a reset bit; it retunes a live stream with deflateParams instead. Matching
 * that is what makes this byte the exercised one rather than a plausible one. It
 * also means the encoder side must keep its stream alive across rectangles, which
 * vnc_deflate does.
 *
 * NOT TO BE CONFUSED WITH rfbTightNoZlib (0x0A << 4 = 0xA0), which says the opposite:
 * that this rectangle's data is raw and no zlib stream is involved at all. */
#define VNC_TIGHT_STREAM0 0x00

/* The canonical name for a positive encoding, or NULL if it is one we do not
 * know. For a negative value, use vnc_rfb_pseudo_name. A NULL is a fact the probe
 * prints -- an unknown number is not a reason to guess a name. */
const char *vnc_rfb_encoding_name(int32_t enc);

/* The canonical name for a pseudo-encoding, or NULL if unknown. */
const char *vnc_rfb_pseudo_name(int32_t enc);

/* Is this pseudo-encoding one of the JPEG-quality ladder (either libvncserver's
 * -32 base or TurboVNC's -512 base)? A client that sends one has told us it wants
 * JPEG, which is the second half of the go/no-go for the hardware encoder --
 * advertising Tight alone is not the same promise. */
int vnc_rfb_pseudo_is_quality(int32_t enc);

/* Does the server send a SecurityResult word once this security phase ends?
 * `minor` is the settled protocol minor version and `sec_type` the security type
 * the session actually uses (VNC_SEC_NONE or VNC_SEC_VNC).
 *
 * This is THREE rules, not one, and RFC 6143 states them in two different places
 * -- section 7.1.3 for 3.8 and appendices A.1/A.2 for 3.3 and 3.7:
 *
 *     type None, 3.3 or 3.7   NO word  -- "the server does not send the
 *                                        SecurityResult message but proceeds
 *                                        directly to the initialization messages"
 *     type None, 3.8          a word   -- section 7.1.3, no exception for None
 *     VNC auth, ANY version   a word
 *
 * THE LAST LINE IS THE ONE THAT IS EASY TO GET WRONG, and it was wrong here: the
 * 3.3 path was written from the belief that "3.3 has no SecurityResult" -- which
 * is true only of the None case it was read from. A 3.3 session that authenticates
 * with a password gets the word. A client that never receives it does not error;
 * it waits. macOS's Screen Sharing sat on the challenge for the full fifteen
 * seconds and never sent ClientInit, which on the glass is a connection that
 * spins forever with nothing in the server's log but silence. */
int vnc_rfb_sends_security_result(int minor, int sec_type);

/* --- version ---------------------------------------------------------------
 *
 * Parse a 12-byte "RFB 003.xxx\n". Returns 1 and fills the out-parameters on
 * success, 0 on anything else -- a client that speaks something we cannot read is a
 * fact for the log, not a parse error to paper over. */
int vnc_rfb_parse_version(const char raw[VNC_RFB_VERSION_LEN], int *major, int *minor);

/* --- pixel format ----------------------------------------------------------
 *
 * Exactly the 16 bytes a ServerInit carries and a SetPixelFormat sends back.
 * Kept as a struct rather than a buffer because every field is read by name in
 * the log, and a mis-set shift or max is invisible until the picture is wrong. */
struct vnc_pixel_format {
    uint8_t  bits_per_pixel;
    uint8_t  depth;
    uint8_t  big_endian;
    uint8_t  true_colour;
    uint16_t red_max, green_max, blue_max;
    uint8_t  red_shift, green_shift, blue_shift;
};

/* fb0's own format: 16 bpp RGB565, red at bit 11, green at 5, blue at 0,
 * little-endian. Advertising *this* is what makes a Raw rectangle a plain memcpy
 * on this hardware -- the framebuffer is already in the format the client is told
 * to expect, with a 1280-pixel stride and no padding. */
extern const struct vnc_pixel_format vnc_pf_rgb565;

void vnc_pf_write(uint8_t out[16], const struct vnc_pixel_format *pf);
void vnc_pf_read(const uint8_t in[16], struct vnc_pixel_format *pf);

/* Human-readable one-line form for the log: "16bpp depth 16 truecolour LE
 * r5g6b5 (max 31/63/31, shift 11/5/0)" -- every field, in the order the struct
 * declares them, so a client's request can be read off against ours at a glance. */
void vnc_pf_describe(const struct vnc_pixel_format *pf, char *buf, size_t n);

/* Does this format describe the framebuffer's own layout, so a Raw rect can be
 * memcpy'd with no conversion at all? Bits, depth, byte order, truecolour and all
 * three shifts/maxes must agree. */
int vnc_pf_is_rgb565_le(const struct vnc_pixel_format *pf);

/* --- Tight JPEG rectangle framing ------------------------------------------
 *
 * A Tight rectangle whose data is a JPEG is, byte for byte:
 *
 *   0x90                 compression-control = rfbTightJpeg << 4. The decoder peels
 *                        the low nibble off first (four stream flags), leaving 0x09,
 *                        which it then compares BY EQUALITY against rfbTightJpeg --
 *                        so 0x90, exactly, and not 0x09.
 *   <compact length>     1-3 bytes, 7 bits each, LSB first, bit 7 = "another byte
 *                        follows". The third byte carries a full 8 bits, so the
 *                        encoding is only 7-bit for the first two.
 *   <JPEG bytes>         exactly that many. No zlib, no palette, no filter byte.
 *
 * A 40 000-byte JPEG goes out as 90 C0 B8 02. libvncserver writes the control byte
 * at src/libvncserver/tight.c and the compact length at rfbSendCompressedDataTight;
 * its client reads both back at src/libvncclient/tight.c (ReadCompactLen). */
int vnc_rfb_write_compact_len(uint8_t *out, uint32_t len);

/* Write just the control byte and the compact length. Returns the number of bytes
 * written (4 for a 40 000-byte JPEG); the caller appends the JPEG itself. `out`
 * needs at least 4 bytes. */
int vnc_rfb_jpeg_header(uint8_t *out, uint32_t jpeg_len);

/* The same for a zlib "basic compression" rectangle on stream 0: the control byte
 * 0x00 and the compact length of the compressed bytes that follow. `out` needs at
 * least 4 bytes; returns the header's length. */
int vnc_rfb_tight_zlib_header(uint8_t *out, uint32_t zlen);

/* --- encoding 6, "zlib" -------------------------------------------------------
 *
 * THE ENCODING macOS's SCREEN SHARING ACTUALLY ASKS FOR, and the one this server
 * did not implement until 2026-10-09 -- which is why a real Screen Sharing session
 * ran on Raw, 4 MB a frame, for the whole life of this program up to that point.
 * Apple's client does NOT offer Tight. It offers, in this order:
 *
 *     zlib (6), ZRLE (16), ... pseudo-encodings ... and NOT Raw and NOT Tight
 *
 * so the Tight path built the day before never once ran for the operator's client.
 * (The `Raw, Tight, NewFBSize` list recorded in docs/19-vnc.md as "Apple's" is this
 * repo's own test client's -- rfbclient.py sends exactly those three.)
 *
 * A zlib rectangle is the Tight zlib rectangle with the wrapper removed: the 12-byte
 * rectangle header carries encoding 6 and then, INSTEAD of a control byte and a
 * compact length, a plain 4-byte BIG-ENDIAN length of the compressed bytes that
 * follow. Everything else is identical, which is why this reuses `vnc_zlib.c` and
 * the band machinery unchanged: ONE deflate stream per connection, never reset,
 * Z_SYNC_FLUSH at the end of each rectangle, and the uncompressed input is exactly
 * the bytes a Raw rectangle of the same size would have carried.
 *
 * RFC 6143 does NOT specify this either -- it lists encoding 6 as a historic IANA
 * assignment and stops -- so the authority is libvncserver, which both writes and
 * reads it: `hdr.nBytes = Swap32IfLE(...)` at src/libvncserver/zlib.c, and
 * `rfbClientSwap32IfLE(hdr.nBytes)` at src/libvncclient/zlib.c, each behind a
 * `decompStreamInited` flag that is set once and never cleared.
 *
 * `out` needs 4 bytes; returns 4. */
int vnc_rfb_zlib_rect_header(uint8_t *out, uint32_t zlen);

/* --- turning fb0's pixels into the client's pixels --------------------------
 *
 * A Raw rectangle means "these pixels, in the format you were told to expect".
 * If the client accepted the RGB565 we advertised in ServerInit, that is a memcpy
 * and nothing here is used. Apple's Screen Sharing does not: it sends a
 * SetPixelFormat asking for 32 bpp before it draws anything, so the server has to
 * be able to produce that too, and -- since the format is entirely the client's
 * choice -- anything else it might name.
 *
 * So this is a general converter, built through three lookup tables of 32, 64 and
 * 32 entries. The tables are built once from the client's shifts and maxima, and
 * then every pixel is three array reads and an OR. A per-pixel multiply-and-shift
 * version is the obvious way to write this and costs about 3 million multiplies a
 * frame at 1280x800; the tables cost none.
 *
 * The scaling is the one every VNC server uses -- the 5- or 6-bit value is stretched
 * across the client's own maximum, so max-to-max, and 0-to-0. A client asking for
 * 24-bit colour gets the full range filled rather than the dark image a plain shift
 * would give it. */
struct vnc_conv {
    int bytes;                      /* 1, 2, 3 or 4 per pixel */
    int big_endian;
    uint32_t lut_r[32];             /* already shifted and scaled */
    uint32_t lut_g[64];
    uint32_t lut_b[32];
};

/* Build the tables for a client's format. Returns 0 on success; -1 if the format is
 * one this cannot produce (not true colour, or a bits-per-pixel it has no packing
 * for), in which case the client must be refused rather than sent a wrong picture. */
int vnc_conv_init(struct vnc_conv *cv, const struct vnc_pixel_format *pf);

/* Convert `n` pixels. `src` is little-endian RGB565 in 16-bit words -- fb0's own
 * layout, straight out of the capture -- and `dst` needs n * bytes. */
void vnc_conv_rows(uint8_t *dst, const uint16_t *src, size_t n, const struct vnc_conv *cv);

#endif /* RBPI4B_VNC_RFB_H */
