/*
 * test_vnc_live.c -- the sharing switch's text, and the file round-trip, pinned.
 *
 * The parsing is the part where a file a human typed meets a program that must not
 * guess, so it gets the same treatment vnc_mode's got. The round-trip through the file
 * is here as well because this switch, unlike the other two, is the one whose turning
 * opens a framebuffer and a listening port -- a write that silently does not take is a
 * button on the page that appears to do nothing.
 *
 * Runs natively on the Mac: make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vnc_live.h"

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
    eq_int(vnc_live_parse(text, &got), 0, what);
    eq_int(got, want, what);
}

static void reject(const char *text, const char *what)
{
    int got = -1;
    checks++;
    if (vnc_live_parse(text, &got) == 0) {
        fails++;
        printf("  FAIL %s: \"%s\" was accepted as %s\n", what, text, vnc_live_name(got));
    }
}

int main(void)
{
    char path[256];
    struct vnc_live l;
    char out[64];
    FILE *f;

    printf("test_vnc_live\n");

    eq_str(vnc_live_name(VNC_LIVE_ON), "on", "on's name");
    eq_str(vnc_live_name(VNC_LIVE_OFF), "off", "off's name");
    /* Anything that is not on reads as off, and that is the safe direction for a switch
     * whose ON opens the operator's display. */
    eq_str(vnc_live_name(999), "off", "an unknown value names itself off");

    accept("on", VNC_LIVE_ON, "on");
    accept("off", VNC_LIVE_OFF, "off");
    /* What the file actually contains, because it is written with a newline. */
    accept("on\n", VNC_LIVE_ON, "on with the newline the writer adds");
    accept("off\n", VNC_LIVE_OFF, "off with the newline");
    /* What an editor, a shell redirect or a human at a keyboard can produce. */
    accept("ON", VNC_LIVE_ON, "upper case");
    accept("  On  ", VNC_LIVE_ON, "surrounded by blanks");
    accept("\toff\t\n", VNC_LIVE_OFF, "tabs and a newline");
    accept("On\r\n", VNC_LIVE_ON, "CRLF, as a file edited over SMB arrives");
    accept("yes", VNC_LIVE_ON, "yes");
    accept("no", VNC_LIVE_OFF, "no");
    accept("TRUE", VNC_LIVE_ON, "TRUE");
    accept("false", VNC_LIVE_OFF, "false");
    accept("1", VNC_LIVE_ON, "1");
    accept("0", VNC_LIVE_OFF, "0");

    /* The refusals matter more than the acceptances: a file that says something else
     * must not be read as the nearest match. "on" is a substring of "onward" and a
     * prefix of nothing it should claim. */
    reject("", "an empty file");
    reject("\n", "a newline and nothing else");
    reject("onward", "a longer word that starts with on");
    reject("offline", "a longer word that starts with off");
    reject("2", "a number that is neither");
    reject("on off", "two words");
    reject("truee", "a word that contains true");

    /* --- the round trip, which is the whole point of the file ------------------ */
    {
        int n = snprintf(path, sizeof path, "/tmp/vnc_live_test_%d", (int)getpid());
        if (n <= 0 || (size_t)n >= sizeof path) { printf("  no temp path\n"); return 1; }
        unlink(path);

        vnc_live_init(&l, path, VNC_LIVE_OFF);
        eq_int(l.on, VNC_LIVE_OFF, "the default is what init was given");
        eq_int(vnc_live_get(&l), VNC_LIVE_OFF, "a missing file keeps the default");
        eq_int(l.have_mtime, 0, "and does not claim to have read one");
        eq_int(vnc_live_note(&l, out, sizeof out), 0, "a missing default says nothing");

        eq_int(vnc_live_set(&l, VNC_LIVE_ON), VNC_LIVE_ON, "set takes the new state");
        eq_int(l.on, VNC_LIVE_ON, "and the tracker agrees");
        eq_int(vnc_live_note(&l, out, sizeof out), 1, "the turn is worth saying");
        eq_str(out, "sharing turned on", "and it says which way");

        /* A second tracker on the same file must read what the first wrote -- this is
         * the page writing and the session loop reading. */
        {
            struct vnc_live r;
            vnc_live_init(&r, path, VNC_LIVE_OFF);
            eq_int(vnc_live_get(&r), VNC_LIVE_ON, "another reader sees the write");
        }

        /* And a file written by hand, with a newline and odd case, reads the same. */
        f = fopen(path, "w");
        if (!f) { printf("  cannot write %s\n", path); return 1; }
        fputs("OFF\n", f);
        fclose(f);
        /* The mtime check is what makes the common turn free, so a write that lands in
         * the same nanosecond as the last read is the case worth forcing: rewrite the
         * file, then read from a tracker that has never read it. */
        vnc_live_init(&l, path, VNC_LIVE_ON);
        eq_int(vnc_live_get(&l), VNC_LIVE_OFF, "a hand-written OFF is read");

        /* A file with nonsense in it is not an error and does not change the state. */
        f = fopen(path, "w");
        if (!f) { printf("  cannot write %s\n", path); return 1; }
        fputs("maybe\n", f);
        fclose(f);
        vnc_live_init(&l, path, VNC_LIVE_ON);
        eq_int(vnc_live_get(&l), VNC_LIVE_ON, "nonsense keeps the state it had");
        {
            /* The refusal sentence names the file and the word, so it is longer than
             * the 64 bytes the other checks use -- which is itself worth knowing: a
             * buffer sized for the happy path truncates exactly the message that
             * explains a mistake. */
            char big[256];
            eq_int(vnc_live_note(&l, big, sizeof big), 1, "and says so");
            checks++;
            if (!strstr(big, "neither on nor off")) {
                fails++;
                printf("  FAIL the refusal names the problem: got \"%s\"\n", big);
            }
        }

        unlink(path);
    }

    /* The note handover, for the same reason vnc_mode's is pinned: the first version of
     * that one cleared the buffer and returned a pointer into it. */
    {
        vnc_live_init(&l, "/nonexistent/vnc.live", VNC_LIVE_OFF);
        eq_int(vnc_live_note(&l, out, sizeof out), 0, "an empty note reports nothing");
        snprintf(l.note, sizeof l.note, "sharing turned off");
        eq_int(vnc_live_note(&l, out, sizeof out), 1, "a note is handed over");
        eq_str(out, "sharing turned off", "the note's text arrives intact");
        eq_int(vnc_live_note(&l, out, sizeof out), 0, "and having been read, it is gone");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
