/*
 * vnc_mode.h -- which way the pixels leave the Pi, and where that choice lives.
 *
 * The operator asked for "the option to switch from raw to hwjpeg". The option has to
 * survive the person choosing it: vncserve is a service that may be restarted, and a
 * setting that lives only in the process's memory is a setting the operator has to
 * make again every time. So the choice is a file on disk, re-read as it changes.
 *
 * /run/rblive4/vnc.mode is the project's own idiom for exactly this -- /tmp/udev_usb1
 * is the same shape -- and /run is the right directory for it because the setting
 * describes the running system and not the installed one. A reboot may clear it; that
 * is what the default in rb.conf is for.
 */
#ifndef RBPI4B_VNC_MODE_H
#define RBPI4B_VNC_MODE_H

#include <stddef.h>
#include <time.h>

#define VNC_MODE_RAW     0
#define VNC_MODE_HWJPEG  1

#define VNC_MODE_PATH    "/run/rblive4/vnc.mode"

struct vnc_mode {
    const char *path;
    int mode;
    struct timespec mtime;   /* of the last successful read */
    int have_mtime;
    long reads;              /* for the status line: how often the file was re-read */

    /* WHAT HAPPENED, FOR SOMEBODY ELSE TO SAY. This module does not log, because the
     * logger lives in vnc_net and a module that reaches for it cannot be linked into
     * a host test on its own -- which is exactly the property that makes the parsing
     * above worth pinning. So a read that changed something, or that found a file it
     * could not use, leaves a sentence here and the caller writes it out. Empty means
     * there is nothing worth saying. */
    char note[128];
};

/* Hand the pending note to the caller and clear it. Returns 1 and copies the sentence
 * into `out` if there was one, 0 if there was nothing to say.
 *
 * IT COPIES RATHER THAN RETURNING A POINTER INTO THE STRUCT, which is not a style
 * choice: the obvious `return m->note` after clearing it hands back a pointer to a
 * buffer whose first byte has just been set to NUL, so every caller logged an empty
 * line and every diagnosis this module could offer was silently thrown away. */
int vnc_mode_note(struct vnc_mode *m, char *out, size_t outlen);

/* Point the tracker at a file and give it a starting mode. Nothing is read or written
 * here -- the caller may be starting before /run exists. */
void vnc_mode_init(struct vnc_mode *m, const char *path, int dflt);

/* The current mode, re-reading the file if its mtime has moved. A missing or
 * unreadable file is not an error and does not change the mode: vncserve must keep
 * serving the screen the operator can already see if somebody deletes the control
 * file. */
int vnc_mode_get(struct vnc_mode *m);

/* Write the file and take the new mode. Returns the mode actually in force, which is
 * the old one if the write failed. */
int vnc_mode_set(struct vnc_mode *m, int mode);

const char *vnc_mode_name(int mode);

/* "raw"/"hwjpeg" -> the constant, case-insensitively and ignoring surrounding
 * whitespace and a trailing newline (the file is written by an editor as often as by
 * this program). Returns 0, or -1 if the text names no mode we have. */
int vnc_mode_parse(const char *text, int *out);

#endif /* RBPI4B_VNC_MODE_H */
