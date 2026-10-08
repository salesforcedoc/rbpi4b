/*
 * jpeg565.c -- one JPEG in, one RGB565LE frame out.
 *
 * WHY THIS EXISTS, AND WHY ffmpeg IS NOT ENOUGH. The shim wants RGB565LE and
 * chromium's screencast hands us JPEG, so something has to decode. ffmpeg decodes it
 * perfectly and still cannot be used here, for two reasons measured on the unit
 * 2026-10-04:
 *
 *   1. `ffmpeg -version` ALONE takes 337 ms on this Pi -- the whole cost of a
 *      per-frame spawn is library loading, because the decode itself is free: a
 *      1120x540 JPEG, the scale to 1120x540 and the RGB565 conversion between them
 *      add nothing measurable to that 337 ms. At 337 ms a frame the window would run
 *      at 2.4 fps and burn the CPU doing it.
 *   2. One long-lived ffmpeg cannot be fed on a pipe either. With the input on a
 *      non-seekable pipe it never leaves avformat_find_stream_info() -- it sits in
 *      anon_pipe_read with the entire JPEG already in hand, decoding nothing, until
 *      the input reaches EOF (its own debug log ends at `bytes read:33266 seeks:0`).
 *      Piping it two or three images does not help, and neither does any of
 *      -fpsprobesize 0, -analyzeduration 0, -probesize, -f mjpeg, -fflags +nobuffer
 *      or -flags +low_delay. Give it the same bytes in a FILE and it works, because
 *      a file can be rewound -- but then it is one process per frame again, back at
 *      problem 1.
 *
 * libjpeg has neither problem. It is handed one image and told nothing else: there
 * is no container to probe, no frame rate to work out and nothing to seek. A program
 * linking only libc and libjpeg starts in milliseconds, which is what makes one
 * process per frame the right answer after all.
 *
 * IT IS ALSO THE ONLY PLACE A PIXEL FORMAT IS DECIDED. rgb565le is what the shim's
 * overlay plane takes (browser_link.h and MENU_FONT_* are all 565), so the packing
 * happens here once rather than in a scaler configured to agree with it.
 *
 * USAGE
 *     jpeg565 <w> <h> <jpeg>
 *
 * writes exactly w*h*2 bytes of RGB565LE to stdout and exits 0, or writes one line
 * to stderr and exits 1.
 *
 * THE RESAMPLE IS NEAREST NEIGHBOUR, deliberately. The screencast is asked for
 * exactly the shim's frame size and measured to arrive at it (1120x540), so this is
 * a safety net for a page that renders at a different device pixel ratio -- not the
 * normal path. Nearest neighbour is a few lines, cannot blur text into mush, and is
 * honest about being a fallback rather than a promise about picture quality.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

/* libjpeg's default error handler calls exit() on a damaged image, which would take
 * the browser window down with one bad frame. A truncated JPEG is a RECOVERABLE
 * error here: the frame came from a browser that is still running, and the next one
 * is a tenth of a second away. */
struct err_jmp {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
};

static void on_error(j_common_ptr cinfo)
{
    struct err_jmp *e = (struct err_jmp *)cinfo->err;

    longjmp(e->jb, 1);
}

/* Warnings (a premature end of data, a corrupt-but-usable image) are not worth a
 * line each per frame -- the driver logs a dropped frame itself if this exits 1. */
static void on_message(j_common_ptr cinfo, int level)
{
    (void)cinfo;
    (void)level;
}

static int usage(const char *why)
{
    fprintf(stderr, "jpeg565: %s\n", why);
    fprintf(stderr, "usage: jpeg565 <w> <h> <jpeg>\n");
    return 1;
}

int main(int argc, char **argv)
{
    struct jpeg_decompress_struct cinfo;
    struct err_jmp jerr;
    FILE *fp;
    unsigned char *rgb = NULL, *out = NULL;
    size_t stride, o = 0;
    long out_w, out_h;
    int y;

    if (argc != 4)
        return usage("three arguments expected");
    out_w = strtol(argv[1], NULL, 10);
    out_h = strtol(argv[2], NULL, 10);
    if (out_w <= 0 || out_h <= 0 || out_w > 8192 || out_h > 8192)
        return usage("bad frame size");

    fp = fopen(argv[3], "rb");
    if (!fp) {
        fprintf(stderr, "jpeg565: cannot open %s\n", argv[3]);
        return 1;
    }

    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = on_error;
    jerr.mgr.emit_message = on_message;
    if (setjmp(jerr.jb)) {
        /* Anything libjpeg did not like, from here to the end of the decode. */
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        free(rgb);
        free(out);
        fprintf(stderr, "jpeg565: not a decodable JPEG\n");
        return 1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    /* Forced, not taken from the file: a greyscale JPEG would otherwise deliver one
     * component and every pixel would be read three bytes apart. */
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    stride = (size_t)cinfo.output_width * 3;
    rgb = malloc(stride * cinfo.output_height);
    out = malloc((size_t)out_w * (size_t)out_h * 2);
    if (!rgb || !out) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        fprintf(stderr, "jpeg565: out of memory\n");
        return 1;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row = rgb + stride * cinfo.output_scanline;

        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    for (y = 0; y < out_h; y++) {
        size_t sy = (size_t)y * cinfo.output_height / (size_t)out_h;
        const unsigned char *row = rgb + stride * sy;

        for (long x = 0; x < out_w; x++) {
            size_t sx = (size_t)x * cinfo.output_width / (size_t)out_w;
            const unsigned char *p = row + sx * 3;
            unsigned v = (unsigned)((p[0] & 0xf8u) << 8) |
                         (unsigned)((p[1] & 0xfcu) << 3) |
                         (unsigned)(p[2] >> 3);

            out[o++] = (unsigned char)(v & 0xffu);   /* little endian, as the plane reads it */
            out[o++] = (unsigned char)(v >> 8);
        }
    }

    if (fwrite(out, 1, o, stdout) != o) {
        fprintf(stderr, "jpeg565: short write\n");
        return 1;
    }
    free(out);
    free(rgb);
    return 0;
}
