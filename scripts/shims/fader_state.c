/*
 * fader_state.c -- the one definition of the shim's channel-fader bookkeeping.
 *
 * WHY THIS FILE EXISTS, AND IT IS NOT TIDINESS. `g_fader` and `g_fader_seen` used
 * to be defined in rbp_vu.c, which is compiled with -fvisibility=hidden and linked
 * only into knobshim.so. The touch drawers live in fbshim.so (pointsrc.c), which
 * links rbp_key.o and not rbp_vu.o -- so a write to "the same" symbol from there
 * would have created a SECOND copy of the array, invisible to knobshim, and
 * rbp_vu.c's mixer_defaults_tick() would have gone on re-asserting unity for ~30 s
 * from its own copy: the operator's fader would have been silently overridden, and
 * only after 30 s would it have stuck. A bug that hides behind a short test.
 *
 * So the arrays are defined ONCE, here, with DEFAULT visibility, and this object is
 * in FBSHIM_OBJS. fbshim.so is loaded first (scripts/device/start-rb.sh), so when
 * knobshim.so loads afterwards its references bind to this copy rather than to a
 * definition of its own. rbp_vu.h keeps the externs; rbp_vu.c stopped defining them.
 *
 * The invariant this file exists to keep: a fader position and "has anyone actually
 * reported one" are ONE fact, so they are written together -- fader_state_set() --
 * and a caller cannot set the first and forget the second.
 */
#include "rbp_vu.h"

/* 1023 = at the top (rbp_vu.h's convention), so the meters read full until some
 * surface reports a real position -- which is what rbp itself is doing on a machine
 * with no absolute-control surface. */
int g_fader[3]      = { 1023, 1023, 1023 };
int g_fader_seen[3];      /* set once a surface has reported a fader */

void fader_state_set(int ch, int v)
{
    if (ch < 1 || ch > 2)
        return;
    if (v < 0)
        v = 0;
    if (v > 1023)
        v = 1023;
    g_fader[ch] = v;
    g_fader_seen[ch] = 1;
}
