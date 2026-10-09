/*
 * keylog.c -- the KEY_DUMP writer and the KEY_REPLAY reader.
 *
 * See keylog.h for what a key dump is for and why its timing is in real
 * microseconds. Three properties of this implementation are worth naming:
 *
 *   - ONE LINE PER CALL, FIXED FIELD ORDER. A record is written by a program,
 *     not by a person, so the fields go out in the order they are declared. The
 *     READER does not depend on that order -- it looks each field up by name --
 *     which is what makes a hand-written line, or a line from a later version
 *     with an extra field, still parse.
 *
 *   - NOTHING IS FATAL. The parser answers "no event here" for a comment, a
 *     header, a blank line, or a line with no timestamp, so replaying a dump
 *     that is still being appended to does not stop at the partial tail.
 *
 *   - THE GAPS ARE THE PERFORMANCE. keylog_replay() sleeps the recorded
 *     difference between consecutive events; it does not re-time them. With
 *     `speed` at 1.0 the replay is the recording.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "keylog.h"

/* Compose in a local buffer first so a too-small destination is reported rather
 * than silently truncated: snprintf's return is the length it *would* have
 * written, which is the check. */
static int finish(char *buf, size_t n, const char *tmp, int len)
{
     if (len < 0 || (size_t)len >= n)
          return -1;
     memcpy(buf, tmp, (size_t)len + 1);
     return len;
}

int keylog_format(const struct keylog_event *ev, char *buf, size_t n)
{
     char tmp[192];
     int len;

     if (!ev)
          return -1;
     /* `%.9g` on the float, not `%.6f`, and that is deliberate: nine significant
      * decimal digits is FLT_DECIMAL_DIG, so a float written and read back is the
      * SAME float -- which is what makes "the replay is the recording" true
      * rather than nearly true. The map computed that value and rbp reads it; a
      * replay that handed rbp a neighbour of it would be a replay of a
      * performance that never happened. `%g` also keeps the common cases short
      * (0.5, 1, 0) instead of padding them to six places.
      *
      * The TIMESTAMP stays `%.6f`: microseconds, fixed width, so the column lines
      * up and a dump can be sorted with `sort -n` as it stands. */
     len = snprintf(tmp, sizeof(tmp),
                    "%.6f op=%d key=%#x ch=%d param=%ld f=%.9g l=%ld",
                    ev->t, ev->op, ev->key, ev->ch,
                    ev->param, (double)ev->fval, ev->lval);
     if (len < 0 || (size_t)len >= sizeof(tmp))
          return -1;
     if (ev->src[0]) {
          int extra = snprintf(tmp + len, sizeof(tmp) - (size_t)len, " src=%s",
                               ev->src);
          if (extra < 0 || (size_t)(len + extra) >= sizeof(tmp))
               return -1;
          len += extra;
     }
     return finish(buf, n, tmp, len);
}

void keylog_write(FILE *f, const struct keylog_event *ev)
{
     char line[224];

     if (!f)
          return;
     if (keylog_format(ev, line, sizeof(line)) < 0)
          return;
     fputs(line, f);
     fputc('\n', f);
}

/* The value of a `name=` field, or NULL. A field has to start a token -- the
 * line, or a space before it -- so `f=` cannot match inside `param=` and a key
 * named like another field's tail cannot be mistaken for one. */
static const char *find_field(const char *s, const char *name)
{
     size_t nl = strlen(name);
     const char *p = s;

     while ((p = strstr(p, name)) != NULL) {
          if ((p == s || p[-1] == ' ' || p[-1] == '\t') && p[nl] == '=')
               return p + nl + 1;
          p += nl;
     }
     return NULL;
}

/* A whole long out of a field. Returns 0 when the field is absent, and takes the
 * field's own base, so `key=0x410e` and `key=16654` are the same keycode. */
static long field_long(const char *line, const char *name, long dflt)
{
     const char *v = find_field(line, name);
     char *end;

     if (!v)
          return dflt;
     long r = strtol(v, &end, 0);
     return end == v ? dflt : r;
}

static double field_double(const char *line, const char *name, double dflt)
{
     const char *v = find_field(line, name);
     char *end;

     if (!v)
          return dflt;
     double r = strtod(v, &end);
     return end == v ? dflt : r;
}

static void field_str(const char *line, const char *name, char *out, size_t n)
{
     const char *v = find_field(line, name);
     size_t i = 0;

     if (n == 0)
          return;
     out[0] = '\0';
     if (!v)
          return;
     while (i + 1 < n && v[i] && v[i] != ' ' && v[i] != '\t' && v[i] != '\n')
          i++;
     memcpy(out, v, i);
     out[i] = '\0';
}

int keylog_parse(const char *line, struct keylog_event *out)
{
     const char *p;
     char *end;
     double when;

     if (!line || line[0] == '\0' || line[0] == '\n' || line[0] == '#')
          return 0;

     /* The timestamp is positional and has to come first: it is what tells an
      * event line from a line of anything else. */
     when = strtod(line, &end);
     if (end == line)
          return 0;
     p = end;
     while (*p == ' ' || *p == '\t')
          p++;
     /* A line whose timestamp is the whole line is a dump with the fields cut
      * off -- the tail of a file being written. Not an event yet. */
     if (*p == '\0' || *p == '\n')
          return 0;

     memset(out, 0, sizeof(*out));
     out->t     = when;
     out->op    = (int)field_long(p, "op", 0);
     out->key   = (int)field_long(p, "key", 0);
     out->ch    = (int)field_long(p, "ch", 0);
     out->param = field_long(p, "param", 0);
     out->fval  = (float)field_double(p, "f", 0.0);
     out->lval  = field_long(p, "l", 0);
     field_str(p, "src", out->src, sizeof(out->src));
     return 1;
}

static void pace(double seconds)
{
     struct timespec ts;

     if (seconds <= 0.0)
          return;
     if (seconds > 3600.0)                  /* a corrupt gap: ignore it */
          return;
     ts.tv_sec = (time_t)seconds;
     ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
     while (nanosleep(&ts, &ts) != 0)
          ;                                   /* EINTR: finish the remainder */
}

int keylog_replay(const char *path, double speed,
                  void (*on_event)(const struct keylog_event *ev))
{
     FILE *f;
     char line[1024];
     struct keylog_event ev;
     double prev = 0.0;
     int n = 0, first = 1;

     if (!path || !path[0] || !on_event)
          return -1;
     f = fopen(path, "r");
     if (!f)
          return -1;
     while (fgets(line, sizeof(line), f)) {
          /* A RECORD THE RECORDER WROTE IS ALWAYS NEWLINE-TERMINATED, so a line
           * without one is a write that was cut short -- the dump of a process
           * that was killed mid-flush. keylog_parse() is lenient on purpose (an
           * unknown field must not be fatal), and leniency is exactly wrong
           * here: `3.5 op=0 key=0x41` would be read as a press of a keycode
           * nobody sent. So the tail is refused by its shape rather than by
           * trusting the fields to be complete. Skipped, not stopped: the loop
           * ends when fgets does, so a tail costs nothing and a long line split
           * across two reads is not the end of the dump. */
          if (!strchr(line, '\n'))
               continue;
          if (!keylog_parse(line, &ev))
               continue;
          if (!first && speed > 0.0)
               pace((ev.t - prev) / speed);
          prev = ev.t;
          first = 0;
          on_event(&ev);
          n++;
     }
     fclose(f);
     return n;
}
