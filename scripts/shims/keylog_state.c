/*
 * keylog_state.c -- the recorder behind keylog.h's format. See keylog_state.h
 * for why the state is a shared default-visibility global and not a static.
 *
 * The file is opened "w" and the header written once; every record is flushed as
 * it is written, because a key dump is read after something has gone wrong, which
 * is exactly when the process may not have exited cleanly (mididump.c's reason,
 * and it applies twice over here: rbp is known to exit by itself).
 *
 * WHAT IS IN THE HEADER, and why each line is there, since the header is what
 * makes the dump replayable rather than merely readable:
 *
 *   # when   -- the wall clock, so two dumps can be ordered; the monotonic base,
 *               so a record's `t` can be turned back into an absolute instant if
 *               the dump is ever compared against another log; the pid.
 *   # rbp    -- the player binary's path and size. A replay is only a replay
 *               against the same player, and this project patches rbp's binary.
 *   # media  -- THE REPLAY CONDITION. Slots, filesystem UUIDs and volume labels.
 *               A key dump says "deck 1, RELOOP"; which track that was is the
 *               media's business, so the dump names the media instead.
 *
 * Everything here is libc and stdio only. No shimutil (fbshim does not link it),
 * no syscalls.h, and NO stat() -- the vendor libc has no `stat`, and a shim that
 * calls it links fine and then kills rbp at startup. The player's size comes from
 * fseek/ftell on its own /proc/self/exe instead, which is stdio's business.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "keylog.h"
#include "keylog_state.h"

/* THE SHARED RECORDER. One instance, whatever the load order: both preloaded
 * libraries define these and the dynamic linker resolves every reference to the
 * first definition in the search order. */
struct keylog_recorder {
     FILE *f;                            /* NULL until opened, and in non-rbp */
     unsigned long long t0;              /* the first event's clock, in us */
     int started;                        /* t0 is set */
     char media[256];                    /* the last `# media` line written */
     unsigned long long media_checked;   /* ms of the last media re-read */
};
struct keylog_recorder g_keylog;

struct keylog_media {
     char label[48];
     char uuid[48];
};

/* An environment string, with the shim's contract: NULL and empty are both
 * "unset". start-rb.sh exports every name in SHIM_VARS even when nobody set one,
 * so a bare presence test would always be true and KEY_DUMP= (an empty export)
 * would look like a path. */
static const char *env_path(const char *name)
{
     const char *v = getenv(name);

     return (v && v[0]) ? v : NULL;
}

/* One small file into `out`, whitespace dropped and anything unprintable or
 * quote-shaped replaced by '?'. An absent file, an empty one and a
 * whitespace-only one are all the same answer -- the empty string -- which is
 * what the header line prints as `label=''`. Whitespace is skipped rather than
 * trimmed afterwards, so both ends go in one pass and a file with a trailing
 * newline is the same string as one without (usb-watch.sh's `printf '%s'` and a
 * hand-edited file must not differ). */
static void read_small(const char *path, char *out, size_t n)
{
     FILE *f;
     int c, i = 0;

     if (n == 0)
          return;
     out[0] = '\0';
     f = fopen(path, "r");
     if (!f)
          return;
     while (i + 1 < (int)n && (c = fgetc(f)) != EOF) {
          if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
               continue;
          out[i++] = (c >= 0x20 && c <= 0x7e && c != '\'' && c != '"')
                     ? (char)c : '?';
     }
     fclose(f);
     out[i] = '\0';
}

static void read_slot(int slot, struct keylog_media *m)
{
     char path[64];

     snprintf(path, sizeof(path), "/tmp/udev_usb%d.label", slot);
     read_small(path, m->label, sizeof(m->label));
     snprintf(path, sizeof(path), "/tmp/udev_usb%d.uuid", slot);
     read_small(path, m->uuid, sizeof(m->uuid));
}

static void media_line(char *out, size_t n)
{
     struct keylog_media a, b;

     read_slot(1, &a);
     read_slot(2, &b);
     snprintf(out, n,
              "# media usb1 uuid='%s' label='%s' usb2 uuid='%s' label='%s'",
              a.uuid, a.label, b.uuid, b.label);
}

/* The player's own size, by seeking its own /proc/self/exe. stdio rather than
 * stat(): the vendor libc has no `stat`, and a shim that calls it links fine and
 * then kills rbp at startup (rbp_abi.h records the measurement). -1 when it
 * cannot be read, which the header prints as `size -1`. */
static long long rbp_size(const char *path, int have_path)
{
     FILE *f;
     long long sz = -1;

     if (!have_path)
          return -1;
     f = fopen(path, "rb");
     if (!f)
          return -1;
     if (fseek(f, 0, SEEK_END) == 0)
          sz = (long long)ftell(f);
     fclose(f);
     return sz;
}

/* rbp is the process this is loaded into, so /proc/self/exe names the exact
 * binary that received the commands -- which matters here more than usual,
 * because this project patches that binary. */
static void rbp_line(char *out, size_t n)
{
     char exe[256];
     ssize_t k;

     exe[0] = '\0';
     k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
     if (k > 0)
          exe[k] = '\0';
     snprintf(out, n, "# rbp %s size %lld",
              k > 0 ? exe : "(unknown)", rbp_size(exe, k > 0));
}

void keylog_open(void)
{
     const char *path = env_path("KEY_DUMP");
     char line[320];
     struct timespec ts;
     struct tm tm;
     time_t now;

     if (!path || g_keylog.f)
          return;
     g_keylog.f = fopen(path, "w");
     if (!g_keylog.f)
          return;

     fputs(KEYLOG_HEADER "\n", g_keylog.f);

     clock_gettime(CLOCK_MONOTONIC, &ts);
     now = time(NULL);
     gmtime_r(&now, &tm);
     strftime(line, sizeof(line), "%Y-%m-%dT%H:%M:%SZ", &tm);
     fprintf(g_keylog.f, "# when %s epoch %lld mono_us %llu pid %d\n", line,
             (long long)now,
             (unsigned long long)ts.tv_sec * 1000000ULL +
             (unsigned long long)ts.tv_nsec / 1000ULL,
             (int)getpid());

     rbp_line(line, sizeof(line));
     fputs(line, g_keylog.f);
     fputc('\n', g_keylog.f);
     fflush(g_keylog.f);
}

void keylog_note(const char *fmt, ...)
{
     char body[256];
     va_list ap;

     if (!g_keylog.f || !fmt)
          return;
     va_start(ap, fmt);
     vsnprintf(body, sizeof(body), fmt, ap);
     va_end(ap);
     fprintf(g_keylog.f, "# %s\n", body);
     fflush(g_keylog.f);
}

void keylog_record(int op, int key, int ch, long param, float fval, long lval,
                   const char *src)
{
     struct keylog_event ev;
     unsigned long long now;

     if (!g_keylog.f)
          return;
     now = keylog_now_us();
     if (!g_keylog.started) {
          g_keylog.started = 1;
          g_keylog.t0 = now;
     }

     /* THE REPLAY CONDITION, written beside the first event and re-written when
      * the media changes. Throttled to a second so a spun jog, which is hundreds
      * of records a second, does not turn into hundreds of file reads. */
     if (now - g_keylog.media_checked > 1000000ULL) {
          char media[256];
          media_line(media, sizeof(media));
          if (strcmp(media, g_keylog.media) != 0) {
               strcpy(g_keylog.media, media);
               fputs(media, g_keylog.f);
               fputc('\n', g_keylog.f);
          }
          g_keylog.media_checked = now;
     }

     memset(&ev, 0, sizeof(ev));
     ev.t = (double)(now - g_keylog.t0) / 1000000.0;
     ev.op = op;
     ev.key = key;
     ev.ch = ch;
     ev.param = param;
     ev.fval = fval;
     ev.lval = lval;
     if (src) {
          strncpy(ev.src, src, sizeof(ev.src) - 1);
          ev.src[sizeof(ev.src) - 1] = '\0';
     }
     keylog_write(g_keylog.f, &ev);
     /* Flushed per record, like MIDI_DUMP and for the same reason. */
     fflush(g_keylog.f);
}
