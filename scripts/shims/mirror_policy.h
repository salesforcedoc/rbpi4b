/*
 * mirror_policy.h — the HDMI mirror's decisions, as pure functions.
 *
 * These live beside audioshim.c rather than inside it for the same reason
 * s24pack.c does: audioshim.c cannot be linked into a test at all (its
 * constructor dlopens libasound and it interposes the process's ALSA calls), so
 * any decision left inside it is a decision only a Pi drill can check. What is
 * here is the arithmetic and the table lookups — which candidates a device list
 * yields, what one writei() return value means, when a down device may be
 * retried — so the verdicts that would otherwise be read off a log line after
 * the fact are instead pinned on the host, with no card, no HDMI sink and no
 * rootfs.
 *
 * The two verdicts that matter most, because getting either wrong produces a
 * mirror that reads as healthy while emitting nothing:
 *
 *   written == 0      -> MIRROR_FULL, never MIRROR_WROTE. Nothing was consumed;
 *                        counting it as a write is how "up and silently dead"
 *                        gets logged as working.
 *   -EBUSY            -> MIRROR_DOWN, never MIRROR_FULL. A busy handle is not a
 *                        full ring, and classing it as "full" hides a dead
 *                        handle for the rest of the process's life.
 *
 * The one part of the mirror deliberately NOT here: the fill in flush_master().
 * It is two assignments inside rbp's per-frame loop, not a decision, and it is
 * covered by the drill in docs/13-raspberrypi4.md rather than by a unit test.
 */
#ifndef RBLIVE4_MIRROR_POLICY_H
#define RBLIVE4_MIRROR_POLICY_H

/* One ALSA device name. Shorter than audioshim.c's DEV_MAX for the master: these
 * come from a list written by hand in rb.conf, not from a probe. */
#define MIRROR_NAME_MAX 64

/* Enough for both HDMI cards in both their hw: and plughw: forms, with room to
 * spare. A list longer than this is truncated, counted and logged, never
 * silently ignored. */
#define MIRROR_CAND_MAX 8

/* Split `list` on whitespace into candidate device names, in order.
 *
 * A `hw:` entry also yields its `plughw:` twin immediately after it, so a sink
 * that will not take the exact format still works through libasound's
 * conversion — the same fallback load_config() builds for the master's single
 * AUDIO_DEV, applied per entry here because each entry is one port. A `plughw:`
 * or `default` entry yields only itself: a twin of a twin is the same device
 * twice.
 *
 * Returns the number of names stored (0 for NULL, "" or all-whitespace, which is
 * how the mirror is turned off). `*dropped` receives the number of entries that
 * were NOT stored — too long for a name, no room left, or a repeat of a name
 * already in the list — so the caller can say so instead of appearing to have
 * tried them. May be NULL.
 */
int mirror_candidates(const char *list, char (*out)[MIRROR_NAME_MAX], int out_max,
                      int *dropped);

/* What one snd_pcm_writei() on the mirror returned, in the terms the shim acts
 * on. The values are the call's two arguments, not just its return, because
 * "wrote all of it" and "wrote some of it" are different outcomes for a
 * non-blocking handle and only the pair distinguishes them. */
enum mirror_verdict {
    MIRROR_WROTE = 0,  /* written == frames: the block went out */
    MIRROR_SHORT,      /* 0 < written < frames: some of it went out */
    MIRROR_FULL,       /* nothing consumed, nothing wrong: drop the block */
    MIRROR_RETRY,      /* the stream needs preparing: prepare, then write once more */
    MIRROR_DOWN        /* a hard error: close it, count a loss, retry later */
};

enum mirror_verdict mirror_verdict(long written, long frames);

/* For the log line. Never NULL. */
const char *mirror_verdict_name(enum mirror_verdict v);

/* Whether a down mirror may be retried now. `reopen_ms` == 0 means the operator
 * turned retries off (RB_AUDIO_MIRROR_REOPEN_MS), and `last_ms` == 0 means there
 * is no attempt to count from; both answer "no", and neither can be folded into
 * the other, because a mirror that never comes up at all must not retry every
 * block.
 *
 * `last_ms` is the LAST ATTEMPT, not the start of the outage, and the caller has
 * to keep it that way: it is restamped after every failed walk (and at the moment
 * the device is lost). The distinction is the whole difference between a backoff
 * and a permanent green light. Stamp it once and `now - stamp >= backoff` is true
 * for the rest of the process's life after the first interval, so nothing bounds
 * the retry any more and it runs on every audio block — which is what this unit
 * measured before the restamp was added: a 500 ms-to-5 s backoff producing ~2000
 * opens and closes per second and 121k log lines in 28 s. Both stamps are now_ms()
 * readings. */
int mirror_reopen_due(unsigned long long now_ms, unsigned long long last_ms,
                      unsigned long backoff_ms, unsigned long reopen_ms);

/* The next wait after a failed attempt: double, capped at the ceiling the
 * operator set. Called only with a starting backoff in hand, so the zero case is
 * defensive — a backoff of 0 would mean retrying on every block. */
unsigned long mirror_next_backoff(unsigned long backoff_ms, unsigned long reopen_ms);

/* How many frames of silence to insert into the mirror's ring to hold `target`
 * frames in it.
 *
 * `avail` is snd_pcm_avail_update()'s free space on that ring, `buffer` its size,
 * and `max_pad` the ceiling on one correction. The result is a whole number of
 * frames in [0, max_pad] — the caller writes that much silence before the master's
 * block, which is how the mirror absorbs the two clocks differing (measured on this
 * unit: the vc4 HDMI sink consumes ~6.9 frames a second faster than the FLX4, i.e.
 * ~155 ppm, which drains a 512-frame lead in ~74 s).
 *
 * A non-positive `avail` is a reading that cannot be trusted — snd_pcm_avail_update()
 * answers -EPIPE on a broken stream and the shim may not have resolved it at all —
 * and answers 0, i.e. "write nothing", never "pad by everything". `buffer` or
 * `target` of 0 is likewise 0: an unset geometry must not turn into a burst of
 * silence.
 *
 * This is the twin of the full-ring drop, and the two are what "no resampler here"
 * means in practice: the ring is held near half full, so drift in either direction
 * costs a few frames of padding or a dropped block rather than a stream restart. */
unsigned long mirror_pad_frames(long avail, unsigned long buffer,
                                unsigned long target, unsigned long max_pad);

#endif /* RBLIVE4_MIRROR_POLICY_H */
