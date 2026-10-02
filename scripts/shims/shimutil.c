/*
 * shimutil.c -- one log and one environment contract for the whole controls shim.
 *
 * Every module logs through klog(), which appends to /tmp/knobshim.log. It is
 * unconditional: the callers gate the chatty lines on `verbose` (KNOB_VERBOSE),
 * because a per-event log from a MIDI bridge is only useful when you are
 * chasing one event in particular.
 *
 * The env_on()/env_num()/env_text() trio below is the shim's reading of
 * rb.conf's flags -- see shimutil.h for why they are not envutil.h's, which the
 * other two shims use. They were added because the flags used to be read by
 * presence (`getenv("LED_VERBOSE") != NULL`), which start-rb.sh's SHIM_VARS loop
 * turned into a landmine: it exports every name in its list, so "not set" and
 * "set to 0" both arrived as a non-NULL "" and the flag could not be turned off
 * from rb.conf at all.
 *
 * The raw syscalls that let a preloaded shim do its own I/O live in syscalls.h,
 * shared with the other shims; this file is the part that is specific to the
 * controls shim.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp, in env_on() */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>
#include <sys/mman.h>
#include <sound/asequencer.h>

/* State shared with the audio shim. This shim is first in LD_PRELOAD, so it is
 * the one that defines them (via shmstate.o) and owns them. */
#include "shmstate.h"

#include "syscalls.h"

#include "shimutil.h"

#define LOG_PATH "/tmp/knobshim.log"

/* The log's fd, opened once and kept.
 *
 * klog() used to open, write and close the file on EVERY call -- three syscalls
 * per line, and a per-event line can be called thousands of times a second. The
 * syscalls were not the only cost: the close is a full release of the file, so
 * two threads logging at once could not interleave, which sounds like a benefit
 * until the line is one the input thread has to get out of the way of.
 *
 * It is deliberately never closed. This lives in a process that runs until rbp
 * exits, so there is nothing to close it for.
 *
 * `log_fd == -1` means "not open yet", so a failed open is retried on the next
 * line rather than silencing the shim for good -- /tmp being momentarily
 * unwritable must not cost the log of whatever went wrong next.
 *
 * Every thread in this shim logs, and two of them finding -1 at the same moment
 * will both open the file, leaking one fd. That is the whole of the race: each
 * write() is a single call with a buffer that is already complete, so lines from
 * two threads cannot interleave or be lost, and one leaked fd on a path that can
 * run at most once per process is not worth a lock on the log. */
static int log_fd = -1;

void klog(const char *fmt, ...)
{
     char buf[256];
     va_list ap;
     int n;

     va_start(ap, fmt);
     n = vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);
     if (n <= 0)
          return;

     if (log_fd < 0)
          log_fd = real_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
     if (log_fd < 0)
          return;
     /* real_write and not write: a shim that ever interposed write() would send
      * its own log line into itself. */
     (void)real_write(log_fd, buf,
                      (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

/* The words that mean yes. Case-insensitive, because rb.conf is written by hand
 * and "True" is a thing people type. */
static int on_word(const char *v)
{
     return strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
            strcasecmp(v, "yes") == 0;
}

int env_on(const char *name, int dflt)
{
     const char *v = getenv(name);
     if (v == NULL || v[0] == '\0')
          return dflt;
     return on_word(v);
}

int env_num(const char *name, int dflt)
{
     const char *v = getenv(name);
     char *end;
     long n;
     if (v == NULL || v[0] == '\0')
          return dflt;
     n = strtol(v, &end, 10);
     if (end == v || *end != '\0')
          return dflt;      /* a typo degrades to the documented default */
     return (int)n;
}

const char *env_text(const char *name, const char *dflt)
{
     const char *v = getenv(name);
     if (v == NULL || v[0] == '\0')
          return dflt;
     return v;
}

/* A number with a fraction. env_num()'s contract exactly, for the one setting
 * that is not a count: MIDI_REPLAY_SPEED, where half speed is a real debugging
 * move and must not degrade to the default. */
double env_dnum(const char *name, double dflt)
{
     const char *v = getenv(name);
     char *end;
     double d;
     if (v == NULL || v[0] == '\0')
          return dflt;
     d = strtod(v, &end);
     if (end == v || *end != '\0')
          return dflt;
     return d;
}
