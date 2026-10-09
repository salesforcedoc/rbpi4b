/*
 * keylog_state.h -- the KEY_DUMP recorder: the file, the clock base, and the
 * header that says what conditions the recording was made under.
 *
 * keylog.h is the format; this is the part of the shim that opens a FILE and
 * reads an environment variable. Split for two reasons:
 *
 *   - keylog.h/.c stay pure, so `make test` pins the format on a host with no
 *     rbp, no chroot and no unit.
 *
 *   - THE STATE HAS TO BE SHARED BETWEEN THE TWO LIBRARIES that can record.
 *     rbp_key.o is linked into BOTH fbshim.so and knobshim.so, and both copies
 *     are live: the touch path (pointsrc.c, fbshim) sends keycodes through
 *     fbshim's copy and the control surface (map_flx4.c, knobshim) through
 *     knobshim's. If each library owned its own FILE the dump would be two dumps
 *     interleaved -- two headers, two clock bases, two orders. So the state here
 *     is a NON-STATIC, DEFAULT-VISIBILITY global linked into both, exactly like
 *     fader_state.o and rbp_transport.o: a default-visibility symbol defined in
 *     two preloaded libraries resolves for BOTH to the first one in the search
 *     order, so there is one recorder no matter which library calls it. That is
 *     also why this object has its own rule in the Makefile rather than a place
 *     in either object list, both of which compile hidden.
 */
#ifndef RBPI4B_KEYLOG_STATE_H
#define RBPI4B_KEYLOG_STATE_H

/* Open the dump if KEY_DUMP names a path, and write the header. Idempotent, and
 * a no-op when KEY_DUMP is unset or empty -- which is how the recorder is off by
 * default. Called from the shim's startup; a record before it is dropped. */
void keylog_open(void);

/* Append one `#` line -- the header's continuation. Used for the things the
 * recorder cannot know on its own (which maps this run selected) and for
 * anything a caller wants downstream to find beside the events. */
void keylog_note(const char *fmt, ...);

/* Record one command. `src` is the source's tag ("midi", "touch", "evdev") or
 * NULL. A no-op until keylog_open() has a file, so callers need no gate of their
 * own; the first record also writes the `# media` line, which is the replay
 * condition and is re-written whenever the media changes. */
void keylog_record(int op, int key, int ch, long param, float fval, long lval,
                   const char *src);

#endif /* RBPI4B_KEYLOG_STATE_H */
