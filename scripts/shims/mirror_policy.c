/*
 * mirror_policy.c — see mirror_policy.h for why this is separate from the shim,
 * and for the two verdicts that are load-bearing.
 *
 * Nothing here allocates, reads a clock, opens a device or touches a global:
 * every input is an argument, which is what lets test_audio.c pin it on the host.
 * The errno values are the only libc dependency, and they are what the kernel
 * actually returns from a non-blocking snd_pcm_writei(). pow() is the one other
 * external call, in mirror_boost_gain(): a dB is what an operator sets and a
 * multiplier is what the mixer needs, and a table of the values anyone would
 * plausibly set would be both longer and less exact than the one line here.
 */
#include "mirror_policy.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* One whitespace-separated token. Advances *pp past it, and returns 0 when the
 * list is exhausted. A token too long for `out` sets *over instead of being
 * truncated: a truncated device name is a name that does not exist, and the
 * failure would read as "the sink refused to open". */
static int next_token(const char **pp, char *out, size_t cap, int *over)
{
    const char *p = *pp;
    size_t n = 0;

    *over = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' &&
           p[n] != '\n' && p[n] != '\r')
        n++;
    if (n >= cap) {
        *over = 1;
    } else {
        memcpy(out, p, n);
        out[n] = '\0';
    }
    *pp = p + n;
    return 1;
}

static int already_listed(char (*out)[MIRROR_NAME_MAX], int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(out[i], name) == 0)
            return 1;
    return 0;
}

/* Store one candidate, or count it as dropped. The out_max test comes first so
 * that a full list never indexes out[] at *n. */
static void emit(char (*out)[MIRROR_NAME_MAX], int out_max, int *n, int *dropped,
                 const char *name)
{
    if (*n >= out_max || already_listed(out, *n, name)) {
        (*dropped)++;
        return;
    }
    strcpy(out[*n], name);
    (*n)++;
}

int mirror_candidates(const char *list, char (*out)[MIRROR_NAME_MAX], int out_max,
                      int *dropped)
{
    const char *p = list;
    char name[MIRROR_NAME_MAX];
    char plug[MIRROR_NAME_MAX];
    int n = 0, over = 0, d = 0;

    if (list == NULL) {
        if (dropped) *dropped = 0;
        return 0;
    }

    while (next_token(&p, name, sizeof name, &over)) {
        if (over) {
            d++;
            continue;
        }
        emit(out, out_max, &n, &d, name);

        /* The plughw: twin of a hw: entry, immediately after it. sizeof is used
         * rather than a literal 64 so the capacity test cannot drift from the
         * array it protects. */
        if (strncmp(name, "hw:", 3) == 0) {
            if (strlen(name) + sizeof("plughw:") <= sizeof plug) {
                strcpy(plug, "plughw:");
                strcpy(plug + 7, name + 3);
                emit(out, out_max, &n, &d, plug);
            } else {
                d++;
            }
        }
    }

    if (dropped) *dropped = d;
    return n;
}

/* The order of these tests is the whole content of this function.
 *
 * -EBUSY is deliberately not with -EAGAIN: the FLX4's own path shows that a card
 * that has been taken away and not yet released answers that way, and a handle
 * classed as "merely full" is never closed, never counted and never recovered —
 * it sits in the log looking healthy for the rest of the process's life.
 *
 * -EPIPE (a broken stream) and its siblings are the only errors worth one
 * prepare-and-retry, because they are the errors a stream returns after an
 * underrun on a device that is still there. */
enum mirror_verdict mirror_verdict(long written, long frames)
{
    if (written == -EAGAIN || written == -EWOULDBLOCK)
        return MIRROR_FULL;
    if (written == -EPIPE || written == -ESTRPIPE || written == -EBADFD)
        return MIRROR_RETRY;
    if (written > 0 && written < frames)
        return MIRROR_SHORT;
    if (written == 0 && frames > 0)
        return MIRROR_FULL;
    if (written == frames)
        return MIRROR_WROTE;
    return MIRROR_DOWN;
}

const char *mirror_verdict_name(enum mirror_verdict v)
{
    switch (v) {
    case MIRROR_WROTE: return "wrote";
    case MIRROR_SHORT: return "short";
    case MIRROR_FULL:  return "full";
    case MIRROR_RETRY: return "retry";
    case MIRROR_DOWN:  return "down";
    }
    return "?";
}

int mirror_reopen_due(unsigned long long now_ms, unsigned long long last_ms,
                      unsigned long backoff_ms, unsigned long reopen_ms)
{
    if (reopen_ms == 0 || last_ms == 0)
        return 0;
    if (now_ms < last_ms)
        return 0;
    return (now_ms - last_ms) >= (unsigned long long)backoff_ms;
}

unsigned long mirror_next_backoff(unsigned long backoff_ms, unsigned long reopen_ms)
{
    if (reopen_ms == 0)
        return 0;
    if (backoff_ms == 0 || backoff_ms > reopen_ms)
        return reopen_ms;
    if (backoff_ms > reopen_ms / 2)
        return reopen_ms;          /* written this way so the doubling cannot wrap */
    return backoff_ms * 2;
}

unsigned long mirror_pad_frames(long avail, unsigned long buffer,
                                unsigned long target, unsigned long max_pad)
{
    unsigned long level;

    /* A negative reading is "no usable level", not "the ring is empty": -EPIPE on
     * a broken stream and -ENOSYS from an unresolved symbol both land here, and
     * neither is a reason to write a burst of silence into a live device. */
    if (avail < 0 || buffer == 0 || target == 0 || max_pad == 0)
        return 0;
    if (target > buffer)                 /* a target outside the ring cannot be met */
        target = buffer;
    level = (unsigned long)avail >= buffer ? 0 : buffer - (unsigned long)avail;
    if (level >= target)                 /* at or above it: the drop side handles it */
        return 0;
    /* One correction is bounded: the deficit is worked off over the blocks that
     * follow rather than in a single burst of silence. */
    if (target - level > max_pad)
        return max_pad;
    return target - level;
}

unsigned long mirror_hold_fill(const unsigned char *frame, unsigned frame_bytes,
                               unsigned long frames, unsigned char *out,
                               unsigned long out_bytes)
{
    unsigned long fits, i;

    /* Nothing to repeat, nowhere to put it, or no frame to repeat: all three are the
     * caller's fallback (silence) rather than an error, so all three answer 0. */
    if (frame == NULL || out == NULL || frame_bytes == 0 || frames == 0)
        return 0;
    fits = out_bytes / frame_bytes;
    if (fits == 0)
        return 0;
    if (frames > fits)
        frames = fits;          /* clamped to the buffer, never trusted past it */
    for (i = 0; i < frames; i++)
        memcpy(out + i * (unsigned long)frame_bytes, frame, frame_bytes);
    return frames;
}

double mirror_boost_gain(double db)
{
    /* NaN is the one input that must not propagate: it would not stay in the sample
     * that produced it, it would multiply into every frame of the mirror for the
     * rest of the process's life, and the only symptom would be silence. Infinity
     * is caught by the window below like any other out-of-range value. `db != db`
     * rather than isnan() so this file keeps its no-<math.h> include list. */
    if (db != db)
        return 1.0;
    if (db > MIRROR_BOOST_DB_MAX)
        db = MIRROR_BOOST_DB_MAX;
    if (db < MIRROR_BOOST_DB_MIN)
        db = MIRROR_BOOST_DB_MIN;
    return pow(10.0, db / 20.0);
}
