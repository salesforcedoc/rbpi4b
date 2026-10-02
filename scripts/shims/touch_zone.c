/*
 * touch_zone.c -- the deck QUANTIZE boxes, and the press that stands for them.
 *
 * See touch_zone.h for what this is for and why it is a separate pure module.
 * This file is the geometry and the edge detector; the keycode it turns into is
 * pointsrc.c's side.
 */
#include "touch_zone.h"

/* The two boxes, in rbp's logical 1280x800 space, read off a captured frame.
 *
 * The capture: /dev/fb0 on the unit, performance screen, 2026-09-28 00:15-00:17
 * (/tmp/q0..q3.raw, r0..r3.raw, z0/z1.raw), decoded as RGB565. The fb was
 * 1280x800 with the layer surface AS the fb page, so fb x maps 1:1 onto logical
 * x -- and the zone is in logical space either way, so a display that needs a
 * blit changes nothing here.
 *
 * Deck 1's widget, exactly as drawn (a label row over a value field):
 *
 *   label  "QUANTIZE"   x 27..88, y 736..746
 *   field  rules at y 754 and y 776, x 26..86, digit centred at x 56
 *   so the whole widget is x 26..88, y 736..776 (63 x 41)
 *
 * Deck 2 is the same widget 640 px to the right -- NOT a mirror about the
 * centre (a mirror would put it at x 1191..1253): its label is x 667..728 and
 * its rules x 666..726. Hence one offset, not two rectangles spelled out.
 *
 * The margin below is the finger-friendliness knob: 4 px on each side, so the
 * target is 71 x 49. Nothing else lives within it on the performance screen --
 * measured, and the frame is black from y 777 to the deck panel's rule at
 * y 789, from x 9 to the panel divider at x 106, in that band.
 *
 * All ten captures are PIXEL-IDENTICAL in this whole region, which is the
 * finding this feature exists for: rbp draws the widget and never lets a touch
 * on it mean anything. See touch_zone.h.
 *
 * ONE SCREEN DISAGREES, and the next reader should not have to rediscover it.
 * The BROWSE screen's bottom band is the deck strip too, and rbp binds a control
 * over the deck-1 time display there: its digits (x 22..94, y 746..784 by
 * measurement) toggle TIME/REMAIN when touched, which is rbp's own behaviour and
 * not something to fight. Injections into the browse screen on 2026-09-28 --
 * wire x 1224 for logical x 55, so the same place a finger lands on the
 * performance screen -- gave, in one run: null 416 px, tap(y 755) 2940 px, the
 * same tap again 2940 px (the label flipped and flipped back, read off the
 * frames), tap(y 745) 208 px i.e. the baseline (the label row is inert), and
 * tap(y 760) 2732 px. The zone below overlaps that control in BOTH axes, so it
 * cannot be trimmed out of it; the gate is the answer. See touch_zone.h and
 * docs/07-touch.md.
 */
#define QZ_WIDGET_X0 26
#define QZ_WIDGET_X1 88
#define QZ_WIDGET_Y0 736
#define QZ_WIDGET_Y1 776
#define QZ_MARGIN    4
#define QZ_DECK2_DX  640

#define QZ_X0 (QZ_WIDGET_X0 - QZ_MARGIN)
#define QZ_X1 (QZ_WIDGET_X1 + QZ_MARGIN)
#define QZ_Y0 (QZ_WIDGET_Y0 - QZ_MARGIN)
#define QZ_Y1 (QZ_WIDGET_Y1 + QZ_MARGIN)

/* Inclusive bounds deliberately: a finger landing on the widget's own outermost
 * row or column is on the button, and the margin is what covers the noise in
 * where exactly that edge is. */
static int zone_deck(int x, int y)
{
     if (y < QZ_Y0 || y > QZ_Y1)
          return 0;
     if (x >= QZ_X0 && x <= QZ_X1)
          return 1;
     if (x >= QZ_X0 + QZ_DECK2_DX && x <= QZ_X1 + QZ_DECK2_DX)
          return 2;
     return 0;
}

/* The last state the pointer path reported. Only the transition into "down"
 * matters -- see touch_zone.h for why the gesture is complete at that moment. */
static int was_down;

int touch_zone_feed(int down, int x, int y)
{
     if (!down) {
          was_down = 0;
          return 0;
     }
     if (was_down)
          return 0;                 /* still the same press */
     was_down = 1;
     return zone_deck(x, y);
}

void touch_zone_reset(void)
{
     was_down = 0;
}
