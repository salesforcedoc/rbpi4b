/*
 * pitch_state.c -- the one definition of the shim's deck-pitch bookkeeping.
 *
 * WHY THIS FILE EXISTS, AND IT IS NOT TIDINESS -- it is fader_state.c's reason a
 * second time. The two controller maps live in knobshim.so and the drawer that nudges
 * the pitch lives in fbshim.so, which RB_LD_PRELOAD loads FIRST, so a "shared" symbol
 * defined in either shim alone would be a private copy to the other and the drawer
 * would nudge from a position no fader had ever reported. rbp_transport.h has the
 * measured version of that mistake: rbp died three seconds in with a `symbol lookup
 * error` naming the shim and not the drawer.
 *
 * So the array is defined ONCE, here, with DEFAULT visibility, and this object is in
 * neither object list -- both shims link it explicitly. A default-visibility symbol
 * defined in two preloaded libraries resolves for BOTH of them to the first in the
 * search order, so there is one array either way and the arrangement keeps working if
 * the order is ever changed. `arm-linux-gnueabi-nm -D --defined-only knobshim.so
 * fbshim.so | grep g_pitch_norm` must show both defining it.
 *
 * A float store and load are each a single aligned 32-bit access, so the reader sees a
 * whole position and never half of an update; the paired `seen` flag is why the write
 * is a function rather than a bare assignment.
 */
#include "pitch_state.h"

/* 0.0 = the fader's middle, which is also where rbp's own tempo slider sits on a cold
 * start -- so a nudge with no controller attached moves from the place rbp is already
 * at. See the header. */
float g_pitch_norm[2];
int   g_pitch_seen[2];   /* set once a surface has reported a fader */

void pitch_state_set(int deck, float norm)
{
    if (deck < 0 || deck > 1)
        return;
    g_pitch_norm[deck] = pitch_norm_clamp(norm);
    g_pitch_seen[deck] = 1;
}
