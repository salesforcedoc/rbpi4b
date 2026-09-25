/*
 * fbdump.c — dump everything about a framebuffer that the rblive4 display path
 * depends on, then say what it means for the port.
 *
 * The patched DirectFB fbdev driver makes decisions from these exact fields:
 * bits_per_pixel and the channel bitfields decide whether a pixel conversion is
 * needed, yres_virtual/ypanstep decide whether the fb can be double-buffered,
 * line_length is the physical stride, and smem_len bounds the mmap. So this tool
 * is the first thing to run on a new target, before touching any display code.
 *
 * Runs natively on the Pi (Pi OS has gcc):
 *   gcc -O2 -static -o fbdump fbdump.c
 *   ./fbdump                 # /dev/fb0
 *   ./fbdump /dev/fb0
 *
 * Read-only: it opens the framebuffer and never issues a set/modeset ioctl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int read_sysfs_str(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static void sysfs(const char *fb, const char *leaf)
{
    char path[256], buf[256];
    snprintf(path, sizeof(path), "/sys/class/graphics/%s/%s", fb, leaf);
    if (read_sysfs_str(path, buf, sizeof(buf)) == 0)
        printf("  %-18s %s\n", leaf, buf);
}

static void dump_bitfield(const char *name, struct fb_bitfield *bf)
{
    printf("  %-7s offset=%-2u length=%-2u msb_right=%u\n",
           name, bf->offset, bf->length, bf->msb_right);
}

/* What the fbdev driver's format-forcing / convert path will have to do. */
static const char *format_verdict(const struct fb_var_screeninfo *v)
{
    if (v->grayscale)
        return "grayscale fb -- not a colour format rbp can use";

    if (v->bits_per_pixel == 16 &&
        v->red.offset == 11 && v->red.length == 5 &&
        v->green.offset == 5 && v->green.length == 6 &&
        v->blue.offset == 0 && v->blue.length == 5)
        return "RGB565, 16bpp, exactly rbp's native format:\n"
               "    present path can memcpy per row (no conversion)";

    if (v->bits_per_pixel == 32 &&
        v->red.offset == 16 && v->red.length == 8 &&
        v->green.offset == 8 && v->green.length == 8 &&
        v->blue.offset == 0 && v->blue.length == 8)
        return "XRGB8888, 32bpp: 565->8888 convert required\n"
               "    use DFB_PRESENT=convert (and watch the frame cost)";

    if (v->bits_per_pixel == 16)
        return "16bpp but NOT the standard 565 bitfield layout:\n"
               "    check the offsets above before trusting a memcpy path";

    return "unrecognised format: the present path must be told about it";
}

static void pan_verdict(const struct fb_var_screeninfo *v,
                        const struct fb_fix_screeninfo *f)
{
    int pages = (v->yres > 0) ? (int)(v->yres_virtual / v->yres) : 0;

    printf("  yres_virtual/yres   %u/%u  (%d page%s)\n",
           v->yres_virtual, v->yres, pages, pages == 1 ? "" : "s");
    printf("  xpanstep/ypanstep   %u/%u\n", f->xpanstep, f->ypanstep);

    if (f->ypanstep == 0 && f->ywrapstep == 0)
        printf("  -> NOT pannable: single-buffered FRONTONLY.\n"
               "     DirectFB will force DLBM_TRIPLE, the patched driver will\n"
               "     keep the real yres_virtual and stop failing the modeset.\n");
    else if (pages > 1)
        printf("  -> pannable with %d pages: double buffering is possible.\n",
               pages);
    else
        printf("  -> pannable flag set but only one page: verify with dmesg.\n");
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    const char *fbname = strrchr(dev, '/');
    fbname = fbname ? fbname + 1 : dev;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fbdump: open %s: %s\n", dev, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr,
                    "  no framebuffer here. Under vc4-kms-v3d, /dev/fb0 comes from\n"
                    "  DRM fbdev emulation and should exist; check that\n"
                    "  dtoverlay=vc4-kms-v3d is in /boot/firmware/config.txt and that\n"
                    "  dmesg says 'fb0: vc4drmfb frame buffer device'.\n");
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    memset(&var, 0, sizeof(var));
    memset(&fix, 0, sizeof(fix));

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
        fprintf(stderr, "fbdump: FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        fprintf(stderr, "fbdump: FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        return 1;
    }

    printf("device: %s\n", dev);
    printf("\n--- var (FBIOGET_VSCREENINFO) ---\n");
    printf("  xres yres           %u %u\n", var.xres, var.yres);
    printf("  xres_virtual        %u\n", var.xres_virtual);
    printf("  xoffset yoffset     %u %u\n", var.xoffset, var.yoffset);
    printf("  bits_per_pixel      %u\n", var.bits_per_pixel);
    printf("  grayscale           %u\n", var.grayscale);
    dump_bitfield("red", &var.red);
    dump_bitfield("green", &var.green);
    dump_bitfield("blue", &var.blue);
    dump_bitfield("transp", &var.transp);
    printf("  physical w x h      %u x %u mm\n", var.width, var.height);
    printf("  pixclock            %u\n", var.pixclock);
    printf("  margins l/r u/l     %u/%u %u/%u\n",
           var.left_margin, var.right_margin, var.upper_margin, var.lower_margin);
    printf("  hsync vsync len     %u %u\n", var.hsync_len, var.vsync_len);
    printf("  sync                %u\n", var.sync);
    printf("  vmode               %u\n", var.vmode);
    printf("  rotate              %u\n", var.rotate);
    printf("  nonstd activate     %u %u\n", var.nonstd, var.activate);

    printf("\n--- fix (FBIOGET_FSCREENINFO) ---\n");
    printf("  id                  %s\n", fix.id);
    printf("  smem_start          0x%08lx\n", (unsigned long)fix.smem_start);
    printf("  smem_len            %u  (%.2f MiB)\n",
           fix.smem_len, fix.smem_len / 1048576.0);
    printf("  type type_aux       %u %u\n", fix.type, fix.type_aux);
    printf("  visual              %u\n", fix.visual);
    printf("  line_length         %u\n", fix.line_length);
    printf("  mmio_len accel      %u %u\n", fix.mmio_len, fix.accel);

    printf("\n--- sysfs (/sys/class/graphics/%s) ---\n", fbname);
    sysfs(fbname, "name");
    sysfs(fbname, "virtual_size");
    sysfs(fbname, "stride");
    sysfs(fbname, "bits_per_pixel");
    sysfs(fbname, "rotate");
    sysfs(fbname, "blank");

    printf("\n--- what this means ---\n");
    printf("  format: %s\n", format_verdict(&var));
    unsigned long need = (unsigned long)fix.line_length * var.yres_virtual;
    printf("  framebuffer bytes   %lu (%.2f MiB) vs smem_len %u\n",
           need, need / 1048576.0, fix.smem_len);
    if (fix.smem_len && need > fix.smem_len)
        printf("  -> OVERFLOW: the mmap would exceed smem_len; the present path\n"
               "     MUST clamp to smem_len or it will fault.\n");
    pan_verdict(&var, &fix);

    /* The shim reports a logical 1280x800 RGB565 fb. Whether that matches what
     * is really there decides if a convert is needed at all. */
    printf("\n--- against rbp's expectation (logical 1280x800 RGB565) ---\n");
    if (var.xres == 1280 && var.yres == 800) {
        printf("  geometry   MATCHES -- present path is a 1:1 copy, no bars\n");
    } else if (var.xres >= 1280 && var.yres >= 800) {
        printf("  geometry   fb is %ux%u: the 1280x800 image FITS 1:1\n",
               var.xres, var.yres);
        printf("             -> DFB_PRESENT=letterbox, bars of %u columns and %u rows\n",
               (var.xres - 1280) / 2, (var.yres - 800) / 2);
        printf("             (best case: no scaler, no distortion)\n");
    } else {
        /* The case worth getting right, and easy to get wrong: a 1:1 copy needs
         * the image to FIT. When the fb is the smaller one -- 1280x720 is the
         * one to expect, because it is what a sink that refused 1280x800 falls
         * back to -- there is no 1:1 placement at all and DFB_PRESENT=letterbox
         * cannot be satisfied. A crop or a scale is the only way onto the
         * screen, and both change what the operator sees. */
        double sx = var.xres / 1280.0, sy = var.yres / 800.0;
        double s = sx < sy ? sx : sy;         /* uniform: preserve the aspect */
        printf("  geometry   fb is %ux%u: SMALLER than the 1280x800 image, so a 1:1\n",
               var.xres, var.yres);
        printf("             copy does NOT fit -- letterbox is impossible here.\n");
        printf("             the ways out, in order of how much they cost:\n");
        printf("             1. set a mode >= 1280x800 and use the 1:1 path -- check\n"
               "                the `modes` file of the connector in /sys/class/drm/\n");
        printf("             2. scale uniformly by %.4f -> %ux%u, bars in the other\n",
               s, (unsigned)(1280 * s), (unsigned)(800 * s));
        printf("                axis (no distortion, but a real per-frame scaler)\n");
        printf("             3. crop %u columns and %u rows, losing UI at the edges\n",
               var.xres < 1280 ? 1280 - var.xres : 0,
               var.yres < 800 ? 800 - var.yres : 0);
    }

    close(fd);
    return 0;
}
