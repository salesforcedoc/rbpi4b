/*
 * browser_link.h -- the shim's end of the pipe to the browser. ALL the I/O.
 *
 * menu_window.c is pure by design, so everything that touches a file lives here and
 * nowhere else: this module maps the page frames the browser renders, reads what the
 * page is doing, and appends the commands the operator's taps produce. menu_draw.c's
 * tick is the only caller.
 *
 * WHY /tmp, AND WHY THAT IS NOT A HACK. Measured on the unit 2026-10-04: the host's
 * /tmp and the chroot's /opt/rblive4/rbx3-run/tmp are the SAME MOUNT (dev 35, ino 1
 * -- the shim's own knobshim.log is one file seen from both sides). So a browser
 * running on the host and the shim running inside rbp's process can hand each other
 * files with no new mount, no new privilege and no SD-card wear. It is the idiom
 * pointsrc_log() already uses for the same reason.
 *
 * THE THREE FILES, and each one has one writer:
 *
 *   /tmp/rbwin.frame   browser -> shim   the page, RGB565LE, w*h*2 bytes under a
 *                                        header, rewritten in place with the
 *                                        sequence word bumped last
 *   /tmp/rbwin.status  browser -> shim   one `key value` per line: the current URL,
 *                                        the page's focus generation, and `mono`,
 *                                        the writer's CLOCK_MONOTONIC in ms
 *   /tmp/rbwin.cmd     shim -> browser   one command per line, appended:
 *                                        `nav <url>`, `text <chars>`,
 *                                        `key <Name>`, `click <x> <y>`,
 *                                        `hist <n>` (-1 back, +1 forward),
 *                                        `scroll <dx> <dy>` -- a DRAG on the
 *                                        page, in page pixels, signed, the
 *                                        finger's own direction
 *
 * The frame is a header and a pixel block rather than a bare image because the
 * shim has to be able to tell a half-written frame from a whole one, and the only
 * cheap way to do that across two processes with no lock is a sequence word the
 * writer bumps after the pixels are down and the reader re-reads afterwards.
 */
#ifndef RBPI4B_BROWSER_LINK_H
#define RBPI4B_BROWSER_LINK_H

#include <stddef.h>

#define BL_FRAME  "/tmp/rbwin.frame"
#define BL_STATUS "/tmp/rbwin.status"
#define BL_CMD    "/tmp/rbwin.cmd"

/* 'R','B','W','F' as the little-endian word the writer stores. */
#define BL_MAGIC 0x46574252u

struct bl_frame_hdr {
    unsigned int magic;
    unsigned int seq;       /* bumped by the writer AFTER the pixels */
    unsigned int w, h;      /* the page, in pixels */
    unsigned int fmt;       /* bits per pixel: 16 = RGB565LE, 32 = XRGB8888 */
};

/* True when anything has been written to the status file recently enough to believe
 * -- i.e. a browser is on the other end. The window uses it to say "no browser"
 * rather than showing a blank page that looks like a broken link.
 *
 * IT IS `mono` IN THE FILE THAT DECIDES THIS, NOT THE FILE'S mtime, and it is the
 * file's own stamp compared with the shim's clock_gettime(CLOCK_MONOTONIC) -- one
 * kernel, so the two are the same clock. The reason is in browser_link.c and it cost
 * a crash loop to learn: this is the vendor's libc, and it has no `stat`. */
int bl_online(void);

/* Copy the TOP-LEFT w x rows of the page into `dst` when the frame file holds one
 * the writer has finished. Returns 1 when a NEW frame was copied -- the caller then
 * repaints -- and 0 when nothing changed, the file is absent, or the frame does not
 * match the geometry asked for. `pitch_px` is the destination's stride in PIXELS, so
 * a padded framebuffer is handled here rather than by the caller.
 *
 * THE PAGE IS TALLER THAN `rows` WHEN THE KEYBOARD IS UP, and that is the point of
 * the parameter: the popup covers the bottom of the page, so the bottom of the page
 * is not copied, and the keyboard is not clobbered by the 30 frames a second that
 * would otherwise land on top of it. The frame's own header carries the whole page;
 * `rows` is how much of it this window has room to show. */
int bl_frame_load(void *dst, int pitch_px, int w, int rows, int bpp);

/* The status line, parsed: the URL into `url` (NUL-terminated, `urln` bytes) and the
 * focus generation into `*focus`. Either out pointer may be NULL. Returns 1 when the
 * file was read. */
int bl_status_read(char *url, int urln, int *focus);

/* Append one command line for the browser. Returns 1 when it was written. */
int bl_cmd(const char *line);

/* Forget the cached mapping and the last sequence -- called when the window closes,
 * so a stale frame is not mistaken for a fresh one the next time it opens. */
void bl_reset(void);

#endif /* RBPI4B_BROWSER_LINK_H */
