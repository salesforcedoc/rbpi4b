/*
 * test_kbd.c — the keyboard fallback, with no keyboard.
 *
 * The controls port has two event sources, and the other tests only take the
 * first one. test_midi.c replays a sequencer fixture through the real JP21 map;
 * this drives synthetic evdev triples through the real keyboard map (map_kbd.o,
 * ctrl_map.o, shimutil.o are the objects that ship, not copies) and pins the
 * rbp keycodes that come out. What is being checked, in the order it matters:
 *
 *   1. The map is a non-MIDI surface. kbd_devices() says so, there is an input()
 *      and no event(), and its build() leaves the shared binding tables exactly
 *      as it found them — a keyboard has no note and no CC. That last one used to
 *      read "build() empties them"; since 2026-09-26 the front end owns the reset
 *      (ctrl_map.h), because a keyboard and an FLX4 are built into the same two
 *      tables and a build that cleared them would leave the controller with no
 *      bindings at all. main() is where that is asserted, with the planted rows.
 *
 *   2. Every binding reaches the keycode its table row says it should. This is
 *      the assertion with teeth: the deck channels (2 as well as 1), LOAD on the
 *      deck's channel rather than CH_GLOBAL, the selector's rotation direction
 *      and its Enter push, BACK and SOURCE, and the mouse's right button.
 *
 *   3. The edge behaviour, which is where a map like this actually goes wrong:
 *      one press per press however many autorepeats arrive, a release that was
 *      never announced, a step that repeats on autorepeat where a button does
 *      not, a wheel that arrives with several notches at once, and a wheel that
 *      arrives with a dozen (the clamp).
 *
 * What is NOT checked here, and cannot be:
 *
 *   - evdev_io.c. Opening /dev/input/event* needs a device; it is deliberately
 *     not linked into THIS test, so nothing here claims it reads events
 *     correctly. What is checked here is the contract on the other side of it:
 *     that this map knows what to do with a raw triple. The reader's own
 *     behaviour — which nodes it opens, when it notices a new one, and what it
 *     does when one goes away — is test_evdev's, which fakes the syscall
 *     boundary rather than the module.
 *   - The sign of the selector rotation against rbp's list. map_kbd.c sends the
 *     same +1/-1 that map_jp21.c's knob sends for a clockwise turn, and that is
 *     asserted here; whether rbp's list scrolls up or down for it had never been
 *     observed, and the operator's correction of 2026-09-26 (the pair worked
 *     backwards) is what the assertion now encodes. See the note under the
 *     binding table in map_kbd.c and docs/16-input-and-hotplug.md.
 *
 * One side effect worth knowing about, the same one test_midi.c has: klog() is
 * unconditional, so running this appends a few lines to /tmp/knobshim.log.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include "rbp_abi.h"       /* the K_..., OP_... and CH_... names the map speaks */
#include "rbp_bridge.h"    /* the keycode calls, and the signatures to match */
#include "ctrl_map.h"

/* ==========================================================================
 * The kernel's input codes, pinned a second time.
 *
 * These are NOT taken from map_kbd.c — they are #defines in a .c file, so this
 * test physically cannot see them — and that is the point: the two copies agree
 * only if both are right. A typo in map_kbd.c's copy makes a key do nothing at
 * runtime and fails a row below, rather than passing because both files shared
 * one wrong constant.
 *
 * The values are the Linux input ABI's, spelled from the same knowledge as the
 * map's copy: KEY_SPACE is 57, not 32, and BTN_RIGHT is 0x111, which is why
 * they are worth pinning rather than remembering.
 * ========================================================================== */

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02

#define KEY_ESC       1
#define KEY_1         2
#define KEY_2         3
#define KEY_5         6
#define KEY_6         7
#define KEY_7         8
#define KEY_8         9
#define KEY_9         10
#define KEY_0         11
#define KEY_BACKSPACE 14
#define KEY_W         17
#define KEY_ENTER     28
#define KEY_S         31
#define KEY_Z         44
#define KEY_X         45
#define KEY_N         49
#define KEY_M         50
#define KEY_COMMA     51
#define KEY_SPACE     57
#define KEY_UP        103
#define KEY_LEFT      105      /* unmapped, on purpose: a key rbp has no use for */
#define KEY_DOWN      108

#define BTN_RIGHT     0x111
#define REL_WHEEL     0x08
#define REL_X         0x00    /* the pointer's own movement: not this map's */

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* ==========================================================================
 * The output of the map, recorded rather than sent.
 *
 * Only send_rx_key() is defined. map_kbd.c calls nothing else — the keyboard
 * has no absolute control, so it has no value or float to send — and leaving
 * the _f/_fl forms out means a binding that ever grew one would fail to link
 * here instead of quietly sending a keycode nothing checks.
 * ========================================================================== */

struct sent {
     int key, op, ch;
     long param;
};

/* 64 is comfortably more than any one gesture: the largest is the clamped wheel
 * below, at 16. */
#define SENT_MAX 64
static struct sent sent[SENT_MAX];
static int sent_n;

void send_rx_key(int keycode, int op, int ch, long param)
{
     if (sent_n >= SENT_MAX) {
          printf("FAIL: more than %d keycodes without a reset\n", SENT_MAX);
          exit(1);
     }
     sent[sent_n].key = keycode;
     sent[sent_n].op = op;
     sent[sent_n].ch = ch;
     sent[sent_n].param = param;
     sent_n++;
}

/* rbp. A non-NULL sentinel: the front end's gate only asks whether the
 * KeyManager exists, and the map must be reached. */
void *get_key_manager(void)
{
     return (void *)&checks;
}

int verbose = 0;          /* every klog() in the map is gated on this */

/* ==========================================================================
 * The gestures, and what each must produce.
 *
 * One row per event exactly as the kernel would deliver it, with the keycodes
 * that event must produce (often none). Rows are ordered, so the held-state
 * behaviour is exercised by the sequence and not by a special case: a press, its
 * autorepeats, a second press, the release, and a release with no press are five
 * consecutive rows for one key.
 *
 *   `n` is how many keycodes the row must produce, and `w[]` is what they must
 *   be, in order. n == 0 is the assertion that a triple produces nothing at all,
 *   which is half of what this table is for.
 * ========================================================================== */

struct want {
     int key, op, ch;
     long param;
};

#define W(k, o, c, p) { (k), (o), (c), (p) }

#define W_MAX 4

struct step {
     const char *what;
     int type, code, value;
     int n;
     struct want w[W_MAX];
};

static const struct step steps[] = {
     /* ---- deck 1: space / z / x ------------------------------------------ */
     { "deck 1 PLAY down",     EV_KEY, KEY_SPACE, 1, 1, { W(K_PLAY, OP_PRESS, 1, 0) } },
     { "deck 1 PLAY repeat",   EV_KEY, KEY_SPACE, 2, 0, { {0,0,0,0} } },
     { "deck 1 PLAY repeat",   EV_KEY, KEY_SPACE, 2, 0, { {0,0,0,0} } },
     { "deck 1 PLAY down again (already down)", EV_KEY, KEY_SPACE, 1, 0, { {0,0,0,0} } },
     { "deck 1 PLAY up",       EV_KEY, KEY_SPACE, 0, 1, { W(K_PLAY, OP_RELEASE, 1, 0) } },
     { "deck 1 PLAY up again", EV_KEY, KEY_SPACE, 0, 0, { {0,0,0,0} } },
     { "deck 1 PLAY down again (after a release)", EV_KEY, KEY_SPACE, 1, 1, { W(K_PLAY, OP_PRESS, 1, 0) } },
     { "deck 1 PLAY up",       EV_KEY, KEY_SPACE, 0, 1, { W(K_PLAY, OP_RELEASE, 1, 0) } },

     { "deck 1 CUE down",      EV_KEY, KEY_Z, 1, 1, { W(K_CUE, OP_PRESS, 1, 0) } },
     { "deck 1 CUE up",        EV_KEY, KEY_Z, 0, 1, { W(K_CUE, OP_RELEASE, 1, 0) } },
     { "deck 1 SYNC down",     EV_KEY, KEY_X, 1, 1, { W(K_SYNC, OP_PRESS, 1, 0) } },
     { "deck 1 SYNC up",       EV_KEY, KEY_X, 0, 1, { W(K_SYNC, OP_RELEASE, 1, 0) } },

     /* ---- deck 2: n / m / , -- the same keycodes on the other channel ---- */
     { "deck 2 PLAY down",     EV_KEY, KEY_N, 1, 1, { W(K_PLAY, OP_PRESS, 2, 0) } },
     { "deck 2 PLAY up",       EV_KEY, KEY_N, 0, 1, { W(K_PLAY, OP_RELEASE, 2, 0) } },
     { "deck 2 CUE down",      EV_KEY, KEY_M, 1, 1, { W(K_CUE, OP_PRESS, 2, 0) } },
     { "deck 2 CUE up",        EV_KEY, KEY_M, 0, 1, { W(K_CUE, OP_RELEASE, 2, 0) } },
     { "deck 2 SYNC down",     EV_KEY, KEY_COMMA, 1, 1, { W(K_SYNC, OP_PRESS, 2, 0) } },
     { "deck 2 SYNC up",       EV_KEY, KEY_COMMA, 0, 1, { W(K_SYNC, OP_RELEASE, 2, 0) } },

     /* ---- w / s: PLAY on deck 1 and deck 2, additive to space/n ---------- */
     { "W deck 1 PLAY down",   EV_KEY, KEY_W, 1, 1, { W(K_PLAY, OP_PRESS, 1, 0) } },
     { "W deck 1 PLAY up",     EV_KEY, KEY_W, 0, 1, { W(K_PLAY, OP_RELEASE, 1, 0) } },
     { "S deck 2 PLAY down",   EV_KEY, KEY_S, 1, 1, { W(K_PLAY, OP_PRESS, 2, 0) } },
     { "S deck 2 PLAY up",     EV_KEY, KEY_S, 0, 1, { W(K_PLAY, OP_RELEASE, 2, 0) } },

     /* The two decks are separate holds: deck 2's PLAY does not touch deck 1's
      * latch, and both may be down at once. */
     { "deck 1 PLAY down",     EV_KEY, KEY_SPACE, 1, 1, { W(K_PLAY, OP_PRESS, 1, 0) } },
     { "deck 2 PLAY down",     EV_KEY, KEY_N, 1, 1, { W(K_PLAY, OP_PRESS, 2, 0) } },
     { "deck 1 PLAY up",       EV_KEY, KEY_SPACE, 0, 1, { W(K_PLAY, OP_RELEASE, 1, 0) } },
     { "deck 2 PLAY up",       EV_KEY, KEY_N, 0, 1, { W(K_PLAY, OP_RELEASE, 2, 0) } },

     /* ---- LOAD: rbp's global keycode, the deck in the channel ------------ */
     { "LOAD deck 1 down",     EV_KEY, KEY_1, 1, 1, { W(K_LOAD, OP_PRESS, 1, 0) } },
     { "LOAD deck 1 up",       EV_KEY, KEY_1, 0, 1, { W(K_LOAD, OP_RELEASE, 1, 0) } },
     { "LOAD deck 2 down",     EV_KEY, KEY_2, 1, 1, { W(K_LOAD, OP_PRESS, 2, 0) } },
     { "LOAD deck 2 up",       EV_KEY, KEY_2, 0, 1, { W(K_LOAD, OP_RELEASE, 2, 0) } },

     /* ---- the rest of the digit row: rbp's browse-screen keys, all global --- */
     { "SOURCE down",          EV_KEY, KEY_5, 1, 1, { W(K_SOURCE, OP_PRESS, CH_GLOBAL, 0) } },
     { "SOURCE repeat",        EV_KEY, KEY_5, 2, 0, { {0,0,0,0} } },
     { "SOURCE up",            EV_KEY, KEY_5, 0, 1, { W(K_SOURCE, OP_RELEASE, CH_GLOBAL, 0) } },
     { "SOURCE up again",      EV_KEY, KEY_5, 0, 0, { {0,0,0,0} } },
     { "BROWSE down",          EV_KEY, KEY_6, 1, 1, { W(K_BROWSE, OP_PRESS, CH_GLOBAL, 0) } },
     { "BROWSE up",            EV_KEY, KEY_6, 0, 1, { W(K_BROWSE, OP_RELEASE, CH_GLOBAL, 0) } },
     { "TAG LIST down",        EV_KEY, KEY_7, 1, 1, { W(K_TAGLIST, OP_PRESS, CH_GLOBAL, 0) } },
     { "TAG LIST up",          EV_KEY, KEY_7, 0, 1, { W(K_TAGLIST, OP_RELEASE, CH_GLOBAL, 0) } },
     { "MENU down",            EV_KEY, KEY_0, 1, 1, { W(K_MENU, OP_PRESS, CH_GLOBAL, 0) } },
     { "MENU up",              EV_KEY, KEY_0, 0, 1, { W(K_MENU, OP_RELEASE, CH_GLOBAL, 0) } },

     /* ---- PLAYLIST and SEARCH: declared, but with no rbp keycode, so the two
      * must produce nothing at all. That is the assertion worth having -- a row
      * that sends keycode 0 would be a keycode rbp never asked for, and these
      * rows are what stop a later "helpful" edit from making it do that. ---- */
     { "PLAYLIST down (no rbp keycode yet)", EV_KEY, KEY_8, 1, 0, { {0,0,0,0} } },
     { "PLAYLIST up (no rbp keycode yet)",   EV_KEY, KEY_8, 0, 0, { {0,0,0,0} } },
     { "SEARCH down (no rbp keycode yet)",   EV_KEY, KEY_9, 1, 0, { {0,0,0,0} } },
     { "SEARCH up (no rbp keycode yet)",     EV_KEY, KEY_9, 0, 0, { {0,0,0,0} } },

     /* ---- the selector: the arrows and the wheel rotate, Enter pushes ----
      *
      * The signs: UP is -1 and DOWN is +1 as of 2026-09-26. They were the other
      * way round and the operator reported the pair working backwards, which is
      * the only observation of the sign there has been -- it cannot be measured
      * on the SOURCE panel, which has nothing to move between. The WHEEL is
      * still +1, i.e. it now agrees with DOWN ("away from the user" = "down the
      * list"), and this assertion is what will catch a later well-meaning flip
      * of one and not the other. docs/16-input-and-hotplug.md. ---- */
     { "selector up",          EV_KEY, KEY_UP, 1, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1) } },
     { "selector up (autorepeat repeats the step)", EV_KEY, KEY_UP, 2, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1) } },
     { "selector up release (a step holds nothing)", EV_KEY, KEY_UP, 0, 0, { {0,0,0,0} } },
     { "selector down",        EV_KEY, KEY_DOWN, 1, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1) } },
     { "selector down (autorepeat)", EV_KEY, KEY_DOWN, 2, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1) } },
     { "selector down again",  EV_KEY, KEY_DOWN, 1, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1) } },
     { "selector down release", EV_KEY, KEY_DOWN, 0, 0, { {0,0,0,0} } },

     { "selector push down",   EV_KEY, KEY_ENTER, 1, 1, { W(K_SELECTOR, OP_PRESS, CH_GLOBAL, 0) } },
     { "selector push repeat", EV_KEY, KEY_ENTER, 2, 0, { {0,0,0,0} } },
     { "selector push up",     EV_KEY, KEY_ENTER, 0, 1, { W(K_SELECTOR, OP_RELEASE, CH_GLOBAL, 0) } },
     { "selector push up again", EV_KEY, KEY_ENTER, 0, 0, { {0,0,0,0} } },

     /* ---- the wheel: the same control as the arrows ---------------------- */
     { "wheel away from the user (one notch)", EV_REL, REL_WHEEL, +1, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1) } },
     { "wheel toward the user (one notch)", EV_REL, REL_WHEEL, -1, 1, { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1) } },
     { "wheel, three notches at once", EV_REL, REL_WHEEL, +3, 3,
       { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1),
         W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1),
         W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, +1) } },
     { "wheel, three notches the other way", EV_REL, REL_WHEEL, -3, 3,
       { W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1),
         W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1),
         W(K_SELECTOR, OP_ROTATE, CH_GLOBAL, -1) } },
     /* A wheel that reports no movement is not a movement: no keycode, and no
      * division of zero notches into anything. */
     { "wheel, no notches",    EV_REL, REL_WHEEL, 0, 0, { {0,0,0,0} } },
     /* The clamp on a runaway wheel is checked on its own, below: sixteen
      * identical keycodes is a row nobody wants to read. */

     /* ---- BACK and SOURCE, on the keyboard and on the mouse ------------- */
     { "BACKSPACE down",       EV_KEY, KEY_BACKSPACE, 1, 1, { W(K_BACK, OP_PRESS, CH_GLOBAL, 0) } },
     { "BACKSPACE up",         EV_KEY, KEY_BACKSPACE, 0, 1, { W(K_BACK, OP_RELEASE, CH_GLOBAL, 0) } },
     { "ESC down",             EV_KEY, KEY_ESC, 1, 1, { W(K_SOURCE, OP_PRESS, CH_GLOBAL, 0) } },
     { "ESC up",               EV_KEY, KEY_ESC, 0, 1, { W(K_SOURCE, OP_RELEASE, CH_GLOBAL, 0) } },
     { "mouse right button down", EV_KEY, BTN_RIGHT, 1, 1, { W(K_BACK, OP_PRESS, CH_GLOBAL, 0) } },
     { "mouse right button repeat", EV_KEY, BTN_RIGHT, 2, 0, { {0,0,0,0} } },
     { "mouse right button up", EV_KEY, BTN_RIGHT, 0, 1, { W(K_BACK, OP_RELEASE, CH_GLOBAL, 0) } },
     /* The right button and Backspace are the same rbp keycode on the same
      * channel, so the two sources are interchangeable — which is the whole
      * reason a mouse is enough to get out of a menu. */
     { "mouse right button down", EV_KEY, BTN_RIGHT, 1, 1, { W(K_BACK, OP_PRESS, CH_GLOBAL, 0) } },
     { "BACKSPACE down while the button is held (its own latch)", EV_KEY, KEY_BACKSPACE, 1, 1, { W(K_BACK, OP_PRESS, CH_GLOBAL, 0) } },
     { "BACKSPACE up",         EV_KEY, KEY_BACKSPACE, 0, 1, { W(K_BACK, OP_RELEASE, CH_GLOBAL, 0) } },
     { "mouse right button up", EV_KEY, BTN_RIGHT, 0, 1, { W(K_BACK, OP_RELEASE, CH_GLOBAL, 0) } },

     /* ---- what must produce nothing at all ------------------------------ */
     { "SYN_REPORT (a packet boundary, not an event)", EV_SYN, 0, 0, 0, { {0,0,0,0} } },
     { "the pointer's own X movement", EV_REL, REL_X, 1, 0, { {0,0,0,0} } },
     { "a key with no binding", EV_KEY, KEY_LEFT, 1, 0, { {0,0,0,0} } },
     { "a key with no binding, released", EV_KEY, KEY_LEFT, 0, 0, { {0,0,0,0} } },
     /* A code the map does not carry, on a type it does: the filter is a pair,
      * so EV_REL carrying a key code is nothing.
      *
      * The EV_KEY half has to be REL_X and not a wheel code, because REL_WHEEL
      * is 8 and KEY_7 is 8 -- the same number in two namespaces. That is not a
      * bug to fix but a coexistence to pin, and the rows above already do: the
      * TAG LIST rows drive EV_KEY code 8 to K_TAGLIST while the wheel rows drive
      * EV_REL code 8 to a selector rotation. Together they are the assertion
      * that `type` is what tells the two apart, which is why this row cannot
      * make the same point with the same number. */
     { "EV_REL carrying a key code", EV_REL, KEY_SPACE, 1, 0, { {0,0,0,0} } },
     { "EV_KEY carrying the pointer's X code", EV_KEY, REL_X, 1, 0, { {0,0,0,0} } },
};

#define NSTEPS ((int)(sizeof(steps) / sizeof(steps[0])))

/* ==========================================================================
 * The checks
 * ========================================================================== */

/* ctrlshim.c's ctrl_input_dispatch(), which is the whole of the path a raw
 * triple takes: the KeyManager gate, then the map. */
static void dispatch(int type, int code, int value)
{
     if (get_key_manager() && map_kbd.input)
          map_kbd.input(type, code, value);
}

/* The keycodes the map's table names, pinned as numbers. rbp_abi.h is
 * authoritative for these and is not what this test is about — but a fat-finger
 * in one of these hex literals is a silent "the button does nothing", and this
 * is the only test that would notice. */
static void pin_keycodes(void)
{
     CHECK(K_PLAY     == 0x4101, "K_PLAY is 0x%04x, not 0x4101", K_PLAY);
     CHECK(K_CUE      == 0x4102, "K_CUE is 0x%04x, not 0x4102", K_CUE);
     CHECK(K_SYNC     == 0x4112, "K_SYNC is 0x%04x, not 0x4112", K_SYNC);
     CHECK(K_LOAD     == 0x4311, "K_LOAD is 0x%04x, not 0x4311", K_LOAD);
     CHECK(K_SELECTOR == 0x420c, "K_SELECTOR is 0x%04x, not 0x420c", K_SELECTOR);
     CHECK(K_BACK     == 0x420d, "K_BACK is 0x%04x, not 0x420d", K_BACK);
     CHECK(K_SOURCE   == 0x0201, "K_SOURCE is 0x%04x, not 0x0201", K_SOURCE);
     CHECK(K_BROWSE   == 0x0202, "K_BROWSE is 0x%04x, not 0x0202", K_BROWSE);
     CHECK(K_TAGLIST  == 0x0203, "K_TAGLIST is 0x%04x, not 0x0203", K_TAGLIST);
     CHECK(K_MENU     == 0x0206, "K_MENU is 0x%04x, not 0x0206", K_MENU);

     /* The ops and the channel are as load-bearing as the keycodes: OP_ROTATE
      * with OP_PRESS's number would push the selector once per notch. */
     CHECK(OP_PRESS == 0 && OP_RELEASE == 2 && OP_ROTATE == 4,
           "the ops are %d/%d/%d, not 0/2/4", OP_PRESS, OP_RELEASE, OP_ROTATE);
     CHECK(CH_GLOBAL == 1, "CH_GLOBAL is %d, not 1", CH_GLOBAL);
}

/* Every gesture in the table, in order. */
static void drive(void)
{
     int i, j, total = 0;

     for (i = 0; i < NSTEPS; i++) {
          const struct step *s = &steps[i];

          /* A row that expected more keycodes than its `w` holds would compare
           * against whatever follows it in memory. Fail loudly instead. */
          CHECK(s->n <= W_MAX, "%s expects %d keycodes; the table holds %d",
                s->what, s->n, W_MAX);

          sent_n = 0;
          dispatch(s->type, s->code, s->value);

          CHECK(sent_n == s->n,
                "%s: produced %d keycodes, expected %d", s->what, sent_n, s->n);
          for (j = 0; j < s->n && j < sent_n && j < W_MAX; j++) {
               const struct sent *g = &sent[j];
               const struct want *w = &s->w[j];

               CHECK(g->key == w->key && g->op == w->op && g->ch == w->ch,
                     "%s: keycode %d is 0x%04x op=%d ch=%d, expected "
                     "0x%04x op=%d ch=%d",
                     s->what, j, g->key, g->op, g->ch, w->key, w->op, w->ch);
               CHECK(g->param == w->param,
                     "%s: keycode %d (0x%04x) has param %ld, expected %ld",
                     s->what, j, w->key, g->param, w->param);
          }
          total += s->n;
     }

     /* A table that asserted nothing would pass every row above. */
     CHECK(total > 0, "the gesture table expects no keycodes at all");
}

/* The wheel's clamp, which the gesture table cannot express: sixteen identical
 * keycodes is a row nobody would read, and what matters is the count and the
 * direction rather than the sixteenth copy. A mouse whose driver reports a
 * dozen notches in one event — or a hundred — must become at most sixteen
 * rotations, because each one is a keycode into rbp. */
static void check_clamp(void)
{
     static const struct { int notches, dir; } t[] = {
          { +20, +1 }, { -20, -1 }, { +100, +1 }, { -100, -1 },
     };
     unsigned i;
     int j;

     for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
          sent_n = 0;
          dispatch(EV_REL, REL_WHEEL, t[i].notches);

          CHECK(sent_n == 16,
                "a %d-notch wheel produced %d keycodes, expected the clamp of 16",
                t[i].notches, sent_n);
          for (j = 0; j < sent_n; j++)
               CHECK(sent[j].key == K_SELECTOR && sent[j].op == OP_ROTATE &&
                     sent[j].ch == CH_GLOBAL && sent[j].param == t[i].dir,
                     "a %d-notch wheel: keycode %d is 0x%04x op=%d ch=%d "
                     "param=%ld, expected 0x%04x op=%d ch=%d param=%d",
                     t[i].notches, j, sent[j].key, sent[j].op, sent[j].ch,
                     sent[j].param, K_SELECTOR, OP_ROTATE, CH_GLOBAL, t[i].dir);
     }
}

/* What the map is, as opposed to what it sends. */
static void check_surface(void)
{
     /* The one question the front end asks (ctrlshim.c's midi_thread): a map
      * with non-MIDI sources gets the evdev reader started, and 0 leaves it
      * unstarted. Anything > 0 would do, but a map that answered 0 here would
      * build, link, and never receive a single event. */
     CHECK(map_kbd.devices != NULL && map_kbd.devices() > 0,
           "the keyboard map says it wants %d non-MIDI sources",
           map_kbd.devices ? map_kbd.devices() : 0);
     CHECK(map_kbd.input != NULL, "the keyboard map has no input() hook");

     /* A keyboard is not a MIDI device: no sequencer event, and no heartbeat.
      * Both are optional in struct ctrl_map, and both being NULL is what makes
      * the reader the map's only source -- which is also why this map can sit
      * beside a live FLX4 without consuming its events. */
     CHECK(map_kbd.event == NULL, "the keyboard map has a sequencer handler");
     CHECK(map_kbd.tick == NULL, "the keyboard map has a tick");

     CHECK(map_kbd.name != NULL && map_kbd.name[0] != '\0',
           "the keyboard map has no name for MIDI_MAP to select it by");
}

int main(void)
{
     /* map_kbd.c reads no environment today, but evdev_io.c reads KBD_DEV and
      * `make test` inherits the shell. Unset it so nothing here depends on the
      * developer's environment, and so the day a knob is added to this map the
      * test is already strict. */
     unsetenv("KBD_DEV");
     unsetenv("KNOB_VERBOSE");
     unsetenv("MIDI_MAP");

     pin_keycodes();

     /* The ownership change of 2026-09-26, and the reason these assertions live
      * here at the top level rather than inside check_surface(): the shared
      * binding tables are reset by the FRONT END (ctrl_map.h), once, before it
      * builds either map, because two maps are now live at the same time -- a
      * MIDI surface and a non-MIDI one.
      *
      * The failure mode to pin is the one that change removed: if kbd_build()
      * ever calls ctrl_bindings_reset() again, it wipes the FLX4's rows and puts
      * nothing in their place, so every button on the controller goes dead from a
      * map that is working exactly as it was designed to. So plant bindings the
      * way a MIDI map would, build, and require them to still be there.
      *
      * Two of each, not one, so the surviving count cannot be read as "the map
      * added something of its own" -- this map adds nothing at all. */
     add_note(0, 36, K_PAD1, 1);
     add_note(0, 37, K_LOOPIN, 1);
     add_abs(0, 17, K_HPMIX, 1);
     add_abs(0, 18, K_HPLEVEL, 1);
     CHECK(note_map_n == 2 && abs_map_n == 2,
           "planting bindings left %d notes and %d absolute controls, expected 2 "
           "and 2", note_map_n, abs_map_n);

     map_kbd.build();
     check_surface();

     CHECK(note_map_n == 2 && abs_map_n == 2,
           "a keyboard build must ADD to the shared binding tables, not wipe "
           "them: it left %d notes and %d absolute controls, expected the 2 and 2 "
           "that were planted before it -- an FLX4 built second would have no "
           "bindings left", note_map_n, abs_map_n);

     /* And the front end's reset -- now the only thing that clears them -- does
      * clear them, both tables together. */
     ctrl_bindings_reset();
     CHECK(note_map_n == 0 && abs_map_n == 0,
           "ctrl_bindings_reset() left %d notes and %d absolute controls",
           note_map_n, abs_map_n);

     /* MIDI_MAP=none / EVDEV_MAP=none exist so that "no surface on this side" is
      * a selection someone can make deliberately, rather than the silent
      * fallback to a map the operator did not ask for. So it has to be
      * selectable: an inert surface, with a build() the front end can call like
      * any other. */
     CHECK(map_none.build != NULL, "map_none has no build()");
     CHECK(map_none.startup == NULL && map_none.event == NULL &&
           map_none.tick == NULL && map_none.devices == NULL &&
           map_none.input == NULL,
           "map_none is not inert");
     CHECK(map_none.name != NULL && map_none.name[0] != '\0',
           "map_none has no name for MIDI_MAP to select it by");
     map_none.build();
     CHECK(note_map_n == 0 && abs_map_n == 0,
           "map_none built %d notes and %d absolute controls into the shared "
           "tables", note_map_n, abs_map_n);

     /* Neither of the surfaces this link knows has a panel to light, and that has
      * to be a declared NULL rather than an omission. rbp_led.c asks the front
      * end for the selected surface's table and sends NOTHING when it gets NULL,
      * so a map that leaves this field out is a map that lights nothing -- and
      * the whole reason it is a field is that "nothing" must be distinguishable
      * from "the previous target's notes". Pinned here because this is the only
      * suite that links both of these maps. */
     CHECK(map_kbd.leds == NULL,
           "map_kbd declares a panel to light, and a keyboard has none");
     CHECK(map_none.leds == NULL,
           "map_none declares a panel to light, and it has no surface at all");

     drive();
     check_clamp();

     printf("test_kbd: %d checks, %d failures\n", checks, failures);
     return failures == 0 ? 0 : 1;
}
