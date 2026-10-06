/*
 * wave_zone.h -- the performance screen's waveform: swipe UP/DOWN on the wave to
 * zoom IN/OUT.
 *
 * THE FEATURE, in the operator's words, after the pinch was rejected:
 *
 *     *"yeah, what you did is no good. it messes up scrolling everywhere else,
 *      instead of pinch can you just allow swiping on the waveform up/down for
 *      zoom in/out."*
 *
 * ...and that message carries both halves of this file. The gesture is a
 * ONE-FINGER VERTICAL SWIPE on the wave instead of a two-finger pinch, and the
 * regression is answered by this module NEVER TAKING A REPORT (see "additive"
 * below, which is the whole design).
 *
 * This is menu_zone.h's / side_zone.h's / util_zone.h's sibling and is written to
 * the same rule: pure C, no rbp address, no env, no I/O, no clock. It says HOW MANY
 * rotations this report is worth; reading rbp's state and sending them is
 * pointsrc.c's.
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS A SELECTOR ROTATION AND NOT A TOUCH. rbp has no waveform pinch. The
 * zoom is reached only from BrowseEncoderRotate @0x1212a4 (CursorWaveZoom @0x102720
 * is the binary's only caller of setPlayModeWaveScale), and the on-screen
 * "− ZOOM GRID" is an INDICATOR: ui::touch_panel::ZoomGrid, vtable @0x004d8580, is
 * referenced nowhere in executable code, and taps and a drag on it were measured to
 * change nothing. So the gesture is spent as K_SELECTOR rotations -- the same key
 * the browse encoder produces and the same one the UTILITY gesture drives. rbp then
 * zooms through its own code path, with its own clamp and its own indicator.
 *
 * ADDITIVE, AND THAT IS THE WHOLE CORRECTION. The pinch answered MZ_FEED_TAKEN: its
 * second contact latched the gesture and every report after that was swallowed until
 * BOTH fingers lifted. A phantom or stale contact therefore latched it, and from
 * then on the shim ate presses it never meant to own -- single-finger drags included,
 * which on every other screen of rbp's are the scroll. That is the operator's
 * *"it messes up scrolling everywhere else"*.
 *
 * This module returns a step count and nothing else. It cannot take a report, so it
 * cannot withhold a press, cannot owe rbp a release, and cannot leave anything
 * latched across a screen change. rbp sees exactly the stream it sees today; the
 * worst this can do is an unwanted zoom, and the gate below is what stops that.
 *
 * THE GATE IS rbp's OWN DISPATCH DECISION, not a rule invented here. On the
 * performance screen ComputeCursorMode @0x113084 routes a rotation to CursorWaveZoom
 * only when two conditions hold, and RB_WAVE_ZOOM_OK() in rbp_abi.h is that branch
 * spelled out. The other branch of the same test adjusts the BEAT GRID, which
 * rewrites the analysis of the operator's track -- so this is a refusal and not a
 * nicety. The gate is computed by the caller and arrives in struct wave_state,
 * because it changes under the gesture (rbp can leave the screen mid-press) and a
 * module that cached it would be a module that sent one rotation too many.
 *
 * ---------------------------------------------------------------------------
 * Geometry, in the 1280x800 logical px every zone in the shim works in, measured
 * off /dev/fb0 with the performance screen up (2026-10-05):
 *
 *   top bar            y   8..41    rbp's own status line
 *   DECK 1/2 panels    x  10..183   deck select, tempo, the deck's own controls
 *   BEAT FX panel      x 1090..1269 rbp binds TOUCH to this and it must stay his
 *   waveform canvas    y  47..490   the band the wave is drawn in
 *   HOT CUE label      y 499..509
 *   pad rows 1 and 2   y 518..567   a slap here is a hot cue and is not ours
 *   − ZOOM GRID        x 1130..1250, y 390..410  (an indicator, and outside the rect)
 *
 * The rect is the part of that canvas which is neither the decks' own panels nor
 * BEAT FX, and it stops above the hot-cue rows:
 *
 *     x 200..1080, y 60..480
 *
 * It starts at y 60 for a second reason: the shim's swipe-down band takes rows
 * 0..55 of the glass and OPENS on any press inside them, so a gesture that could
 * reach that strip could open the band out from under itself.
 * ------------------------------------------------------------------------- */
#ifndef RBLIVE4_WAVE_ZONE_H
#define RBLIVE4_WAVE_ZONE_H

/* The waveform rect. See the measurement table above -- these four numbers are the
 * whole of the region rule, and the comment block is why each of them is where it
 * is rather than a pixel further out. */
#define WAVE_X0  200
#define WAVE_X1  1080
#define WAVE_Y0   60
#define WAVE_Y1  480

/* Vertical travel, in logical px, per zoom step. 40 px is about a fifth of the
 * canvas height: a full-height swipe crosses the whole 0..4 ladder with room to
 * spare, and a finger resting on the wave cannot drift a step by accident. */
#define WAVE_STEP_PX 40

/* rbp's wave-scale ladder, and this module's own copy of it. Kept here rather than
 * included from rbp_abi.h so the header stays dependency-free the way util_zone.h's
 * browse mode is; test_wave.c includes both and pins that the two agree.
 *
 * This is not decoration -- it is the module's BOUND. A press may only ask for
 * scales it can actually reach from where it started, so at most
 * WAVE_ZOOM_MAX - WAVE_ZOOM_MIN rotations can ever come out of one report, and
 * there is no separate per-report cap to invent. It is also what makes the gesture
 * feel right at the ends: rbp clamps silently, so without this an overshoot would
 * be remembered by the ratchet and paid back on the way down (drag well past the
 * top, drag back to the anchor, and the wave would jump all the way to the bottom). */
#define WAVE_ZOOM_MIN 0
#define WAVE_ZOOM_MAX 4

/* What the caller must put in one of these. `zoom_ok` is RB_WAVE_ZOOM_OK() for THIS
 * report, and it is the caller's because only the caller reads rbp.
 *
 * `scale` is rbp's current wave scale, and it is read because the module has to know
 * where the ladder's ends are for this press -- it is used at the DOWN EDGE ONLY, and
 * it must be in range for the press to be ours at all. A caller that cannot read it
 * (not rbp, no screen) leaves `zoom_ok` zero and the module never latches. */
struct wave_state {
    int zoom_ok;
    int scale;
};

/* Is this point on the waveform? The caller's ladder must give the band, the window
 * and the drawers first refusal -- they are drawn over rbp's screen -- but this module
 * would answer for its rect regardless, which is why the caller asks it only on the
 * reports rbp is about to receive. */
int wave_in_rect(int x, int y);

/* The gesture. Returns the SIGNED number of K_SELECTOR rotations this report is
 * worth: POSITIVE is ZOOM IN, NEGATIVE is ZOOM OUT, 0 is "nothing" -- and 0 is the
 * answer on every report this module does not own, which is all of them on every
 * screen of rbp's but the performance one.
 *
 * A bigger scale is zoomed IN (calcParticularWave @0x123014's samples-per-screen
 * ladder is {1600,800,400,200,100} for scales 0..4), so A SWIPE UP ZOOMS IN: the
 * finger moves up, dy is negative, and the step count is `-dy / WAVE_STEP_PX`.
 *
 * The rules in full, and there is no latch to lose because none of them consumes a
 * report:
 *
 *   down inside the rect, gate open, scale readable
 *                          -- this press is ours; the landing point is the anchor and
 *                             the scale now is the base. Nothing is sent, so a tap
 *                             on the wave costs one report and does nothing.
 *   down anywhere else     -- not ours, and never becomes ours when it wanders on.
 *   move, ours             -- steps coalesced from the ANCHOR, minus what this press
 *                             has already sent, clamped to the ladder from the base,
 *                             so a resting finger sends nothing, a wobble sends
 *                             nothing, and a reversal pays back exactly what it
 *                             overran.
 *   move, NOT vertical     -- nothing, and this is the one rule that is not about
 *                             zoom at all. |dy| must be at least |dx| before any step
 *                             is sent: a drag ALONG the wave is the operator scrubbing
 *                             or searching the track, and it must not zoom. The one
 *                             case that passes with no travel at all is a finger back
 *                             on its anchor, which the ratchet needs to pay back.
 *   gate closes mid-press -- the press stops being ours, silently, and the rest of it
 *                             is rbp's. Nothing is owed anyone: rbp has had every
 *                             report of this press and always will.
 *   release                -- nothing; the zoom is delivered while the finger moves.
 */
int wave_feed(const struct wave_state *st, int down, int x, int y);

/* Forget the press in progress. Called where the touch device is lost (beside
 * menu_reset(), side_reset_all() and util_reset()): an anchor left behind by a finger
 * that went away with the device would make the NEXT press look like a continuation
 * of this one, and the first move it reported would be measured from a point on a
 * screen that is gone. */
void wave_reset(void);

#endif /* RBLIVE4_WAVE_ZONE_H */
