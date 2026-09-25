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
 *  - The device is AUDIO_DEV, default plughw:CARD=DDJFLX4,DEV=0, instead of a
 *    hardcoded "hw:1,0". plughw on purpose: ALSA's plug chain does S24_LE <->
 *    S24_3LE packing and 44.1 -> 48 kHz for free, and rbp only ever speaks S24_LE
 *    at 44.1. AUDIO_FMT is the escape hatch for a hw: device that will not.
 *  - The channel count is negotiated from the real card (AUDIO_CHANNELS=auto) and
 *    clamped to the 8 the output buffer holds, instead of being forced to 8. The
 *    rule is to ask for exactly as many channels as the card has, because the
 *    route plugin is 1:1 only while logical <= hw; ask for more and it downmixes,
 *    which folds the master into the second pair.
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

/* Declarations of the state owned by the controls shim (which is preloaded
 * before us), plus the constructor-time check that it is actually there. */
#include "shmstate.h"
#include "envutil.h"
#include "s24pack.h"

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

/* ALSA's own snd_pcm_format_t values. Spelled out rather than included, because
 * this shim deliberately does not link against the target's ALSA headers — only
 * against libdl and libc — so that it can be built from the armel toolchain
 * without a sysroot for the chroot's libasound. */
#define SND_PCM_FORMAT_S16_LE   2
#define SND_PCM_FORMAT_S24_LE   6
#define SND_PCM_FORMAT_S24_3LE  10

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
 * see docs/13-raspberrypi4.md for how to check it. */
#define AUDIO_DEV_DEFAULT "plughw:CARD=DDJFLX4,DEV=0"

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
static int (*real_snd_ctl_open)(snd_ctl_t **, const char *, int) = NULL;
static int (*real_snd_ctl_close)(snd_ctl_t *) = NULL;

static void init_real_alsa(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

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
    real_snd_ctl_open = dlsym(lib, "snd_ctl_open");
    real_snd_ctl_close = dlsym(lib, "snd_ctl_close");

    alog("audioshim: real ALSA initialized\n");
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

    /* USB audio has no codec power-up transient, so rb.conf ships both of these
     * at 0 on the Pi. The mechanism stays because a card that does thump on open
     * is a real thing, and 44100 is the rate rbp speaks regardless of what the
     * card ends up running at. */
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
         * so it cannot be mistaken for a routing bug. */
        g_out_channels = 2;
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

/* The master stream is the only one backed by a real device. When the device
 * could not be opened at all, stream 0 gets g_h_master instead of a NULL pointer:
 * rbp is not written to handle a NULL pcm, and the write path still needs a
 * recognisable identity so it can pace itself with a sleep. */
static inline int is_master(snd_pcm_t *pcm)
{
    return pcm != NULL &&
           (pcm == g_real_playback || pcm == (snd_pcm_t *)&g_h_master);
}

/* Strictly "this is the handle we were given by a successful real open()". The
 * parameter plumbing forwards only for this one, so with no device every set_*
 * call is answered locally and nothing is called on a NULL handle. */
static inline int is_real(snd_pcm_t *pcm)
{
    return pcm != NULL && pcm == g_real_playback;
}

static void open_real_device(int mode)
{
    /* rbp asks for nonblocking and then relies on the device to pace it. Masking
     * SND_PCM_NONBLOCK is what makes the card the audio clock. */
    int real_mode = mode & ~2;
    int i, err = -1;

    for (i = 0; g_dev_candidates[i] != NULL; i++) {
        err = real_snd_pcm_open(&g_real_playback, g_dev_candidates[i],
                                SND_PCM_STREAM_PLAYBACK, real_mode);
        alog("audioshim: open('%s') mode=%d->%d res=%d handle=%p\n",
             g_dev_candidates[i], mode, real_mode, err, g_real_playback);
        if (err >= 0)
            return;
        g_real_playback = NULL;
    }

    alog("audioshim: NO OUTPUT DEVICE — rbp will run silent and every stream will "
         "sleep to pace itself\n");
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_open(name='%s', stream=%d, mode=%d)\n",
         name ? name : "null", stream, mode);

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
        /* Output 0: master -> the real device. */
        if (g_real_playback == NULL && real_snd_pcm_open)
            open_real_device(mode);
        *pcm = g_real_playback ? g_real_playback : (snd_pcm_t *)&g_h_master;
        alog("audioshim: stream 0 is the master stream (handle=%p)\n", *pcm);
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
    init_real_alsa();
    alog("audioshim: snd_pcm_close(handle=%p)\n", pcm);

    if (is_master(pcm)) {
        if (g_real_playback && real_snd_pcm_close)
            real_snd_pcm_close(g_real_playback);
        g_real_playback = NULL;
        /* A reopen has to renegotiate: the next card may not be the same card. */
        g_playback_open_count = 0;
        g_out_channels = 0;
        g_pairs_resolved = 0;
        g_phone_stage.frames = 0;
        g_phone_stage.warn_size = 0;
        g_booth_stage.frames = 0;
        g_booth_stage.warn_size = 0;
        memset(g_out, 0, sizeof g_out);
    }
    return 0;
}

/* ---- parameter plumbing --------------------------------------------------- */

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_hw_params_any)
        return real_snd_pcm_hw_params_any(g_real_playback, params);
    return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int access)
{
    init_real_alsa();
    alog("audioshim: set_access req=%d\n", access);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_access)
        return real_snd_pcm_hw_params_set_access(g_real_playback, params, access);
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int format)
{
    init_real_alsa();
    alog("audioshim: set_format req=%d\n", format);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_format) {
        /* S24_LE by default, whatever rbp asked for: that is the only thing rbp
         * can produce, and a plughw device converts it onward. When AUDIO_FMT
         * names a real format we ask for *that* instead, so the card receives
         * exactly the bytes s24pack() built and nothing converts behind us. */
        int want = SND_PCM_FORMAT_S24_LE;
        if (g_cfg.fmt == AUDIO_FMT_S24_3LE) want = SND_PCM_FORMAT_S24_3LE;
        else if (g_cfg.fmt == AUDIO_FMT_S16_LE) want = SND_PCM_FORMAT_S16_LE;
        int err = real_snd_pcm_hw_params_set_format(g_real_playback, params, want);
        alog("audioshim: real set_format(%d) res=%d\n", want, err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    init_real_alsa();
    alog("audioshim: set_channels req=%u\n", val);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_channels) {
        if (g_out_channels == 0)
            g_out_channels = negotiate_channels(params);
        int err = real_snd_pcm_hw_params_set_channels(g_real_playback, params,
                                                      g_out_channels);
        alog("audioshim: real set_channels(%u) res=%d\n", g_out_channels, err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_rate_near req=%u\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_rate_near) {
        /* rbp asks for 44100 and means it; the plug chain resamples to whatever
         * the card actually runs at, which is the FLX4's business, not rbp's. */
        if (val) *val = 44100;
        int err = real_snd_pcm_hw_params_set_rate_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_rate_near res=%d rate=%u\n", err, val ? *val : 0);
        return err;
    }
    if (val) *val = 44100;
    return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_period_size_near req=%lu\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_period_size_near) {
        int err = real_snd_pcm_hw_params_set_period_size_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_period_size_near res=%d period=%lu\n", err, val ? *val : 0);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_periods_near req=%u\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_periods_near) {
        int err = real_snd_pcm_hw_params_set_periods_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_periods_near res=%d periods=%u\n", err, val ? *val : 0);
        return err;
    }
    return 0;
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
    if (is_real(pcm) && real_snd_pcm_hw_params) {
        int err = real_snd_pcm_hw_params(g_real_playback, params);
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
    if (is_real(pcm) && real_snd_pcm_sw_params_current)
        err = real_snd_pcm_sw_params_current(g_real_playback, params);
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
    if (is_real(pcm) && real_snd_pcm_sw_params_set_silence_threshold)
        real_snd_pcm_sw_params_set_silence_threshold(g_real_playback, params, val);
    alog("audioshim: set_silence_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_sw_params_set_silence_size)
        real_snd_pcm_sw_params_set_silence_size(g_real_playback, params, val);
    alog("audioshim: set_silence_size(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_sw_params_set_start_threshold)
        real_snd_pcm_sw_params_set_start_threshold(g_real_playback, params, val);
    alog("audioshim: set_start_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_sw_params_set_stop_threshold)
        real_snd_pcm_sw_params_set_stop_threshold(g_real_playback, params, val);
    alog("audioshim: set_stop_threshold(%lu) -> ok\n", val);
    return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_sw_params)
        real_snd_pcm_sw_params(g_real_playback, params);
    alog("audioshim: snd_pcm_sw_params(pcm=%p) -> ok\n", pcm);
    return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_prepare)
        err = real_snd_pcm_prepare(g_real_playback);
    alog("audioshim: snd_pcm_prepare(pcm=%p) res=%d\n", pcm, err);
    return 0;   /* always succeed: see the sw_params note above */
}

int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
    (void)pcm1; (void)pcm2;
    alog("audioshim: snd_pcm_link intercepted -> success\n");
    return 0;
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
        out = g_packed;
        if (out_bytes == 0) {
            /* The destination could not hold the block. Writing a truncated one
             * would be worse than writing nothing, so skip the device entirely
             * and say so once. */
            if (!g_pack_failed_logged) {
                g_pack_failed_logged = 1;
                alog("audioshim: packing %u frames x %u ch to %s failed; output "
                     "will be silent until this is fixed\n",
                     (unsigned)size, g_out_channels, g_cfg.fmt_name);
            }
            return (snd_pcm_sframes_t)size;
        }
    }

    if (g_real_playback && real_snd_pcm_writei) {
        snd_pcm_sframes_t written =
            real_snd_pcm_writei(g_real_playback, out, size);
        if (written < 0) {
            /* The usual cause is an underrun or an XRUN after a stall; preparing
             * and rewriting once recovers it, and rbp never learns. */
            if (real_snd_pcm_prepare)
                real_snd_pcm_prepare(g_real_playback);
            written = real_snd_pcm_writei(g_real_playback, out, size);
        }
        if ((g_write_count % 500) == 1)
            alog("audioshim: writei #%lu frames=%lu bytes=%u written=%ld "
                 "peak_m=%d mainvol=%.3f\n",
                 g_write_count, (unsigned long)size, out_bytes, (long)written,
                 s_peak_master, (double)master_gain);
    } else {
        pace_without_device(size);
    }
    /* Reset every block, not only on the logged one: left to run, this is a
     * running maximum that never comes back down. */
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

    /* A handle we did not hand out: the capture, the dummy, or something rbp
     * invented. The old code treated any unrecognised handle as the master, which
     * would put an unknown stream onto the master pair; discarding it is the
     * honest answer. */
    if (!g_real_playback)
        pace_without_device(size);
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
