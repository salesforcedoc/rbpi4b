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
void klog(const char *fmt, ...)
{
     char buf[256];
     va_list ap;
     va_start(ap, fmt);
     int n = vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);
     if (n > 0) {
          int fd = real_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
          if (fd >= 0) {
               (void)write(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
               real_close(fd);
          }
     }
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
