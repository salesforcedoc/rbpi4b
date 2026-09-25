/* fbshim16.c - LD_PRELOAD ioctl shim: report a 16bpp RGB565 logical fb.
 *
 * rbp (rekordbox) was built for the RX3's RGB565 framebuffer.  We force the
 * window to RGB32 means DirectFB makes 32bpp surfaces but rbp still writes
 * RGB16 pixels -> colors wrong and content half-width.
 *
 * By reporting 16bpp RGB565 to DirectFB, DirectFB creates RGB16 surfaces and
 * rbp draws correct RGB565;  the fbdev driver then converts RGB16->RGB32 when
 * presenting into the real 800x1280x32 fb.
 *
 *   FBIOGET_VSCREENINFO : report 1280x800, 16bpp, RGB565 bitfields, yv=800
 *   FBIOGET_FSCREENINFO : line_length = 2560  (1280*2)
 *   FBIOPUT_VSCREENINFO : accept silently
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

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

static int real_ioctl(int fd, unsigned long request, void *arg)
{
    return syscall(SYS_ioctl, fd, request, arg);
}

int ioctl(int fd, unsigned long request, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo *v = arg;
        int r = real_ioctl(fd, request, v);
        if (r == 0 && v) {
            v->xres = 1280; v->yres = 800;
            v->xres_virtual = 1280; v->yres_virtual = 800;
            v->bits_per_pixel = 16;           /* RGB565 */
            v->grayscale = 0; v->nonstd = 0;
            v->red.offset = 11; v->red.length = 5; v->red.msb_right = 0;
            v->green.offset = 5; v->green.length = 6; v->green.msb_right = 0;
            v->blue.offset = 0; v->blue.length = 5; v->blue.msb_right = 0;
            v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            v->rotate = 0;
        }
        return r;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo *f = arg;
        int r = real_ioctl(fd, request, f);
        if (r == 0 && f)
            f->line_length = 2560;            /* 1280*2 */
        return r;
    }
    case FBIOPUT_VSCREENINFO:
        return 0;
    default:
        return real_ioctl(fd, request, arg);
    }
}
