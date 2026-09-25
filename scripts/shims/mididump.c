/*
 * mididump.c -- the MIDI_DUMP writer and the MIDI_REPLAY reader.
 *
 * See mididump.h for the format and why it exists. Two properties are worth
 * spelling out here because they are what makes the format usable rather than
 * merely readable:
 *
 *   - Everything is recorded. An event type the replay path cannot reconstruct
 *     is written as `SKIP type=N`, so a dump is a complete record of what the
 *     surface sent. Otherwise "the control sent nothing" and "the shim's format
 *     dropped it" look identical, which is exactly the ambiguity a bring-up dump
 *     is supposed to remove.
 *
 *   - Nothing is fatal. The parser returns "no event here" for a blank line, a
 *     comment, a SKIP, or a line that does not parse, so replaying a dump that
 *     is still being appended to does not stop at the half-written tail.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mididump.h"

/* Compose in a local buffer first so a too-small destination is reported rather
 * than silently truncated: snprintf's return value is the length it *would*
 * have written, which is the check. */
static int finish(char *buf, size_t n, const char *tmp, int len)
{
     if (len < 0 || (size_t)len >= n)
          return -1;
     memcpy(buf, tmp, (size_t)len + 1);
     return len;
}

int mididump_format(const struct snd_seq_event *ev, unsigned long long t_us,
                    char *buf, size_t n)
{
     char tmp[128];
     int len;

     switch (ev->type) {
     case SNDRV_SEQ_EVENT_NOTEON:
     case SNDRV_SEQ_EVENT_NOTEOFF:
          len = snprintf(tmp, sizeof(tmp), "%.6f %s ch=%d note=%d vel=%d",
                         (double)t_us / 1000000.0,
                         ev->type == SNDRV_SEQ_EVENT_NOTEON ? "NOTEON"
                                                            : "NOTEOFF",
                         ev->data.note.channel, ev->data.note.note,
                         ev->data.note.velocity);
          break;
     case SNDRV_SEQ_EVENT_CONTROLLER:
          len = snprintf(tmp, sizeof(tmp), "%.6f CONTROLLER ch=%d cc=%d val=%d",
                         (double)t_us / 1000000.0,
                         ev->data.control.channel, ev->data.control.param,
                         ev->data.control.value);
          break;
     default:
          len = snprintf(tmp, sizeof(tmp), "%.6f SKIP type=%d",
                         (double)t_us / 1000000.0, (int)ev->type);
          break;
     }
     return finish(buf, n, tmp, len);
}

void mididump_write(FILE *f, const struct snd_seq_event *ev,
                    unsigned long long t_us)
{
     char line[160];
     if (!f)
          return;
     if (mididump_format(ev, t_us, line, sizeof(line)) < 0)
          return;
     fputs(line, f);
     fputc('\n', f);
}

/* Parse one line. `t` (optional) receives the recorded timestamp; the public
 * mididump_parse() is this without it, so the replay loop can pace itself
 * without parsing the line twice. */
static int parse_line(const char *line, struct snd_seq_event *out, double *t)
{
     const char *p;
     char *end;
     double when;
     int ch, a, b;

     if (!line || line[0] == '\0' || line[0] == '\n' || line[0] == '#')
          return 0;

     when = strtod(line, &end);
     if (end == line)
          return 0;
     p = end;
     while (*p == ' ' || *p == '\t')
          p++;

     /* Longest name first: "NOTEON" and "NOTEOFF" share their first five
      * characters, and a strncmp short enough to accept both would also accept
      * a name that is neither. */
     if (strncmp(p, "CONTROLLER ", 11) == 0) {
          p += 11;
          if (sscanf(p, "ch=%d cc=%d val=%d", &ch, &a, &b) != 3)
               return 0;
          memset(out, 0, sizeof(*out));
          out->type = SNDRV_SEQ_EVENT_CONTROLLER;
          out->data.control.channel = ch;
          out->data.control.param = a;
          out->data.control.value = b;
     } else if (strncmp(p, "NOTEOFF ", 8) == 0) {
          p += 8;
          if (sscanf(p, "ch=%d note=%d vel=%d", &ch, &a, &b) != 3)
               return 0;
          memset(out, 0, sizeof(*out));
          out->type = SNDRV_SEQ_EVENT_NOTEOFF;
          out->data.note.channel = ch;
          out->data.note.note = a;
          out->data.note.velocity = b;
     } else if (strncmp(p, "NOTEON ", 7) == 0) {
          p += 7;
          if (sscanf(p, "ch=%d note=%d vel=%d", &ch, &a, &b) != 3)
               return 0;
          memset(out, 0, sizeof(*out));
          out->type = SNDRV_SEQ_EVENT_NOTEON;
          out->data.note.channel = ch;
          out->data.note.note = a;
          out->data.note.velocity = b;
     } else {
          /* SKIP, or anything else: recorded, not replayable. */
          return 0;
     }
     if (t)
          *t = when;
     return 1;
}

int mididump_parse(const char *line, struct snd_seq_event *out)
{
     return parse_line(line, out, NULL);
}

static void pace(double seconds)
{
     struct timespec ts;
     if (seconds <= 0.0)
          return;
     if (seconds > 3600.0)                 /* a corrupt timestamp: ignore it */
          return;
     ts.tv_sec = (time_t)seconds;
     ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
     while (nanosleep(&ts, &ts) != 0)
          ;                                  /* EINTR: finish the remainder */
}

int mididump_replay(const char *path, double speed,
                    void (*on_event)(const struct snd_seq_event *ev))
{
     FILE *f;
     char line[512];
     struct snd_seq_event ev;
     double t, prev = 0.0;
     int n = 0, first = 1;

     if (!path || !path[0] || !on_event)
          return -1;
     f = fopen(path, "r");
     if (!f)
          return -1;
     while (fgets(line, sizeof(line), f)) {
          if (!parse_line(line, &ev, &t))
               continue;
          if (!first && speed > 0.0)
               pace((t - prev) / speed);
          prev = t;
          first = 0;
          on_event(&ev);
          n++;
     }
     fclose(f);
     return n;
}
