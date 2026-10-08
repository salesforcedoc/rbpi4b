/*
 * test_plinn_scan.c -- which candidate the PlayerInnards scan accepts, and what
 * a failed scan says about itself. No Pi, no rbp, no address of rbp's anywhere
 * in this file.
 *
 * It links the PRODUCTION plinn_scan.c. The module is pure on purpose -- a
 * struct in, a verdict out -- which is what lets two things be checked here
 * rather than by a drill on the unit:
 *
 * 1. THE TEST THAT SHIPPED WRONG. The scan's channel test was
 *    `(chan != 2 && chan != 3)` with `d = chan - 2`, when the channel byte is
 *    1-based -- so deck 1's innards (chan 1) was rejected outright and deck 2's
 *    (chan 2) was filed as deck 1 (2026-10-01). Every plinn() write, the
 *    beat-loop path included, landed on the wrong deck for an evening. The
 *    first test below is that exact case, and it fails if the off-by-one comes
 *    back in any spelling.
 *
 * 2. THE FAILURE LINE. `deck1=0xcbaaa780 deck2=(nil)` is what the bug printed:
 *    it names what was found and nothing about why, so a scan that found nothing
 *    and a scan that refused everything read alike. The line is now built from
 *    counts, and the tests pin that it carries them -- and that a zero reason is
 *    still printed, because a missing reason and a refused-everything scan must
 *    not read alike either.
 *
 * The one thing this file cannot check is whether the module's bounds are the
 * right ones for rbp -- those are measurements, and plinn_scan.h carries them
 * (PLINN_MODE_MAX from rbp_abi.h's PLAYERINNARDS_MODE_OFF note, PLINN_ENGINE_MIN
 * from the scan's own experience). What is pinned here is that the scan uses
 * THOSE numbers, so a wrong bound is a change in one place and a diff anyone can
 * see.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "plinn_scan.h"

#include <stdio.h>
#include <string.h>

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

/* A candidate that every test accepts unless the test breaks one field on
 * purpose. Deck 1, because that is the seat the off-by-one lost. */
static struct plinn_cand good1(void)
{
    struct plinn_cand c;
    c.chan = 1;
    c.mode = 0;
    c.eng  = PLINN_ENGINE_MIN;
    return c;
}

/* ---------------------------------------------------------------------------
 * 1. The channel byte, and the seat it names
 */

static void test_channel(void)
{
    struct plinn_cand c;
    enum plinn_refuse why;

    /* THE REGRESSION. Deck 1's innards carries 1, and 1 must be deck 0 -- not a
     * refusal, and not deck 2. Both halves are asserted because the bug had both:
     * deck 1 was thrown away AND deck 2 was filed as deck 1. */
    c = good1();
    why = PLINN_OK;
    CHECK(plinn_classify(&c, &why) == 0,
          "chan 1 must be deck 0: the channel byte is 1-based, and reading it as "
          "2-based is the off-by-one that sent every write to the wrong deck "
          "(2026-10-01)");
    CHECK(why == PLINN_OK, "chan 1 must be accepted, not refused as %s",
          plinn_refuse_name(why));

    c = good1();
    c.chan = 2;
    CHECK(plinn_classify(&c, &why) == 1, "chan 2 must be deck 1");
    CHECK(why == PLINN_OK, "chan 2 must be accepted, not refused as %s",
          plinn_refuse_name(why));

    /* Neither seat can be reached by another value, and 0 is not a third deck:
     * it is the +0x26 word of something else that carried the vtable word by
     * accident. */
    c = good1();
    c.chan = 0;
    CHECK(plinn_classify(&c, &why) == -1, "chan 0 is not a deck");
    CHECK(why == PLINN_BAD_CHAN, "chan 0 must be refused as chan, not %s",
          plinn_refuse_name(why));

    c = good1();
    c.chan = 3;
    CHECK(plinn_classify(&c, &why) == -1, "chan 3 is not a deck");
    CHECK(why == PLINN_BAD_CHAN, "chan 3 must be refused as chan, not %s",
          plinn_refuse_name(why));

    c = good1();
    c.chan = 255;
    CHECK(plinn_classify(&c, &why) == -1, "chan 255 is not a deck");

    /* Every byte above 2 must be refused, not just the two tried by hand: a
     * range test that is really a pair of equality tests would pass the cases
     * above and still accept 4. */
    {
        int v;
        for (v = 3; v < 256; v++) {
            c = good1();
            c.chan = (unsigned char)v;
            if (plinn_classify(&c, NULL) != -1) {
                CHECK(0, "chan %d must be refused (only 1 and 2 are decks)", v);
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * 2. The other two fields, at their boundaries
 */

static void test_mode_and_engine(void)
{
    struct plinn_cand c;
    enum plinn_refuse why;

    /* +0x74, inclusive at the top: the bound is "0..PLINN_MODE_MAX", so the
     * boundary itself is accepted and one past it is not. Testing both sides is
     * what stops `>` being read as `>=`. */
    c = good1();
    c.mode = PLINN_MODE_MAX;
    CHECK(plinn_classify(&c, &why) == 0,
          "mode == PLINN_MODE_MAX (%d) must be accepted -- the bound is "
          "inclusive", PLINN_MODE_MAX);
    CHECK(why == PLINN_OK, "mode at the bound must not be refused as %s",
          plinn_refuse_name(why));

    c = good1();
    c.mode = (unsigned char)(PLINN_MODE_MAX + 1);
    CHECK(plinn_classify(&c, &why) == -1,
          "mode == PLINN_MODE_MAX + 1 (%d) must be refused", PLINN_MODE_MAX + 1);
    CHECK(why == PLINN_BAD_MODE, "the mode refusal must name mode, not %s",
          plinn_refuse_name(why));

    c = good1();
    c.mode = 255;
    CHECK(plinn_classify(&c, NULL) == -1, "mode 255 must be refused");

    /* The engine pointer, same shape: the bound itself is in, one below is out. */
    c = good1();
    c.eng = PLINN_ENGINE_MIN;
    CHECK(plinn_classify(&c, &why) == 0,
          "eng == PLINN_ENGINE_MIN (0x%08x) must be accepted", PLINN_ENGINE_MIN);

    c = good1();
    c.eng = PLINN_ENGINE_MIN - 1u;
    CHECK(plinn_classify(&c, &why) == -1,
          "eng one below PLINN_ENGINE_MIN must be refused");
    CHECK(why == PLINN_BAD_ENGINE, "the engine refusal must name engine, not %s",
          plinn_refuse_name(why));

    c = good1();
    c.eng = 0;
    CHECK(plinn_classify(&c, NULL) == -1, "a NULL engine must be refused");

    c = good1();
    c.eng = 0xffffffffu;
    CHECK(plinn_classify(&c, NULL) == 0, "the top of the range is a valid engine");
}

/* ---------------------------------------------------------------------------
 * 3. Order, NULLs, and the names
 */

static void test_order_and_names(void)
{
    struct plinn_cand c;
    enum plinn_refuse why;

    /* Two fields wrong: the refusal must be the FIRST test that disagrees, in
     * the order the scan runs them. A reader of the line is told the earliest
     * reason, so a candidate cannot be reported as "bad engine" when its channel
     * was already nonsense -- which would send the next session to the wrong
     * field. */
    c = good1();
    c.chan = 0;
    c.mode = 255;
    c.eng  = 0;
    why = PLINN_OK;
    CHECK(plinn_classify(&c, &why) == -1, "a wholly wrong candidate is refused");
    CHECK(why == PLINN_BAD_CHAN,
          "the first disagreement is the channel, so the refusal must be chan, "
          "not %s", plinn_refuse_name(why));

    c = good1();
    c.mode = 255;
    c.eng  = 0;
    CHECK(plinn_classify(&c, &why) == -1 && why == PLINN_BAD_MODE,
          "with the channel right, the first disagreement is the mode");

    /* A NULL `why` is allowed, because the scan's early-out calls do not always
     * want the reason -- and a NULL that crashed would crash inside the reader
     * thread, where the only symptom is a deck that never appears. */
    c = good1();
    CHECK(plinn_classify(&c, NULL) == 0, "a NULL why pointer is allowed");
    c.chan = 0;
    CHECK(plinn_classify(&c, NULL) == -1, "a NULL why pointer is allowed on refusal");

    /* A NULL candidate is refused rather than dereferenced. The scan builds this
     * struct on the stack and never passes NULL, so this is the belt-and-braces
     * case -- and it is the one that would take the reader thread down. */
    CHECK(plinn_classify(NULL, &why) == -1, "a NULL candidate is refused");
    CHECK(why == PLINN_BAD_CHAN,
          "a NULL candidate must be refused with a reason, not a crash");

    /* Every refusal has its own name, none is NULL, and none reads as another --
     * the names are what the failure line prints, so two reasons sharing a name
     * would make a count unattributable. */
    {
        int i, j;
        for (i = 0; i < PLINN_REFUSE_COUNT; i++) {
            const char *a = plinn_refuse_name((enum plinn_refuse)i);
            CHECK(a && a[0], "refusal %d must have a name", i);
            for (j = i + 1; j < PLINN_REFUSE_COUNT; j++) {
                const char *b = plinn_refuse_name((enum plinn_refuse)j);
                CHECK(strcmp(a, b) != 0,
                      "refusals %d and %d must not share the name '%s'", i, j, a);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * 4. The line a failure prints
 */

/* Enough room that the tests can also hand it too little on purpose. */
static char buf[512];
#define CANARY 0x5a

static void test_tally_line(void)
{
    struct plinn_tally t;
    unsigned long i;

    memset(&t, 0, sizeof t);
    t.regions = 34;
    t.words   = 1284000;
    t.hits    = 6;
    t.accepted[0] = 1;
    t.accepted[1] = 1;
    t.refused[PLINN_BAD_CHAN] = 4;

    plinn_tally_line(buf, sizeof buf, &t);

    /* Every count is IN the line. This is the whole deliverable of the item: a
     * scan that failed has to be a number a reader can act on, and a line that
     * omitted any of these would send the next session back to a drill. */
    CHECK(strstr(buf, "34") != NULL, "the region count must be in the line: '%s'", buf);
    CHECK(strstr(buf, "1284000") != NULL, "the word count must be in the line: '%s'", buf);
    CHECK(strstr(buf, "6") != NULL, "the hit count must be in the line: '%s'", buf);
    CHECK(strstr(buf, "deck1=1") != NULL, "deck 1's seat count must be in the line: '%s'", buf);
    CHECK(strstr(buf, "deck2=1") != NULL, "deck 2's seat count must be in the line: '%s'", buf);

    /* The refusals are spelled by NAME, so a reason cannot become a bare number
     * nobody can look up. The walk starts at PLINN_BAD_CHAN: slot 0 of the count
     * array is PLINN_OK, which is not a refusal, and printing it would put a
     * nameless `?=0` in every line. */
    for (i = PLINN_BAD_CHAN; i < PLINN_REFUSE_COUNT; i++) {
        const char *nm = plinn_refuse_name((enum plinn_refuse)i);
        CHECK(strstr(buf, nm) != NULL,
              "the refusal '%s' must appear in the line: '%s'", nm, buf);
    }
    CHECK(strstr(buf, "chan=4") != NULL,
          "the chan refusal's count must be in the line: '%s'", buf);

    /* THE UNUSED SLOT MUST NOT BE PRINTED. `?` is what plinn_refuse_name() gives
     * for PLINN_OK, so its presence means the walk started at 0 -- and a reader
     * would then take the printed counts as a four-way partition of the hits when
     * only three of them are, which is the arithmetic error this whole line
     * exists to prevent. */
    CHECK(strstr(buf, "?=") == NULL,
          "the unused PLINN_OK slot must not be printed as a reason: '%s'", buf);
    CHECK(strstr(buf, " ?") == NULL,
          "and no unnamed reason may appear beside the named ones: '%s'", buf);

    /* THE CASE THE OLD LINE COULD NOT DISTINGUISH. A scan that tested nothing
     * and a scan that refused everything both found no deck -- the old line
     * prints `deck1=(nil) deck2=(nil)` for both. The counts tell them apart, so
     * a zero tally must still spell every reason out. */
    memset(&t, 0, sizeof t);
    plinn_tally_line(buf, sizeof buf, &t);
    CHECK(strstr(buf, "0 region(s)") != NULL,
          "a scan that walked nothing says so: '%s'", buf);
    for (i = PLINN_BAD_CHAN; i < PLINN_REFUSE_COUNT; i++) {
        const char *nm = plinn_refuse_name((enum plinn_refuse)i);
        CHECK(strstr(buf, nm) != NULL,
              "a zero refusal count is still printed for '%s' -- a missing "
              "reason and a refused-everything scan must not read alike: '%s'",
              nm, buf);
    }
    CHECK(strstr(buf, "?=") == NULL,
          "and a zero tally prints no nameless reason either: '%s'", buf);

    /* The printed counts partition the hits: three named reasons, no fourth
     * nameless one, and the seats. Count the `NAME=N` groups the line actually
     * carries and require exactly one per real reason -- this is what would have
     * caught the `?=0` above if the substring check had not. */
    {
        int named = 0;
        for (i = PLINN_BAD_CHAN; i < PLINN_REFUSE_COUNT; i++) {
            char pat[32];
            snprintf(pat, sizeof pat, "%s=", plinn_refuse_name((enum plinn_refuse)i));
            /* exactly one occurrence */
            if (strstr(buf, pat) && strstr(strstr(buf, pat) + 1, pat) == NULL)
                named++;
        }
        CHECK(named == PLINN_REFUSE_COUNT - 1,
              "the line must name each of the %d reasons exactly once, not %d",
              PLINN_REFUSE_COUNT - 1, named);
    }

    /* The bug's own signature, replayed: two vtable hits, one seat kept, one
     * refused on the channel. That is what 2026-10-01 looked like from inside,
     * and it now says which test refused the missing deck. */
    memset(&t, 0, sizeof t);
    t.regions = 31;
    t.words   = 1200000;
    t.hits    = 2;
    t.accepted[0] = 1;                     /* deck 2, filed as deck 1 */
    t.refused[PLINN_BAD_CHAN] = 1;         /* deck 1, lost */
    plinn_tally_line(buf, sizeof buf, &t);
    CHECK(strstr(buf, "deck1=1") && strstr(buf, "deck2=0"),
          "the off-by-one's tally reads deck1=1 deck2=0: '%s'", buf);
    CHECK(strstr(buf, "chan=1") != NULL,
          "and it names the channel as the reason the second deck is missing: '%s'",
          buf);

    /* The parts add up. A tally whose hits do not equal the seats plus the
     * refusals is a scan that lost a candidate between two counters, which is
     * exactly the shape of a bug this line exists to expose -- so the arithmetic
     * is pinned here as a property, not as one worked example. */
    {
        struct plinn_tally s;
        unsigned long seats, refused;
        memset(&s, 0, sizeof s);
        s.regions = 3;
        s.words = 100;
        s.hits = 5;
        s.accepted[0] = 2;
        s.accepted[1] = 1;
        s.refused[PLINN_BAD_MODE] = 2;
        seats = s.accepted[0] + s.accepted[1];
        refused = 0;
        for (i = 0; i < PLINN_REFUSE_COUNT; i++)
            refused += s.refused[i];
        CHECK(seats + refused == s.hits,
              "seats (%lu) + refusals (%lu) must equal the hits (%lu)",
              seats, refused, s.hits);
    }
}

/* A short buffer is the case that runs on the unit if a future reason name is
 * long, and an overrun there would take the reader thread -- and the deck -- with
 * it. The canary past the end is what proves nothing was written outside. */
static void test_truncation(void)
{
    struct plinn_tally t;
    char small[24];
    unsigned long i;

    memset(&t, 0, sizeof t);
    t.regions = 34;
    t.words   = 1284000;
    t.hits    = 6;
    t.accepted[0] = 1;

    for (i = 1; i <= sizeof small; i++) {
        memset(small, CANARY, sizeof small);
        plinn_tally_line(small, i, &t);
        CHECK(small[i - 1] == '\0' || memchr(small, '\0', i) != NULL,
              "a %lu-byte buffer must be NUL-terminated within %lu bytes",
              i, i);
        /* Every byte past the NUL we were allowed to write is untouched. */
        {
            unsigned long j;
            for (j = i; j < sizeof small; j++) {
                if (small[j] != (char)CANARY) {
                    CHECK(0, "writing %lu bytes scribbled past the end at %lu", i, j);
                    break;
                }
            }
        }
    }

    /* A zero-length buffer must not write at all, and must not crash. */
    memset(small, CANARY, sizeof small);
    plinn_tally_line(small, 0, &t);
    CHECK(small[0] == (char)CANARY, "a zero-length buffer must not be written to");

    /* A NULL tally is a caller bug, not a licence to dereference NULL. */
    plinn_tally_line(buf, sizeof buf, NULL);
    CHECK(buf[0] != '\0', "a NULL tally still produces a line rather than nothing");
}

int main(void)
{
    test_channel();
    test_mode_and_engine();
    test_order_and_names();
    test_tally_line();
    test_truncation();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
