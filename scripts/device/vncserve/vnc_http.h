/*
 * vnc_http.h -- the little page that flips the switch, and the live preview beside it.
 *
 * THE OPERATOR ASKED FOR "the option to switch from raw to hwjpeg", and chose a small
 * web page with buttons as the way to do it. So this is a page, not an API: it says
 * what the server is doing right now, offers the two modes as buttons, and shows the
 * picture the current mode produces. It is served by the same process and the same
 * loop as the VNC session, from the same single encoded frame -- a preview costs one
 * socket write and no second capture.
 *
 * THE PREVIEW IS THE POINT OF THE PAGE, and it is deliberately not the VNC mode. It
 * shows what the hardware encoder is producing *at this moment*, whether or not the
 * VNC client has been switched to it -- so the operator can look at the JPEG artefacts
 * over WiFi and decide, which is a much better way to make the choice than switching
 * the shared screen back and forth to compare. Turning the page off turns the encoder
 * off with it; nothing is encoded for a preview nobody is looking at.
 *
 * NO FRAMEWORK, NO JAVASCRIPT, NO EXTERNAL ANYTHING. The page is one HTML string with
 * its style inline, the buttons are two submits of a GET form, and the preview is an
 * <img> pointing at a multipart/x-mixed-replace stream, which every browser on the
 * operator's desk has understood for twenty years. A LAN page that needs a CDN to
 * render is a page that fails the one time it is needed.
 *
 * A SECOND, BARER PORT. The operator asked for "a page that has just the preview image"
 * as well -- and a page with no chrome on it is not a different page, it is a different
 * USE: a window to park on a second screen, or to point something else at. So there are
 * two listeners, and they differ in exactly one respect: what `/` returns. Both serve
 * `/preview.mjpg` from the same encode, both count as a viewer, and both hang up the
 * same way. See vnc_http_open.
 */
#ifndef RBPI4B_VNC_HTTP_H
#define RBPI4B_VNC_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "vnc_mode.h"
#include "vnc_input.h"

/* What the page prints about the world outside this module. The session owns one of
 * these and refills it every tick; vnc_http only ever reads it, at the moment a request
 * for `/` arrives. Every pointer must stay valid for the life of the server -- strings
 * are literals or fields of the caller's own long-lived state. */
struct vnc_http_state {
    struct vnc_mode *mode;          /* the live mode; the page both shows and sets it */
    const char *mode_path;

    /* THE INPUT SWITCH, which the page also both shows and sets. It is a switch and not
     * a setting: the session re-reads the file every turn and acts on the transitions,
     * so turning this off from the page puts up any finger that was down. */
    struct vnc_input_switch *in;

    int jpeg_ok;                    /* is the hardware encoder up right now      */
    const char *jpeg_status;        /* one line from vnc_jpeg_status()           */
    int vnc_clients;                /* clients past the handshake                */
    int vnc_jpeg_clients;           /* of those, how many can be sent JPEG       */
    const char *vnc_note;           /* a sentence about the VNC side, or NULL    */
    int fps;                        /* the configured ceiling                    */
    int w, h;                       /* the screen                                */

    /* The port a VNC client has to be told. The page prints it, and it is here rather
     * than derived from the page's own port because the two are separate settings: the
     * page is one above the session by default and by accident of arithmetic, and a page
     * that guesses is a page that hands the operator a refused connection the one time
     * the two numbers were set apart. */
    int vnc_port;
};

struct vnc_http;

/* Bind the page's port. Returns NULL and fills `err` if it cannot be taken -- a page that
 * is silently absent is worse than one that refuses to start, because the operator will
 * spend the next ten minutes reloading it.
 *
 * There was once a second listener on which the same preview was served with no chrome on
 * it, for parking on a second screen. It is gone: the operator asked for the picture-only
 * view to be a tap on this page rather than a port of its own, so the page carries a
 * full-screen state and `RB_VNC_PREVIEW_PORT` no longer exists. */
struct vnc_http *vnc_http_open(const char *bindaddr, int port,
                               struct vnc_http_state *st, char *err, size_t errlen);
void vnc_http_close(struct vnc_http *h);

/* Accept and serve, without blocking. Called once per turn of the session's loop with
 * a zero timeout; the session's own poll already decides how often that is, so this
 * inherits its cadence rather than adding a second timer to the process. */
void vnc_http_poll(struct vnc_http *h, int timeout_ms);

/* Is anybody watching the preview? The session uses this to decide whether the
 * hardware encoder has to run at all when the VNC mode does not want it. */
int vnc_http_preview_wanted(const struct vnc_http *h);

/* Hand the newest JPEG to every preview watcher. Called once per frame, with the same
 * bytes the VNC clients are being sent. A watcher that has not finished taking the
 * previous frame is skipped rather than queued behind -- a preview is worth dropping a
 * frame for, and it is never worth growing this process for. */
void vnc_http_preview_frame(struct vnc_http *h, const uint8_t *jpeg, size_t len);

#endif /* RBPI4B_VNC_HTTP_H */
