/*
 * audioshim.c — rbp's ALSA contract, put onto a DDJ-FLX4 on a Pi 4.
 *
 * rbp was built for the XDJ-RX3's audio path and opens ALSA in a fixed order:
 * three stereo playback streams (master, phone, booth) and a dummy capture. It
 * believes each is a 2-channel S24_LE 44.1 kHz device, and it never checks. This
 * shim keeps that belief intact and puts the audio where rb.conf says:
 *
 *   stream 0  master       -> AUDIO_MAP's master pair      (default 0,1)
 *   stream 1  phone / cue  -> AUDIO_MAP's headphones pair  (default 2,3)
 *   stream 2  booth        -> AUDIO_MAP's booth pair       (default: dropped)
 *   stream 3+              -> discarded
 *   capture                -> silence, forever
 *
 * There is one real device and one real stream, so all pairs are written in a
 * single snd_pcm_writei() on the master stream: the phone and booth streams stage
 * their last block, and the master's write flushes the whole frame to the card.
 * That is also why the audio clock is the master's.
 *
 * ---- What changed for the Pi (the SC Live 4's version is in git history) ----
 *
 *  - The device is AUDIO_DEV, default hw:CARD=DDJFLX4,DEV=0, instead of a
 *    hardcoded "hw:1,0". hw: and not plughw:, and the reason is the next bullet:
 *    AUDIO_MAP indexes *hardware* channels, so this shim has to learn the card's
 *    real channel count, and a plughw device will not tell it. The plug layer
 *    exists to make the logical channel count arbitrary and reports what it can
 *    accept — 10000 on this unit — against the card's actual 4. Since there is
 *    then no plug chain to convert anything, AUDIO_FMT (default s24_3le) names
 *    the format the card really wants and s24pack.c packs it. The FLX4 takes
 *    44100 natively, so nothing needs resampling.
 *  - The channel count is negotiated from the real card (AUDIO_CHANNELS=auto) and
 *    clamped to the 8 the output buffer holds, instead of being forced to 8. The
 *    rule is to ask for exactly as many channels as the card has, so that this
 *    shim's frame *is* the card's frame and the map's indices mean what they say.
 *  - Stream->pair routing is a table (AUDIO_MAP) rather than index arithmetic on
 *    a fixed 8-channel frame.
 *  - The built-in-speaker pair is gone — a Pi has no internal speakers — so
 *    g_speaker_gain/g_speaker_on have no consumer here (see shmstate.h).
 *  - The cue mix/level knobs are *not* applied here. rbp's phone stream already
 *    contains cue + master, mixed internally and time-aligned; summing the
 *    separate streams on this side combed on the SC Live 4, which is why the old
 *    code routed it straight through and left cue_mix/cue_gain unread. The knobs
 *    therefore belong on rbp's own mixer engine, via the controls bridge — that
 *    is M3b's job, and this note is the handoff.
 *  - The scheduler/affinity interpositions are gated by SCHED_RT.
 *
 * ---- What must not change ----
 *
 *  - The S24 sign-extension `(int32_t)(l << 8) >> 8`. rbp's samples are
 *    right-justified 24-bit values whose top byte is zero, so the raw word for -1
 *    is the *positive* 0x00ffffff and every gain and every peak measurement is
 *    wrong without it. (The packing in s24pack.c is not — it reads only bits 23..0
 *    — which is the useful half of this rule: a wrong level is never the packer's
 *    doing, so look in the arithmetic.)
 *  - The mmap interposition for rbp's broken user_space_rtc_init() mapping.
 *  - The sw-params setters always succeeding, the 2/2 channel min/max lie, the
 *    44100-only test_rate, the prepare-retry after a failed write, and the
 *    silence-for-capture answer. Each one is a lie rbp needs.
 *  - g_vu_peak is still published even though the DDJ-FLX4 has no meters to light:
 *    the controls shim decides what to do with it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>   /* gettimeofday, for the reopen backoff — see now_ms() */

/* Declarations of the state owned by the controls shim (which is preloaded
 * before us), plus the constructor-time check that it is actually there. */
#include "shmstate.h"
#include "envutil.h"
#include "s24pack.h"
#include "mirror_policy.h"

/* Enforce GLIBC_2.4 versioning for libdl on glibc 2.13 */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");
__asm__(".symver dlopen, dlopen@GLIBC_2.4");
__asm__(".symver dlerror, dlerror@GLIBC_2.4");
__asm__(".symver dlclose, dlclose@GLIBC_2.4");

#ifndef SYS_mmap2
#define SYS_mmap2 __NR_mmap2
#endif

/* rbp's user_space_rtc_init() calls mmap(MAP_SHARED, fd=-1) after /dev/mem open
 * fails (we chmod 000 /dev/mem). That mmap returns MAP_FAILED and leaves a
 * dangling RTC pointer (0x10000023) which later causes SIGSEGV loops -> watchdog.
 * Redirect that broken combination to an anonymous private mapping so it succeeds
 * with zeroed memory and sched_clock/v2_get_cycles read 0 instead of crashing. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
        fd = -1;
        offset = 0;
    }
    return (void *)syscall(SYS_mmap2, addr, length, prot, flags, fd,
                           (unsigned long)offset >> 12);
}

#define LOG_PATH "/tmp/audioshim.log"

static void alog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

/* Opaque ALSA types */
typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef void snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef void snd_ctl_t;
typedef void snd_pcm_info_t;

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_STREAM_CAPTURE  1
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

/* 0x1, and NOT the 0x2 that open_real_device() clears for the master. The mirror
 * (below) is the one caller that wants this bit to survive: a non-blocking write
 * that cannot be delivered returns -EAGAIN instead of stalling rbp's audio thread
 * on a screen someone has just unplugged. */
#define SND_PCM_NONBLOCK        1

/* ALSA's own snd_pcm_format_t values. Spelled out rather than included, because
 * this shim deliberately does not link against the target's ALSA headers — only
 * against libdl and libc — so that it can be built from the armel toolchain
 * without a sysroot for the chroot's libasound.
 *
 * The packed 24-bit formats are the trap: they are NOT next to the padded ones,
 * they start at 32. S24_3LE read as 10 here for a long time, and 10 is S32_LE —
 * so the shim asked a 16/24-bit card for 32-bit samples and got a bare -EINVAL
 * that named nothing. Nothing caught it because the value only ever reaches
 * libasound on an `hw:` device, and AUDIO_DEV was `plughw:` until the Pi port,
 * where the old set_format() answered for itself and the number was never used.
 *
 * check_format_constants() below asks libasound to name each of these at startup,
 * so a wrong number is now one loud log line instead of silent no-audio.
 * tools/pcmprobe.c prints the whole map, measured from libasound rather than
 * transcribed. */
#define SND_PCM_FORMAT_S16_LE    2   /* width=16 physical=16 */
#define SND_PCM_FORMAT_S24_LE    6   /* width=24 physical=32 — 4-byte container */
#define SND_PCM_FORMAT_S24_3LE  32   /* width=24 physical=24 — 3 bytes packed */
/* width=24 physical=32 — the sample in the HIGH three bytes of the word. The only
 * format the vc4 HDMI devices offer, and not a convenience: measured 2026-09-27
 * with tools/pcmprobe inside the chroot, `plughw:CARD=vc4hdmi0,DEV=0` advertises
 * this one value and rejects S16_LE, S24_LE and S24_3LE with -EINVAL at
 * set_format, so the plug layer converts nothing for it and `hw:` is the route. */
#define SND_PCM_FORMAT_IEC958_SUBFRAME_LE 18

/* ---- geometry ------------------------------------------------------------- */

/* The largest block rbp has ever written. A bigger one is truncated, which is a
 * lie the old code told silently; here it is logged (see g_big_block_logged). */
#define MAX_FRAMES 4096

/* The output buffer's channel capacity, and the ceiling on the negotiated count.
 * The old code hardcoded 8 (the SC Live 4's frame); the Pi asks the card. */
#define MAX_OUT_CHANNELS 8

#define PAIR_NONE (-1)

struct pair { int a, b; };

static inline int pair_mapped(const struct pair *p)
{
    return p->a >= 0 && p->b >= 0;
}

/* Everything read once, in the constructor, from the environment start-rb.sh
 * exported out of rb.conf. No lazy re-reads anywhere: an env var read twice is an
 * env var that can disagree with itself. */
#define DEV_MAX 160

static struct {
    char dev[DEV_MAX];         /* AUDIO_DEV */
    char dev_plug[DEV_MAX + 8];/* its plughw: form, or "" (see load_config) */
    char card_id[64];          /* "DDJFLX4" extracted from a CARD= in dev, or "" */
    int  channels;             /* AUDIO_CHANNELS: 0 = auto (negotiate) */
    int  fmt;                  /* s24pack format, from AUDIO_FMT */
    char fmt_name[24];         /* as written, for the log */
    struct pair master;        /* AUDIO_MAP */
    struct pair headphones;
    struct pair booth;
    struct pair monitor;       /* AUDIO_MONITOR_PAIR */
    char mirror_dev[DEV_MAX + 64]; /* AUDIO_MIRROR_DEV, the raw list ("" = off) */
    int  mirror_fmt;           /* s24pack format, from AUDIO_MIRROR_FMT */
    char mirror_fmt_name[24];  /* as written, for the log */
    long mirror_reopen_ms;     /* AUDIO_MIRROR_REOPEN_MS: ceiling on the retry wait */
    long mute_frames;          /* STARTUP_MUTE_MS at 44100 */
    long fade_frames;          /* STARTUP_FADE_MS at 44100 */
    int  sched_rt;             /* SCHED_RT: 1 = keep rbp off RT (default) */
} g_cfg;

/* The device strings to try, in order, NULL-terminated. Built once in
 * load_config() rather than assembled at open time: a fallback chain that is
 * assembled on the failure path is a fallback chain whose last step is the one
 * nobody tested. */
static const char *g_dev_candidates[4];

/* AUDIO_DEV's default. The card id must match what /proc/asound/cards reports;
 * see docs/13-raspberrypi4.md for how to check it.
 *
 * hw: rather than plughw:, see the header: a plug device answers for its own
 * logical channel space (10000 on this unit) instead of the card's 4, so the map's
 * hardware channel indices cannot be resolved through one. rb.conf sets the same
 * string; this is the fallback for when the variable is unset entirely. */
#define AUDIO_DEV_DEFAULT "hw:CARD=DDJFLX4,DEV=0"

/* ---- the HDMI mirror: defaults and tunables ------------------------------- */

/* AUDIO_MIRROR_DEV's default, and the measurement that chose it.
 *
 * `hw:` and not `plughw:`, which is the opposite of what the first version of
 * this assumed — and the way it was wrong is worth keeping, because the same
 * measurement will be repeated by whoever reads this next.
 *
 * The vc4 HDMI PCMs offer exactly ONE format: IEC958_SUBFRAME_LE, 32-bit samples,
 * 2 channels, 32000-48000 Hz. The question is whether anything will convert to it
 * for us. A HOST aplay says yes, enthusiastically — the Pi OS libasound's plug
 * layer advertises the full 32-format logical set for this device and
 * `aplay -D plughw:CARD=vc4hdmi0,DEV=0 -f S24_3LE` plays happily. That is the
 * wrong libasound. rbp and this shim run against the CHROOT's (XDJ-RX3 rootfs,
 * glibc 2.13 era), and there the plug advertises THE SAME ONE FORMAT as the hw
 * device and rejects everything else:
 *
 *   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe plughw:CARD=vc4hdmi0,DEV=0 32 nonblock
 *     mask:          IEC958_SUBFRAME_LE
 *     set_format     FAILED  Invalid argument (-22)          # S24_3LE, and S16_LE
 *                                                            # and S24_LE likewise
 *   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe plughw:CARD=vc4hdmi0,DEV=0 18 nonblock
 *     set_format ok … hw_params ok … negotiated: rate=44100 channels=2 period=256
 *     write ok
 *
 * So there is no conversion to lean on, on either device name, and the one that
 * works is the one with no plugin under it at all. tools/pcmprobe's sequence mode
 * is the instrument (see its header: a host aplay is not an oracle for the
 * chroot's libasound, which is the whole reason it grew that mode).
 *
 * Both HDMI ports are listed because moving the monitor's cable is an ordinary
 * thing to do. A disconnected port fails at open — `Unknown error 524`, measured
 * on both forms — so a panel on the other port costs one failed open and no write.
 * Note that neither the rate nor the format is negotiable here: a sink that wanted
 * a rate outside 32000-48000 would need RB_AUDIO_MIRROR_FMT and a different
 * device, which is why the knob exists. */
#define AUDIO_MIRROR_DEV_DEFAULT \
    "hw:CARD=vc4hdmi0,DEV=0 hw:CARD=vc4hdmi1,DEV=0"

/* subframe_le, and it is the only value that can work on this device: see above.
 * It is the master pair at full 24 bits, left-justified in the 32-bit word the
 * vc4 DMA reads (s24pack.c documents the layout and why a byte-aligned reading of
 * "IEC958 subframe" is the one to take). s16_le and s24_3le are still accepted by
 * this knob — they are the right names for a device that converts for us — but on
 * the vc4 HDMI PCMs they fail at set_format, loudly, which is how the log reads. */
#define AUDIO_MIRROR_FMT_DEFAULT "subframe_le"

/* The ceiling on the retry wait after the mirror goes down, in ms. 5000 matches
 * the master's reopen ceiling and is deliberately the same kind of number: a
 * screen coming back is not urgent, and the attempt happens on rbp's audio thread.
 * RB_AUDIO_MIRROR_REOPEN_MS=0 means try once at startup and never again. */
#define AUDIO_MIRROR_REOPEN_MS_DEFAULT 5000

/* The mirror's own ring, in frames and periods. Bigger than the master's 64-frame
 * block on purpose: the two ends run on independent clocks (the FLX4 and the HDMI
 * sink), so what this sizes is how much drift the mirror can absorb before it has
 * to correct. 256 x 4 is 1024 frames.
 *
 * The first version of this comment predicted the drift from crystal tolerance —
 * ~50 ppm, 2.2 frames/s, a block dropped every ~460 s — and only in the draining
 * direction. The unit measured something else, which is why mirror_pad() exists:
 * this sink consumes **6.9 frames/s faster** than the FLX4 feeds it (~155 ppm, the
 * vc4 HDMI clock being derived rather than a plain crystal), so the ring empties
 * rather than fills, and a lead sized only by the prefill is gone in ~74 s. The
 * half-full target that both correctors work around is the number the measurement
 * chose: half the ring is the most room in either direction, and at 1024 frames it
 * is 11.6 ms of latency on an output nobody syncs to anything. There is no
 * resampler here and there is not meant to be: the master's clock is the one that
 * must not move. */
#define MIRROR_RATE      44100
#define MIRROR_PERIOD    256
#define MIRROR_PERIODS   4

/* The silence written at each stream start, in frames: half the ring above.
 *
 * This is not padding and it is not optional. rbp hands the mirror 64 frames per
 * master block, which at 44100 Hz is exactly real time — measured on the unit as
 * ~700 blocks a second, i.e. one every 1.43 ms. A ring that is only ever given 64
 * frames holds 64 frames: the DMA drains it in 1.43 ms and then has nothing, so
 * the stream reaches XRUN before the next block arrives, every single time. That
 * is what the first working deploy measured — status XRUN, avail pinned at the
 * full buffer size, and `retries=3500` of 3501 blocks, i.e. a prepare (which
 * discards the ring) before nearly every write. The counters read perfectly
 * healthy throughout, because the retry path resets the stream and then accepts
 * the block; what was wrong was invisible in them and visible only in the sink's
 * own state.
 *
 * Half the ring is the lead that makes the two ends independent: the mirror then
 * writes into a ring that is already playing, jitter in rbp's thread is absorbed
 * instead of starving the DMA, and the two clocks drift against a half-full ring
 * rather than against an empty one — which is what the drift comment above sizes
 * it for. 512 frames is 11.6 ms of latency on an output nobody syncs to anything.
 *
 * It is silence in every format this shim packs: all four of them encode a zero
 * sample as zero bytes. */
#define MIRROR_PREFILL_FRAMES (MIRROR_PERIOD * MIRROR_PERIODS / 2)

/* The ceiling on one block's padding, in frames. The measured drift needs about one
 * frame per 0.2 s of audio, so anything approaching this number is not drift — it is
 * a ring that was drained by something else (a stop in between, a level reading that
 * arrived after a stall), and 64 frames bounds how much silence such an event can
 * inject in one go. Sixteen blocks at most to work off a whole ring. */
#define MIRROR_PAD_MAX 64

/* The retry floor, matching the master's REOPEN_MIN_MS. */
#define MIRROR_REOPEN_MIN_MS 500

/* ---- virtual handles ------------------------------------------------------ */

/* rbp holds these as opaque snd_pcm_t*, so they only have to be distinct and
 * stable. g_h_master exists for the case where no device could be opened at all:
 * stream 0 then needs a non-NULL handle that the write path still recognises, so
 * it can pace itself with a sleep instead of spamming the card. */
static int g_h_master = 1;
static int g_h_phone  = 2;
static int g_h_booth  = 3;
static int g_h_dummy  = 4;
static int g_h_cap    = 5;

static snd_pcm_t *g_real_playback = NULL;
static int g_playback_open_count  = 0;

/* Set while this shim is inside a real snd_pcm_open() of its own, so that a
 * re-entrant public snd_pcm_open() from libasound's plugin chain is forwarded
 * instead of being handed one of the fake handles. See snd_pcm_open() below. */
static int g_in_real_open = 0;

/* Real ALSA function pointers */
static int (*real_snd_pcm_open)(snd_pcm_t **, const char *, int, int) = NULL;
static int (*real_snd_pcm_close)(snd_pcm_t *) = NULL;
static int (*real_snd_pcm_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *) = NULL;
static int (*real_snd_pcm_hw_params_any)(snd_pcm_t *, snd_pcm_hw_params_t *) = NULL;
static int (*real_snd_pcm_hw_params_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = NULL;
static int (*real_snd_pcm_hw_params_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = NULL;
static int (*real_snd_pcm_hw_params_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int) = NULL;
static int (*real_snd_pcm_hw_params_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *) = NULL;
static int (*real_snd_pcm_hw_params_set_period_size_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *) = NULL;
static int (*real_snd_pcm_hw_params_set_periods_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *) = NULL;
static int (*real_snd_pcm_hw_params_get_channels_min)(const snd_pcm_hw_params_t *, unsigned int *) = NULL;
static int (*real_snd_pcm_hw_params_get_channels_max)(const snd_pcm_hw_params_t *, unsigned int *) = NULL;
static int (*real_snd_pcm_sw_params_current)(snd_pcm_t *, snd_pcm_sw_params_t *) = NULL;
static int (*real_snd_pcm_sw_params_get_boundary)(const snd_pcm_sw_params_t *, snd_pcm_uframes_t *) = NULL;
static int (*real_snd_pcm_sw_params_set_silence_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_silence_size)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_start_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_stop_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params)(snd_pcm_t *, snd_pcm_sw_params_t *) = NULL;
static int (*real_snd_pcm_prepare)(snd_pcm_t *) = NULL;
static snd_pcm_sframes_t (*real_snd_pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t) = NULL;
/* The mirror's level sensor, and the mirror's only use of it: see mirror_pad(). */
static snd_pcm_sframes_t (*real_snd_pcm_avail_update)(snd_pcm_t *) = NULL;
static int (*real_snd_ctl_open)(snd_ctl_t **, const char *, int) = NULL;
static int (*real_snd_ctl_close)(snd_ctl_t *) = NULL;
static const char *(*real_snd_pcm_format_name)(int) = NULL;
/* The params allocators, for the reopen only: see replay_negotiation(). rbp's own
 * params objects are unreachable from here, and must not be borrowed — the values
 * inside one are rbp's to own and one of them may live on a frame that has
 * already returned. */
static int (*real_snd_pcm_hw_params_malloc)(snd_pcm_hw_params_t **) = NULL;
static void (*real_snd_pcm_hw_params_free)(snd_pcm_hw_params_t *) = NULL;
static int (*real_snd_pcm_sw_params_malloc)(snd_pcm_sw_params_t **) = NULL;
static void (*real_snd_pcm_sw_params_free)(snd_pcm_sw_params_t *) = NULL;

/* Confirm that the hand-rolled SND_PCM_FORMAT_* values above name the formats this
 * shim believes they name, by asking libasound to name them. A mismatch is not
 * cosmetic: set_format() passes the value straight to the card on a hw: device
 * and the card rejects it with -EINVAL, which says nothing about which number was
 * wrong. See the comment on the constants.
 *
 * It reports the values it *did* resolve, not just the ones it caught, because a
 * check that is silent both when it passes and when it has been quietly disabled
 * — an unresolved dlsym returns early — is not a check anyone can trust. One
 * line, once per process that touches ALSA. */
static void check_format_constants(void)
{
    static const struct { int val; const char *want; } t[] = {
        { SND_PCM_FORMAT_S16_LE,  "S16_LE"  },
        { SND_PCM_FORMAT_S24_LE,  "S24_LE"  },
        { SND_PCM_FORMAT_S24_3LE, "S24_3LE" },
        { SND_PCM_FORMAT_IEC958_SUBFRAME_LE, "IEC958_SUBFRAME_LE" },
    };
    size_t i;
    int bad = 0;

    if (!real_snd_pcm_format_name) {
        alog("audioshim: cannot verify the format constants: libasound has no "
             "snd_pcm_format_name\n");
        return;
    }
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *got = real_snd_pcm_format_name(t[i].val);
        if (!got || strcmp(got, t[i].want) != 0) {
            bad = 1;
            alog("audioshim: FORMAT CONSTANT WRONG: %d names '%s', not '%s' — "
                 "fix the #define above; a wrong value reaches the card as a bare "
                 "-EINVAL with no name attached\n",
                 t[i].val, got ? got : "(null)", t[i].want);
        }
    }
    if (!bad)
        alog("audioshim: format constants verified: S16_LE=%d S24_LE=%d S24_3LE=%d "
             "IEC958_SUBFRAME_LE=%d\n",
             SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE,
             SND_PCM_FORMAT_IEC958_SUBFRAME_LE);
}

static void init_real_alsa(void)
{
    /* The latch goes up *last*, after every pointer is assigned. Raising it first
     * — which this used to do — lets a second thread return early and go on to
     * call a real_snd_* that is still NULL, and the failure mode is silent:
     * snd_pcm_open() then hands out the fake master and rbp runs with no device
     * while the log looks perfectly healthy. Two threads racing the dlopen is
     * harmless, because the writes are idempotent and all of them are the same
     * addresses; a half-populated table is not. */
    static int initialized = 0;
    if (initialized) return;

    void *lib = dlopen("libasound.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        alog("audioshim: failed to dlopen libasound.so.2: %s\n", dlerror());
        return;
    }

    real_snd_pcm_open = dlsym(lib, "snd_pcm_open");
    real_snd_pcm_close = dlsym(lib, "snd_pcm_close");
    real_snd_pcm_hw_params = dlsym(lib, "snd_pcm_hw_params");
    real_snd_pcm_hw_params_any = dlsym(lib, "snd_pcm_hw_params_any");
    real_snd_pcm_hw_params_set_access = dlsym(lib, "snd_pcm_hw_params_set_access");
    real_snd_pcm_hw_params_set_format = dlsym(lib, "snd_pcm_hw_params_set_format");
    real_snd_pcm_hw_params_set_channels = dlsym(lib, "snd_pcm_hw_params_set_channels");
    real_snd_pcm_hw_params_set_rate_near = dlsym(lib, "snd_pcm_hw_params_set_rate_near");
    real_snd_pcm_hw_params_set_period_size_near = dlsym(lib, "snd_pcm_hw_params_set_period_size_near");
    real_snd_pcm_hw_params_set_periods_near = dlsym(lib, "snd_pcm_hw_params_set_periods_near");
    real_snd_pcm_hw_params_get_channels_min = dlsym(lib, "snd_pcm_hw_params_get_channels_min");
    real_snd_pcm_hw_params_get_channels_max = dlsym(lib, "snd_pcm_hw_params_get_channels_max");
    real_snd_pcm_sw_params_current = dlsym(lib, "snd_pcm_sw_params_current");
    real_snd_pcm_sw_params_get_boundary = dlsym(lib, "snd_pcm_sw_params_get_boundary");
    real_snd_pcm_sw_params_set_silence_threshold = dlsym(lib, "snd_pcm_sw_params_set_silence_threshold");
    real_snd_pcm_sw_params_set_silence_size = dlsym(lib, "snd_pcm_sw_params_set_silence_size");
    real_snd_pcm_sw_params_set_start_threshold = dlsym(lib, "snd_pcm_sw_params_set_start_threshold");
    real_snd_pcm_sw_params_set_stop_threshold = dlsym(lib, "snd_pcm_sw_params_set_stop_threshold");
    real_snd_pcm_sw_params = dlsym(lib, "snd_pcm_sw_params");
    real_snd_pcm_prepare = dlsym(lib, "snd_pcm_prepare");
    real_snd_pcm_writei = dlsym(lib, "snd_pcm_writei");
    real_snd_pcm_avail_update = dlsym(lib, "snd_pcm_avail_update");
    real_snd_ctl_open = dlsym(lib, "snd_ctl_open");
    real_snd_ctl_close = dlsym(lib, "snd_ctl_close");
    real_snd_pcm_format_name = dlsym(lib, "snd_pcm_format_name");
    real_snd_pcm_hw_params_malloc = dlsym(lib, "snd_pcm_hw_params_malloc");
    real_snd_pcm_hw_params_free = dlsym(lib, "snd_pcm_hw_params_free");
    real_snd_pcm_sw_params_malloc = dlsym(lib, "snd_pcm_sw_params_malloc");
    real_snd_pcm_sw_params_free = dlsym(lib, "snd_pcm_sw_params_free");

    check_format_constants();

    alog("audioshim: real ALSA initialized\n");
    initialized = 1;
}

/* ---- buffers -------------------------------------------------------------- */

/* The flush buffer: one interleaved frame of g_out_channels, rebuilt from scratch
 * on every master write and sent to the card in one call. */
static int32_t g_out[MAX_FRAMES * MAX_OUT_CHANNELS];

/* The packed form, when AUDIO_FMT asks for something other than S24_LE. Sized for
 * the worst case (a 4-byte container) so no format can overflow it; s24pack()
 * still refuses rather than truncate, and that refusal is handled below. */
static int32_t g_packed[MAX_FRAMES * MAX_OUT_CHANNELS];

/* One staged block per secondary stream. The phone and booth streams cannot write
 * to the card themselves — there is only one device and one ALSA stream — so they
 * leave their last block here and the master's write folds it in.
 *
 * The limitation this carries, stated plainly because it is the kind of thing
 * that shows up as an intermittent glitch: the fold takes min(size, staged)
 * frames. If rbp ever drives the phone stream at a different block size to the
 * master, the cue pair is truncated or its last block repeated. Rather than hide
 * that, flush_master() compares the two and logs the mismatch — a per-pair ring
 * buffer is the fix if it ever appears. */
struct stage {
    int32_t buf[MAX_FRAMES * 2];
    unsigned frames;        /* frames ready, 0 = nothing staged yet */
    unsigned warn_size;     /* last mismatching size already logged */
};
static struct stage g_phone_stage, g_booth_stage;

/* ---- the HDMI mirror's runtime state -------------------------------------- */

/* The advisory second output: the master pair, post Main Vol and post startup
 * mute, written to whatever HDMI sink is attached. It is advisory in the strict
 * sense — every failure below costs a log line and some frames on HDMI, and none of
 * them may touch g_real_playback, rbp's return value, or the master's cadence.
 *
 * It is written from the audio thread, non-blocking, and recovers from there too;
 * there is no second thread and no dependency beyond libc (the shim links only
 * libc/libdl — do not add -lpthread for this). */
static char g_mirror_cands[MIRROR_CAND_MAX][MIRROR_NAME_MAX];
static int g_mirror_ncands;            /* 0 = the mirror is off */
static int g_mirror_dropped_cands;     /* entries the list could not hold */
static snd_pcm_t *g_mirror;            /* the open handle, NULL = not up */
static char g_mirror_dev[MIRROR_NAME_MAX];  /* what opened, for the log */
static unsigned long g_mirror_blocks;  /* writei() calls made */
static unsigned long g_mirror_frames;  /* frames actually delivered */
static unsigned long g_mirror_drops;   /* blocks dropped (ring full) */
static unsigned long g_mirror_shorts;  /* blocks only partly accepted */
static unsigned long g_mirror_retries; /* streams prepared once and retried */
static unsigned long g_mirror_losses;  /* handles closed after a hard error */
static unsigned long long g_mirror_absent_ms;  /* 0 = never lost */
static unsigned long g_mirror_backoff_ms = MIRROR_REOPEN_MIN_MS;
static int g_mirror_open_fails;        /* walks that opened nothing; quiets at 3 */
static int g_mirror_tried;             /* the startup attempt has been made */
static int g_mirror_written_once;      /* "UP" has been earned and logged */
static int g_mirror_pack_failed;       /* the pack refusal has been logged */
static unsigned long g_mirror_prefill; /* frames of silence at each stream start */
static unsigned long g_mirror_buffer;  /* the ring the sink granted, in frames */
static unsigned long g_mirror_pads;    /* blocks whose write carried padding */
static unsigned long g_mirror_pad_frames_sum;  /* frames of padding written */

/* The mirror's staging buffers, stereo and only stereo: channel 0 is the master's
 * left, channel 1 its right (flush_master() fills them below). Two buffers, like
 * the master's, because AUDIO_MIRROR_FMT may ask for a packed form. The packed one
 * is int32_t sized, i.e. four bytes per sample per channel, which is the widest
 * container s24pack() has (s24pack_worst()); a format wider than that could not
 * overflow it anyway, because s24pack() answers 0 rather than truncating. */
static int32_t g_mirror_out[MAX_FRAMES * 2];
static int32_t g_mirror_packed[MAX_FRAMES * 2];

/* The prefill, in the mirror's packed format: all zeroes, which is a zero sample
 * in all four of them (see MIRROR_PREFILL_FRAMES). Static and byte-typed because
 * what the device is handed is bytes, and it is sized for the worst case — four
 * bytes per sample per channel — so a larger negotiated geometry can only ever be
 * clamped, never written past. */
static unsigned char g_mirror_silence[MIRROR_PREFILL_FRAMES * 2 * sizeof(int32_t)];

static unsigned int g_out_channels;   /* negotiated; 0 until set_channels() */
static int g_pairs_resolved;
static unsigned long g_write_count;
static int g_big_block_logged;
static int g_pack_failed_logged;

/* Startup mute/fade bookkeeping. Frames are counted on the master stream only:
 * the secondary streams advance on their own clocks, so a single shared counter
 * would not describe them. See the note where the fade is applied. */
static unsigned long long g_startup_frames_done;
static int g_startup_released;

/* Set by resolve_pairs() when it had to assume a channel count because no device
 * was ever negotiated with. It is what lets the recovery line below say "master
 * pair only" and mean it, rather than leaving the operator to work out that the
 * cue and booth streams are missing because the map was latched before the card
 * existed. */
static int g_channels_assumed;

/* ---- losing the card, and getting it back ----------------------------------
 *
 * The FLX4 is a USB device, so it can be pulled while rbp is playing. What that
 * does to this shim is not subtle: rbp holds the real snd_pcm_t as an opaque
 * pointer, the card is re-enumerated, and every write on that handle then fails
 * with -ENODEV (-19) for as long as the process lives. Measured on the unit:
 * 5.1 million such lines, and a restart was the only cure.
 *
 * Everything below exists to make the same handle work again. */

/* rbp calls flush_master() from its audio thread continuously, so the clock for
 * everything here is the wall clock rather than a thread of our own.
 *
 * gettimeofday and not clock_gettime: this shim deliberately does not link
 * librt, and does not link shimutil.o either — its only shared dependency is
 * libasound, resolved by dlsym. */
static unsigned long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL
         + (unsigned long long)(tv.tv_usec / 1000);
}

#define REOPEN_MIN_MS 500
#define REOPEN_MAX_MS 5000

static unsigned long long g_master_absent_ms;   /* 0 = unknown, or a card is open */
static unsigned long g_reopen_backoff_ms = REOPEN_MIN_MS;
static unsigned g_reopen_failures;              /* consecutive failed reopen tries */
static unsigned long g_master_losses;           /* mid-stream card losses */

/* What rbp asked the card for, recorded as it asked, so that a reopen can ask
 * again.
 *
 * The format, the channel count and the rate are deliberately NOT here: this shim
 * forces all three itself (set_format() writes AUDIO_FMT's format, set_channels()
 * writes the count it negotiated, set_rate_near() forces 44100), so replaying
 * through those setters reproduces them from the same source the first
 * negotiation used. Everything else is rbp's own choice and would otherwise go
 * down with its params object.
 *
 * `mode` is snd_pcm_open()'s, as rbp passed it — open_real_device() re-masks
 * SND_PCM_NONBLOCK out of it on every call, so what is stored is rbp's request
 * and not our adjustment of it. */
static struct {
    int mode;
    int access;
    snd_pcm_uframes_t period_size;   /* after the near call; 0 = never seen */
    unsigned int periods;            /* ditto */
    snd_pcm_uframes_t silence_threshold, silence_size;
    snd_pcm_uframes_t start_threshold, stop_threshold;
} g_nego = {
    SND_PCM_STREAM_PLAYBACK, SND_PCM_ACCESS_RW_INTERLEAVED, 0, 0, 0, 0, 0, 0,
};

/* ---- configuration -------------------------------------------------------- */

static void parse_pair(const char *what, const char *v, struct pair *out)
{
    char *end;
    long a, b;

    if (v[0] == '-' && v[1] == '\0') {   /* the documented way to drop a stream */
        out->a = out->b = PAIR_NONE;
        return;
    }
    a = strtol(v, &end, 10);
    if (end == v || *end != ',') {
        alog("audioshim: %s: '%s' is not a channel pair (want a,b or -); "
             "leaving the default\n", what, v);
        return;
    }
    const char *rest = end + 1;
    b = strtol(rest, &end, 10);
    if (end == rest || *end != '\0') {
        alog("audioshim: %s: '%s' is not a channel pair (want a,b or -); "
             "leaving the default\n", what, v);
        return;
    }
    if (a < 0 || b < 0 || a >= MAX_OUT_CHANNELS || b >= MAX_OUT_CHANNELS) {
        alog("audioshim: %s: channel %ld,%ld is outside 0..%d; leaving the default\n",
             what, a, b, MAX_OUT_CHANNELS - 1);
        return;
    }
    if (a == b) {
        alog("audioshim: %s: channel %ld appears twice in %ld,%ld; leaving the "
             "default\n", what, a, a, b);
        return;
    }
    out->a = (int)a;
    out->b = (int)b;
}

/* AUDIO_MAP is a list of `stream=pair` entries. It starts from the defaults and
 * overrides only what it names, so `AUDIO_MAP=booth=0,1` moves one stream rather
 * than silently unmapping the other two. */
static void parse_map(const char *s)
{
    char buf[192], *tok, *next;

    if (s[0] == '\0')
        return;
    if (strlen(s) >= sizeof buf) {
        alog("audioshim: AUDIO_MAP is longer than %u bytes; ignoring it entirely\n",
             (unsigned)sizeof buf - 1);
        return;
    }
    strcpy(buf, s);

    for (tok = buf; tok != NULL; tok = next) {
        char *eq, *name, *val;
        struct pair *dst;

        next = strchr(tok, ';');
        if (next) *next++ = '\0';
        if (*tok == '\0')
            continue;

        eq = strchr(tok, '=');
        if (eq == NULL) {
            alog("audioshim: AUDIO_MAP entry '%s' is not stream=pair; ignoring it\n", tok);
            continue;
        }
        *eq = '\0';
        name = tok;
        val = eq + 1;

        if (strcmp(name, "monitor") == 0) {
            alog("audioshim: AUDIO_MAP has no 'monitor' entry — set "
                 "AUDIO_MONITOR_PAIR instead; ignoring it\n");
            continue;
        }
        if (strcmp(name, "master") == 0)
            dst = &g_cfg.master;
        else if (strcmp(name, "headphones") == 0)
            dst = &g_cfg.headphones;
        else if (strcmp(name, "booth") == 0)
            dst = &g_cfg.booth;
        else {
            alog("audioshim: AUDIO_MAP stream '%s' is not one of "
                 "master/headphones/booth; ignoring it\n", name);
            continue;
        }
        parse_pair(name, val, dst);
    }
}

/* Pull the card id out of an ALSA device string's CARD= token, for snd_ctl_open
 * matching. Empty when the string names no card. */
static void parse_card_id(const char *dev)
{
    const char *p = strstr(dev, "CARD=");
    size_t n;

    if (p == NULL)
        return;
    p += 5;
    n = strcspn(p, ",:");
    if (n == 0 || n >= sizeof g_cfg.card_id)
        return;
    memcpy(g_cfg.card_id, p, n);
    g_cfg.card_id[n] = '\0';
}

/* Read the whole configuration once. Called from the constructor, before rbp has
 * threads: an env var read once cannot disagree with itself, and there is no lock
 * to take. */
static void load_config(void)
{
    const char *s;
    long mute_ms, fade_ms;

    s = env_str("AUDIO_DEV", AUDIO_DEV_DEFAULT);
    if (strlen(s) >= sizeof g_cfg.dev) {
        alog("audioshim: AUDIO_DEV is longer than %u bytes; using the default "
             "%s\n", (unsigned)sizeof g_cfg.dev - 1, AUDIO_DEV_DEFAULT);
        s = AUDIO_DEV_DEFAULT;
    }
    strcpy(g_cfg.dev, s);
    parse_card_id(g_cfg.dev);

    /* A hw: device that will not open is nearly always one that needs the plug
     * chain for format or rate conversion, so its plughw: form gets one attempt
     * before falling back to 'default'. Both strings are bounded by DEV_MAX here,
     * which is the only place they are built. */
    g_cfg.dev_plug[0] = '\0';
    if (strncmp(g_cfg.dev, "hw:", 3) == 0) {
        strcpy(g_cfg.dev_plug, "plughw:");
        strcat(g_cfg.dev_plug, g_cfg.dev + 3);
    }
    g_dev_candidates[0] = g_cfg.dev;
    g_dev_candidates[1] = g_cfg.dev_plug[0] ? g_cfg.dev_plug : NULL;
    g_dev_candidates[2] = "default";
    g_dev_candidates[3] = NULL;

    /* 0 means auto. env_int() returns the default for anything unparseable, so
     * rb.conf's literal "auto" lands here as auto, which is the intent. */
    g_cfg.channels = env_int("AUDIO_CHANNELS", 0);

    s = env_str("AUDIO_FMT", "");
    g_cfg.fmt = s24pack_parse(s);
    if (g_cfg.fmt < 0) {
        alog("audioshim: AUDIO_FMT='%s' is not a format this shim can pack "
             "(s24_le, s24_3le, s16_le); using s24_le\n", s);
        g_cfg.fmt = AUDIO_FMT_S24_LE;
    }
    snprintf(g_cfg.fmt_name, sizeof g_cfg.fmt_name, "%s",
             g_cfg.fmt == AUDIO_FMT_S24_3LE ? "s24_3le" :
             g_cfg.fmt == AUDIO_FMT_S16_LE  ? "s16_le"  : "s24_le");

    /* Defaults first; AUDIO_MAP overrides what it names. */
    g_cfg.master.a = 0;      g_cfg.master.b = 1;
    g_cfg.headphones.a = 2;  g_cfg.headphones.b = 3;
    g_cfg.booth.a = PAIR_NONE; g_cfg.booth.b = PAIR_NONE;
    g_cfg.monitor.a = PAIR_NONE; g_cfg.monitor.b = PAIR_NONE;

    parse_map(env_str("AUDIO_MAP", ""));
    parse_pair("AUDIO_MONITOR_PAIR", env_str("AUDIO_MONITOR_PAIR", ""), &g_cfg.monitor);

    /* The HDMI mirror. An EMPTY AUDIO_MIRROR_DEV turns it off, and empty is what
     * start-rb.sh exports for a knob rb.conf does not set — so the in-source
     * default below reaches only a shim run by hand with no environment at all,
     * exactly as AUDIO_DEV's does. rb.conf ships the list, and rb.local.conf
     * overrides it per unit. */
    s = env_str("AUDIO_MIRROR_DEV", AUDIO_MIRROR_DEV_DEFAULT);
    if (strlen(s) >= sizeof g_cfg.mirror_dev) {
        alog("audioshim: AUDIO_MIRROR_DEV is longer than %u bytes; using the "
             "default %s\n", (unsigned)sizeof g_cfg.mirror_dev - 1,
             AUDIO_MIRROR_DEV_DEFAULT);
        s = AUDIO_MIRROR_DEV_DEFAULT;
    }
    strcpy(g_cfg.mirror_dev, s);
    g_mirror_ncands = mirror_candidates(g_cfg.mirror_dev, g_mirror_cands,
                                        MIRROR_CAND_MAX, &g_mirror_dropped_cands);

    s = env_str("AUDIO_MIRROR_FMT", AUDIO_MIRROR_FMT_DEFAULT);
    g_cfg.mirror_fmt = s24pack_parse(s);
    if (g_cfg.mirror_fmt < 0) {
        alog("audioshim: AUDIO_MIRROR_FMT='%s' is not a format this shim can pack "
             "(s24_le, s24_3le, s16_le, subframe_le); using %s\n",
             s, AUDIO_MIRROR_FMT_DEFAULT);
        g_cfg.mirror_fmt = s24pack_parse(AUDIO_MIRROR_FMT_DEFAULT);
    }
    snprintf(g_cfg.mirror_fmt_name, sizeof g_cfg.mirror_fmt_name, "%s",
             g_cfg.mirror_fmt == AUDIO_FMT_S24_3LE     ? "s24_3le"     :
             g_cfg.mirror_fmt == AUDIO_FMT_S16_LE      ? "s16_le"      :
             g_cfg.mirror_fmt == AUDIO_FMT_SUBFRAME_LE ? "subframe_le" : "s24_le");

    g_cfg.mirror_reopen_ms = env_int("AUDIO_MIRROR_REOPEN_MS",
                                     AUDIO_MIRROR_REOPEN_MS_DEFAULT);
    if (g_cfg.mirror_reopen_ms < 0)
        g_cfg.mirror_reopen_ms = 0;

    /* One line per candidate, at startup only, so the log says what was asked for
     * even when nothing ever opens. alog() truncates at 512 bytes, which a list
     * could reach, and a silently shortened device list is the kind of thing this
     * port keeps having to chase. */
    if (g_mirror_ncands > 0) {
        int i;
        alog("audioshim: mirror dev=\"%s\" fmt=%s reopen=%ldms candidates=%d "
             "dropped=%d\n", g_cfg.mirror_dev, g_cfg.mirror_fmt_name,
             g_cfg.mirror_reopen_ms, g_mirror_ncands, g_mirror_dropped_cands);
        for (i = 0; i < g_mirror_ncands; i++)
            alog("audioshim: mirror candidate %d/%d: %s\n",
                 i + 1, g_mirror_ncands, g_mirror_cands[i]);
    } else {
        alog("audioshim: mirror off (AUDIO_MIRROR_DEV empty)\n");
    }

    /* The in-source default is 0, but rb.conf ships 1500/300: the FLX4 DOES thump
     * when the card is opened (the operator heard it), so the "USB audio has no
     * codec power-up transient" reasoning this comment used to carry was wrong for
     * this card. The zero default stays because a shim run by hand with no
     * environment should not impose 1.8 s of silence on anyone, and because a card
     * that is merely LATE benefits from having the mute off. 44100 is the rate rbp
     * speaks regardless of what the card ends up running at. */
    mute_ms = env_int("STARTUP_MUTE_MS", 0);
    fade_ms = env_int("STARTUP_FADE_MS", 0);
    if (mute_ms < 0) mute_ms = 0;
    if (fade_ms < 0) fade_ms = 0;
    g_cfg.mute_frames = mute_ms * 44100L / 1000L;
    g_cfg.fade_frames = fade_ms * 44100L / 1000L;

    /* 1 (the default) means the interpositions below stay in force: rbp's audio
     * threads run at normal priority and unpinned, which is what kept playback
     * stable. 0 lets rbp's own SCHED_FIFO 98 / core-0 requests through, for when
     * the difference itself is what you are measuring. */
    g_cfg.sched_rt = env_flag("SCHED_RT", 1);

    alog("audioshim: config dev=%s card=%s channels=%s map=[master=%d,%d "
         "headphones=%d,%d booth=%d,%d monitor=%d,%d] fmt=%s mute=%ldms "
         "fade=%ldms sched_rt=%d\n",
         g_cfg.dev, g_cfg.card_id[0] ? g_cfg.card_id : "-",
         g_cfg.channels ? "fixed" : "auto",
         g_cfg.master.a, g_cfg.master.b, g_cfg.headphones.a, g_cfg.headphones.b,
         g_cfg.booth.a, g_cfg.booth.b, g_cfg.monitor.a, g_cfg.monitor.b,
         g_cfg.fmt_name, mute_ms, fade_ms, g_cfg.sched_rt);
}

/* Verify the shared-state contract before anything reads g_master_gain and
 * friends. Without the controls shim preloaded ahead of us those symbols are
 * simply absent, and the failure mode is otherwise silent: the cue knob and the
 * master level would just do nothing. */
__attribute__((constructor))
static void audioshim_init(void)
{
    shmstate_require("audioshim");
    load_config();
}

/* ---- the pair map, resolved against the real device ----------------------- */

/* Drop a pair the negotiated device cannot reach. The plan for a card with fewer
 * channels than the map assumes is to *drop* the stream with a log line, never to
 * fold it onto a lower pair: a downmix would put the master on the headphone jack
 * and sound like a wiring fault. */
static void clamp_pair(const char *what, struct pair *p)
{
    if (!pair_mapped(p))
        return;
    if (p->a >= (int)g_out_channels || p->b >= (int)g_out_channels) {
        alog("audioshim: %s pair %d,%d is beyond the %u channel(s) %s has; "
             "dropping that stream\n", what, p->a, p->b, g_out_channels, g_cfg.dev);
        p->a = p->b = PAIR_NONE;
    }
}

static void resolve_pairs(void)
{
    if (g_pairs_resolved)
        return;
    g_pairs_resolved = 1;

    if (g_out_channels == 0) {
        /* set_channels() never arrived, so there is no params object left to ask.
         * Two channels — master only — is the honest degradation, and it is logged
         * so it cannot be mistaken for a routing bug.
         *
         * This is also the state a unit boots into with the controller unplugged,
         * and the reason a card that appears later recovers the master pair and
         * nothing else: everything below clamps against this 2 and drops the cue
         * and booth pairs for the life of the process. The flag is what lets the
         * recovery line say so. */
        g_out_channels = 2;
        g_channels_assumed = 1;
        alog("audioshim: set_channels() was never called; assuming 2 channels\n");
    }

    clamp_pair("master", &g_cfg.master);
    clamp_pair("headphones", &g_cfg.headphones);
    clamp_pair("booth", &g_cfg.booth);
    clamp_pair("monitor", &g_cfg.monitor);

    alog("audioshim: resolved %u channel(s): master=%d,%d headphones=%d,%d "
         "booth=%d,%d monitor=%d,%d\n", g_out_channels,
         g_cfg.master.a, g_cfg.master.b, g_cfg.headphones.a, g_cfg.headphones.b,
         g_cfg.booth.a, g_cfg.booth.b, g_cfg.monitor.a, g_cfg.monitor.b);

    /* Two streams sharing a channel is not fatal — the last write wins — but it
     * is always a configuration mistake and it sounds like a broken mix, so name
     * the offender rather than let it be diagnosed by ear. */
    {
        const struct pair *pairs[4];
        const char *names[4] = { "master", "headphones", "booth", "monitor" };
        int i, j;

        pairs[0] = &g_cfg.master; pairs[1] = &g_cfg.headphones;
        pairs[2] = &g_cfg.booth;  pairs[3] = &g_cfg.monitor;
        for (i = 0; i < 4; i++)
            for (j = i + 1; j < 4; j++) {
                if (!pair_mapped(pairs[i]) || !pair_mapped(pairs[j]))
                    continue;
                if (pairs[i]->a == pairs[j]->a || pairs[i]->a == pairs[j]->b ||
                    pairs[i]->b == pairs[j]->a || pairs[i]->b == pairs[j]->b)
                    alog("audioshim: %s and %s share a channel; the later write "
                         "wins\n", names[i], names[j]);
            }
    }
}

/* ---- the negotiation ------------------------------------------------------ */

/* How many channels to ask the real device for.
 *
 * Called from snd_pcm_hw_params_set_channels(), which is the first point at which
 * rbp's params object has been through hw_params_any() and therefore describes the
 * device's whole configuration space.
 *
 * The rule is to ask for exactly as many channels as the card has, and never more:
 * the route plugin is 1:1 only while the logical count is <= the hardware's, and
 * an over-ask makes it downmix — folding FL/FR into the second pair and putting
 * the master on the headphone jack. */
static unsigned int negotiate_channels(const snd_pcm_hw_params_t *params)
{
    unsigned int max = 0, want;

    if (real_snd_pcm_hw_params_get_channels_max)
        real_snd_pcm_hw_params_get_channels_max(params, &max);
    if (max == 0) {
        /* The query failed or is not implemented. 2 is what rbp itself believes,
         * so it is the only count that cannot surprise it. */
        alog("audioshim: the device did not report a channel maximum; assuming 2\n");
        max = 2;
    }
    if (max > MAX_OUT_CHANNELS) {
        alog("audioshim: %s reports %u channels; using the %d this shim can "
             "carry\n", g_cfg.dev, max, MAX_OUT_CHANNELS);
        max = MAX_OUT_CHANNELS;
    }

    want = g_cfg.channels ? (unsigned int)g_cfg.channels : max;
    if (want > max) {
        alog("audioshim: AUDIO_CHANNELS=%u exceeds the %u %s has; using %u\n",
             want, max, g_cfg.dev, max);
        want = max;
    }
    if (want < 1)
        want = 1;

    alog("audioshim: negotiating %u channel(s) on %s (%s)\n", want, g_cfg.dev,
         g_cfg.channels ? "AUDIO_CHANNELS" : "auto");
    return want;
}

/* ---- open / close --------------------------------------------------------- */

/* Handles that used to be the master and are not any more — a card that was
 * pulled and re-enumerated, so the snd_pcm_t rbp is still holding is dead. The
 * old pointer cannot be replaced (see the write path: rbp keeps it as an opaque
 * value and nothing here can rewrite rbp's variable), so it has to be
 * *recognised*, and the two things that must then happen to it are opposite:
 *
 *   a write on it must still reach flush_master(), which writes through the
 *   *global* and needs no unwrapping. That is what makes the recovery driven by
 *   rbp's own audio stream rather than by a timer: the moment rbp next writes,
 *   this shim is in a position to notice the card is back.
 *
 *   a parameter call on it must NOT be forwarded. is_forwardable()'s doctrine is
 *   to pass the caller's own pointer straight to libasound — deliberately, see
 *   the note there — and a dead pointer handed to a live libasound is a
 *   use-after-free.
 *
 * A ring rather than a set: the number that matters is "at least one", a replug
 * cycle retires at most one, and a handle retired long enough ago to have been
 * overwritten cannot still be in rbp's hands. */
#define RETIRED_MAX 8
static snd_pcm_t *g_retired[RETIRED_MAX];
static unsigned g_retired_next;

static int is_retired(snd_pcm_t *pcm)
{
    unsigned i;

    if (pcm == NULL)
        return 0;
    for (i = 0; i < RETIRED_MAX; i++)
        if (g_retired[i] == pcm)
            return 1;
    return 0;
}

static void retire(snd_pcm_t *pcm)
{
    if (pcm == NULL || is_retired(pcm))
        return;      /* idempotent: losing the same handle twice is one loss */
    g_retired[g_retired_next] = pcm;
    g_retired_next = (g_retired_next + 1) % RETIRED_MAX;
}

/* The master stream is the only one backed by a real device. When the device
 * could not be opened at all, stream 0 gets g_h_master instead of a NULL pointer:
 * rbp is not written to handle a NULL pcm, and the write path still needs a
 * recognisable identity so it can pace itself with a sleep.
 *
 * A retired handle counts. rbp still writes to the pointer it was given, and
 * dropping those writes would leave the stream unflushed *and* the recovery with
 * nothing to run on. */
static inline int is_master(snd_pcm_t *pcm)
{
    return pcm != NULL &&
           (pcm == g_real_playback || pcm == (snd_pcm_t *)&g_h_master ||
            is_retired(pcm));
}

/* Strictly "this is the handle we were given by a successful real open()". The
 * parameter plumbing forwards only for this one, so with no device every set_*
 * call is answered locally and nothing is called on a NULL handle. */
static inline int is_real(snd_pcm_t *pcm)
{
    return pcm != NULL && pcm == g_real_playback;
}

/* One of the five handles this shim invented, rather than one libasound made.
 * They are the addresses of static ints, so they are distinct from every heap
 * snd_pcm_t and stable for the process's life. */
static inline int is_fake(snd_pcm_t *pcm)
{
    return pcm == (snd_pcm_t *)&g_h_master ||
           pcm == (snd_pcm_t *)&g_h_phone  ||
           pcm == (snd_pcm_t *)&g_h_booth  ||
           pcm == (snd_pcm_t *)&g_h_dummy  ||
           pcm == (snd_pcm_t *)&g_h_cap;
}

/* rbp's own master stream, whether or not a device could be opened for it. This
 * is where a negotiation is *recorded* (see g_nego), and it is deliberately not
 * is_master(): a retired handle is still routed to flush_master — its write must
 * not be dropped — but it is not a stream rbp is negotiating with, and letting
 * one overwrite the recorded request would replay a dead stream's parameters onto
 * a freshly opened card. */
static inline int is_rbp_master(snd_pcm_t *pcm)
{
    return pcm == (snd_pcm_t *)&g_h_master || is_real(pcm);
}

/* The rule for the whole parameter plumbing: answer the handles *this shim
 * invented* with a local success, and forward everything else to the real
 * function — passing the caller's own pointer, never g_real_playback.
 *
 * "Everything else" is not just our master. A plugin device (plughw and friends)
 * configures and drives its slave by calling the public snd_pcm_* functions, so
 * this shim is re-entered with a libasound-internal handle: measured on the FLX4,
 * snd_pcm_prepare() and snd_pcm_sw_params() both arrive for pointers
 * snd_pcm_open() never handed out. Gating on is_real() alone made those a silent
 * no-op, which left the hw slave unconfigured and unprepared — every write then
 * failed with -EINVAL, showing up as `written=-22` with nothing in the log to say
 * why. It is the same defect snd_pcm_close() had, and snd_ctl_close() never did.
 *
 * On a hw: device this is inert: no plugin sits below us, so only the fakes and
 * g_real_playback ever reach these functions, and for those it means exactly what
 * is_real() meant.
 *
 * A retired handle is excluded. It was a real snd_pcm_t once, so it passes the
 * "not one of ours" test, and forwarding it would hand a dead pointer to
 * libasound — the one thing this predicate must never do. Answering locally is
 * also the honest answer for a card that is gone: the setter has nothing to
 * configure. */
static inline int is_forwardable(snd_pcm_t *pcm)
{
    return pcm != NULL && !is_fake(pcm) && !is_retired(pcm);
}

/* Forget everything a negotiation established, so the next open renegotiates
 * from scratch: the next card may not be the same card. */
static void reset_stream_state(void)
{
    g_playback_open_count = 0;
    g_out_channels = 0;
    g_pairs_resolved = 0;
    g_phone_stage.frames = 0;
    g_phone_stage.warn_size = 0;
    g_booth_stage.frames = 0;
    g_booth_stage.warn_size = 0;
    memset(g_out, 0, sizeof g_out);
}

/* Walk the candidate chain and take the first device that opens.
 *
 * `quiet` suppresses the per-candidate failure lines and the final NO OUTPUT
 * DEVICE one. It is set by the reopen below and only ever on a *repeat* failure:
 * a unit whose operator runs it with no controller at all is a supported
 * configuration, not an emergency, and a line every five seconds forever is how a
 * log stops being read. A success is logged either way — that line is the one the
 * operator is waiting for. */
static void open_real_device(int mode, int quiet)
{
    /* rbp asks for nonblocking and then relies on the device to pace it, and for
     * the master's real stream that really is the behaviour wanted: the card is
     * the audio clock, so every write blocks until the card has room.
     *
     * The masked bit is NOT SND_PCM_NONBLOCK, which this comment claimed for a
     * long time. The hand-rolled constants above put SND_PCM_NONBLOCK at 0x1 and
     * SND_PCM_ASYNC at 0x2, and 0x2 is what is cleared here. That is harmless for
     * this call — async notification is off unless rbp asks for it and installs a
     * callback — but it means a caller that does want a non-blocking handle must
     * ask for it explicitly and must not expect this function to leave the bit
     * alone. The HDMI mirror is such a caller: it passes SND_PCM_NONBLOCK and
     * opens the device itself (mirror_open_one()), precisely because this
     * function's contract is "the master, paced by the card". */
    int real_mode = mode & ~2;
    int i, err = -1;

    for (i = 0; g_dev_candidates[i] != NULL; i++) {
        g_in_real_open = 1;
        err = real_snd_pcm_open(&g_real_playback, g_dev_candidates[i],
                                SND_PCM_STREAM_PLAYBACK, real_mode);
        g_in_real_open = 0;
        if (err >= 0) {
            alog("audioshim: open('%s') mode=%d->%d res=%d handle=%p\n",
                 g_dev_candidates[i], mode, real_mode, err, g_real_playback);
            return;
        }
        if (!quiet)
            alog("audioshim: open('%s') mode=%d->%d res=%d handle=%p\n",
                 g_dev_candidates[i], mode, real_mode, err, g_real_playback);
        g_real_playback = NULL;
    }

    if (!quiet)
        alog("audioshim: NO OUTPUT DEVICE — rbp will run silent and every stream "
             "will sleep to pace itself\n");
    /* When this process first found itself cardless, which is what the reopen
     * backoff counts from. Left alone on a later failure so the backoff keeps
     * growing instead of restarting at half a second. */
    if (g_master_absent_ms == 0)
        g_master_absent_ms = now_ms();
}

/* ---- the HDMI mirror -------------------------------------------------------
 *
 * A second, ADVISORY output: the same master samples the card is being fed, sent
 * to whatever HDMI sink is attached, so the person looking at the screen hears
 * what the room hears. The operator asked for both at once, and "both" is the
 * constraint that shapes everything here.
 *
 * Advisory is meant in the strict sense. Every rule below exists to keep this
 * device from reaching the master's path:
 *
 *   - Opened SND_PCM_NONBLOCK and written non-blocking, so a sink that has been
 *     unplugged mid-set costs one -EAGAIN and a counter, never a stall on rbp's
 *     audio thread. A monitor is hot-swapped on this unit with the player running.
 *   - An ALSA `multi` PCM would have been the config-only answer, and it is
 *     all-or-nothing: if the HDMI slave will not open, the FLX4 goes silent too.
 *     That is why this is a shim change rather than an asound.conf one.
 *   - Opened on this shim's own initiative from rbp's first snd_pcm_open() and
 *     from the write path, never from a second thread: the shim links only
 *     libc/libdl by design, and -lpthread is not to be added for this.
 *   - Considered UP only once a write has actually delivered frames. An open that
 *     succeeds against a port with nothing on it must not read as working.
 *   - Recovered by a frame-counted backoff, and its recovery deliberately does NOT
 *     re-arm the startup mute the way reopen_try() does for the master: that mute
 *     exists because the FLX4 thumps when its card is opened, and copying it here
 *     would silence the FLX4 for 1.8 s every time the HDMI side hiccuped.
 *
 * The master's own write, its prepare-and-retry, its master_lost(-ENODEV) path and
 * its size accounting are untouched by all of it. flush_master() calls in here
 * after its device branch has finished, and nothing here returns anything to it.
 */

/* s24pack's format as ALSA's own number: the hand-rolled constants at the top of
 * this file, which check_format_constants() has already asked libasound to name. */
static int mirror_alsa_format(int fmt)
{
    if (fmt == AUDIO_FMT_S24_3LE)     return SND_PCM_FORMAT_S24_3LE;
    if (fmt == AUDIO_FMT_S16_LE)      return SND_PCM_FORMAT_S16_LE;
    if (fmt == AUDIO_FMT_SUBFRAME_LE) return SND_PCM_FORMAT_IEC958_SUBFRAME_LE;
    return SND_PCM_FORMAT_S24_LE;
}

/* Write up to MIRROR_PREFILL_FRAMES of silence, non-blocking and best-effort. The
 * return is deliberately dropped: a ring with room takes it, a ring without answers
 * -EAGAIN, and neither is anything the master's audio thread should act on. */
static void mirror_write_silence(snd_pcm_t *h, unsigned long frames)
{
    if (h == NULL || frames == 0 || real_snd_pcm_writei == NULL)
        return;
    if (frames > (unsigned long)MIRROR_PREFILL_FRAMES)
        frames = MIRROR_PREFILL_FRAMES;
    real_snd_pcm_writei(h, g_mirror_silence, (snd_pcm_uframes_t)frames);
}

/* Give the mirror a lead before it is asked to keep up: half the ring of silence,
 * once per stream start — at open, and again whenever a broken stream has been
 * prepared. MIRROR_PREFILL_FRAMES carries the measurement that makes this necessary
 * rather than tidy.
 *
 * The frames are deliberately NOT counted and NOT logged per call: they are not the
 * master's output, so they must not reach g_mirror_frames (which the UP line and the
 * drift arithmetic both mean as delivered audio), and the RETRY path this runs on
 * was taken once per block on this unit — a line here would have been the flood the
 * restamp below already cost. What goes on the record is the number itself, on the
 * OPEN line. */
static void mirror_prefill(snd_pcm_t *h)
{
    mirror_write_silence(h, g_mirror_prefill);
}

/* Hold the ring near half full while the mirror is up, by writing a few frames of
 * silence before the master's block when the level has fallen.
 *
 * This is the drop's twin, and the measurement is why it exists: the vc4 HDMI sink
 * consumes ~6.9 frames a second faster than the FLX4 feeds the mirror — ~155 ppm,
 * two crystals with no resampler between them — so a lead sized by the prefill alone
 * drains away and the stream reaches XRUN in about 74 s. The recovery from that is a
 * prepare plus a re-prefill, i.e. an 11.6 ms gap in the monitor's audio every minute
 * and a quarter, which is exactly the kind of "works, mostly" this port does not
 * ship when the alternative is a few frames of silence per second.
 *
 * The correction is bounded by MIRROR_PAD_MAX and takes several blocks to work off,
 * so it cannot arrive as a burst; at the measured drift it is about one frame every
 * 0.2 s, which is 23 us of silence — a number this code does not get to choose,
 * because it follows from the two clocks.
 *
 * A device that will not report a level (no symbol, or -EPIPE on a stream that is
 * about to be retried anyway) simply gets no padding: the prefill and the RETRY
 * recovery still carry it, exactly as they did before this existed. */
static void mirror_pad(void)
{
    long avail;
    unsigned long pad;

    if (g_mirror == NULL || g_mirror_buffer == 0 || real_snd_pcm_avail_update == NULL)
        return;
    avail = (long)real_snd_pcm_avail_update(g_mirror);
    pad = mirror_pad_frames(avail, g_mirror_buffer, g_mirror_buffer / 2,
                            MIRROR_PAD_MAX);
    if (pad == 0)
        return;
    mirror_write_silence(g_mirror, pad);
    g_mirror_pads++;
    g_mirror_pad_frames_sum += pad;
}

/* Open and configure one candidate, leaving it in g_mirror. Returns 0, or a
 * negative errno having freed and closed everything it opened.
 *
 * Every call here is the REAL function rather than the interposed one, because
 * this is the shim configuring a device of its own: the entry points above answer
 * for the handles rbp holds, and the mirror's handle is not one of them. What lies
 * BELOW these calls does re-enter them — libasound's plug layer configures and
 * drives the hw slave through the public snd_pcm_* symbols — which is why the
 * re-entrancy guard in snd_pcm_open(), the is_forwardable() branches in the
 * parameter plumbing, and the is_rbp_master() scoping of the format override are
 * all load-bearing for this device and were not for a hw: one. */
static int mirror_open_one(const char *dev)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_t *h = NULL;
    unsigned int rate = MIRROR_RATE;
    unsigned int periods = MIRROR_PERIODS;
    snd_pcm_uframes_t period = MIRROR_PERIOD;
    int err;
    /* Which step is running, for the failure line at the bottom. This sequence has
     * libasound's plug layer underneath it, so a negative result says nothing about
     * which call produced it — a candidate that opens and then rejects a parameter
     * returns exactly what a candidate that never opened returns, and the first
     * version of this function logged only that number, which left -EINVAL
     * unattributable between fifteen calls. The measurement it cost: a candidate
     * that aplay configures without complaint (aplay -D plughw:CARD=vc4hdmi0,DEV=0
     * -t raw -f S24_3LE -c 2 -r 44100 -d 1 /dev/zero, rc=0) and this function
     * reported as res=-22. */
    const char *step = "open";

    if (!real_snd_pcm_open || !real_snd_pcm_close ||
        !real_snd_pcm_hw_params_malloc || !real_snd_pcm_hw_params_free ||
        !real_snd_pcm_hw_params_any || !real_snd_pcm_hw_params_set_access ||
        !real_snd_pcm_hw_params_set_format || !real_snd_pcm_hw_params_set_channels ||
        !real_snd_pcm_hw_params_set_rate_near ||
        !real_snd_pcm_hw_params_set_period_size_near ||
        !real_snd_pcm_hw_params_set_periods_near || !real_snd_pcm_hw_params ||
        !real_snd_pcm_sw_params_malloc || !real_snd_pcm_sw_params_free ||
        !real_snd_pcm_sw_params_current ||
        !real_snd_pcm_sw_params_set_start_threshold || !real_snd_pcm_sw_params ||
        !real_snd_pcm_prepare)
        return -ENOSYS;

    /* The guard is what keeps a plughw: candidate's slave open from consuming a
     * slot in g_playback_open_count and being handed a fake handle. */
    g_in_real_open = 1;
    err = real_snd_pcm_open(&h, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    g_in_real_open = 0;
    if (err < 0)
        goto fail;                       /* `step` is still "open" */

    step = "hw_params_malloc";
    if ((err = real_snd_pcm_hw_params_malloc(&hw)) < 0) goto fail;
    step = "hw_params_any";
    if ((err = real_snd_pcm_hw_params_any(h, hw)) < 0) goto fail;
    step = "set_access";
    if ((err = real_snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) goto fail;
    step = "set_format";
    if ((err = real_snd_pcm_hw_params_set_format(h, hw, mirror_alsa_format(g_cfg.mirror_fmt))) < 0) goto fail;
    step = "set_channels";
    if ((err = real_snd_pcm_hw_params_set_channels(h, hw, 2)) < 0) goto fail;
    /* _near for the rate and the geometry, like rbp does for the master: the sink
     * is entitled to answer with something else, and what it answered with is what
     * the log line reports. */
    step = "set_rate_near";
    if ((err = real_snd_pcm_hw_params_set_rate_near(h, hw, &rate, NULL)) < 0) goto fail;
    step = "set_period_size_near";
    if ((err = real_snd_pcm_hw_params_set_period_size_near(h, hw, &period, NULL)) < 0) goto fail;
    step = "set_periods_near";
    if ((err = real_snd_pcm_hw_params_set_periods_near(h, hw, &periods, NULL)) < 0) goto fail;
    step = "hw_params";
    if ((err = real_snd_pcm_hw_params(h, hw)) < 0) goto fail;

    step = "sw_params_malloc";
    if ((err = real_snd_pcm_sw_params_malloc(&sw)) < 0) goto fail;
    step = "sw_params_current";
    if ((err = real_snd_pcm_sw_params_current(h, sw)) < 0) goto fail;
    /* start_threshold 1, and the mirror does not work without it. hw_params leaves
     * a playback stream's threshold at the buffer size, so a writer that never
     * fills the ring never starts the stream: every write succeeds into a buffer
     * that is never drained, the counters advance, and the sink is silent. It is
     * the one setting here whose absence would look exactly like success. */
    step = "set_start_threshold";
    if ((err = real_snd_pcm_sw_params_set_start_threshold(h, sw, 1)) < 0) goto fail;
    step = "sw_params";
    if ((err = real_snd_pcm_sw_params(h, sw)) < 0) goto fail;
    step = "prepare";
    if ((err = real_snd_pcm_prepare(h)) < 0) goto fail;

    if (hw) real_snd_pcm_hw_params_free(hw);
    if (sw) real_snd_pcm_sw_params_free(sw);

    g_mirror = h;
    /* Half the ring the sink actually granted, not half of what was asked for: the
     * two differ whenever the device rounds a _near request, and this is the lead
     * that has to fit in the buffer that exists. Clamped to the silence buffer,
     * which is sized for the requested geometry. A device that granted no geometry
     * at all (both zero) leaves the prefill at 0, i.e. the old behaviour, which the
     * OPEN line then shows as prefill=0 rather than leaving it to be inferred. */
    g_mirror_prefill = (unsigned long)period * (unsigned long)periods / 2;
    if (g_mirror_prefill > (unsigned long)MIRROR_PREFILL_FRAMES)
        g_mirror_prefill = MIRROR_PREFILL_FRAMES;
    /* The ring the sink granted, kept for mirror_pad()'s level arithmetic — it is the
     * same product the prefill is half of, and it is recorded rather than re-derived
     * because a device that grants no geometry must not have one invented for it. */
    g_mirror_buffer = (unsigned long)period * (unsigned long)periods;
    /* The precision is what bounds this: a candidate comes from g_mirror_cands,
     * which is this size, but -Wformat-truncation cannot see that and is right not
     * to guess. */
    snprintf(g_mirror_dev, sizeof g_mirror_dev, "%.*s",
             (int)sizeof g_mirror_dev - 1, dev);
    alog("audioshim: mirror OPEN %s fmt=%s rate=%u period=%lu periods=%u "
         "buffer=%lu prefill=%lu handle=%p\n",
         dev, g_cfg.mirror_fmt_name, rate, (unsigned long)period, periods,
         g_mirror_buffer, g_mirror_prefill, h);

    /* And the lead itself, which is what makes the ring a ring rather than a
     * one-block staging area: see MIRROR_PREFILL_FRAMES. The stream starts here
     * (start_threshold 1), so nothing after this write is a first write. */
    mirror_prefill(h);

    /* Not "UP": that word is earned by a write, below. */
    return 0;

fail:
    if (hw) real_snd_pcm_hw_params_free(hw);
    if (sw) real_snd_pcm_sw_params_free(sw);
    /* NULL when the open itself is what failed; libasound's close tolerates that,
     * but the interposed one is what runs here and guarding is free. */
    if (h) real_snd_pcm_close(h);
    /* The step names the call; the number alone does not. */
    alog("audioshim: mirror open('%s') FAILED at %s res=%d (%s)\n",
         dev, step, err, strerror(err < 0 ? -err : err));
    return err;
}

/* Walk the candidate list, taking the first device that opens. A failure to open
 * ANY of them leaves the mirror down and arms the backoff; it is never an error
 * rbp hears about. */
static void mirror_open(void)
{
    int i, err = -ENODEV;
    int quiet;

    if (g_mirror != NULL || g_mirror_ncands == 0)
        return;

    /* Three failed walks are logged in full; after that the per-candidate lines
     * stop. A unit with no monitor on it is a supported configuration, and a line
     * every few seconds forever is how a log stops being read — the same rule (and
     * the same number) the master's reopen_try() follows. */
    quiet = g_mirror_open_fails >= 3;

    for (i = 0; i < g_mirror_ncands; i++) {
        err = mirror_open_one(g_mirror_cands[i]);
        if (err == 0) {
            g_mirror_absent_ms = 0;      /* it is not down any more */
            g_mirror_backoff_ms = MIRROR_REOPEN_MIN_MS;
            g_mirror_open_fails = 0;
            g_mirror_written_once = 0;
            return;
        }
        /* No per-candidate line here: mirror_open_one() has already logged which
         * step failed, and that line carries the device name. */
    }

    /* Restamped on every failed walk, not only the first, and this one line is the
     * difference between a backoff and a permanent green light. reopen_try() above
     * does the same for the master. A stamp that is set once leaves
     * `now - stamp >= backoff` true forever after the first interval, so the wait
     * stops bounding anything and the reopen runs on every audio block: measured on
     * this unit as ~2000 opens and closes a second and 121k log lines in 28 s, with
     * a 500 ms floor and a 5 s ceiling configured. */
    g_mirror_absent_ms = now_ms();
    g_mirror_open_fails++;
    g_mirror_backoff_ms = mirror_next_backoff(g_mirror_backoff_ms,
                                              (unsigned long)g_cfg.mirror_reopen_ms);
    if (!quiet)
        alog("audioshim: mirror NONE — none of the %d candidate(s) opened; next "
             "attempt in %lums (or never, if AUDIO_MIRROR_REOPEN_MS is 0)\n",
             g_mirror_ncands, g_mirror_backoff_ms);
}

/* Close the handle after a hard error. The master keeps serving; this device does
 * not. */
static void mirror_lost(long err)
{
    snd_pcm_t *dead = g_mirror;

    if (dead == NULL)
        return;
    g_mirror = NULL;
    g_mirror_dev[0] = '\0';

    /* Deliberately NOT retire()d, and this is the one place a future reader is
     * likely to "fix" something that is not broken: is_master() counts a retired
     * handle as the master stream, so retiring this one would have rbp's next
     * write folded into flush_master() a second time as if it were the card's.
     * Nothing but this shim ever holds this pointer, so there is nothing to
     * protect it from. */
    if (real_snd_pcm_close)
        real_snd_pcm_close(dead);

    g_mirror_losses++;
    g_mirror_absent_ms = now_ms();
    g_mirror_backoff_ms = MIRROR_REOPEN_MIN_MS;
    alog("audioshim: mirror LOST err=%ld (%s) after %lu frames in %lu blocks; "
         "the master is unaffected\n",
         err, strerror(err < 0 ? (int)-err : (int)err),
         g_mirror_frames, g_mirror_blocks);
}

/* Send one block of the master pair to the mirror. Called for every master block,
 * so everything in here is on rbp's audio thread and nothing in here may block. */
static void mirror_write(const int32_t *stereo, unsigned frames)
{
    unsigned bytes;
    long written;
    enum mirror_verdict v;

    if (g_mirror == NULL || frames == 0 || real_snd_pcm_writei == NULL)
        return;

    bytes = s24pack(g_cfg.mirror_fmt, stereo, frames, 2, g_mirror_packed,
                    sizeof g_mirror_packed);
    if (bytes == 0) {
        /* Only reachable if MAX_FRAMES or the buffer sizes above are changed out
         * from under this call: it is sized for the 4-byte container, the worst
         * case. Counted rather than silent, because "the mirror stopped writing"
         * with no line at all is the shape of failure this port keeps chasing. */
        if (!g_mirror_pack_failed) {
            g_mirror_pack_failed = 1;
            alog("audioshim: mirror packing %u frames to %s FAILED — the mirror "
                 "will stay silent\n", frames, g_cfg.mirror_fmt_name);
        }
        g_mirror_drops++;
        return;
    }

    g_mirror_blocks++;
    /* Hold the ring near half full before asking it to take this block: a separate,
     * best-effort silence write, so the block's own verdict and accounting below are
     * untouched by it. See mirror_pad(). */
    mirror_pad();
    written = real_snd_pcm_writei(g_mirror, g_mirror_packed, frames);
    v = mirror_verdict(written, (long)frames);

    if (v == MIRROR_RETRY) {
        /* One prepare and one more attempt: a broken stream on a device that is
         * still there. Note what is NOT done here — no mute, no fade, and no touch
         * of the master's startup state.
         *
         * The prepare is what discards the ring, so the lead has to be rebuilt here
         * and not only at open: this path is taken *because* the stream ran dry, and
         * a fresh prepare with the same 64 frames per block would run dry again
         * immediately — which is precisely the state that produced retries on 3500
         * of 3501 blocks before the prefill existed. Silence, so it costs 11.6 ms of
         * the mirror's own latency and nothing of the master's audio. */
        g_mirror_retries++;
        if (real_snd_pcm_prepare)
            real_snd_pcm_prepare(g_mirror);
        mirror_prefill(g_mirror);
        written = real_snd_pcm_writei(g_mirror, g_mirror_packed, frames);
        v = mirror_verdict(written, (long)frames);
        if (v == MIRROR_RETRY)      /* twice is a stream that is not coming back */
            v = MIRROR_DOWN;
    }

    switch (v) {
    case MIRROR_WROTE:
        g_mirror_frames += frames;
        break;
    case MIRROR_SHORT:
        /* The head of the block went out and the tail did not. The next block
         * carries the audio that follows, so this is a lost tail and not a retry;
         * counting it separately from a whole-block drop is what makes the log say
         * how much audio actually went missing. */
        g_mirror_frames += (unsigned long)written;
        g_mirror_shorts++;
        break;
    case MIRROR_FULL:
        /* Nothing consumed and nothing wrong: the sink's ring is full, which is
         * what two independent clocks drifting into each other look like. The
         * block is dropped and the count is the only record of it. */
        g_mirror_drops++;
        break;
    case MIRROR_RETRY:
        break;              /* classified away above */
    case MIRROR_DOWN:
        mirror_lost(written);
        return;
    }

    if (!g_mirror_written_once && g_mirror_frames > 0) {
        g_mirror_written_once = 1;
        alog("audioshim: mirror UP %s (first block delivered %ld of %u frames)\n",
             g_mirror_dev, written > 0 ? written : 0, frames);
    }

    if ((g_mirror_blocks % 500) == 1)
        alog("audioshim: mirror #%lu blocks=%lu frames=%lu dropped=%lu short=%lu "
             "retries=%lu lost=%lu pads=%lu padframes=%lu gain=%.3f dev=%s\n",
             g_mirror_blocks, g_mirror_blocks, g_mirror_frames, g_mirror_drops,
             g_mirror_shorts, g_mirror_retries, g_mirror_losses, g_mirror_pads,
             g_mirror_pad_frames_sum, (double)g_mirror_gain,
             g_mirror_dev);
}

/* Reopen a mirror that is down, if the backoff allows it. Runs on the audio
 * thread, once per master block; the frame counter in mirror_reopen_due() is what
 * replaces a timer thread. */
static void mirror_retry(void)
{
    if (g_mirror != NULL || g_mirror_ncands == 0 || real_snd_pcm_open == NULL)
        return;
    if (!mirror_reopen_due(now_ms(), g_mirror_absent_ms, g_mirror_backoff_ms,
                           (unsigned long)g_cfg.mirror_reopen_ms))
        return;
    mirror_open();
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_open(name='%s', stream=%d, mode=%d)\n",
         name ? name : "null", stream, mode);

    /* A plugin device (plughw and friends) opens its slave by calling the PUBLIC
     * snd_pcm_open(), so this function is re-entered from inside our own
     * open_real_device(). Without this, the re-entrant call would fall through to
     * the switch below, consume a slot in g_playback_open_count and hand
     * libasound one of the fake handles — which is_fake() then answers locally for
     * the rest of the process's life, so the plugin believes it configured and
     * drove a device that does not exist and every write surfaces as an
     * unexplained -EINVAL.
     *
     * This is the same defect the parameter plumbing and snd_pcm_close() both had;
     * they were fixed with is_forwardable(), and this is the one entry point where
     * that predicate cannot help, because there is no handle to test yet. It was
     * latent for as long as the master opened a hw: device, and it becomes
     * reachable the moment anything tries a plughw: candidate — the master's own
     * fallback chain has one, and the HDMI mirror's device list has one per entry.
     *
     * Forwarding with the caller's own arguments, exactly as is_forwardable() does:
     * the handle belongs to libasound, and it is not this shim's to renumber. */
    if (g_in_real_open) {
        alog("audioshim: snd_pcm_open re-entered by a plugin for '%s' — forwarding\n",
             name ? name : "null");
        if (real_snd_pcm_open == NULL)
            return -ENODEV;
        return real_snd_pcm_open(pcm, name, stream, mode);
    }

    if (pcm == NULL)
        return -EINVAL;

    if (stream != SND_PCM_STREAM_PLAYBACK) {
        /* Capture / Mic -> always virtual, so nothing can contend for the card. */
        alog("audioshim: mapped virtual Capture device\n");
        *pcm = (snd_pcm_t *)&g_h_cap;
        return 0;
    }

    switch (g_playback_open_count++) {
    case 0:
        /* Output 0: master -> the real device. The mode is remembered before the
         * open, because it is one of the things the reopen has to replay, and this
         * is the only place rbp ever states it. */
        g_nego.mode = mode;
        if (g_real_playback == NULL && real_snd_pcm_open)
            open_real_device(mode, 0);
        *pcm = g_real_playback ? g_real_playback : (snd_pcm_t *)&g_h_master;
        alog("audioshim: stream 0 is the master stream (handle=%p)\n", *pcm);
        /* The HDMI mirror's first attempt, and the only one made from here: after
         * this it is mirror_retry() on the write path, which respects the backoff.
         * Attempted after the master so the master's open, prepare and first write
         * are never queued behind a device that may not answer. `g_playback_open_count`
         * is reset by reset_stream_state(), so case 0 can recur — this flag is what
         * keeps that from becoming an unbounded second attempt. */
        if (!g_mirror_tried) {
            g_mirror_tried = 1;
            mirror_open();
        }
        return 0;

    case 1:
        alog("audioshim: mapped virtual Headphone device\n");
        *pcm = (snd_pcm_t *)&g_h_phone;
        return 0;

    case 2:
        alog("audioshim: mapped virtual Booth device\n");
        *pcm = (snd_pcm_t *)&g_h_booth;
        return 0;

    default:
        alog("audioshim: mapped dummy output device %d\n",
             g_playback_open_count - 1);
        *pcm = (snd_pcm_t *)&g_h_dummy;
        return 0;
    }
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    int ours;

    init_real_alsa();
    alog("audioshim: snd_pcm_close(handle=%p)\n", pcm);

    if (is_fake(pcm)) {
        /* Nothing real was ever opened under these, so there is no device to
         * release — but closing the master still ends the negotiation, and the
         * next open must start a fresh one. */
        if (pcm == (snd_pcm_t *)&g_h_master)
            reset_stream_state();
        return 0;
    }

    /* Anything else is a genuine snd_pcm_t, and there are two kinds of those.
     *
     * Ours, from open_real_device() — and libasound's own. The PLT calls a plugin
     * makes are interposed as well, which is not obvious and is the whole point of
     * this branch: plughw's plug layer tears itself down by calling
     * snd_pcm_close() on its slave hw: handle, so this function is re-entered with
     * a pointer that is neither g_real_playback nor one of the fakes. Swallowing
     * that — as returning 0 here used to — leaves the slave's
     * /dev/snd/pcmC*D*p fd open for the life of the process, so the card stays
     * busy: the next open returns -EBUSY, rbp is handed the fake master instead,
     * and every stream then runs silent. Forward it.
     *
     * snd_ctl_close() below has always done exactly this; this side did not. */
    ours = is_real(pcm);
    if (ours) {
        /* Clear the global *before* the call: the close re-enters this function
         * for the slave, and that inner call must not mistake itself for the
         * master's close. */
        g_real_playback = NULL;
        reset_stream_state();
    }

    if (real_snd_pcm_close) {
        int err = real_snd_pcm_close(pcm);
        alog("audioshim: closed %s handle %p res=%d\n",
             ours ? "the master's" : "a libasound-internal", pcm, err);
        return err;
    }
    return 0;
}

/* ---- parameter plumbing --------------------------------------------------- */

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_any)
        return real_snd_pcm_hw_params_any(pcm, params);
    return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int access)
{
    init_real_alsa();
    alog("audioshim: set_access req=%d\n", access);
    /* Recorded whether or not there is a device to forward to: rbp's request is
     * rbp's request, and on a unit that booted with no card this is the only
     * record of it there will ever be. See g_nego. */
    if (is_rbp_master(pcm))
        g_nego.access = access;
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_access)
        return real_snd_pcm_hw_params_set_access(pcm, params, access);
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int format)
{
    init_real_alsa();
    alog("audioshim: set_format req=%d\n", format);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_format) {
        /* rbp's request is not honoured literally: the device is configured with
         * AUDIO_FMT's format, because that is the one s24pack() actually produces.
         * On the default hw: device that is s24_3le, which is the format the FLX4
         * accepts and the only reason the bytes we hand over are the bytes the card
         * sees. With an unset AUDIO_FMT this is S24_LE, rbp's own container, which
         * is right for a device that converts for us (the plughw fallback
         * candidate).
         *
         * That substitution belongs to rbp's device and to nothing else. A plugin
         * chain configuring its own slave re-enters this function with the format
         * IT needs — for the vc4 HDMI devices that is IEC958_SUBFRAME_LE, the only
         * format those hw devices offer — and answering that with our PCM format
         * fails the slave's negotiation and takes the whole plug chain down with
         * it. Measured on this unit: set_format(S24_3LE) on hw:CARD=vc4hdmi0,DEV=0
         * is a bare -EINVAL, and that device's only format is a 32-bit subframe. */
        int want = format;
        if (is_rbp_master(pcm)) {
            want = SND_PCM_FORMAT_S24_LE;
            if (g_cfg.fmt == AUDIO_FMT_S24_3LE) want = SND_PCM_FORMAT_S24_3LE;
            else if (g_cfg.fmt == AUDIO_FMT_S16_LE) want = SND_PCM_FORMAT_S16_LE;
        }
        int err = real_snd_pcm_hw_params_set_format(pcm, params, want);
        alog("audioshim: real set_format(%d) res=%d\n", want, err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    init_real_alsa();
    alog("audioshim: set_channels req=%u\n", val);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_channels) {
        /* rbp's own request is the one that decides the frame width. A plugin
         * chain re-entering this function for its own slave is asking about that
         * slave and must not be allowed to set g_out_channels from its params. */
        if (is_real(pcm) && g_out_channels == 0)
            g_out_channels = negotiate_channels(params);
        int err = real_snd_pcm_hw_params_set_channels(pcm, params,
                                                      is_real(pcm) ? g_out_channels : val);
        alog("audioshim: real set_channels(%u) res=%d\n",
             is_real(pcm) ? g_out_channels : val, err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_rate_near req=%u\n", val ? *val : 0);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_rate_near) {
        /* rbp asks for 44100 and means it, and the FLX4 runs at 44100 natively, so
         * this is the rate the card is configured with. It is not a conversion
         * point: nothing resamples for us on a hw: device, which is one reason the
         * default is hw: rather than plughw — see the header. The value is forced
         * because rbp asks for it as a *near* request and reads back what it gets. */
        if (val) *val = 44100;
        int err = real_snd_pcm_hw_params_set_rate_near(pcm, params, val, dir);
        alog("audioshim: real set_rate_near res=%d rate=%u\n", err, val ? *val : 0);
        return err;
    }
    if (val) *val = 44100;
    return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
    int err = 0;

    init_real_alsa();
    alog("audioshim: set_period_size_near req=%lu\n", val ? *val : 0);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_period_size_near) {
        err = real_snd_pcm_hw_params_set_period_size_near(pcm, params, val, dir);
        alog("audioshim: real set_period_size_near res=%d period=%lu\n", err, val ? *val : 0);
    }
    /* After the call: what a reopen has to ask for is the period the first
     * negotiation *ended* with, which is not always what rbp asked for. */
    if (is_rbp_master(pcm) && val)
        g_nego.period_size = *val;
    return err;
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    int err = 0;

    init_real_alsa();
    alog("audioshim: set_periods_near req=%u\n", val ? *val : 0);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params_set_periods_near) {
        err = real_snd_pcm_hw_params_set_periods_near(pcm, params, val, dir);
        alog("audioshim: real set_periods_near res=%d periods=%u\n", err, val ? *val : 0);
    }
    if (is_rbp_master(pcm) && val)
        g_nego.periods = *val;
    return err;
}

/* The 2/2 lie, kept deliberately. rbp sizes its stream handling from this and
 * believes every stream it opens is stereo, which is exactly true of what it
 * writes: it hands us 2-channel blocks and the shim spreads them across the
 * card's pairs. Answering with the card's real count would invite rbp to open
 * streams it has no data for. */
int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    (void)params;
    if (val) *val = 2;
    return 0;
}

int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    (void)params;
    if (val) *val = 2;
    return 0;
}

int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int rate)
{
    (void)pcm; (void)params;
    return (rate == 44100) ? 0 : -EINVAL;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_hw_params(pcm=%p)\n", pcm);
    if (is_forwardable(pcm) && real_snd_pcm_hw_params) {
        int err = real_snd_pcm_hw_params(pcm, params);
        alog("audioshim: real hw_params res=%d\n", err);
        return err;
    }
    return 0;
}

/* The software-parameter setters all report success regardless of what the card
 * said. rbp treats a failure here as fatal, and the boundary/threshold values it
 * asks for are the SC Live 4's DSP's, not the FLX4's; refusing them would abort
 * audio setup over numbers that only affect where ALSA thinks the ring wraps. */
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    int err = 0;
    if (is_forwardable(pcm) && real_snd_pcm_sw_params_current)
        err = real_snd_pcm_sw_params_current(pcm, params);
    alog("audioshim: snd_pcm_sw_params_current(pcm=%p) res=%d\n", pcm, err);
    return err;
}

int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *params, snd_pcm_uframes_t *val)
{
    init_real_alsa();
    int err = 0;
    if (real_snd_pcm_sw_params_get_boundary)
        err = real_snd_pcm_sw_params_get_boundary(params, val);
    if (err != 0 || !val || *val == 0) {
        if (val) *val = 0x40000000;
        err = 0;
    }
    alog("audioshim: get_boundary() res=%d val=%lx\n", err, val ? *val : 0);
    return err;
}

int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_rbp_master(pcm))
        g_nego.silence_threshold = val;
    if (is_forwardable(pcm) && real_snd_pcm_sw_params_set_silence_threshold)
        real_snd_pcm_sw_params_set_silence_threshold(pcm, params, val);
    alog("audioshim: set_silence_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_rbp_master(pcm))
        g_nego.silence_size = val;
    if (is_forwardable(pcm) && real_snd_pcm_sw_params_set_silence_size)
        real_snd_pcm_sw_params_set_silence_size(pcm, params, val);
    alog("audioshim: set_silence_size(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_rbp_master(pcm))
        g_nego.start_threshold = val;
    if (is_forwardable(pcm) && real_snd_pcm_sw_params_set_start_threshold)
        real_snd_pcm_sw_params_set_start_threshold(pcm, params, val);
    alog("audioshim: set_start_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_rbp_master(pcm))
        g_nego.stop_threshold = val;
    if (is_forwardable(pcm) && real_snd_pcm_sw_params_set_stop_threshold)
        real_snd_pcm_sw_params_set_stop_threshold(pcm, params, val);
    alog("audioshim: set_stop_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    /* The rbp-facing handle is answered locally with the pointer rbp is really
     * writing to (g_real_playback, which is not the handle rbp holds). Every other
     * real handle is libasound's own — a plugin applying sw_params to its slave —
     * and that one has to be forwarded with the caller's pointer, or the slave
     * keeps alsa's defaults.
     *
     * That is not a tidiness point. hw_params leaves a slave's start_threshold at
     * the buffer size, so with its sw_params swallowed a device sits in a ring
     * that never fills: every write succeeds into the buffer, nothing ever starts,
     * and the log reads perfectly healthy. It is the same failure the parameter
     * setters above were fixed for; these two — this and snd_pcm_prepare() — are
     * the pair that still gated on is_real(). */
    if (is_real(pcm) && real_snd_pcm_sw_params)
        real_snd_pcm_sw_params(g_real_playback, params);
    else if (is_forwardable(pcm) && real_snd_pcm_sw_params)
        real_snd_pcm_sw_params(pcm, params);
    alog("audioshim: snd_pcm_sw_params(pcm=%p) -> ok\n", pcm);
    return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_prepare)
        err = real_snd_pcm_prepare(g_real_playback);
    else if (is_forwardable(pcm) && real_snd_pcm_prepare)
        err = real_snd_pcm_prepare(pcm);
    alog("audioshim: snd_pcm_prepare(pcm=%p) res=%d\n", pcm, err);
    return 0;   /* always succeed: see the sw_params note above */
}

int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
    (void)pcm1; (void)pcm2;
    alog("audioshim: snd_pcm_link intercepted -> success\n");
    return 0;
}

/* ---- the card going away, and coming back ---------------------------------- */

/* One mid-stream loss of the master handle. Modelled on snd_pcm_close()'s own
 * handling, for the same reason it exists there: the close re-enters this shim
 * for the plugin's slave handle, so the global has to be cleared *before* the
 * call or that inner call mistakes itself for the master.
 *
 * `err` is the negative errno the write failed with. It is not assumed to be one
 * value — it is only *acted* on for -ENODEV, but the line names whatever it was,
 * because "the write failed and we did not handle it" is a different report from
 * "the write failed with a number nobody has seen yet". */
static void master_lost(int err)
{
    snd_pcm_t *dead = g_real_playback;

    if (dead == NULL)
        return;

    /* rbp writes the master stream from one thread, so this is not made
     * re-entrant-safe with a lock; what it is made safe against is the inner call
     * snd_pcm_close() makes, which is what the ordering below is for. */
    g_real_playback = NULL;
    retire(dead);

    if (real_snd_pcm_close)
        real_snd_pcm_close(dead);

    /* Deliberately NOT reset_stream_state(): resolve_pairs() has already clamped
     * g_cfg.* against g_out_channels, and clearing the channel count would
     * silently change what the pair indices mean — the cue pair would come back on
     * top of the master. The map is kept and the recovery is logged against it. */
    g_master_losses++;
    g_master_absent_ms = now_ms();
    g_reopen_backoff_ms = REOPEN_MIN_MS;
    g_reopen_failures = 0;
    alog("audioshim: MASTER LOST: the card went away mid-stream (writei said %s, "
         "after %lu write(s) on that handle). Audio is silent until it is back; "
         "this shim is now looking for it. The stream map is kept\n",
         strerror(-err), g_write_count);
}

/* Put a freshly opened handle back into the configuration rbp negotiated on the
 * one that died.
 *
 * Why replay at all, rather than let the next open negotiate: rbp negotiates once,
 * when it opens its stream, and it is never going to do it again — it still holds
 * the old handle and believes that handle is configured. Everything the card needs
 * has to be put there from this side.
 *
 * The values are replayed *through this shim's own setters*, and that is the point
 * rather than a convenience: those setters are what force AUDIO_FMT's format, the
 * negotiated channel count and 44100 — the three things the pair indices were
 * resolved against. Calling the real_* functions here would configure the card
 * with rbp's raw requests instead, which is the one configuration we know the map
 * does not match.
 *
 * The params objects are ours, from libasound's allocators. rbp's are unreachable
 * (it holds them, and one may be alloca'd on a frame that has already returned),
 * and there is no params object anywhere in the process that describes this card:
 * with a real device open, every setter the shim was sent has already been
 * forwarded and the object it was applied to is rbp's.
 *
 * Returns 0 when the card accepted the configuration. */
static int replay_negotiation(snd_pcm_t *pcm)
{
    snd_pcm_hw_params_t *hp = NULL;
    snd_pcm_sw_params_t *sp = NULL;
    snd_pcm_uframes_t period_size = g_nego.period_size;
    snd_pcm_uframes_t boundary = 0;
    unsigned int periods = g_nego.periods;
    unsigned int rate = 44100;
    int dir = 0, err, bad = 0;

    if (!real_snd_pcm_hw_params_malloc || !real_snd_pcm_hw_params_free ||
        !real_snd_pcm_sw_params_malloc || !real_snd_pcm_sw_params_free) {
        alog("audioshim: replay: this libasound has no params allocator, so the "
             "reopened handle cannot be configured\n");
        return -1;
    }
    if (real_snd_pcm_hw_params_malloc(&hp) < 0)
        return -1;
    if (real_snd_pcm_sw_params_malloc(&sp) < 0) {
        real_snd_pcm_hw_params_free(hp);
        return -1;
    }

    /* rbp's order, as far as the order matters at all: every setter has to be
     * applied to the params object before snd_pcm_hw_params() commits it. */
    snd_pcm_hw_params_any(pcm, hp);
    snd_pcm_hw_params_set_access(pcm, hp, g_nego.access);
    /* The format argument is ignored by set_format(), which writes AUDIO_FMT's
     * format; S24_LE is passed because that is rbp's own container and the value
     * is only read by a build where the shim is not doing the deciding. */
    snd_pcm_hw_params_set_format(pcm, hp, SND_PCM_FORMAT_S24_LE);
    snd_pcm_hw_params_set_channels(pcm, hp, g_out_channels);
    snd_pcm_hw_params_set_rate_near(pcm, hp, &rate, &dir);
    if (period_size)
        snd_pcm_hw_params_set_period_size_near(pcm, hp, &period_size, &dir);
    if (periods)
        snd_pcm_hw_params_set_periods_near(pcm, hp, &periods, &dir);

    err = snd_pcm_hw_params(pcm, hp);
    if (err < 0) {
        alog("audioshim: replay: snd_pcm_hw_params failed on the new handle: %s\n",
             strerror(-err));
        bad = 1;
    }

    if (!bad) {
        snd_pcm_sw_params_current(pcm, sp);
        snd_pcm_sw_params_set_silence_threshold(pcm, sp, g_nego.silence_threshold);
        snd_pcm_sw_params_set_silence_size(pcm, sp, g_nego.silence_size);
        snd_pcm_sw_params_set_start_threshold(pcm, sp, g_nego.start_threshold);
        snd_pcm_sw_params_set_stop_threshold(pcm, sp, g_nego.stop_threshold);
        snd_pcm_sw_params_get_boundary(sp, &boundary);
        snd_pcm_sw_params(pcm, sp);
        snd_pcm_prepare(pcm);

        alog("audioshim: replay: access=%d channels=%u rate=%u period=%lu "
             "periods=%u boundary=%lu — the configuration rbp negotiated, put "
             "back on a card libasound has just opened\n",
             g_nego.access, g_out_channels, rate,
             (unsigned long)period_size, periods, (unsigned long)boundary);
    }

    real_snd_pcm_sw_params_free(sp);
    real_snd_pcm_hw_params_free(hp);
    return bad ? -1 : 0;
}

/* One attempt at getting the card back, at most one per backoff interval.
 *
 * Called from the no-device branch of flush_master(), which is where rbp's audio
 * thread already is: no new thread, and the retry cadence follows the one clock
 * that is definitely still running. That branch is also the state a unit boots
 * into with the controller unplugged, so this is both the mid-session recovery and
 * the cold one — they differ only in whether a handle was ever opened.
 *
 * 500 ms, doubling to 5 s and staying there: a USB re-enumeration takes about that
 * long, and a unit with no controller at all must not be probing the card nine
 * times a second forever. */
static void reopen_try(void)
{
    unsigned long long now;
    int quiet;

    if (g_real_playback || !real_snd_pcm_open)
        return;
    now = now_ms();
    if (g_master_absent_ms && now - g_master_absent_ms < g_reopen_backoff_ms)
        return;

    /* The first few failures are reported one by one — that is a card that was
     * there and went away. After that this is a unit that is meant to run without
     * a controller, and the useful thing is silence in the log until something
     * changes. */
    quiet = g_reopen_failures >= 3;
    open_real_device(g_nego.mode, quiet);

    if (!g_real_playback) {
        g_reopen_failures++;
        g_master_absent_ms = now;
        if (g_reopen_backoff_ms < REOPEN_MAX_MS)
            g_reopen_backoff_ms *= 2;
        if (!quiet)
            alog("audioshim: no output device yet (%u attempt(s)); next try in "
                 "%lu ms\n", g_reopen_failures, g_reopen_backoff_ms);
        return;
    }

    g_reopen_failures = 0;
    g_reopen_backoff_ms = REOPEN_MIN_MS;
    g_master_absent_ms = 0;

    alog("audioshim: MASTER RECOVERED: reopened %s%s. Replaying the negotiation "
         "on the new handle\n", g_cfg.dev,
         g_master_losses ? "" : " (it was never open — this is the cold case)");

    if (replay_negotiation(g_real_playback) < 0) {
        snd_pcm_t *bad = g_real_playback;

        g_real_playback = NULL;
        retire(bad);
        if (real_snd_pcm_close)
            real_snd_pcm_close(bad);
        g_master_absent_ms = now;
        alog("audioshim: the reopened handle would not configure; closed it "
             "again and will keep trying\n");
        if (g_reopen_backoff_ms < REOPEN_MAX_MS)
            g_reopen_backoff_ms *= 2;
        return;
    }

    /* The card thumps when it is opened, so a recovery is muted exactly like a
     * start is. Unambiguous in the log, too: this line precedes the release. */
    g_startup_frames_done = 0;
    g_startup_released = 0;

    if (g_channels_assumed)
        alog("audioshim: NOTE the channel count was resolved with no device "
             "present, so it is the 2-channel fallback: only the master pair "
             "(channels %d,%d) is recovered. %s has %u channel(s) and the cue and "
             "booth pairs were dropped before it appeared — restart to get them "
             "back\n", g_cfg.master.a, g_cfg.master.b, g_cfg.dev, g_out_channels);
    else
        alog("audioshim: the pair map is unchanged: master=%d,%d headphones=%d,%d "
             "booth=%d,%d monitor=%d,%d on %u channel(s)\n",
             g_cfg.master.a, g_cfg.master.b, g_cfg.headphones.a, g_cfg.headphones.b,
             g_cfg.booth.a, g_cfg.booth.b, g_cfg.monitor.a, g_cfg.monitor.b,
             g_out_channels);
}

/* ---- the write path ------------------------------------------------------- */

static inline float clamp01(float v)
{
    if (!(v == v)) return 0.0f;   /* NaN */
    if (v < 0.0f)  return 0.0f;
    if (v > 1.0f)  return 1.0f;
    return v;
}

/* rbp's samples are right-justified 24-bit values whose top byte is zero, so the
 * raw word for -1 is the positive 0x00ffffff. This is the expression the SC Live
 * 4 version used, and it is load-bearing for every gain and every peak: multiply
 * the raw word by 0.5 and a full-scale negative sample becomes full-scale
 * positive. It is *not* load-bearing for the packing in s24pack.c, which reads
 * only bits 23..0 and is unaffected — the one exception being the S24_LE
 * container, which copies the whole word and so passes the extension through. */
static inline int32_t sext24(int32_t l)
{
    return (int32_t)(l << 8) >> 8;
}

/* No device: pace the caller with a sleep, so rbp's audio threads run at roughly
 * real time instead of spinning. Applied to *every* stream — the old code only
 * slept on the master path, which left the phone and booth threads busy-looping
 * whenever the device was missing. */
static void pace_without_device(snd_pcm_uframes_t size)
{
    usleep((useconds_t)(size * 1000000L / 44100L));
}

/* Stage one block from a secondary stream. No card access: the master owns the
 * only ALSA stream, and this block is folded in by the next master write. */
static snd_pcm_sframes_t stage_stream(struct stage *st, const struct pair *pair,
                                      const int32_t *src, snd_pcm_uframes_t size)
{
    snd_pcm_uframes_t i;

    if (!pair_mapped(pair)) {
        /* Dropped on purpose (AUDIO_MAP's '-', or the card is too small). Nothing
         * to stage and nothing to say per block; resolve_pairs() already said it
         * once. */
        if (!g_real_playback)
            pace_without_device(size);
        return (snd_pcm_sframes_t)size;
    }

    for (i = 0; i < size; i++) {
        st->buf[i * 2 + 0] = sext24(src[i * 2 + 0]);
        st->buf[i * 2 + 1] = sext24(src[i * 2 + 1]);
    }
    st->frames = (unsigned)size;

    if (!g_real_playback)
        pace_without_device(size);
    return (snd_pcm_sframes_t)size;
}

/* Fold a staged block into the flush buffer, reporting a block-size mismatch
 * once per distinct size rather than every block. */
static void fold_stage(const struct stage *st, const struct pair *pair,
                       unsigned frames)
{
    unsigned n, i;

    if (!pair_mapped(pair) || st->frames == 0)
        return;
    n = st->frames < frames ? st->frames : frames;
    for (i = 0; i < n; i++) {
        g_out[i * g_out_channels + pair->a] = st->buf[i * 2 + 0];
        g_out[i * g_out_channels + pair->b] = st->buf[i * 2 + 1];
    }
}

static snd_pcm_sframes_t flush_master(const int32_t *src, snd_pcm_uframes_t size)
{
    float master_gain = clamp01(g_master_gain);
    /* The HDMI mirror's level, from the unit's own MASTER LEVEL knob -- the one
     * thing here that scales the mirror and not the master. Hoisted out of the
     * loop with master_gain: it is written by the controls shim from the MIDI
     * thread, and sampling it once per flush means every frame of a block is
     * scaled by the same value. Clamped because it arrives over the shared-state
     * contract rather than from our own arithmetic. */
    float mirror_gain = clamp01(g_mirror_gain);
    snd_pcm_uframes_t i;
    unsigned long long t0 = g_startup_frames_done;
    unsigned long long mute = (unsigned long long)g_cfg.mute_frames;
    unsigned long long fade = (unsigned long long)g_cfg.fade_frames;
    int fading = (mute > 0 && t0 < mute + fade);
    static float env_l = 0.0f, env_r = 0.0f;
    static int32_t s_peak_master = 0;
    int32_t bl = 0, br = 0;
    const void *out;
    unsigned out_bytes;

    resolve_pairs();

    /* Cleared every block. With pair routing — unlike the old fixed 8-channel
     * frame, where some branch always wrote every channel — a channel no stream
     * maps to would otherwise go to the card holding whatever the previous block
     * left there. Writing silence on every channel is also what keeps a card
     * happy that mutes a pair it is never fed. */
    memset(g_out, 0, (size_t)size * g_out_channels * sizeof *g_out);

    /* Master, and the monitor pair if one is configured: a copy of the master
     * output, post Main Vol, so it can be used as a second output rather than as
     * a differently-scaled feed. */
    for (i = 0; i < size; i++) {
        int32_t sl = sext24(src[i * 2 + 0]);
        int32_t sr = sext24(src[i * 2 + 1]);
        int32_t al = (sl < 0) ? -sl : sl;
        int32_t ar = (sr < 0) ? -sr : sr;
        float g = master_gain;
        int32_t ol, or_;

        if (al > s_peak_master) s_peak_master = al;
        if (ar > s_peak_master) s_peak_master = ar;
        /* master VU is POST Main-Vol, like a real mixer */
        if ((int32_t)(al * master_gain) > bl) bl = (int32_t)(al * master_gain);
        if ((int32_t)(ar * master_gain) > br) br = (int32_t)(ar * master_gain);

        if (fading) {
            unsigned long long t = t0 + i;
            if (t < mute)
                g = 0.0f;
            else if (fade > 0 && t < mute + fade)
                g *= (float)(t - mute) / (float)fade;
        }
        ol = (int32_t)(sl * g);
        or_ = (int32_t)(sr * g);
        if (pair_mapped(&g_cfg.master)) {
            g_out[i * g_out_channels + g_cfg.master.a] = ol;
            g_out[i * g_out_channels + g_cfg.master.b] = or_;
        }
        if (pair_mapped(&g_cfg.monitor)) {
            g_out[i * g_out_channels + g_cfg.monitor.a] = ol;
            g_out[i * g_out_channels + g_cfg.monitor.b] = or_;
        }

        /* The HDMI mirror's copy of the same two samples: channel 0 is the
         * master's left and channel 1 its right — post Main Vol, post startup
         * mute and fade, exactly the values that went to the card, then scaled
         * by the mirror's own gain from the unit's MASTER LEVEL knob. That gain
         * is applied HERE and only here: the two writes above are untouched, so
         * the knob can never change what the FLX4 receives. It exists because
         * the knob is downstream of the FLX4's USB audio, so the mirror would
         * otherwise stay at full level while the room went quiet.
         *
         * Filled unconditionally rather than only while the mirror is up, so
         * that whether it is up can never change what the master sends, and so
         * the buffer that mirror_write() picks up is always this block's. It is
         * outside the pair_mapped tests on purpose: the mirror is the master
         * OUTPUT, not a destination for the master PAIR, and must still carry
         * audio if AUDIO_MAP's master pair is unmapped. */
        g_mirror_out[i * 2 + 0] = (int32_t)(ol * mirror_gain);
        g_mirror_out[i * 2 + 1] = (int32_t)(or_ * mirror_gain);
    }
    g_startup_frames_done += size;

    /* The staged secondary streams. Their block sizes are rbp's business, and a
     * mismatch is truncated rather than hidden — see the note on struct stage. */
    if (g_phone_stage.frames && g_phone_stage.frames != size &&
        g_phone_stage.warn_size != g_phone_stage.frames) {
        g_phone_stage.warn_size = g_phone_stage.frames;
        alog("audioshim: NOTE the phone stream staged %u frames when the master "
             "flush is %lu; the cue pair is folded over %u frame(s). If the sound "
             "breaks up, this line is why\n",
             g_phone_stage.frames, (unsigned long)size,
             g_phone_stage.frames < size ? g_phone_stage.frames : (unsigned)size);
    }
    if (g_booth_stage.frames && g_booth_stage.frames != size &&
        g_booth_stage.warn_size != g_booth_stage.frames) {
        g_booth_stage.warn_size = g_booth_stage.frames;
        alog("audioshim: NOTE the booth stream staged %u frames against a %lu "
             "frame flush\n", g_booth_stage.frames, (unsigned long)size);
    }
    fold_stage(&g_phone_stage, &g_cfg.headphones, (unsigned)size);
    fold_stage(&g_booth_stage, &g_cfg.booth, (unsigned)size);

    g_write_count++;

    /* publish decaying master peaks for the meters (~1.45 ms/block, 0.995 per
     * block gives a ~300 ms release). The DDJ-FLX4 has none, but the controls
     * shim is the one that decides that, not this one. */
    env_l = ((float)bl > env_l) ? (float)bl : env_l * 0.995f;
    env_r = ((float)br > env_r) ? (float)br : env_r * 0.995f;
    g_vu_peak[0] = (int)env_l;
    g_vu_peak[1] = (int)env_r;

    if (g_cfg.mute_frames && !g_startup_released &&
        g_startup_frames_done >= mute + fade) {
        g_startup_released = 1;
        alog("audioshim: startup mute released after %llu frames\n",
             g_startup_frames_done);
    }

    /* S24_LE is the container rbp already wrote, so the default path hands the
     * flush buffer straight over and the packing never runs. */
    if (g_cfg.fmt == AUDIO_FMT_S24_LE) {
        out = g_out;
        out_bytes = (unsigned)size * g_out_channels * sizeof *g_out;
    } else {
        out_bytes = s24pack(g_cfg.fmt, g_out, (unsigned)size, g_out_channels,
                            g_packed, sizeof g_packed);
        if (out_bytes == 0) {
            /* The destination could not hold the block. Writing a truncated one
             * would be worse than writing nothing, so the master's device write is
             * skipped entirely and it is said once.
             *
             * `out` is what decides that, rather than this function returning here
             * as it used to: the mirror below packs its own two channels into its
             * own buffer, so a master pack failure says nothing about it, and a
             * screen that keeps playing is worth more than the tidy exit. What is
             * skipped for the master is the write, the pacing and the reopen —
             * exactly the three things the early return used to skip. */
            if (!g_pack_failed_logged) {
                g_pack_failed_logged = 1;
                alog("audioshim: packing %u frames x %u ch to %s failed; output "
                     "will be silent until this is fixed\n",
                     (unsigned)size, g_out_channels, g_cfg.fmt_name);
            }
            out = NULL;
        } else {
            out = g_packed;
        }
    }

    if (g_real_playback && real_snd_pcm_writei && out) {
        snd_pcm_sframes_t written =
            real_snd_pcm_writei(g_real_playback, out, size);

        if (written != -ENODEV && written < 0) {
            /* The usual cause is an underrun or an XRUN after a stall; preparing
             * and rewriting once recovers it, and rbp never learns.
             *
             * -ENODEV is the one error this retry is skipped for, and skipping it
             * is the point: it is the measured value once the FLX4 has
             * re-enumerated, it means the handle's card is gone rather than the
             * stream being out of sync, and no amount of preparing revives it.
             * Retrying would double the log rate and delay nothing. */
            if (real_snd_pcm_prepare)
                real_snd_pcm_prepare(g_real_playback);
            written = real_snd_pcm_writei(g_real_playback, out, size);
        }
        if (written == -ENODEV)
            master_lost(-ENODEV);
        if ((g_write_count % 500) == 1)
            alog("audioshim: writei #%lu frames=%lu bytes=%u written=%ld "
                 "peak_m=%d mainvol=%.3f\n",
                 g_write_count, (unsigned long)size, out_bytes, (long)written,
                 s_peak_master, (double)master_gain);
    } else if (out) {
        pace_without_device(size);
        /* No card: either one was never opened (a unit booted with the controller
         * unplugged) or it was lost above. Both recover the same way, and this is
         * the only place a still-running thread already is. */
        reopen_try();
    }

    /* The HDMI mirror, last and unconditionally: it is advisory, so it runs on the
     * block the master has just finished with whatever the master's own outcome
     * was, and nothing it does reaches anything above this line. That includes the
     * case where the FLX4 is gone — the controller being unplugged is precisely
     * when a screen that keeps playing is worth having, and the monitor pair
     * cannot serve that purpose, because it is a channel pair on the missing card.
     * mirror_retry() comes after the write so that a mirror which has just gone
     * down is not immediately reopened with a block that predates the loss. */
    mirror_write(g_mirror_out, (unsigned)size);
    mirror_retry();

    /* Reset every block, not only on the logged one: left to run, this is a
     * running maximum that never comes back down. (Now reached on the pack-failure
     * path as well, which used to return early: a stale peak from before the
     * failure would be printed on the next logged line as though it were current.) */
    s_peak_master = 0;

    return (snd_pcm_sframes_t)size;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    init_real_alsa();
    if (!buffer || size == 0)
        return (snd_pcm_sframes_t)size;

    if (g_playback_open_count == 0)
        resolve_pairs();   /* no device was opened; the map still has to be sane */

    if (size > MAX_FRAMES) {
        /* Truncating a block is a lie about how much was consumed, but the
         * alternative is a buffer overflow. Say it once so a rare large block is
         * a log line and not a mystery. */
        if (!g_big_block_logged) {
            g_big_block_logged = 1;
            alog("audioshim: rbp wrote %lu frames in one call; truncating to %d "
                 "(MAX_FRAMES)\n", (unsigned long)size, MAX_FRAMES);
        }
        size = MAX_FRAMES;
    }

    if (pcm == (snd_pcm_t *)&g_h_phone)
        return stage_stream(&g_phone_stage, &g_cfg.headphones,
                            (const int32_t *)buffer, size);

    if (pcm == (snd_pcm_t *)&g_h_booth)
        return stage_stream(&g_booth_stage, &g_cfg.booth,
                            (const int32_t *)buffer, size);

    if (is_master(pcm))
        return flush_master((const int32_t *)buffer, size);

    if (is_fake(pcm)) {
        /* The capture and the dummy: there is nowhere for this to go. The old code
         * treated any unrecognised handle as the master, which would put an
         * unknown stream onto the master pair; discarding it is the honest answer.
         * Pace it, though, or rbp spins. */
        if (!g_real_playback)
            pace_without_device(size);
        return (snd_pcm_sframes_t)size;
    }

    /* A real handle that is not ours: libasound's own, because a plugin layer
     * writes to its slave with the public snd_pcm_writei(). Pass it through. This
     * is not a nicety — swallowing it would discard every frame the plug layer had
     * just converted, so the card would get silence while the log read healthy,
     * which is the exact failure this port is trying to stop chasing. */
    if (real_snd_pcm_writei)
        return real_snd_pcm_writei(pcm, buffer, size);
    return (snd_pcm_sframes_t)size;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
    (void)pcm;
    /* Clean silence, always, so the capture path can never produce a read error.
     * The 8 is rbp's capture frame (2 channels x 4 bytes) — an assumption about
     * the caller's buffer that has held since the SC Live 4 and is left alone. */
    if (buffer && size > 0)
        memset(buffer, 0, size * 8);
    return (snd_pcm_sframes_t)size;
}

/* ---- control interface ---------------------------------------------------- */

/* Forward to the real control interface only for the card we were told to use.
 * The SC Live 4 version hardcoded hw:1/hw:0/default, which is exactly the
 * assumption this port removes; anything else gets the fake handle so rbp cannot
 * go probing a card we did not select. */
static int ctl_matches_config(const char *name)
{
    if (name == NULL)
        return 0;
    if (g_cfg.card_id[0] && strstr(name, g_cfg.card_id))
        return 1;
    if (strcmp(name, g_cfg.dev) == 0)
        return 1;
    return 0;
}

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_ctl_open(name='%s', mode=%d)\n", name ? name : "null", mode);
    if (ctl == NULL)
        return -EINVAL;
    if (real_snd_ctl_open && ctl_matches_config(name) &&
        real_snd_ctl_open(ctl, name, mode) == 0)
        return 0;
    *ctl = (snd_ctl_t *)0x12345;
    return 0;
}

int snd_ctl_close(snd_ctl_t *ctl)
{
    init_real_alsa();
    alog("audioshim: snd_ctl_close(handle=%p)\n", ctl);
    if (ctl != (snd_ctl_t *)0x12345 && real_snd_ctl_close)
        return real_snd_ctl_close(ctl);
    return 0;
}

int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info)
{
    (void)info;
    alog("audioshim: snd_ctl_pcm_info(ctl=%p)\n", ctl);
    return 0;
}

/* ---- scheduler & affinity -------------------------------------------------
 *
 * Gated by SCHED_RT, which defaults to 1: the interpositions stay in force, so
 * rbp's audio threads run at normal priority and unpinned. That is what kept
 * playback stable on the SC Live 4, where a thread at SCHED_FIFO 98 pinned to
 * core 0 could starve the rest of the machine.
 *
 * SCHED_RT=0 makes every one of them call the real function instead, which is how
 * you measure what the suppression is buying. It is a debugging mode and not the
 * default: at 0 rbp really does take priority 98 on one core, and the documented
 * symptom is the UI stalling while audio keeps going.
 *
 * The real functions come from RTLD_NEXT, resolved lazily on the first call that
 * needs them, so a default run never pays for the lookup.
 *
 * Signatures here are spelled with `void *` / `const void *` where the real ones
 * use sched_param/cpu_set_t, which is why this file does not include <pthread.h>:
 * it would redeclare these with conflicting types. The ABI is identical — every
 * one of them is a pointer — and this is the same spelling the SC Live 4 version
 * compiled against.
 */
static int (*real_pthread_setaffinity_np)(pthread_t, size_t, const void *);
static int (*real_sched_setaffinity)(pid_t, size_t, const void *);
static int (*real_sched_setscheduler)(pid_t, int, const void *);
static int (*real_pthread_setschedparam)(pthread_t, int, const void *);
static int (*real_pthread_setschedprio)(pthread_t, int);
static int (*real_pthread_attr_setschedpolicy)(void *, int);
static int (*real_pthread_attr_setschedparam)(void *, const void *);

static int g_sched_resolved;

/* Resolve RTLD_NEXT once, and answer whether the caller should pass its request
 * through. With SCHED_RT on (the default) this never even looks the symbols up. */
static int sched_passthrough(void)
{
    if (!g_cfg.sched_rt)
        return 0;
    if (!g_sched_resolved) {
        g_sched_resolved = 1;
        real_pthread_setaffinity_np      = dlsym(RTLD_NEXT, "pthread_setaffinity_np");
        real_sched_setaffinity           = dlsym(RTLD_NEXT, "sched_setaffinity");
        real_sched_setscheduler          = dlsym(RTLD_NEXT, "sched_setscheduler");
        real_pthread_setschedparam       = dlsym(RTLD_NEXT, "pthread_setschedparam");
        real_pthread_setschedprio        = dlsym(RTLD_NEXT, "pthread_setschedprio");
        real_pthread_attr_setschedpolicy = dlsym(RTLD_NEXT, "pthread_attr_setschedpolicy");
        real_pthread_attr_setschedparam  = dlsym(RTLD_NEXT, "pthread_attr_setschedparam");
        alog("audioshim: SCHED_RT=0 — rbp's scheduling requests are passed through "
             "to the real functions\n");
    }
    return 1;
}

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const void *cpuset)
{
    if (sched_passthrough() && real_pthread_setaffinity_np)
        return real_pthread_setaffinity_np(thread, cpusetsize, cpuset);
    return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const void *cpuset)
{
    if (sched_passthrough() && real_sched_setaffinity)
        return real_sched_setaffinity(pid, cpusetsize, cpuset);
    return 0;
}

int sched_setscheduler(pid_t pid, int policy, const void *param)
{
    if (sched_passthrough() && real_sched_setscheduler)
        return real_sched_setscheduler(pid, policy, param);
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const void *param)
{
    if (sched_passthrough() && real_pthread_setschedparam)
        return real_pthread_setschedparam(thread, policy, param);
    return 0;
}

int pthread_setschedprio(pthread_t thread, int prio)
{
    if (sched_passthrough() && real_pthread_setschedprio)
        return real_pthread_setschedprio(thread, prio);
    return 0;
}

int pthread_attr_setschedpolicy(void *attr, int policy)
{
    if (sched_passthrough() && real_pthread_attr_setschedpolicy)
        return real_pthread_attr_setschedpolicy(attr, policy);
    return 0;
}

int pthread_attr_setschedparam(void *attr, const void *param)
{
    if (sched_passthrough() && real_pthread_attr_setschedparam)
        return real_pthread_attr_setschedparam(attr, param);
    return 0;
}
