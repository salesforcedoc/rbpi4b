/*
 * midi_io.c -- the two MIDI directions, kept in one place.
 *
 * In:  the ALSA sequencer, /dev/snd/seq. midi_poll_forever() is the read loop;
 *      it hands each event to a caller-supplied handler so the control-surface
 *      front end can decide what an event means without owning the fd. On every
 *      idle timeout it looks for the surface again, which is what makes a
 *      controller plugged in after the shim work.
 *
 * Out: whichever route to the surface's receive side is available. Two exist,
 *      and which one is in use is always in the log, because with no route the
 *      UI still draws and no control ever reaches it -- which looks exactly like
 *      a broken map:
 *
 *        sequencer  a second local port, subscribed to the surface's receive
 *                   port. Preferred: it needs no device node and it survives a
 *                   replug. This is the FLX4's route.
 *        rawmidi    a device node, opened when the sequencer route cannot be
 *                   established. This is the fallback, and on the JP21/SC Live 4
 *                   it is the route that is actually used, because the match
 *                   name (MIDI_IN_MATCH, default FLX4) does not match it.
 *
 * The out side is small and dumb on purpose: midi_note()/midi_cc() build three
 * bytes and never interpret, and the LED and VU modules decide what to send.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sound/asequencer.h>

#include "syscalls.h"
#include "shimutil.h"
#include "midi_io.h"
#include "ctrl_map.h"    /* ctrl_abs_invalidate() */

/* ---- ALSA sequencer, in ---- */
#define SEQ_DEV       "/dev/snd/seq"

int seq_fd = -1;
int led_fd = -1;

static int seq_client = -1;
static int in_port = -1;         /* our receive port (the surface sends to it) */
static int out_port = -1;        /* our send port (the surface receives from it) */

/* Defaults live here, not only in rb.conf: the shim has to work when it is run
 * by hand with no environment at all. */
static const char *in_match  = "FLX4";
static const char *out_match = "FLX4";

/* The surface ports we are subscribed to, -1 when we are not. Kept so a surface
 * that goes away and comes back (new client number) is re-subscribed, and so the
 * per-second lookup can tell "nothing changed" from "this is a new surface". */
static int sub_in_client = -1, sub_in_port = -1;
static int sub_out_client = -1, sub_out_port = -1;

enum { ROUTE_NONE = 0, ROUTE_SEQ, ROUTE_RAW };
static int out_route = ROUTE_NONE;

/* The rawmidi scan is 8 opens of nodes that are usually absent, so it is
 * throttled rather than retried every second. */
static unsigned long long raw_next_ms;

static int name_has(const char *name, const char *match)
{
     if (!match || !match[0])
          return 0;
     return strcasestr(name, match) != NULL;
}

/* Walk the sequencer's client and port lists looking for the surface.
 *
 * The kernel advances the address itself: a query is "the next client/port after
 * the one in the struct", with -1 starting at 0, and the answer is written back.
 * So the loops below never touch the address between calls.
 *
 * A port can be found for each direction independently -- the kernel creates one
 * port per direction for a rawmidi device, and a bidirectional port answers both
 * tests. Returns 1 when the surface's transmit side was found. */
static int find_surface(int *in_c, int *in_p, int *out_c, int *out_p,
                        char *in_name, char *out_name)
{
     struct snd_seq_client_info cinfo;
     int found_in = 0, nclients = 0;

     *in_c = *in_p = *out_c = *out_p = -1;
     in_name[0] = out_name[0] = '\0';

     memset(&cinfo, 0, sizeof(cinfo));
     cinfo.client = -1;
     while (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &cinfo) >= 0) {
          struct snd_seq_port_info pinfo;
          int nports = 0;

          /* Our own two ports are not the surface. */
          if (cinfo.client == seq_client)
               continue;
          if (++nclients > 64) {
               klog("knobshim2: sequencer client walk hit the cap; "
                    "surface not looked for past client %d\n", cinfo.client);
               break;
          }
          memset(&pinfo, 0, sizeof(pinfo));
          pinfo.addr.client = cinfo.client;
          pinfo.addr.port = -1;
          while (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT,
                            &pinfo) >= 0) {
               int cap = pinfo.capability;
               if (++nports > 128)
                    break;
               if (!found_in &&
                   (cap & (SNDRV_SEQ_PORT_CAP_READ |
                           SNDRV_SEQ_PORT_CAP_SUBS_READ)) ==
                   (SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ) &&
                   name_has(pinfo.name, in_match)) {
                    *in_c = pinfo.addr.client;
                    *in_p = pinfo.addr.port;
                    snprintf(in_name, 64, "%s", pinfo.name);
                    found_in = 1;
               }
               if (*out_c < 0 &&
                   (cap & (SNDRV_SEQ_PORT_CAP_WRITE |
                           SNDRV_SEQ_PORT_CAP_SUBS_WRITE)) ==
                   (SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE) &&
                   name_has(pinfo.name, out_match)) {
                    *out_c = pinfo.addr.client;
                    *out_p = pinfo.addr.port;
                    snprintf(out_name, 64, "%s", pinfo.name);
               }
          }
     }
     return found_in;
}

static int subscribe_in(int c, int p)
{
     struct snd_seq_port_subscribe sub;

     memset(&sub, 0, sizeof(sub));
     sub.sender.client = c;
     sub.sender.port = p;
     sub.dest.client = seq_client;
     sub.dest.port = in_port;
     sub.queue = SNDRV_SEQ_QUEUE_DIRECT;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) {
          klog("knobshim2: subscribe %d:%d -> %d:%d (input) failed: %s\n",
               c, p, seq_client, in_port, strerror(errno));
          return -1;
     }
     return 0;
}

static int subscribe_out(int c, int p)
{
     struct snd_seq_port_subscribe sub;

     memset(&sub, 0, sizeof(sub));
     sub.sender.client = seq_client;
     sub.sender.port = out_port;
     sub.dest.client = c;
     sub.dest.port = p;
     sub.queue = SNDRV_SEQ_QUEUE_DIRECT;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) {
          klog("knobshim2: subscribe %d:%d -> %d:%d (output) failed: %s\n",
               seq_client, out_port, c, p, strerror(errno));
          return -1;
     }
     return 0;
}

static void route_sequencer(const char *name)
{
     if (led_fd >= 0) {
          real_close(led_fd);
          led_fd = -1;
     }
     out_route = ROUTE_SEQ;
     klog("knobshim2: LED/meter output route: sequencer %d:%d -> %d:%d '%s'\n",
          seq_client, out_port, sub_out_client, sub_out_port, name);
}

/* Find a rawmidi node to write to. MIDI_LED_DEV is tried first, then the rest of
 * the range, because the card number depends on what else the kernel has. */
static void open_raw_out(void)
{
     char path[32];

     for (int i = 0; i < 9 && led_fd < 0; i++) {
          if (i == 0)
               snprintf(path, sizeof(path), "%s", MIDI_LED_DEV);
          else
               snprintf(path, sizeof(path), "/dev/snd/midiC%dD0", i - 1);
          if (i > 0 && strcmp(path, MIDI_LED_DEV) == 0)
               continue;              /* MIDI_LED_DEV was already tried */
          led_fd = real_open(path, O_WRONLY | O_NONBLOCK, 0);
          if (led_fd < 0)
               continue;
          out_route = ROUTE_RAW;
          klog("knobshim2: LED/meter output route: rawmidi %s\n", path);
          return;
     }
     klog("knobshim2: LED/meter output route: none (no sequencer surface, "
          "no rawmidi node)\n");
}

/* One attempt at connecting to the surface. Cheap and idempotent: it does
 * nothing when nothing changed, which is what lets the read loop call it once a
 * second forever. */
static void midi_connect_try(void)
{
     int in_c, in_p, out_c, out_p;
     char in_name[64], out_name[64];

     if (!find_surface(&in_c, &in_p, &out_c, &out_p, in_name, out_name)) {
          if (sub_in_client >= 0) {
               klog("knobshim2: control surface '%s' disappeared "
                    "(unplugged?); waiting for it to come back\n", in_match);
               sub_in_client = sub_in_port = -1;
               sub_out_client = sub_out_port = -1;
               if (out_route == ROUTE_SEQ)
                    out_route = ROUTE_NONE;
          }
          return;
     }
     if (in_c != sub_in_client || in_p != sub_in_port) {
          if (subscribe_in(in_c, in_p) < 0)
               return;
          sub_in_client = in_c;
          sub_in_port = in_p;
          klog("knobshim2: control surface: subscribed to %d:%d '%s' "
               "(match '%s')\n", in_c, in_p, in_name, in_match);
     }
     if (out_c >= 0 && (out_c != sub_out_client || out_p != sub_out_port) &&
         subscribe_out(out_c, out_p) == 0) {
          sub_out_client = out_c;
          sub_out_port = out_p;
          route_sequencer(out_name);
     }
}

void seq_setup(void)
{
     struct snd_seq_client_info cinfo;
     struct snd_seq_port_info pinfo;
     int ver;

     seq_fd = real_open(SEQ_DEV, O_RDWR | O_NONBLOCK, 0);
     if (seq_fd < 0) {
          klog("knobshim2: open %s failed: %s\n", SEQ_DEV, strerror(errno));
          return;
     }
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_PVERSION, &ver) < 0) {
          klog("knobshim2: PVERSION failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &seq_client) < 0) {
          klog("knobshim2: CLIENT_ID failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }

     memset(&cinfo, 0, sizeof(cinfo));
     cinfo.client = seq_client;
     cinfo.type = USER_CLIENT;
     snprintf(cinfo.name, sizeof(cinfo.name), "rbp-knob2");
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &cinfo) < 0) {
          klog("knobshim2: SET_CLIENT_INFO failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }

     /* Two ports: one we receive the surface's events on, one we send LED and
      * meter messages from. The capabilities are named from the *peer's* point of
      * view -- a port that can be written to is CAP_WRITE|CAP_SUBS_WRITE -- which
      * is why our input port has the write bits and our output port the read
      * ones. Getting this backwards produces EPERM on subscribe, not a comment. */
     memset(&pinfo, 0, sizeof(pinfo));
     pinfo.addr.client = seq_client;
     snprintf(pinfo.name, sizeof(pinfo.name), "rbp-knob2-in");
     pinfo.capability = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
     pinfo.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
     pinfo.midi_channels = 16;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pinfo) < 0) {
          klog("knobshim2: CREATE_PORT (in) failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     in_port = pinfo.addr.port;

     memset(&pinfo, 0, sizeof(pinfo));
     pinfo.addr.client = seq_client;
     snprintf(pinfo.name, sizeof(pinfo.name), "rbp-knob2-out");
     pinfo.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
     pinfo.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
     pinfo.midi_channels = 16;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pinfo) < 0) {
          klog("knobshim2: CREATE_PORT (out) failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     out_port = pinfo.addr.port;

     in_match = env_text("MIDI_IN_MATCH", "FLX4");
     out_match = env_text("MIDI_OUT_MATCH", "FLX4");
     klog("knobshim2: seq ok client=%d ports=%d/%d, looking for '%s'\n",
          seq_client, in_port, out_port, in_match);

     /* The surface may already be there; if it is not, try the device node so
      * the LEDs and meters work regardless (a JP21 is not named FLX4, and a Pi
      * with no controller has no surface at all). */
     midi_connect_try();
     if (out_route == ROUTE_NONE)
          open_raw_out();
}

/* The sequencer read loop. Does not return: rbp exiting is the only way out, and
 * the caller is a detached thread that has nothing left to do. */
void midi_poll_forever(void (*on_event)(const struct snd_seq_event *ev))
{
     struct snd_seq_event ev;
     /* Rate limit for the "this fd is not usable" line below. Static rather than
      * automatic because it has to survive the round it was set in. */
     static unsigned long long seq_bad_next_ms;

     for (;;) {
          struct pollfd pfd;
          int pr;
          ssize_t n;

          pfd.fd = seq_fd;
          pfd.events = POLLIN;
          pr = poll(&pfd, 1, 1000);
          if (pr <= 0) {
               /* Idle: the moment to look for a surface that was not there
                * before, and to fall back to a node that may have appeared
                * with it. */
               midi_connect_try();
               if (out_route == ROUTE_NONE && shim_now_ms() >= raw_next_ms) {
                    raw_next_ms = shim_now_ms() + 5000;
                    open_raw_out();
               }
               continue;
          }
          n = real_read(seq_fd, &ev, sizeof(ev));
          if (n == (ssize_t)sizeof(ev))
               on_event(&ev);
          else if (n < 0 && errno == EINTR)
               continue;
          else if (n < 0) {
               /* Not EINTR: poll() said there was something to read and the read
                * failed. A sequencer fd the kernel has given up on answers
                * POLLNVAL -- with a POSITIVE count, so the idle branch above is
                * skipped, read() fails immediately, and the loop spins at 100%
                * CPU for as long as rbp runs. This shim never reopens the
                * sequencer, so there is nothing to fix here; what there is to do
                * is stop the spin, keep the surface work below running, and say
                * so once every five seconds rather than once per turn. */
               if (shim_now_ms() >= seq_bad_next_ms) {
                    seq_bad_next_ms = shim_now_ms() + 5000;
                    klog("knobshim2: seq read: %s; this fd is not usable"
                         " (poll said there was data)\n", strerror(errno));
               }
               usleep(100000);
          }
     }
}

/* ---- panel output ---- */

int midi_out_ready(void)
{
     return out_route != ROUTE_NONE;
}

/* Write one three-byte MIDI message, over whichever route is up. */
static int out_write(const unsigned char *m)
{
     if (out_route == ROUTE_RAW)
          return led_fd >= 0 && write(led_fd, m, 3) == 3;

     if (out_route == ROUTE_SEQ) {
          struct snd_seq_event e;

          memset(&e, 0, sizeof(e));
          e.flags = SNDRV_SEQ_EVENT_LENGTH_FIXED;   /* what alsa-lib sends */
          switch (m[0] & 0xf0) {
          case 0x90:
               e.type = SNDRV_SEQ_EVENT_NOTEON;
               e.data.note.channel = m[0] & 0x0f;
               e.data.note.note = m[1];
               e.data.note.velocity = m[2];
               break;
          case 0x80:
               e.type = SNDRV_SEQ_EVENT_NOTEOFF;
               e.data.note.channel = m[0] & 0x0f;
               e.data.note.note = m[1];
               e.data.note.velocity = m[2];
               break;
          case 0xb0:
               e.type = SNDRV_SEQ_EVENT_CONTROLLER;
               e.data.control.channel = m[0] & 0x0f;
               e.data.control.param = m[1];
               e.data.control.value = m[2];
               break;
          default:
               /* SysEx and anything else: see led_query_absolute(). */
               return 0;
          }
          e.queue = SNDRV_SEQ_QUEUE_DIRECT;
          e.source.client = seq_client;
          e.source.port = out_port;
          e.dest.client = sub_out_client;
          e.dest.port = sub_out_port;
          return write(seq_fd, &e, sizeof(e)) == (ssize_t)sizeof(e);
     }
     return 0;
}

/* One LED. Velocity 0 means OFF, and it goes out as a Note On with velocity 0 --
 * never as a Note Off (0x80).
 *
 * MEASURED ON THE FLX4, 2026-10-01, and it is the whole of "the LEDs stay lit
 * when I turn the cue off": the panel's LED hardware IGNORES a real Note Off.
 * Deck 1's channel CUE LED lit, `90 54 00` put it out; with that LED lit again,
 * `80 54 00` left it on. Same channel, same note, the status byte the only
 * difference. This function used to build off as `0x80 | ch`, so every LED the
 * map lit stayed lit -- the wire carried the off and the panel did not act on it.
 *
 * Note On with velocity 0 is also the safer encoding in general, which is why
 * there is no per-surface switch here: the MIDI spec defines Note On velocity 0
 * as a Note Off, so a conforming surface must accept it, while a surface that
 * special-cases its LEDs on Note On (this one) requires it. The previous target
 * drove its LEDs through this same function and read the Note Off; nothing about
 * that surface can be measured from this rig, and the spec-equivalent form is
 * the one that satisfies both. */
int midi_note(int midi_ch, int note, int vel)
{
     unsigned char m[3];

     m[0] = (unsigned char)(0x90 | (midi_ch & 0x0f));
     m[1] = (unsigned char)(note & 0x7f);
     m[2] = (unsigned char)(vel & 0x7f);
     return out_write(m);
}

int midi_cc(int midi_ch, int cc, int val)
{
     unsigned char m[3];

     m[0] = (unsigned char)(0xB0 | (midi_ch & 0x0f));
     m[1] = (unsigned char)(cc & 0x7f);
     m[2] = (unsigned char)(val & 0x7f);
     return out_write(m);
}

/* Ask the panel to report every absolute control (faders/knobs) -- Engine OS
 * does this at startup (`queryAbsoluteControls()` in JP21_Controller_Device.qml).
 * Without it the fader positions are unknown until first moved, which made the
 * channel meters ignore the fader.
 *
 * This is a JP21 protocol message, and the sequencer route carries only the
 * three-byte LED/meter messages, so it goes out on rawmidi or not at all. A
 * surface matched by name (the FLX4) has no absolute controls to report and no
 * meters to light, so "not at all" is the correct outcome there.
 *
 * Returns whether the query went out, which is a different question from
 * whether the panel will answer: rbp_vu.c reads the 0 as "nobody is going to
 * tell rbp where the faders are", which is the case it has to seed them for. */
int led_query_absolute(void)
{
     static const unsigned char sysex[10] = {
          0xF0, 0x00, 0x02, 0x0B, 0x7F, 0x12, 0x04, 0x00, 0x00, 0xF7
     };
     static int told;

     /* Invalidate the CC cache even when the query cannot be sent: the cache is
      * about what the *panel* last reported, and we no longer know. */
     ctrl_abs_invalidate();

     if (out_route != ROUTE_RAW || led_fd < 0) {
          if (out_route == ROUTE_SEQ && !told) {
               told = 1;
               klog("knobshim2: absolute-value query not sent: the rawmidi route "
                    "is what carries it (sequencer route in use)\n");
          }
          return 0;
     }
     (void)write(led_fd, sysex, sizeof(sysex));
     return 1;
}
