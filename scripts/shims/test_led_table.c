/*
 * test_led_table.c -- the settled LED table and the blink phase, with no Pi, no
 * rbp and no address of rbp's anywhere in this file.
 *
 * It links the PRODUCTION led_table.c, for the reason test_wave.c links
 * wave_zone.c: what is pinned here is a rule that would otherwise be rewritten
 * in a copy and never noticed.
 *
 * The module is pure on purpose -- buffers in, a buffer out -- which is what
 * makes the two things that matter testable without a unit:
 *
 * 1. THE TORN-READ FILTER. rbp rebuilds uif::LedStat every 20 ms and a read
 *    taken across a rebuild is a state rbp never had. Two reads that agree are
 *    evidence; an entry that disagrees is in flux and must NOT be believed. The
 *    tests below hand the merge a fabricated torn table, including the case
 *    that makes the carry-forward worth having: an entry whose two reads
 *    disagree but which HAS a previous value must keep that value rather than
 *    vanish, or a filter meant to stop a wrong colour would blink an LED dark
 *    instead. And a filter that never fires is indistinguishable from no filter
 *    at all, so the tests also prove it fires -- and that it does not fire on a
 *    table that merely sat still.
 *
 * 2. THE BLINK PHASE, half duty, and the wrap. `now` is an unsigned long long
 *    millisecond count and `long` is four bytes on this target, which this tree
 *    has already paid for once (a nanosecond CLOCK_MONOTONIC in a `long` wraps
 *    every 4.29 s). The clock here passes 2^32 ms within seven weeks of uptime,
 *    so the tests check the phase on both sides of the 32-bit boundary.
 *
 * The one thing this file cannot check is whether rbp's period is really at +28
 * -- that is a measurement, not a rule, and rbp_abi.h's LED_ENTRY_OFF_PERIOD
 * carries it (work/blinkprobe.py is the reading). What is pinned here is that
 * the module uses the offset rbp_abi.h states, so a wrong offset is a change in
 * one place and a diff anyone can see.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "led_table.h"
#include "rbp_abi.h"

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

/* ---------------------------------------------------------------------------
 * A table rbp could have written: 0x2c bytes per entry, id at +0, channel at
 * +4, state at +16, dim at +20, period at +28, RGB at +40.
 */

/* Room for a few entries PAST the module's bound, so the bound can be tested
 * rather than assumed: a sentinel placed there must never be found. */
#define TBL_ENTRIES (LED_DUMP_MAX + 16)
static unsigned char a_tbl[TBL_ENTRIES * LED_ENTRY_SIZE];   /* read A */
static unsigned char b_tbl[TBL_ENTRIES * LED_ENTRY_SIZE];   /* read B */
static unsigned char hist[TBL_ENTRIES * LED_ENTRY_SIZE];    /* previous settled */
static unsigned char out[TBL_ENTRIES * LED_ENTRY_SIZE];

#define TBL_BYTES(n) ((size_t)(n) * LED_ENTRY_SIZE)

static void put32(unsigned char *p, unsigned int v)
{
    memcpy(p, &v, 4);
}

static void entry(unsigned char *t, unsigned int i, unsigned int id,
                  unsigned int ch, unsigned int state, unsigned int period)
{
    unsigned char *e = t + (size_t)i * LED_ENTRY_SIZE;
    memset(e, 0, LED_ENTRY_SIZE);
    put32(e + 0, id);
    put32(e + 4, ch);
    put32(e + LED_ENTRY_OFF_STATE, state);
    put32(e + LED_ENTRY_OFF_PERIOD, period);
    e[40] = (unsigned char)id;
    e[41] = 0x80;
    e[42] = 0xff;
}

static unsigned int get32(const unsigned char *p)
{
    unsigned int v;
    memcpy(&v, p, 4);
    return v;
}

static unsigned int state_of(const unsigned char *t, unsigned int i)
{
    return get32(t + (size_t)i * LED_ENTRY_SIZE + LED_ENTRY_OFF_STATE);
}

static unsigned int id_of(const unsigned char *t, unsigned int i)
{
    return get32(t + (size_t)i * LED_ENTRY_SIZE + 0);
}

/* ---------------------------------------------------------------------------
 * 1. Finding an entry. Every reader in rbp_led.c goes through this, so it is
 *    the thing that decides whether an LED is rbp's or absent.
 */
static void test_find(void)
{
    const unsigned char *e;

    memset(a_tbl, 0, sizeof a_tbl);
    entry(a_tbl, 0, 49, 1, 2, 500);
    entry(a_tbl, 1, 41, 0, 2, 250);
    entry(a_tbl, 2, 18, 1, 1, 0);

    e = led_find(a_tbl, 3, 41, 0);
    CHECK(e != NULL, "id 41 ch 0 was not found");
    CHECK(e && get32(e + LED_ENTRY_OFF_PERIOD) == 250,
          "the entry found for id 41 is not the one that was written");
    CHECK(e && (size_t)(e - a_tbl) == LED_ENTRY_SIZE,
          "the entry found for id 41 is at the wrong position");

    /* THE PAIR IS (id, channel), not id alone. Deck 1 and deck 2 carry the same
     * ids -- PLAY is 49 on both -- so an id-only lookup would light the wrong
     * deck's LED on a two-deck surface. */
    memset(a_tbl, 0, sizeof a_tbl);
    entry(a_tbl, 0, 49, 1, 1, 0);
    entry(a_tbl, 1, 49, 2, 2, 250);
    e = led_find(a_tbl, 2, 49, 2);
    CHECK(e != NULL, "id 49 ch 2 was not found when ch 1 is also present");
    CHECK(e && get32(e + LED_ENTRY_OFF_PERIOD) == 250,
          "the lookup matched the other deck's entry");
    CHECK(led_find(a_tbl, 2, 49, 3) == NULL, "a channel rbp has no entry for was found");
    CHECK(led_find(a_tbl, 2, 4, 1) == NULL, "an id rbp has no entry for was found");

    CHECK(led_find(NULL, 0, 49, 1) == NULL, "a NULL table found an entry");
    CHECK(led_find(a_tbl, 0, 49, 1) == NULL, "an empty table found an entry");

    /* A count above the bound is clamped rather than believed: the count comes
     * out of rbp's memory, and reading past a bound is how a plausible-looking
     * table becomes a fault inside rbp's own process. The sentinel is placed
     * where a missing clamp would reach for it. */
    memset(a_tbl, 0, sizeof a_tbl);
    entry(a_tbl, 0, 49, 1, 1, 0);
    entry(a_tbl, LED_DUMP_MAX, 7, 1, 1, 0);     /* past the bound */
    CHECK(led_find(a_tbl, LED_DUMP_MAX + 10, 49, 1) != NULL,
          "an over-bound count stopped the lookup finding an entry inside it");
    CHECK(led_find(a_tbl, LED_DUMP_MAX + 10, 7, 1) == NULL,
          "an over-bound count was believed and read past the bound");
}

/* ---------------------------------------------------------------------------
 * 2. The torn-read filter.
 */
static void test_merge(void)
{
    unsigned int n;

    /* A table that held still: everything is kept, byte for byte, in rbp's own
     * order. This is the case that must NOT change anything -- a filter that
     * costs a tick on every reading is a filter that blinks LEDs dark. */
    memset(a_tbl, 0, sizeof a_tbl);
    entry(a_tbl, 0, 49, 1, 2, 500);
    entry(a_tbl, 1, 41, 0, 1, 0);
    entry(a_tbl, 2, 18, 1, 1, 0);
    memcpy(b_tbl, a_tbl, TBL_BYTES(3));
    n = led_merge_settled(a_tbl, b_tbl, 3, NULL, 0, out);
    CHECK(n == 3, "a table that read the same twice lost entries (%u)", n);
    CHECK(memcmp(out, a_tbl, TBL_BYTES(3)) == 0,
          "a table that read the same twice came out different");

    /* One entry torn, and NO history: it is dropped. rbp is rebuilding, so the
     * second read has an entry the first did not (here: a colour already
     * cleared). Believing either one would be a state rbp never had. */
    memset(a_tbl, 0, sizeof a_tbl);
    memset(b_tbl, 0, sizeof b_tbl);
    entry(a_tbl, 0, 49, 1, 2, 500);             /* read A: settled */
    entry(a_tbl, 1, 18, 1, 1, 0);               /* read A: the pad */
    entry(b_tbl, 0, 49, 1, 2, 500);             /* read B: settled */
    entry(b_tbl, 1, 18, 1, 0, 0);               /* read B: caught mid-clear */

    memset(out, 0xaa, sizeof out);
    n = led_merge_settled(a_tbl, b_tbl, 2, NULL, 0, out);
    CHECK(n == 1, "a torn entry with no history was kept (%u)", n);
    CHECK(state_of(out, 0) == 2, "the surviving entry is not the settled one");

    /* THE CARRY-FORWARD, and it is a lookup by (id, channel) rather than by
     * position -- rbp's order is rbp's. The history here holds the pad at
     * position 7, the torn read has it at position 1, and the kept value has to
     * be the history's, not the mid-clear one. */
    memset(hist, 0, sizeof hist);
    entry(hist, 7, 18, 1, 1, 0);
    CHECK(id_of(hist, 7) == 18, "the fixture did not place the pad at position 7");

    n = led_merge_settled(a_tbl, b_tbl, 2, hist, 8, out);
    CHECK(n == 2, "an entry with a history was dropped (%u)", n);
    CHECK(id_of(out, 0) == 49, "the first kept entry is not the settled one");
    CHECK(id_of(out, 1) == 18, "the carried entry is not the pad");
    CHECK(state_of(out, 1) == 1,
          "the torn pad kept the mid-clear value instead of its history");

    /* The carry-forward reads the PREVIOUS SETTLED table and not the live
     * array, which is the whole point: carrying from the live array would just
     * be the torn read again. Both reads disagree here, and the answer is
     * neither of them. */
    memset(hist, 0, sizeof hist);
    entry(hist, 0, 49, 1, 3, 0);                /* history says: dim */
    entry(hist, 1, 18, 1, 3, 0);
    n = led_merge_settled(a_tbl, b_tbl, 2, hist, 2, out);
    CHECK(n == 2, "the second merge dropped an entry (%u)", n);
    CHECK(state_of(out, 0) == 2,
          "a settled entry took the history's value instead of the settled read");
    CHECK(state_of(out, 1) == 3, "the carried entry did not take its history's value");
}

/* ---------------------------------------------------------------------------
 * 3. The blink phase.
 */
static void test_blink(void)
{
    /* Half duty, exactly: on for the first half of the cycle and off for the
     * second, and the boundary belongs to the OFF half (a cycle of 500 is 250
     * on and 250 off, not 251/249). */
    CHECK(led_blink_on(0, 500, 0) == 1, "t=0 of a 500 ms cycle is not on");
    CHECK(led_blink_on(249, 500, 0) == 1, "t=249 of a 500 ms cycle is not on");
    CHECK(led_blink_on(250, 500, 0) == 0, "t=250 of a 500 ms cycle is still on");
    CHECK(led_blink_on(499, 500, 0) == 0, "t=499 of a 500 ms cycle is on");
    CHECK(led_blink_on(500, 500, 0) == 1, "the second cycle did not start");

    /* rbp asks for two different periods at once on this unit (250 and 500);
     * they must not be the same answer. */
    int a = 0, b = 0;
    for (unsigned long long t = 0; t < 500; t++) {
        if (led_blink_on(t, 250, 0)) a++;
        if (led_blink_on(t, 500, 0)) b++;
    }
    CHECK(a == 250, "a 250 ms period is not half duty over one cycle (%d)", a);
    CHECK(b == 250, "a 500 ms period is not half duty over one cycle (%d)", b);

    /* NO PERIOD is rbp not asking for a blink: the caller's fallback cadence is
     * used, and that fallback is the cadence this replaces (400 ms on / off),
     * so an LED rbp has no opinion about does not move. */
    CHECK(led_blink_on(0, 0, LED_BLINK_FALLBACK_MS) == 1, "the fallback is not on at t=0");
    CHECK(led_blink_on(399, 0, LED_BLINK_FALLBACK_MS) == 1,
          "the fallback is not the old 400 ms on");
    CHECK(led_blink_on(400, 0, LED_BLINK_FALLBACK_MS) == 0,
          "the fallback is not the old 400 ms off");

    /* A period that is not a duty cycle at all reads as on rather than as a
     * division by something the module would have to invent an answer for. */
    CHECK(led_blink_on(0, 1, 0) == 1, "a 1 ms period is not on");
    CHECK(led_blink_on(99, 1, 0) == 1, "a 1 ms period is not on later");
    CHECK(led_blink_on(0, 0, 0) == 1, "a period of nothing at all is not on");

    /* AND THE WRAP. `long` is four bytes on this target and this tree has paid
     * for that once already (a nanosecond CLOCK_MONOTONIC in a `long`, every
     * 4.29 s). The clock here is unsigned long long and passes 2^32 ms within
     * seven weeks of uptime.
     *
     * 2^32 is NOT a whole number of cycles -- 2^32 mod 500 is 296 and 2^32 mod
     * 800 is 96 -- which is what makes these three values worth pinning: a clock
     * truncated to 32 bits reads each of them as t=0 and gives the OTHER answer,
     * so passing all three is evidence the phase came out of the full value.
     * (An expectation written at a whole cycle boundary would pass either way
     * and pin nothing.) */
    CHECK(led_blink_on(4294967296ULL, 500, 0) == 0,
          "2^32 ms into a 500 ms cycle is not in the off half (296 >= 250); a "
          "32-bit-truncated clock reads this as t=0 and says on");
    CHECK(led_blink_on(4294967296ULL + 300ULL, 500, 0) == 1,
          "2^32+300 ms into a 500 ms cycle is not on (596 -> 96 < 250); a "
          "32-bit-truncated clock reads this as t=300 and says off");
    /* The fallback, which is where a wrapped clock lands hardest: 2^32 lands 96
     * ms into the 800 ms cycle, so +350 carries it to 446 -- the off half --
     * while a clock truncated to 32 bits reads 350 and calls it on. */
    CHECK(led_blink_on(4294967296ULL, 0, LED_BLINK_FALLBACK_MS) == 1,
          "2^32 ms into the 800 ms fallback is not on (96 < 400)");
    CHECK(led_blink_on(4294967296ULL + 350ULL, 0, LED_BLINK_FALLBACK_MS) == 0,
          "2^32+350 ms into the 800 ms fallback is not in the off half "
          "(96 + 350 = 446 >= 400); a 32-bit-truncated clock reads this as "
          "t=350 and says on");
}

int main(void)
{
    test_find();
    test_merge();
    test_blink();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
