/*
 * pitch_state.h -- the deck tempo-slider position that the controller maps and the
 * edge drawers share, and the arithmetic a nudge cell does with it.
 *
 * WHY THIS IS ITS OWN OBJECT AND ITS OWN FILE.
 *
 * The position is WRITTEN by the controller maps -- map_flx4.c's flx4_pitch() and
 * map_jp21.c's handle_pitch() -- which are compiled into knobshim.so, and READ by the
 * edge drawers, which are driven by pointsrc.c in fbshim.so. rb.conf's RB_LD_PRELOAD
 * loads fbshim.so BEFORE knobshim.so, so fbshim cannot call into knobshim at all: at
 * fbshim's relocation the name does not exist yet. rbp_transport.h carries the
 * measured account of what happens when something tries anyway (rbp dies three
 * seconds in with `undefined symbol`), and the Makefile carries the arrangement that
 * avoids it -- ONE object, built WITHOUT -fvisibility=hidden, deliberately in neither
 * object list, and linked into BOTH shims. fader_state.o and rbp_transport.o are the
 * other two.
 *
 * A drawer nudged with no controller attached reads the default below rather than
 * "nothing", and that is deliberate: 0.0 is the fader's centre and rbp's own
 * cold-start tempo position, so a nudge from an untouched fader moves from the place
 * rbp is already at rather than from a guess. g_pitch_seen[] is what lets the log say
 * which of the two it was.
 *
 * THIS HEADER MUST STAY PURE: no syscall, no clock, no rbp address, no allocation.
 * That is the same rule rbp_transport.h states, and for the same reason -- it is what
 * lets a host test link the real object and drive it directly.
 */
#ifndef RBPI4B_PITCH_STATE_H
#define RBPI4B_PITCH_STATE_H

/* [0] = deck 1 (the LEFT drawer), [1] = deck 2 -- which is the index side_zone.h's
 * SZ_LEFT/SZ_RIGHT already are, and the index map_flx4.c's own pitch_state[] uses.
 *
 * The value is the float rbp's K_TEMPO_SLIDER/OP_VALUE consumes, in [-1.0 .. +1.0],
 * -1.0 slower, AFTER any TEMPO_REV -- because what the drawer has to nudge from is the
 * position rbp was actually told, not the raw fader. */
extern float g_pitch_norm[2];
extern int   g_pitch_seen[2];   /* set once a surface has reported a fader */

/* Publish one deck's position. Clamps, and sets `seen` with it: the position and "has
 * anyone reported one" are ONE fact and are written together, exactly as
 * fader_state_set() keeps them for the channel faders. So a caller cannot set the
 * first and forget the second, and cannot report a position without marking it real. */
void pitch_state_set(int deck, float norm);

/* ---------------------------------------------------------------------------
 * The nudge, and the one number that defines it.
 *
 * WHAT "5%" MEANS, AND IT IS NOT FIVE POINTS OF TEMPO. rbp's tempo slider is a
 * POSITION across whatever tempo range rbp is set to (K_TEMPO_RANGE 0x4107: +-6, +-10,
 * +-16, WIDE) and the shim is never told which: an FLX4 has no range button, so the
 * operator sets it in rbp's menu and nothing on the wire reports it. The nudge is
 * therefore expressed in the only unit the shim has -- PERCENT OF THE FADER'S FULL
 * TRAVEL. SIDE_NUDGE_PCT=5 is +-0.05 in the [-1,+1] space, which is roughly 0.3% of
 * tempo at a +-6% range, 0.5% at +-10% and 0.8% at +-16%: a beatmatch-sized step, and
 * one whose size cannot be changed behind the operator's back by a range setting no
 * surface names. It also cannot be pushed out of the fader's travel, which a
 * five-point tempo step can -- at +-6% that would be most of the slider.
 * ------------------------------------------------------------------------- */
static inline float pitch_norm_clamp(float n)
{
    if (n > 1.0f)
        n = 1.0f;
    if (n < -1.0f)
        n = -1.0f;
    return n;
}

/* What the deck should be told while a nudge cell is held, or what it should be put
 * back to on the lift.
 *
 * `dir` is -1 for the '-' cell, +1 for the '+' cell and 0 for "put it back". `pct` is
 * percent of the fader's travel (see above). A restore IGNORES `pct` -- it is the
 * fader's own position handed back unchanged -- which is why dir 0 is a case of its
 * own rather than a nudge of zero that would only be right at pct 0. */
static inline float pitch_nudge_norm(float fader_norm, int dir, float pct)
{
    if (dir == 0)
        return pitch_norm_clamp(fader_norm);
    return pitch_norm_clamp(fader_norm + (float)dir * (pct / 100.0f));
}

/* The 14-bit companion the tempo message carries, DERIVED FROM THE NORM so the two
 * can never disagree about the same position. flx4_pitch() reads its companion
 * straight off the CC pair instead; this is the exact inverse of the arithmetic it
 * turns that pair into the norm with -- pos = norm * 8192 + 0x2000, clamped, which
 * reproduces its ((pos - 0x2000) / 8192) both ways. */
static inline int pitch_pos_from_norm(float norm)
{
    int pos = (int)(norm * 8192.0f) + 0x2000;

    if (pos < 0)
        pos = 0;
    if (pos > 0x3FFF)
        pos = 0x3FFF;
    return pos;
}

/* The 10-bit value, spelled exactly as flx4_pitch() computes it so the drawer's
 * message and the fader's are the same message.
 *
 * THE TRUNCATION IS LOAD-BEARING, and test_flx4.c already pins it from the other side:
 * the fader's own travel tops out at norm 0.99988 (pos 0x3FFF), which is v10 1022 and
 * NOT 1023. Only a nudge clamped to exactly 1.0 reaches 1023, so this bound is not
 * dead -- it is the one path that gets there. */
static inline int pitch_v10_from_norm(float norm)
{
    int v10 = (int)((norm + 1.0f) * 511.5f);

    if (v10 < 0)
        v10 = 0;
    if (v10 > 1023)
        v10 = 1023;
    return v10;
}

#endif /* RBPI4B_PITCH_STATE_H */
