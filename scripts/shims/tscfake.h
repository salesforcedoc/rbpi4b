/*
 * tscfake.h — the fake /dev/tsc2007_2-0048 touch controller.
 *
 * This is the half of the old fbshim-tsc.c that faces *rbp*, and it is
 * target-independent: rbp was built against a board with a tsc2007 on SPI, and
 * it will ask for that device on a Raspberry Pi exactly as it did on an SC Live
 * 4. Nothing in this file may learn about where the touch data comes from — that
 * is pointsrc.c's job — because every byte here is rbp ABI.
 *
 * The contract, byte for byte:
 *
 *   ioctl(fd, 0x80046b00, &u32)  -> u32 = 3      (max X)
 *   ioctl(fd, 0x40046b00, &u32)  -> accepted
 *   ioctl(fd, 0x80026b01, &u16)  -> u16 = 3900   (max Y)
 *   ioctl(fd, 0x40026b01, &u16)  -> accepted
 *   read(fd, buf, 6)             -> { u8 flag; u8 pad; u16 x; u16 y } LE
 *
 * with x/y *already in rbp's logical 1280x800 space* — the scaling happens
 * before the record is built, so x can legitimately exceed TSC_MAX_X. That looks
 * wrong and is not: the "max" the ioctls report is what rbp uses to size its
 * calibration, and rbp applies its own (patched) calibration to the raw values,
 * so the two are deliberately not the same scale.
 */
#ifndef RBLIVE4_TSCFAKE_H
#define RBLIVE4_TSCFAKE_H

#include <sys/types.h>

#define TSC_DEVICE_PATH "/dev/tsc2007_2-0048"

#define TSC_MAX_X 3
#define TSC_MAX_Y 3900

#define TSC_RECORD_LEN 6

/* Concurrent opens rbp may hold on the fake device. Carried over from the
 * original shim, which also held 64; rbp opens it once and dups it. */
#define TSC_MAX_FDS 64

/* Open the fake device. Returns an fd rbp can read, or -1 if the pipe behind it
 * could not be created. Always succeeds once the pipe exists, even with no
 * pointer attached: rbp does not degrade gracefully when this device is missing,
 * so "no touchscreen" has to mean "a device that never reports", not "no
 * device". Starting the pointer source is attempted here and retried by
 * pointsrc itself if it fails. */
int tscfake_open(void);

/* Is this fd one of ours? Used by the read/close interpositions in fb_shim.c to
 * route those calls to the pipe. */
int tscfake_is_fd(int fd);

/* read()/close() for a fake fd. */
ssize_t tscfake_read(int fd, void *buf, size_t count);
int tscfake_close(int fd);

/* The tsc2007 ioctls. Returns 1 when the request was one of ours and was
 * answered, 0 when the caller should pass it to the real kernel. `arg` may be
 * NULL, which the read directions tolerate. */
int tscfake_ioctl(unsigned long request, void *arg);

/* Encode one record. Pure, so the unit test can check the exact bytes without a
 * pipe, a thread or a device. */
void tscfake_record(int down, int x, int y, unsigned char out[TSC_RECORD_LEN]);

/* Publish a pointer state to rbp. Applies rbp's two quirks, both properties of
 * the *consumer* and therefore kept here rather than in pointsrc:
 *
 *  - duplicates are suppressed, because a stream of identical records makes
 *    rbp's touch thread do work for no change;
 *  - a transition from up to down is sent as a burst of two frames, because
 *    rbp's TouchAdValueHysteresis() discards the first frame after a gap as
 *    debounce. Sending one frame would make the first tap of every gesture
 *    invisible.
 */
void tscfake_emit(int down, int x, int y);

#endif /* RBLIVE4_TSCFAKE_H */
