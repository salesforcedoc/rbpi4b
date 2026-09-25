/*
 * mididump.h -- MIDI_DUMP and MIDI_REPLAY: recording the surface and playing it
 * back.
 *
 * A map cannot be written from a datasheet. Which channel a section emits on,
 * which CCs are switches and at what threshold, the jog's pulses per turn --
 * all of it differs between controllers and between firmware revisions of the
 * same controller. So the shim can record everything it sees and replay it
 * later, with no hardware attached.
 *
 * The format is one event per line, greppable and hand-editable:
 *
 *   # rblive4 midi dump v1 -- t is seconds since the first event
 *   0.000000 NOTEON ch=0 note=36 vel=127
 *   0.001234 CONTROLLER ch=4 cc=17 val=64
 *   2.000000 SKIP type=8
 *
 * Three event types are replayable because they are the three a control surface
 * uses; anything else is recorded as SKIP so a dump is a complete record of what
 * arrived, and the parser skips it. Every event goes in, including the ones no
 * map consumes -- a dump that quietly dropped them would make "there was no
 * event" and "my map is wrong" look the same.
 *
 * This file is deliberately free of the shim's other dependencies: no globals,
 * no logging, no syscalls.h. That is what lets `make test` link it together with
 * a map and drive the real code path on a bench.
 */
#ifndef RBLIVE4_MIDIDUMP_H
#define RBLIVE4_MIDIDUMP_H

#include <stddef.h>
#include <stdio.h>
#include <sound/asequencer.h>   /* struct snd_seq_event */

/* The header line written at the top of a dump. */
#define MIDIDUMP_HEADER "# rblive4 midi dump v1 -- t is seconds since the first event"

/* Render one event as one line (without the newline), with `t_us` microseconds
 * since the start of the dump. Always succeeds: a type the format cannot carry
 * is rendered as SKIP. Returns the length written, or -1 if `buf` is too small. */
int mididump_format(const struct snd_seq_event *ev, unsigned long long t_us,
                    char *buf, size_t n);

/* Parse one line in that format into `out`. Returns 1 for a usable event, 0 for
 * a line that carries nothing to dispatch -- blank, a comment, SKIP, or the
 * tail of a dump that is still being written. Never returns -1: a replay of a
 * file that is being appended to must not stop at the half-written last line. */
int mididump_parse(const char *line, struct snd_seq_event *out);

/* Write one event to an open dump, prefixed by its timestamp. */
void mididump_write(FILE *f, const struct snd_seq_event *ev,
                    unsigned long long t_us);

/* Feed a whole dump to on_event in file order, dividing the recorded gaps by
 * `speed`. `speed` <= 0 means no pacing at all, which is what the unit test
 * uses. Returns the number of events dispatched, or -1 if the file could not be
 * opened. */
int mididump_replay(const char *path, double speed,
                    void (*on_event)(const struct snd_seq_event *ev));

#endif /* RBLIVE4_MIDIDUMP_H */
