/*
 * master_policy.c — see master_policy.h for why this is separate from the shim,
 * and for the failure the two verdicts come from.
 *
 * Nothing here allocates, reads a clock, opens a device or touches a global:
 * every input is an argument, which is what lets test_audio.c pin it on the host.
 * The errno values are the only libc dependency, and they are what the kernel
 * actually returns from a writei().
 */
#include "master_policy.h"

#include <errno.h>
#include <string.h>

static int already_listed(char (*out)[MASTER_NAME_MAX], int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(out[i], name) == 0)
            return 1;
    return 0;
}

/* Store one candidate, or count it as dropped. The capacity test comes first so
 * that a full list never indexes out[] at *n, and a name too long for the array
 * is dropped rather than truncated: a truncated device name is a name that does
 * not exist, and the failure would read as "the sink refused to open".
 *
 * An absent name — NULL or "" — is NOT a dropped candidate. The caller passes ""
 * for the plug twin precisely when there is none, which is the documented way to
 * say "one device only"; counting it would put a lie in the log on every unit
 * whose AUDIO_DEV is already a plug device, and on every unit that has an
 * AUDIO_DEV at all and no twin to go with it. *dropped is for names that were
 * given and could not be kept. (Same reading as the mirror's own list, whose
 * empty and whitespace-only inputs drop nothing.) */
static void emit(char (*out)[MASTER_NAME_MAX], int out_max, int *n, int *dropped,
                 const char *name)
{
    size_t len;

    if (name == NULL || name[0] == '\0')
        return;
    if (already_listed(out, *n, name)) {
        (*dropped)++;
        return;
    }
    len = strlen(name);
    if (len >= MASTER_NAME_MAX || *n >= out_max) {
        (*dropped)++;
        return;
    }
    memcpy(out[*n], name, len + 1);
    (*n)++;
}

int master_candidates(const char *dev, const char *plug,
                      char (*out)[MASTER_NAME_MAX], int out_max, int *dropped)
{
    int n = 0, d = 0;

    if (out_max < 0)
        out_max = 0;
    emit(out, out_max, &n, &d, dev);
    emit(out, out_max, &n, &d, plug);

    if (dropped) *dropped = d;
    return n;
}

/* The order of these tests is the whole content of this function.
 *
 * -EAGAIN is not a fault. The handle rbp holds may genuinely be non-blocking —
 * open_real_device() clears only SND_PCM_ASYNC from the mode rbp asked for — and
 * a full ring answers this. Anything that counted it would retire a healthy card.
 *
 * -EPIPE (a broken stream) and its siblings are the only errors worth one
 * prepare-and-retry, because they are what a stream returns after an underrun on
 * a device that is still there. The caller does that retry; what reaches the
 * counter is the block's FINAL verdict, so a second -EPIPE counts (see the header).
 *
 * Everything else is MASTER_DOWN, including written == 0 with frames > 0: for a
 * blocking master write, "nothing was delivered" is not a full ring and not a
 * write, and only a delivery proves the handle is up. This is the one reading
 * that differs from the mirror's, deliberately — see the enum's comment. */
enum master_verdict master_verdict(long written, long frames)
{
    if (written == -EAGAIN || written == -EWOULDBLOCK)
        return MASTER_FULL;
    if (written == -EPIPE || written == -ESTRPIPE || written == -EBADFD)
        return MASTER_RETRY;
    if (written > 0 && written < frames)
        return MASTER_SHORT;
    if (written == frames)
        return MASTER_WROTE;
    return MASTER_DOWN;
}

const char *master_verdict_name(enum master_verdict v)
{
    switch (v) {
    case MASTER_WROTE: return "wrote";
    case MASTER_SHORT: return "short";
    case MASTER_FULL:  return "full";
    case MASTER_RETRY: return "retry";
    case MASTER_DOWN:  return "down";
    }
    return "?";
}

unsigned long long master_fail_frames_next(unsigned long long have,
                                           unsigned long frames,
                                           enum master_verdict v)
{
    switch (v) {
    case MASTER_WROTE:
    case MASTER_SHORT:
    case MASTER_FULL:
        /* Audio was delivered, or the ring was full because the card is taking
         * it. Either way this is not a handle that is failing to consume. */
        return 0;
    case MASTER_RETRY:
    case MASTER_DOWN:
        break;
    }

    /* Saturating, so a failure long enough to overflow comes back as "still
     * dead" rather than wrapping round to healthy. */
    if (have > ~0ULL - (unsigned long long)frames)
        return ~0ULL;
    return have + frames;
}

int master_is_dead(unsigned long long fail_frames, unsigned long long limit)
{
    return limit != 0 && fail_frames >= limit;
}
