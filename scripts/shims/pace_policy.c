/*
 * pace_policy.c — see pace_policy.h for why this is a deadline and not a sleep.
 *
 * Pure: no clock of its own, no state, no allocation. The caller supplies `now',
 * which is what lets the host test drive a thousand blocks in a microsecond and
 * assert what they cost.
 */
#include "pace_policy.h"

unsigned long long pace_next_deadline(unsigned long long now,
                                      unsigned long long period,
                                      unsigned long long deadline)
{
    /* Either there is no deadline to keep, or the caller is too far from it for
     * keeping it to be the right answer: more than a period late (repaying that
     * would run the next blocks fast) or more than a period early (a clock that
     * stepped backwards). Both restart the clock from here, which loses at most
     * that one block's time and never accumulates. */
    if (deadline == 0 || period == 0 ||
        now + period < deadline ||
        now > deadline + period)
        return now + period;

    return deadline + period;
}
