/*
 * vncserve.c -- the Pi's screen over VNC, with a runtime raw <-> hwjpeg switch.
 *
 * WHAT THIS HAS TO DO THAT x11vnc DOES NOT. rbp paints the deck into /dev/fb0,
 * but it paints the two edge drawers, the top band and the USB-STOP chooser onto
 * vc4 DRM overlay planes on /dev/dri/card1. A viewer that reads fb0 alone shows a
 * screen with no drawers and no band -- docs/06-display.md names that gap. So the
 * capture composites the active planes over fb0 before anything is sent, which is
 * most of what is unusual about this file.
 *
 * The other thing is the switch. The operator asked for the option to move between
 * raw 16bpp and the hardware JPEG, because the unit is on wlan0 and 1280x800x2
 * bytes is 2 MB a frame -- 32 Mbit/s at 2 fps. /dev/video11 (bcm2835-codec-encode)
 * takes fb0's own RGB565 and emits MJPG without touching the CPU, and libvncserver
 * cannot be handed a JPEG it did not make, so the RFB server here is hand-rolled
 * and needs only libc.
 *
 * --probe exists because the first question is not about this port at all: does
 * macOS's Screen Sharing advertise Tight at all, and does it take security type
 * None? Both are answered by eighty lines that speak the greeting and then only
 * listen, which is far cheaper than discovering either after the encoder is written.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vnc_capture.h"
#include "vnc_input.h"
#include "vnc_mode.h"
#include "vnc_net.h"
#include "vnc_rfb.h"
#include "vnc_session.h"

/* --- logging and sockets ---------------------------------------------------
 *
 * Both moved to vnc_net so the session server can share them; re-exported here under
 * the names the probe below was written against. */
#define logfp() vnc_logfp()

/* THE VERSION THE SERVER ANNOUNCES, AND IT IS NOT A DETAIL.
 *
 * The server speaks first, and the version it names decides which dialect the
 * client will use -- so it decides the shape of everything that follows. 3.008 is
 * the correct, modern, conservative answer: any client that understands 3.7+
 * negotiation gets it, and a client that does not speak it falls back to 3.3.
 *
 * macOS's Screen Sharing is the reason this is a flag rather than a constant. Run
 * against 3.008 it answers "RFB 003.003" -- it does not simply use 3.8; it drops all
 * the way to legacy 3.3 -- and then says nothing further. 003.889 is the version
 * Apple's own server (screensharingd) announces, i.e. the dialect Apple's *client*
 * is written against, so the probe offers both and the log decides which one gets
 * a conversation out of it. */
static char g_version[VNC_RFB_VERSION_LEN];   /* filled in main(); see above */

/* WHICH SECURITY TYPES WE OFFER, AND WHY IT IS A FLAG.
 *
 * The design said None, on the grounds that the unit is on the operator's own
 * network and already runs sshd. That was wrong twice over, and the probe found
 * both: against a server announcing 3.889, macOS's Screen Sharing reads a list of
 * [None] and closes the socket without answering. It refuses unauthenticated
 * servers outright. So the choice is not ours to make -- offer what the client will
 * take, or have no client.
 *
 * 'both' exists to make the client's preference visible rather than assumed: it is
 * offered None and VNC auth together and the log says which one it picks. */
enum { SEC_NONE = 0, SEC_VNC = 1, SEC_BOTH = 2 };
static int g_sec_offer = SEC_BOTH;

/* Named logfp and not logf: libm has a logf(), and gcc warns about the clash. */

/* --- small printing aids -------------------------------------------------- */

/* Escape anything that is not printable ASCII, so a version string with a stray
 * byte in it is visible in the log rather than eating the rest of the line. */
static void print_escaped(FILE *f, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c >= 0x20 && c < 0x7F) fputc(c, f);
        else fprintf(f, "\\x%02X", c);
    }
}

#define be32 vnc_net_be32
#define write_all vnc_net_write_all
#define read_to vnc_net_read_to

/* --- the probe ------------------------------------------------------------
 *
 * Speak the greeting, send a ServerInit so the client gets as far as telling us
 * what it wants, then read and decode every message it sends. It never sends a
 * FramebufferUpdate, so the client sits at "connecting" -- that is expected, and
 * the log is the result, not the glass.
 *
 * The two answers that matter are on the `SUMMARY` line:
 *   security=None    the client took the only type we offer. If it does not, the
 *                    whole route needs an auth type it will accept.
 *   Tight=yes        the client advertised encoding 7. Without it a JPEG rectangle
 *                    cannot ride this session, and the hardware encoder can only
 *                    be shown through the page's own preview instead.
 */
static void probe_session(int fd, const char *peer, int seconds)
{
    char ver[VNC_RFB_VERSION_LEN];
    uint8_t sec[2], chosen = 0, shared = 0;
    uint8_t pf16[16];
    char pfdesc[160];
    int maj = 3, min = 3, tight = 0, jpeg_pseudo = 0, taken_none = 0;
    struct timespec t0, t1;

    vlog("=== connection from %s", peer);

    memcpy(ver, g_version, sizeof ver);
    if (write_all(fd, ver, sizeof ver) < 0) { vlog("write version failed"); return; }
    {
        FILE *f = logfp();
        fprintf(f, "server version sent: \"");
        print_escaped(f, g_version, sizeof ver);
        fprintf(f, "\" (12 bytes)\n");
        fflush(f);
    }

    if (!read_to(fd, ver, sizeof ver, 5000)) {
        vlog("client sent no version within 5s -- giving up");
        return;
    }
    {
        FILE *f = logfp();
        fprintf(f, "client version raw: \"");
        print_escaped(f, ver, sizeof ver);
        fprintf(f, "\"\n");
        fflush(f);
    }

    if (vnc_rfb_parse_version(ver, &maj, &min))
        vlog("client version parsed: %d.%d -> effective %d.%d", maj, min, maj, min > 8 ? 8 : min);
    else
        vlog("client version unparseable; treating as 3.3");

    /* THE HANDSHAKE DIVERGES AT 3.7 AND IT DIVERGES SILENTLY.
     *
     * At 3.7 and later the server offers a *list* of security types, the client
     * answers with the one it chose, and the server then sends a SecurityResult.
     * At 3.3 the server sends a single uint32 type and the client says nothing.
     *
     * Getting the *list* wrong does not error. A 3.3 client handed a 3.8 security
     * list reads our two bytes plus the two that follow as a four-byte type of
     * 0x01010000, and then waits forever for a ServerInit that will never come --
     * which is exactly what macOS's client did here the first time this probe was
     * run (it announces 003.003, not the 003.889 it is documented to send, so the
     * probe's log and not the documentation is what decides).
     *
     * The SecurityResult is NOT part of that divergence -- see
     * vnc_rfb_sends_security_result, and the note on the 3.3 branch below. */
    if (min >= 7) {
        int n = 0;
        if (g_sec_offer != SEC_VNC)  sec[n++] = VNC_SEC_NONE;
        if (g_sec_offer != SEC_NONE) sec[n++] = VNC_SEC_VNC;
        if (write_all(fd, &n, 1) < 0) return;
        if (write_all(fd, sec, (size_t)n) < 0) { vlog("write security list failed"); return; }
        vlog("security types offered: %d %s  [3.7+ negotiation]", n,
             g_sec_offer == SEC_NONE ? "(None)" : g_sec_offer == SEC_VNC ? "(VNC auth)" : "(None, VNC auth)");

        if (!read_to(fd, &chosen, 1, 300000)) { vlog("client chose no security type in 300s"); return; }
        if (chosen == VNC_SEC_NONE) {
            taken_none = 1;
            vlog("client chose security type %u (None)", chosen);
        } else if (chosen == VNC_SEC_VNC) {
            vlog("client chose security type %u (VNC auth)", chosen);
        } else {
            vlog("client chose security type %u -- one we did not offer (30/33/35 are "
                 "Apple's own Diffie-Hellman schemes).", chosen);
        }

        /* Type 2 is the standard DES challenge/response. The probe does not verify
         * anything -- it sends a fixed challenge, logs whatever comes back, and then
         * claims success, because the only question here is whether Apple's client
         * will *play*: whether it takes the type and answers at all. */
        if (chosen == VNC_SEC_VNC) {
            uint8_t chal[16], resp[16];
            int k;
            for (k = 0; k < 16; k++) chal[k] = (uint8_t)(0xA0 + k);
            if (write_all(fd, chal, 16) < 0) { vlog("write challenge failed"); return; }
            vlog("sent 16-byte challenge A0..AF");
            if (!read_to(fd, resp, 16, 300000)) {
                vlog("client sent no auth response in 300s -- it took the type but "
                     "did not answer");
                return;
            }
            {
                FILE *f = logfp();
                fprintf(f, "client auth response:");
                for (k = 0; k < 16; k++) fprintf(f, " %02X", resp[k]);
                fprintf(f, "\n");
                fflush(f);
            }
        }

        /* The word is owed for VNC auth in every version, and for None only at 3.8.
         * Omitting it where it is owed is the other classic silent desync: the
         * client waits for it, or reads our ServerInit's width as its result code. */
        if (chosen == VNC_SEC_NONE || chosen == VNC_SEC_VNC) {
            if (vnc_rfb_sends_security_result(min, chosen)) {
                uint8_t ok[4] = { 0, 0, 0, 0 };
                if (write_all(fd, ok, 4) < 0) { vlog("write SecurityResult failed"); return; }
                vlog("SecurityResult: 0 (ok, unconditionally -- this is a probe)");
            } else {
                vlog("no SecurityResult: 3.%d with type None does not send one",
                     min > 8 ? 8 : min);
            }
        }
    } else {
        uint8_t t[4] = { 0, 0, 0, VNC_SEC_NONE };
        if (g_sec_offer == SEC_VNC) t[3] = VNC_SEC_VNC;
        if (write_all(fd, t, 4) < 0) { vlog("write security type failed"); return; }
        taken_none = (t[3] == VNC_SEC_NONE);
        vlog("security type sent: %u  [3.3: no list and no choice]", t[3]);

        /* THE 3.3 BRANCH USED TO STOP HERE, AND THAT MADE IT A LIAR. It offered VNC
         * auth, never sent the challenge that auth consists of, never sent the
         * SecurityResult that follows it, and then reported the client's silence as
         * "no ClientInit within 5s" -- blaming the client for a hang the probe had
         * caused. macOS's client speaks 3.3, so this is the branch it lands in, and
         * the one branch that had never been exercised. */
        if (t[3] == VNC_SEC_VNC) {
            uint8_t chal[16], resp[16];
            int k;
            for (k = 0; k < 16; k++) chal[k] = (uint8_t)(0xA0 + k);
            if (write_all(fd, chal, 16) < 0) { vlog("write challenge failed"); return; }
            vlog("sent 16-byte challenge A0..AF");
            if (!read_to(fd, resp, 16, 300000)) {
                vlog("client sent no auth response in 300s -- it took the type but "
                     "did not answer");
                return;
            }
            {
                FILE *f = logfp();
                fprintf(f, "client auth response:");
                for (k = 0; k < 16; k++) fprintf(f, " %02X", resp[k]);
                fprintf(f, "\n");
                fflush(f);
            }
            {
                uint8_t ok[4] = { 0, 0, 0, 0 };
                if (write_all(fd, ok, 4) < 0) { vlog("write SecurityResult failed"); return; }
            }
            vlog("SecurityResult: 0 (ok, unconditionally -- this is a probe)");
        }
    }

    if (!read_to(fd, &shared, 1, 5000)) { vlog("client sent no ClientInit within 5s"); return; }
    vlog("ClientInit: shared-flag=%u", shared);

    /* ServerInit. Real geometry (the probe is not drawing, but the client sizes its
     * window from this and may refuse nonsense), our own pixel format, and a name. */
    {
        const char *name = "rbpi4b rbp";
        uint32_t nl = (uint32_t)strlen(name);
        uint8_t hdr[24];
        hdr[0] = 1280 >> 8; hdr[1] = 1280 & 0xFF;
        hdr[2] = 800 >> 8;  hdr[3] = 800 & 0xFF;
        vnc_pf_write(hdr + 4, &vnc_pf_rgb565);
        hdr[20] = (uint8_t)(nl >> 24); hdr[21] = (uint8_t)(nl >> 16);
        hdr[22] = (uint8_t)(nl >> 8);  hdr[23] = (uint8_t)nl;
        if (write_all(fd, hdr, sizeof hdr) < 0 ||
            write_all(fd, name, nl) < 0) { vlog("write ServerInit failed"); return; }
    }
    vnc_pf_describe(&vnc_pf_rgb565, pfdesc, sizeof pfdesc);
    vlog("ServerInit: 1280x800, %s, name \"rbpi4b rbp\"", pfdesc);

    /* Now listen. Nothing is sent from here on. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint8_t type, buf[16];
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= seconds) { vlog("probe window elapsed (%ds)", seconds); break; }

        if (!read_to(fd, &type, 1, 500)) continue;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        vlog("client msg type %u  (+%ld ms)", type, (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                                                          (t1.tv_nsec - t0.tv_nsec) / 1000000));

        switch (type) {
        case 0: {                       /* SetPixelFormat */
            struct vnc_pixel_format pf;
            if (!read_to(fd, buf, 3, 2000)) { vlog("  truncated pad"); return; }
            if (!read_to(fd, pf16, 16, 2000)) { vlog("  truncated pixel format"); return; }
            vnc_pf_read(pf16, &pf);
            vnc_pf_describe(&pf, pfdesc, sizeof pfdesc);
            vlog("  SetPixelFormat: %s", pfdesc);
            vlog("  matches fb0's RGB565? %s", vnc_pf_is_rgb565_le(&pf) ? "YES (Raw = memcpy)"
                                                                        : "no (needs conversion)");
            break;
        }
        case 2: {                       /* SetEncodings */
            uint8_t n2[3];
            uint16_t count, i;
            if (!read_to(fd, n2, 3, 2000)) { vlog("  truncated"); return; }
            count = (uint16_t)((n2[1] << 8) | n2[2]);
            vlog("  SetEncodings: %u entries", count);
            for (i = 0; i < count; i++) {
                uint8_t e4[4];
                int32_t e;
                const char *nm;
                if (!read_to(fd, e4, 4, 2000)) { vlog("  truncated at entry %u", i); return; }
                e = (int32_t)be32(e4);
                if (e == VNC_ENC_TIGHT) tight = 1;
                if (vnc_rfb_pseudo_is_quality(e)) jpeg_pseudo = 1;
                nm = e >= 0 ? vnc_rfb_encoding_name(e) : vnc_rfb_pseudo_name(e);
                vlog("    [%2u] %11d  %s", i, e, nm ? nm : "(unknown)");
            }
            break;
        }
        case 3: {                       /* FramebufferUpdateRequest */
            uint8_t r[9];
            if (!read_to(fd, r, 9, 2000)) { vlog("  truncated"); return; }
            vlog("  FramebufferUpdateRequest: incremental=%u rect %u,%u %ux%u",
                 r[0], (r[1] << 8) | r[2], (r[3] << 8) | r[4],
                 (r[5] << 8) | r[6], (r[7] << 8) | r[8]);
            break;
        }
        case 4: {                       /* KeyEvent */
            uint8_t k[7];
            if (!read_to(fd, k, 7, 2000)) { vlog("  truncated"); return; }
            vlog("  KeyEvent: down=%u keysym=0x%08x", k[0], be32(k + 3));
            break;
        }
        case 5: {                       /* PointerEvent */
            uint8_t p[5];
            if (!read_to(fd, p, 5, 2000)) { vlog("  truncated"); return; }
            vlog("  PointerEvent: buttons=0x%02x at %u,%u",
                 p[0], (p[1] << 8) | p[2], (p[3] << 8) | p[4]);
            break;
        }
        case 6: {                       /* ClientCutText */
            uint8_t l4[7];
            uint32_t len;
            if (!read_to(fd, l4, 7, 2000)) { vlog("  truncated"); return; }
            len = be32(l4 + 3);
            vlog("  ClientCutText: %u bytes (discarded)", len);
            {   /* drain in chunks; it can be large */
                uint8_t sink[256];
                while (len) {
                    size_t chunk = len > sizeof sink ? sizeof sink : len;
                    if (!read_to(fd, sink, chunk, 2000)) { vlog("  cut-text truncated"); return; }
                    len -= (uint32_t)chunk;
                }
            }
            break;
        }
        case 251: {                     /* SetDesktopSize */
            uint8_t d[5];
            if (!read_to(fd, d, 5, 2000)) { vlog("  truncated"); return; }
            vlog("  SetDesktopSize: %ux%u, %u screens", (d[1] << 8) | d[2],
                 (d[3] << 8) | d[4], d[0] == 0 ? 0 : d[0]);
            vlog("  ... we advertised no such pseudo-encoding; ignoring is correct.");
            break;
        }
        default:
            vlog("  *** unknown message type %u. Its length is not knowable from the "
                 "type alone, so the stream cannot be resynchronised. Closing.", type);
            goto done;
        }
    }
done:
    {
        FILE *f = logfp();
        fprintf(f, "SUMMARY security=None:%s Tight:%s JPEG-quality-pseudo:%s\n",
                taken_none ? "YES" : "NO", tight ? "YES" : "NO", jpeg_pseudo ? "YES" : "NO");
        fflush(f);
    }
}

/* --- --dump: one frame, to a file ------------------------------------------ */

/*
 * THE CHEAPEST POSSIBLE PROOF THAT THE COMPOSITE IS REAL, and it exists because the
 * alternative -- building the whole RFB server, connecting a real client, and judging
 * the picture through a VNC window -- makes a broken composite look like a broken
 * protocol. Here the frame goes to a file, the plane table is printed, and the file is
 * converted and looked at.
 *
 * It is also the only way to answer the question the whole capture layer rests on:
 * does the drawer rbp has open actually appear? A frame dumped with a drawer out and
 * one with it closed differ by exactly the plane, and nothing else.
 */
static int dump_once(const char *path, int include_primary)
{
    char err[256];
    struct vnc_capture *c;
    uint16_t *frame;
    int w, h, i;
    FILE *f;

    c = vnc_capture_open(err, sizeof err);
    if (!c) { fprintf(stderr, "capture: %s\n", err); return 1; }

    w = vnc_capture_width(c);
    h = vnc_capture_height(c);
    frame = malloc((size_t)w * (size_t)h * sizeof *frame);
    if (!frame) { fprintf(stderr, "out of memory for %dx%d\n", w, h); vnc_capture_close(c); return 1; }

    if (include_primary)
        vnc_capture_include_primary(c, 1);

    vnc_capture_frame(c, frame);
    fprintf(stderr, "%s\n", vnc_capture_status(c));
    for (i = 0; i < vnc_capture_plane_count(c); i++) {
        const struct vnc_plane *p = &vnc_capture_planes(c)[i];
        fprintf(stderr, "  plane %d: z=%d dst=(%d,%d) %dx%d stride=%d\n",
                i, p->zpos, p->dst_x, p->dst_y, p->w, p->h, p->src_stride_px);
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        free(frame); vnc_capture_close(c); return 1;
    }
    if (fwrite(frame, sizeof *frame, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
        fprintf(stderr, "short write to %s\n", path);
        fclose(f); free(frame); vnc_capture_close(c); return 1;
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d RGB565, %d bytes)\n",
            path, w, h, w * h * 2);

    free(frame);
    vnc_capture_close(c);
    return 0;
}

/* --- main ----------------------------------------------------------------- */

/* deflate takes 1..9 and returns Z_STREAM_ERROR for anything else, so a bad number
 * would not fail the run -- it would leave every client on Raw, compressing nothing,
 * with only the one line printed at startup to say so. Hence a check where the value
 * is accepted rather than where it is used. 0 is our own spelling of "off", which is
 * why it is legal here and illegal to zlib. */
static int zlib_level_ok(int v)
{
    return v == 0 || (v >= 1 && v <= 9);
}

static void usage(void)
{
    fputs("usage: vncserve [--port N] [--bind ADDR] [--fps N] [--password PW]\n"
          "                 [--version NNN.NNN] [--name STR] [--log PATH]\n"
          "                 [--http-port N | --no-http] [--mode-file PATH]\n"
          "                 [--mode raw|hwjpeg] [--zlib-level 0..9]\n"
          "                 [--input on|off] [--input-file PATH] [--input-dev PATH]\n"
          "                     Serve the screen over RFB until killed, and the\n"
          "                     switch's control page on --http-port (default: one\n"
          "                     above --port). Tapping the picture on that page hides\n"
          "                     every word on it, for the full-screen view.\n"
          "                     --input makes a click in the picture a REAL press on\n"
          "                     the panel. It starts OFF, and --input-file is the\n"
          "                     switch the page writes.\n"
          "       vncserve --probe [--port N] [--bind ADDR] [--seconds N]\n"
          "                 [--version NNN.NNN] [--sec none|vnc|both]\n"
          "                 [--once] [--log PATH]\n"
          "                     Greet a client, decode what it says, send no picture.\n"
          "       vncserve --dump FILE [--include-primary]   one composed frame, raw RGB565\n"
          "       vncserve --planes                           every DRM plane, active or not\n",
          stderr);
}

int main(int argc, char **argv)
{
    int port = 5900, seconds = 60, once = 0, i, fps = 2;
    int mode_probe = 0;
    int http_port = -1;                 /* -1: not given, so --port + 1 */
    int default_mode = VNC_MODE_RAW;
    int default_input = VNC_INPUT_OFF;  /* OFF until the page is told otherwise */
    int zlib_level = 1;                 /* 0 disables compressing updates entirely */
    const char *bindaddr = NULL, *logpath = NULL, *mode_file = NULL;
    const char *input_file = NULL, *input_dev = NULL;
    const char *password = getenv("RB_VNC_PASSWORD");
    const char *name = "rbpi4b rbp";
    struct vnc_session_opts opts;

    signal(SIGPIPE, SIG_IGN);

    memcpy(g_version, VNC_RFB_VERSION, sizeof g_version);

    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "--dump") == 0) {
        int all = 0;
        if (argc < 3) { usage(); return 2; }
        for (i = 3; i < argc; i++)
            if (!strcmp(argv[i], "--include-primary")) all = 1;
            else { usage(); return 2; }
        return dump_once(argv[2], all);
    }
    if (strcmp(argv[1], "--planes") == 0) {
        char err[256];
        struct vnc_capture *c = vnc_capture_open(err, sizeof err);
        if (!c) { fprintf(stderr, "capture: %s\n", err); return 1; }
        vnc_capture_describe(c);
        vnc_capture_close(c);
        return 0;
    }
    if (strcmp(argv[1], "--probe") == 0) mode_probe = 1;

    for (i = mode_probe ? 2 : 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)         port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)  seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc)     bindaddr = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)      logpath = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)       fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--password") && i + 1 < argc) password = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)     name = argv[++i];
        else if (!strcmp(argv[i], "--http-port") && i + 1 < argc) http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-http"))                    http_port = 0;
        else if (!strcmp(argv[i], "--mode-file") && i + 1 < argc) mode_file = argv[++i];
        else if (!strcmp(argv[i], "--zlib-level") && i + 1 < argc) {
            /* Same range and the same meaning as RB_VNC_ZLIB_LEVEL below, and this is
             * the form rb.conf's value reaches through: vnc-run.sh passes it, and the
             * environment variable is only the fallback for a hand-run server. A
             * setting that appears in rb.conf and does not appear in the command line
             * does not reach this process at all -- rb_load_conf sets a shell variable
             * and does not export it. */
            int v = atoi(argv[++i]);
            if (!zlib_level_ok(v)) {
                fprintf(stderr, "--zlib-level wants 0, or 1..9 (not %d)\n", v);
                return 2;
            }
            zlib_level = v;
        }
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            int m;
            if (vnc_mode_parse(argv[++i], &m) < 0) {
                fprintf(stderr, "--mode wants raw or hwjpeg\n");
                return 2;
            }
            default_mode = m;
        }
        else if (!strcmp(argv[i], "--once"))                      once = 1;
        else if (!strcmp(argv[i], "--input-file") && i + 1 < argc) input_file = argv[++i];
        else if (!strcmp(argv[i], "--input-dev") && i + 1 < argc)  input_dev = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            /* Parsed by vnc_input's own reader, so "on", "yes" and "1" are the same
             * word here as they are in the file the page writes. */
            int v;
            if (vnc_input_parse(argv[++i], &v) < 0) {
                fprintf(stderr, "--input wants on or off\n");
                return 2;
            }
            default_input = v;
        }
        else if (mode_probe && !strcmp(argv[i], "--sec") && i + 1 < argc) {
            const char *s = argv[++i];
            if (!strcmp(s, "none")) g_sec_offer = SEC_NONE;
            else if (!strcmp(s, "vnc")) g_sec_offer = SEC_VNC;
            else if (!strcmp(s, "both")) g_sec_offer = SEC_BOTH;
            else { fprintf(stderr, "--sec wants none|vnc|both\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--version") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strlen(v) != 7 || v[3] != '.' ||
                strspn(v, "0123456789.") != 7) {
                fprintf(stderr, "--version wants NNN.NNN, e.g. 003.889\n");
                return 2;
            }
            /* Built by hand rather than with snprintf: the buffer is exactly 12
             * bytes with no room for a NUL, which is the whole point of the wire
             * format, and snprintf's truncation warning is correct about that. */
            memcpy(g_version, "RFB ", 4);
            memcpy(g_version + 4, v, 7);
            g_version[11] = '\n';
        }
        else { usage(); return 2; }
    }

    /* The password and the fps are also readable from the environment, so the unit's
     * rb.conf can set them without the launcher having to build a command line. The
     * flag wins, so a one-off run over ssh can override without editing anything. */
    if (!password) password = getenv("RB_VNC_PASSWORD");
    if (!password) password = "";
    if (getenv("RB_VNC_FPS") && fps == 2) {
        int v = atoi(getenv("RB_VNC_FPS"));
        if (v > 0) fps = v;
    }
    if (!mode_file) mode_file = getenv("RB_VNC_MODE_FILE");
    if (http_port < 0 && getenv("RB_VNC_HTTP_PORT"))
        http_port = atoi(getenv("RB_VNC_HTTP_PORT"));
    if (getenv("RB_VNC_MODE")) {
        int m;
        if (vnc_mode_parse(getenv("RB_VNC_MODE"), &m) == 0)
            default_mode = m;
        else
            fprintf(stderr, "RB_VNC_MODE=%s names no mode; using %s\n",
                    getenv("RB_VNC_MODE"), vnc_mode_name(default_mode));
    }
    /* The zlib level, 0 to turn compression off for every client. It governs BOTH
     * compressing encodings -- Tight (7) and zlib (6) -- because underneath the wrapper
     * they are the same deflate stream over the same bands; the name says "zlib" because
     * that is the library, not because it is the encoding. It is a level rather than a
     * boolean because zlib already has a spelling for "off" -- but 0 here means "send
     * everything Raw", not "compress at level 0", which is what zlib would make of it.
     * The range check is not decoration: deflate accepts 1..9 and returns Z_STREAM_ERROR
     * for anything else, which would leave every client on Raw with only a one-line
     * warning to explain it. */
    if (getenv("RB_VNC_ZLIB_LEVEL")) {
        int v = atoi(getenv("RB_VNC_ZLIB_LEVEL"));
        if (zlib_level_ok(v))
            zlib_level = v;
        else
            fprintf(stderr, "RB_VNC_ZLIB_LEVEL=%s is not 0..9; using %d\n",
                    getenv("RB_VNC_ZLIB_LEVEL"), zlib_level);
    }
    /* The page goes next door to the session by default, so one port number in rb.conf
     * names both and the operator only has one thing to remember. */
    if (http_port < 0)
        http_port = port + 1;

    /* THE INPUT SWITCH, which rb.conf reaches through RB_VNC_INPUT. It is read here and
     * not in vnc_session for the same reason every other setting is: one place decides
     * what the option means, and a bad value is reported at startup rather than at the
     * moment someone clicks. Unrecognised words keep the default and say so -- the
     * default being OFF, which is the safe direction when the file that decides this
     * also cannot be read. */
    if (!input_file) input_file = getenv("RB_VNC_INPUT_FILE");
    if (!input_dev) input_dev = getenv("RB_VNC_INPUT_DEV");
    if (getenv("RB_VNC_INPUT")) {
        int v;
        if (vnc_input_parse(getenv("RB_VNC_INPUT"), &v) == 0)
            default_input = v;
        else
            fprintf(stderr, "RB_VNC_INPUT=%s is neither on nor off; input stays %s\n",
                    getenv("RB_VNC_INPUT"), vnc_input_name(default_input));
    }

    if (logpath) {
        FILE *f = fopen(logpath, "a");
        if (!f) { fprintf(stderr, "cannot open %s: %s\n", logpath, strerror(errno)); return 1; }
        vnc_log_to(f);
    }

    if (!mode_probe) {
        memset(&opts, 0, sizeof opts);
        opts.bindaddr = bindaddr;
        opts.port = port;
        opts.password = password;
        opts.announce_minor = (g_version[8] == '0' && g_version[9] == '3') ? 3 : 8;
        opts.name = name;
        opts.fps = fps > 0 ? fps : 1;
        opts.mode_path = mode_file;
        opts.default_mode = default_mode;
        opts.http_port = http_port;
        opts.zlib_level = zlib_level;
        opts.input_path = input_file;
        opts.input_dev = input_dev;
        opts.default_input = default_input;
        return vnc_session_run(&opts);
    }

    {
        int ls = vnc_net_listen(bindaddr, port);
        if (ls < 0) return 1;
        vlog("vncserve --probe listening on %s:%d", bindaddr ? bindaddr : "0.0.0.0", port);
        for (;;) {
            struct sockaddr_in peer;
            socklen_t pl = sizeof peer;
            char pbuf[64];
            int c = accept(ls, (struct sockaddr *)&peer, &pl);
            if (c < 0) { if (errno == EINTR) continue; vlog("accept: %s", strerror(errno)); break; }
            snprintf(pbuf, sizeof pbuf, "%s:%u", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            probe_session(c, pbuf, seconds);
            close(c);
            vlog("=== connection closed");
            if (once) break;
        }
        close(ls);
    }
    return 0;
}
