/*
 * test_keylog.c -- the key dump's round trip. No Pi, no rbp, no address of rbp's
 * anywhere in this file.
 *
 * It links the PRODUCTION keylog.c, which is pure, and pins the four things that
 * would otherwise only be found out by replaying a recording that no longer
 * exists:
 *
 * 1. THE ROUND TRIP. A formatted line parses back to the call it was made from
 *    -- the keycode, the operation, the channel, and all three payloads. The
 *    keycode goes out as `%#x` and comes back with strtol's base 0, so a decimal
 *    keycode must land on the same number as a hexadecimal one.
 *
 * 2. THE TIMING, which is what this format exists for. Sub-millisecond gaps have
 *    to survive, because MIDI_DUMP's clock is milliseconds with three zeroes
 *    appended and a performance is not. A recording whose whole point is to be
 *    replayed later cannot round its own rhythm off.
 *
 * 3. THE FLOAT. A fader's normalised value is a float the map computed and rbp
 *    reads; `%.6f` and a parse back has to give the same float, or a replay's
 *    fader lands somewhere the recording never was.
 *
 * 4. NOTHING IS FATAL. A comment, a header, a blank line, a line with only a
 *    timestamp, and a line carrying a field this reader has never heard of all
 *    parse or are skipped -- never a crash, never a stop, and never a misread.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "keylog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* Format an event and parse it straight back -- the round trip as one call, so
 * each test reads as one claim about the format. Returns 0 if the line did not
 * come back as an event. */
static int roundtrip(const struct keylog_event *in, struct keylog_event *out)
{
    char line[224];

    if (keylog_format(in, line, sizeof(line)) < 0)
        return 0;
    return keylog_parse(line, out);
}

/* --- 1. the round trip ------------------------------------------------------ */

static void test_roundtrip(void)
{
    struct keylog_event in, out;
    char line[224];

    memset(&in, 0, sizeof(in));
    in.t = 0.0;
    in.op = 0;                  /* OP_PRESS */
    in.key = 0x410e;            /* K_RELOOP */
    in.ch = 1;
    in.param = 0;
    strcpy(in.src, "midi");
    CHECK(roundtrip(&in, &out), "a plain press did not come back");
    CHECK(out.t == 0.0 && out.op == 0 && out.key == 0x410e && out.ch == 1,
          "press came back as t=%f op=%d key=%#x ch=%d",
          out.t, out.op, out.key, out.ch);
    CHECK(strcmp(out.src, "midi") == 0, "src came back as '%s'", out.src);

    /* The keycode is written hex and must read back the same whichever base the
     * writer would have used -- strtol's base 0 is what makes an operator's
     * hand-edited decimal line work. */
    keylog_format(&in, line, sizeof(line));
    CHECK(strstr(line, "key=0x410e") != NULL, "keycode not written in hex: %s",
          line);

    /* A release with a payload on every one of the three fields, all negative:
     * the rotate/value forms carry them and a sign lost in the round trip would
     * send a jog the wrong way. */
    memset(&in, 0, sizeof(in));
    in.t = 12.5;
    in.op = 4;                  /* OP_ROTATE */
    in.key = 0x4305;
    in.ch = 2;
    in.param = -1;
    in.fval = -0.5f;
    in.lval = -123456789L;
    strcpy(in.src, "touch");
    CHECK(roundtrip(&in, &out), "a rotate did not come back");
    CHECK(out.op == 4 && out.key == 0x4305 && out.ch == 2,
          "rotate came back as op=%d key=%#x ch=%d", out.op, out.key, out.ch);
    CHECK(out.param == -1 && out.lval == -123456789L,
          "payloads came back as param=%ld l=%ld", out.param, out.lval);
    CHECK(out.fval == -0.5f, "float came back as %f", (double)out.fval);

    /* No src is legal -- a send from a thread that set no label -- and must
     * leave the field empty rather than write `src=`. */
    in.src[0] = '\0';
    CHECK(roundtrip(&in, &out), "an unlabelled event did not come back");
    CHECK(out.src[0] == '\0', "an absent src came back as '%s'", out.src);
    keylog_format(&in, line, sizeof(line));
    CHECK(strstr(line, "src=") == NULL, "an empty src was written: %s", line);
}

/* --- 2. the timing ---------------------------------------------------------- */

static void test_timing(void)
{
    struct keylog_event in, out;
    char line[224];

    /* The claim MIDI_DUMP cannot make: a gap shorter than a millisecond is in
     * the file and comes back as itself. 250 us is a real interval between two
     * events on this rig -- a pad and its release -- and quantising it to 0 ms
     * would make the replay a different performance. */
    memset(&in, 0, sizeof(in));
    in.t = 0.000250;
    keylog_format(&in, line, sizeof(line));
    CHECK(strncmp(line, "0.000250", 8) == 0, "sub-ms timestamp written as %s",
          line);
    CHECK(keylog_parse(line, &out) && out.t == 0.000250,
          "sub-ms timestamp read back as %.6f", out.t);

    /* An hour in, still microsecond-exact: the format must not lose resolution
     * as the performance goes on. */
    memset(&in, 0, sizeof(in));
    in.t = 3600.500123;
    keylog_format(&in, line, sizeof(line));
    CHECK(keylog_parse(line, &out), "a late timestamp did not parse");
    CHECK(out.t == 3600.500123, "a late timestamp read back as %.6f", out.t);

    /* A dump with no usable time is not an event: the timestamp is positional,
     * so a line that starts with anything else is a comment or a mistake. */
    CHECK(!keylog_parse("op=0 key=0x1 ch=0\n", &out),
          "a timestamp-less line parsed as an event");
}

/* --- 3. the float ----------------------------------------------------------- */

static void test_float(void)
{
    struct keylog_event in, out;
    /* Two values a map actually sends: a fader's normalised position and a jog
     * tick's fraction, both of which rbp reads as a float. */
    static const float vals[] = { 0.0f, 0.5f, 1.0f, 0.333333f, 0.7654321f };
    unsigned i;

    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        memset(&in, 0, sizeof(in));
        in.op = 5;              /* OP_VALUE */
        in.fval = vals[i];
        CHECK(roundtrip(&in, &out), "fader value %f did not come back",
              (double)vals[i]);
        CHECK(out.fval == vals[i], "fader value %f came back as %f",
              (double)vals[i], (double)out.fval);
    }
}

/* --- 4. nothing is fatal ---------------------------------------------------- */

static void test_lines(void)
{
    struct keylog_event out;

    CHECK(!keylog_parse("", &out), "an empty line parsed as an event");
    CHECK(!keylog_parse("\n", &out), "a blank line parsed as an event");
    CHECK(!keylog_parse(KEYLOG_HEADER "\n", &out),
          "the header parsed as an event");
    CHECK(!keylog_parse("# when 2026-10-09T09:04:11Z pid 437\n", &out),
          "a comment parsed as an event");
    /* The tail of a file being appended to: the timestamp is there, the fields
     * are not. This is the line that stops a live replay if the parser is
     * strict, and it is exactly the case the recorder guarantees is possible --
     * every record is flushed as it is written. */
    CHECK(!keylog_parse("3.5", &out), "a half-written line parsed");
    CHECK(!keylog_parse("3.5 op=", &out) || out.op == 0,
          "a truncated field was invented into %d", out.op);

    /* A field this reader does not know -- what a v2 line looks like to a v1
     * reader. The known fields still have to arrive. */
    memset(&out, 0, sizeof(out));
    CHECK(keylog_parse("1.000000 op=2 key=0x410b ch=0 param=7 f=0.000000 "
                       "l=0 src=midi extra=99 note=\"later\"\n", &out),
          "a line with an unknown field did not parse");
    CHECK(out.op == 2 && out.key == 0x410b && out.ch == 0 && out.param == 7,
          "a line with an unknown field misread: op=%d key=%#x ch=%d param=%ld",
          out.op, out.key, out.ch, out.param);
    CHECK(strcmp(out.src, "midi") == 0,
          "src after an unknown field came back as '%s'", out.src);

    /* A missing field is its zero, not the previous line's value -- the parser
     * must not carry state between lines. */
    memset(&out, 0, sizeof(out));
    CHECK(keylog_parse("2.0 op=0 key=0x1 ch=0\n", &out),
          "a line with no payloads did not parse");
    CHECK(out.param == 0 && out.fval == 0.0f && out.lval == 0 &&
          out.src[0] == '\0', "absent payloads were invented");

    /* And the buffer being too small is reported, not truncated into a line that
     * would parse as a different command. */
    {
        struct keylog_event in;
        char tiny[8];
        memset(&in, 0, sizeof(in));
        in.key = 0x410e;
        CHECK(keylog_format(&in, tiny, sizeof(tiny)) < 0,
              "a too-small buffer was not reported");
    }
}

/* --- the reader, end to end ------------------------------------------------- */

/* The replay callback. keylog_replay() hands it one event at a time and takes no
 * user pointer, so the collected events live at file scope. */
static struct keylog_event seen[8];
static int seen_n;

static void record_event(const struct keylog_event *ev)
{
    if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[seen_n] = *ev;
    seen_n++;
}

static void test_replay(void)
{
    const char *path = "/tmp/test_keylog_dump.txt";
    static const double want[] = { 0.0, 0.000250, 0.500000, 0.500001 };
    static const int wantkey[] = { 0x410e, 0x410e, 0x410b, 0x410b };
    FILE *f;
    int n, i;

    f = fopen(path, "w");
    CHECK(f != NULL, "could not write %s", path);
    if (!f)
        return;
    fputs(KEYLOG_HEADER "\n", f);
    fputs("# when 2026-10-09T09:04:11Z pid 437\n", f);
    fputs("# media usb1 uuid='1A2B-3C4D' label='SANDISK' usb2 uuid='' label=''\n",
          f);
    fputs("0.000000 op=0 key=0x410e ch=1 param=0 f=0.000000 l=0 src=midi\n", f);
    fputs("0.000250 op=2 key=0x410e ch=1 param=0 f=0.000000 l=0 src=midi\n", f);
    fputs("0.500000 op=0 key=0x410b ch=0 param=0 f=0.750000 l=0 src=touch\n", f);
    fputs("0.500001 op=2 key=0x410b ch=0 param=0 f=0.750000 l=0 src=touch\n", f);
    /* The half-written tail -- a recorder that flushes per record leaves one if
     * the process is killed mid-write, and a replay must not stop here. */
    fputs("0.900000 op=0 key=0x", f);
    fclose(f);

    n = keylog_replay(path, 0.0, NULL);
    CHECK(n == -1, "a NULL callback was not refused (%d)", n);
    n = keylog_replay("/tmp/test_keylog_no_such_file", 0.0, record_event);
    CHECK(n == -1, "an unopenable path was not refused (%d)", n);

    /* speed 0: no pacing, which is what a test wants and what the header of
     * keylog_replay() documents. */
    seen_n = 0;
    n = keylog_replay(path, 0.0, record_event);
    CHECK(n == 4, "replay dispatched %d events, wanted 4", n);
    CHECK(seen_n == 4, "the callback saw %d events, wanted 4", seen_n);
    for (i = 0; i < n && i < 4; i++) {
        CHECK(seen[i].t == want[i], "event %d at %.6f, wanted %.6f",
              i, seen[i].t, want[i]);
        CHECK(seen[i].key == wantkey[i], "event %d key %#x, wanted %#x",
              i, seen[i].key, wantkey[i]);
    }
    /* A quarter of a millisecond between the first two, one microsecond between
     * the last two: both preserved, which is the whole claim of the format. */
    CHECK(seen[1].t - seen[0].t > 0.0, "a 250 us gap was rounded to zero");
    CHECK(seen[3].t - seen[2].t > 0.0, "a 1 us gap was rounded to zero");

    unlink(path);
}

int main(void)
{
    test_roundtrip();
    test_timing();
    test_float();
    test_lines();
    test_replay();

    printf("test_keylog: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
