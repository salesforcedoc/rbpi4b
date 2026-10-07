/*
 * plinn_scan.c -- the PlayerInnards scan's decision and its arithmetic.
 *
 * Pure: a struct in, a verdict out; no address of rbp's, no I/O, no globals, no
 * clock. See plinn_scan.h for why the decision is not left inside the shim.
 */
#include <stdio.h>

#include "plinn_scan.h"

int plinn_classify(const struct plinn_cand *c, enum plinn_refuse *why)
{
     int deck;

     if (!c) {
          if (why)
               *why = PLINN_BAD_CHAN;
          return -1;
     }

     /*
      * THE CHANNEL BYTE IS 1-BASED, and this is the test that shipped wrong.
      * The old spelling was `(chan != 2 && chan != 3)` with `d = chan - 2`, and
      * the cost is measured: deck 1's innards carries 1, so it was rejected
      * outright; deck 2's carries 2 and was filed as deck 1; g_plinn[1] stayed
      * NULL for the life of the process. The log said so plainly --
      * `deck1=0xcbaaa780 deck2=(nil)`, where 0xcbaaa780 is [player[1]+0x138],
      * i.e. DECK 2's innards sitting in deck 1's slot.
      *
      * A byte of 0 is not a third case to handle: it is the +0x26 word of some
      * other structure that happens to carry the vtable word too, which is
      * exactly the accident the rest of this function exists to catch.
      */
     if (c->chan != 1 && c->chan != 2) {
          if (why)
               *why = PLINN_BAD_CHAN;
          return -1;
     }
     deck = (int)c->chan - 1;

     if (c->mode > PLINN_MODE_MAX) {
          if (why)
               *why = PLINN_BAD_MODE;
          return -1;
     }

     if (c->eng < PLINN_ENGINE_MIN) {
          if (why)
               *why = PLINN_BAD_ENGINE;
          return -1;
     }

     if (why)
          *why = PLINN_OK;
     return deck;
}

const char *plinn_refuse_name(enum plinn_refuse why)
{
     switch (why) {
     case PLINN_BAD_CHAN:   return "chan";
     case PLINN_BAD_MODE:   return "mode";
     case PLINN_BAD_ENGINE: return "engine";
     case PLINN_OK:         break;
     case PLINN_REFUSE_COUNT: break;
     }
     return "?";
}

char *plinn_tally_line(char *buf, unsigned long n, const struct plinn_tally *t)
{
     int i, off;

     if (!buf || n == 0)
          return buf;
     if (!t) {
          snprintf(buf, (size_t)n, "(no tally)");
          return buf;
     }

     off = snprintf(buf, (size_t)n,
                    "%lu region(s), %lu word(s), %lu vtable hit(s); "
                    "seats deck1=%lu deck2=%lu",
                    t->regions, t->words, t->hits,
                    t->accepted[0], t->accepted[1]);
     if (off < 0)
          off = 0;
     if ((unsigned long)off >= n)
          return buf;                  /* truncated; done rather than unsafe */

     /* The refusals, spelled by name so a reason added later cannot silently
      * become a number nobody can look up. Every one is printed even when zero:
      * a missing reason and a refused-everything scan must not read alike.
      *
      * The walk starts at PLINN_BAD_CHAN, not at 0: slot 0 of `refused` is
      * PLINN_OK, which is not a refusal and has no name, so printing it puts a
      * `?=0` in every line -- noise that reads like a fourth reason and, worse,
      * invites a reader to take the four numbers as a partition of the hits when
      * only three of them are. The slots and the reasons are the same set on
      * purpose (so a caller indexes by the enum it was handed), which is exactly
      * why the first one has to be skipped here by name. */
     off += snprintf(buf + off, (size_t)(n - (unsigned long)off), "; refused");
     for (i = PLINN_BAD_CHAN; i < PLINN_REFUSE_COUNT && (unsigned long)off < n; i++)
          off += snprintf(buf + off, (size_t)(n - (unsigned long)off),
                          " %s=%lu", plinn_refuse_name((enum plinn_refuse)i),
                          t->refused[i]);
     return buf;
}
