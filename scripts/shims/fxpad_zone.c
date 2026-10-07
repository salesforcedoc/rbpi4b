/*
 * fxpad_zone.c -- the BPM cell as a momentary X/Y pad: the gesture, the two mappings,
 * the snapshot and the closed-loop restore.
 *
 * See fxpad_zone.h for what the pad is for, where the rectangle comes from, the live
 * read it rests on and why this is pure. Nothing here reaches rbp, the framebuffer or a
 * clock: the whole of the module is the rectangle, the arithmetic and six words of
 * state, which is what lets test_fxpad.c drive a whole gesture on a host.
 *
 * THE ONE IDEA WORTH RESTATING is that the level and the beats are NOT two of the same
 * thing. The level is an ABSOLUTE 0..1023, so a target is a value to write and the write
 * is exact and idempotent -- `sent_depth` is enough to know when to send. The beat count
 * lives on a ladder whose rungs halve and double, so a target is a PLACE TO CLIMB TO and
 * the only honest way there is to ask rbp for one rung and look at where it landed. That
 * asymmetry is why this file has a `beat_pending` and no `depth_pending`.
 */
#include "fxpad_zone.h"

/* ---------------------------------------------------------------------------
 * THE SNAPSHOT, and the answer to "what the beat and level was before you pressed it".
 * Written once, on the first tick after the press, and never again during the gesture.
 * ------------------------------------------------------------------------- */
static int on0;                 /* 0/1, the effect's state before the press */
static int depth0;              /* 0..1023, the level before the press */
static int beat0;               /* the rung before the press */
static int mark_x0, mark_y0;    /* where that rung and level sit in the cell, for the dot */

/* The gesture. `was_down` is the edge detector every feeder in this shim carries
 * (touch_zone.c's shape), `engaged` says this gesture is OURS -- they are two different
 * questions, and the difference is a finger that went down outside the cell and then
 * slid across it: it is down, but it is rbp's. `restoring` is the unwind after the
 * release; `snapped` says the snapshot above has been taken, which is what tells a tap
 * that lived entirely between two ticks apart from a real gesture (see fxpad_tick). */
static int was_down;
static int engaged;
static int restoring;
static int snapped;
static int finger_x, finger_y;  /* the press point, clamped into the cell */

/* What has been asked for, as opposed to what rbp has answered. The level's own send is
 * absolute so `sent_depth` is the last value written and needs no loop at all.
 *
 * THE ON/OFF IS A TOGGLE ON THE WIRE, which is why it is the one axis that is asked for
 * AND WATCHED. It cannot be driven like the level -- the desired state is not a value to
 * write, it is a flip -- so the only honest way to reach it is to ask, then read rbp's
 * own answer back, and ask again if the answer still disagrees. `on_want` is the state
 * being asked for (1 while the finger is down, `on0` once it is not), `on_asking` says a
 * toggle is in flight and unanswered, `on_wait`/`on_tries`/`on_dead` are the bounded
 * give-up the ladder also carries, and a change of target clears all three.
 *
 * This was one blind send, once, on the snapshot tick -- and the operator's own report
 * is what it cost: *"when i change effects it doesn't seem to remember to trigger when i
 * press until i turn it on off"* (2026-10-07). A single unverified toggle that rbp drops
 * leaves the effect off for the whole gesture and nothing ever notices. */
static int on_want;
static int on_asking;
static int on_wait;
static int on_tries;
static int on_dead;
static int sent_depth;

/* The climb. `beat_pending` is set while a step is in flight and `beat_pending_from` is
 * the rung it was taken from, so a second step is never sent until rbp has moved -- that
 * is what stops a rung being skipped when rbp applies a key a tick late. `beat_wait`
 * counts the ticks that step has gone unanswered, `beat_dead` latches the giving-up, and
 * `beat_want`/`beat_want_valid` clear it when the operator asks for somewhere else. */
static int beat_pending;
static int beat_pending_from;
static int beat_wait;
static int beat_dead;
static int beat_want;
static int beat_want_valid;

/* ------------------------------------------------------------------- the truth
 *
 * IS THE EFFECT ACTUALLY RUNNING? rbp has two words that answer it and only one of them
 * can be trusted, which is the whole of the bug this file was fixed for on 2026-10-07.
 *
 * `BeatEffect+0x3c` is the one that reads like the answer, and on a switched-off effect
 * it is a LIE: when the type is 0 rbp points BeatEffectManager+0x08 at a `BeatEffectOff`, a
 * class whose ON/OFF, level and time virtuals are all compiled out (`bx lr`), so nothing
 * ever maintains that object's flag and it sits at 1. Measured on the unit 2026-10-07 --
 * one step of the effect selector while the effect is off leaves the type at 0 AND flips
 * +0x3c from 0 to 1, which is the operator's own repro and the whole of the bug. rbp_abi.h
 * has the table, including the cold-start row that makes it a CONJUNCTION and not an
 * equivalence (type 0 can also carry a stale real object whose own flag reads 0).
 *
 * So ON is the CONJUNCTION, and the pad asks for a toggle exactly when the conjunction
 * says no. That is what the operator could not get: switching to a new effect while the
 * effect was off left the pad reading "on", agreeing with itself and sending nothing at
 * all -- to a `BeatEffectOff` that ignores the level and the rung too. "It doesn't
 * engage."
 */
static int live_on(const struct fxpad_live *live)
{
    return live->type != 0 && live->on != 0;
}

int fxpad_live_on(const struct fxpad_live *live)
{
    return live && live_on(live);
}

/* ------------------------------------------------------------------ the arithmetic */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* X -> the level. Integer rounding of a positive quotient: (a + b/2) / b. */
static int depth_of_x(int x)
{
    int w = FXPAD_X1 - FXPAD_X0;

    x = clampi(x, FXPAD_X0, FXPAD_X1);
    return (int)(((long)(x - FXPAD_X0) * FXPAD_DEPTH_MAX + w / 2) / w);
}

/* The level -> X, which is the dot's position in the restore and nothing else. It is
 * NOT depth_of_x's inverse in the strict sense -- 160 px carry 1024 values, so several
 * levels share a column -- and it does not need to be: the level that travels on the
 * wire is the one depth_of_x produced, and this only decides where to put a dot. */
static int x_of_depth(int d)
{
    d = clampi(d, 0, FXPAD_DEPTH_MAX);
    return FXPAD_X0
         + (int)(((long)d * (FXPAD_X1 - FXPAD_X0) + FXPAD_DEPTH_MAX / 2) / FXPAD_DEPTH_MAX);
}

/* Y -> the rung. THE TOP OF THE CELL IS THE MOST BEATS, which is the way up the ladder
 * reads on the glass ("BEAT >" doubles), and a span of zero or less -- a type rbp will
 * not give limits for -- answers the floor rather than dividing by it. */
static int beat_of_y(int y, int bmin, int bmax)
{
    int h = FXPAD_Y1 - FXPAD_Y0;
    int span = bmax - bmin;

    if (span <= 0)
        return bmin;
    y = clampi(y, FXPAD_Y0, FXPAD_Y1);
    return bmin + (int)(((long)(FXPAD_Y1 - y) * span + h / 2) / h);
}

/* ------------------------------------------------------------------- the rect */

int fxpad_hit(int x, int y)
{
    return x >= FXPAD_X0 && x <= FXPAD_X1 && y >= FXPAD_Y0 && y <= FXPAD_Y1;
}

void fxpad_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = FXPAD_X0;
    if (y0) *y0 = FXPAD_Y0;
    if (x1) *x1 = FXPAD_X1;
    if (y1) *y1 = FXPAD_Y1;
}

/* ------------------------------------------------------------------- the gesture */

int fxpad_feed(int down, int x, int y)
{
    if (!down) {
        int owned = engaged;

        was_down = 0;
        /* A release this module never saw the press for is not ours -- unlike the
         * picker, this pad is not up over anything, so rbp may keep it. */
        if (!owned)
            return MZ_FEED_NONE;
        engaged = 0;
        restoring = 1;         /* the unwind; fxpad_tick does the work */
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        was_down = 1;
        /* A press that began outside the cell is rbp's for its WHOLE gesture: it is not
         * adopted on a slide, because a drag that merely crossed the panel would then
         * move the effect nobody aimed at. */
        if (fxpad_hit(x, y)) {
            engaged = 1;
            restoring = 0;     /* a new press supersedes an unwind still in flight */
            snapped = 0;
        }
    }

    if (!engaged)
        return MZ_FEED_NONE;   /* still a gesture that began somewhere else */

    /* Clamped, so a finger dragged past an edge pins the value at full rather than
     * running off the end of a mapping. */
    finger_x = clampi(x, FXPAD_X0, FXPAD_X1);
    finger_y = clampi(y, FXPAD_Y0, FXPAD_Y1);
    return MZ_FEED_TAKEN;
}

/* ---------------------------------------------------------------------- the tick */

void fxpad_tick(const struct fxpad_live *live, struct fxpad_out *out)
{
    int span, want_depth, want_beat, want_on, on_now, bound;

    if (out) {
        out->want_on = -1;
        out->depth = -1;
        out->beat_step = 0;
    }
    if (!out || !live || (!engaged && !restoring))
        return;

    /* A TAP THAT PRESSED AND RELEASED BETWEEN TWO TICKS. No tick ever saw it engaged,
     * so nothing was ever sent, so there is nothing to put back. Going idle here is the
     * same rule the rest of the module rests on. */
    if (!engaged && restoring && !snapped) {
        restoring = 0;
        return;
    }

    if (engaged && !snapped) {
        /* THE SNAPSHOT. This is the last moment at which these three are still what
         * they were before the finger arrived, and it is the whole of the operator's
         * "set it back to what the beat and level was before you pressed it". */
        on0 = live_on(live) ? 1 : 0;
        depth0 = clampi(live->depth, 0, FXPAD_DEPTH_MAX);
        beat0 = live->beat;
        mark_x0 = x_of_depth(depth0);
        mark_y0 = beat_of_y(beat0, live->beat_min, live->beat_max);
        sent_depth = depth0;
        on_want = on0;             /* the axis starts settled; the block below moves it */
        on_asking = 0;
        on_wait = 0;
        on_tries = 0;
        on_dead = 0;
        beat_pending = 0;
        beat_wait = 0;
        beat_dead = 0;
        snapped = 1;
    }

    want_depth = engaged ? depth_of_x(finger_x) : depth0;
    want_on = engaged ? 1 : on0;
    on_now = live_on(live);
    span = live->beat_max - live->beat_min;
    want_beat = engaged ? beat_of_y(finger_y, live->beat_min, live->beat_max)
                        : clampi(beat0, live->beat_min, live->beat_max);
    if (span <= 0)
        want_beat = live->beat;      /* no limits read: nothing to climb towards */

    /* ---- the level: absolute, so one send and no feedback loop ---- */
    if (want_depth != sent_depth) {
        out->depth = want_depth;
        sent_depth = want_depth;
    }

    /* ---- the on/off: a flip, so ask and then WATCH RBP'S OWN ANSWER ----
     *
     * `want_on` is 1 while the finger is down (the effect has to be running for the
     * level to be audible, which is what the operator asked for) and `on0` once it is
     * not. Three states, and the whole of the axis:
     *
     *   a toggle is in flight  -- wait for rbp to agree; if it never does, spend another
     *                             attempt once the wait runs out, and give up after
     *                             FXPAD_ON_TRIES of them. A TOGGLE ASKED FOR TWICE IS
     *                             ONLY SAFE BECAUSE THE ANSWER IS READ: the second ask
     *                             is a correction of a flip rbp did not make, not a
     *                             blind repeat of one it may have.
     *   no toggle in flight    -- if rbp's answer disagrees with what is wanted, ask.
     *                             While the finger is down this also RE-ARMS the effect:
     *                             an effect that rbp switched off under the operator --
     *                             a type change is the one this was written for -- is
     *                             asked for again on the next tick instead of leaving
     *                             the rest of the gesture silent.
     *   target changed         -- clear the give-up and start over. Moving the finger is
     *                             not a reason to stop trying to be on.
     *
     * "rbp's answer" is `on_now`, which is `live_on(live)` -- the type AND the flag, see
     * the top of this file. Every comparison here is against that and none against
     * `live->on`, because the flag alone says "on" for an effect that is switched off.
     */
    if (want_on != on_want) {
        on_want = want_on;
        on_asking = 0;
        on_wait = 0;
        on_tries = 0;
        on_dead = 0;
    }
    if (on_asking) {
        if (on_now == on_want) {
            on_asking = 0;         /* rbp answered: the flip landed */
            on_wait = 0;
            on_tries = 0;
        } else if (++on_wait > FXPAD_ON_WAIT) {
            on_asking = 0;
            on_wait = 0;
            if (++on_tries >= FXPAD_ON_TRIES)
                on_dead = 1;       /* rbp is not going to answer this one */
        }
    } else if (!on_dead && on_now != on_want) {
        out->want_on = on_want;
        on_asking = 1;
        on_wait = 0;
    }

    /* ---- the beats: a ladder climbed on rbp's own answer ---- */
    if (span > 0) {
        bound = 2 * span + 4;
        if (beat_pending) {
            if (live->beat != beat_pending_from) {
                beat_pending = 0;      /* rbp moved: free to ask again */
                beat_wait = 0;
            } else if (++beat_wait > bound) {
                /* A rung rbp will not move off is a plateau, not a reason to send a key
                 * forever. Latch the giving-up; a new target clears it below. */
                beat_pending = 0;
                beat_wait = 0;
                beat_dead = 1;
            }
        }
        if (beat_want_valid && want_beat != beat_want)
            beat_dead = 0;             /* asked for somewhere else: try again */
        beat_want = want_beat;
        beat_want_valid = 1;

        if (!beat_pending && !beat_dead && want_beat != live->beat) {
            out->beat_step = (want_beat > live->beat) ? 1 : -1;
            beat_pending = 1;
            beat_pending_from = live->beat;
            beat_wait = 0;
        }
    }

    /* ---- the unwind ----
     *
     * The on/off needs nothing of its own here: `want_on` became `on0` the moment the
     * finger went (the block above), so the same ask-and-watch is already putting it
     * back. What is left is to say when the whole thing is home -- and the level being
     * ASKED for is enough for that axis (it is absolute), while the other two have to
     * have ARRIVED, or to have given up: the ladder on the rung, the on/off on the state
     * the operator had before they pressed. A restore that stopped at what it had asked
     * for is exactly the bug this file was just fixed for. */
    if (restoring) {
        if (sent_depth == want_depth
            && (on_now == on_want || on_dead)
            && (span <= 0 || live->beat == want_beat || beat_dead))
            restoring = 0;
    }
}

/* --------------------------------------------------------------- the whole pad */

int fxpad_busy(void)
{
    return engaged || restoring;
}

int fxpad_engaged(void)
{
    return engaged;
}

int fxpad_mark(int *mx, int *my)
{
    if (engaged) {
        if (mx) *mx = finger_x;
        if (my) *my = finger_y;
        return 1;
    }
    if (restoring) {
        /* The finger has gone; the dot shows where the value is going home to. */
        if (mx) *mx = mark_x0;
        if (my) *my = mark_y0;
        return 1;
    }
    return 0;
}

void fxpad_reset(void)
{
    was_down = 0;
    engaged = 0;
    restoring = 0;
    snapped = 0;
    finger_x = finger_y = 0;
    on0 = depth0 = beat0 = 0;
    mark_x0 = mark_y0 = 0;
    sent_depth = 0;
    on_want = on_asking = on_wait = on_tries = on_dead = 0;
    beat_pending = 0;
    beat_pending_from = 0;
    beat_wait = 0;
    beat_dead = 0;
    beat_want = 0;
    beat_want_valid = 0;
}
