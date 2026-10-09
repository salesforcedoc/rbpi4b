/*
 * test_vnc_mode.c -- the control-file text, pinned.
 *
 * Only the parsing is worth a test. The file I/O around it is four system calls whose
 * failure modes are all "keep the mode you had", which is observable in the log; the
 * parsing is the part where a file a human typed meets a program that must not guess.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <string.h>

#include "vnc_mode.h"

static int fails, checks;

static void eq_int(long got, long want, const char *what)
{
    checks++;
    if (got != want) { fails++; printf("  FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("  FAIL %s: got %s, want %s\n", what, got ? got : "(null)", want);
    }
}

static void accept(const char *text, int want, const char *what)
{
    int got = -1;
    eq_int(vnc_mode_parse(text, &got), 0, what);
    eq_int(got, want, what);
}

static void reject(const char *text, const char *what)
{
    int got = -1;
    checks++;
    if (vnc_mode_parse(text, &got) == 0) {
        fails++;
        printf("  FAIL %s: \"%s\" was accepted as %s\n", what, text, vnc_mode_name(got));
    }
}

int main(void)
{
    printf("test_vnc_mode\n");

    eq_str(vnc_mode_name(VNC_MODE_RAW), "raw", "raw's name");
    eq_str(vnc_mode_name(VNC_MODE_HWJPEG), "hwjpeg", "hwjpeg's name");
    /* Anything that is not hwjpeg reads as raw, so a corrupt value cannot leave the
     * server in a mode it has no encoder for. */
    eq_str(vnc_mode_name(999), "raw", "an unknown mode names itself raw");

    accept("raw", VNC_MODE_RAW, "raw");
    accept("hwjpeg", VNC_MODE_HWJPEG, "hwjpeg");
    /* What the file actually contains, because it is written with a newline. */
    accept("raw\n", VNC_MODE_RAW, "raw with the newline the writer adds");
    accept("hwjpeg\n", VNC_MODE_HWJPEG, "hwjpeg with a newline");
    /* What an editor or a shell redirect can produce. */
    accept("HWJPEG", VNC_MODE_HWJPEG, "upper case");
    accept("  hwjpeg  ", VNC_MODE_HWJPEG, "surrounded by blanks");
    accept("\thwjpeg\t\n", VNC_MODE_HWJPEG, "tabs and a newline");
    accept("Raw\r\n", VNC_MODE_RAW, "CRLF, as a file edited over SMB arrives");

    /* The refusals matter more than the acceptances: a control file that says
     * something else entirely must not be read as the nearest match. */
    reject("", "an empty file");
    reject("\n", "a newline and nothing else");
    reject("jpeg", "a name we do not have");
    reject("hwjpegx", "a longer word that starts with a mode");
    reject("raw2", "a longer word that starts with a mode");
    reject("hw jpeg", "two words");
    reject("0", "a number");
    /* "raw" is a prefix of nothing and a suffix of nothing, but a substring of a
     * longer word -- and this is the case a strstr-based parser gets wrong. */
    reject("draw", "a word that merely contains raw");

    /* The note is the only way this module ever explains itself, so the handover is
     * worth pinning. The first version of it cleared the buffer and then returned a
     * pointer into it, which handed every caller an empty string -- the log said
     * "mode: " and nothing else, for every diagnosis the module could make. */
    {
        struct vnc_mode m;
        char out[64];
        vnc_mode_init(&m, "/nonexistent/vnc.mode", VNC_MODE_RAW);
        eq_int(vnc_mode_note(&m, out, sizeof out), 0, "an empty note reports nothing");
        snprintf(m.note, sizeof m.note, "mode changed: raw -> hwjpeg");
        eq_int(vnc_mode_note(&m, out, sizeof out), 1, "a note is handed over");
        eq_str(out, "mode changed: raw -> hwjpeg", "the note's text arrives intact");
        eq_int(vnc_mode_note(&m, out, sizeof out), 0, "and having been read, it is gone");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
