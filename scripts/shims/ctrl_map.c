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
