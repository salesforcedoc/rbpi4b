/*
 * vnc_zlib.h -- deflate, for the Tight encoder's "basic compression".
 *
 * WHY THERE IS A WRAPPER AT ALL. The unit has the zlib RUNTIME and no zlib
 * HEADERS: /lib/arm-linux-gnueabihf/libz.so.1 is there and /usr/include/zlib.h is
 * not, and there is no development symlink for `-lz` either. So this file will
 * #include <zlib.h> where one exists (the Mac, where the tests run) and otherwise
 * transcribe the pieces it needs -- and the transcription is not left on trust:
 * deflateInit2_ validates BOTH the version string's first byte and
 * sizeof(z_stream) itself, and returns Z_VERSION_ERROR if either is wrong. The
 * version string handed to it is not transcribed at all but READ FROM THE
 * LIBRARY at runtime via zlibVersion(). A wrong struct here is therefore a
 * refused stream and a fall back to Raw, not a crash and not a wrong picture.
 *
 * The struct is opaque so that the one ABI-risky declaration in the project stays
 * inside vnc_zlib.c, where it is in front of a test that inflates what it
 * produces. Nothing else in the tree names z_stream.
 *
 * WHAT THIS IS FOR, measured on the unit against rbp's live 1280x800 screen
 * (2026-10-09): zlib level 1 takes the client's 32-bpp frame from 4 096 000 bytes
 * to 127 499 -- 32x -- in 45 ms. Raw was what Screen Sharing was being sent, and
 * it was being paced by it: the client asks for the next update only once it has
 * decoded and painted the last one, so a 2 MB rect bought 0.65 frames a second.
 */
#ifndef RBPI4B_VNC_ZLIB_H
#define RBPI4B_VNC_ZLIB_H

#include <stddef.h>
#include <stdint.h>

struct vnc_deflate;

/* A deflate stream that will not reset, which is what a Tight client expects: the
 * spec gives the encoder a stream per client and a reset flag per rectangle, and
 * libvncserver -- the server half of the x11vnc sessions macOS Screen Sharing joins
 * every day -- never sets the flag and relies on the stream staying live. So this
 * does the same, and one of these belongs to each client.
 *
 * `cap` is the largest compressed chunk that will ever be asked for, and it must be
 * at least (the largest uncompressed chunk + 64 KiB): deflate's worst case is
 * stored blocks, which cost about 5 bytes per 65 535.
 *
 * Returns NULL if a stream could not be initialised, in which case the caller must
 * fall back to Raw. It is not an error worth aborting over -- a screen sharing
 * session that works at 2 MB a frame is still a screen sharing session. */
struct vnc_deflate *vnc_deflate_new(int level, size_t cap);
void vnc_deflate_free(struct vnc_deflate *d);

/* ONE CHUNK, FED IN PIECES. A chunk is one rectangle's worth of compressed bytes,
 * and the pieces exist so that a rectangle never has to be held in its client's
 * pixel format all at once -- a converted 1280x800 screen is 4 MB, and a row at a
 * time is 5 KB. The compressed output still accumulates whole, because a Tight
 * rectangle carries its compressed length in its own header and so must be counted
 * before it can be written.
 *
 * vnc_deflate_begin() moves the output cursor back to the start (the caller having
 * already copied the previous chunk onto the wire), feed() appends pixels, and
 * end() sync-flushes and returns the whole chunk, valid until the next begin(). */
void vnc_deflate_begin(struct vnc_deflate *d);
int vnc_deflate_feed(struct vnc_deflate *d, const uint8_t *src, size_t n);

/* Z_SYNC_FLUSH AND NOT Z_FINISH, and not for tidiness: each rectangle carries its
 * own compressed length on the wire, so the bytes of one rectangle have to be
 * complete and countable before the next begins -- and the client's inflate stream
 * must stay open across the boundary. libvncserver's sender calls exactly this, and
 * Z_FINISH appears nowhere in it. NULL on failure. */
const uint8_t *vnc_deflate_end(struct vnc_deflate *d, size_t *outlen);

/* Bytes in and out since the stream was made -- for the log, so "is it costing the
 * player anything" has an answer that is not a guess. */
void vnc_deflate_stats(const struct vnc_deflate *d, unsigned long long *in,
                       unsigned long long *out);

/* Why a stream could not be made, for the one log line that says so. NULL if it
 * could. */
const char *vnc_deflate_error(void);

#endif /* RBPI4B_VNC_ZLIB_H */
