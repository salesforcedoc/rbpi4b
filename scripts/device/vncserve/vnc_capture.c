/*
 * vnc_capture.c -- fb0 plus the overlay planes. See vnc_capture.h for the why.
 *
 * THE SHAPE. Open both devices once. Every frame: copy fb0, then walk card1's planes
 * and copy each active one over it. The plane LIST is re-read every 250 ms rather than
 * every frame (the set of planes changes only when rbp creates or destroys one), but a
 * plane's PIXELS and its rectangle are read every frame -- a drawer is repainted in
 * place, and its position moves as it slides. Getting that split wrong gives either a
 * drawer that never updates or an ioctl storm.
 *
 * NO DRM MASTER IS TAKEN. Everything here is a query: GETPLANE for the binding, GETFB
 * for the buffer, MAP_DUMB to map it. The plane rbp is scanning out is never written,
 * never re-bound, never disabled. rbp cannot tell this process is running.
 */
#define _GNU_SOURCE
#include "vnc_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ drm uapi
 *
 * The subset this file uses, byte-identical to scripts/shims/drmband.h, which took
 * them from libdrm 2.4.114. See that header for why they are frozen here rather than
 * included: Debian ships no drm uapi headers, and drmband.h also drags in the shim's
 * syscall wrappers, which a standalone binary must not link against.
 */
#define DRM_IOCTL_BASE   'd'
#define DRM_IOW(nr, type)   _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)  _IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_SET_CLIENT_CAP          DRM_IOW(0x0d, struct drm_set_client_cap)
#define DRM_IOCTL_MODE_GETPROPERTY        DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GETFB              DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_MAP_DUMB           DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES  DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE           DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES  DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)

#define DRM_MODE_OBJECT_PLANE   0xeeeeeeee
/* The atomic capability, and it is needed for a reason that is not obvious: the
 * properties this file reads to place a plane -- CRTC_ID, FB_ID, CRTC_X/Y/W/H,
 * SRC_X/Y/W/H -- are created with the kernel's DRM_MODE_PROP_ATOMIC flag, and the
 * kernel does not list an ATOMIC-flagged property to a client that has not set this
 * capability. Measured on this unit: without it a plane object lists 11 properties
 * (type, IN_FORMATS, alpha, pixel blend mode, rotation, COLOR_ENCODING, COLOR_RANGE,
 * SCALING_FILTER, CHROMA_SITING_H/V, zpos) and -- the trap -- the read of CRTC_X does
 * not fail loudly, it returns -1 for a name that is not in the list at all, which
 * looks exactly like a plane sitting at the origin. Setting it is a property of the
 * FILE, it commits nothing, and it is orthogonal to the master this process never
 * takes. */
#define DRM_CLIENT_CAP_ATOMIC   0
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_PLANE_TYPE_PRIMARY  1
#define DRM_PROP_NAME_LEN       32

/* 'R''G''1''6' -- fb0's own format, and the one every plane this port creates uses
 * (RG16; see scripts/shims/drmband.h). */
#define DRM_FORMAT_RGB565       0x36314752

/* vc4's planes report 39 formats -- measured, and the reason a fixed 32-format buffer
 * cost a whole run in work/drmplane.c: GETPLANE returns EINVAL when count_format_types
 * is below the real count. 64 is comfortably clear of it. */
#define VNC_MAX_FORMATS 64
#define VNC_MAX_PLANES  32
#define VNC_MAX_FBS     16
#define VNC_MAX_PROPS   64

struct drm_mode_get_plane_res {
    __u64 plane_id_ptr;
    __u32 count_planes;
};

struct drm_mode_get_plane {
    __u32 plane_id;
    __u32 crtc_id;
    __u32 fb_id;
    __u32 possible_crtcs;
    __u32 gamma_size;
    __u32 count_format_types;
    __u64 format_type_ptr;
};

struct drm_mode_get_property {
    __u64 values_ptr;
    __u64 enum_blob_ptr;
    __u32 prop_id;
    __u32 flags;
    char  name[DRM_PROP_NAME_LEN];
    __u32 count_values;
    __u32 count_enum_blobs;
};

struct drm_mode_obj_get_properties {
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u32 count_props;
    __u32 obj_id;
    __u32 obj_type;
};

struct drm_mode_map_dumb {
    __u32 handle;
    __u32 pad;
    __u64 offset;
};

struct drm_mode_fb_cmd {
    __u32 fb_id;
    __u32 width;
    __u32 height;
    __u32 pitch;
    __u32 bpp;
    __u32 depth;
    __u32 handle;
};

struct drm_set_client_cap {
    __u64 capability;
    __u64 value;
};

/* ------------------------------------------------------------------ state */

struct vnc_fb {
    uint32_t id;
    int w, h, pitch_px;
    void *map;
    size_t size;
    int dead;                 /* GETFB or MAP_DUMB failed; dropped at the next refresh */
};

/* One plane's placement, keyed by the framebuffer it scans out of. See
 * refresh_geometry() for where these come from and why they cannot come from the
 * property API. */
struct vnc_geom {
    uint32_t fb;
    int x, y, w, h;
};

#define VNC_MAX_GEOM 16
#define VNC_MAX_CRTC 8

/* THE ONLY PLACE PLANE GEOMETRY IS READABLE ON THIS UNIT, and that is a measured
 * fact rather than a preference.
 *
 * The obvious route -- the CRTC_X / CRTC_Y / CRTC_W / CRTC_H properties -- does not
 * exist here. Those properties are created with the kernel's DRM_MODE_PROP_ATOMIC
 * flag, and vc4 on this kernel is not an atomic driver, so DRM_CLIENT_CAP_ATOMIC is
 * refused ("atomic capability: REFUSED", measured 2026-10-09) and the kernel omits
 * every ATOMIC-flagged property from a plane's list. A plane object here reports
 * eleven properties -- type, IN_FORMATS, alpha, pixel blend mode, rotation,
 * COLOR_ENCODING, COLOR_RANGE, SCALING_FILTER, CHROMA_SITING_H/V, zpos -- and neither
 * FB_ID, CRTC_ID, CRTC_X/Y/W/H nor SRC_X/Y/W/H is among them. The failure is silent
 * in the worst way: the lookup for "CRTC_X" finds no such name and returns nothing,
 * which reads exactly like a plane sitting at the origin.
 *
 * struct drm_mode_get_plane carries no rectangle either (it stops at
 * count_format_types), so there is no ioctl that answers this. What does answer it is
 * the driver's own state dump, which prints every plane's placement as
 * "crtc-pos=WxH+X+Y" whether or not the driver is atomic. It is debugfs, so it needs
 * root and a mounted debugfs -- both of which hold for this unit, where vncserve runs
 * as root and the kernel is pinned by the deploy.
 *
 * READ EVERY FRAME, not on the plane list's 250 ms cadence: a drawer is *sliding*
 * while it opens, and a position sampled four times a second would make it jump. A
 * 10 KB text read is free at the few frames per second this serves. */
#define VNC_DRM_STATE "/sys/kernel/debug/dri/1/state"

struct vnc_capture {
    int fbfd;
    void *fb;                 /* the mmap of /dev/fb0 */
    size_t fbsize;
    int w, h, stride_px;

    int drmfd;
    struct vnc_fb fbs[VNC_MAX_FBS];

    struct vnc_geom geom[VNC_MAX_GEOM];
    int ngeom;
    uint32_t crtc_on[VNC_MAX_CRTC];   /* CRTCs with enable=1 */
    int ncrtc_on;
    int state_err;                   /* errno from the last state read; 0 if it worked */

    uint32_t plane_ids[VNC_MAX_PLANES];
    int nplane_ids;
    struct timespec listed_at;
    int have_listed;

    struct vnc_plane planes[VNC_MAX_PLANES];
    int nplanes;

    int skipped_primary;      /* counted once for the status line */
    int include_primary;      /* diagnostic; see vnc_capture.h */
    int no_atomic;            /* the kernel refused the atomic cap */
    int no_geometry;          /* a plane was on screen but its placement was not readable */
    int failed_planes;
    int bad_format;

    char status[256];
};

/* ------------------------------------------------------------------ geometry
 *
 * The debugfs state dump, parsed. The relevant shape, verbatim from this unit:
 *
 *     plane[127]: plane-6
 *             crtc=pixelvalve-2
 *             fb=723
 *                     size=180x800
 *             crtc-pos=180x800+0+0
 *             normalized-zpos=1
 *     crtc[102]: pixelvalve-2
 *             enable=1
 *
 * so a plane block is keyed by its `fb=` line and placed by the `crtc-pos=WxH+X+Y`
 * that follows it. Parsing is line-oriented and deliberately forgiving: an unknown
 * line is skipped, and a block that never yields both a framebuffer and a position
 * simply contributes nothing. A dump this file cannot parse leaves the geometry table
 * empty, which shows up as "no geometry" in the status rather than as a wrong picture.
 */
static void refresh_geometry(struct vnc_capture *c)
{
    FILE *f;
    char line[256];
    uint32_t cur_fb = 0, cur_crtc = 0;
    int in_crtc = 0;

    c->ngeom = 0;
    c->ncrtc_on = 0;

    f = fopen(VNC_DRM_STATE, "r");
    if (!f) {
        c->state_err = errno;
        return;
    }
    c->state_err = 0;

    while (fgets(line, sizeof line, f)) {
        const char *p = line;

        if (!strncmp(p, "plane[", 6)) { cur_fb = 0; in_crtc = 0; continue; }
        if (!strncmp(p, "crtc[", 5))  { in_crtc = 1; cur_crtc = 0; sscanf(p, "crtc[%u]", &cur_crtc); continue; }
        if (!strncmp(p, "connector[", 10) || !strncmp(p, "HVS State", 9)) { in_crtc = 0; break; }

        while (*p == '\t' || *p == ' ') p++;

        if (in_crtc) {
            if (!strncmp(p, "enable=1", 8) && c->ncrtc_on < VNC_MAX_CRTC)
                c->crtc_on[c->ncrtc_on++] = cur_crtc;
            continue;
        }

        if (!strncmp(p, "fb=", 3)) {
            cur_fb = (uint32_t)strtoul(p + 3, NULL, 10);
            continue;
        }
        if (!strncmp(p, "crtc-pos=", 9) && cur_fb != 0 && c->ngeom < VNC_MAX_GEOM) {
            unsigned w = 0, h = 0, x = 0, y = 0;
            if (sscanf(p + 9, "%ux%u+%u+%u", &w, &h, &x, &y) == 4) {
                c->geom[c->ngeom].fb = cur_fb;
                c->geom[c->ngeom].x  = (int)x;
                c->geom[c->ngeom].y  = (int)y;
                c->geom[c->ngeom].w  = (int)w;
                c->geom[c->ngeom].h  = (int)h;
                c->ngeom++;
            }
        }
    }
    fclose(f);
}

static const struct vnc_geom *geom_for(const struct vnc_capture *c, uint32_t fb)
{
    int i;
    for (i = 0; i < c->ngeom; i++)
        if (c->geom[i].fb == fb)
            return &c->geom[i];
    return NULL;
}

/* A CRTC that is switched off still lists its planes, and drawing one would put a
 * drawer on a screen that is not being scanned out. If the state file could not be
 * read at all, filter nothing rather than drop every plane. */
static int crtc_enabled(const struct vnc_capture *c, uint32_t crtc_id)
{
    int i;
    if (c->ncrtc_on == 0)
        return 1;
    for (i = 0; i < c->ncrtc_on; i++)
        if (c->crtc_on[i] == crtc_id)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ helpers */

static long ms_since(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (long)((b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000);
}

/* One named property of one plane object. Returns 0 and stores the value, or -1.
 *
 * MATCHED BY NAME, EVERY TIME, with no cache. The obvious optimisation -- build the
 * name-to-id table once and reuse it, since property ids are global to the device --
 * was written first and was WRONG here: it reported "zpos" as unreadable for two
 * planes whose own property list plainly contained zpos=1, i.e. it turned a working
 * read into a silent default of zero, which for zpos means the composite stacks its
 * planes in whatever order the kernel happened to list them. The cost of not caching
 * is one extra ioctl per property per plane per frame, which at a handful of planes
 * and a few frames a second is nothing weighed against a wrong picture. */
static int plane_prop(struct vnc_capture *c, uint32_t obj_id, const char *want, uint64_t *out)
{
    struct drm_mode_obj_get_properties q;
    uint32_t ids[VNC_MAX_PROPS];
    uint64_t vals[VNC_MAX_PROPS];
    int i;

    memset(&q, 0, sizeof q);
    memset(ids, 0, sizeof ids);
    memset(vals, 0, sizeof vals);
    q.props_ptr       = (__u64)(uintptr_t)ids;
    q.prop_values_ptr = (__u64)(uintptr_t)vals;
    q.count_props     = VNC_MAX_PROPS;
    q.obj_id          = obj_id;
    q.obj_type        = DRM_MODE_OBJECT_PLANE;
    if (ioctl(c->drmfd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &q) < 0)
        return -1;

    for (i = 0; i < (int)q.count_props && i < VNC_MAX_PROPS; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof gp);
        gp.prop_id = ids[i];
        if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        gp.name[DRM_PROP_NAME_LEN - 1] = '\0';
        if (strcmp(gp.name, want) == 0) {
            *out = vals[i];
            return 0;
        }
    }
    return -1;
}

/* The pixels of one framebuffer, mapped once and kept. NULL if it cannot be had. */
static struct vnc_fb *get_fb(struct vnc_capture *c, uint32_t fb_id)
{
    struct drm_mode_fb_cmd fc;
    struct drm_mode_map_dumb md;
    struct vnc_fb *slot = NULL;
    int i;

    for (i = 0; i < VNC_MAX_FBS; i++)
        if (c->fbs[i].id == fb_id && !c->fbs[i].dead)
            return &c->fbs[i];
    for (i = 0; i < VNC_MAX_FBS; i++)
        if (c->fbs[i].id == 0 || c->fbs[i].dead) { slot = &c->fbs[i]; break; }
    if (!slot)
        return NULL;                       /* 16 live planes would be a different problem */

    memset(slot, 0, sizeof *slot);
    memset(&fc, 0, sizeof fc);
    fc.fb_id = fb_id;
    if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETFB, &fc) < 0) {
        slot->id = fb_id; slot->dead = 1; slot->w = slot->h = 0;
        return NULL;
    }

    memset(&md, 0, sizeof md);
    md.handle = fc.handle;
    if (ioctl(c->drmfd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        slot->id = fb_id; slot->dead = 1; slot->w = slot->h = 0;
        return NULL;
    }

    slot->size = (size_t)fc.pitch * fc.height;
    slot->map = mmap(NULL, slot->size, PROT_READ, MAP_SHARED, c->drmfd, (off_t)md.offset);
    if (slot->map == MAP_FAILED) {
        slot->map = NULL; slot->id = fb_id; slot->dead = 1; slot->w = slot->h = 0;
        return NULL;
    }

    slot->id       = fb_id;
    slot->w        = (int)fc.width;
    slot->h        = (int)fc.height;
    slot->pitch_px = (int)(fc.pitch / 2);
    return slot;
}

/* The plane list, re-read at most every 250 ms. */
static void refresh_plane_list(struct vnc_capture *c)
{
    struct drm_mode_get_plane_res pr;
    uint32_t ids[VNC_MAX_PLANES];
    int i;

    if (c->have_listed && ms_since(&c->listed_at) < 250)
        return;

    memset(&pr, 0, sizeof pr);
    memset(ids, 0, sizeof ids);
    pr.plane_id_ptr = (__u64)(uintptr_t)ids;
    pr.count_planes = VNC_MAX_PLANES;
    if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        c->nplane_ids = 0;
        return;
    }

    c->nplane_ids = (int)pr.count_planes;
    if (c->nplane_ids > VNC_MAX_PLANES)
        c->nplane_ids = VNC_MAX_PLANES;
    for (i = 0; i < c->nplane_ids; i++)
        c->plane_ids[i] = ids[i];

    clock_gettime(CLOCK_MONOTONIC, &c->listed_at);
    c->have_listed = 1;

    /* Any framebuffer that failed last round gets another chance now: rbp reallocates
     * a plane's buffer when the drawer resizes, and the old id is simply gone. */
    for (i = 0; i < VNC_MAX_FBS; i++)
        if (c->fbs[i].dead && c->fbs[i].map == NULL)
            c->fbs[i].dead = 0;
}

/* One plane on screen, appended to c->planes. */
static void add_plane(struct vnc_capture *c, uint32_t plane_id)
{
    struct drm_mode_get_plane gp;
    uint32_t formats[VNC_MAX_FORMATS];
    uint64_t type = 0, z = 0;
    struct vnc_fb *fb;
    struct vnc_plane *p;
    int has_rgb565 = 0, i;

    memset(&gp, 0, sizeof gp);
    memset(formats, 0, sizeof formats);
    gp.plane_id          = plane_id;
    gp.count_format_types = VNC_MAX_FORMATS;
    gp.format_type_ptr   = (__u64)(uintptr_t)formats;
    if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
        return;

    if (gp.crtc_id == 0 || gp.fb_id == 0)
        return;                            /* not scanning out; nothing on screen */

    /* THE PRIMARY PLANE IS /dev/fb0. It is already in `dst` from the memcpy, and
     * copying it again from its own buffer would be a second 2 MB read for no
     * change -- and worse, it would cover any plane that is below it in z. */
    if (plane_prop(c, plane_id, "type", &type) == 0 && (int)type == DRM_PLANE_TYPE_PRIMARY) {
        c->skipped_primary = 1;
        if (!c->include_primary)
            return;
    }

    for (i = 0; i < (int)gp.count_format_types; i++)
        if (formats[i] == DRM_FORMAT_RGB565) { has_rgb565 = 1; break; }
    if (!has_rgb565) {
        c->bad_format++;
        return;                            /* XRGB8888 and friends would need a swizzle */
    }

    fb = get_fb(c, gp.fb_id);
    if (!fb) { c->failed_planes++; return; }

    if (!crtc_enabled(c, gp.crtc_id))
        return;

    /* THE PLACEMENT, from the kernel's state dump -- see refresh_geometry() for why
     * this is not a property read. Without it there is no way to know where the plane
     * goes, and drawing it at a guess would be worse than not drawing it. */
    {
        const struct vnc_geom *g = geom_for(c, gp.fb_id);
        if (!g) { c->no_geometry++; return; }
        if (c->nplanes >= VNC_MAX_PLANES)
            return;

        p = &c->planes[c->nplanes++];
        p->dst_x = g->x;
        p->dst_y = g->y;
        p->w     = g->w;
        p->h     = g->h;
        /* The framebuffer is the only thing that can actually be read; a rectangle
         * larger than it would walk off the end of the mapping. */
        if (p->w > fb->w) p->w = fb->w;
        if (p->h > fb->h) p->h = fb->h;
    }

    if (plane_prop(c, plane_id, "zpos", &z) < 0) z = 0;
    p->src           = (const uint16_t *)fb->map;
    p->src_stride_px = fb->pitch_px;
    p->zpos          = (int)(int32_t)z;
}

/* ------------------------------------------------------------------ public */

struct vnc_capture *vnc_capture_open(char *err, size_t errlen)
{
    struct vnc_capture *c;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    struct drm_set_client_cap cap;
    size_t n;

    c = calloc(1, sizeof *c);
    if (!c) { if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    c->fbfd = c->drmfd = -1;

    c->fbfd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (c->fbfd < 0) {
        if (err) snprintf(err, errlen, "open /dev/fb0: %s", strerror(errno));
        goto fail;
    }
    if (ioctl(c->fbfd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        ioctl(c->fbfd, FBIOGET_VSCREENINFO, &var) < 0) {
        if (err) snprintf(err, errlen, "FBIOGET_*SCREENINFO: %s", strerror(errno));
        goto fail;
    }
    if (var.bits_per_pixel != 16) {
        if (err) snprintf(err, errlen, "/dev/fb0 is %u bpp, not the 16 this expects",
                          var.bits_per_pixel);
        goto fail;
    }

    c->w = (int)var.xres;
    c->h = (int)var.yres;
    c->stride_px = (int)(fix.line_length / 2);
    n = (size_t)fix.line_length * var.yres;
    c->fb = mmap(NULL, n, PROT_READ, MAP_SHARED, c->fbfd, 0);
    if (c->fb == MAP_FAILED) {
        c->fb = NULL;
        if (err) snprintf(err, errlen, "mmap /dev/fb0: %s", strerror(errno));
        goto fail;
    }
    c->fbsize = n;

    c->drmfd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (c->drmfd < 0) {
        if (err) snprintf(err, errlen, "open /dev/dri/card1: %s", strerror(errno));
        goto fail;
    }

    /* Without this the kernel lists only the overlay planes -- this board returned 48
     * of card1's 60 objects with every primary and cursor simply absent -- so the plane
     * rbp's deck is on would not even be found. It is a property of the FILE. */
    memset(&cap, 0, sizeof cap);
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value      = 1;
    if (ioctl(c->drmfd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        if (err) snprintf(err, errlen, "SET_CLIENT_CAP UNIVERSAL_PLANES: %s", strerror(errno));
        goto fail;
    }

    /* And this, or every plane appears to sit at (0,0) -- see the note on
     * DRM_CLIENT_CAP_ATOMIC in this file's uapi block. A failure is not fatal: the
     * planes still composite, at the wrong place, and the status line says so. */
    memset(&cap, 0, sizeof cap);
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value      = 1;
    if (ioctl(c->drmfd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        c->no_atomic = 1;
    }

    snprintf(c->status, sizeof c->status, "%dx%d RGB565, no frame yet", c->w, c->h);
    return c;

fail:
    vnc_capture_close(c);
    return NULL;
}

void vnc_capture_close(struct vnc_capture *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < VNC_MAX_FBS; i++)
        if (c->fbs[i].map) munmap(c->fbs[i].map, c->fbs[i].size);
    if (c->fb) munmap(c->fb, c->fbsize);
    if (c->fbfd >= 0) close(c->fbfd);
    if (c->drmfd >= 0) close(c->drmfd);
    free(c);
}

int vnc_capture_width(const struct vnc_capture *c)  { return c->w; }
int vnc_capture_height(const struct vnc_capture *c) { return c->h; }
int vnc_capture_plane_count(const struct vnc_capture *c) { return c->nplanes; }
const struct vnc_plane *vnc_capture_planes(const struct vnc_capture *c) { return c->planes; }
const char *vnc_capture_status(const struct vnc_capture *c) { return c->status; }

void vnc_capture_include_primary(struct vnc_capture *c, int on) { c->include_primary = on; }

void vnc_capture_describe(struct vnc_capture *c)
{
    int i;

    refresh_plane_list(c);
    refresh_geometry(c);
    printf("card1: %d plane object(s)\n", c->nplane_ids);
    printf("atomic capability: %s\n", c->no_atomic ? "REFUSED (vc4 is not an atomic driver)" : "accepted");
    printf("geometry: %d plane(s) placed from " VNC_DRM_STATE "%s\n", c->ngeom,
           c->state_err ? " -- UNREADABLE" : "");
    for (i = 0; i < c->nplane_ids; i++) {
        struct drm_mode_get_plane gp;
        uint32_t formats[VNC_MAX_FORMATS];
        uint64_t type = 0, z = 0;
        const char *tn = "?";
        int k, rgb565 = 0;

        memset(&gp, 0, sizeof gp);
        memset(formats, 0, sizeof formats);
        gp.plane_id           = c->plane_ids[i];
        gp.count_format_types = VNC_MAX_FORMATS;
        gp.format_type_ptr    = (__u64)(uintptr_t)formats;
        if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0) {
            printf("  plane %u: GETPLANE failed (%s)\n", c->plane_ids[i], strerror(errno));
            continue;
        }
        for (k = 0; k < (int)gp.count_format_types && k < VNC_MAX_FORMATS; k++)
            if (formats[k] == DRM_FORMAT_RGB565) rgb565 = 1;

        plane_prop(c, c->plane_ids[i], "type", &type);
        if ((int)type == DRM_PLANE_TYPE_PRIMARY) tn = "primary";
        else if ((int)type == 0)                 tn = "overlay";
        else if ((int)type == 2)                 tn = "cursor";

        printf("  plane %u: %-7s crtc=%u fb=%u %s", c->plane_ids[i], tn,
               gp.crtc_id, gp.fb_id, rgb565 ? "RGB565" : "NOT-RGB565");
        if (gp.crtc_id && gp.fb_id) {
            const struct vnc_geom *g = geom_for(c, gp.fb_id);
            if (g)
                printf(" dst=(%d,%d) %dx%d", g->x, g->y, g->w, g->h);
            else
                printf(" dst=UNKNOWN (fb %u is not in the state dump)", gp.fb_id);
            if (plane_prop(c, c->plane_ids[i], "zpos", &z) == 0)
                printf(" zpos-prop=%d", (int)(int32_t)z);
            else
                printf(" zpos-prop=UNREADABLE");
            printf("  <-- on screen%s", crtc_enabled(c, gp.crtc_id) ? "" : " [crtc off!]");
            {
                struct drm_mode_fb_cmd fc;
                memset(&fc, 0, sizeof fc);
                fc.fb_id = gp.fb_id;
                if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETFB, &fc) == 0)
                    printf(" fb=%ux%u pitch=%u bpp=%u",
                           fc.width, fc.height, fc.pitch, fc.bpp);
                else
                    printf(" GETFB failed (%s)", strerror(errno));
            }
        }
        printf("\n");
        if (gp.crtc_id && gp.fb_id) {
            struct drm_mode_obj_get_properties q;
            uint32_t ids[VNC_MAX_PROPS];
            uint64_t vals[VNC_MAX_PROPS];
            memset(&q, 0, sizeof q);
            memset(ids, 0, sizeof ids);
            memset(vals, 0, sizeof vals);
            q.props_ptr       = (__u64)(uintptr_t)ids;
            q.prop_values_ptr = (__u64)(uintptr_t)vals;
            q.count_props     = VNC_MAX_PROPS;
            q.obj_id          = c->plane_ids[i];
            q.obj_type        = DRM_MODE_OBJECT_PLANE;
            if (ioctl(c->drmfd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &q) < 0) {
                printf("    props: OBJ_GETPROPERTIES failed (%s)\n", strerror(errno));
            } else {
                int m;
                printf("    %u prop(s):", q.count_props);
                for (m = 0; m < (int)q.count_props && m < VNC_MAX_PROPS; m++) {
                    struct drm_mode_get_property gp2;
                    memset(&gp2, 0, sizeof gp2);
                    gp2.prop_id = ids[m];
                    if (ioctl(c->drmfd, DRM_IOCTL_MODE_GETPROPERTY, &gp2) < 0) {
                        printf(" [%u:GETPROPERTY failed]", ids[m]);
                        continue;
                    }
                    gp2.name[DRM_PROP_NAME_LEN - 1] = '\0';
                    printf(" %s=%llu", gp2.name, (unsigned long long)vals[m]);
                }
                printf("\n");
            }
        }
    }
}

int vnc_capture_frame(struct vnc_capture *c, uint16_t *dst)
{
    int row, i;
    const uint16_t *fb = (const uint16_t *)c->fb;

    /* 1. The deck, straight out of fb0's mapping. One memcpy per row when the pitch
     *    is exactly the width (it is: 2560 B for 1280 px), which the branch below
     *    collapses to a single call. */
    if (c->stride_px == c->w) {
        memcpy(dst, fb, (size_t)c->w * (size_t)c->h * sizeof *dst);
    } else {
        for (row = 0; row < c->h; row++)
            memcpy(dst + (size_t)row * c->w, fb + (size_t)row * c->stride_px,
                   (size_t)c->w * sizeof *dst);
    }

    /* 2. Every active overlay plane, over the top. Geometry first: a drawer's
     *    position changes as it slides, so it is read every frame. */
    c->nplanes = 0;
    c->failed_planes = 0;
    c->bad_format = 0;
    c->no_geometry = 0;
    refresh_plane_list(c);
    refresh_geometry(c);
    for (i = 0; i < c->nplane_ids; i++)
        add_plane(c, c->plane_ids[i]);

    vnc_compose_planes(dst, c->w, c->h, c->planes, c->nplanes);

    snprintf(c->status, sizeof c->status,
             "%dx%d RGB565; %d plane(s)%s%s%s%s",
             c->w, c->h, c->nplanes,
             c->failed_planes ? "; a plane failed to map" : "",
             c->bad_format    ? "; a plane is not RGB565"  : "",
             c->no_geometry   ? "; a plane had no readable placement" : "",
             (c->skipped_primary && c->state_err == 0) ? "" :
                 (c->state_err ? "; " VNC_DRM_STATE " unreadable" : "; no primary plane seen"));

    /* A frame that could not read a plane is still a frame -- the operator sees a
     * screen with a stale drawer rather than no screen at all. */
    return 0;
}
