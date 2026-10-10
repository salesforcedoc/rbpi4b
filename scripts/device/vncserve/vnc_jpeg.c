/*
 * vnc_jpeg.c -- the /dev/video11 memory-to-memory client. See vnc_jpeg.h.
 *
 * The whole of the V4L2 sequence is here in one place, because it has an order that
 * the driver enforces and a failure at any step leaves the device in a state the next
 * attempt must not inherit. It is: open, set both formats, request and map one buffer
 * on each side, stream both on, and then per frame queue the output, queue the
 * capture, wait, and dequeue both. Setting the capture format before the output one,
 * or streaming the capture side first, both "work" until the first poll, which is why
 * this is written down rather than discovered again.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vnc_jpeg.h"
#include "vnc_net.h"

/* WHERE THE ENCODER LIVES, AND THE TWO CANDIDATES ARE NOT THE SAME ENCODER. This is the
 * default -- /dev/video11, bcm2835-codec-encode, the VIDEO encoder -- and it is what this
 * unit has always used. /dev/video31 is a different component, bcm2835-codec-encode_image,
 * and it is the one that exposes the JPEG quality control; see vnc_jpeg.h for the measured
 * trade, which is real on both sides. */
#define VNC_JPEG_DEV      "/dev/video11"

/* How long to leave a broken encoder alone. Long enough that a device that is gone for
 * good costs nothing to keep asking about, short enough that a browser which grabbed
 * the codec and let it go is picked up again without the operator restarting
 * anything. */
#define VNC_JPEG_RETRY_MS 30000

struct vnc_jpeg {
    int fd;
    int ok;
    int w, h;
    char dev[64];                    /* which node; see VNC_JPEG_DEV above */
    int quality;                     /* 0 = leave the node's own default alone */
    char qnote[96];                  /* what actually happened to the quality, in words */

    /* The two mapped buffers. `in_map` is the RGB565 side -- the driver's copy of the
     * frame, because V4L2_MEMORY_MMAP is the only memory model this driver takes
     * reliably and USERPTR against a mapping we already own is not a fight worth
     * having for a 0.4 ms memcpy. */
    uint8_t *in_map;
    size_t in_len;
    uint8_t *out_map;
    size_t out_len;

    unsigned long long down_until;   /* monotonic ms; before this, do not retry */
    unsigned long long last_err;     /* when the last failure happened */
    char why[192];                   /* the standing reason, for the status line */
    char detail[192];                /* the most recent failure, in full */

    unsigned long long frames;
    unsigned long long failures;
    unsigned long long total_us;
    int last_us;
};

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
}

static unsigned long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000);
}

static void set_why(struct vnc_jpeg *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_why(struct vnc_jpeg *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->why, sizeof j->why, fmt, ap);
    va_end(ap);
}

static int fail(struct vnc_jpeg *j, const char *step)
{
    snprintf(j->detail, sizeof j->detail, "%s: %s", step, strerror(errno));
    return -1;
}

static void jpeg_down(struct vnc_jpeg *j);

void vnc_jpeg_destroy(struct vnc_jpeg *j)
{
    if (!j)
        return;
    jpeg_down(j);          /* streams off, unmaps, closes -- and leaves j->why alone */
    free(j);
}

/* Bring the device up from nothing. Everything above the ioctls in vnc_jpeg_frame
 * assumes this succeeded; on any failure the fd is closed and the handle is left with
 * ok == 0 and a reason in `detail`. */
static int jpeg_up(struct vnc_jpeg *j)
{
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer b;
    struct v4l2_plane planes[1];
    int t;

    j->fd = open(j->dev, O_RDWR);
    if (j->fd < 0) {
        snprintf(j->detail, sizeof j->detail, "open %s: %s", j->dev, strerror(errno));
        return -1;
    }

    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)j->w;
    f.fmt.pix_mp.height = (uint32_t)j->h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_RGB565;   /* == 'RGBP'; fb0's own layout */
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(j->fd, VIDIOC_S_FMT, &f) < 0) { fail(j, "S_FMT output RGB565"); goto bad; }
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_RGB565 ||
        f.fmt.pix_mp.width != (uint32_t)j->w || f.fmt.pix_mp.height != (uint32_t)j->h) {
        /* A driver is allowed to substitute a format it prefers. This one does not,
         * for this format, and if a kernel ever changes its mind the bytes we push
         * would be silently reinterpreted -- so it is refused rather than drawn. */
        snprintf(j->detail, sizeof j->detail,
                 "the encoder substituted its own input format (%c%c%c%c %ux%u) for the "
                 "RGB565 %dx%d asked for",
                 (char)(f.fmt.pix_mp.pixelformat & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 8) & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 16) & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 24) & 0xff),
                 f.fmt.pix_mp.width, f.fmt.pix_mp.height, j->w, j->h);
        goto bad;
    }

    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)j->w;
    f.fmt.pix_mp.height = (uint32_t)j->h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(j->fd, VIDIOC_S_FMT, &f) < 0) { fail(j, "S_FMT capture MJPG"); goto bad; }
    /* TWO NAMES FOR THE SAME BITSTREAM, and this is the line the image encoder found. The
     * video encoder answers MJPG; /dev/video31 answers JPEG -- which is what a still-image
     * encoder calls exactly the same payload. Both are baseline JPEG, which is what a preview
     * part and a Tight-JPEG rectangle both need, so either is taken. The substitution is still
     * refused for anything ELSE: a driver picking a genuinely different format at this point
     * would have us pushing bytes it will not read the way we think. */
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_MJPEG &&
        f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG) {
        snprintf(j->detail, sizeof j->detail,
                 "the encoder will not emit JPEG (it answered %c%c%c%c)",
                 (char)(f.fmt.pix_mp.pixelformat & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 8) & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 16) & 0xff),
                 (char)((f.fmt.pix_mp.pixelformat >> 24) & 0xff));
        goto bad;
    }

    /* THE QUALITY CONTROL, WHERE THE NODE HAS ONE -- and on the default node it does not.
     * The video encoder refuses V4L2_CID_JPEG_COMPRESSION_QUALITY outright, so a refusal here
     * is the ORDINARY case rather than a failure: the node's own default stands and the reason
     * is written down instead of acted on. Where it does take, it is read back, because "I set
     * it" and "the device is using it" are different claims and this string is what the page
     * shows the operator. */
    if (j->quality > 0) {
        struct v4l2_control c;

        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        c.value = j->quality;
        if (ioctl(j->fd, VIDIOC_S_CTRL, &c) < 0) {
            snprintf(j->qnote, sizeof j->qnote,
                     "quality %d not accepted on %s (%s), so the node's own default stands",
                     j->quality, j->dev, strerror(errno));
        } else if (ioctl(j->fd, VIDIOC_G_CTRL, &c) == 0) {
            snprintf(j->qnote, sizeof j->qnote, "quality %d on %s", c.value, j->dev);
        } else {
            snprintf(j->qnote, sizeof j->qnote, "quality %d on %s (not readable back)",
                     j->quality, j->dev);
        }
    } else {
        snprintf(j->qnote, sizeof j->qnote, "%s, quality left at its own default", j->dev);
    }

    /* One buffer a side. The driver granted two when asked for two and index 0 was
     * re-queueable for thirty frames, so one is enough and two megabytes is two
     * megabytes on a machine with a gigabyte. */
    memset(&rb, 0, sizeof rb);
    rb.count = 1;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(j->fd, VIDIOC_REQBUFS, &rb) < 0) { fail(j, "REQBUFS output"); goto bad; }
    if (rb.count < 1) {
        snprintf(j->detail, sizeof j->detail, "the encoder granted no output buffer");
        goto bad;
    }
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(j->fd, VIDIOC_REQBUFS, &rb) < 0) { fail(j, "REQBUFS capture"); goto bad; }
    if (rb.count < 1) {
        snprintf(j->detail, sizeof j->detail, "the encoder granted no capture buffer");
        goto bad;
    }

    memset(&b, 0, sizeof b);
    memset(planes, 0, sizeof planes);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = planes;
    b.length = 1;
    if (ioctl(j->fd, VIDIOC_QUERYBUF, &b) < 0) { fail(j, "QUERYBUF output"); goto bad; }
    j->in_len = planes[0].length;
    j->in_map = mmap(NULL, j->in_len, PROT_READ | PROT_WRITE, MAP_SHARED, j->fd,
                     (off_t)planes[0].m.mem_offset);
    if (j->in_map == MAP_FAILED) { j->in_map = NULL; fail(j, "mmap output"); goto bad; }

    memset(&b, 0, sizeof b);
    memset(planes, 0, sizeof planes);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = planes;
    b.length = 1;
    if (ioctl(j->fd, VIDIOC_QUERYBUF, &b) < 0) { fail(j, "QUERYBUF capture"); goto bad; }
    j->out_len = planes[0].length;
    j->out_map = mmap(NULL, j->out_len, PROT_READ | PROT_WRITE, MAP_SHARED, j->fd,
                      (off_t)planes[0].m.mem_offset);
    if (j->out_map == MAP_FAILED) { j->out_map = NULL; fail(j, "mmap capture"); goto bad; }

    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(j->fd, VIDIOC_STREAMON, &t) < 0) { fail(j, "STREAMON capture"); goto bad; }
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(j->fd, VIDIOC_STREAMON, &t) < 0) { fail(j, "STREAMON output"); goto bad; }

    j->ok = 1;
    j->detail[0] = '\0';
    set_why(j, "ready: %dx%d RGB565 -> Motion-JPEG, %u KB in / %u KB out, no CPU -- %s",
            j->w, j->h, (unsigned)(j->in_len / 1024), (unsigned)(j->out_len / 1024),
            j->qnote);
    return 0;

bad:
    if (j->in_map && j->in_map != MAP_FAILED) { munmap(j->in_map, j->in_len); j->in_map = NULL; }
    if (j->out_map && j->out_map != MAP_FAILED) { munmap(j->out_map, j->out_len); j->out_map = NULL; }
    close(j->fd);
    j->fd = -1;
    j->ok = 0;
    set_why(j, "%s", j->detail);
    return -1;
}

/* Give the device back, without forgetting why it went wrong. */
static void jpeg_down(struct vnc_jpeg *j)
{
    int t;
    if (j->fd >= 0) {
        t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(j->fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(j->fd, VIDIOC_STREAMOFF, &t);
        close(j->fd);
        j->fd = -1;
    }
    if (j->in_map) { munmap(j->in_map, j->in_len); j->in_map = NULL; }
    if (j->out_map) { munmap(j->out_map, j->out_len); j->out_map = NULL; }
    j->ok = 0;
}

struct vnc_jpeg *vnc_jpeg_create(int w, int h, const char *dev, int quality)
{
    struct vnc_jpeg *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
    j->fd = -1;
    j->w = w;
    j->h = h;
    snprintf(j->dev, sizeof j->dev, "%s", dev && *dev ? dev : VNC_JPEG_DEV);
    j->quality = quality > 0 ? quality : 0;
    set_why(j, "not started yet");
    if (jpeg_up(j) < 0) {
        j->down_until = now_ms() + VNC_JPEG_RETRY_MS;
        vlog("jpeg: %s -- hardware JPEG is unavailable, will try again in %d s",
             j->why, VNC_JPEG_RETRY_MS / 1000);
    } else {
        vlog("jpeg: %s", j->why);
    }
    return j;
}

int vnc_jpeg_ok(const struct vnc_jpeg *j) { return j->ok; }

const char *vnc_jpeg_dev(const struct vnc_jpeg *j) { return j->dev; }
int vnc_jpeg_quality(const struct vnc_jpeg *j) { return j->quality; }

const char *vnc_jpeg_status(const struct vnc_jpeg *j)
{
    return j->why[0] ? j->why : "no status";
}

int vnc_jpeg_frame(struct vnc_jpeg *j, const uint16_t *px, const uint8_t **out,
                   int timeout_ms)
{
    struct v4l2_buffer ob, cb;
    struct v4l2_plane op[1], cp[1];
    struct pollfd pfd;
    unsigned long long t0;
    size_t bytes;

    if (!j->ok) {
        if (now_ms() < j->down_until)
            return -1;
        if (jpeg_up(j) < 0) {
            j->down_until = now_ms() + VNC_JPEG_RETRY_MS;
            vlog("jpeg: %s -- still unavailable, next attempt in %d s", j->why,
                 VNC_JPEG_RETRY_MS / 1000);
            return -1;
        }
        vlog("jpeg: %s", j->why);
    }

    bytes = (size_t)j->w * (size_t)j->h * 2;
    if (bytes > j->in_len)
        bytes = j->in_len;
    memcpy(j->in_map, px, bytes);

    memset(&ob, 0, sizeof ob);
    memset(op, 0, sizeof op);
    ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ob.memory = V4L2_MEMORY_MMAP;
    ob.m.planes = op;
    ob.length = 1;
    ob.index = 0;
    op[0].bytesused = (uint32_t)bytes;
    op[0].length = (uint32_t)j->in_len;
    if (ioctl(j->fd, VIDIOC_QBUF, &ob) < 0) { fail(j, "QBUF output"); goto dead; }

    memset(&cb, 0, sizeof cb);
    memset(cp, 0, sizeof cp);
    cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cb.memory = V4L2_MEMORY_MMAP;
    cb.m.planes = cp;
    cb.length = 1;
    cb.index = 0;
    if (ioctl(j->fd, VIDIOC_QBUF, &cb) < 0) { fail(j, "QBUF capture"); goto dead; }

    pfd.fd = j->fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    t0 = now_us();
    {
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0 && errno == EINTR) {
            /* One interrupted poll is not a failed encode; the buffers are queued and
             * the next call will find them, so try once more with the time left. */
            r = poll(&pfd, 1, 0);
        }
        if (r < 0) { j->detail[0] = '\0'; snprintf(j->detail, sizeof j->detail,
                                                   "poll: %s", strerror(errno)); goto dead; }
        if (r == 0) {
            snprintf(j->detail, sizeof j->detail,
                     "the encoder did not return a frame within %d ms", timeout_ms);
            goto dead;
        }
    }

    memset(&ob, 0, sizeof ob);
    memset(op, 0, sizeof op);
    ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ob.memory = V4L2_MEMORY_MMAP;
    ob.m.planes = op;
    ob.length = 1;
    if (ioctl(j->fd, VIDIOC_DQBUF, &ob) < 0) { fail(j, "DQBUF output"); goto dead; }

    memset(&cb, 0, sizeof cb);
    memset(cp, 0, sizeof cp);
    cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cb.memory = V4L2_MEMORY_MMAP;
    cb.m.planes = cp;
    cb.length = 1;
    if (ioctl(j->fd, VIDIOC_DQBUF, &cb) < 0) { fail(j, "DQBUF capture"); goto dead; }

    if (cp[0].bytesused < 2 || j->out_map[0] != 0xFF || j->out_map[1] != 0xD8) {
        /* A frame that is not a JPEG is worse than no frame: it would be framed as a
         * Tight JPEG rectangle and the client would feed a decoder garbage, which is a
         * desync rather than an error. So it is checked for its start-of-image here,
         * every frame, for two byte comparisons. */
        snprintf(j->detail, sizeof j->detail,
                 "the encoder returned %u bytes that are not a JPEG (starts %02X %02X)",
                 cp[0].bytesused, cp[0].bytesused ? j->out_map[0] : 0,
                 cp[0].bytesused > 1 ? j->out_map[1] : 0);
        goto dead;
    }

    j->last_us = (int)(now_us() - t0);
    j->total_us += (unsigned long long)j->last_us;
    j->frames++;
    set_why(j, "ready: %dx%d RGB565 -> Motion-JPEG, %llu frame(s), %.1f ms each, "
               "no CPU", j->w, j->h, j->frames,
               j->frames ? (double)j->total_us / 1000.0 / (double)j->frames : 0.0);
    *out = j->out_map;
    return (int)cp[0].bytesused;

dead:
    /* The device is out of step -- buffers queued but not returned, or returned and
     * not matching. There is no way to resynchronise a V4L2 queue from the outside, so
     * the honest move is to give it back and start over later. */
    jpeg_down(j);
    j->failures++;
    j->last_err = now_ms();
    j->down_until = now_ms() + VNC_JPEG_RETRY_MS;
    set_why(j, "%s -- hardware JPEG failed; falling back to raw and retrying in %d s",
            j->detail[0] ? j->detail : "the encode failed", VNC_JPEG_RETRY_MS / 1000);
    vlog("jpeg: %s", j->why);
    return -1;
}
