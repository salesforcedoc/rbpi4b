/*
 * ctrl_map.c -- the binding tables every map fills in.
 *
 * There is nothing controller-specific here, which is the point: the two shapes
 * (a note that means a keycode, a CC that means a position) are the same for
 * every surface, and holding them in one place is what lets the maps be read as
 * tables rather than as bookkeeping.
 *
 * The arrays are not `static` because two other modules need them: the active
 * map, which fills them, and the absolute-value query in midi_io.c, which
 * invalidates them.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrl_map.h"
#include "shimutil.h"   /* klog, for map_none's one line */

struct note_ctrl note_map[CTRL_NKEYS];
int note_map_n = 0;

struct abs_ctrl abs_map[CTRL_NABS];
int abs_map_n = 0;

void ctrl_bindings_reset(void)
{
     note_map_n = 0;
     abs_map_n = 0;
}

void add_note(int rch, int note, int key, int sch)
{
     if (note_map_n >= CTRL_NKEYS)
          return;
     note_map[note_map_n].rch = rch;
     note_map[note_map_n].note = note;
     note_map[note_map_n].key = key;
     note_map[note_map_n].sch = sch;
     note_map[note_map_n].pressed = 0;
     note_map_n++;
}

void add_abs(int rch, int cc, int key, int sch)
{
     if (abs_map_n >= CTRL_NABS)
          return;
     abs_map[abs_map_n].rch = rch;
     abs_map[abs_map_n].cc = cc;
     abs_map[abs_map_n].key = key;
     abs_map[abs_map_n].sch = sch;
     abs_map[abs_map_n].last = -1;
     abs_map_n++;
}

void ctrl_abs_invalidate(void)
{
     int i;
     for (i = 0; i < abs_map_n; i++)
          abs_map[i].last = -1;
}

/* ---- map_none ------------------------------------------------------------
 *
 * The selection that means "no surface here". It lives in this file, beside the
 * tables, rather than in a map_none.c of its own, because it is the absence of a
 * surface: there is no table for it to fill and nothing for it to read, and a
 * file whose whole content is a NULL initialiser would read as a surface that
 * had failed to load.
 *
 * Its build() logs, and that is the reason it has one at all. A silent "none"
 * and a shim that never started the side it was asked to start are the same
 * observation from the log, and the one thing this map exists to prevent is
 * exactly that confusion -- so it says so, once, at the moment it is selected. */
static void none_build(void)
{
     klog("knobshim2: none: no surface on this side; nothing built "
          "(this is a selection, not a failure)\n");
}

const struct ctrl_map map_none = {
     "none",
     none_build,
     NULL,   /* startup(): rbp is told nothing on behalf of nothing */
     NULL,   /* event(): no sequencer events are consumed */
     NULL,   /* tick(): nothing is about time */
     NULL,   /* devices(): no non-MIDI source wanted */
     NULL,   /* input(): ditto */
     NULL,   /* leds: no surface, so nothing to illuminate */
};
