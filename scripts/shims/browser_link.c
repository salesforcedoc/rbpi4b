/*
 * browser_link.c -- the shim's end of the pipe to the browser. See browser_link.h.
 *
 * Everything here runs on ONE thread (menu_draw.c's tick), so the cached mapping and
 * the last sequence are plain statics with no lock. The touch thread never comes
 * near this file: a tap records a request in menu_window.c's ring and the tick does
 * the writing, which is what keeps the two threads out of each other's way.
 */
#include "browser_link.h"
#include "shimutil.h"           /* shim_now_ms(), for bl_online()'s clock */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>             /* atoi, strtoull */
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>           /* fstat, for the frame file's identity */
#include <unistd.h>

/* A status file older than this is a browser that has stopped. Ten seconds is
 * several frame times at any rate this will run at and far shorter than the time it
 * takes an operator to notice a dead picture. */
#define BL_STATUS_STALE_MS 10000ULL

/* The cached mapping of the frame file. Re-mapped when the file's identity or size
 * changes, which is what a browser restart looks like from here. */
static int      fr_fd = -1;
static void    *fr_map;
static size_t   fr_len;
static dev_t    fr_dev;
static ino_t    fr_ino;
static unsigned fr_seq;         /* the last sequence copied */

/* The monotonic stamp the last status read carried, in milliseconds. 0 until
 * something has been read, which bl_online() reads as "no browser" -- the honest
 * answer when nothing has ever written the file. */
static unsigned long long bl_mono;

void bl_reset(void)
{
    if (fr_map && fr_map != MAP_FAILED)
        munmap(fr_map, fr_len);
    if (fr_fd >= 0)
        close(fr_fd);
    fr_fd = -1;
    fr_map = NULL;
    fr_len = 0;
    fr_dev = 0;
    fr_ino = 0;
    fr_seq = 0;
}

/* Map the frame file, or say why not. Returns the mapping and its length through the
 * pointers; 0 when there is nothing to map. A re-map is attempted whenever the file
 * has been replaced, so a browser restart does not leave the shim holding an
 * unlinked file's pages forever. */
static int bl_frame_map(void **map, size_t *len)
{
    struct stat st;

    if (fr_fd >= 0 && fstat(fr_fd, &st) == 0 && st.st_dev == fr_dev &&
        st.st_ino == fr_ino && (size_t)st.st_size == fr_len) {
        *map = fr_map;
        *len = fr_len;
        return fr_map != NULL;
    }
    if (fr_fd >= 0)
        bl_reset();

    fr_fd = open(BL_FRAME, O_RDONLY);
    if (fr_fd < 0)
        return 0;
    if (fstat(fr_fd, &st) != 0 || st.st_size < (off_t)sizeof(struct bl_frame_hdr)) {
        bl_reset();
        return 0;
    }
    /* A size that is not a whole number of frames is a writer mid-rewrite; the
     * sequence word will fail to settle below and this pass draws nothing. */
    fr_map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fr_fd, 0);
    if (fr_map == MAP_FAILED) {
        fr_map = NULL;
        bl_reset();
        return 0;
    }
    fr_len = (size_t)st.st_size;
    fr_dev = st.st_dev;
    fr_ino = st.st_ino;
    fr_seq = 0;                 /* a new file's sequence is not the old one's */
    *map = fr_map;
    *len = fr_len;
    return 1;
}

int bl_frame_load(void *dst, int pitch_px, int w, int rows, int bpp)
{
    const struct bl_frame_hdr *h;
    const unsigned char *src;
    unsigned char *out = dst;
    void *map;
    size_t len, need;
    unsigned int seq0, seq1;
    int y, stride;

    if (!dst || pitch_px <= 0 || w <= 0 || rows <= 0 || (bpp != 16 && bpp != 32))
        return 0;
    if (!bl_frame_map(&map, &len))
        return 0;

    h = (const struct bl_frame_hdr *)map;
    /* The page may be TALLER than the rows asked for -- that is the keyboard
     * covering its bottom (browser_link.h) -- but never narrower or shorter. */
    if (h->magic != BL_MAGIC || (int)h->w != w || (int)h->h < rows ||
        (int)h->fmt != bpp)
        return 0;
    stride = w * (bpp / 8);
    /* Bounded by the PAGE's geometry, not the rows asked for: the file has to hold
     * the whole page it claims to. */
    need = sizeof(*h) + (size_t)stride * (size_t)h->h;
    if (len < need)
        return 0;

    /* THE TWO READS THAT MAKE A TORN FRAME IMPOSSIBLE TO DRAW: the sequence before
     * the pixels are copied and again after. Equal means no writer was mid-frame
     * across the copy; unequal means this pass draws nothing and the next one
     * (100 ms later at the very worst) gets a whole one. A frame that is one frame
     * late is invisible; a frame with its top from one page and its bottom from
     * another is not. */
    seq0 = h->seq;
    if (seq0 == fr_seq)
        return 0;               /* nothing new since the last copy */
    src = (const unsigned char *)map + sizeof(*h);
    for (y = 0; y < rows; y++)
        memcpy(out + (size_t)y * pitch_px * (bpp / 8),
               src + (size_t)y * stride, (size_t)stride);
    seq1 = h->seq;
    if (seq1 != seq0)
        return 0;

    fr_seq = seq0;
    return 1;
}

int bl_status_read(char *url, int urln, int *focus)
{
    char buf[512];
    FILE *f;
    size_t n;
    int got = 0;

    if (url && urln > 0)
        url[0] = '\0';
    if (focus)
        *focus = 0;
    f = fopen(BL_STATUS, "r");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* One `key value` per line. The parser reads what it knows and ignores the
     * rest, so the browser can add a line without a shim change -- which is the
     * whole reason this is a text file and not a struct. */
    {
        char *p = buf;

        while (*p) {
            char *eol = strchr(p, '\n');
            char *v;

            if (eol)
                *eol = '\0';
            v = strchr(p, ' ');
            if (v) {
                *v = '\0';
                v++;
                if (strcmp(p, "url") == 0 && url && urln > 0) {
                    snprintf(url, (size_t)urln, "%s", v);
                    got = 1;
                } else if (strcmp(p, "focus") == 0 && focus) {
                    *focus = atoi(v);
                    got = 1;
                } else if (strcmp(p, "mono") == 0) {
                    /* Recorded whether or not the caller wants it: bl_online() is
                     * asked separately and has to answer from THIS read, since it
                     * cannot stat the file (see bl_online()). */
                    bl_mono = strtoull(v, NULL, 10);
                    got = 1;
                }
            }
            if (!eol)
                break;
            p = eol + 1;
        }
    }
    return got;
}

int bl_online(void)
{
    unsigned long long now;

    /* FRESHNESS COMES FROM INSIDE THE FILE, NOT FROM ITS mtime, and that is not a
     * style choice: `stat` is NOT IN THE VENDOR'S LIBC. browser_link.h's protocol
     * says the status carries `mono`; this compares it with the shim's own clock.
     *
     * Measured on the unit 2026-10-04, the expensive way: the first build of this
     * module called stat() and time(), which linked cleanly -- a shared object is
     * allowed undefined symbols -- and then killed the player at startup with
     * `/root/pdj/rbp: symbol lookup error: /usr/lib/fbshim.so: undefined symbol:
     * stat`. rbp would not start at all, in a restart loop.
     *
     * Why the build did not catch it: Makefile's GLIBC check reads VERSIONED symbol
     * references (`name@GLIBC_x.y`), and an undefined name with no version at all
     * never appears in that list. A missing symbol is invisible to it; only running
     * it says so.
     *
     * CLOCK_MONOTONIC is the same clock on both sides -- the browser writes it from
     * the host and this reads it from inside the chroot, over one kernel -- which is
     * what makes an integer of milliseconds a complete answer. shimutil.h's
     * shim_now_ms() is the shims' existing reader of it. */
    if (!bl_mono)
        return 0;                       /* nothing read yet: no browser, honestly */
    now = shim_now_ms();
    return now >= bl_mono && now - bl_mono <= BL_STATUS_STALE_MS;
}

int bl_cmd(const char *line)
{
    int fd;
    size_t n;
    ssize_t w;

    if (!line || !*line)
        return 0;
    /* Opened per command rather than held: a command is one tap, so the cost is
     * nothing, and a held fd would survive a browser restart as a write into an
     * unlinked file -- a command the operator gave that silently went nowhere. */
    fd = open(BL_CMD, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return 0;
    n = strlen(line);
    w = write(fd, line, n);
    if (w == (ssize_t)n)
        w = write(fd, "\n", 1);
    close(fd);
    return w == 1;
}
