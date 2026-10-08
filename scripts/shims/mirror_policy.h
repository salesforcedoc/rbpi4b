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
#ifndef RBPI4B_MIRROR_POLICY_H
#define RBPI4B_MIRROR_POLICY_H

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

/* Fill `out` with `frames` copies of the packed frame in `frame`, which is
 * `frame_bytes` long — the content of one pad, i.e. the mirror's drift correction
 * as a zero-order hold rather than as silence.
 *
 * Returns the number of frames actually placed (each one `frame_bytes` bytes), or 0
 * when there is nothing to place: a NULL or empty frame, no frames asked for, or a
 * buffer with no room for even one. `frames` is CLAMPED to what `out_bytes` holds
 * rather than the caller being trusted — this is the one place in the correction
 * that touches a caller-sized buffer, and an overrun here is a corruption that would
 * read as a mixer fault.
 *
 * Why a hold and not silence: the unit measured the pads arriving one every 5.2-5.5 s
 * at 34-38 frames (0.76-0.86 ms, ~145-157 ppm of the stream, ~11 a minute), not the
 * "one frame every 0.2 s" the arithmetic predicted. A 0.8 ms gap of *silence* is the
 * envelope dropping to zero and coming back — a tick — where holding the last
 * delivered frame costs one step, to the frame that follows the pad, and no energy
 * at all. See mirror_write_hold() in audioshim.c for the whole argument. */
unsigned long mirror_hold_fill(const unsigned char *frame, unsigned frame_bytes,
                               unsigned long frames, unsigned char *out,
                               unsigned long out_bytes);

/* The window RB_AUDIO_MIRROR_BOOST_DB is allowed to land in. A typo of one decimal
 * place is a factor of ten — "+40" instead of "+4.0" is 100x, i.e. every sample
 * pinned to a rail — so the setting is bounded rather than trusted. The top is
 * 12 dB rather than the +4 the unit runs: this is the shim's guard rail, not a
 * recommendation, and it leaves the operator room to go louder than the level the
 * headroom measurement justified without leaving room for a mis-keyed value. The
 * bottom is where "off" would be if 0 dB were not already off: below -24 dB the
 * mirror is inaudible, so nothing is lost by refusing to go further. */
#define MIRROR_BOOST_DB_MAX  12.0
#define MIRROR_BOOST_DB_MIN  (-24.0)

/* The mirror's fixed level lift, in dB, as a linear multiplier.
 *
 * This is the ONLY place the mirror is allowed to exceed unity, and it is not the
 * knob: MASTER LEVEL stays a 0..1 attenuator (flx4_mastervol_gain()'s own law,
 * pinned in test_flx4.c), so the room's control keeps the feel the operator
 * confirmed and the mirror's floor above unity comes from configuration instead.
 * The two multiply: the FLX4's knob attenuates *this* level rather than defining
 * it.
 *
 * Why the mirror alone: the knob exists because it is downstream of the FLX4's USB
 * audio, so the same music arrives at the monitor quieter than at the room's
 * output. A level that made the room louder would be a level in the master path,
 * where it would also attenuate the FLX4 — the double attenuation g_mirror_gain
 * exists to avoid.
 *
 * Why this is safe to raise past unity: the mirror's samples are saturated to the
 * domain in s24pack.h before they are packed (mirror_saturate() in audioshim.c,
 * which also counts them), so a lifted sample that runs out of headroom CLIPS.
 * Before that clamp existed the same gain WRAPPED instead — the loud, very
 * distorted failure S4.6 measured — which is why the comment beside the knob's law
 * used to say a boost was impossible until a clamp landed. It has landed; this is
 * the boost. The clamp is at the gain rather than inside s24pack() because the
 * packers are modular by contract and must stay that way; the header's
 * S24PACK_SAMPLE_* comment is the argument.
 *
 * Non-finite input answers 1.0 (unity, i.e. no change) rather than propagating: a
 * NaN here would not stay in one sample, it would multiply into every frame of the
 * mirror for the life of the process. Out-of-window input is clamped, so this
 * function cannot be the reason a unit is silent or screaming; the caller logs the
 * value it was given. */
double mirror_boost_gain(double db);

#endif /* RBPI4B_MIRROR_POLICY_H */
