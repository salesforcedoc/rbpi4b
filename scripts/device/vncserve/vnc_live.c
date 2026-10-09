/*
 * vnc_live.c -- see vnc_live.h.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vnc_live.h"

/* `struct stat`'s nanosecond timestamp has two names: st_mtim on glibc, where this
 * runs, and st_mtimespec on macOS, where the tests run. Sub-second resolution is worth
 * the two lines, because the failure it prevents -- a change written within the same
 * second as the last read being ignored -- is a switch that silently does nothing,
 * which here means a button on the operator's page that appears to be broken. */
#if defined(__APPLE__)
#  define VNC_ST_MTIM(st) ((st).st_mtimespec)
#else
#  define VNC_ST_MTIM(st) ((st).st_mtim)
#endif

const char *vnc_live_name(int on)
{
    /* ONLY THE EXACT VALUE NAMES ITSELF ON. vnc_mode_name has the same shape and the
     * same reason: a value this program did not intend -- a corrupt field, a future
     * switch with a third state -- must not print as the state that opens the
     * operator's display. The safe direction for a name is off. */
    return on == VNC_LIVE_ON ? "on" : "off";
}

int vnc_live_parse(const char *text, int *out)
{
    char buf[32];
    size_t n = 0, i, j;

    while (*text && n < sizeof buf - 1) {
        char ch = *text++;
        if (ch == '\n' || ch == '\r')
            continue;
        buf[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    buf[n] = '\0';

    /* Leading and trailing blanks stripped by hand, for the reason vnc_mode_parse
     * gives: strtok would eat the interior of a word, and a control file with two
     * words in it is a mistake that should read as one. */
    i = 0; j = n;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
    while (j > i && (buf[j - 1] == ' ' || buf[j - 1] == '\t')) j--;

    if (j - i == 2 && !strncmp(buf + i, "on", 2))  { *out = VNC_LIVE_ON;  return 0; }
    if (j - i == 3 && !strncmp(buf + i, "off", 3)) { *out = VNC_LIVE_OFF; return 0; }
    if (j - i == 3 && !strncmp(buf + i, "yes", 3)) { *out = VNC_LIVE_ON;  return 0; }
    if (j - i == 2 && !strncmp(buf + i, "no", 2))  { *out = VNC_LIVE_OFF; return 0; }
    if (j - i == 4 && !strncmp(buf + i, "true", 4))  { *out = VNC_LIVE_ON;  return 0; }
    if (j - i == 5 && !strncmp(buf + i, "false", 5)) { *out = VNC_LIVE_OFF; return 0; }
    if (j - i == 1 && buf[i] == '1') { *out = VNC_LIVE_ON;  return 0; }
    if (j - i == 1 && buf[i] == '0') { *out = VNC_LIVE_OFF; return 0; }
    return -1;
}

int vnc_live_note(struct vnc_live *l, char *out, size_t outlen)
{
    if (!l->note[0] || outlen == 0)
        return 0;
    snprintf(out, outlen, "%s", l->note);
    l->note[0] = '\0';
    return 1;
}

void vnc_live_init(struct vnc_live *l, const char *path, int dflt)
{
    memset(l, 0, sizeof *l);
    l->path = path;
    l->on = dflt ? VNC_LIVE_ON : VNC_LIVE_OFF;
}

int vnc_live_get(struct vnc_live *l)
{
    struct stat st;
    char buf[32];
    int fd;
    ssize_t n;

    if (stat(l->path, &st) != 0) {
        /* Missing is the ordinary state at every boot -- /run is cleared, and the
         * default is OFF -- so say so only when the file has just gone away under a
         * running process, which is the one case where the log has to explain a
         * change nobody asked for. */
        if (l->have_mtime) {
            l->have_mtime = 0;
            snprintf(l->note, sizeof l->note,
                     "switch file %s is gone; sharing is still %s", l->path,
                     vnc_live_name(l->on));
        }
        return l->on;
    }
    if (l->have_mtime &&
        VNC_ST_MTIM(st).tv_sec == l->mtime.tv_sec &&
        VNC_ST_MTIM(st).tv_nsec == l->mtime.tv_nsec)
        return l->on;

    fd = open(l->path, O_RDONLY);
    if (fd < 0)
        return l->on;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return l->on;
    buf[n] = '\0';

    l->mtime = VNC_ST_MTIM(st);
    l->have_mtime = 1;
    l->reads++;

    {
        int want;
        char text[33];
        size_t k = 0;
        while (k < (size_t)n && k < sizeof text - 1 && buf[k] != '\n' && buf[k] != '\r') {
            text[k] = buf[k];
            k++;
        }
        text[k] = '\0';
        if (vnc_live_parse(buf, &want) < 0) {
            snprintf(l->note, sizeof l->note,
                     "switch file %s says \"%s\", which is neither on nor off; "
                     "sharing is still %s", l->path, text, vnc_live_name(l->on));
            return l->on;
        }
        if ((want ? 1 : 0) != (l->on ? 1 : 0)) {
            snprintf(l->note, sizeof l->note, "sharing turned %s", vnc_live_name(want));
            l->on = want;
        }
    }
    return l->on;
}

int vnc_live_set(struct vnc_live *l, int on)
{
    char buf[16];
    int fd, len;
    const char *name = vnc_live_name(on);

    len = snprintf(buf, sizeof buf, "%s\n", name);
    /* Written with O_TRUNC on the same open rather than "write a temp and rename":
     * the reader is this same process on its next turn and the file is a four-byte
     * word, so a half-written read is not a real race -- while a rename needs a
     * temporary file, a directory that may not exist yet, and a cleanup path on every
     * failure. vnc_mode_set makes the same trade for the same reason. */
    fd = open(l->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(l->note, sizeof l->note, "cannot write %s: %s", l->path,
                 strerror(errno));
        return l->on;
    }
    if (write(fd, buf, (size_t)len) != len) {
        snprintf(l->note, sizeof l->note, "short write to %s: %s", l->path,
                 strerror(errno));
        close(fd);
        return l->on;
    }
    close(fd);
    /* Re-stat so the change is not mistaken for the file being new, and so a later
     * vnc_live_get picks up nothing it did not write. */
    {
        struct stat st;
        if (stat(l->path, &st) == 0) {
            l->mtime = VNC_ST_MTIM(st);
            l->have_mtime = 1;
        }
    }
    if ((on ? 1 : 0) != (l->on ? 1 : 0)) {
        snprintf(l->note, sizeof l->note, "sharing turned %s", name);
        l->on = on;
    }
    return l->on;
}
