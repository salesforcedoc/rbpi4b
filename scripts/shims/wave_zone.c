/*
 * wave_zone.c -- swipe up/down on the performance screen's wave to zoom in/out.
 *
 * See wave_zone.h for the feature, the measurement every constant comes from and the
 * rules in full. Nothing here reaches rbp, the framebuffer, the environment or the
 * clock; it is util_zone.c's shape with the list taken away -- one press, one rect,
 * and a rotation count as the answer. It differs from util_zone.c in exactly one
 * way, and that way is the point: it never TAKES a report, so nothing it does can
 * change what rbp is handed.
 */
#include "wave_zone.h"

#include <stddef.h>          /* NULL, in wave_ready() */

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* ---------------------------------------------------------------------------
 * State. `was_down` is set at EVERY down edge and cleared at every release, ours or
 * not, so the module can tell "a press I ignored" from "a release with no press
 * behind it" -- and, more to the point, so a down edge is only ever read as a down
 * edge. `latched` is the narrower "this press is mine", fixed at the down edge.
 *
 * `anchor_y` is where the finger landed in screen space and `sent` is how many
 * rotations this press has already asked for. Together they are the whole of the
 * gesture: the steps travelled is `-(y - anchor_y) / WAVE_STEP_PX` measured from the
 * ANCHOR rather than from the last report, and what goes out is the difference from
 * `sent`. `base_scale` is rbp's scale when the press began, and it is what keeps the
 * ratchet inside the ladder (see the header's note on overshoot).
 *
 * `anchor_x` is carried only so the vertical-dominance test can be made against the
 * anchor rather than against the last report: a finger that drifts sideways while it
 * rises is still a vertical swipe, and one that sets off along the wave and then
 * turns is not ours to claim.
 * ------------------------------------------------------------------------- */
struct wave_st {
    int was_down;
    int latched;
    int anchor_x;
    int anchor_y;
    int base_scale;
    int sent;       /* rotations THIS press has already asked for */
};

static struct wave_st st;

static int wave_ready(const struct wave_state *s)
{
    return s != NULL && s->zoom_ok &&
           s->scale >= WAVE_ZOOM_MIN && s->scale <= WAVE_ZOOM_MAX;
}

int wave_in_rect(int x, int y)
{
    return x >= WAVE_X0 && x <= WAVE_X1 && y >= WAVE_Y0 && y <= WAVE_Y1;
}

int wave_feed(const struct wave_state *s, int down, int x, int y)
{
    if (!down) {
        /* The release, and it carries no answer whatever: the zoom is delivered while
         * the finger is moving, and a press that never moved was a tap on the wave. */
        st.was_down = 0;
        st.latched = 0;
        return 0;
    }

    if (!st.was_down) {
        /* THE DOWN EDGE. Note what is NOT here: no `return` that swallows anything,
         * because this module has nothing to swallow with. It decides only whether it
         * will have an opinion about the moves that follow. */
        st.was_down = 1;
        st.latched = 0;
        st.sent = 0;

        if (!wave_ready(s) || !wave_in_rect(x, y))
            return 0;

        st.latched = 1;
        st.anchor_x = x;
        st.anchor_y = y;
        st.base_scale = s->scale;
        return 0;
    }

    /* A MOVE (or the down edge repeated, which carries no new information). */
    if (!st.latched)
        return 0;

    /* THE SCREEN CAN GO AWAY UNDER A PRESS. rbp's own BACK, the operator's hand, a
     * mode change -- and the gate is re-read by the caller on every report precisely
     * so that this is noticed. Unlatching stops the zoom at once and hands the rest
     * of the press back to rbp, which has had every report of it all along. */
    if (!wave_ready(s)) {
        st.latched = 0;
        return 0;
    }

    {
        int dy = y - st.anchor_y;
        int dx = x - st.anchor_x;
        int lo = WAVE_ZOOM_MIN - st.base_scale;
        int hi = WAVE_ZOOM_MAX - st.base_scale;
        int steps, delta;

        /* NOT VERTICAL, NOT OURS. A press that has travelled further along the wave
         * than up it is the operator scrubbing or searching the track -- rbp's own
         * gesture on this very canvas -- and a zoom out of it would be a zoom the
         * operator did not ask for and cannot see the cause of.
         *
         * It is tested against the ANCHOR and on every report, so it is a property of
         * the swipe as a whole and not of one sample of it: a finger that sets off
         * along the wave and then turns up never becomes a zoom, and the steps already
         * sent are not paid back by this refusal.
         *
         * The comparison is `>=` and not `>`, and the difference is the finger that has
         * come back exactly to where it started: that report has no travel in either
         * axis, and it must pass so that the steps this press sent are paid back. Under
         * `>` it would be refused as "not vertical", the ratchet would keep them, and
         * the first move past the anchor would then pay back twice over -- the wave
         * would drop two steps for one step of finger. */
        if (iabs(dy) < iabs(dx))
            return 0;

        /* UP IS ZOOM IN, and the truncation is toward zero, so -40/40 is 1 and -39/40
         * is 0: a step is crossed exactly at a multiple of WAVE_STEP_PX, going up just
         * as it does going down. */
        steps = -dy / WAVE_STEP_PX;
        if (steps > hi)
            steps = hi;
        if (steps < lo)
            steps = lo;

        delta = steps - st.sent;
        if (delta == 0)
            return 0;              /* a resting or wobbling finger: nothing new */

        st.sent += delta;
        return delta;
    }
}

void wave_reset(void)
{
    st.was_down = 0;
    st.latched = 0;
    st.anchor_x = 0;
    st.anchor_y = 0;
    st.base_scale = 0;
    st.sent = 0;
}
