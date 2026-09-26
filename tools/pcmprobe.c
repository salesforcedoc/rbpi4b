/* pcmprobe — name ALSA's snd_pcm_format_t values, and list the formats a device
 * actually accepts.
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
 *
 * The device half wants the card to be visible. On the Pi, run it from inside the
 * chroot, where /dev/snd is bound in and the libasound is again the shim's own:
 *
 *   cp scripts/shims/pcmprobe /opt/rblive4/rbx3-run/tmp/
 *   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe hw:CARD=DDJFLX4,DEV=0
 *
 * No ALSA headers: every type below is opaque to us and the armel toolchain has no
 * sysroot for them. Only the symbol names and their prototypes matter, and those
 * are stable ABI.
 */
#include <stdio.h>
#include <string.h>

typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef void snd_pcm_format_mask_t;

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

/* The enum currently ends at 43 (U18_3BE). Probe past it and stop at the first
 * value libasound cannot name — which is how we find the end without the header,
 * and why the table above needs no maintenance when ALSA grows. */
#define PROBE_MAX 64

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
    return 0;
}
