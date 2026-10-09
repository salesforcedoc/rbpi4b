/*
 * vnc_http.c -- the control page. See vnc_http.h.
 *
 * A deliberately small HTTP/1.1: a request line, headers read and thrown away, four
 * routes, and no keep-alive. The only subtlety is that a preview connection outlives
 * its request by hours, so every connection carries a write buffer and the ones that
 * are not streaming are simply marked to close once that buffer has drained.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vnc_http.h"
#include "vnc_net.h"

#define VNC_HTTP_CONNS  6
#define VNC_HTTP_REQ    4096
#define VNC_HTTP_BOUND  "rblive4"

struct hconn {
    int fd;
    int preview;                 /* this one is a running mjpeg stream */
    int close_after;             /* send what is queued, then hang up  */
    char req[VNC_HTTP_REQ];
    size_t reqlen;
    int headers_done;

    uint8_t *pend;              /* bytes still owed to this peer */
    size_t pendlen, pendoff;
};

struct vnc_http {
    int ls;                      /* the control page's port  */
    struct vnc_http_state *st;
    struct hconn conn[VNC_HTTP_CONNS];
    unsigned long long served, switched;
};

/* --- appending to a response ------------------------------------------------- *
 * A small growable string. The page is built in one of these rather than with a chain
 * of printfs into a fixed buffer, because the one thing that must not happen to a
 * status page is for it to come out truncated halfway through the sentence that
 * explains what went wrong. */
struct sbuf {
    char *p;
    size_t len, cap;
    int over;
};

static void sb_room(struct sbuf *b, size_t n)
{
    size_t cap;
    if (b->over || b->cap - b->len > n)
        return;
    cap = b->cap ? b->cap : 4096;
    while (cap - b->len <= n)
        cap *= 2;
    {
        char *np = realloc(b->p, cap);
        if (!np) { b->over = 1; return; }
        b->p = np;
        b->cap = cap;
    }
}

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
    sb_room(b, n);
    if (b->over) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_adds(struct sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_addf(struct sbuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_addf(struct sbuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { sb_add(b, tmp, (size_t)n); return; }
    {
        char *big = malloc((size_t)n + 1);
        if (!big) { b->over = 1; return; }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sb_add(b, big, (size_t)n);
        free(big);
    }
}

/* Anything that came from outside this program -- an errno message naming a path, say
 * -- goes through here. A status page is a place where a stray angle bracket turns
 * today's diagnostic into tomorrow's broken layout. */
static void sb_addesc(struct sbuf *b, const char *s)
{
    if (!s) { sb_adds(b, "(none)"); return; }
    for (; *s; s++) {
        switch (*s) {
        case '<': sb_adds(b, "&lt;");   break;
        case '>': sb_adds(b, "&gt;");   break;
        case '&': sb_adds(b, "&amp;");  break;
        case '"': sb_adds(b, "&quot;"); break;
        default:  sb_add(b, s, 1);      break;
        }
    }
}

/* --- the page ---------------------------------------------------------------- */

/* THE PICTURE'S BOX IS NOT THE `<img>`. THAT IS THE FIX, AND IT IS LOAD-BEARING.
 *
 * An `<img>` fed by `multipart/x-mixed-replace` is swapped in place, and browsers clear the
 * old frame before the new one is ready to paint. For that instant the element has NO frame,
 * and a `<img>` with no frame is not a *replaced* element any more -- so `width`, `height`
 * and `aspect-ratio` all stop applying to it. Measured in one browser: the box goes
 * 762x477 -> 98x25, with `aspect-ratio` computing to `auto`, i.e. the rule simply stops
 * applying. Twelve times a second the picture is gone and whatever is behind it is showing.
 *
 * The first attempt at a fix was `display:block` plus a definite `aspect-ratio` on the image.
 * That is measurably better -- the sizing properties DO apply to a block box whatever its
 * content -- but it is not sufficient, because it still leaves `height` derived from the
 * image. A browser that drops `aspect-ratio` in that instant falls back to `height:auto`,
 * which with no content is ZERO: the picture collapses vertically and the whole page below it
 * jumps up and back, twelve times a second. That is a strobe of the LAYOUT, and no statistic
 * over the picture's own pixels can see it -- which is why the operator's report outlived a
 * fix that measured clean.
 *
 * So the ratio lives on a plain `<div class="shot">`, and the `<img>` merely fills it. A div's
 * width never depends on any image, so `aspect-ratio` on it always resolves, and the box is
 * fixed before a single byte is decoded. `overflow:hidden` and `background:#000` mean the
 * gap paints black inside a box that has not moved.
 *
 * The `body.only` rules below are the same idea again for the full-screen state: both terms
 * definite, neither read off the frame, so hiding the text around the picture cannot resize
 * it either. */

static const char VNC_HTTP_CSS[] =
    "body{background:#14161a;color:#e8e6e3;font:15px/1.5 -apple-system,"
    "BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;padding:24px;max-width:760px}"
    "h1{font-size:20px;font-weight:600;margin:0 0 4px}"
    "p.lead{color:#9aa0a6;margin:0 0 20px}"
    "table{border-collapse:collapse;margin:0 0 20px;width:100%}"
    "td{padding:5px 0;vertical-align:top}"
    "td.k{color:#9aa0a6;width:11em;padding-right:12px}"
    "code{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:#cfd8dc}"
    "form{margin:0 0 20px}"
    "button{font:inherit;font-weight:600;padding:10px 20px;margin:0 10px 0 0;"
    "border:1px solid #3a3f45;border-radius:6px;background:#1e2126;color:#e8e6e3;"
    "cursor:pointer}"
    "button:hover{background:#282c33}"
    "button.on{background:#2e7d32;border-color:#4caf50;color:#fff}"
    "button.on.jpeg{background:#b26a00;border-color:#ff9800}"
    "img{display:block;width:100%;height:100%;object-fit:contain;border:0}"
    "p.note{color:#9aa0a6;font-size:13px}"
    "b.bad{color:#ef9a9a}b.good{color:#a5d6a7}"
    /* THE PICTURE'S BOX IS A PLAIN DIV, AND THAT IS THE POINT. See the note above this
     * table. Sizing the <img> itself from its own decoded frame -- even with a definite
     * `aspect-ratio` -- leaves one way for the box to move: a browser that drops
     * `aspect-ratio` on a replaced element with no frame falls back to `height:auto`, which
     * with no content is ZERO, and the whole page below the picture jumps up and back.
     * A div's width never depends on the image, so its ratio always resolves and the box
     * is fixed before a single pixel is decoded. */
    ".shot{display:block;width:100%;aspect-ratio:1280/800;background:#000;"
    "border:1px solid #3a3f45;border-radius:6px;overflow:hidden;"
    "touch-action:manipulation;cursor:pointer}"
    /* THE FULL-SCREEN STATE, AND THE ONE THE PAGE OPENS IN: every word hidden, the
     * picture edge to edge on black -- a state of this page rather than a page of its
     * own. A tap toggles it, which is the one piece of script in the program. Both terms
     * are definite and neither is read off the frame, for the reason just given. */
    "body.only{margin:0;padding:0;max-width:none;background:#000;min-height:100vh;"
    "display:flex;align-items:center;justify-content:center;overflow:hidden}"
    "body.only .chrome{display:none}"
    "body.only .shot{flex:none;width:min(1280px,100vw,160vh);"
    "height:min(800px,62.5vw,100vh);aspect-ratio:auto;border:0;border-radius:0;"
    "cursor:default}";

static void page_html(struct vnc_http *h, struct sbuf *b)
{
    struct vnc_http_state *s = h->st;
    int raw = s->mode->mode == VNC_MODE_RAW;
    /* "sharing" is asked twice below, once for the words and once for whether there
     * is a picture to point the preview at, and it is asked through s->live, which
     * may be absent. One local, so neither place can forget the NULL. */
    const int share = s->live && s->live->on;

    sb_adds(b, "<!doctype html><html><head><meta charset=\"utf-8\">"
               "<title>rbp screen</title>"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<style>");
    sb_adds(b, VNC_HTTP_CSS);
    sb_adds(b, "</style></head><body class=\"only\">");

    /* Everything that is a word goes inside a `.chrome` block, and the picture sits
     * between the two of them, so one tap on it can take the whole page down to a bare
     * picture and the next tap can bring the words back. See the script at the foot. */
    sb_adds(b, "<div class=\"chrome\">");

    sb_adds(b, "<h1>rbp screen</h1>");
    sb_addf(b, "<p class=\"lead\">%dx%d &middot; the deck, the drawers and the band, "
               "over VNC.</p>", s->w, s->h);

    /* --- sharing ------------------------------------------------------------ *
     * THE FIRST CONTROL ON THE PAGE, AND THE ONLY ONE ABOUT THIS PROGRAM RATHER
     * THAN ABOUT THE PICTURE. It is here, at the top and as the largest thing on
     * the page, because with sharing off there is no picture anywhere on this page
     * -- so the thing to press has to be impossible to miss. It is also the whole
     * reason the page survives when the screen does not: see vnc_live.h. */
    sb_adds(b, "<h1 style=\"font-size:16px\">Sharing</h1>");
    if (s->live) {
        sb_addf(b, "<p class=\"lead\">%s</p>", share
                 ? "On. The screen is being served &mdash; connect with VNC, or watch "
                   "the preview below."
                 : "Off. Nothing is being served: this program has not opened the "
                   "framebuffer, the display or the VNC port at all, so the player "
                   "cannot tell that it is running.");
        sb_adds(b, "<form method=\"get\" action=\"/session\">");
        sb_addf(b, "<button name=\"set\" value=\"on\"%s>Start sharing</button>",
                share ? " class=\"on\"" : "");
        sb_addf(b, "<button name=\"set\" value=\"off\"%s>Stop sharing</button>",
                share ? "" : " class=\"on\"");
        sb_adds(b, "</form>");
        sb_adds(b, "<table>");
        sb_adds(b, "<tr><td class=\"k\">Switch file</td><td><code>");
        sb_addesc(b, s->live->path);
        sb_addf(b, "</code> &mdash; %s</td></tr>",
                s->live->have_mtime ? "set" : "not written yet, using the default");
        sb_adds(b, "</table>");
        if (!share)
            sb_adds(b, "<p class=\"note\">Starting sharing is the moment the display "
                       "is opened. On a unit whose player comes up blank on a cold "
                       "boot, leaving this off across the reboot is how you find out "
                       "whether this program is the reason for it.</p>");
    } else {
        sb_adds(b, "<p class=\"lead\">Off.</p>");
    }

    sb_adds(b, "<form method=\"get\" action=\"/mode\">");
    sb_addf(b, "<button name=\"set\" value=\"raw\"%s>Raw</button>",
            raw ? " class=\"on\"" : "");
    sb_addf(b, "<button name=\"set\" value=\"hwjpeg\"%s>Hardware JPEG</button>",
            raw ? "" : " class=\"on jpeg\"");
    sb_adds(b, "</form>");

    sb_adds(b, "<table>");

    sb_adds(b, "<tr><td class=\"k\">Sending now</td><td>");
    sb_addf(b, "<code>%s</code>", vnc_mode_name(s->mode->mode));
    if (raw)
        sb_adds(b, " &mdash; full frames, every pixel, no compression. The picture is "
                   "exact and it is expensive: 2 MB a frame.");
    else
        sb_adds(b, " &mdash; the Pi's own encoder, JPEG per frame, a few per cent of "
                   "one core.");
    sb_adds(b, "</td></tr>");

    sb_adds(b, "<tr><td class=\"k\">Mode file</td><td><code>");
    sb_addesc(b, s->mode_path);
    sb_addf(b, "</code> &mdash; %s</td></tr>",
            s->mode->have_mtime ? "set" : "not written yet, using the default");

    sb_adds(b, "<tr><td class=\"k\">Hardware encoder</td><td>");
    if (s->jpeg_ok)
        sb_addf(b, "<b class=\"good\">up</b> &mdash; <code>%s</code>", "");
    else
        sb_adds(b, "<b class=\"bad\">not available</b> &mdash; ");
    sb_addesc(b, s->jpeg_status);
    sb_adds(b, "</td></tr>");

    sb_adds(b, "<tr><td class=\"k\">VNC clients</td><td>");
    sb_addf(b, "%d (%d that JPEG may be sent to)</td></tr>",
            s->vnc_clients, s->vnc_jpeg_clients);
    if (s->vnc_note) {
        sb_adds(b, "<tr><td class=\"k\"></td><td>");
        sb_addesc(b, s->vnc_note);
        sb_adds(b, "</td></tr>");
    }
    sb_adds(b, "</table>");

    /* --- the mouse ---------------------------------------------------------- *
     * THE ONE CONTROL ON THIS PAGE THAT CAN MOVE THE PLAYER. It is here rather than
     * behind a command-line flag because it is the thing the operator reaches for and
     * lets go of while looking at the picture, and because turning it off has to be one
     * press away: a switch that lets a machine press a live player's buttons is not one
     * to bury. */
    sb_adds(b, "<h1 style=\"font-size:16px\">Mouse</h1>");
    if (s->in) {
        int on = s->in->on;
        sb_addf(b, "<p class=\"lead\">%s</p>", on
                 ? "On. A click in the picture is a <b>real press on the panel</b>."
                 : "Off. The picture is a picture; clicks do nothing.");
        sb_adds(b, "<form method=\"get\" action=\"/input\">");
        sb_addf(b, "<button name=\"set\" value=\"on\"%s>On</button>", on ? " class=\"on\"" : "");
        sb_addf(b, "<button name=\"set\" value=\"off\"%s>Off</button>", on ? "" : " class=\"on\"");
        sb_adds(b, "</form>");
        sb_adds(b, "<table>");
        sb_adds(b, "<tr><td class=\"k\">Switch file</td><td><code>");
        sb_addesc(b, s->in->path);
        sb_addf(b, "</code> &mdash; %s</td></tr>",
                s->in->have_mtime ? "set" : "not written yet, using the default");
        sb_adds(b, "</table>");
        if (on)
            sb_adds(b, "<p class=\"note\" style=\"color:#e8b26a\">A click lands where you "
                       "aim it, on the real screen, and the shim's own zones act on it: "
                       "the seventh column of the top menu is <b>USB STOP</b>, which "
                       "raises the eject chooser. Nothing on this page ejects the "
                       "operator's media &mdash; that is a three-second hold, and it is a "
                       "press for a person to make.</p>");
    }

    sb_adds(b, "<h1 style=\"font-size:16px\">Preview</h1>");
    if (!share)
        sb_adds(b, "<p class=\"note\">Nothing to show: the preview is the hardware "
                   "encoder's own output, and the encoder is closed while sharing is "
                   "off. Press <b>Start sharing</b> above &mdash; the page comes back "
                   "with the picture on it.</p>");
    else
        sb_adds(b, "<p class=\"note\">This is the hardware encoder's own output, live, "
                   "whatever the switch above says &mdash; so the JPEG can be judged before "
                   "the VNC picture is handed over to it. It runs only while this page is "
                   "open. <b>Tap the picture to show or hide every word on this page.</b> "
                   "The page opens with them hidden, picture alone on black.</p>");
    sb_adds(b, "</div>");

    if (!share)
        /* NO <img>, AND THAT IS THE POINT: an <img> pointing at the preview would open
         * a preview connection, and a preview connection is a reason to run the capture
         * -- which is exactly what sharing off has just closed. The box is still drawn,
         * at the right shape, so the page does not jump when the picture appears. */
        sb_adds(b, "<div class=\"shot\" id=\"shot\"></div>");
    else
        sb_addf(b, "<div class=\"shot\" id=\"shot\"><img src=\"/preview.mjpg?%llu\" "
                   "alt=\"live preview\"></div>", h->switched);

    /* The listen address is deliberately not printed here: it is 0.0.0.0 by default,
     * which is not a thing to put in a URL, and the address the operator needs is the
     * one already in the address bar of the page they are reading. */
    sb_adds(b, "<div class=\"chrome\">");
    sb_adds(b, "<p class=\"note\" style=\"margin-top:20px\">Connect with <code>vnc://");
    sb_addesc(b, "<the address in your browser's bar>");   /* escaped by sb_addesc */
    sb_addf(b, ":%d</code> in macOS &rarr; Go &rarr; Connect to Server, or paste it "
               "into Safari.</p>", s->vnc_port > 0 ? s->vnc_port : 5900);
    sb_adds(b, "</div>");

    /* The one piece of script in this program, and it is deliberately the smallest kind:
     * a class toggle on this document. Nothing is fetched, nothing is retried, and the
     * stream URL is not touched -- so the rule the rest of this file follows still holds,
     * that a restart leaves a dead <img> until the page is reloaded by hand. It is here
     * because the operator asked for the picture-only view to be a tap on this page rather
     * than a second port, and hiding the words around a picture cannot be done in CSS
     * alone. It sits at the end of the body so `getElementById` cannot miss. */
    sb_adds(b, "<script>document.getElementById('shot').addEventListener('click',"
               "function(){document.body.classList.toggle('only')});</script>");

    sb_adds(b, "</body></html>");
}

/* --- responses ---------------------------------------------------------------- */

static void queue(struct hconn *c, const void *p, size_t n)
{
    if (c->pendoff == c->pendlen) { c->pendoff = c->pendlen = 0; }
    if (c->pendoff > 0) {
        memmove(c->pend, c->pend + c->pendoff, c->pendlen - c->pendoff);
        c->pendlen -= c->pendoff;
        c->pendoff = 0;
    }
    {
        uint8_t *np = realloc(c->pend, c->pendlen + n);
        if (!np) return;
        c->pend = np;
        memcpy(c->pend + c->pendlen, p, n);
        c->pendlen += n;
    }
}

static void reply(struct hconn *c, int code, const char *status, const char *ctype,
                  const void *body, size_t blen, const char *extra)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     code, status, ctype, blen, extra ? extra : "");
    if (n > 0) queue(c, hdr, (size_t)n);
    if (blen) queue(c, body, blen);
    c->close_after = 1;
    c->headers_done = 1;
}

/* --- the preview stream -------------------------------------------------------- */

static void preview_start(struct hconn *c)
{
    char hdr[320];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=" VNC_HTTP_BOUND
                     "\r\n"
                     "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                     "Pragma: no-cache\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    if (n > 0) queue(c, hdr, (size_t)n);
    c->preview = 1;
    c->headers_done = 1;
}

/* --- routing --------------------------------------------------------------------- */

/* One `?set=<word>` out of a request target. A zero return means there was nothing to
 * read; anything else is the word, NUL-terminated in the caller's buffer.
 *
 * IT IS THE SAME PARSE FOR BOTH SWITCHES because it is the same request, and because the
 * word is handed to the module that owns the meaning rather than being interpreted here:
 * what "raw" and "hwjpeg" are, and what "on" and "off" are, is vnc_mode's and vnc_input's
 * business, and both refuse anything they do not recognise. A page that guessed would be
 * a second place for the two spellings to disagree. */
static size_t query_set(const char *target, char *word, size_t cap)
{
    const char *q = strchr(target, '?');
    size_t n;

    if (!q || strncmp(q + 1, "set=", 4) != 0)
        return 0;
    n = strcspn(q + 5, "& ");
    if (n == 0 || n >= cap)
        return 0;
    memcpy(word, q + 5, n);
    word[n] = '\0';
    return n;
}

static void route_get(struct vnc_http *h, struct hconn *c, const char *target)
{
    if (!strncmp(target, "/mode", 5) && (target[5] == '\0' || target[5] == '?')) {
        char word[32];
        if (query_set(target, word, sizeof word)) {
            int want;
            if (vnc_mode_parse(word, &want) == 0) {
                int took = vnc_mode_set(h->st->mode, want);
                h->switched++;
                vlog("http: the page set the mode to %s (file %s)",
                     vnc_mode_name(took), h->st->mode_path);
            } else {
                vlog("http: the page asked for mode \"%s\", which names no mode; "
                     "leaving it at %s", word, vnc_mode_name(h->st->mode->mode));
            }
        }
        /* 303 and not 200: the answer to "change the mode" is not this response, it is
         * the page, so send the browser back to fetch it. */
        reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
              "Location: /\r\n");
        return;
    }

    if (!strncmp(target, "/input", 6) && (target[6] == '\0' || target[6] == '?')) {
        char word[32];
        if (h->st->in && query_set(target, word, sizeof word)) {
            int want;
            if (!strcmp(word, "toggle")) {
                /* The page's own word, and it is the page's because it does not name a
                 * state -- a file cannot hold "the other one". It exists so that a
                 * bookmark or a shortcut can be one press rather than two. */
                want = h->st->in->on ? VNC_INPUT_OFF : VNC_INPUT_ON;
            } else if (vnc_input_parse(word, &want) < 0) {
                want = -1;
            }
            if (want >= 0) {
                int took = vnc_input_switch_set(h->st->in, want);
                vlog("http: the page turned vnc input %s (file %s)",
                     vnc_input_name(took), h->st->in->path);
            } else {
                vlog("http: the page asked for input \"%s\", which is neither on nor "
                     "off; leaving it %s", word, vnc_input_name(h->st->in->on));
            }
        }
        reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
              "Location: /\r\n");
        return;
    }

    if (!strncmp(target, "/session", 8) && (target[8] == '\0' || target[8] == '?')) {
        char word[32];
        if (h->st->live && query_set(target, word, sizeof word)) {
            int want;
            if (!strcmp(word, "toggle")) {
                /* The same word as the input switch, for the same reason: a file
                 * cannot hold "the other one". */
                want = h->st->live->on ? VNC_LIVE_OFF : VNC_LIVE_ON;
            } else if (vnc_live_parse(word, &want) < 0) {
                want = -1;
            }
            if (want >= 0) {
                int took = vnc_live_set(h->st->live, want);
                vlog("http: the page turned sharing %s (file %s)",
                     vnc_live_name(took), h->st->live->path);
            } else {
                vlog("http: the page asked for sharing \"%s\", which is neither on nor "
                     "off; leaving it %s", word, vnc_live_name(h->st->live->on));
            }
        }
        reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
              "Location: /\r\n");
        return;
    }

    if (!strncmp(target, "/preview.mjpg", 13)) {
        preview_start(c);
        return;
    }

    if (target[0] == '/' && (target[1] == '\0' || target[1] == '?')) {
        struct sbuf b;
        memset(&b, 0, sizeof b);
        page_html(h, &b);
        if (b.over) {
            const char *msg = "the page could not be built: out of memory\n";
            reply(c, 500, "Internal Server Error", "text/plain; charset=utf-8",
                  msg, strlen(msg), NULL);
        } else {
            reply(c, 200, "OK", "text/html; charset=utf-8", b.p, b.len, NULL);
        }
        free(b.p);
        return;
    }

    {
        const char *msg = "no such page here. try /\n";
        reply(c, 404, "Not Found", "text/plain; charset=utf-8", msg, strlen(msg), NULL);
    }
}

/* --- connection handling --------------------------------------------------------- */

static void conn_close(struct hconn *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->pend);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void conn_drain(struct hconn *c)
{
    while (c->pendoff < c->pendlen) {
        ssize_t w = send(c->fd, c->pend + c->pendoff, c->pendlen - c->pendoff,
                         MSG_NOSIGNAL);
        if (w > 0) { c->pendoff += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        conn_close(c);
        return;
    }
    c->pendoff = c->pendlen = 0;
    if (c->close_after)
        conn_close(c);
}

static void conn_read(struct vnc_http *h, struct hconn *c)
{
    ssize_t g;

    if (c->preview) {
        /* A preview connection has nothing more to say on the way in; a read that
         * returns zero is the browser closing the tab, which is the only way this
         * server ever learns to stop encoding. */
        char sink[256];
        g = recv(c->fd, sink, sizeof sink, 0);
        if (g == 0 || (g < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            conn_close(c);
        return;
    }

    if (c->reqlen == sizeof c->req)
        return;
    g = recv(c->fd, c->req + c->reqlen, sizeof c->req - c->reqlen, 0);
    if (g == 0) { conn_close(c); return; }
    if (g < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        conn_close(c);
        return;
    }
    c->reqlen += (size_t)g;
    c->req[c->reqlen] = '\0';

    if (!strstr(c->req, "\r\n\r\n") && !strstr(c->req, "\n\n")) {
        if (c->reqlen == sizeof c->req - 1) {
            const char *msg = "request headers too long\n";
            reply(c, 431, "Request Header Fields Too Large",
                  "text/plain; charset=utf-8", msg, strlen(msg), NULL);
        }
        return;
    }

    {
        char method[8];
        char target[256];
        if (sscanf(c->req, "%7s %255s", method, target) != 2) {
            const char *msg = "malformed request\n";
            reply(c, 400, "Bad Request", "text/plain; charset=utf-8", msg, strlen(msg), NULL);
            return;
        }
        if (strcmp(method, "GET") != 0) {
            const char *msg = "this is a read-only page\n";
            reply(c, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                  msg, strlen(msg), NULL);
            return;
        }
        h->served++;
        route_get(h, c, target);
    }
}

/* --- public --------------------------------------------------------------------- */

struct vnc_http *vnc_http_open(const char *bindaddr, int port,
                               struct vnc_http_state *st, char *err, size_t errlen)
{
    struct vnc_http *h = calloc(1, sizeof *h);
    int i;

    if (err && errlen) err[0] = '\0';
    if (!h) { snprintf(err, errlen, "out of memory"); return NULL; }
    h->st = st;
    h->ls = vnc_net_listen(bindaddr, port);
    if (h->ls < 0) {
        snprintf(err, errlen, "cannot listen on %s:%d: %s",
                 bindaddr ? bindaddr : "0.0.0.0", port, strerror(errno));
        free(h);
        return NULL;
    }
    for (i = 0; i < VNC_HTTP_CONNS; i++) h->conn[i].fd = -1;
    return h;
}

void vnc_http_close(struct vnc_http *h)
{
    int i;
    if (!h) return;
    for (i = 0; i < VNC_HTTP_CONNS; i++) conn_close(&h->conn[i]);
    if (h->ls >= 0) close(h->ls);
    free(h);
}

int vnc_http_preview_wanted(const struct vnc_http *h)
{
    int i;
    if (!h) return 0;
    /* SHARING OFF MEANS NOT EVEN THE PREVIEW IS ENCODED. A preview connection is a
     * reason for the capture to run, and while sharing is off there is no capture at
     * all -- honouring one here would open the display and the encoder behind the
     * operator's back, which is precisely the thing the switch exists to stop. The
     * page does not ask for a preview in that state either; this is the backstop for
     * a tab that was already open when sharing was turned off. */
    if (h->st && h->st->live && !h->st->live->on)
        return 0;
    for (i = 0; i < VNC_HTTP_CONNS; i++)
        if (h->conn[i].fd >= 0 && h->conn[i].preview)
            return 1;
    return 0;
}

void vnc_http_preview_frame(struct vnc_http *h, const uint8_t *jpeg, size_t len)
{
    int i;
    if (!h || !jpeg || !len)
        return;
    for (i = 0; i < VNC_HTTP_CONNS; i++) {
        struct hconn *c = &h->conn[i];
        char hdr[160];
        int n;
        if (c->fd < 0 || !c->preview)
            continue;
        /* Behind by a frame? Then drop this one rather than queue it. A preview that
         * is a little behind is a preview; a preview that makes the whole server fall
         * behind is a problem the VNC client shares. */
        if (c->pendoff < c->pendlen)
            continue;
        n = snprintf(hdr, sizeof hdr,
                     "--" VNC_HTTP_BOUND "\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n\r\n", len);
        if (n <= 0) continue;
        queue(c, hdr, (size_t)n);
        queue(c, jpeg, len);
        queue(c, "\r\n", 2);
        conn_drain(c);
    }
}

void vnc_http_poll(struct vnc_http *h, int timeout_ms)
{
    struct pollfd pf[VNC_HTTP_CONNS + 1];
    int idx[VNC_HTTP_CONNS + 1];
    int nf = 0, i, r;

    if (!h)
        return;

    /* One listener, and `idx` carries -1 for it, so the accept arm below can tell a
     * listener from a connection without a second array. */
    pf[nf].fd = h->ls;
    pf[nf].events = POLLIN;
    pf[nf].revents = 0;
    idx[nf] = -1;
    nf++;

    for (i = 0; i < VNC_HTTP_CONNS; i++) {
        struct hconn *c = &h->conn[i];
        if (c->fd < 0) continue;
        pf[nf].fd = c->fd;
        pf[nf].events = (short)(POLLIN | (c->pendoff < c->pendlen ? POLLOUT : 0));
        pf[nf].revents = 0;
        idx[nf] = i;
        nf++;
    }

    r = poll(pf, (nfds_t)nf, timeout_ms);
    if (r < 0 && errno != EINTR)
        return;

    for (i = 0; i < nf; i++) {
        int ci = idx[i];
        if (!pf[i].revents)
            continue;
        if (ci < 0) {
            if (pf[i].revents & POLLIN) {
                struct sockaddr_in peer;
                socklen_t pl = sizeof peer;
                int fd = accept(pf[i].fd, (struct sockaddr *)&peer, &pl);
                int slot;
                if (fd < 0)
                    continue;
                for (slot = 0; slot < VNC_HTTP_CONNS && h->conn[slot].fd >= 0; slot++)
                    ;
                if (slot == VNC_HTTP_CONNS) {
                    const char *msg = "too many connections\n";
                    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
                    close(fd);
                    continue;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                memset(&h->conn[slot], 0, sizeof h->conn[slot]);
                h->conn[slot].fd = fd;
            }
            continue;
        }
        {
            struct hconn *c = &h->conn[ci];
            if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
                conn_read(h, c);
            if (c->fd >= 0 && (pf[i].revents & POLLOUT))
                conn_drain(c);
        }
    }
}
