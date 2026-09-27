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
 * printed with its result. A fourth argument, `nonblock`, opens it non-blocking.
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
 * for the same reason the format numbers are: no headers here. */
#define ACCESS_RW_INTERLEAVED 3

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

static int run_sequence(const char *dev, int fmt, int nonblock)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_format_mask_t *mask = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    unsigned int rate = 44100, periods = 4, got_rate = 0, got_chans = 0;
    snd_pcm_uframes_t period = 256, got_period = 0;
    /* The shim's mirror block shape, in 4-byte words: MIRROR_PERIOD frames of 2
     * channels. Zero is silence in every format this probe can ask for. */
    static unsigned char silence[256 * 2 * 4];
    const char *nm = snd_pcm_format_name(fmt);
    const unsigned long total = 44100 / 10;          /* 0.1 s */
    long written = 0;
    int err, rc = 0, spins = 0;

    printf("\nsequence against %s, fmt=%d (%s), mode=%s:\n",
           dev, fmt, nm ? nm : "unnamed", nonblock ? "NONBLOCK" : "blocking");

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

    err = snd_pcm_hw_params_set_access(pcm, params, ACCESS_RW_INTERLEAVED);
    if (step("set_access(3)", err)) { rc = 1; goto done; }
    mask_now("mask:", params, mask);

    /* The step the shim's mirror has been failing at, and the reason the probe
     * prints the mask either side of it. */
    err = snd_pcm_hw_params_set_format(pcm, params, fmt);
    if (step("set_format", err)) { rc = 1; goto done; }
    mask_now("mask:", params, mask);

    if (step("set_channels(2)", snd_pcm_hw_params_set_channels(pcm, params, 2))) { rc = 1; goto done; }
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
    printf("  negotiated:        rate=%u channels=%u period=%lu\n",
           got_rate, got_chans, (unsigned long)got_period);

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
        long w = snd_pcm_writei(pcm, silence, sizeof silence / (2 * 4));
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

    if (argc > 2)
        return run_sequence(argv[1], atoi(argv[2]), argc > 3 && !strcmp(argv[3], "nonblock"));

    return 0;
}
