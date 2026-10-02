/*
 * pace_policy.h — the block clock, for a unit whose card is gone.
 *
 * With no master card there is nothing to be rbp's clock: the audio thread writes
 * a block, and the only thing that can make that block take one block's time is
 * the sleep this shim takes on its behalf. What is here is that arithmetic and
 * nothing else, beside audioshim.c for the reason mirror_policy.h gives — the shim
 * cannot be linked into a test, so anything left inside it can only be read off a
 * log after a drill. A rate especially: every counter in the shim counts content,
 * and a clock is not content, so a wrong one is invisible to all of them and
 * audible to the operator.
 *
 * The rule is a DEADLINE per block, not a sleep per block. usleep() sleeps at
 * least its argument — wakeup latency is on top of it — and the block also has
 * work in it, so a sleep of a whole period makes every block cost
 * period + overhead and the engine's rate becomes period + overhead.
 *
 * Measured on the unit with the FLX4 away, and the same overhead is in the
 * pre-fix numbers, which is what makes the split legible: three sleeps per block
 * gave 220 blocks/s, and 3 x (1451.247 us + 64 us) is 4545 us, i.e. 220.0/s — so
 * one sleep carries about 64 us of wakeup latency on top of its argument. With
 * one sleep per block that leaves 617.9 blocks/s rather than 689, 89.7 % of real
 * time: 167 us of overhead on a 1451.247 us block, 64 us of it the sleep's own
 * and ~103 us the block's work.
 *
 * The engine's rate is not a small pitch error, because the HDMI mirror is fed one
 * block per master block and so inherits it exactly, and the mirror says so in its
 * own counters: with the card away its ring fell 7.38 frames per 64-frame block,
 * and its drift correction (mirror_pad(), sized for the ~6.9 frames a second the
 * two crystals differ by) topped it up with ~4560 frames a second of silence
 * against the 6.9 the ring needs with a card open — 660x, and 10.3 % of the
 * monitor's timeline replaced by holes of ~0.17 ms about a hundred times a second.
 * That is the "slow and distorted" the operator reported: a block clock 10.3 %
 * slow, heard as music with silence interleaved into it.
 *
 * So: the deadline advances one period per block and the caller sleeps only the
 * remainder. Work and wakeup come out of the block's own period, and the long-run
 * rate is the period exactly, whatever the per-block cost is.
 *
 * The one thing a deadline cannot do is run a block late for free: a block that
 * overruns (a reopen attempt, a stall, a clock that stepped) leaves the deadline
 * in the past, and paying that back would run the next blocks FAST. Running fast
 * is the worse artefact — it is the mirror's ring overflowing rather than
 * draining, and it cannot be corrected by silence — so the deadline is clamped to
 * at most one period behind `now', and a late block is written off instead. The
 * caller therefore gets a rate that is exactly right while the overhead is under a
 * period (12x of margin at the measured 0.19 ms) and, past that, the fastest rate
 * the unit can actually hold, never a burst.
 */
#ifndef RBLIVE4_PACE_POLICY_H
#define RBLIVE4_PACE_POLICY_H

/*
 * The next block's deadline, in nanoseconds on the caller's clock.
 *
 *   now       the caller's clock now
 *   period    the block's own duration (its frames / 44100, in ns)
 *   deadline  the previous deadline, or 0 if there has not been one
 *
 * Returns `deadline + period' in the ordinary case, so the caller sleeps
 * `deadline + period - now' and the work it has just done comes out of the block's
 * own period.
 *
 * Returns `now + period' when there is no deadline to keep (the first block, or
 * `deadline' is 0) and when the caller has moved further than a period from it in
 * either direction: later than a period (a stall, which must not be repaid as a
 * burst of fast blocks) or before `deadline - period' (a clock that stepped
 * backwards, which must not become a sleep of that length).
 *
 * A `period' of 0 is not reachable from the write path — snd_pcm_writei() returns
 * before this for a zero-frame call, and one frame is 22675 ns — and would mean
 * there is no block to keep time for; it is answered as `now', i.e. nothing to
 * wait for, rather than by dividing by it.
 */
unsigned long long pace_next_deadline(unsigned long long now,
                                      unsigned long long period,
                                      unsigned long long deadline);

#endif /* RBLIVE4_PACE_POLICY_H */
