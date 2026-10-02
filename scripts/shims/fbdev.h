/*
 * fbdev.h — the framebuffer ABI, shared by the two files that talk to it.
 *
 * Hand-rolled rather than taken from <linux/fb.h>, for the reason fb_shim.c
 * already gave: this is the layout rbp was compiled against, with a 32-bit
 * `unsigned long smem_start`, and the build host's headers are not guaranteed to
 * agree with either the target's kernel or rbp. Both fb_shim.c (which lies to
 * its callers about the geometry) and fb_cursor.c (which reads the *real*
 * geometry, with a raw syscall, so it can draw into the actual page) need the
 * same structs, and a second copy of a kernel ABI struct is exactly the kind of
 * duplication that drifts silently — a wrong sizeof here is an ioctl that fills
 * the wrong bytes and returns success.
 */
#ifndef RBLIVE4_FBDEV_H
#define RBLIVE4_FBDEV_H

#ifndef FBIOGET_VSCREENINFO
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#endif

struct fb_var_screeninfo {
    unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned int bits_per_pixel, grayscale;
    struct { unsigned int offset, length, msb_right; } red, green, blue, transp;
    unsigned int nonstd;
    unsigned int activate;
    unsigned int height, width;
    unsigned int accel_flags;
    unsigned int pixclock, left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len, sync, vmode;
    unsigned int rotate;
    unsigned int colorspace;
    unsigned int reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned int smem_len;
    unsigned int type;
    unsigned int type_aux;
    unsigned int visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned int line_length;
    unsigned long mmio_start;
    unsigned int mmio_len;
    unsigned int accel;
    unsigned short capabilities;
    unsigned short reserved[2];
};

#endif /* RBLIVE4_FBDEV_H */
