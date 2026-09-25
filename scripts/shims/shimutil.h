/*
 * shimutil.h -- the log, the clock, and how the controls shim reads a flag.
 *
 * See shimutil.c. `verbose` is defined in ctrlshim.c (the module that reads the
 * environment) and read by any module whose logging it gates, which is why it
 * is declared here rather than beside its users.
 *
 * The environment helpers are here rather than in envutil.h deliberately.
 * envutil.h belongs to the other two shims and its env_flag() answers "is this
 * string something other than 0?"; the controls shim needs the stricter
 * question "did the operator write one of the words that means yes?", because
 * an export of `LED_VERBOSE=no` must be off and not on. Two different
 * questions, so two different helpers rather than one that has to guess.
 */
#ifndef RBLIVE4_SHIMUTIL_H
#define RBLIVE4_SHIMUTIL_H

#include <time.h>       /* clock_gettime, for shim_now_ms() */

/* Append one line to /tmp/knobshim.log. Unconditional: gate chatty callers on
 * `verbose` yourself. */
void klog(const char *fmt, ...);

/* KNOB_VERBOSE. */
extern int verbose;

/* Monotonic milliseconds. `static inline` like syscalls.h and for the same
 * reason: every module that needs it gets its own copy, so nothing is added to
 * the dynamic symbol table and `make test` can link one module on its own. */
static inline unsigned long long shim_now_ms(void)
{
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return (unsigned long long)ts.tv_sec * 1000ULL +
            (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* A yes/no switch, read by value. NULL, empty, "1", "true" or "yes" (any case)
 * is on; "0", "false", "no" and every other value is off. An empty or absent
 * value takes `dflt`.
 *
 * This is the contract rb.conf's flags are written against -- start-rb.sh's
 * SHIM_VARS loop exports every name in its list even when nobody set one, so
 * "absent" never arrives as NULL and a presence test on these names is always
 * true. */
int env_on(const char *name, int dflt);

/* A number, read by value: NULL or empty takes `dflt`, and so does anything
 * that is not a complete integer. A value of 0 is honoured, which is the point
 * -- an exported empty string must not become 0. */
int env_num(const char *name, int dflt);

/* The same for a fraction (MIDI_REPLAY_SPEED). Separate rather than one
 * do-everything reader, so that "0.5" reaching an integer setting is a compile
 * error at the call site instead of a silent fallback to the default. */
double env_dnum(const char *name, double dflt);

/* A string: NULL or empty -> dflt, otherwise the value. The pointer is into the
 * environment (never freed, never modified). */
const char *env_text(const char *name, const char *dflt);

#endif /* RBLIVE4_SHIMUTIL_H */
