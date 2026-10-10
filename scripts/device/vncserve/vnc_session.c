/*
 * vnc_session.c -- see vnc_session.h.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vnc_capture.h"
#include "vnc_des.h"
#include "vnc_diff.h"
#include "vnc_http.h"
#include "vnc_input.h"
#include "vnc_jpeg.h"
#include "vnc_live.h"
#include "vnc_mode.h"
#include "vnc_net.h"
#include "vnc_rfb.h"
#include "vnc_session.h"
#include "vnc_zlib.h"

#define VNC_MAX_CLIENTS  4

/* THE SCREEN, BEFORE ANYBODY HAS OPENED ONE. While sharing is off there is no capture
 * and therefore no geometry to ask for, but the control page still prints a size and
 * the preview <img> still wants an aspect ratio -- so these stand in until the first
 * vnc_capture_open, whose width and height replace them. They are the panel's real
 * numbers, which is what makes the page look right in the off state rather than
 * merely being non-empty. */
#define VNC_FALLBACK_W   1280
#define VNC_FALLBACK_H   800

/* A CLIENT THAT HAS BEEN SENT THIS MUCH AND HAS NOT TAKEN IT IS NOT GOING TO.
 * Twelve megabytes is two whole 32-bpp frames: a link that is merely slow is never
 * cut, but a laptop that has gone to sleep cannot make this process grow until the
 * unit swaps. Without a cap the frame loop keeps composing into a queue nobody
 * reads, which on a live player is the one failure that actually matters. */
#define VNC_OUT_CAP      (12u * 1024u * 1024u)

/* Big enough for any SetEncodings a real client sends -- Apple's is around ten
 * entries -- and small enough to be a stack-sized per-client buffer. A client that
 * wants to name more than four thousand encodings is not one to serve. */
#define VNC_MSG_CAP      16384

/* Per-state give-up times, in milliseconds. CS_AUTH is the long one and it is the
 * reason none of this is a blocking read: that is a human typing a password. */
#define VNC_T_HELLO     15000
#define VNC_T_SECCHOICE 15000
#define VNC_T_AUTH     300000
#define VNC_T_CINIT     15000

enum {
    CS_HELLO = 0,     /* our version is out; theirs is due          */
    CS_SECCHOICE,     /* 3.7+: a list is out; the chosen type is due */
    CS_AUTH,          /* the challenge is out; 16 bytes are due      */
    CS_CINIT,         /* the ClientInit flag is due                  */
    CS_READY          /* ServerInit is out; messages from here on    */
};

struct client {
    int fd;
    char peer[64];
    int state;
    int minor;                 /* the EFFECTIVE protocol minor: min(ours, theirs) */
    unsigned long long since;  /* when the current state began */
    int used;                  /* slot in use */

    /* The client's pixels, once it has told us. Until then it is our advertised
     * format, which is what ServerInit promised and what a client that never sends
     * SetPixelFormat is entitled to. */
    struct vnc_pixel_format pf;
    int pf_set;
    struct vnc_conv conv;
    int conv_ok;               /* the format is one vnc_conv_init accepted */
    int tight_seen;            /* the client offered Tight (7)                */
    int zlib_seen;             /* the client offered zlib (6) -- see vnc_rfb.h */
    int quality_seen;

    /* The input stream, parsed strictly in order -- which is what makes the
     * handshake a switch statement rather than four rounds of reads. */
    uint8_t in[VNC_MSG_CAP];
    size_t inlen;
    uint32_t discard;          /* bytes of a ClientCutText still to skip */

    int want_update;
    int incremental;
    int got_frame;
    unsigned long long sent_serial;   /* which frame this client last received */

    /* The last button mask this client sent, so that a click is two lines in the log
     * rather than one line per PointerEvent -- a drag is a hundred reports and none of
     * them is worth a line. */
    int ptr_mask;
    unsigned long long ptr_presses;

    /* THE CHALLENGE IS PER CLIENT AND NOT PER SERVER. Two clients connecting at once
     * share this process, and a single global challenge would be overwritten by the
     * second connection while the first is at its password prompt -- so the first
     * client's (perfectly correct) password would be checked against a challenge it
     * was never sent, and rejected. */
    uint8_t challenge[16];
    uint8_t expected[16];

    uint8_t *out;
    size_t outcap, outlen, outoff;

    uint8_t *rowbuf;           /* one converted scanline, for a non-RGB565 client */
    size_t rowcap;

    /* --- the compressed path, shared by Tight (7) and zlib (6).
     *
     * THE TWO ENCODINGS DIFFER ONLY IN THEIR WRAPPER. Both are the same deflate
     * stream over the same banded pixels; Tight puts a control byte and a compact
     * length in front of each rectangle and zlib puts a plain 4-byte big-endian
     * length there instead. So the state below serves whichever one a client
     * offered, and the send functions differ only in the header they write.
     *
     * `prev` is THE FRAME THIS CLIENT WAS LAST SENT, and it is per client rather than
     * shared because two viewers can be a frame or ten apart, and the bands have to
     * be computed against what each one is actually looking at. 2 MB a client, which
     * is the price of not sending a whole rectangle full of pixels that did not move.
     *
     * `z` is this client's zlib stream -- one per client, never reset, which is what
     * libvncserver does for BOTH encodings and therefore what every real client has
     * been tested against. See vnc_zlib.h. */
    struct vnc_deflate *z;
    uint16_t *prev;
    int prev_ok;
    int comp_ok;              /* the stream exists and this client offered 7 or 6 */
    int comp_failed;          /* said so once, then stopped trying */
    struct vnc_band bands[VNC_BAND_MAX];

    unsigned long long frames, bytes;
    unsigned long long comp_out;   /* compressed bytes this client has been sent */
};

static struct client g_clients[VNC_MAX_CLIENTS];

/* --- the input, for the message parser to reach ----------------------------- *
 *
 * ONE PANEL, ONE PROCESS, ONE POINTER. There is exactly one of each here, so this is
 * file scope rather than a field of the client: the client that is pressing is not
 * the thing being pressed. `g_input_owner` is the slot that last put the finger down,
 * and it exists for one reason -- when that client goes away, the finger has to come
 * up, and only the client that pressed knows whether the press was ever released.
 *
 * ALL THREE ARE SET ONCE, BEFORE THE LOOP, and left alone: `g_input_sw->on` is the
 * switch the loop re-reads each turn, so the gate below is live rather than a copy. */
static struct vnc_input *g_input;
static struct vnc_input_switch *g_input_sw;
static int g_input_owner = -1;

/* Can this client be sent a JPEG at all?
 *
 * BOTH halves are required, and the second one is the whole reason macOS's Screen
 * Sharing refused to stay connected. The RFB specification's Tight-encoding section
 * says, in as many words:
 *
 *   "JpegCompression may only be used when bits-per-pixel is either 16 or 32 and the
 *    client has advertized a quality level using the JPEG Quality Level
 *    Pseudo-encoding."
 *
 * Advertising Tight is a claim about the *container*; the quality-level pseudo-encoding
 * is the client separately opting in to a payload that is LOSSY. A server that consults
 * the quality level alone -- which this one did -- breaks that rule on its very first
 * update. The client noticed and hung up 16 ms later, and macOS's only remark about it
 * was its generic "make sure Screen Sharing or Remote Management is enabled on the
 * remote computer", which names the wrong machine and the wrong cause.
 *
 * THE PAIR IS NOW DOUBLY OUT OF REACH FOR macOS, and the second half was only learned
 * on 2026-10-09 from a live session: Screen Sharing offers no JPEG quality level *and
 * no Tight*, so it fails both halves independently. And there is no way around it,
 * because a JPEG has exactly one container in RFB -- a Tight rectangle -- and a client
 * that does not offer Tight cannot be sent one. The consequence is worth stating
 * plainly, because it is not a bug to be fixed later: there is NO way to put the
 * hardware JPEG through a Screen Sharing session, and the control page's own preview is
 * where the encoder can be seen. */
static int client_jpeg_ok(const struct client *c)
{
    return c->tight_seen && c->quality_seen;
}

/* The compressing encoding this client will be sent, or -1 if it can be sent none --
 * in which case the caller sends Raw.
 *
 * ZLIB (6) IS PREFERRED OVER TIGHT (7), AND THAT ORDER IS A MEASUREMENT, NOT A TASTE.
 * The two containers carry the same deflate stream, and that stream is a ZLIB stream in
 * the RFC1950 sense (windowBits 15 -- see vnc_zlib.c). That is exactly what encoding 6
 * wants and exactly what Tight's BASIC compression does not: Tight is raw deflate. So a
 * client that offers both and is handed Tight is given bytes it cannot inflate, and the
 * only symptom is the client hanging up.
 *
 * Measured 2026-10-10 on this unit, from vncserve's own log: macOS Screen Sharing, which
 * offers zlib and never Tight, held one session from 08:34 onward ("first frame sent
 * (181601 bytes, zlib (encoding 6), lossless)"). Every noVNC session -- which offers
 * both, so was given Tight before this change -- was closed BY THE CLIENT 66-94 ms after
 * its first frame: eight for eight, uniformly, with nothing logged, because the client
 * is the one that hangs up. Preferring zlib puts noVNC on the path macOS had already
 * proved, and it costs the feature nothing: the hardware JPEG, which is the whole reason
 * the web client exists, is chosen AHEAD of either container in the send loop -- and
 * JPEG rides in Tight, which is fine, because JPEG uses no deflate stream at all.
 *
 * STILL BROKEN, AND NOT FIXED HERE: a client that offers Tight and NOT zlib (a bare
 * TigerVNC) is still handed the zlib stream in a Tight container and will still fail to
 * inflate it. The cure is to emit raw deflate for the Tight arm, which needs a second
 * deflate mode in vnc_zlib.c and a Tight-only client to verify it against -- neither of
 * which this unit has. Nothing on this unit is in that class, so this order removes the
 * failure that is actually reachable. */
static int comp_encoding_for(const struct client *c,
                             const struct vnc_session_opts *o)
{
    if (o->zlib_level <= 0)
        return -1;                 /* RB_VNC_ZLIB_LEVEL=0: the switch is off */
    if (c->zlib_seen)
        return VNC_ENC_ZLIB;
    if (c->tight_seen)
        return VNC_ENC_TIGHT;
    return -1;
}

/* --- time ------------------------------------------------------------------ *
 * unsigned long long and not unsigned long: `long` is four bytes on this unit's
 * 32-bit userland, so a millisecond CLOCK_MONOTONIC kept in one wraps every 49
 * days -- which would be a mysterious once-a-month hang rather than a bug. */
static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
}

/* --- per-client output ------------------------------------------------------ */

static int out_reserve(struct client *c, size_t n)
{
    size_t live = c->outlen - c->outoff;

    if (live + n > VNC_OUT_CAP)
        return -1;
    if (c->outoff > 0 && (c->outoff == c->outlen || c->outcap - c->outlen < n)) {
        memmove(c->out, c->out + c->outoff, live);
        c->outlen = live;
        c->outoff = 0;
    }
    if (c->outcap - c->outlen < n) {
        size_t cap = c->outcap ? c->outcap : 65536;
        uint8_t *nb;
        while (cap - c->outlen < n)
            cap *= 2;
        nb = realloc(c->out, cap);
        if (!nb)
            return -1;
        c->out = nb;
        c->outcap = cap;
    }
    return 0;
}

static int out_push(struct client *c, const void *p, size_t n)
{
    if (out_reserve(c, n) < 0)
        return -1;
    memcpy(c->out + c->outlen, p, n);
    c->outlen += n;
    return 0;
}

/* Hand as much as the socket will take. Returns -1 if the peer is gone. */
static int out_drain(struct client *c)
{
    while (c->outoff < c->outlen) {
        ssize_t w = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_NOSIGNAL);
        if (w > 0) { c->outoff += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    c->outoff = c->outlen = 0;
    return 0;
}

static int out_pending(const struct client *c) { return c->outoff < c->outlen; }

/* --- reading --------------------------------------------------------------- */

static void consume(struct client *c, size_t n)
{
    memmove(c->in, c->in + n, c->inlen - n);
    c->inlen -= n;
}

/* Take whatever has arrived. Never blocks. 1 if bytes were read, 0 if there was
 * nothing to read, -1 if the peer is gone. */
static int slurp(struct client *c)
{
    ssize_t g;

    if (c->inlen == sizeof c->in)
        return 0;                       /* the parser will drop or consume */
    g = recv(c->fd, c->in + c->inlen, sizeof c->in - c->inlen, 0);
    if (g > 0) { c->inlen += (size_t)g; return 1; }
    if (g == 0) return -1;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

/* --- ServerInit and the two updates ---------------------------------------- */

static void send_server_init(struct client *c, const struct vnc_session_opts *o,
                             int w, int h)
{
    uint8_t hdr[24];
    uint32_t nl = (uint32_t)strlen(o->name);

    hdr[0] = (uint8_t)(w >> 8);  hdr[1] = (uint8_t)w;
    hdr[2] = (uint8_t)(h >> 8);  hdr[3] = (uint8_t)h;
    vnc_pf_write(hdr + 4, &vnc_pf_rgb565);
    hdr[20] = (uint8_t)(nl >> 24); hdr[21] = (uint8_t)(nl >> 16);
    hdr[22] = (uint8_t)(nl >> 8);  hdr[23] = (uint8_t)nl;
    if (out_push(c, hdr, sizeof hdr) < 0 || out_push(c, o->name, nl) < 0) {
        vlog("%s: could not queue ServerInit", c->peer);
        c->state = -1;
    }
}

/* The pixels of one frame, in whatever format the client settled on.
 *
 * WHEN THE FORMAT IS OURS THIS IS ONE memcpy, and that is the whole reason
 * ServerInit advertises fb0's own layout rather than the 32-bpp one clients like:
 * a client that takes us at our word costs a copy, not a conversion. */
static int push_pixels(struct client *c, const uint16_t *px, int w, int h)
{
    if (vnc_pf_is_rgb565_le(&c->pf))
        return out_push(c, px, (size_t)w * (size_t)h * 2);

    if (!c->conv_ok)
        return -1;
    {
        size_t rowbytes = (size_t)w * (size_t)c->conv.bytes;
        int y;
        if (rowbytes > c->rowcap) {
            uint8_t *nb = realloc(c->rowbuf, rowbytes);
            if (!nb) return -1;
            c->rowbuf = nb;
            c->rowcap = rowbytes;
        }
        for (y = 0; y < h; y++) {
            vnc_conv_rows(c->rowbuf, px + (size_t)y * w, (size_t)w, &c->conv);
            if (out_push(c, c->rowbuf, rowbytes) < 0)
                return -1;
        }
    }
    return 0;
}

/* One FramebufferUpdate, carrying either a Raw rectangle or a Tight rectangle whose
 * payload is a JPEG the hardware encoder just produced.
 *
 * THE PIXEL FORMAT PLAYS NO PART IN THE JPEG ARM, and that is the quiet advantage of
 * it: whichever format the client asked for in SetPixelFormat, a JPEG is a JPEG, so a
 * client that wants 32 bpp BGRA costs nothing extra here -- the conversion table is
 * built, and never used. Raw and Tight are decided per client per frame, so a session
 * where the mode is flipped takes effect on the very next update. */
static int send_update(struct client *c, const uint16_t *px, int w, int h,
                       const uint8_t *jpg, size_t jlen)
{
    uint8_t hdr[16];
    uint8_t jh[4];
    int jhn = 0;
    int tight = jpg && jlen && client_jpeg_ok(c);
    size_t pxbytes;

    if (tight) {
        jhn = vnc_rfb_jpeg_header(jh, (uint32_t)jlen);
        pxbytes = (size_t)jhn + jlen;
    } else if (vnc_pf_is_rgb565_le(&c->pf)) {
        pxbytes = (size_t)w * (size_t)h * 2;
    } else {
        pxbytes = (size_t)w * (size_t)c->conv.bytes * (size_t)h;
    }

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0;                                    /* FramebufferUpdate */
    hdr[2] = 0; hdr[3] = 1;                        /* one rectangle     */
    hdr[8]  = (uint8_t)(w >> 8); hdr[9]  = (uint8_t)w;
    hdr[10] = (uint8_t)(h >> 8); hdr[11] = (uint8_t)h;
    /* hdr[12..15] = 0 (Raw) or 7 (Tight) */
    if (tight)
        hdr[15] = VNC_ENC_TIGHT;

    if (out_push(c, hdr, sizeof hdr) < 0)
        return -1;
    if (tight) {
        if (out_push(c, jh, (size_t)jhn) < 0 || out_push(c, jpg, jlen) < 0)
            return -1;
    } else if (push_pixels(c, px, w, h) < 0) {
        return -1;
    }

    c->frames++;
    c->bytes += sizeof hdr + pxbytes;
    return 0;
}

/* --- the compressed path, in bands --------------------------------------------
 *
 * WHAT THIS REPLACES AND WHY IT HAD TO GO. Until 2026-10-09 every update was one
 * rectangle covering the whole screen, sent Raw, at 2 MB a frame. RFB IS
 * CLIENT-DRIVEN: a client asks for an update and the server answers, and macOS's
 * Screen Sharing does not ask for the next one until it has decoded and painted the
 * last. So the 2 MB rectangle WAS the frame rate. The operator's own session logged
 * `360 frame(s) composed, 59 sent` -- the server was never the thing holding it
 * back, and no amount of server speed would have helped.
 *
 * A compressing encoding takes a frame from 2 MB to tens of kilobytes, and --
 * because the picture only moves in a few places -- the bands take it further.
 * Measured on the unit, one 300 ms interval of rbp playing: 112 KB and 101 ms for
 * the whole frame in the client's 32-bpp format, against 59-69 KB and 14-24 ms for
 * the six bands that actually moved.
 *
 * WHICH ENCODING, THOUGH, AND THIS IS THE PART THAT WAS WRONG FOR A DAY. This block
 * used to say "Apple's client offers Raw, Tight and NewFBSize and nothing else", and
 * built the Tight arm on it. That list is rfbclient.py's -- THIS REPO'S OWN TEST
 * CLIENT. Read off a live session on 2026-10-09, with the connecting process named
 * from the Mac end (`lsof` -> `Screen Sharing.app`, pid 87331), Apple's real list is
 * thirteen entries led by **zlib (6)** and **ZRLE (16)**, with NO Tight and no Raw.
 * So the Tight arm never once ran for the operator: every real session took the Raw
 * floor, 4 096 016 bytes an update, and the socket's send queue sat at 1.5 MB. */

/* Give this client a zlib stream and a copy of its frame, once. 1 if a compressing
 * encoding is usable, 0 if it is not -- in which case the caller sends Raw and the
 * session still works, just as slowly as it used to. Encoding-agnostic: the same
 * stream serves Tight (7) and zlib (6). */
static int comp_ready(struct client *c, int w, int h, int level)
{
    if (c->comp_ok)
        return 1;
    if (c->comp_failed)
        return 0;

    if (!c->z) {
        size_t rowbytes = vnc_pf_is_rgb565_le(&c->pf) ? (size_t)w * 2
                                                      : (size_t)w * (size_t)c->conv.bytes;
        /* Sized for the worst rectangle there can be: the whole frame, at this
         * client's pixel depth, incompressible. Almost every chunk is a few bands
         * of a mostly-black screen and lands three orders of magnitude under it. */
        c->z = vnc_deflate_new(level, rowbytes * (size_t)h);
        if (!c->z) {
            vlog("%s: no deflate stream (%s), so this client gets Raw frames -- the "
                 "session works, it is just 2 MB an update", c->peer,
                 vnc_deflate_error() ? vnc_deflate_error() : "unknown");
            c->comp_failed = 1;
            return 0;
        }
    }
    if (!c->prev) {
        c->prev = malloc((size_t)w * (size_t)h * 2);
        if (!c->prev) {
            vlog("%s: out of memory for the frame comparison, so this client gets "
                 "Raw frames", c->peer);
            c->comp_failed = 1;
            return 0;
        }
    }
    c->comp_ok = 1;
    return 1;
}

/* One row into the client's zlib stream, converted if its format is not ours. */
static int comp_feed_row(struct client *c, const uint16_t *row, int w)
{
    if (vnc_pf_is_rgb565_le(&c->pf))
        return vnc_deflate_feed(c->z, (const uint8_t *)row, (size_t)w * 2);

    if (!c->conv_ok)
        return -1;
    {
        size_t rowbytes = (size_t)w * (size_t)c->conv.bytes;
        if (rowbytes > c->rowcap) {
            uint8_t *nb = realloc(c->rowbuf, rowbytes);
            if (!nb)
                return -1;
            c->rowbuf = nb;
            c->rowcap = rowbytes;
        }
        vnc_conv_rows(c->rowbuf, row, (size_t)w, &c->conv);
        return vnc_deflate_feed(c->z, c->rowbuf, rowbytes);
    }
}

/* Which rows this update has to carry: the whole frame for a first or non-incremental
 * request -- there is nothing to compare against -- and otherwise only the bands that
 * actually moved. Returns the band count, or 0 when the client is already looking at
 * exactly this frame. */
static int comp_bands(struct client *c, const uint16_t *px, int w, int h,
                      int incremental)
{
    int n;

    if (!incremental || !c->prev_ok) {
        c->bands[0].y0 = 0;
        c->bands[0].y1 = h;
        return 1;
    }
    if (vnc_diff_same(c->prev, px, w, h))
        return 0;
    n = vnc_diff_bands(c->prev, px, w, h, VNC_BAND_GAP, c->bands, VNC_BAND_MAX, NULL);
    if (n == 0)
        return 0;
    if (n > VNC_BAND_MAX) {
        /* Too fragmented to be worth N rectangle headers. One rectangle of the whole
         * screen is what this used to always do. */
        c->bands[0].y0 = 0;
        c->bands[0].y1 = h;
        return 1;
    }
    return n;
}

/* Deflate one band into the client's stream. On success *z and *zlen describe the
 * compressed chunk, which stays valid until the next call -- it lives in the client's
 * own deflate buffer. Returns -1 on a failure the caller must drop the client for. */
static int comp_deflate_band(struct client *c, const uint16_t *px, int w,
                             const struct vnc_band *b, const uint8_t **z,
                             size_t *zlen)
{
    int r, ok = 1;

    vnc_deflate_begin(c->z);
    for (r = b->y0; r < b->y1; r++)
        if (comp_feed_row(c, px + (size_t)r * w, w) < 0) {
            ok = 0;
            break;
        }
    *z = vnc_deflate_end(c->z, zlen);
    if (!ok || !*z) {
        vlog("%s: deflate failed mid-rectangle (%s); dropping it rather than sending "
             "a client a stream it cannot decode", c->peer,
             vnc_deflate_error() ? vnc_deflate_error() : "unknown");
        return -1;
    }
    return 0;
}

/* The queue is the backpressure: a client that cannot keep up gets fewer frames
 * rather than a longer and longer queue, and one that has stopped reading altogether
 * is dropped. */
static int comp_overrun(struct client *c)
{
    vlog("%s: cannot keep up (%zu bytes queued unsent); dropping it rather than growing",
         c->peer, c->outlen - c->outoff);
    return -1;
}

/* One FramebufferUpdate carrying the bands, in whichever of the two compressing
 * encodings this client offered: `enc` is VNC_ENC_TIGHT (7) or VNC_ENC_ZLIB (6), and
 * those are the only two values it may take.
 *
 * Returns 1 if an update went out, 0 if the screen is already what this client is
 * looking at, -1 on a failure that must drop the client.
 *
 * ZERO BANDS IS NOT AN ERROR AND IS NOT A SILENT DROP. An incremental request with
 * nothing to report leaves the request outstanding -- c->want_update stays set and
 * the next tick looks again -- which is what a server is supposed to do and what
 * lets a paused deck cost this process about a millisecond a second. */
static int send_comp_update(struct client *c, const uint16_t *px, int w, int h,
                            int incremental, int level, int enc)
{
    uint8_t up[4], rh[12], ch[4];
    int nbands, i;

    if (!comp_ready(c, w, h, level))
        return -1;                          /* caller falls back to Raw */

    nbands = comp_bands(c, px, w, h, incremental);
    if (nbands <= 0)
        return 0;                           /* nothing moved; the request stands */

    up[0] = 0;                             /* FramebufferUpdate      */
    up[1] = 0;
    up[2] = (uint8_t)(nbands >> 8);
    up[3] = (uint8_t)nbands;
    if (out_push(c, up, sizeof up) < 0)
        return comp_overrun(c);

    for (i = 0; i < nbands; i++) {
        const struct vnc_band *b = &c->bands[i];
        const uint8_t *z;
        size_t zlen;
        int chn;

        if (comp_deflate_band(c, px, w, b, &z, &zlen) < 0)
            return -1;

        /* THE WHOLE DIFFERENCE BETWEEN THE TWO ENCODINGS IS THIS ONE CHOICE. Tight
         * puts a control byte and a compact length in front of the chunk; zlib puts a
         * plain 4-byte big-endian length there and nothing else. The deflate stream,
         * the bands, and the pixels underneath are identical. */
        chn = (enc == VNC_ENC_ZLIB)
                  ? vnc_rfb_zlib_rect_header(ch, (uint32_t)zlen)
                  : vnc_rfb_tight_zlib_header(ch, (uint32_t)zlen);

        memset(rh, 0, sizeof rh);
        rh[0] = 0;                                       rh[1]  = 0;
        rh[2] = (uint8_t)(b->y0 >> 8);                   rh[3]  = (uint8_t)b->y0;
        rh[4] = (uint8_t)(w >> 8);                       rh[5]  = (uint8_t)w;
        rh[6] = (uint8_t)((b->y1 - b->y0) >> 8);         rh[7]  = (uint8_t)(b->y1 - b->y0);
        rh[11] = (uint8_t)enc;

        if (out_push(c, rh, sizeof rh) < 0 ||
            out_push(c, ch, (size_t)chn) < 0 ||
            out_push(c, z, zlen) < 0)
            return comp_overrun(c);

        c->comp_out += zlen;
        c->bytes += sizeof rh + (size_t)chn + zlen;
    }

    memcpy(c->prev, px, (size_t)w * (size_t)h * 2);
    c->prev_ok = 1;
    c->frames++;
    return 1;
}

/* --- message lengths ------------------------------------------------------- *
 * Client-to-server RFB has no framing: the length of a message is a function of its
 * type, and for SetEncodings of a count inside it. There is deliberately no "read
 * until you recognise something" -- that is how a desynchronised stream turns into a
 * client that renders garbage instead of one that gets dropped.
 *
 * Returns the total length of the message starting at p, 0 if more bytes are needed
 * before that can be known, or (size_t)-1 for a type this server does not handle. */
static size_t msg_need(const uint8_t *p, size_t have)
{
    switch (p[0]) {
    case 0:  return 20;                       /* SetPixelFormat: 4 + 16           */
    case 2:                                   /* SetEncodings                    */
        if (have < 4) return 0;
        return 4 + 4u * (size_t)(((unsigned)p[2] << 8) | p[3]);
    case 3:  return 10;                       /* FramebufferUpdateRequest        */
    case 4:  return 8;                        /* KeyEvent                        */
    case 5:  return 6;                        /* PointerEvent                    */
    case 6:                                   /* ClientCutText                   */
        if (have < 8) return 0;
        return 8 + (size_t)vnc_net_be32(p + 4);
    default: return (size_t)-1;
    }
}

/* --- the ready-state parser ------------------------------------------------ */

static int handle_message(struct client *c, const uint8_t *p, size_t len)
{
    (void)len;

    switch (p[0]) {
    case 0: {                                 /* SetPixelFormat */
        char desc[160];
        vnc_pf_read(p + 4, &c->pf);
        c->pf_set = 1;
        vnc_pf_describe(&c->pf, desc, sizeof desc);
        c->conv_ok = vnc_conv_init(&c->conv, &c->pf) == 0;
        vlog("%s: SetPixelFormat: %s", c->peer, desc);
        if (!c->conv_ok) {
            vlog("%s: *** that is not a format this server can produce (not true "
                 "colour, a bits-per-pixel other than 8/16/24/32, or channels that "
                 "do not fit the pixels carrying them). Dropping rather than drawing "
                 "something wrong.", c->peer);
            return -1;
        }
        if (vnc_pf_is_rgb565_le(&c->pf))
            vlog("%s: ... that is fb0's own layout, so Raw is a straight copy", c->peer);
        break;
    }
    case 2: {                                 /* SetEncodings */
        unsigned count = ((unsigned)p[2] << 8) | p[3];
        unsigned i;
        char list[400];
        char nums[256];
        size_t used = 0, nused = 0;
        list[0] = '\0';
        nums[0] = '\0';
        /* SetEncodings REPLACES the list rather than adding to it, so a client that
         * sends it a second time must not carry the first list's answers into the
         * second. */
        c->tight_seen = c->zlib_seen = c->quality_seen = 0;
        for (i = 0; i < count; i++) {
            int32_t e = (int32_t)vnc_net_be32(p + 4 + 4 * i);
            const char *nm = e >= 0 ? vnc_rfb_encoding_name(e) : vnc_rfb_pseudo_name(e);
            if (e == VNC_ENC_TIGHT) c->tight_seen = 1;
            if (e == VNC_ENC_ZLIB)  c->zlib_seen = 1;
            if (vnc_rfb_pseudo_is_quality(e)) c->quality_seen = 1;
            if (used < sizeof list - 24 && nm) {
                int n = snprintf(list + used, sizeof list - used, "%s%s",
                                 used ? ", " : "", nm);
                if (n > 0) used += (size_t)n;
            }
            /* THE NUMBERS GO IN THE LOG TOO, and they are not decoration. The names
             * alone cost a day: docs/19-vnc.md recorded "Apple's Screen Sharing sends
             * Raw, Tight, NewFBSize -- three entries", which is what rfbclient.py
             * sends, and a real Screen Sharing session was read through that
             * assumption for a whole day of work. Two encodings Apple does offer are
             * not in that list at all, and one of them -- zlib, 6 -- is the encoding it
             * names FIRST. An unnamed number in the log is a question; a name that is
             * quietly wrong is an answer, and it is the wrong one. */
            if (nused < sizeof nums - 12) {
                int n = snprintf(nums + nused, sizeof nums - nused, "%s%d",
                                 nused ? " " : "", (int)e);
                if (n > 0) nused += (size_t)n;
            }
        }
        vlog("%s: SetEncodings: %u entr%s -- Tight:%s zlib:%s JPEG-quality:%s -- %s "
             "[%s]", c->peer, count, count == 1 ? "y" : "ies",
             c->tight_seen ? "yes" : "no", c->zlib_seen ? "yes" : "no",
             c->quality_seen ? "yes" : "no", list, nums);
        /* The line whose absence cost a round trip with the operator. A JPEG quality
         * level is the client's only consent to a lossy payload, and the words above
         * are the whole difference between a session that stays up and one that dies
         * on its first frame. Naming the compressing encoding it WILL get is not
         * decoration either: for a day this line promised Tight to clients that had
         * never offered it. */
        if (!c->quality_seen)
            vlog("%s: ... no JPEG quality level was offered, and the spec allows JPEG "
                 "only when the client asks for one, so JPEG is out for this session "
                 "whatever the mode file says. It will get %s.", c->peer,
                 c->tight_seen ? "Tight's zlib (lossless, compressed)"
                 : c->zlib_seen ? "the zlib encoding (lossless, compressed)"
                                : "Raw frames -- it offered no compressing encoding "
                                  "this server can produce");
        break;
    }
    case 3: {                                 /* FramebufferUpdateRequest */
        c->incremental = p[1];
        c->want_update = 1;
        break;
    }
    case 4:                                   /* KeyEvent */
        /* THE KEYBOARD IS STILL NOT WIRED UP, and it is not an oversight: a keycode
         * has to name a deck, a page and a slot in a way a pointer position does not,
         * and what the operator asked for was the mouse. vnc_input.h takes a position
         * and nothing else. */
        vlog("%s: KeyEvent ignored (only the pointer is wired up)", c->peer);
        break;
    case 5: {                                 /* PointerEvent */
        int buttons = p[1];
        int x = (int)(((unsigned)p[2] << 8) | p[3]);
        int y = (int)(((unsigned)p[4] << 8) | p[5]);
        int live = g_input && g_input->fd >= 0 && g_input_sw && g_input_sw->on;

        /* ONE LINE PER EDGE. A click is two lines and a drag is two lines; logging every
         * report would be a hundred lines a second into a file on tmpfs, which is a
         * mistake this project has already made once (RB_VERBOSE in rb.conf). */
        if ((buttons & 1) && !(c->ptr_mask & 1)) {
            int rx = 0, ry = 0;
            if (g_input)
                vnc_input_map(x, y, g_input->scr_w, g_input->scr_h,
                              g_input->raw_w, g_input->raw_h, &rx, &ry);
            vlog("%s: the mouse went down at (%d,%d), the panel's raw (%d,%d)%s",
                 c->peer, x, y, rx, ry,
                 live ? " -- a real press on the glass"
                      : " -- and input is off, so nothing was pressed");
            if (live)
                g_input_owner = (int)(c - g_clients);
            c->ptr_presses++;
        } else if (!(buttons & 1) && (c->ptr_mask & 1)) {
            vlog("%s: the mouse came up at (%d,%d)", c->peer, x, y);
        } else if ((buttons & ~1) && !(c->ptr_mask & ~1)) {
            /* The panel has one contact and no buttons; RFB's middle and right buttons
             * have nothing to press. Said once, because a client that sends them sends
             * them often. */
            vlog("%s: PointerEvent with button mask %d; this panel has one contact and "
                 "no buttons, so only the left one does anything", c->peer, buttons);
        }
        c->ptr_mask = buttons;
        if (live)
            vnc_input_pointer(g_input, buttons, x, y, now_ms());
        break;
    }
    case 6:                                   /* ClientCutText */
        c->discard = (uint32_t)vnc_net_be32(p + 4);
        break;
    }
    return 0;
}

/* --- the handshake --------------------------------------------------------- */

static void send_challenge(struct client *c, const struct vnc_session_opts *o);

static void send_security(struct client *c, const struct vnc_session_opts *o)
{
    if (c->minor >= 7) {
        uint8_t list[3];
        /* None is offered even though macOS's client will not take it: the log is
         * worth more than the tidiness, because "which type did it choose" is the
         * question this whole route turns on and it is free to answer. */
        list[0] = 2;
        list[1] = VNC_SEC_NONE;
        list[2] = VNC_SEC_VNC;
        if (out_push(c, list, 3) < 0) { c->state = -1; return; }
        vlog("%s: offering security types None(1) and VNC auth(2)  [3.7+ list]",
             c->peer);
        c->state = CS_SECCHOICE;
        c->since = now_ms();
    } else {
        /* AT 3.3 THERE IS NO LIST AND NO CHOICE -- the server names one type in a
         * uint32 and that is that. The type is VNC auth and never None, because a
         * client that announces 3.3 has told us nothing about what it will accept,
         * and None is the one answer known to close Apple's socket.
         *
         * WHAT 3.3 DOES HAVE IS A SecurityResult, and this file used to say it did
         * not. See vnc_rfb_sends_security_result: the no-word rule belongs to the
         * *None* path, and 3.3 never takes it here. Omitting the word after a
         * password is accepted leaves the client waiting for it forever -- which is
         * exactly what Apple's Screen Sharing did, for the full fifteen-second
         * client-init timeout, with nothing in the log but silence.
         *
         * NAMING THE TYPE IS NOT THE SAME AS STARTING THE EXCHANGE. The challenge
         * follows immediately, and a version of this that set the state to CS_AUTH
         * without sending it left the client waiting on a challenge the server never
         * wrote -- a hang with nothing in the log, which is the least debuggable shape
         * a protocol bug can take. */
        uint8_t t[4] = { 0, 0, 0, VNC_SEC_VNC };
        if (out_push(c, t, 4) < 0) { c->state = -1; return; }
        vlog("%s: security type 2 (VNC auth), 3.3-style: no list and no choice",
             c->peer);
        send_challenge(c, o);
    }
    c->since = now_ms();
}

/* Send the challenge and work out, now, the response that would be correct.
 *
 * Computed here rather than when the reply arrives for a reason that is easy to miss:
 * comparing the client's bytes against a computed value takes a variable amount of
 * time if the comparison is a strcmp-shaped one, and DES is cheap enough to brute
 * force offline. The compare in CS_AUTH is a single memcmp over sixteen bytes and the
 * response is already sitting in memory, so there is no timing signal to read. */
static void send_challenge(struct client *c, const struct vnc_session_opts *o)
{
    FILE *f;
    int k;

    /* A random challenge from /dev/urandom, because a fixed one would let anyone on
     * the network replay a captured response. */
    f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(c->challenge, 1, sizeof c->challenge, f) != sizeof c->challenge) {
            for (k = 0; k < 16; k++) c->challenge[k] = (uint8_t)(now_ms() >> (k % 8));
            vlog("%s: /dev/urandom came up short; the challenge is weaker than it "
                 "should be", c->peer);
        }
        fclose(f);
    } else {
        for (k = 0; k < 16; k++) c->challenge[k] = (uint8_t)(now_ms() >> (k % 8));
        vlog("%s: cannot open /dev/urandom; the challenge is weaker than it should be",
             c->peer);
    }
    vnc_auth_response(o->password, c->challenge, c->expected);

    if (out_push(c, c->challenge, sizeof c->challenge) < 0) { c->state = -1; return; }
    vlog("%s: sent the 16-byte challenge; waiting up to %ds for the response",
         c->peer, VNC_T_AUTH / 1000);
    c->state = CS_AUTH;
    c->since = now_ms();
}

static void send_security_result(struct client *c, uint32_t code)
{
    uint8_t r[4];
    r[0] = (uint8_t)(code >> 24); r[1] = (uint8_t)(code >> 16);
    r[2] = (uint8_t)(code >> 8);  r[3] = (uint8_t)code;
    if (out_push(c, r, 4) < 0) { c->state = -1; return; }
}

/* The whole of one client's input handling, in order, at whatever pace it arrives.
 * Returns -1 when the client must be dropped. */
static int parse(struct client *c, const struct vnc_session_opts *o, int w, int h)
{
    for (;;) {
        if (c->state < 0)
            return -1;

        if (c->discard) {
            size_t take = c->discard < c->inlen ? (size_t)c->discard : c->inlen;
            if (take == 0) return 0;
            consume(c, take);
            c->discard -= (uint32_t)take;
            continue;
        }

        switch (c->state) {
        case CS_HELLO: {
            char raw[VNC_RFB_VERSION_LEN + 1];
            int maj = 3, cmin = 3;
            if (c->inlen < VNC_RFB_VERSION_LEN) return 0;
            memcpy(raw, c->in, VNC_RFB_VERSION_LEN);
            if (!vnc_rfb_parse_version((const char *)c->in, &maj, &cmin)) {
                /* Not fatal: a client that names something unreadable is treated as
                 * 3.3, which is the oldest dialect and the one that will not be
                 * surprised by a single named security type. */
                cmin = 3;
                vlog("%s: unparseable version string; treating it as 3.3", c->peer);
            }
            consume(c, VNC_RFB_VERSION_LEN);
            /* RFB TAKES THE LOWER OF THE TWO VERSIONS, and this is where that is
             * applied. We announce 3.8; macOS's Screen Sharing answers 3.003, so the
             * session that follows is a 3.3 session -- single named security type, no
             * list, no choice -- even though both us and its own server speak 3.8.
             * The SecurityResult is NOT among the things 3.3 drops; see
             * vnc_rfb_sends_security_result. */
            c->minor = cmin < o->announce_minor ? cmin : o->announce_minor;
            vlog("%s: client speaks RFB %d.%03d; we announced 3.%03d; the session runs "
                 "at 3.%d", c->peer, maj, cmin, o->announce_minor, c->minor);
            if (c->minor > 8) c->minor = 8;
            send_security(c, o);
            continue;
        }

        case CS_SECCHOICE: {
            uint8_t chosen;
            if (c->inlen < 1) return 0;
            chosen = c->in[0];
            consume(c, 1);
            vlog("%s: chose security type %u (%s)", c->peer, chosen,
                 chosen == VNC_SEC_NONE ? "None" :
                 chosen == VNC_SEC_VNC  ? "VNC auth" : "one we did not offer");
            if (chosen == VNC_SEC_NONE) {
                if (vnc_rfb_sends_security_result(c->minor, VNC_SEC_NONE))
                    send_security_result(c, 0);
                c->state = CS_CINIT;
                c->since = now_ms();
            } else if (chosen == VNC_SEC_VNC) {
                send_challenge(c, o);
            } else {
                vlog("%s: *** that type is not implemented here (Apple's own schemes "
                     "are 30/33/35, Diffie-Hellman based). Dropping.", c->peer);
                return -1;
            }
            continue;
        }

        case CS_AUTH: {
            if (c->inlen < 16) return 0;
            if (memcmp(c->in, c->expected, 16) != 0) {
                char got[3 * 16 + 1];
                int k;
                for (k = 0; k < 16; k++)
                    snprintf(got + 3 * k, 4, " %02X", c->in[k]);
                vlog("%s: WRONG PASSWORD (it answered%s)", c->peer, got);
                /* The word goes out in every version here -- for VNC auth the
                 * SecurityResult is 3.3's too. What 3.3 and 3.7 lack is the reason
                 * *string* that 3.8 puts after a failed word, and that is the part
                 * not worth sending: the spec says the client may not read it, so a
                 * server that writes one is racing its own close(). */
                if (vnc_rfb_sends_security_result(c->minor, VNC_SEC_VNC))
                    send_security_result(c, 1);
                return -1;
            }
            consume(c, 16);
            vlog("%s: password accepted", c->peer);
            if (vnc_rfb_sends_security_result(c->minor, VNC_SEC_VNC))
                send_security_result(c, 0);
            c->state = CS_CINIT;
            c->since = now_ms();
            continue;
        }

        case CS_CINIT: {
            int shared;
            if (c->inlen < 1) return 0;
            shared = c->in[0];
            consume(c, 1);
            vlog("%s: ClientInit: shared-flag=%d", c->peer, shared);
            send_server_init(c, o, w, h);
            if (c->state < 0) return -1;
            c->state = CS_READY;
            c->since = now_ms();
            vlog("%s: ServerInit sent (%dx%d); client is live", c->peer, w, h);
            {
                char desc[160];
                vnc_pf_describe(&c->pf, desc, sizeof desc);
                vlog("%s: until it says otherwise it gets %s", c->peer, desc);
            }
            continue;
        }

        case CS_READY: {
            size_t need;
            if (c->inlen < 1) return 0;
            need = msg_need(c->in, c->inlen);
            if (need == 0) return 0;
            if (need == (size_t)-1) {
                vlog("%s: *** unknown message type %u. Its length is not knowable from "
                     "the type alone, so the stream cannot be resynchronised. Dropping.",
                     c->peer, c->in[0]);
                return -1;
            }
            if (need > sizeof c->in) {
                vlog("%s: *** a %zu-byte message (type %u) exceeds the %zu-byte limit "
                     "this server parses. Dropping.", c->peer, need, c->in[0],
                     sizeof c->in);
                return -1;
            }
            if (c->inlen < need) return 0;
            if (handle_message(c, c->in, need) < 0)
                return -1;
            consume(c, need);
            continue;
        }

        default:
            return -1;
        }
    }
}

/* --- the frame ------------------------------------------------------------- */

struct frame {
    uint16_t *px;              /* the composed screen, RGB565                */
    uint16_t *prev;            /* the last one sent out, for change detection */
    int w, h;
    /* A serial rather than a boolean. "Has anything changed" cannot be a flag that
     * the frame loop clears, because a client whose previous update is still going
     * out does not get this one -- and clearing the flag then would leave it waiting
     * for a frame that has already been superseded and will never be offered again. */
    unsigned long long serial;
    unsigned long long ticks;
    unsigned long long sends;
    unsigned long long last_report;
};

static void frame_tick(struct frame *fr, struct vnc_capture *cap)
{
    fr->ticks++;
    vnc_capture_frame(cap, fr->px);
    /* THE COMPARISON IS THE WHOLE BANDWIDTH STRATEGY. A Raw rectangle carries the
     * entire screen whether one pixel changed or all two million, so sending on a
     * timer rather than on a change would put 4 MB on the operator's WiFi every tick
     * with the player sitting idle. Two megabytes of memcmp costs about a third of a
     * millisecond. */
    if (memcmp(fr->px, fr->prev, (size_t)fr->w * fr->h * 2) != 0) {
        memcpy(fr->prev, fr->px, (size_t)fr->w * fr->h * 2);
        fr->serial++;
    }
}

/* --- main ------------------------------------------------------------------ */

static void client_close(struct client *c)
{
    /* THE FINGER COMES UP BEFORE THE CLIENT GOES. A client that closes its window
     * mid-press never sends the button-up, and a press left down on the operator's
     * panel is a pointer stuck where it was for as long as the shim lives -- the one
     * failure of this feature that would need somebody to walk over and touch the
     * glass to clear. The slot has to be read before the memset below, and only the
     * client that actually pressed owns the finger. */
    if (c->fd >= 0 && g_input && (int)(c - g_clients) == g_input_owner) {
        g_input_owner = -1;
        vnc_input_release(g_input);
    }
    if (c->fd >= 0) close(c->fd);
    free(c->out);
    free(c->rowbuf);
    vnc_deflate_free(c->z);
    free(c->prev);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static const char *state_name(int s)
{
    switch (s) {
    case CS_HELLO:     return "version";
    case CS_SECCHOICE: return "security-choice";
    case CS_AUTH:      return "auth";
    case CS_CINIT:     return "client-init";
    case CS_READY:     return "ready";
    default:           return "?";
    }
}

static int state_timeout(int s)
{
    switch (s) {
    case CS_HELLO:     return VNC_T_HELLO;
    case CS_SECCHOICE: return VNC_T_SECCHOICE;
    case CS_AUTH:      return VNC_T_AUTH;
    case CS_CINIT:     return VNC_T_CINIT;
    default:           return 0;
    }
}

/* --- coming up, and going down --------------------------------------------- */

/*
 * WHAT "SERVING" COSTS, IN ONE PLACE. Everything this program opens to watch the
 * display is opened here and closed in stop_serving: /dev/fb0 and /dev/dri/card1
 * through vnc_capture_open, the two frame buffers, the hardware encoder, and the RFB
 * listener. Sharing off means none of them exist, which is the whole point of the
 * switch -- see vnc_live.h, which also says why the process stays up regardless.
 *
 * It is a function rather than a block inside vnc_session_run because it has to be
 * able to run a SECOND time, minutes later, when somebody presses the button on the
 * page -- and because the failure path has to leave the process exactly as it found
 * it. The listener can fail on a port a stale vncserve still holds; the honest answer
 * to that is the page saying so, not a half-open server.
 */
static int start_serving(struct vnc_capture **cap, struct vnc_jpeg **jpeg,
                         struct frame *fr, int *ls,
                         const struct vnc_session_opts *o)
{
    char err[256];
    int i;

    *cap = vnc_capture_open(err, sizeof err);
    if (!*cap) {
        vlog("capture: %s", err);
        return -1;
    }
    fr->w = vnc_capture_width(*cap);
    fr->h = vnc_capture_height(*cap);
    fr->px   = malloc((size_t)fr->w * fr->h * 2);
    fr->prev = malloc((size_t)fr->w * fr->h * 2);
    if (!fr->px || !fr->prev) {
        vlog("out of memory for two %dx%d frames", fr->w, fr->h);
        goto fail_frames;
    }

    /* One capture before the first client arrives, so the first frame anybody asks
     * for is a picture rather than a buffer of zeroes. The serial starts at 1 so the
     * first client's first update has something to differ from -- and it is set here
     * rather than left where it was, because fr.prev is a fresh allocation and a
     * comparison against unfilled memory is meaningless. */
    vnc_capture_frame(*cap, fr->px);
    memcpy(fr->prev, fr->px, (size_t)fr->w * fr->h * 2);
    fr->serial = 1;

    /* THE ENCODER IS OPENED EVEN WHEN THE MODE IS RAW, because the page's preview must
     * be able to show what the other mode looks like without the mode being switched
     * first. It costs a few hundred kilobytes of mapping and, until a preview or a
     * hwjpeg client wants a frame, not one millisecond of CPU -- no buffer is ever
     * queued to it until vnc_jpeg_frame is called. */
    *jpeg = vnc_jpeg_create(fr->w, fr->h, o->jpeg_dev, o->jpeg_quality);
    if (!*jpeg) {
        vlog("out of memory for the JPEG encoder handle");
        goto fail_frames;
    }

    *ls = vnc_net_listen(o->bindaddr, o->port);
    if (*ls < 0) {
        vlog("cannot listen on %s:%d: %s -- the screen is NOT being served",
             o->bindaddr ? o->bindaddr : "0.0.0.0", o->port, strerror(errno));
        goto fail_jpeg;
    }

    for (i = 0; i < VNC_MAX_CLIENTS; i++) g_clients[i].fd = -1;
    vlog("serving: listening on %s:%d -- %d bpp, %d fps ceiling, password %s",
         o->bindaddr ? o->bindaddr : "0.0.0.0", o->port, 16, o->fps,
         o->password[0] ? "set" : "EMPTY (any client that answers nothing is in)");
    vlog("serving: first frame captured: %s", vnc_capture_status(*cap));
    return 0;

fail_jpeg:
    vnc_jpeg_destroy(*jpeg);
    *jpeg = NULL;
fail_frames:
    free(fr->px); free(fr->prev);
    fr->px = fr->prev = NULL;
    vnc_capture_close(*cap);
    *cap = NULL;
    return -1;
}

/* Everything start_serving opened, closed, and the clients first -- a client
 * mid-handshake holds a socket this is about to have no listener for. The geometry
 * goes back to the fallback so the page still lays its placeholder out at the right
 * shape while sharing is off. */
static void stop_serving(struct vnc_capture **cap, struct vnc_jpeg **jpeg,
                         struct frame *fr, int *ls)
{
    int i;

    for (i = 0; i < VNC_MAX_CLIENTS; i++) client_close(&g_clients[i]);
    if (*ls >= 0) { close(*ls); *ls = -1; }
    if (*jpeg) { vnc_jpeg_destroy(*jpeg); *jpeg = NULL; }
    free(fr->px); free(fr->prev);
    fr->px = fr->prev = NULL;
    if (*cap) { vnc_capture_close(*cap); *cap = NULL; }
    fr->w = VNC_FALLBACK_W;
    fr->h = VNC_FALLBACK_H;
    fr->serial = 0;
}

int vnc_session_run(const struct vnc_session_opts *o)
{
    struct vnc_capture *cap = NULL;
    struct frame fr;
    int ls = -1, i;
    unsigned long long next_tick;
    struct vnc_mode mode;
    struct vnc_live live;
    struct vnc_jpeg *jpeg = NULL;
    struct vnc_http *http = NULL;
    struct vnc_http_state hst;
    struct vnc_input_switch insw;
    struct vnc_input input;

    memset(&fr, 0, sizeof fr);
    fr.w = VNC_FALLBACK_W;
    fr.h = VNC_FALLBACK_H;

    vnc_live_init(&live, o->live_path ? o->live_path : VNC_LIVE_PATH, o->default_live);
    vnc_live_get(&live);
    if (live.on && start_serving(&cap, &jpeg, &fr, &ls, o) < 0) {
        /* Asked for at startup and could not: put the switch back OFF rather than run
         * a server that says it is serving and is not. The page is still up, so the
         * button still works once whatever held the display or the port has gone. */
        char note[128];
        vlog("*** sharing was asked for at startup and could not start; leaving it off");
        vnc_live_set(&live, VNC_LIVE_OFF);
        if (vnc_live_note(&live, note, sizeof note))
            vlog("sharing: %s", note);
    }
    if (!live.on)
        vlog("sharing is OFF: this process serves the control page only -- no capture, "
             "no DRM, no RFB listener. Press the button on the page to start.");
    else
        vlog("sharing is ON at startup (switch file %s)", live.path);

    vnc_mode_init(&mode, o->mode_path ? o->mode_path : VNC_MODE_PATH, o->default_mode);
    vnc_input_switch_init(&insw, o->input_path ? o->input_path : VNC_INPUT_PATH,
                          o->default_input);
    vnc_input_init(&input, fr.w, fr.h, 0, 0);
    g_input = &input;
    g_input_sw = &insw;
    g_input_owner = -1;

    memset(&hst, 0, sizeof hst);
    hst.mode = &mode;
    hst.mode_path = mode.path;
    hst.fps = o->fps;
    hst.w = fr.w;
    hst.h = fr.h;
    hst.in = &insw;
    hst.live = &live;
    hst.vnc_port = o->port;

    if (o->http_port > 0) {
        char herr[256];
        http = vnc_http_open(o->bindaddr, o->http_port, &hst, herr, sizeof herr);
        if (!http)
            vlog("http: %s -- the control page is NOT available", herr);
        else
            vlog("control page on http://<this host>:%d/ -- it opens full screen with the "
                 "words hidden; tapping the picture brings them back and hides them again",
                 o->http_port);
    }

    vlog("announcing RFB 3.%03d; the session's real dialect is the lower of that and "
         "whatever the client answers", o->announce_minor);
    vlog("starting in %s mode, switch file %s", vnc_mode_name(mode.mode), mode.path);

    vlog("vnc input starts %s, switch file %s", vnc_input_name(insw.on), insw.path);

    next_tick = now_ms() + 1000 / (o->fps > 0 ? o->fps : 1);
    fr.last_report = now_ms();

    for (;;) {
        struct pollfd pf[VNC_MAX_CLIENTS + 1];
        int nf = 0, idx[VNC_MAX_CLIENTS + 1];
        unsigned long long now = now_ms();
        int timeout;
        int r;

        /* This turn's JPEG, if the encoder was asked for one. It points into the
         * encoder's own mapping and is valid only until the next call, so every
         * consumer copies out of it before this turn ends. */
        const uint8_t *jpg = NULL;
        size_t jlen = 0;

        /* The switch, read first thing every turn. It is a five-byte file behind a
         * stat, so this is free when nothing has changed -- and reading it here rather
         * than after the frame means a button pressed on the page is in force for the
         * very next frame, not the one after that. */
        vnc_mode_get(&mode);
        {
            char note[128];
            if (vnc_mode_note(&mode, note, sizeof note))
                vlog("mode: %s", note);
        }

        /* THE SHARING SWITCH, read beside the mode file -- but this is the one whose
         * TURN is expensive, because acting on it opens or closes a framebuffer, a
         * DRM node, an encoder and a listener. So it is the TRANSITION that matters,
         * and the common turn is a stat() of a four-byte file.
         *
         * A SWITCH THAT CANNOT DO WHAT IT SAYS IS TURNED BACK OFF, the same rule the
         * input switch follows and for the same reason: leaving the page showing "on"
         * while nothing is being served is the least debuggable answer available. The
         * case is real here rather than theoretical -- the RFB port can be held by a
         * vncserve somebody left running, and opening the capture can fail outright. */
        {
            char note[128];
            vnc_live_get(&live);

            /* STATE, NOT TRANSITION -- and `int was = live.on` was the bug, not the
             * style. The control page's own /session handler writes the switch file
             * through the SAME tracker this loop reads, so the turn that mattered had
             * already moved live.on before the loop ever compared it, the two were
             * equal, and nothing was started. Measured on the unit 2026-10-09: button
             * pressed, the page said on, /run/rblive4/vnc.live read `on` -- and the
             * process held no framebuffer, no DRM node and no RFB listener at all.
             * A switch that reports on and does nothing is the worst of the answers.
             *
             * What is compared instead is what the FILE asks for against what this
             * process is DOING, and `cap` non-NULL is the doing: start_serving sets it
             * and stop_serving is the only thing that clears it. Whoever moved the
             * request -- the page, an editor, another process entirely -- the loop
             * converges on the next turn. Re-running this when nothing changed costs
             * one stat() of a four-byte file and two pointer comparisons. */
            if (live.on && !cap) {
                if (start_serving(&cap, &jpeg, &fr, &ls, o) < 0) {
                    vlog("*** the page asked to start sharing and it could not; "
                         "turning the switch back off");
                    stop_serving(&cap, &jpeg, &fr, &ls);
                    vnc_live_set(&live, VNC_LIVE_OFF);
                } else {
                    /* The first frame of a run is a whole frame, so the timer is
                     * pulled forward rather than waiting out the old tick. */
                    next_tick = now;
                }
            } else if (!live.on && cap) {
                stop_serving(&cap, &jpeg, &fr, &ls);
            }
            if (vnc_live_note(&live, note, sizeof note))
                vlog("sharing: %s", note);
        }

        /* THE INPUT SWITCH, read beside the mode file and for the same reason. What
         * matters here is the TRANSITION rather than the state: a switch turned off
         * while a button is held has to put the finger up, and the panel has to be
         * opened the first time somebody turns it on. Both are one-time costs, so the
         * common turn is a stat() of a four-byte file and nothing else.
         *
         * A SWITCH THAT CANNOT DO WHAT IT SAYS IS TURNED BACK OFF. If there is no panel
         * to press -- a unit with a different digitiser, a typo in RB_VNC_INPUT_DEV --
         * then leaving the page showing "on" while nothing happens is the least
         * debuggable answer available. The file is written to "off" and the page and
         * the log both say so on the next turn. */
        {
            int was = insw.on;
            int want_input;
            vnc_input_switch_get(&insw);
            if (insw.on != was) {
                char note[VNC_NOTE_MAX];
                if (vnc_input_switch_note(&insw, note, sizeof note))
                    vlog("input: %s", note);
            }

            /* BOTH SWITCHES HAVE TO SAY SO, and the second one is the reason this is
             * not simply `if (insw.on)`. With sharing off there is no picture, so
             * there is no click in it to inject -- but the node would still be OPEN,
             * held from boot by a process whose entire purpose at boot is to be
             * indistinguishable from not existing. Opening the operator's panel
             * before anyone can possibly press it is a side effect with no upside.
             *
             * So the panel opens when sharing is turned on and closes when it is
             * turned off, and the input switch is what decides whether a CLICK is
             * allowed once it is open. Turning sharing off puts the finger up first:
             * a finger left down when the node closes is the one state that would
             * follow the operator back to the glass. */
            want_input = insw.on && live.on;
            if (want_input && input.fd < 0) {
                char ierr[256];
                if (vnc_input_open(&input, o->input_dev, ierr, sizeof ierr) == 0) {
                    vlog("input is ON: a click in the picture is a REAL press on the "
                         "panel, at raw x %d max. On the default screen the seventh "
                         "menu column is raw x 1884 -- USB STOP, which raises the "
                         "chooser rather than stopping anything.",
                         input.raw_w - 1);
                } else {
                    vlog("input: %s", ierr);
                    vlog("*** vnc input was asked for and there is no panel to press; "
                         "turning the switch back off. Set RB_VNC_INPUT_DEV if the "
                         "panel is a node this program does not recognise.");
                    vnc_input_switch_set(&insw, VNC_INPUT_OFF);
                    g_input_owner = -1;
                }
            }
            if (!want_input && input.fd >= 0) {
                /* The file says off -- or sharing does: put the finger up and let the
                 * panel go. Closing costs nothing and turning it back on reopens, which
                 * is what keeps "off" meaning off rather than merely idle. */
                vnc_input_release(&input);
                g_input_owner = -1;
                vnc_input_close(&input);
                vlog("input: the panel node is closed (%s)", live.on
                     ? "the input switch says off"
                     : "sharing is off, so there is no picture to click");
            }
        }

        /* A release the client sent sooner than the press could be a tap on its own. */
        vnc_input_tick(&input, now);
        {
            char note[VNC_NOTE_MAX];
            if (vnc_input_note(&input, note, sizeof note))
                vlog("input: %s", note);
        }
        /* The page, swept without blocking. Its own poll has a zero timeout so this
         * inherits the cadence of the loop below rather than adding a second timer to
         * a process that exists to stay out of the player's way. */
        vnc_http_poll(http, 0);

        if (ls >= 0) {
            pf[nf].fd = ls;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            idx[nf] = -1;
            nf++;
        }

        for (i = 0; i < VNC_MAX_CLIENTS; i++) {
            struct client *c = &g_clients[i];
            if (c->fd < 0) continue;
            pf[nf].fd = c->fd;
            pf[nf].events = POLLIN | (out_pending(c) ? POLLOUT : 0);
            pf[nf].revents = 0;
            idx[nf] = i;
            nf++;
        }

        timeout = (int)(next_tick > now ? next_tick - now : 0);
        if (timeout < 0) timeout = 0;
        if (timeout > 1000) timeout = 1000;

        r = poll(pf, (nfds_t)nf, timeout);
        if (r < 0 && errno != EINTR) { vlog("poll: %s", strerror(errno)); break; }

        now = now_ms();

        /* The deadlines first: a client that has gone quiet in the middle of the
         * handshake is the ordinary way a connect fails, and it must not hold a slot
         * for the rest of the process's life. */
        for (i = 0; i < VNC_MAX_CLIENTS; i++) {
            struct client *c = &g_clients[i];
            int t;
            if (c->fd < 0) continue;
            t = state_timeout(c->state);
            if (t && now - c->since > (unsigned long long)t) {
                vlog("%s: no %s in %d ms -- giving up on this client", c->peer,
                     state_name(c->state), t);
                client_close(c);
            }
        }

        for (i = 0; i < nf; i++) {
            int ci = idx[i];
            if (!pf[i].revents) continue;
            if (ci < 0) {
                if (pf[i].revents & POLLIN) {
                    struct sockaddr_in peer;
                    socklen_t pl = sizeof peer;
                    int fd = accept(ls, (struct sockaddr *)&peer, &pl);
                    if (fd < 0) {
                        if (errno != EINTR && errno != EAGAIN)
                            vlog("accept: %s", strerror(errno));
                    } else {
                        int slot;
                        for (slot = 0; slot < VNC_MAX_CLIENTS && g_clients[slot].fd >= 0; slot++)
                            ;
                        if (slot == VNC_MAX_CLIENTS) {
                            vlog("refused a connection from %s: already serving %d",
                                 inet_ntoa(peer.sin_addr), VNC_MAX_CLIENTS);
                            close(fd);
                        } else {
                            struct client *c = &g_clients[slot];
                            uint8_t herr[VNC_RFB_VERSION_LEN];
                            client_close(c);
                            c->fd = fd;
                            snprintf(c->peer, sizeof c->peer, "%s:%u",
                                     inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                            c->pf = vnc_pf_rgb565;
                            vnc_conv_init(&c->conv, &c->pf);
                            c->conv_ok = 1;
                            c->state = CS_HELLO;
                            c->since = now;
                            memcpy(herr, VNC_RFB_VERSION, sizeof herr);
                            if (o->announce_minor == 3)
                                memcpy(herr, "RFB 003.003\n", sizeof herr);
                            if (out_push(c, herr, sizeof herr) < 0) {
                                client_close(c);
                            } else {
                                vlog("=== connection from %s; sent \"RFB 3.%03d\"",
                                     c->peer, o->announce_minor);
                            }
                        }
                    }
                }
                continue;
            }
            {
                struct client *c = &g_clients[ci];
                int dead = 0;
                if (pf[i].revents & POLLIN) {
                    for (;;) {
                        int g = slurp(c);
                        if (g <= 0) { if (g < 0) dead = 1; break; }
                        if (parse(c, o, fr.w, fr.h) < 0) { dead = 1; break; }
                        /* parse() eats what it can; if the buffer is still full it
                         * means the parser refused to consume -- which only happens
                         * on the drop paths above. */
                        if (c->inlen == 0) break;
                    }
                }
                if (!dead && (pf[i].revents & POLLOUT))
                    if (out_drain(c) < 0) dead = 1;
                if (dead) {
                    vlog("%s: connection closed", c->peer);
                    client_close(c);
                }
            }
        }

        now = now_ms();
        /* NOBODY WATCHING MEANS NOBODY CAPTURING. A frame costs about ten
         * milliseconds to composite, and at two frames a second that is two per cent
         * of a core spent forever on a machine whose whole purpose is to play audio
         * -- for a viewer that is not there. So the timer only advances when a client
         * has finished its handshake, and jumps forward when one does, so its first
         * frame is a fresh one rather than whatever was in the buffer. */
        {
            int live = 0, want_soon = 0;
            for (i = 0; i < VNC_MAX_CLIENTS; i++) {
                struct client *c = &g_clients[i];
                if (c->fd < 0 || c->state != CS_READY) continue;
                live++;
                if (!c->got_frame) want_soon = 1;
            }
            /* A PAGE COUNTED AS A VIEWER. Somebody watching the preview is somebody
             * waiting for a frame just as much as a VNC client is, so the encoder and
             * the capture have to run for them too -- and, just as importantly, stop
             * when they close the tab. */
            if (vnc_http_preview_wanted(http))
                live++;
            if (!live)
                next_tick = now + (unsigned long long)(1000 / (o->fps > 0 ? o->fps : 1));
            else if (want_soon)
                next_tick = now;
        }

        if (cap && now >= next_tick) {
            frame_tick(&fr, cap);
            next_tick = now + (unsigned long long)(1000 / (o->fps > 0 ? o->fps : 1));

            /* IS A JPEG WANTED THIS TICK? Two things can want one, and they want it for
             * different reasons. The preview wants it whenever the page is open, in
             * either mode, because seeing the encoder's output is how the operator
             * decides which mode to be in. A VNC client wants it only in hwjpeg mode and
             * only if it can actually be sent one -- a client that never said Tight has
             * promised nothing about being able to decode one, and sending it a Tight
             * rectangle anyway is a desync rather than a fallback.
             *
             * AND "SAID TIGHT" IS NOT THE TEST, which is worth the sentence because this
             * gate is where the mistake would be expensive rather than fatal: since the
             * spec lets JPEG go only to a client that asked for a quality level, a client
             * that offers Tight and nothing more is going to be sent Raw whatever this
             * produces. Encoding for it would burn the encoder and 768 KB of buffer
             * every frame, forever, on a machine whose job is playing audio -- the exact
             * cost this feature exists to avoid. macOS's Screen Sharing is that client. */
            {
                int want = vnc_http_preview_wanted(http) ? 1 : 0;
                if (!want && mode.mode == VNC_MODE_HWJPEG) {
                    for (i = 0; i < VNC_MAX_CLIENTS; i++) {
                        struct client *c = &g_clients[i];
                        if (c->fd >= 0 && c->state == CS_READY && client_jpeg_ok(c)) {
                            want = 1;
                            break;
                        }
                    }
                }
                if (want && jpeg) {
                    int n = vnc_jpeg_frame(jpeg, fr.px, &jpg, 200);
                    jlen = n > 0 ? (size_t)n : 0;
                    if (n <= 0)
                        jpg = NULL;
                }
            }
            if (jlen)
                vnc_http_preview_frame(http, jpg, jlen);
        }

        /* Keep the page's view of the world current. Every field here is read by
         * vnc_http at the moment a request for `/` arrives, so it is refreshed every
         * turn rather than on a timer -- the operator pressing "reload" and reading a
         * minute-old client count is exactly the kind of small lie this page must not
         * tell. */
        {
            int clients = 0, jpeg_ok = 0;
            for (i = 0; i < VNC_MAX_CLIENTS; i++) {
                struct client *c = &g_clients[i];
                if (c->fd < 0 || c->state != CS_READY) continue;
                clients++;
                if (client_jpeg_ok(c)) jpeg_ok++;
            }
            hst.vnc_clients = clients;
            hst.vnc_jpeg_clients = jpeg_ok;
            /* With no encoder there is no encoder to describe, and asking one for its
             * status is the kind of "the switch is off but the code still runs" that
             * makes a page lie. */
            hst.jpeg_ok = jpeg ? vnc_jpeg_ok(jpeg) : 0;
            hst.jpeg_status = jpeg ? vnc_jpeg_status(jpeg)
                                   : "off -- sharing is not on, so nothing is encoded";
            /* Name the RIGHT reason. This said "did not offer Tight", which is the
             * wrong half of the rule: Apple's client does offer Tight and still gets
             * Raw, because it never asks for a JPEG quality level. A page that blames
             * the wrong half sends the next person to look in the wrong place. */
            if (mode.mode == VNC_MODE_HWJPEG && clients && !jpeg_ok)
                hst.vnc_note = "The connected client did not ask for JPEG -- a client "
                               "must advertise a JPEG quality level for that, and "
                               "macOS Screen Sharing never does -- so it is being sent "
                               "Raw frames even though the mode is hwjpeg.";
            else if (mode.mode == VNC_MODE_HWJPEG && !clients)
                hst.vnc_note = "No VNC client is connected yet.";
            else
                hst.vnc_note = NULL;
        }

        /* Hand the newest frame to whoever is waiting for one, but only once the
         * previous update has actually gone out -- the queue is the backpressure, and
         * a client that cannot keep up simply gets fewer frames rather than a longer
         * and longer queue. */
        for (i = 0; i < VNC_MAX_CLIENTS; i++) {
            struct client *c = &g_clients[i];
            int enc;
            if (c->fd < 0 || c->state != CS_READY) continue;
            if (!c->want_update || out_pending(c)) continue;
            if (c->incremental && c->got_frame && c->sent_serial == fr.serial) continue;

            /* THE ORDER MATTERS AND IT IS THE ORDER OF WHAT THE CLIENT CAN ACTUALLY
             * DECODE. A hardware JPEG is the smallest update there is, but only a
             * client that asked for a JPEG quality level may be sent one -- so it is
             * first, and for macOS's Screen Sharing it is never taken. A compressing
             * encoding is next, and there are two of them because real clients split
             * on this: Tight (7) is what TigerVNC, RealVNC and this repo's own test
             * client offer, and zlib (6) is what macOS's Screen Sharing offers --
             * and Apple offers no other compressing encoding, which is why a session
             * with it ran on the Raw floor until encoding 6 existed. Raw is the floor,
             * and the floor is what this server was doing all the time before today.
             *
             * AND THE JPEG RUNG IS GATED ON THE MODE, WHICH IS NOT OBVIOUS. A JPEG is
             * produced whenever ANYTHING wants one, and the PREVIEW wants one in either
             * mode -- so in raw mode a `jpg` is sitting right here, made for the control
             * page, and handing it to a VNC client as a bonus silently downgrades that
             * session from lossless zlib (~200 KB) to lossy JPEG (~37 KB) that nobody
             * asked for. Measured 2026-10-10: opening the config page on :5904 -- which
             * embeds /preview.mjpg -- visibly degraded a noVNC session that was
             * otherwise pixel-identical to macOS's. The mode is the honest test here,
             * and it is the rule the `want` computation above already states in words:
             * "A VNC client wants it only in hwjpeg mode." */
            enc = comp_encoding_for(c, o);
            if (mode.mode == VNC_MODE_HWJPEG && jpg && jlen && client_jpeg_ok(c)) {
                if (send_update(c, fr.px, fr.w, fr.h, jpg, jlen) < 0) {
                    vlog("%s: cannot keep up (%zu bytes queued unsent); dropping it "
                         "rather than growing", c->peer, c->outlen - c->outoff);
                    client_close(c);
                    continue;
                }
            } else if (enc >= 0) {
                int r = send_comp_update(c, fr.px, fr.w, fr.h, c->incremental,
                                         o->zlib_level, enc);
                if (r == 0)
                    continue;               /* nothing moved; the request stands */

                if (r < 0 && c->comp_failed) {
                    /* No stream at all -- comp_ready has already said why. Raw is
                     * the honest answer here and the session carries on, slowly, the
                     * way it did before any of this existed. */
                    enc = -1;
                    if (send_update(c, fr.px, fr.w, fr.h, jpg, jlen) < 0) {
                        vlog("%s: cannot keep up (%zu bytes queued unsent); dropping "
                             "it rather than growing", c->peer,
                             c->outlen - c->outoff);
                        client_close(c);
                        continue;
                    }
                } else if (r < 0) {
                    /* A stream existed and then failed part-way through an update.
                     * The client has already been sent a prefix of it, so there is
                     * nothing to fall back to -- its inflate state cannot be
                     * repaired and it has to reconnect. Logged where it happened. */
                    client_close(c);
                    continue;
                }
            } else if (send_update(c, fr.px, fr.w, fr.h, jpg, jlen) < 0) {
                vlog("%s: cannot keep up (%zu bytes queued unsent); dropping it rather "
                     "than growing", c->peer, c->outlen - c->outoff);
                client_close(c);
                continue;
            }
            c->want_update = 0;
            c->got_frame = 1;
            c->sent_serial = fr.serial;
            fr.sends++;
            if (c->frames == 1) {
                /* WHAT IT ACTUALLY GOT, AND WHY, IN ONE PLACE. This used to be two
                 * lines printed at different times -- a fallback message before the
                 * send, computed from the client's list, and a one-word label after
                 * it. The fallback message then had to guess whether the deflate
                 * stream would come up, and once guessed wrong. Printing both here,
                 * after the update has gone out, makes every word of it a fact about
                 * something that has already happened. */
                /* THIS PREDICATE MUST MIRROR THE SEND SITE ABOVE, MODE GATE INCLUDED.
                 * It did not, for one build: the gate was added to the send and not to
                 * this, so a frame that went out as lossless zlib was logged as
                 * "Tight, hardware JPEG" -- the log stating the opposite of the fact it
                 * exists to report, and doing it in the one line an operator reads to
                 * find out what a session actually got. The byte count was the tell
                 * (183 KB is a zlib frame; a hardware JPEG here is ~37 KB), and the
                 * wire settled it: encoding 6. Keep the two conditions identical. */
                const int jpeg = mode.mode == VNC_MODE_HWJPEG && jpg && jlen &&
                                 client_jpeg_ok(c);
                const char *what =
                    jpeg ? "Tight, hardware JPEG"
                    : enc == VNC_ENC_ZLIB ? "zlib (encoding 6), lossless"
                    : enc == VNC_ENC_TIGHT ? "Tight (encoding 7), zlib, lossless"
                    : vnc_pf_is_rgb565_le(&c->pf) ? "Raw, RGB565 copied"
                                                  : "Raw, converted";
                vlog("%s: first frame sent (%zu bytes, %s)", c->peer,
                     c->outlen - c->outoff, what);
                if (mode.mode == VNC_MODE_HWJPEG && !jpeg)
                    vlog("%s: ... the mode is hwjpeg and this client cannot be sent "
                         "JPEG: it offered no JPEG quality level, and the spec allows "
                         "a lossy payload only when the client asks for one. So the "
                         "session runs on %s, and the preview on the control page is "
                         "the only place the hardware encoder can be seen.",
                         c->peer, what);
            }
            if (out_drain(c) < 0) {
                vlog("%s: connection closed while sending", c->peer);
                client_close(c);
            }
        }

        /* One line a minute, so "is it costing the player anything" is a question
         * with an answer in the log rather than in top. The frame line is printed in
         * both states -- "sharing off" is an answer too, and the one that matters
         * when the operator is looking at why a boot came up blank. */
        if (now - fr.last_report >= 60000) {
            int live = 0;
            for (i = 0; i < VNC_MAX_CLIENTS; i++)
                if (g_clients[i].fd >= 0) live++;
            if (cap)
                vlog("status: %s mode, %d client(s), %llu frame(s) composed, %llu sent, "
                     "%d plane(s) composited -- %s", vnc_mode_name(mode.mode), live,
                     fr.ticks, fr.sends, vnc_capture_plane_count(cap),
                     vnc_capture_status(cap));
            else
                vlog("status: sharing OFF -- %d client(s), nothing captured, no display "
                     "opened, %llu frame(s) composed before it was turned off",
                     live, fr.ticks);
            fr.last_report = now;
        }
    }

    if (ls >= 0) close(ls);
    for (i = 0; i < VNC_MAX_CLIENTS; i++) client_close(&g_clients[i]);
    /* THE FINGER LAST, and after every client that could have pressed has been closed
     * -- client_close() releases the panel on its way out when that client owned it,
     * and this is the backstop for anything it missed. The failure it prevents -- a
     * press left down on the operator's panel by a process that has exited -- is the one
     * that needs somebody to walk over and touch the glass to clear, so it is worth a
     * line even though there is no path that reaches it today. */
    vnc_input_release(&input);
    vnc_input_close(&input);
    g_input = NULL;
    g_input_sw = NULL;
    vnc_http_close(http);
    if (jpeg) vnc_jpeg_destroy(jpeg);
    free(fr.px);
    free(fr.prev);
    if (cap) vnc_capture_close(cap);
    return 0;
}
