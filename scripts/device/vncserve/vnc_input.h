/*
 * vnc_input.h -- the mouse in the picture, and the finger on the glass.
 *
 * A CLICK OVER VNC IS A REAL PRESS ON THE REAL PANEL. That is the whole design, and
 * it is why this file is short: it does not invent an input path, it writes into the
 * panel's own evdev node the same report sequence work/poke.py has written into it
 * since September, which is measured working on this unit, on this glass, with the
 * operator's own media. pointsrc is reading that same node, so a report from here
 * walks rbp's whole pointer chain exactly as a finger does -- the shim's zones, the
 * drawers, the USB STOP chooser, and rbp's own widgets. This file knows none of that
 * and should know none of it.
 *
 * TWO THINGS MAKE IT NOT A COPY OF poke.py, and both are load-bearing.
 *
 * 1. THE MAPPING IS DIRECT, AND poke.py's IS NOT. poke.py writes the raw position a
 *    *hand* would have to touch and rbp then acts on the pixel that hand is over. The
 *    reflection in the middle -- tscfake_wire_x() -- belongs to the pipe between
 *    pointsrc and rbp, not to the panel: it is applied on the way out of pointsrc and
 *    undone on the way into rbp, so a report arriving at pointsrc from this node is
 *    in the panel's own space with nothing to reflect. A VNC PointerEvent is already
 *    in framebuffer coordinates, which are rbp's 1280x800 screen. So a click at
 *    picture x needs only the panel's raw range put back on it. If you find yourself
 *    reaching for 1279 - x here, the reflection has been double-counted: see
 *    vnc_input_map() and docs/19-vnc.md.
 *
 * 2. IT HOLDS A PRESS OPEN FOR A MOMENT. rbp wants a few frames of "down" before it
 *    will call a press a tap, and a Mac's mouse click can be shorter than that;
 *    work/poke.py's proven tap dwells 150 ms for exactly this reason. So a release
 *    that arrives sooner than VNC_INPUT_MIN_PRESS_MS is held back until the press has
 *    lasted that long -- never extended past it, so a press the operator really did
 *    hold for longer is still the long press rbp sees. A second click landing inside
 *    that hold is queued behind it rather than merged into it, so two clicks stay two
 *    taps and never become one drag.
 *
 * THE ONE THING THAT MUST NOT HAPPEN: the node is opened WRITE-ONLY and is never
 * read. evdev delivers every event to every client that has the node open, so a
 * reader here would not steal from pointsrc -- and poke.py's own drains show it is
 * safe to look -- but this is a service that runs for weeks beside a live player, and
 * the only way to be certain it can never consume a report meant for the glass is not
 * to have the capability at all.
 */
#ifndef RBPI4B_VNC_INPUT_H
#define RBPI4B_VNC_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* --- the switch ------------------------------------------------------------- *
 * The same idiom as vnc_mode, deliberately: a small file under /run/rblive4 re-read
 * by mtime, so the operator's page can turn it on and off, so a restart keeps
 * whatever it was, and so the setting can be changed from a shell when the page
 * cannot be reached. It is OFF until something says otherwise -- this is the control
 * that lets a program on the network press the buttons of a live player, and it is
 * not a thing to have on by accident.
 *
 * THE FILE IS THE SWITCH, NOT THE CONFIGURATION. RB_VNC_INPUT seeds it when the file
 * does not exist yet; once the operator has pressed the button on the page, the page
 * wins until the next restart, which is what somebody pressing a button expects. */
#define VNC_INPUT_OFF   0
#define VNC_INPUT_ON    1

#define VNC_INPUT_PATH  "/run/rblive4/vnc.input"

/* The size of a note. It is one log line, and it has to hold a path, a device name and
 * a sentence about what happened to them -- 128 was enough for every message in this
 * file but one, and that one is the greeting that names all three at once. gcc's
 * -Wformat-truncation says what the worst case of a format is, and at 128 it said so;
 * a log line that truncates is a worse failure than a log line that is long, so the
 * buffer is the size of the longest line rather than the size of the shortest. */
#define VNC_NOTE_MAX 256

struct vnc_input_switch {
    const char *path;
    int on;
    struct timespec mtime;   /* of the last successful read */
    int have_mtime;
    long reads;

    /* The module does not log -- a module that reaches for the logger cannot be
     * linked into a host test on its own -- so a sentence waits here for the caller
     * to write out. See vnc_mode.h for the copy trap this avoids. */
    char note[VNC_NOTE_MAX];
};

int vnc_input_switch_note(struct vnc_input_switch *s, char *out, size_t outlen);
void vnc_input_switch_init(struct vnc_input_switch *s, const char *path, int dflt);
int vnc_input_switch_get(struct vnc_input_switch *s);
int vnc_input_switch_set(struct vnc_input_switch *s, int on);
const char *vnc_input_name(int on);

/* "on"/"off"/"1"/"0"/"yes"/"no"/"true"/"false", case-insensitively, ignoring
 * surrounding blanks and a trailing newline. Returns 0, or -1 if it names neither.
 * The word "toggle" is accepted by the page and not here: a file cannot hold it. */
int vnc_input_parse(const char *text, int *out);

/* --- the mapping ------------------------------------------------------------ *
 * Pure, and pinned by a test against the raw coordinates in docs/07-touch.md.
 *
 * `lspan` and `rspan` are the number of distinct values in each space -- 1280 and
 * 1921, not 1279 and 1920 -- and the result is a CEILING, not the floor the eye
 * expects. Both halves were measured: with the floor, a fader asked for value 0
 * landed on 2 and logical x 1279 came back as 1278. */
int vnc_input_raw_of_logical(int l, int lspan, int rspan);
void vnc_input_map(int lx, int ly, int lspan_w, int lspan_h,
                   int raw_w, int raw_h, int *rx, int *ry);

/* --- the reports ------------------------------------------------------------ *
 * The exact bytes, built into a caller's buffer. These are separated out because
 * they are the half of this file that a host test can check, and because the sequence
 * is the only thing here that has to be right *as a sequence*: work/poke.py's prime,
 * press and release, in that order, byte for byte.
 *
 * `evsize` is 16 or 24 and IS NOT ASSUMED -- see vnc_input_open(). The two layouts
 * differ only in the two timestamp fields in front; the type, code and value are
 * little-endian u16/u16/s32 at the tail either way, which is what a 32-bit reader gets
 * from a 64-bit kernel's compat path and what a 64-bit reader gets from the kernel's
 * own struct. */
#define VNC_INPUT_REPORT_MAX 256
size_t vnc_input_build_prime(uint8_t *buf, size_t cap, int evsize, int rx, int ry);
size_t vnc_input_build_press(uint8_t *buf, size_t cap, int evsize, int rx, int ry);
size_t vnc_input_build_move(uint8_t *buf, size_t cap, int evsize, int rx, int ry);
size_t vnc_input_build_release(uint8_t *buf, size_t cap, int evsize);
size_t vnc_input_build_syn(uint8_t *buf, size_t cap, int evsize);

/* --- the shell -------------------------------------------------------------- */

/* A press the client let go of sooner than this is held open to exactly this long.
 * 150 ms is work/poke.py's HOLD_MS, the dwell its taps have used on this glass for as
 * long as it has existed, and it is well under MZ_HOLD_FINGER_MS (350), so it can
 * never turn a tap into a hold. */
#define VNC_INPUT_MIN_PRESS_MS 150

/* The panel's own range on this unit, and pointsrc's own numbers (POINT_LOGICAL_W/H
 * and work/poke.py's RAW_X/RAW_Y). Used until EVIOCGABS says otherwise. */
#define VNC_INPUT_RAW_W_DEFAULT 1921
#define VNC_INPUT_RAW_H_DEFAULT 1081
#define VNC_INPUT_SCREEN_W      1280
#define VNC_INPUT_SCREEN_H      800

/* What the panel must call itself to be found by the scan. The same test pointsrc
 * applies to its absolute devices: /dev/input/event0, "TSTP CTouch", on this unit. */
#define VNC_INPUT_NAME_MATCH    "CTouch"
#define VNC_INPUT_SCAN_MAX      32

struct vnc_input {
    int fd;                  /* -1 when there is no panel */
    int evsize;              /* 16 or 24, whichever write() accepted */
    int raw_w, raw_h;        /* the panel's own range, from EVIOCGABS where possible */
    int scr_w, scr_h;        /* the space the client's coordinates are in */

    int down;                /* a press has gone out and has not been released */
    int pending;             /* the client let go; the release is held until due */
    int queued;              /* a press arrived inside that hold; it waits its turn */
    int qx, qy;
    unsigned long long press_ms;
    int have_last;
    int last_x, last_y;

    unsigned long long presses, moves, releases, refused;
    char note[VNC_NOTE_MAX];
};

int vnc_input_note(struct vnc_input *in, char *out, size_t outlen);

/* No fd, no device: the geometry only. Callers that go on to open a panel pass the
 * numbers the capture reported. */
void vnc_input_init(struct vnc_input *in, int scr_w, int scr_h, int raw_w, int raw_h);

/* Open the panel, work out the record size, and learn the raw range. `dev` NULL means
 * scan /dev/input/event0..31 for VNC_INPUT_NAME_MATCH. Returns 0, or -1 with `err`
 * filled -- the caller decides whether that is fatal, and it never is: a server with
 * no input still serves a picture. */
int vnc_input_open(struct vnc_input *in, const char *dev, char *err, size_t errlen);

/* For tests: hand it an fd that is not a panel. It will be written to and never read. */
void vnc_input_set_fd(struct vnc_input *in, int fd, int evsize);

void vnc_input_close(struct vnc_input *in);

/* One RFB PointerEvent. `buttons` is RFB's button mask, of which only bit 0 -- the
 * left button -- means anything here: the panel has one contact and no buttons, and
 * the other seven bits are logged and dropped. `lx`/`ly` are in the client's
 * framebuffer coordinates, which are rbp's own screen. `now` is a millisecond clock
 * the caller already has, so this file needs no clock of its own. */
void vnc_input_pointer(struct vnc_input *in, int buttons, int lx, int ly,
                       unsigned long long now);

/* Send a held-back release whose dwell has now elapsed. Cheap, and called every turn
 * of the session's loop. */
void vnc_input_tick(struct vnc_input *in, unsigned long long now);

/* Let go, now, whatever the dwell says. For a client that has gone, a switch turned
 * off, and shutdown -- the three ways a press could otherwise outlive its author.
 * A finger left down on the operator's panel is not an acceptable failure mode. */
void vnc_input_release(struct vnc_input *in);

int vnc_input_is_down(const struct vnc_input *in);

#endif /* RBPI4B_VNC_INPUT_H */
