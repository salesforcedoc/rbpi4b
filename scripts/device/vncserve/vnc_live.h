/*
 * vnc_live.h -- whether the screen is being served at all.
 *
 * THE OPERATOR ASKED FOR THIS AFTER THE BLANK-BOOT DEFECT, and the shape of the ask
 * is the shape of the reason: "pause the VNC service for now, have it something you
 * can enable on the :5902 page instead". vncserve starts at 6.9 s on this unit --
 * before rblive4 (10.3 s) and before rbp itself (15.0 s) -- and it was the only thing
 * in the boot that touched the display from outside the player, so it is the first
 * thing to take out of the boot path when a boot comes up blank. But it is also the
 * thing that serves the control page, so a switch that *stops the program* would take
 * the page away with it and leave nothing to press.
 *
 * So the process always runs and always serves the page; this switch decides whether
 * it also serves the screen. OFF means the program does not open /dev/fb0 or
 * /dev/dri/card1, does not listen on the RFB port, and never touches the hardware
 * JPEG encoder -- there is nothing left of it that a display can notice. Turning it
 * on opens all three; turning it off closes them again.
 *
 * It is a file and not a command-line flag for the reason vnc_mode.h gives: the
 * person who turns it on is looking at a web page in a browser, and the choice has to
 * survive the page being reloaded, the browser being closed, and this process being
 * restarted. /run/rblive4/vnc.live is the project's own idiom for that -- /tmp/udev_usb1
 * and /run/rblive4/vnc.mode are the same shape -- and /run is right because the
 * setting describes the running system, not the installed one. A reboot going back to
 * the rb.conf default (OFF) is deliberate: an unattended boot must not start serving
 * the operator's screen because somebody left a switch on last week.
 */
#ifndef RBPI4B_VNC_LIVE_H
#define RBPI4B_VNC_LIVE_H

#include <stddef.h>
#include <time.h>

#define VNC_LIVE_OFF 0
#define VNC_LIVE_ON  1

#define VNC_LIVE_PATH "/run/rblive4/vnc.live"

struct vnc_live {
    const char *path;
    int on;
    struct timespec mtime;   /* of the last successful read */
    int have_mtime;
    long reads;              /* for the status line: how often the file was re-read */

    /* WHAT HAPPENED, FOR SOMEBODY ELSE TO SAY -- the same contract as vnc_mode's
     * note, and for the same reason: this module does not log, because the logger
     * lives in vnc_net and a module that reaches for it cannot be linked into a host
     * test on its own. Empty means there is nothing worth saying. */
    char note[128];
};

/* "on"/"off", and the words people actually type for them -- yes/no, true/false,
 * 1/0 -- case-insensitively and ignoring surrounding blanks and a trailing newline
 * (the file is written by an editor as often as by this program). Returns 0, or -1
 * if the text names neither. */
int vnc_live_parse(const char *text, int *out);

const char *vnc_live_name(int on);

/* Hand the pending note to the caller and clear it. Returns 1 if there was one.
 * It COPIES rather than returning a pointer into the struct -- vnc_mode.h explains
 * why that is not a style choice. */
int vnc_live_note(struct vnc_live *l, char *out, size_t outlen);

/* Point the tracker at a file and give it a starting state. Nothing is read or
 * written here -- the caller may be starting before /run exists. */
void vnc_live_init(struct vnc_live *l, const char *path, int dflt);

/* The current state, re-reading the file if its mtime has moved. A missing or
 * unreadable file is not an error and does not change the state. */
int vnc_live_get(struct vnc_live *l);

/* Write the file and take the new state. Returns the state actually in force, which
 * is the old one if the write failed. */
int vnc_live_set(struct vnc_live *l, int on);

#endif /* RBPI4B_VNC_LIVE_H */
