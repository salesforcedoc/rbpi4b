/*
 * vnc_mode.c -- see vnc_mode.h.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vnc_mode.h"

/* `struct stat`'s nanosecond timestamp has two names: st_mtim on glibc, where this
 * runs, and st_mtimespec on macOS, where the tests run. Sub-second resolution is worth
 * the two lines, because the failure it prevents -- a change written within the same
 * second as the last read being ignored -- is a mode switch that silently does
 * nothing, which is the least debuggable shape this feature could fail in. */
#if defined(__APPLE__)
#  define VNC_ST_MTIM(st) ((st).st_mtimespec)
#else
#  define VNC_ST_MTIM(st) ((st).st_mtim)
#endif

int vnc_mode_note(struct vnc_mode *m, char *out, size_t outlen)
{
    if (!m->note[0] || outlen == 0)
        return 0;
    snprintf(out, outlen, "%s", m->note);
    m->note[0] = '\0';
    return 1;
}

const char *vnc_mode_name(int mode)
{
    return mode == VNC_MODE_HWJPEG ? "hwjpeg" : "raw";
}

int vnc_mode_parse(const char *text, int *out)
{
    char buf[32];
    size_t n = 0;

    while (*text && n < sizeof buf - 1) {
        char ch = *text++;
        if (ch == '\n' || ch == '\r')
            continue;
        buf[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        (void)0;
    }
    buf[n] = '\0';
    /* Trailing and leading blanks are stripped by hand: strtok would also eat the
     * interior of a word, and `sscanf %31s` on a Mac takes the first token, which is
     * the right answer but hides a file with two words in it. A control file with two
     * words in it is a mistake and should read as one. */
    {
        size_t i = 0, j = n;
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        while (j > i && (buf[j - 1] == ' ' || buf[j - 1] == '\t')) j--;
        if (j - i == 3 && !strncmp(buf + i, "raw", 3)) { *out = VNC_MODE_RAW; return 0; }
        if (j - i == 6 && !strncmp(buf + i, "hwjpeg", 6)) { *out = VNC_MODE_HWJPEG; return 0; }
    }
    return -1;
}

void vnc_mode_init(struct vnc_mode *m, const char *path, int dflt)
{
    memset(m, 0, sizeof *m);
    m->path = path;
    m->mode = dflt;
}

int vnc_mode_get(struct vnc_mode *m)
{
    struct stat st;
    char buf[32];
    int fd;
    ssize_t n;

    if (stat(m->path, &st) != 0) {
        /* Missing is the ordinary state before anyone has chosen. Say so once, so the
         * log explains a mode the operator did not set, and then be quiet about it. */
        if (m->have_mtime) {
            m->have_mtime = 0;
            snprintf(m->note, sizeof m->note,
                     "mode file %s is gone; keeping %s", m->path,
                     vnc_mode_name(m->mode));
        }
        return m->mode;
    }
    if (m->have_mtime &&
        VNC_ST_MTIM(st).tv_sec == m->mtime.tv_sec &&
        VNC_ST_MTIM(st).tv_nsec == m->mtime.tv_nsec)
        return m->mode;

    fd = open(m->path, O_RDONLY);
    if (fd < 0)
        return m->mode;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return m->mode;
    buf[n] = '\0';

    m->mtime = VNC_ST_MTIM(st);
    m->have_mtime = 1;
    m->reads++;

    {
        int want;
        char text[33];
        size_t k = 0;
        while (k < (size_t)n && k < sizeof text - 1 && buf[k] != '\n' && buf[k] != '\r') {
            text[k] = buf[k];
            k++;
        }
        text[k] = '\0';
        if (vnc_mode_parse(buf, &want) < 0) {
            snprintf(m->note, sizeof m->note,
                     "mode file %s says \"%s\", which names no mode; keeping %s",
                     m->path, text, vnc_mode_name(m->mode));
            return m->mode;
        }
        if (want != m->mode) {
            snprintf(m->note, sizeof m->note, "mode changed: %s -> %s",
                     vnc_mode_name(m->mode), vnc_mode_name(want));
            m->mode = want;
        }
    }
    return m->mode;
}

int vnc_mode_set(struct vnc_mode *m, int mode)
{
    char buf[16];
    int fd, len;
    const char *name = vnc_mode_name(mode);

    len = snprintf(buf, sizeof buf, "%s\n", name);
    /* Written with O_TRUNC on the same open rather than "write a temp and rename":
     * a reader here is this same process on its next frame, and the file is a
     * five-byte word, so a half-written read is not a real race -- while a rename
     * needs a temporary file, a directory that may not exist yet, and a cleanup path
     * on every failure. */
    fd = open(m->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(m->note, sizeof m->note, "cannot write %s: %s", m->path,
                 strerror(errno));
        return m->mode;
    }
    if (write(fd, buf, (size_t)len) != len) {
        snprintf(m->note, sizeof m->note, "short write to %s: %s", m->path,
                 strerror(errno));
        close(fd);
        return m->mode;
    }
    close(fd);
    /* Re-stat so the change is picked up by vnc_mode_get on the next frame without
     * waiting for a clock tick, and so a write this process did is not mistaken for
     * the file being new when it is not. */
    {
        struct stat st;
        if (stat(m->path, &st) == 0) {
            m->mtime = VNC_ST_MTIM(st);
            m->have_mtime = 1;
        }
    }
    if (mode != m->mode) {
        snprintf(m->note, sizeof m->note, "mode changed: %s -> %s",
                 vnc_mode_name(m->mode), name);
        m->mode = mode;
    }
    return m->mode;
}
