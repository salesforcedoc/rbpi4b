/*
 * keylog.h -- KEY_DUMP and KEY_REPLAY: every command the shim hands rbp,
 * recorded so the session can be played back later.
 *
 * MIDI_DUMP records what a *surface* sent. That is not the same thing as what
 * rbp was told, and the difference is the whole reason this file exists:
 *
 *   - a touch on the glass that the shim turns into a keycode (a QUANTIZE tap,
 *     a side drawer's SYNC, a hot cue pad) is not a sequencer event, so
 *     MIDI_DUMP has nothing to record it as;
 *   - the mapping from a note to a keycode lives in a map, and a map can be
 *     edited -- so replaying a MIDI dump through today's map replays today's
 *     *interpretation* of those notes, not the commands rbp actually received;
 *   - MIDI_DUMP can only replay while a sequencer and a working map are
 *     present, because that is the path it drives.
 *
 * A key dump records the OTHER end: the call rbp's KeyManager was handed, with
 * the keycode, the operation, the channel and the three payloads exactly as
 * sent, from whichever source produced them, in one ordered stream. Replaying it
 * needs no controller, no map and no sequencer -- only rbp.
 *
 * THE FORMAT IS THE POINT, and it is deliberately the shape mididump.c uses: a
 * one-line version header, `#` comments (the header carries the conditions --
 * see below), then one event per line, greppable and hand-editable:
 *
 *   # rbpi4b key dump v1 -- t is seconds since the first event; op 0=press ...
 *   # when 2026-10-09T09:04:11Z epoch 1791... mono_us 8...  pid 437
 *   # rbp /root/pdj/rbp size 8123456
 *   # shim maps MIDI_MAP=flx4 EVDEV_MAP=kbd
 *   # media usb1 uuid='1A2B-3C4D' label='SANDISK' usb2 uuid='' label=''
 *   0.000000 op=0 key=0x410e ch=1 param=0 f=0.000000 l=0 src=midi
 *   0.083214 op=2 key=0x410e ch=1 param=0 f=0.000000 l=0 src=midi
 *
 * THE MEDIA LINE IS THE REPLAY CONDITION. A key dump is deck-relative: it says
 * "deck 1, RELOOP" and not "the fourth track on the stick". So a replay
 * reproduces the *performance* only against the same media, and the header
 * names that media -- the slot, the filesystem UUID (the device's own serial,
 * from blkid) and the volume label -- so a downstream reader can check it
 * before replaying, or refuse and say why. `# media` is re-emitted whenever the
 * media changes, so the LAST one before an event is the one that event played
 * against.
 *
 * TIMING. `t` is seconds since the first event with microsecond resolution,
 * from a real CLOCK_MONOTONIC microsecond clock -- not milliseconds with three
 * zeroes appended, which is what MIDI_DUMP's `shim_now_ms() * 1000` amounts to.
 * A performance is a timing; a replay that quantised every gap to 1 ms would not
 * be one. keylog_replay() preserves each gap exactly, divided by `speed`.
 *
 * This file is free of the shim's other dependencies -- no globals, no logging,
 * no syscalls.h -- which is what lets `make test` link it and pin the format on
 * a host. keylog_state.c is the part that is a FILE and an environment variable.
 */
#ifndef RBPI4B_KEYLOG_H
#define RBPI4B_KEYLOG_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>       /* clock_gettime, for keylog_now_us() */

/* The header line written at the top of a dump. The op legend is here rather
 * than in a document because a dump gets read on its own, years downstream. */
#define KEYLOG_HEADER \
     "# rbpi4b key dump v1 -- t is seconds since the first event; " \
     "op 0=press 1=repeat 2=release 4=rotate 5=value; src is the source that sent it"

/* A source tag's bytes, including the NUL. Long enough for the longest name the
 * shim uses ("touch", "evdev", "midi") with room to spare; a tag that does not
 * fit is truncated rather than refused -- the tag is a label, and a label is not
 * worth a failing record. */
#define KEYLOG_SRC_MAX 16

/* One command, as it was handed to rbp. `t` is filled by the reader on replay
 * and by the writer from the clock; everything else is the call's arguments. */
struct keylog_event {
     double  t;                          /* seconds since the first event */
     int     op;                         /* OP_PRESS/REPEAT/RELEASE/ROTATE/VALUE */
     int     key;                        /* the keycode */
     int     ch;                         /* the channel (deck, or CH_GLOBAL) */
     long    param;
     float   fval;
     long    lval;
     char    src[KEYLOG_SRC_MAX];        /* "midi", "touch", "evdev", ... */
};

/* Render one event as one line (without the newline), using `ev->t`. Always
 * succeeds. Returns the length written, or -1 if `buf` is too small. */
int keylog_format(const struct keylog_event *ev, char *buf, size_t n);

/* Parse one line in that format into `out`. Returns 1 for an event, 0 for a
 * line that carries none -- blank, a comment, a header, or the tail of a dump
 * still being written. Never -1, so a replay of a growing file does not stop at
 * the half-written last line. An unknown field is ignored and a missing one
 * keeps its zero, so a v1 reader still reads a v2 line. */
int keylog_parse(const char *line, struct keylog_event *out);

/* Write one event to an open dump, with a trailing newline. */
void keylog_write(FILE *f, const struct keylog_event *ev);

/* Feed a whole dump to on_event in file order, dividing the recorded gaps by
 * `speed`. `speed` <= 0 means no pacing at all, which is what the unit test
 * uses. Returns the number of events dispatched, or -1 if the file could not be
 * opened. */
int keylog_replay(const char *path, double speed,
                  void (*on_event)(const struct keylog_event *ev));

/* Monotonic MICROseconds. `static inline` like shimutil.h's millisecond clock
 * and for the same reason: every module that needs it gets its own copy, so
 * nothing is added to the dynamic symbol table and a host test can link one
 * module on its own.
 *
 * `unsigned long long`, never `long`: on this target `long` is 4 bytes, and a
 * nanosecond CLOCK_MONOTONIC in one wraps every 4.29 s. */
static inline unsigned long long keylog_now_us(void)
{
     struct timespec ts;

     clock_gettime(CLOCK_MONOTONIC, &ts);
     return (unsigned long long)ts.tv_sec * 1000000ULL +
            (unsigned long long)ts.tv_nsec / 1000ULL;
}

#endif /* RBPI4B_KEYLOG_H */
