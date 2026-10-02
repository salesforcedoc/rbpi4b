/* pcmprobe — name ALSA's snd_pcm_format_t values, list the formats a device
 * actually accepts, and run the configuration sequence a writer has to run.
 *
 * This exists because the audio shim rolls its own copy of the format constants
 * (it deliberately does not link the target's ALSA headers — see the comment on
 * them in scripts/shims/audioshim.c) and one of them was wrong for a long time:
 * S24_3LE was written as 10, which is S32_LE. The symptom was a single -EINVAL
 * from the card that named nothing, and it cost a hardware round-trip to find.
 * This asks libasound to name each integer, so the mapping is measured instead of
 * transcribed, and so the next card's accepted set can be read off in one go.
 *
 * Build and run — it needs libasound, so it links the CHROOT's copy, which is the
 * one the shim itself dlopens (soft-float, from the XDJ-RX3 rootfs):
 *
 *   make -C scripts/shims pcmprobe
 *   qemu-arm -L extracted/XDJRX3-rootfs scripts/shims/pcmprobe            # the enum
 *   qemu-arm -L extracted/XDJRX3-rootfs scripts/shims/pcmprobe hw:0       # + a card
 *   ... scripts/shims/pcmprobe hw:0 2                                     # + sequence
 *
 * A third argument turns on the sequence mode: the device is opened, configured
 * exactly as the audio shim's mirror configures it (RW_INTERLEAVED, a format, 2
 * channels, 44100, 256-frame periods × 4), prepared, written to, and every step is
 * printed with its result. A fourth argument, `nonblock`, opens it non-blocking,
 * and a fifth sets the ACCESS explicitly — 3 is RW_INTERLEAVED, 4 is
 * RW_NONINTERLEAVED, and the difference between them is whether snd_pcm_writei()
 * is allowed to write at all:
 *
 *   ... scripts/shims/pcmprobe hw:CARD=DDJFLX4,DEV=0 32 nonblock 4   # writes -EINVAL
 *
 * A sixth sets the CHANNELS (default 2, clamped to 1..8). It is not cosmetic: a
 * rejected set_access is not fatal here — that is the case the access argument
 * exists to show — so the sequence continues with whatever the params object
 * holds, and the write loop's outcome is only meaningful if the channel count is
 * one the card can actually run. The FLX4 is 4-channel-only, so on it the two
 * decisive runs are:
 *
 *   ... hw:CARD=DDJFLX4,DEV=0 32 nonblock 3 4   # set_access ok, writes ok
 *   ... hw:CARD=DDJFLX4,DEV=0 32 nonblock 4 4   # set_access FAILS, writes -EINVAL
 *
 * The device half wants the card to be visible. On the Pi, run it from inside the
 * chroot, where /dev/snd is bound in and the libasound is again the shim's own:
 *
 *   cp scripts/shims/pcmprobe /opt/rblive4/rbx3-run/tmp/
 *   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe hw:CARD=DDJFLX4,DEV=0
 *   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe plughw:CARD=vc4hdmi0,DEV=0 32 nonblock
 *
 * WHICH libasound is the point of the chroot, and it is not a detail. Measured on
 * 2026-09-27: the host's `aplay -D plughw:CARD=vc4hdmi0,DEV=0 -f S24_3LE` played
 * happily, while the same device through the chroot's older libasound offered the
 * plug exactly one format and rejected S24_3LE with -EINVAL. A host aplay answers
 * a different question from the one the shim is asking, so it is not an oracle for
 * it.
 *
 * No ALSA headers: every type below is opaque to us and the armel toolchain has no
 * sysroot for them. Only the symbol names and their prototypes matter, and those
 * are stable ABI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef void snd_pcm_format_mask_t;
typedef void snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;

extern int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode);
extern int snd_pcm_close(snd_pcm_t *pcm);
extern int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **ptr);
extern void snd_pcm_hw_params_free(snd_pcm_hw_params_t *obj);
extern int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
extern void snd_pcm_hw_params_get_format_mask(snd_pcm_hw_params_t *params,
                                              snd_pcm_format_mask_t *mask);
extern int snd_pcm_format_mask_malloc(snd_pcm_format_mask_t **ptr);
extern void snd_pcm_format_mask_free(snd_pcm_format_mask_t *obj);
extern int snd_pcm_format_mask_test(const snd_pcm_format_mask_t *mask, int format);
extern const char *snd_pcm_format_name(int format);
extern int snd_pcm_format_width(int format);
extern int snd_pcm_format_physical_width(int format);

/* The sequence mode's half. ACCESS_RW_INTERLEAVED is 3 in the enum (MMAP_INTERLEAVED
 * 0, MMAP_NONINTERLEAVED 1, MMAP_COMPLEX 2, RW_INTERLEAVED 3), and it is a literal
 * for the same reason the format numbers are: no headers here.
 *
 * It is the sequence mode's DEFAULT and not its only value: a fifth argument picks
 * another one, which is the difference between a stream snd_pcm_writei() can write
 * and one it cannot. alsa-lib refuses an interleaved write on any access other than
 * RW_INTERLEAVED or MMAP_INTERLEAVED, and it says so with a bare -EINVAL — the same
 * errno a card returns for a hundred other reasons. Measured on 2026-09-27 (see
 * tools/README.md): access 4 = RW_NONINTERLEAVED configures cleanly here, hw_params
 * and prepare both return 0, and then EVERY write returns -EINVAL. */
#define ACCESS_RW_INTERLEAVED 3

/* The write loop's block, in frames, and the widest stream the probe will ask the
 * card for. Both are bounds on the silence buffer rather than preferences: the
 * FLX4 is 4 channels and 256 frames of it is 4 KB, so a buffer sized for the
 * 2-channel case the probe began with is a read past its end. 8 is more than any
 * card this port has met and is what the argument is clamped to, so the buffer
 * arithmetic is bounded by a #define and not by what a caller typed. */
#define PROBE_BLOCK_FRAMES 256
#define PROBE_MAX_CHANNELS 8

extern const char *snd_pcm_access_name(int access);

extern int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                        int access);
extern int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                        int format);
extern int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                          unsigned int val);
extern int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                           unsigned int *val, int *dir);
extern int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm,
                                                  snd_pcm_hw_params_t *params,
                                                  snd_pcm_uframes_t *val, int *dir);
extern int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm,
                                              snd_pcm_hw_params_t *params,
                                              unsigned int *val, int *dir);
extern int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
extern int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *params, unsigned int *val,
                                      int *dir);
extern int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params,
                                             snd_pcm_uframes_t *val, int *dir);
extern int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params,
                                          unsigned int *val);
extern int snd_pcm_hw_params_get_access(const snd_pcm_hw_params_t *params,
                                        unsigned int *val);
extern int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **ptr);
extern void snd_pcm_sw_params_free(snd_pcm_sw_params_t *obj);
extern int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
extern int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
extern int snd_pcm_prepare(snd_pcm_t *pcm);
extern long snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, unsigned long size);

/* The enum currently ends at 43 (U18_3BE). Probe past it and stop at the first
 * value libasound cannot name — which is how we find the end without the header,
 * and why the table above needs no maintenance when ALSA grows. */
#define PROBE_MAX 64

/* The formats worth naming at each stage of the sequence: the three the shim can
 * pack, and the one the vc4 HDMI hw devices offer on their own. Printed by name
 * rather than by number because the number is what the argument is. */
static const int interesting[] = { 2, 6, 18, 32 };   /* S16_LE S24_LE IEC958_SUBFRAME_LE S24_3LE */

/* One step of the sequence. Prints the result either way and answers 1 on failure
 * so the caller can stop where the shim stops. */
static int step(const char *name, int err)
{
    if (err < 0) {
        printf("  %-20s FAILED  %s (%d)\n", name, strerror(-err), err);
        return 1;
    }
    printf("  %-20s ok\n", name);
    return 0;
}

/* What the device is offering AT THIS POINT in the sequence. Not a constant: the
 * plugin chain refines its constraint space as parameters are pinned, which is
 * the whole reason the sequence exists — a mask read straight after hw_params_any
 * (what the mode above prints) is not necessarily the last word. */
static void mask_now(const char *name, snd_pcm_hw_params_t *params, snd_pcm_format_mask_t *mask)
{
    size_t k;
    int any = 0;

    snd_pcm_hw_params_get_format_mask(params, mask);
    printf("  %-20s ", name);
    for (k = 0; k < sizeof interesting / sizeof interesting[0]; k++) {
        if (snd_pcm_format_mask_test(mask, interesting[k])) {
            printf("%s ", snd_pcm_format_name(interesting[k]));
            any = 1;
        }
    }
    printf("%s\n", any ? "" : "(none of S16_LE S24_LE IEC958_SUBFRAME_LE S24_3LE)");
}

/* What the params object is holding as its access, asked of libasound rather than
 * tracked here: the point of the exercise is what it says after a set_access that
 * FAILED. -1 is LAST, i.e. nothing has been set. */
static void access_now(snd_pcm_hw_params_t *params, const char *what)
{
    unsigned int a = 0;

    if (snd_pcm_hw_params_get_access(params, &a) < 0) {
        printf("  %-20s (could not read it)\n", what);
        return;
    }
    printf("  %-20s %d (%s)\n", what, (int)a,
           snd_pcm_access_name((int)a) ? snd_pcm_access_name((int)a) : "unnamed");
}

static int run_sequence(const char *dev, int fmt, int nonblock, int access, int channels)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_format_mask_t *mask = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    unsigned int rate = 44100, periods = 4, got_rate = 0, got_chans = 0;
    snd_pcm_uframes_t period = 256, got_period = 0;
    /* The shim's mirror block shape, in 4-byte words: 256 frames of up to
     * PROBE_MAX_CHANNELS channels. Sized for the widest stream this probe will
     * ask for rather than for the 2-channel one it started as: a 4-channel FLX4
     * write of 256 frames is 4096 bytes, twice the old buffer, and reading past
     * the end of it would be a fault in the instrument. Zero is silence in every
     * format this probe can ask for. */
    static unsigned char silence[PROBE_BLOCK_FRAMES * PROBE_MAX_CHANNELS * 4];
    const char *nm = snd_pcm_format_name(fmt);
    const unsigned long total = 44100 / 10;          /* 0.1 s */
    long written = 0;
    int err, rc = 0, spins = 0;

    printf("\nsequence against %s, fmt=%d (%s), mode=%s, access=%d (%s), channels=%d:\n",
           dev, fmt, nm ? nm : "unnamed", nonblock ? "NONBLOCK" : "blocking",
           access, snd_pcm_access_name(access) ? snd_pcm_access_name(access) : "?",
           channels);

    err = snd_pcm_open(&pcm, dev, 0 /* playback */, nonblock ? 1 : 0);
    if (step("open", err))
        return 1;

    if (snd_pcm_hw_params_malloc(&params) < 0 || snd_pcm_format_mask_malloc(&mask) < 0) {
        printf("  out of memory\n");
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params_any(pcm, params);
    if (step("hw_params_any", err)) { rc = 1; goto done; }
    mask_now("mask:", params, mask);

    /* Reported, and a failure here is deliberately NOT fatal: it is the shim's own
     * behaviour, and the case this probe was extended to explain. A rejected
     * set_access leaves the params object holding whatever hw_params_any() put
     * there, and the commit below then configures the stream with THAT — so a
     * stream can configure, prepare, and still refuse every interleaved write.
     * Stopping at the failure, as this used to, is what hid it. */
    err = snd_pcm_hw_params_set_access(pcm, params, access);
    step("set_access", err);
    access_now(params, "access after set_access:");
    mask_now("mask:", params, mask);

    /* The step the shim's mirror has been failing at, and the reason the probe
     * prints the mask either side of it. */
    err = snd_pcm_hw_params_set_format(pcm, params, fmt);
    if (step("set_format", err)) { rc = 1; goto done; }
    mask_now("mask:", params, mask);

    if (step("set_channels", snd_pcm_hw_params_set_channels(pcm, params, (unsigned)channels))) { rc = 1; goto done; }
    if (step("set_rate_near(44100)",
             snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL))) { rc = 1; goto done; }
    if (step("set_period_size(256)",
             snd_pcm_hw_params_set_period_size_near(pcm, params, &period, NULL))) { rc = 1; goto done; }
    if (step("set_periods(4)",
             snd_pcm_hw_params_set_periods_near(pcm, params, &periods, NULL))) { rc = 1; goto done; }
    if (step("hw_params", snd_pcm_hw_params(pcm, params))) { rc = 1; goto done; }

    snd_pcm_hw_params_get_rate(params, &got_rate, NULL);
    snd_pcm_hw_params_get_period_size(params, &got_period, NULL);
    snd_pcm_hw_params_get_channels(params, &got_chans);

    /* Read, and the read is checked before anything is printed. This value is the
     * whole point of the access argument, so a read that FAILED must not render as
     * a number: with the result ignored it prints the initialiser, "access=0
     * (MMAP_INTERLEAVED)", which is a configuration the card may never have seen
     * — the instrument inventing the measurement. (It is not hypothetical: right
     * after a set_access the card refuses, get_access() fails on the uncommitted
     * params, as print_access() above shows.) */
    {
        unsigned int acc = 0;
        char accbuf[48];

        if (snd_pcm_hw_params_get_access(params, &acc) >= 0)
            snprintf(accbuf, sizeof accbuf, "%u (%s)", acc,
                     snd_pcm_access_name((int)acc) ? snd_pcm_access_name((int)acc)
                                                   : "unnamed");
        else
            snprintf(accbuf, sizeof accbuf, "unreadable (get_access failed)");

        printf("  negotiated:        rate=%u channels=%u period=%lu access=%s\n",
               got_rate, got_chans, (unsigned long)got_period, accbuf);
    }

    if (snd_pcm_sw_params_malloc(&sw) < 0) {
        printf("  out of memory for sw_params\n");
        rc = 1;
        goto done;
    }
    if (step("sw_params_current", snd_pcm_sw_params_current(pcm, sw))) { rc = 1; goto done; }
    /* start_threshold 1 is a macro over the underscore-suffixed function in the
     * headers (SND_PCM_SW_PARAMS_SET_START_THRESHOLD), and the shim sets the same
     * value by dlsym. Reached through the params accessor the macros use, since
     * there is no header here to expand it: the setter is a plain function. */
    {
        extern int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
                                                         snd_pcm_sw_params_t *params,
                                                         snd_pcm_uframes_t val);
        if (step("set_start_threshold(1)",
                 snd_pcm_sw_params_set_start_threshold(pcm, sw, 1))) { rc = 1; goto done; }
    }
    if (step("sw_params", snd_pcm_sw_params(pcm, sw))) { rc = 1; goto done; }
    if (step("prepare", snd_pcm_prepare(pcm))) { rc = 1; goto done; }

    /* Silence, in the shim's own block size. A stream that opens and configures but
     * never drains is the failure mode this catches: without start_threshold 1 a
     * non-blocking writer fills the ring and the stream stays silent forever, and
     * the counters look exactly like success. */
    while ((unsigned long)written < total) {
        long w = snd_pcm_writei(pcm, silence, PROBE_BLOCK_FRAMES);
        if (w == -11 /* -EAGAIN */) {
            if (++spins > 100000) {
                printf("  write              FAILED  the ring never drained (%ld/%lu frames)\n",
                       written, total);
                rc = 1;
                goto done;
            }
            continue;
        }
        if (w < 0) {
            printf("  write              FAILED  %s (%d) after %ld frames\n",
                   strerror(-(int)w), (int)w, written);
            rc = 1;
            goto done;
        }
        written += w;
    }
    printf("  write              ok  %ld frames of silence, %d block(s)\n", written, spins);

done:
    if (sw) snd_pcm_sw_params_free(sw);
    snd_pcm_format_mask_free(mask);
    snd_pcm_hw_params_free(params);
    snd_pcm_close(pcm);
    return rc;
}

int main(int argc, char **argv)
{
    int i;

    /* `physical` is the one to read: it is the bytes-per-sample the card sees, so
     * S24_LE (24 bits in a 4-byte container) and S24_3LE (24 bits in 3) are
     * distinguishable here and nowhere else. That difference is exactly what
     * s24pack()'s AUDIO_FMT names. */
    printf("enum, as the target's own libasound names it:\n");
    for (i = 0; i < PROBE_MAX; i++) {
        const char *nm = snd_pcm_format_name(i);
        int w, pw;
        if (!nm) continue;
        w = snd_pcm_format_width(i);
        pw = snd_pcm_format_physical_width(i);
        if (w < 0 || pw < 0) continue;
        printf("  %2d  %-18s width=%2d physical=%2d\n", i, nm, w, pw);
    }

    if (argc > 1) {
        snd_pcm_t *pcm = NULL;
        snd_pcm_hw_params_t *params = NULL;
        snd_pcm_format_mask_t *mask = NULL;
        int err;

        printf("\nformats %s accepts:\n", argv[1]);
        err = snd_pcm_open(&pcm, argv[1], 0 /* playback */, 0);
        if (err < 0) {
            printf("  open failed: %s (%d)\n", strerror(-err), err);
            return 1;
        }
        if (snd_pcm_hw_params_malloc(&params) < 0 ||
            snd_pcm_hw_params_any(pcm, params) < 0 ||
            snd_pcm_format_mask_malloc(&mask) < 0) {
            printf("  could not read the params: out of memory\n");
            snd_pcm_close(pcm);
            return 1;
        }
        snd_pcm_hw_params_get_format_mask(params, mask);
        for (i = 0; i < PROBE_MAX; i++) {
            const char *nm = snd_pcm_format_name(i);
            if (!nm || snd_pcm_format_width(i) < 0) continue;
            if (snd_pcm_format_mask_test(mask, i))
                printf("  yes  %2d  %-18s physical=%d\n",
                       i, nm, snd_pcm_format_physical_width(i));
        }
        snd_pcm_format_mask_free(mask);
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
    }

    if (argc > 2) {
        /* Clamped, not trusted: `channels` sizes the write's block against the
         * silence buffer, so a typo on the command line must not be able to walk
         * the instrument off the end of it. */
        int chans = argc > 5 ? atoi(argv[5]) : 2;

        if (chans < 1) chans = 1;
        if (chans > PROBE_MAX_CHANNELS) chans = PROBE_MAX_CHANNELS;
        return run_sequence(argv[1], atoi(argv[2]), argc > 3 && !strcmp(argv[3], "nonblock"),
                            argc > 4 ? atoi(argv[4]) : ACCESS_RW_INTERLEAVED, chans);
    }

    return 0;
}
