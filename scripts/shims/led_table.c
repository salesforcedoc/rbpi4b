/*
 * led_table.c -- see led_table.h for what this is for and why it is a separate
 * module. No address of rbp's appears here; the reads are rbp_led.c's.
 */
#include <string.h>

#include "led_table.h"

/* Little-endian u32 out of a byte buffer. memcpy rather than a cast because the
 * contract here is "some bytes rbp wrote", with no alignment promise, and a
 * table this module is handed in a test is a local array. */
static unsigned int le32(const unsigned char *p)
{
     unsigned int v;
     memcpy(&v, p, sizeof v);
     return v;
}

const unsigned char *led_find(const unsigned char *tbl, unsigned int count,
                              unsigned int id, unsigned int ch)
{
     unsigned int i;

     if (!tbl)
          return NULL;
     if (count > LED_DUMP_MAX)
          count = LED_DUMP_MAX;
     for (i = 0; i < count; i++) {
          const unsigned char *e = tbl + (size_t)i * LED_ENTRY_SIZE;
          if (le32(e + 0) == id && le32(e + 4) == ch)
               return e;
     }
     return NULL;
}

unsigned int led_merge_settled(const unsigned char *a, const unsigned char *b,
                               unsigned int count,
                               const unsigned char *prev, unsigned int prev_count,
                               unsigned char *out)
{
     unsigned int n = 0, i;

     if (!a || !b || !out)
          return 0;
     if (count > LED_DUMP_MAX)
          count = LED_DUMP_MAX;
     for (i = 0; i < count; i++) {
          const unsigned char *ea = a + (size_t)i * LED_ENTRY_SIZE;
          const unsigned char *eb = b + (size_t)i * LED_ENTRY_SIZE;
          const unsigned char *keep;

          if (memcmp(ea, eb, LED_ENTRY_SIZE) == 0)
               keep = ea;                      /* agreed: rbp held still */
          else
               keep = led_find(prev, prev_count, le32(ea + 0), le32(ea + 4));
          if (!keep)
               continue;                       /* in flux, and no history */
          memcpy(out + (size_t)n * LED_ENTRY_SIZE, keep, LED_ENTRY_SIZE);
          n++;
     }
     return n;
}

int led_blink_on(unsigned long long now_ms, unsigned int period_ms,
                 unsigned int fallback_ms)
{
     unsigned long long p = period_ms ? period_ms : fallback_ms;

     if (p < 2)
          return 1;
     return (now_ms % p) < (p / 2);
}
