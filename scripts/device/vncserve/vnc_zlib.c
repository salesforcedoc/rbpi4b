/*
 * vnc_zlib.c -- the deflate binding. See vnc_zlib.h for why it is hand-transcribed
 * on the unit and why that is safe.
 */
#include "vnc_zlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* zlib's own header where there is one. The Mac has it and the tests run there; the
 * unit has the runtime and not the header, which is the whole reason for the block
 * below.
 *
 * ZLIB_CONST makes z_stream's next_in a `const Bytef *`, which is what the
 * transcription below declares and what deflate has always promised -- it does not
 * write through the input pointer. Without it the two paths differ by a qualifier and
 * the compiler says so at every assignment. */
#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    define VNC_HAVE_ZLIB_H 1
#  endif
#endif

#ifdef VNC_HAVE_ZLIB_H
#  define ZLIB_CONST 1
#  include <zlib.h>
#else
/* --- the transcription ------------------------------------------------------
 *
 * This is zlib 1.x's public ABI, which the project promises it will not change --
 * "the zlib format and the documented interface of the zlib library will not be
 * modified in an incompatible way in future versions of zlib". Only the pieces
 * used here are declared.
 *
 * IT IS NOT TAKEN ON TRUST. deflateInit2_ refuses a stream whose `stream_size`
 * differs from the library's own sizeof(z_stream), and refuses one whose version
 * string does not begin with the library's version string's first character.
 * vnc_deflate_new() hands it zlibVersion() -- read out of the library itself --
 * rather than a transcribed literal, so the second check is exact and the first
 * is the one that would catch a wrong struct. Either way the answer is
 * Z_VERSION_ERROR, a NULL stream, and a session that stays on Raw. */
typedef unsigned char  Bytef;
typedef unsigned int   uInt;      /* 4 bytes on armhf and on aarch64 alike   */
typedef unsigned long  uLong;     /* 4 on the unit's armhf userland          */

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt         avail_in;
    uLong        total_in;
    Bytef       *next_out;
    uInt         avail_out;
    uLong        total_out;
    const char  *msg;
    void        *state;
    void        *zalloc;
    void        *zfree;
    void        *opaque;
    int          data_type;
    uLong        adler;
    uLong        reserved;
} z_stream;

extern const char *zlibVersion(void);
extern int deflateInit2_(z_stream *, int, int, int, int, int, const char *, int);
extern int deflate(z_stream *, int);
extern int deflateEnd(z_stream *);

#define Z_OK             0
#define Z_DEFLATED       8
#define Z_DEFAULT_STRATEGY 0
#define Z_NO_FLUSH       0
#define Z_SYNC_FLUSH     2
#endif /* VNC_HAVE_ZLIB_H */

/* zlib's worst case for a sync-flushed chunk is stored blocks, about 5 bytes per
 * 65 535 of input, plus the flush marker. A whole chunk of slack is generous by
 * three orders of magnitude and costs nothing next to the 4 MB a frame needs. */
#define VNC_DEFLATE_SLACK (64u * 1024u)

struct vnc_deflate {
    z_stream zs;
    int      ok;
    uint8_t *out;
    size_t   cap;          /* total buffer                                   */
    size_t   used;         /* bytes produced since begin()                   */
    unsigned long long in, out_total;
};

static char g_err[128];

const char *vnc_deflate_error(void)
{
    return g_err[0] ? g_err : NULL;
}

struct vnc_deflate *vnc_deflate_new(int level, size_t cap)
{
    struct vnc_deflate *d;
    int rc;

    g_err[0] = '\0';
    if (cap == 0)
        cap = VNC_DEFLATE_SLACK;
    cap += VNC_DEFLATE_SLACK + 1024;

    d = calloc(1, sizeof *d);
    if (!d) { snprintf(g_err, sizeof g_err, "out of memory"); return NULL; }
    d->out = malloc(cap);
    if (!d->out) {
        free(d);
        snprintf(g_err, sizeof g_err, "out of memory");
        return NULL;
    }
    d->cap = cap;

    /* windowBits 15 is the zlib format's maximum window, memLevel 8 the default.
     * A Tight client's inflate stream has neither of these under its control, and
     * 15 is what every encoder uses. */
    rc = deflateInit2_(&d->zs, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY,
                       zlibVersion(), (int)sizeof(z_stream));
    if (rc != Z_OK) {
        /* Z_VERSION_ERROR (-6) is the one that means the transcription above does
         * not match the library. Naming it in the log is the difference between a
         * five-minute diagnosis and an afternoon. */
        snprintf(g_err, sizeof g_err,
                 "deflateInit2_ -> %d (sizeof(z_stream)=%d; -6 is Z_VERSION_ERROR, "
                 "i.e. this build's z_stream does not match libz)", rc,
                 (int)sizeof(z_stream));
        free(d->out);
        free(d);
        return NULL;
    }
    d->ok = 1;
    return d;
}

void vnc_deflate_free(struct vnc_deflate *d)
{
    if (!d)
        return;
    if (d->ok)
        deflateEnd(&d->zs);
    free(d->out);
    free(d);
}

void vnc_deflate_begin(struct vnc_deflate *d)
{
    if (!d || !d->ok)
        return;
    /* Carry the finished chunk into the running total before losing sight of it --
     * the caller has already copied it out. */
    d->out_total += d->used;
    d->used = 0;
    d->zs.next_out  = d->out;
    d->zs.avail_out = (uInt)d->cap;
}

int vnc_deflate_feed(struct vnc_deflate *d, const uint8_t *src, size_t n)
{
    if (!d || !d->ok)
        return -1;
    if (n == 0)
        return 0;

    d->in += n;
    d->zs.next_in  = src;
    d->zs.avail_in = (uInt)n;

    while (d->zs.avail_in > 0) {
        int rc = deflate(&d->zs, Z_NO_FLUSH);
        if (rc != Z_OK) {
            snprintf(g_err, sizeof g_err, "deflate -> %d", rc);
            return -1;
        }
        if (d->zs.avail_out == 0) {
            /* Sizeable enough for a whole rectangle of incompressible pixels. The
             * only way to get here is a caller that under-sized `cap`. */
            snprintf(g_err, sizeof g_err,
                     "deflate filled its %zu-byte buffer while feeding %zu bytes",
                     d->cap, n);
            return -1;
        }
    }
    return 0;
}

const uint8_t *vnc_deflate_end(struct vnc_deflate *d, size_t *outlen)
{
    if (outlen) *outlen = 0;
    if (!d || !d->ok)
        return NULL;

    for (;;) {
        int rc = deflate(&d->zs, Z_SYNC_FLUSH);
        if (rc != Z_OK) {
            snprintf(g_err, sizeof g_err, "deflate(Z_SYNC_FLUSH) -> %d", rc);
            return NULL;
        }
        /* A sync flush is complete once it has emitted an empty stored block, which
         * it says by lowering avail_out and leaving input for nothing. The loop is
         * for the case where the flush marker straddles a full buffer; it costs one
         * extra call in the common case only when the buffer is exactly full. */
        if (d->zs.avail_out == 0) {
            snprintf(g_err, sizeof g_err,
                     "deflate filled its %zu-byte buffer while flushing", d->cap);
            return NULL;
        }
        break;
    }

    d->used = d->cap - (size_t)d->zs.avail_out;
    if (outlen) *outlen = d->used;
    return d->out;
}

void vnc_deflate_stats(const struct vnc_deflate *d, unsigned long long *in,
                       unsigned long long *out)
{
    if (in)  *in  = d ? d->in : 0;
    if (out) *out = d ? d->out_total + d->used : 0;
}
