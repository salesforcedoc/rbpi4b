/*
 * drmband.c -- the menu band on a vc4 overlay plane. See drmband.h for why.
 *
 * THE ONE PREDICTION, so that a failure here is a result rather than a mystery:
 * a plane is not in rbp's atomic state, and rbp does not use atomic commits at
 * all -- it draws through /dev/fb0, i.e. CPU writes into the primary plane's dumb
 * buffer. A plane scanning out of its own buffer therefore cannot be touched by
 * anything rbp does short of a full modeset, and if the band flickers anyway then
 * one of those beliefs is wrong.
 *
 * IT IS A FALLBACK DESIGN, NOT A REPLACEMENT. Every path out of drm_band_setup()
 * that is not "the plane is up and mapped" leaves the caller with the fb0 route
 * it shipped with, untouched. menu_draw.c checks one return value and either way
 * the panel appears; the only difference is whether it is erased 57 times a
 * second.
 *
 * SAFETY. Master is taken once per DEVICE, on the first setup, so a machine where it
 * cannot be taken says so before a pixel is drawn rather than on the first swipe.
 * Nothing here is irreversible: the kernel tears all of it down when the fd closes,
 * so process exit -- clean or not -- always clears the plane, and there is no state
 * on disk.
 *
 * SEVERAL BANDS AT ONCE, which is what the drawer work needed and what the holder
 * below provides: the fd, the capability and master are refcounted, each band owns
 * its own buffer/framebuffer/plane, and a second setup is a buffer and a plane rather
 * than a second open. See the device block for why that was impossible before.
 */
#define _GNU_SOURCE
#include "drmband.h"
#include "syscalls.h"          /* real_open/ioctl/mmap/close: the shim interposes the real ones */
#include "pointsrc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The card rbp's picture is actually on. NOT card0: recon measured card0 = v3d,
 * a render node with no CRTC at all, and card1 = vc4-drm, whose primary plane is
 * the framebuffer /dev/fb0 mmaps. Hardcoded rather than a knob because it is a
 * fact about the board; the A/B that matters is MENU_PLANE, which is the band's
 * route and lives in menu_draw.c. */
#define DRM_BAND_CARD  "/dev/dri/card1"

/* vc4's planes report 39 formats -- measured, and the reason a fixed 32-format
 * buffer cost a whole run in work/drmplane.c: GETPLANE returns EINVAL when
 * count_format_types is below the real format_count, and the `continue` on that
 * error turned it into a short plane list AND an empty format list, which reads
 * exactly like "this hardware has no RGB565 overlay". */
#define DRM_BAND_MAXFORMATS 64

/* ------------------------------------------------------------- the device
 *
 * ONE DEVICE, ONE MASTER, MANY PLANES -- and this holder is the whole of it.
 *
 * DRM master is per OPEN FILE, not per process, so the second drm_band_setup() in
 * a process used to be refused EBUSY: it opened the card again, got a second
 * file description, and could not take master from the first. That is why the
 * band, the browser window and the drawer were written as MUTUALLY EXCLUSIVE
 * HOLDERS OF ONE PLANE, handing a single slot back and forth (menu_draw.c).
 *
 * The operator then asked for both edge drawers at once -- "allow for both side
 * panels to be visable at the same time and to accept input" -- and one plane
 * cannot do it: an RGB565 plane is OPAQUE (drmband.h), so two drawers composited
 * into a single full-width buffer would each carry the other's black everywhere
 * it is not drawn, over rbp's whole picture. Two planes is the honest answer.
 *
 * So the device is held once and shared. The FIRST setup opens the card, sets
 * UNIVERSAL_PLANES and takes master; every later setup reuses that fd and is
 * granted master with it. The card is closed and master dropped only when the
 * LAST band goes, which is what keeps the "a machine that cannot have a plane
 * always has the /dev/fb0 route" promise intact at both the first and the last.
 *
 * `refs` counts BANDS, not callers: dev_acquire() bumps it and dev_release()
 * drops it, so a teardown of one band cannot close the fd another is drawing
 * through. Nothing here is irreversible -- the kernel tears the planes down when
 * the fd finally closes, clean exit or not.
 */
static struct {
    int fd;
    int refs;
} g_dev = { -1, 0 };

static int dev_acquire(void)
{
    struct drm_set_client_cap cap;

    if (g_dev.refs > 0) {           /* the card is already ours: master included */
        g_dev.refs++;
        return g_dev.fd;
    }

    g_dev.fd = real_open(DRM_BAND_CARD, O_RDWR | O_CLOEXEC, 0);
    if (g_dev.fd < 0) {
        pointsrc_log("menu plane: open %s: %s -- the band stays on /dev/fb0",
                     DRM_BAND_CARD, strerror(errno));
        return -1;
    }

    /* Without this a client is shown only the OVERLAY planes: this kernel
     * returned 48 of card1's 60 objects, with every primary and every cursor
     * simply absent from the list. It is what makes the plane the primary is on
     * invisible to the search below -- and it is a capability of the FILE, so it
     * is set here, once, rather than per band. */
    memset(&cap, 0, sizeof cap);
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value      = 1;
    if (real_ioctl(g_dev.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        pointsrc_log("menu plane: SET_CLIENT_CAP UNIVERSAL_PLANES: %s -- the band"
                     " stays on /dev/fb0", strerror(errno));
        real_close(g_dev.fd);
        g_dev.fd = -1;
        return -1;
    }

    /* Master, ONCE for the device. Nothing is composited until drm_band_show(),
     * so a failure here costs nothing but this line.
     *
     * Measured before the band was wired in: taking master did NOT disturb the mode
     * DirectFB set up -- work/drmplane held it for minutes with rbp redrawing
     * underneath and the primary stayed at fb 722 throughout. */
    if (real_ioctl(g_dev.fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
        pointsrc_log("menu plane: SET_MASTER on %s: %s -- something else holds"
                     " the display; the band stays on /dev/fb0",
                     DRM_BAND_CARD, strerror(errno));
        real_close(g_dev.fd);
        g_dev.fd = -1;
        return -1;
    }

    g_dev.refs = 1;
    return g_dev.fd;
}

static void dev_release(void)
{
    if (g_dev.refs <= 0)
        return;
    if (--g_dev.refs > 0)
        return;                     /* another band is still drawing through it */
    real_ioctl(g_dev.fd, DRM_IOCTL_DROP_MASTER, NULL);
    real_close(g_dev.fd);
    g_dev.fd = -1;
}

/* --------------------------------------------------------------- helpers */

/* The `type` property of a plane object, or 0xffffffffu if it cannot be read.
 * This is the only way to tell an overlay from a primary from a cursor: the plane
 * ids are not grouped, and the two we must not steal are the primary the live
 * CRTC is scanning out and the cursor the pointer path may want. */
static unsigned int plane_type_of(int fd, unsigned int plane_id)
{
    struct drm_mode_obj_get_properties q;
    unsigned int props[DRM_BAND_MAXFORMATS], vals[DRM_BAND_MAXFORMATS];
    unsigned int i;

    memset(props, 0, sizeof props);
    memset(vals, 0, sizeof vals);
    memset(&q, 0, sizeof q);
    q.props_ptr       = (__u64)(uintptr_t)props;
    q.prop_values_ptr = (__u64)(uintptr_t)vals;
    q.count_props     = DRM_BAND_MAXFORMATS;
    q.obj_id          = plane_id;
    q.obj_type        = DRM_MODE_OBJECT_PLANE;
    if (real_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &q) < 0)
        return 0xffffffffu;

    for (i = 0; i < q.count_props && i < DRM_BAND_MAXFORMATS; i++) {
        struct drm_mode_get_property gp;

        memset(&gp, 0, sizeof gp);
        gp.prop_id = props[i];
        if (real_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        if (strcmp(gp.name, "type") == 0)
            return vals[i];
    }
    return 0xffffffffu;
}

/* The CRTC that is actually scanning something out, and its INDEX -- the index,
 * not the id, because possible_crtcs is a bitmask over the order the ids come
 * back in and not over the ids themselves. On this unit that is crtc[3] /
 * pixelvalve-2 at fb 722, which is rbp's picture. */
static int live_crtc(int fd, unsigned int *id, unsigned int *index)
{
    struct drm_mode_card_res res;
    unsigned int *ids, i, n;

    memset(&res, 0, sizeof res);
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;
    n = res.count_crtcs;
    if (n == 0)
        return -1;

    ids = calloc(n, sizeof *ids);
    if (!ids)
        return -1;
    res.crtc_id_ptr      = (__u64)(uintptr_t)ids;
    res.fb_id_ptr        = 0;
    res.connector_id_ptr = 0;
    res.encoder_id_ptr   = 0;
    res.count_fbs        = 0;
    res.count_connectors = 0;
    res.count_encoders   = 0;
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        free(ids);
        return -1;
    }

    for (i = 0; i < n; i++) {
        struct drm_mode_crtc c;

        memset(&c, 0, sizeof c);
        c.crtc_id = ids[i];
        if (real_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0)
            continue;
        /* mode_valid AND an fb: a CRTC between modes has one or the other but
         * not both, and putting a plane on it there would composite onto
         * nothing. */
        if (c.mode_valid && c.fb_id) {
            *id = ids[i];
            *index = i;
            free(ids);
            return 0;
        }
    }
    free(ids);
    return -1;
}

/* The first overlay plane that can carry `fourcc` on crtc[index] and is not
 * already bound to some crtc. Unbound is the strong condition: the primary
 * carrying rbp's picture reports crtc_id = 102 (bound), so this cannot pick it,
 * and a cursor plane would be an overlay in name only if it were bound to the
 * pointer. */
static int pick_plane(int fd, unsigned int index, unsigned int fourcc,
                      unsigned int *out)
{
    struct drm_mode_get_plane_res pr;
    unsigned int *ids, i, n;

    memset(&pr, 0, sizeof pr);
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0)
        return -1;
    n = pr.count_planes;
    if (n == 0)
        return -1;

    ids = calloc(n, sizeof *ids);
    if (!ids)
        return -1;
    pr.plane_id_ptr = (__u64)(uintptr_t)ids;
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        free(ids);
        return -1;
    }

    for (i = 0; i < n; i++) {
        struct drm_mode_get_plane gp;
        unsigned int fmts[DRM_BAND_MAXFORMATS];
        unsigned int k, count;
        int found = 0;

        /* ONE call, asking for more formats than any plane has. The kernel
         * reports the TRUE format_count in count_format_types whether or not it
         * could copy them, so asking for MAXFORMATS gets both the list and the
         * truth about its length -- the count-first idiom work/drmplane.c needed
         * only because it had guessed a buffer smaller than vc4's 39. */
        memset(&gp, 0, sizeof gp);
        gp.plane_id           = ids[i];
        gp.count_format_types = DRM_BAND_MAXFORMATS;
        gp.format_type_ptr    = (__u64)(uintptr_t)fmts;
        if (real_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
            continue;

        count = gp.count_format_types;
        if (count > DRM_BAND_MAXFORMATS)
            continue;       /* longer than we can see: cannot vouch for it */

        if (gp.crtc_id != 0)
            continue;
        if (!(gp.possible_crtcs & (1u << index)))
            continue;
        for (k = 0; k < count; k++)
            if (fmts[k] == fourcc) {
                found = 1;
                break;
            }
        if (!found)
            continue;
        if (plane_type_of(fd, ids[i]) != DRM_PLANE_TYPE_OVERLAY)
            continue;

        *out = ids[i];
        free(ids);
        return 0;
    }
    free(ids);
    return -1;
}

/* ------------------------------------------------------------ lifecycle */

int drm_band_setup(struct drm_band *b, int w, int h, int bpp)
{
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_mode_fb_cmd fc;
    unsigned int crtc_id, crtc_index, fourcc;
    int fd;

    memset(b, 0, sizeof *b);
    b->fd = -1;

    /* The two depths menu_paint.c draws in, and no others: a third would need a
     * palette this does not have. */
    if (bpp == 32) {
        fourcc = DRM_FORMAT_XRGB8888;
    } else if (bpp == 16) {
        fourcc = DRM_FORMAT_RGB565;
    } else {
        pointsrc_log("menu plane: %d bpp is not a format the band draws in", bpp);
        return -1;
    }
    if (w < 1 || h < 1)
        return -1;

    /* The device, its master and its UNIVERSAL_PLANES capability, SHARED with any
     * other band this process already has up -- see the holder above. The first
     * setup pays for all three; a second drawer's setup pays only for its own
     * buffer and plane, which is what the operator's "both side panels at the same
     * time" needs and what a second open+SET_MASTER refused. */
    fd = dev_acquire();
    if (fd < 0) {
        drm_band_teardown(b);       /* all-zero and safe: b->fd is -1, refs untouched */
        return -1;
    }
    b->fd = fd;

    if (live_crtc(fd, &crtc_id, &crtc_index) != 0) {
        pointsrc_log("menu plane: no CRTC on %s is scanning anything out",
                     DRM_BAND_CARD);
        drm_band_teardown(b);
        return -1;
    }
    if (pick_plane(fd, crtc_index, fourcc, &b->plane) != 0) {
        pointsrc_log("menu plane: no free overlay plane on crtc %u can carry"
                     " %c%c%c%c", crtc_id,
                     (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
                     (char)((fourcc >> 16) & 0xff), (char)((fourcc >> 24) & 0xff));
        drm_band_teardown(b);
        return -1;
    }
    b->crtc = crtc_id;

    memset(&cd, 0, sizeof cd);
    cd.width  = (__u32)w;
    cd.height = (__u32)h;
    cd.bpp    = (__u32)bpp;
    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        pointsrc_log("menu plane: CREATE_DUMB %dx%d %d bpp: %s",
                     w, h, bpp, strerror(errno));
        drm_band_teardown(b);
        return -1;
    }
    b->handle = cd.handle;

    memset(&fc, 0, sizeof fc);
    fc.width  = (__u32)w;
    fc.height = (__u32)h;
    fc.pitch  = cd.pitch;
    fc.bpp    = (__u32)bpp;
    fc.depth  = (bpp == 32) ? 24 : 16;
    fc.handle = cd.handle;
    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fc) < 0) {
        pointsrc_log("menu plane: ADDFB %dx%d %d bpp pitch %u: %s",
                     w, h, bpp, cd.pitch, strerror(errno));
        drm_band_teardown(b);
        return -1;
    }
    b->fb = fc.fb_id;

    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (real_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        pointsrc_log("menu plane: MAP_DUMB: %s", strerror(errno));
        drm_band_teardown(b);
        return -1;
    }
    b->map_len = (size_t)cd.size;
    b->pix = real_mmap(NULL, b->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, md.offset);
    if (b->pix == (void *)-1) {
        b->pix = NULL;
        /* The offset is 4 GiB and change (DRM_FILE_PAGE_OFFSET_START), which is
         * why real_mmap()'s offset argument is 64 bits; if this ever EINVALs
         * again, that is the first thing to check, not the buffer. */
        pointsrc_log("menu plane: mmap %zu bytes at offset 0x%llx: %s",
                     b->map_len, (unsigned long long)md.offset, strerror(errno));
        drm_band_teardown(b);
        return -1;
    }

    b->pitch = (int)(cd.pitch / (unsigned int)(bpp / 8));
    b->w     = w;
    b->h     = h;
    b->bpp   = bpp;

    /* Master is NOT taken here any more -- the shared device took it when the first
     * band went up and this band draws through that same file description.
     * `b->master` now means "the device holds master", which is true of every band
     * that got this far, and it is what makes dev_release()'s refcount the only
     * thing that decides when master is given back.
     *
     * Measured before the band was first wired in: taking master did NOT disturb the
     * mode DirectFB set up -- work/drmplane held it for minutes with rbp redrawing
     * underneath and the primary stayed at fb 722 throughout. */
    b->master = 1;

    /* The plane was chosen while every OTHER band's plane was already BOUND to the
     * CRTC -- pick_plane skips any plane whose crtc_id is not 0 -- which is the whole
     * of how two drawers get two planes instead of fighting over one. The id is in
     * the line below so the drill can see that the two are distinct. */
    pointsrc_log("menu plane: crtc %u plane %u fb %u %dx%d %d bpp pitch %d px"
                 " (%zu bytes) at %p",
                 b->crtc, b->plane, b->fb, w, h, bpp, b->pitch, b->map_len, b->pix);
    return 0;
}

int drm_band_show(struct drm_band *b, int x, int y)
{
    struct drm_mode_set_plane sp;

    if (b->fd < 0 || !b->pix)
        return -1;
    if (b->on && b->on_x == x && b->on_y == y)
        return 0;

    memset(&sp, 0, sizeof sp);
    sp.plane_id = b->plane;
    sp.crtc_id  = b->crtc;
    sp.fb_id    = b->fb;
    sp.crtc_x   = (__s32)x;
    sp.crtc_y   = (__s32)y;
    sp.crtc_w   = (__u32)b->w;
    sp.crtc_h   = (__u32)b->h;
    sp.src_x    = 0;
    sp.src_y    = 0;
    sp.src_w    = (__u32)b->w << 16;      /* 16.16 fixed point */
    sp.src_h    = (__u32)b->h << 16;
    if (real_ioctl(b->fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0) {
        pointsrc_log("menu plane: SETPLANE %u at %d,%d: %s",
                     b->plane, x, y, strerror(errno));
        b->on = 0;
        return -1;
    }
    b->on   = 1;
    b->on_x = x;
    b->on_y = y;
    return 0;
}

void drm_band_hide(struct drm_band *b)
{
    struct drm_mode_set_plane sp;

    if (b->fd < 0 || !b->on)
        return;
    memset(&sp, 0, sizeof sp);
    sp.plane_id = b->plane;
    sp.crtc_id  = b->crtc;
    sp.fb_id    = 0;                       /* fb_id 0 IS the disable */
    if (real_ioctl(b->fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0)
        /* Loud on purpose: the band is opaque, so a plane left on covers the top
         * of rbp's UI with a stale strip for the life of the process. */
        pointsrc_log("menu plane: could not switch plane %u off: %s -- a stale"
                     " band may be left on the glass", b->plane, strerror(errno));
    b->on = 0;
}

void drm_band_teardown(struct drm_band *b)
{
    if (b->fd < 0) {
        memset(b, 0, sizeof *b);
        b->fd = -1;
        return;
    }
    drm_band_hide(b);

    if (b->fb) {
        __u32 fbid = b->fb;

        if (real_ioctl(b->fd, DRM_IOCTL_MODE_RMFB, &fbid) < 0)
            pointsrc_log("menu plane: RMFB %u: %s", b->fb, strerror(errno));
        b->fb = 0;
    }
    if (b->pix) {
        munmap(b->pix, b->map_len);
        b->pix = NULL;
    }
    if (b->handle) {
        struct drm_mode_destroy_dumb dd;

        memset(&dd, 0, sizeof dd);
        dd.handle = b->handle;
        if (real_ioctl(b->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) < 0)
            pointsrc_log("menu plane: DESTROY_DUMB %u: %s", b->handle,
                         strerror(errno));
        b->handle = 0;
    }
    /* Everything above is PER BAND -- its framebuffer, its map, its dumb buffer --
     * and had to happen while the fd was still open. The device itself is shared, so
     * master is dropped and the card closed by dev_release() and only when the last
     * band lets go: a drawer closing must not pull the plane out from under the
     * drawer on the other edge, which is exactly what the operator asked for. */
    dev_release();

    memset(b, 0, sizeof *b);
    b->fd = -1;
}
