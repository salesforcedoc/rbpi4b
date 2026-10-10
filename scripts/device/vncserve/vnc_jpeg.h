/*
 * vnc_jpeg.h -- one composed frame to one JPEG, on the Pi's own encoder.
 *
 * WHY THIS EXISTS. A Raw rectangle is the whole screen every time: 1280x800 at 16 bpp
 * is 2 MB a frame, so four frames a second is 64 Mbit/s. On this unit that is not a
 * theoretical figure -- `eth0` is down and the operator's laptop is on `wlan0`. The
 * way out is not a better software JPEG (ffmpeg's own encoder measured 490 ms a
 * frame, which is a still image, not a screen); it is the block that is already on
 * the die. /dev/video11 is bcm2835-codec-encode, a V4L2 memory-to-memory device that
 * takes RGB565 and emits Motion-JPEG. Measured here on 2026-10-09: 1280x800, 30
 * consecutive frames, 10.7 ms each, 93 fps ceiling, 16-37 KB a frame. That is not a
 * compromise against the 9.5 ms it costs merely to read /dev/fb0.
 *
 * THE FORMAT NEEDS NO CONVERSION, and that was checked rather than assumed. The
 * driver answers S_FMT with fourcc 'RGBP' -- which reads like a substitution and is
 * not one: V4L2_PIX_FMT_RGB565 is *defined* as 'RGBP' (videodev2.h:546). A colour-bar
 * frame pushed through as fb0's own little-endian RGB565 came back with red, green,
 * blue and a green ramp in the right places, so the framebuffer's bytes go in
 * untouched.
 *
 * THERE IS NO QUALITY KNOB. V4L2_CID_JPEG_COMPRESSION_QUALITY is refused on this
 * device: the MJPG path exposes the H.264 control set instead (gop size, bitrate,
 * QP), and none of it applies. Rather than ship an option that silently does nothing,
 * there is no quality setting anywhere in this program.
 *
 * FAILS SOFT, ALWAYS. The encoder is a shared piece of the SoC and the browser's
 * own video decoder will have it too. If it is busy, or wedged, or somebody
 * unplugged it, this reports why and the caller falls back to Raw -- a screen that is
 * expensive is worth far more than a screen that is absent. A failure backs off for
 * thirty seconds before trying again, so a permanently absent encoder costs one failed
 * open every half minute rather than one per frame.
 */
#ifndef RBPI4B_VNC_JPEG_H
#define RBPI4B_VNC_JPEG_H

#include <stddef.h>
#include <stdint.h>

struct vnc_jpeg;

/* Create the handle and try once to bring the encoder up. Returns NULL only when
 * memory is exhausted -- an encoder that will not open is still a handle, because the
 * caller wants to hold on to the reason. */
/* TWO NODES CAN DO THIS JOB, and they are not the same encoder:
 *
 *   /dev/video11  bcm2835-codec-encode        the VIDEO encoder. Its MJPG path exposes no
 *                                            quality control at all -- V4L2_CID_JPEG_
 *                                            COMPRESSION_QUALITY is refused on it -- and it
 *                                            is what this unit has always used.
 *   /dev/video31  bcm2835-codec-encode_image  the IMAGE encoder. It exposes that control,
 *                                            1..100, default 80, and it STREAMS: measured on
 *                                            this unit at 1280x800 from the live screen,
 *                                            60-65 fps (15.5 ms a frame) against a 12 fps
 *                                            budget, with the knob monotonic -- 182 KB at
 *                                            quality 80 down to 72 KB at quality 20.
 *
 * THE SCALES ARE NOT COMPARABLE, which is the part that decides anything: for the same screen
 * the video encoder produces about 45 KB and the image encoder AT ITS DEFAULT 80 about 182 KB
 * -- four times the bytes for the same picture. Moving to the image encoder to gain the knob
 * therefore means choosing a quality as well, and around 10-15 is where its bytes match what
 * the video encoder gives today.
 *
 * `dev` NULL or empty means the video encoder. `quality` 0 means leave the node's own default
 * alone; a node that refuses the control keeps its default and says so in the status. */
struct vnc_jpeg *vnc_jpeg_create(int w, int h, const char *dev, int quality);
void vnc_jpeg_destroy(struct vnc_jpeg *j);

/* Is the encoder streaming right now? */
int vnc_jpeg_ok(const struct vnc_jpeg *j);

/* WHAT THIS HANDLE WAS MADE WITH, so a caller can tell whether a switch has moved under it.
 * The strings are the ones it was handed, not the ones the driver echoed back. */
const char *vnc_jpeg_dev(const struct vnc_jpeg *j);
int vnc_jpeg_quality(const struct vnc_jpeg *j);

/* One line, for the log and for the web page: either what the encoder is doing, or
 * why it is not. Never NULL. */
const char *vnc_jpeg_status(const struct vnc_jpeg *j);

/* Encode one frame of w*h RGB565 pixels.
 *
 * On success returns the JPEG's length and sets *out to point at it. THE POINTER IS
 * INTO THE ENCODER'S OWN MAPPED BUFFER and is valid only until the next call -- the
 * caller must copy anything it means to keep, which it has to do anyway because every
 * consumer here queues into its own socket buffer.
 *
 * On failure returns -1: either the encoder is standing down after a failure, or this
 * frame did not come back before `timeout_ms` (10.7 ms measured, so 200 ms is already
 * twenty times the honest worst case). The reason is in vnc_jpeg_status(). */
int vnc_jpeg_frame(struct vnc_jpeg *j, const uint16_t *px, const uint8_t **out,
                   int timeout_ms);

#endif /* RBPI4B_VNC_JPEG_H */
