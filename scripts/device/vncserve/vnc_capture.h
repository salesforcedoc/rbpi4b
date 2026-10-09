/*
 * vnc_capture.h -- one frame of the operator's screen, as RGB565.
 *
 * WHAT "ONE FRAME" MEANS HERE, and why this is not a screenshot. /dev/fb0 holds
 * rbp's deck and nothing else: the two edge drawers, the top band and the USB-STOP
 * chooser are painted onto vc4 overlay planes on /dev/dri/card1, and a plane "is not
 * in /dev/fb0 at all" (docs/06-display.md, the section on the band). A viewer built
 * on fb0 alone shows the operator a screen with no drawers and no band -- so this
 * reads fb0, then composites every active plane on top of it through vnc_compose.
 *
 * READ-ONLY, AND THAT IS LOAD-BEARING. Nothing here takes DRM master, sets a plane,
 * or does a modeset: the planes are asked for by id (GETPLANE), their buffers are
 * mapped through the dumb-buffer API (GETFB, MAP_DUMB) and copied out. That is
 * exactly the route work/boxshot2.py already established on this board, and it is
 * why watching the screen costs rbp nothing.
 *
 * The ioctl numbers and the thirteen frozen structs come from scripts/shims/
 * drmband.h, which took them from libdrm 2.4.114 because Debian's linux-libc-dev
 * ships no drm uapi. They are repeated here rather than included: drmband.h is a
 * shim header and pulls in the shim's syscall wrappers, which a standalone program
 * must not link.
 */
#ifndef RBPI4B_VNC_CAPTURE_H
#define RBPI4B_VNC_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#include "vnc_compose.h"

struct vnc_capture;

/* Open /dev/fb0 and /dev/dri/card1 and describe the screen. On failure returns NULL
 * and writes a reason into `err` -- a viewer that cannot say why it has no picture is
 * worse than one that refuses to start. */
struct vnc_capture *vnc_capture_open(char *err, size_t errlen);
void vnc_capture_close(struct vnc_capture *c);

int vnc_capture_width(const struct vnc_capture *c);
int vnc_capture_height(const struct vnc_capture *c);

/* Fill `dst`, which must hold w*h uint16_t, with the current screen. Returns 0 on
 * success. A failure to read a single plane is not a failure of the frame: that plane
 * is skipped, the rest of the screen is still delivered, and the reason is counted. */
int vnc_capture_frame(struct vnc_capture *c, uint16_t *dst);

/* The planes used by the most recent frame, for the log and the status line. */
int vnc_capture_plane_count(const struct vnc_capture *c);
const struct vnc_plane *vnc_capture_planes(const struct vnc_capture *c);

/* A one-line description: geometry, plane count, and anything that went wrong. */
const char *vnc_capture_status(const struct vnc_capture *c);

/* Include the PRIMARY plane in the composite. It is /dev/fb0, which is already in
 * `dst`, so the picture does not change -- and that is precisely what makes it
 * useful: on a machine with no drawer open, it is the only way to exercise
 * GETFB -> MAP_DUMB -> mmap -> blit against a real 2 MB plane and see it come back
 * byte-identical. A diagnostic, not a mode. */
void vnc_capture_include_primary(struct vnc_capture *c, int on);

/* Print every plane the kernel lists -- active, idle, and of any type -- with the
 * binding, the rectangle and the property values. For --planes, and for docs when
 * the drawers are next on the glass. */
void vnc_capture_describe(struct vnc_capture *c);

#endif /* RBPI4B_VNC_CAPTURE_H */
