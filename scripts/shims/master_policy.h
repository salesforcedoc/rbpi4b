/*
 * master_policy.h — the master's device chain and its write verdicts, as pure
 * functions.
 *
 * Beside audioshim.c rather than inside it for exactly the reason
 * mirror_policy.h gives: audioshim.c cannot be linked into a test at all (its
 * constructor dlopens libasound and it interposes the process's ALSA calls), so
 * a decision left inside it is a decision only a drill on the Pi can check. What
 * is here is a table lookup and a counter, so the two verdicts that produced the
 * worst failure this port has had are pinned on the host instead of being read
 * off a log after the fact.
 *
 * The failure they come from, measured on the unit 2026-09-27: with no FLX4 on
 * the bus the master's chain ended in the bare name `default`, which on this Pi
 * is card 0 `bcm2835 Headphones` — a device that OPENS (`res=0`) and takes
 * hw_params, and then fails EVERY write with -EINVAL. The shim therefore believed
 * the master was up and never took its cardless path: rbp's recovery loop spun
 * prepare → write(-22) → prepare ~29k times a second (13M failures), put 449 MB
 * into /tmp in eight minutes, and froze the player's whole UI — it read touch and
 * keyboard and painted nothing, which is what "the unit is dead, nothing responds,
 * not even the touchscreen" turned out to mean. Attaching the FLX4 afterwards did
 * not help, because only -ENODEV was acted on. So:
 *
 *   the chain may only name the card AUDIO_DEV names — anything else is a device
 *   whose channel count gets latched into the pair map, which the map's indices
 *   were never resolved against. This is why master_candidates() never adds a
 *   device of its own: that property is what stops `default` coming back.
 *
 *   a successful OPEN is not a working device. The master is up only once a write
 *   has delivered frames — the rule mirror_policy.h already states for the mirror
 *   ("considered UP only once a write has actually delivered frames"). MASTER_DOWN
 *   is what makes a handle that opens and refuses everything get retired, so the
 *   cardless path (paced, silent, and returning success) takes over and a card
 *   that appears later is found by reopen_try().
 */
#ifndef RBLIVE4_MASTER_POLICY_H
#define RBLIVE4_MASTER_POLICY_H

/* One ALSA device name. Must hold DEV_MAX (160, audioshim.c) plus the eight bytes
 * of "plughw:" that the caller prepends to a hw: device. */
#define MASTER_NAME_MAX 176

/* The configured device, its plughw: twin, and nothing. The array is sized one
 * larger than the chain can be so that a reintroduced third rung is a dropped
 * candidate the log names rather than a silent overwrite. */
#define MASTER_CAND_MAX 3

/* How much audio a handle may fail to consume before it is treated as dead —
 * one second at the master's rate.
 *
 * Derived rather than picked, and in frames because the file's thresholds are
 * frames (g_startup_frames_done, the startup mute's ms * 44100 / 1000). The
 * FLX4's ring is 64 frames x 2 periods (2.9 ms) and a healthy write is absorbed
 * in 1.45 ms, so the counter can only reach this by BOTH attempts on a block
 * failing, over and over: a card that is merely slow blocks, it does not fail.
 * Under the measured -EINVAL spin it is reached in 690 consecutive failed blocks,
 * i.e. ~24 ms of wall clock; under a card that delivers anything at all it is
 * never reached, because every delivery resets the count.
 *
 * A limit of 0 disables the rule (master_is_dead()'s own zero case, the same
 * idiom as AUDIO_MIRROR_REOPEN_MS=0). */
#define MASTER_DEAD_FRAMES 44100UL

/* The master's device chain, in order: `dev` (AUDIO_DEV), then `plug` (its
 * plughw: form, or "" — the caller builds that string, because it is a string
 * edit on dev and the caller owns the bounds).
 *
 * Nothing is emitted that the caller did not name. The plughw twin is kept
 * because it is the SAME physical card, which is the whole justification for it:
 * a card that will not take our format directly is still the card, whereas
 * anything else would be a device the pair map was never resolved against. (Note
 * the twin's own residual trap: on a plug device the card reports an arbitrary
 * channel count, which the shim clamps to its own ceiling, and that is the shape
 * of the historical -EINVAL spin. It is contained from the other side now, by
 * master_verdict() retiring a handle that will not carry a stream.)
 *
 * Returns the number of names stored — 0 for NULL or "". `*dropped` receives the
 * names NOT stored (too long, no room, or a repeat), so the caller can say so
 * rather than appear to have tried them. An ABSENT name (NULL or "") is not a
 * drop: "" is how the caller says "no twin", and counting it would put a lie in
 * the log on every unit whose AUDIO_DEV is already a plug device. May be NULL. */
int master_candidates(const char *dev, const char *plug,
                      char (*out)[MASTER_NAME_MAX], int out_max, int *dropped);

/* What one snd_pcm_writei() on the master returned, in the terms the shim acts
 * on. Deliberately a separate enum from mirror_verdict() rather than a shared
 * one, because exactly one reading differs and the difference is the point:
 *
 *   the mirror's written == 0 is MIRROR_FULL — "nothing consumed, but nothing is
 *   wrong, so do not count it as a write". For the master nothing was delivered,
 *   so the handle is not carrying the stream, which is MASTER_DOWN. (The two are
 *   pinned against each other in test_audio.c so the divergence stays deliberate.)
 *
 * -EAGAIN is MASTER_FULL and must never be a fault. audioshim.c's
 * open_real_device() clears only bit 0x2 (SND_PCM_ASYNC) from the mode rbp asked
 * for, so rbp's SND_PCM_NONBLOCK (0x1) survives onto the handle and a full ring
 * answers -EAGAIN. Counting that as a failure would retire a perfectly healthy
 * card on its first full ring. */
enum master_verdict {
    MASTER_WROTE = 0,  /* written == frames: the block went out */
    MASTER_SHORT,      /* 0 < written < frames: audio was delivered; the handle is up */
    MASTER_FULL,       /* -EAGAIN/-EWOULDBLOCK: a full ring, not a fault */
    MASTER_RETRY,      /* -EPIPE/-ESTRPIPE/-EBADFD: prepare, then write once more */
    MASTER_DOWN        /* a hard error: this handle is not carrying the stream */
};

enum master_verdict master_verdict(long written, long frames);

/* For the log line. Never NULL. */
const char *master_verdict_name(enum master_verdict v);

/* The consecutive-failure counter, advanced by one block's FINAL outcome.
 *
 * "Final" matters: the caller has already done its single prepare-and-rewrite, so
 * a MASTER_RETRY arriving here is a stream that was prepared and still would not
 * take the block — it counts, and that is what stops a card stuck in a retryable
 * error loop from reproducing the -EINVAL spin under a different errno. Anything
 * that delivered audio (MASTER_WROTE, MASTER_SHORT) or was consumed by a working
 * ring (MASTER_FULL) resets the count to 0: a card that delivers one block in 690
 * is not dead.
 *
 * Saturates rather than wrapping, so a long failure cannot come back round to
 * "healthy"; the limit itself is not needed here. */
unsigned long long master_fail_frames_next(unsigned long long have,
                                           unsigned long frames,
                                           enum master_verdict v);

/* Whether that counter has run out. A limit of 0 disables the rule. */
int master_is_dead(unsigned long long fail_frames, unsigned long long limit);

#endif /* RBLIVE4_MASTER_POLICY_H */
