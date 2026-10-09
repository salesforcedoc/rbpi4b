/*
 * test_pitch.c -- the edge drawers' nudge arithmetic. No Pi, no rbp, no controller,
 * no framebuffer.
 *
 * It links the PRODUCTION pitch_state.c, which is pure, and pins the three things
 * that would otherwise only ever be seen as "the button does nothing" on the glass:
 *
 * 1. THE STEP, AND ITS SIZE. Five percent of the fader's travel is +-0.05 in the
 *    [-1,+1] space rbp's K_TEMPO_SLIDER consumes, NOT five points of tempo -- the
 *    shim is never told rbp's tempo range, so travel is the only unit it has. At
 *    rbp's default range the whole point of pinning this is that +-0.05 is ~0.3% of
 *    tempo and not 5%, which is not a number anyone can read off a deck by eye.
 *
 * 2. THE CLAMP, at both ends, because a nudge near the end of the fader is the one
 *    press that could otherwise send rbp a position that does not exist.
 *
 * 3. THE MESSAGE, pinned AGAINST test_flx4.c's own rows rather than re-derived. The
 *    drawer and the FLX4's pitch fader must send the same numbers for the same
 *    position, and those rows already fix centre = v10 511, top = 1022, bottom = 0.
 *    Note 1022 and not 1023: the fader's travel tops out at norm 0.99988, so only a
 *    nudge clamped to exactly 1.0 reaches 1023 -- which is what keeps the upper bound
 *    in the header from being dead code.
 *
 * The restore is a case of its own and is pinned as one: dir 0 hands the fader's own
 * position back UNCHANGED, at any pct, because "put it back" must not depend on the
 * size knob -- setting SIDE_NUDGE_PCT to 0 must not disable the release.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#include "pitch_state.h"

#include <stdio.h>

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

/* test_flx4.c's macro, spelled the same way: this file has to agree with the FADER's
 * table, and that is the macro its rows are built from. */
#define PITCH_NORM(pos) (((float)(pos) - 0x2000) / 8192.0f)

#define NEAR(a, b) (((a) > (b) ? (a) - (b) : (b) - (a)) < 1e-6f)

/* ---- 1. the step ---------------------------------------------------------- */

static void test_step(void)
{
    /* A press on '+', from the fader's centre: +5% of travel, which is +0.05. */
    CHECK(NEAR(pitch_nudge_norm(0.0f, +1, 5.0f), 0.05f),
          "'+' from centre gave %.6f, expected +0.05", (double)pitch_nudge_norm(0.0f, 1, 5.0f));
    CHECK(NEAR(pitch_nudge_norm(0.0f, -1, 5.0f), -0.05f),
          "'-' from centre gave %.6f, expected -0.05", (double)pitch_nudge_norm(0.0f, -1, 5.0f));

    /* Off-centre the step is the same SIZE, which is the whole contract: it is a
     * move of the fader and not a move to a tempo. */
    CHECK(NEAR(pitch_nudge_norm(0.3f, +1, 5.0f), 0.35f),
          "'+' from 0.3 gave %.6f, expected 0.35", (double)pitch_nudge_norm(0.3f, 1, 5.0f));
    CHECK(NEAR(pitch_nudge_norm(-0.3f, -1, 5.0f), -0.35f),
          "'-' from -0.3 gave %.6f, expected -0.35", (double)pitch_nudge_norm(-0.3f, -1, 5.0f));

    /* The knob scales it and nothing else does. */
    CHECK(NEAR(pitch_nudge_norm(0.0f, +1, 10.0f), 0.10f),
          "pct 10 gave %.6f, expected +0.10", (double)pitch_nudge_norm(0.0f, 1, 10.0f));
    CHECK(NEAR(pitch_nudge_norm(0.0f, +1, 0.5f), 0.005f),
          "pct 0.5 gave %.6f, expected +0.005", (double)pitch_nudge_norm(0.0f, 1, 0.5f));

    /* Five percent of TRAVEL, in the units rbp actually gets: the 10-bit field the
     * message carries runs 0..1023 across the fader, centre 511, so 5% of the travel
     * is ~2.5% of that field -- about 26 counts, not 51. Said here because "5%" in
     * the operator's words and "5%" of the wire are two different numbers. */
    CHECK(pitch_v10_from_norm(pitch_nudge_norm(0.0f, +1, 5.0f)) == 537,
          "a 5%% nudge from centre is v10 %d, expected 537",
          pitch_v10_from_norm(pitch_nudge_norm(0.0f, 1, 5.0f)));
}

/* ---- 2. the clamp --------------------------------------------------------- */

static void test_clamp(void)
{
    /* Near the '+' end: the step would run past the fader, so it stops ON it. */
    CHECK(NEAR(pitch_nudge_norm(0.98f, +1, 5.0f), 1.0f),
          "'+' near the top gave %.6f, expected 1.0", (double)pitch_nudge_norm(0.98f, 1, 5.0f));
    CHECK(NEAR(pitch_nudge_norm(-0.98f, -1, 5.0f), -1.0f),
          "'-' near the bottom gave %.6f, expected -1.0", (double)pitch_nudge_norm(-0.98f, -1, 5.0f));

    /* Already at the end: pressing further is a no-op, not an overflow. */
    CHECK(NEAR(pitch_nudge_norm(1.0f, +1, 5.0f), 1.0f), "'+' at the top moved off 1.0");
    CHECK(NEAR(pitch_nudge_norm(-1.0f, -1, 5.0f), -1.0f), "'-' at the bottom moved off -1.0");

    /* The far side is not affected: a '+' at the bottom is a plain step. */
    CHECK(NEAR(pitch_nudge_norm(-1.0f, +1, 5.0f), -0.95f),
          "'+' at the bottom gave %.6f, expected -0.95", (double)pitch_nudge_norm(-1.0f, 1, 5.0f));

    /* A nonsense knob does not produce a nonsense position. */
    CHECK(NEAR(pitch_nudge_norm(0.0f, +1, 1000.0f), 1.0f), "pct 1000 did not clamp at 1.0");
    CHECK(NEAR(pitch_nudge_norm(-0.5f, +1, -5.0f), -0.55f),
          "a negative pct did not simply invert, got %.6f",
          (double)pitch_nudge_norm(-0.5f, 1, -5.0f));
}

/* ---- 3. the restore ------------------------------------------------------- */

static void test_restore(void)
{
    /* dir 0 is "put it back": the fader's own position, unchanged, wherever it is. */
    CHECK(NEAR(pitch_nudge_norm(0.0f, 0, 5.0f), 0.0f), "restore from centre moved");
    CHECK(NEAR(pitch_nudge_norm(0.42f, 0, 5.0f), 0.42f), "restore from 0.42 moved");
    CHECK(NEAR(pitch_nudge_norm(-0.42f, 0, 5.0f), -0.42f), "restore from -0.42 moved");

    /* AND IT IGNORES THE KNOB, which is the reason it is a case of its own: turning
     * SIDE_NUDGE_PCT down to zero must move the press by nothing, not disable the
     * release that puts the tempo back. */
    CHECK(NEAR(pitch_nudge_norm(0.42f, 0, 0.0f), 0.42f), "restore moved at pct 0");
    CHECK(NEAR(pitch_nudge_norm(0.42f, 0, 1000.0f), 0.42f), "restore moved at pct 1000");

    /* A fader already at an end restores to that end and not past it. */
    CHECK(NEAR(pitch_nudge_norm(1.0f, 0, 5.0f), 1.0f), "restore from 1.0 moved");
}

/* ---- 4. the message ------------------------------------------------------- */

static void test_message(void)
{
    /* The three positions test_flx4.c pins from the CC pair, through this header's
     * arithmetic instead: the drawer must send the same numbers the fader sends. */
    CHECK(pitch_v10_from_norm(PITCH_NORM(0x2000)) == 511,
          "centre is v10 %d, expected 511", pitch_v10_from_norm(PITCH_NORM(0x2000)));
    CHECK(pitch_v10_from_norm(PITCH_NORM(0x3FFF)) == 1022,
          "the '+' end is v10 %d, expected 1022 (the fader tops out at norm 0.99988)",
          pitch_v10_from_norm(PITCH_NORM(0x3FFF)));
    CHECK(pitch_v10_from_norm(PITCH_NORM(0x0000)) == 0,
          "the '-' end is v10 %d, expected 0", pitch_v10_from_norm(PITCH_NORM(0x0000)));

    /* 1023 is reachable, and ONLY from a nudge clamped to exactly 1.0. That is what
     * makes the header's upper bound a real bound rather than decoration. */
    CHECK(pitch_v10_from_norm(1.0f) == 1023, "a clamped nudge is v10 %d, expected 1023",
          pitch_v10_from_norm(1.0f));
    CHECK(pitch_v10_from_norm(-1.0f) == 0, "a clamped nudge low is v10 %d, expected 0",
          pitch_v10_from_norm(-1.0f));

    /* The 14-bit companion is the exact inverse of flx4_pitch()'s own arithmetic, so
     * it round-trips EVERY position the fader can report and not just the ends. */
    {
        int pos, bad = 0;

        for (pos = 0; pos <= 0x3FFF; pos++) {
            if (pitch_pos_from_norm(PITCH_NORM(pos)) != pos)
                bad++;
        }
        CHECK(bad == 0, "%d of 16384 fader positions did not round-trip", bad);
    }

    /* And a nudge clamped high cannot report a position the fader does not have. */
    CHECK(pitch_pos_from_norm(1.0f) == 0x3FFF,
          "a clamped nudge reported pos %#x, expected 0x3fff", pitch_pos_from_norm(1.0f));
    CHECK(pitch_pos_from_norm(-1.0f) == 0, "a clamped low nudge reported pos %#x",
          pitch_pos_from_norm(-1.0f));
}

/* ---- 5. the shared state -------------------------------------------------- */

static void test_state(void)
{
    /* The default a drawer with no controller reads: the fader's centre, which is
     * also where rbp's own tempo slider cold-starts. */
    CHECK(g_pitch_norm[0] == 0.0f && g_pitch_norm[1] == 0.0f,
          "the arrays do not start at centre");
    CHECK(g_pitch_seen[0] == 0 && g_pitch_seen[1] == 0, "the arrays start marked as seen");

    pitch_state_set(0, 0.25f);
    CHECK(g_pitch_norm[0] == 0.25f, "deck 1 published %.6f, expected 0.25",
          (double)g_pitch_norm[0]);
    CHECK(g_pitch_seen[0] == 1, "deck 1 published a position but was not marked seen");
    CHECK(g_pitch_norm[1] == 0.0f && g_pitch_seen[1] == 0,
          "deck 1's publish touched deck 2");

    /* The publish clamps, so the fader's own report can never put a position into the
     * shared state that the nudge arithmetic would then have to defend against. */
    pitch_state_set(0, 5.0f);
    CHECK(g_pitch_norm[0] == 1.0f, "the publish did not clamp high: %.6f",
          (double)g_pitch_norm[0]);
    pitch_state_set(1, -5.0f);
    CHECK(g_pitch_norm[1] == -1.0f, "the publish did not clamp low: %.6f",
          (double)g_pitch_norm[1]);

    /* An index that is not a deck is dropped rather than written out of bounds --
     * which is what a map with a third channel would otherwise do. */
    g_pitch_norm[0] = 0.0f;
    pitch_state_set(2, 0.5f);
    pitch_state_set(-1, 0.5f);
    CHECK(g_pitch_norm[0] == 0.0f && g_pitch_norm[1] == -1.0f,
          "a publish to a nonexistent deck wrote somewhere");
}

int main(void)
{
    test_step();
    test_clamp();
    test_restore();
    test_message();
    test_state();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
