/*
 * controllers.c -- the table itself, and its lookups.
 *
 * Pure: constants in, a row out; no address of rbp's, no I/O, no clock (the
 * keepalive's clock is an argument). See controllers.h for why the facts are
 * collected here at all.
 */
#define _GNU_SOURCE
#include "controllers.h"

#include <string.h>

/* ---- the FLX4's keepalive ----
 *
 * MEASURED BY SOMEONE ELSE, AND THAT IS SAID PLAINLY. These twelve bytes are the
 * sibling port's `SYSEX_FLX4_KEEPALIVE` (rx3-handoff/controllers.py, "reverse
 * engineered with Wireshark", citing Mixxx), and the 200 ms is theirs too. This
 * port has never sent one and is not obviously broken without it: LEDs, meters,
 * faders and pads are all measured working here (docs/15). The competing
 * explanation is that this port already keeps the link warm another way --
 * rbp_led.c re-sends the whole LED state every LED_RESEND_TICKS (100 ms) on the
 * FLX4's own port, where the sibling's bridge writes LEDs only on change and so
 * goes quiet between them. If that is the whole story this poll is redundant; if
 * the "keeps its audio path alive" half is real, it is not. It is a bench
 * measurement to take, not a fact to assume -- which is why it is one knob
 * (CTRL_KEEPALIVE) away from off and logs every send. */
static const unsigned char sysex_flx4_keepalive[] = {
     0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x05, 0x00, 0x50, 0x02, 0xF7
};

/* ---- the JP21's absolute-value query ----
 *
 * This port's OWN bytes, moved here verbatim from midi_io.c's
 * led_query_absolute(), which carried them inline. Engine OS sends this at
 * startup (`queryAbsoluteControls()` in JP21_Controller_Device.qml); without it
 * the panel's fader positions stay unknown until each is first moved, which is
 * what made the channel meters ignore the fader. */
static const unsigned char sysex_jp21_abs[] = {
     0xF0, 0x00, 0x02, 0x0B, 0x7F, 0x12, 0x04, 0x00, 0x00, 0xF7
};

/*
 * THE TABLE.
 *
 * Two rows, and the difference between them is the whole point: the FLX4 is the
 * surface this port has measured end to end, so every token is filled in; the
 * JP21 is the surface this port was built from but no longer runs, so its USB id,
 * its card id and its port-name hint are NULL -- UNKNOWN, and printed as unknown
 * -- rather than guessed. A guessed token is exactly the drift this table exists
 * to remove, and a wrong card id would send audioshim looking for a device that
 * cannot exist.
 *
 * Rows are ordered newest-target-first. Nothing indexes this array, so the order
 * is for the reader (and for the CLI's `names`).
 */
static const struct controller table[] = {
     {
          "flx4",
          "DDJ-FLX4",
          "2b73:0045",
          "FLX4",
          "DDJFLX4",
          "flx4",
          NULL, 0,                                  /* no SysEx of its own */
          sysex_flx4_keepalive, sizeof sysex_flx4_keepalive, 200,
          0, NULL,                                  /* a surface, not a mixer */
     },
     {
          "jp21",
          "SC Live 4 / Prime GO",
          NULL,                                     /* USB id not recorded here */
          NULL,                                     /* not named in the sequencer
                                                     * list; reached on rawmidi,
                                                     * which is why nothing
                                                     * matches it by name */
          NULL,                                     /* card id not recorded here */
          "jp21",
          sysex_jp21_abs, sizeof sysex_jp21_abs,
          NULL, 0, 0,                               /* wants no keepalive */
          0, NULL,                                  /* a surface, not a mixer */
     },

     /* ---- external digital mixers ------------------------------------------------
      *
      * NOT control surfaces, and the row says so by having no map: these are the devices
      * RB_MIXER_MODE=external hands the mixing to. With one on the bus the mixer-routing
      * setting applies and the page offers it; with none of them it does not. See
      * usb_devices.h for the asking and docs/09-audio.md for what turns on.
      *
      * HOW MUCH OF EACH ROW IS A MEASUREMENT IS SAID PER ROW, because the three kinds of
      * fact here are not interchangeable:
      *
      *   euphonia     id AND product string BOTH measured on .239 2026-10-10
      *                (`2b73:0047`, product "euphonia", ALSA card "euphonia").
      *   DJM-V10      id from the kernel's own sound/usb/quirks-table.h -- a device the
      *   DJM-A9       kernel already carries a quirk for, which is a citable source and
      *   DJM-900NXS2  not a guess. The model name is the product token.
      *   DJM-V5       id NOT recorded anywhere this tree could check, so it is NULL --
      *   DJM-900NXS   unknown, printed as unknown -- and the row is reached by its model
      *                name alone. The V5 is a 2026 product with no kernel quirk yet, and a
      *                GUESSED id here is exactly the drift this table exists to remove.
      *
      * The 900NXS family token is last of the family: "djm900nxs" is a substring of
      * "djm900nxs2", so first-match-wins order is what keeps the specific row's name on a
      * 900NXS2. */
     {
          "euphonia",
          "AlphaTheta euphonia",
          "2b73:0047",                              /* measured on .239 */
          NULL,                                     /* not reached by a sequencer name */
          "euphonia",                               /* and its card id, also measured */
          NULL,                                     /* no map: not a surface */
          NULL, 0,                                  /* no SysEx of its own */
          NULL, 0, 0,                               /* wants no keepalive */
          1, "euphonia",
     },
     {
          "djm900nxs2",
          "DJM-900NXS2",
          "2b73:000a",                              /* kernel quirks-table.h */
          NULL, NULL, NULL,
          NULL, 0, NULL, 0, 0,
          1, "DJM-900NXS2",
     },
     {
          "djmv10",
          "DJM-V10",
          "2b73:0034",                              /* kernel quirks-table.h */
          NULL, NULL, NULL,
          NULL, 0, NULL, 0, 0,
          1, "DJM-V10",
     },
     {
          "djma9",
          "DJM-A9",
          "2b73:003c",                              /* kernel quirks-table.h */
          NULL, NULL, NULL,
          NULL, 0, NULL, 0, 0,
          1, "DJM-A9",
     },
     {
          "djmv5",
          "DJM-V5",
          NULL,                                     /* id not known here: see above */
          NULL, NULL, NULL,
          NULL, 0, NULL, 0, 0,
          1, "DJM-V5",
     },
     {
          "djm900nxs",
          "DJM-900NXS",
          NULL,                                     /* id not known here */
          NULL, NULL, NULL,
          NULL, 0, NULL, 0, 0,
          1, "DJM-900NXS",                          /* family token: must stay last */
     },
};

unsigned controllers_count(void)
{
     return (unsigned)(sizeof table / sizeof table[0]);
}

const struct controller *controllers_at(unsigned i)
{
     if (i >= controllers_count())
          return NULL;
     return &table[i];
}

const struct controller *controllers_find(const char *id)
{
     unsigned i;

     if (!id || !id[0])
          return NULL;
     for (i = 0; i < controllers_count(); i++)
          if (strcmp(table[i].id, id) == 0)
               return &table[i];
     return NULL;
}

const struct controller *controllers_by_hint(const char *port_name)
{
     unsigned i;

     if (!port_name)
          return NULL;
     for (i = 0; i < controllers_count(); i++) {
          const char *hint = table[i].alsa_hint;

          /* An empty needle matches EVERY haystack under strcasestr(), so a
           * hint-less row would otherwise be returned for the first port in the
           * list. midi_io.c's name_has() refuses the same two cases for the same
           * reason -- this is the same rule, not a second one.
           *
           * The NULL half is not decoration: strcasestr() with a NULL needle
           * dereferences it. Removing this guard does not return the wrong row,
           * it takes the CALLER down -- inside the reader thread, on the unit,
           * where the symptom is a shim that stops looking for its surface. */
          if (!hint || !hint[0])
               continue;
          if (strcasestr(port_name, hint) != NULL)
               return &table[i];
     }
     return NULL;
}

const struct controller *controllers_by_usb(const char *usb)
{
     unsigned i;

     if (!usb || !usb[0])
          return NULL;
     for (i = 0; i < controllers_count(); i++) {
          /* A row that does not record its USB id must not be reachable by one,
           * and `usb` is never empty here, so NULL simply never matches. */
          if (table[i].usb && strcmp(table[i].usb, usb) == 0)
               return &table[i];
     }
     return NULL;
}

const struct controller *controllers_default(void)
{
     const struct controller *c = controllers_find(CONTROLLERS_DEFAULT_ID);

     /* Never NULL in a build that passes its own tests: test_controllers.c pins
      * that CONTROLLERS_DEFAULT_ID names a row in this table, so this fallback
      * is unreachable rather than a silent second default. It exists so that no
      * caller has to carry a NULL arm for a lookup that cannot fail; the row is
      * the first one, which the same test pins, so even a hypothetical miss
      * lands on the surface this port targets. */
     return c ? c : &table[0];
}

/* One spelling rule for a model name, and the only place it is applied. See the
 * declaration in controllers.h for why punctuation is DROPPED rather than turned into a
 * separator: "DJM-900NXS2", "DJM900NXS2" and "djm-900nxs2" all have to reach one token,
 * and a rule that keeps the dash cannot reach all three. Written out rather than using
 * tolower(), so it does not depend on the locale of the unit's libc. */
static void normalize_into(const char *s, char *out, size_t outsz)
{
     size_t o = 0;

     if (!s)
          s = "";
     for (; *s && o + 1 < outsz; s++) {
          unsigned char c = (unsigned char)*s;

          if (c >= 'A' && c <= 'Z')
               c = (unsigned char)(c - 'A' + 'a');
          if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
               out[o++] = (char)c;
     }
     out[o] = '\0';
}

int controller_is_mixer(const struct controller *c)
{
     return c != NULL && c->mixer != 0;
}

const struct controller *controllers_mixer_by_usb(const char *usb)
{
     unsigned i;

     if (!usb || !usb[0])
          return NULL;
     for (i = 0; i < controllers_count(); i++)
          /* A row that does not record its id is not reachable by one -- NULL never
           * matches -- and `mixer` is what keeps a surface out of this lookup. */
          if (table[i].mixer && table[i].usb && strcmp(table[i].usb, usb) == 0)
               return &table[i];
     return NULL;
}

const struct controller *controllers_mixer_by_product(const char *text)
{
     char hay[128];
     unsigned i;

     if (!text || !text[0])
          return NULL;
     normalize_into(text, hay, sizeof hay);
     if (!hay[0])
          return NULL;
     for (i = 0; i < controllers_count(); i++) {
          char needle[128];

          if (!table[i].mixer || !table[i].product || !table[i].product[0])
               continue;
          normalize_into(table[i].product, needle, sizeof needle);
          /* Both halves of the guard above are load-bearing, the rule
           * controllers_by_hint states: an empty needle matches every haystack. */
          if (needle[0] && strstr(hay, needle) != NULL)
               return &table[i];
     }
     return NULL;
}

int controller_keepalive_due(const struct controller *c,
                             unsigned long long now_ms,
                             unsigned long long last_ms)
{
     unsigned long long elapsed;

     if (!c || !c->keepalive || c->keepalive_len == 0 || c->keepalive_ms == 0)
          return 0;

     /* The subtraction is unsigned, and that is what makes a WRAPPED clock safe
      * rather than a case to handle: a `now_ms` below `last_ms` underflows to a
      * value near 2^64, which is "due". So a clock that runs backwards costs one
      * early keepalive and never a silence.
      *
      * Written out because it is not obvious, and a mutation test cannot tell
      * this apart from an explicit `if (now_ms < last_ms) return 1;` -- the two
      * have the same behaviour by construction, so the arithmetic is the whole
      * mechanism and there is no second branch to keep in step. What WOULD
      * change it is a signed type or a division, which is why both ends are
      * `unsigned long long` milliseconds and the tests pin the wrap on both
      * sides of the 32-bit boundary. */
     elapsed = now_ms - last_ms;
     return elapsed >= (unsigned long long)c->keepalive_ms;
}
