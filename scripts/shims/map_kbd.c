/*
 * map_kbd.c -- the keyboard fallback: rbp driven from a keyboard and a mouse.
 *
 * This is the surface that needs nothing plugged in, and that is its whole
 * point: display, audio and the USB import can be brought up and played with on
 * a Pi whose USB bus is empty, and a controller that has not arrived yet cannot
 * block the rest of the port. It is not a degraded mode -- it is the fastest
 * path to "the display works, now play something".
 *
 * It is also the only map whose events do not come from the sequencer. A
 * keyboard is not a MIDI device: /dev/input/event* carries EV_KEY and EV_REL,
 * which is why this map implements devices()/input() (ctrl_map.h) and the front
 * end starts evdev_io.c for it. The split is the same one the rest of the shim
 * is built on -- evdev_io.c never learns a keycode, this file never learns an
 * rbp address, and rbp is reached only through rbp_bridge.h.
 *
 * What it deliberately does not do:
 *
 *   - no LEDs and no meters. There is no panel: the keyboard has no lights rbp
 *     knows about, and the mouse has none at all.
 *   - no rbp-side startup. map_jp21.c's jp21_startup() exists for a surface with
 *     DECK/LINE switches, a Sweep knob and a Main Vol knob; a keyboard has none
 *     of those. The one rbp-side value the port does depend on -- the master
 *     level at unity -- is re-asserted for every map by rbp_vu.c's vu_thread,
 *     and is neither this map's to send nor this map's to forget.
 */
#define _GNU_SOURCE

/* A map reads the environment through shimutil.h, touches rbp through
 * rbp_bridge.h and reports through klog() -- the same short list map_jp21.c
 * uses, and for the same reason: it does no I/O of its own. */
#include "shimutil.h"
#include "rbp_abi.h"       /* the K_..., OP_... and CH_... names, and nothing else */
#include "rbp_bridge.h"    /* send_rx_key*() */
#include "ctrl_map.h"

/* ---- the kernel's input codes ---------------------------------------------
 *
 * These are the Linux input ABI's key codes, which are stable, spelled out here
 * rather than taken from <linux/input.h>: that header has changed shape under
 * this cross-toolchain once already (see pointsrc.c and tools/evdevdump.c) and
 * this file needs twenty-one constants out of it. They are NOT rbp keycodes --
 * the K_* names below are -- and they are not ASCII: KEY_SPACE is 57, not 32.
 *
 * The numbers are pinned a second time, independently, in tests/test_kbd.c, so a
 * typo here is a failing expectation row rather than a key that does nothing.
 */
#define EV_KEY 0x01
#define EV_REL 0x02

#define KEY_ESC       1
#define KEY_1         2
#define KEY_2         3
/* The rest of the top row, for rbp's browse keys (see the bindings below). The
 * digit row is contiguous in the ABI, so KEY_0 is 11 and not 10. */
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
#define KEY_DOWN      108

#define BTN_RIGHT     0x111    /* 273: the mouse's right button */
/* REL_WHEEL is 8, which is also KEY_7: one number, two namespaces -- and it is
 * the (type, code) pair, not the code, that the binding loop matches on. So an
 * EV_KEY of code 8 is the digit 7 and an EV_REL of code 8 is a wheel notch, and
 * neither can be "simplified" into one row. */
#define REL_WHEEL     0x08     /* value +1 = one notch away from the user */

/* ---- the bindings ---------------------------------------------------------
 *
 * One row per evdev code. `step` is the selector rotation this binding produces:
 * 0 means an ordinary press/release button, +1/-1 a step (OP_ROTATE). `down` is
 * the button's held state -- the kernel sends a press, then repeats, then one
 * release, and rbp must hear exactly one press and one release.
 *
 * Deck 2 mirrors deck 1 on the same keycodes with the deck in the *channel*,
 * which is how map_jp21 drives both decks from one table; rbp has no per-deck
 * variants of the selector, BACK or SOURCE, so those three are global on both.
 */
struct kbd_bind {
     int type;    /* EV_KEY or EV_REL */
     int code;    /* evdev code */
     int key;     /* rbp keycode, or 0 for a control rbp has no code for yet */
     int ch;      /* rbp send channel: 1/2 for a deck, CH_GLOBAL for a global */
     int step;    /* OP_ROTATE direction, or 0 for a press/release button */
     int down;    /* held (EV_KEY buttons only) */
};

static struct kbd_bind binds[] = {
     /* ---- deck 1: space / z / x ---- */
     { EV_KEY, KEY_SPACE, K_PLAY, 1, 0, 0 },
     { EV_KEY, KEY_Z,     K_CUE,  1, 0, 0 },
     { EV_KEY, KEY_X,     K_SYNC, 1, 0, 0 },

     /* ---- deck 2: n / m / , -- the bottom row again under the other hand, so
      * both decks are reachable without leaving the home row. ---- */
     { EV_KEY, KEY_N,     K_PLAY, 2, 0, 0 },
     { EV_KEY, KEY_M,     K_CUE,  2, 0, 0 },
     { EV_KEY, KEY_COMMA, K_SYNC, 2, 0, 0 },

     /* ---- W and S: PLAY on deck 1 and deck 2, for the hand that is not on
      * the bottom row. Requested by the operator on 2026-09-26 and additive --
      * SPACE and N stay where they are. ---- */
     { EV_KEY, KEY_W,     K_PLAY, 1, 0, 0 },
     { EV_KEY, KEY_S,     K_PLAY, 2, 0, 0 },

     /* ---- LOAD: the keys the deck numbers are printed on. LOAD is rbp's
      * global keycode with the deck in the channel -- map_jp21.c sends it the
      * same way (its notes 1/2 on the global receive channel, send channel
      * 1/2), which is why the channel here is 1/2 and not CH_GLOBAL. ---- */
     { EV_KEY, KEY_1,     K_LOAD, 1, 0, 0 },
     { EV_KEY, KEY_2,     K_LOAD, 2, 0, 0 },

     /* ---- the browse screen's buttons, on the rest of the digit row: 5/6/7
      * SOURCE / BROWSE / TAG LIST and 0 MENU. All four are rbp's *global*
      * browse-area keycodes (rbp_abi.h's 0x020x block), sent the way this map
      * already sends SOURCE and BACK -- CH_GLOBAL, the channel rbp's own
      * browse keys arrive on -- and they are press/release buttons, not steps.
      *
      * They sit on the digit row on purpose: 1/2 are LOAD, so the whole browse
      * surface is one hand's worth of keys that need no mnemonic to remember.
      *
      * rbp has these six controls -- the labels are in the binary as Source,
      * BROWSE, TAGLIST, PlayList/PLAYLIST, Search and menu -- but rbp_abi.h
      * carries a keycode for only four of them. ---- */
     { EV_KEY, KEY_5,     K_SOURCE,  CH_GLOBAL, 0, 0 },
     { EV_KEY, KEY_6,     K_BROWSE,  CH_GLOBAL, 0, 0 },
     { EV_KEY, KEY_7,     K_TAGLIST, CH_GLOBAL, 0, 0 },

     /* ---- 8 and 9: PLAYLIST and SEARCH. TODO: rbp has both controls and this
      * map has the keys, but rbp_abi.h has no keycode for either, and the
      * MAPPING.md it cites for the 0x020x block is not in the repository. These
      * two rows are declared with key 0 -- ctrl_map.h's "a control rbp has no
      * code for" -- so the keys are *visibly* pending rather than silently
      * absent: kbd_input() names the control and sends nothing. Replace the 0
      * with the real keycode and move the row up once one is measured; do not
      * guess a value into the 0x020x block, whose two unclaimed slots (0x0204,
      * 0x0205) are a plausible-looking trap rather than an answer. ---- */
     { EV_KEY, KEY_8,     0,         CH_GLOBAL, 0, 0 },   /* PLAYLIST */
     { EV_KEY, KEY_9,     0,         CH_GLOBAL, 0, 0 },   /* SEARCH */

     { EV_KEY, KEY_0,     K_MENU,    CH_GLOBAL, 0, 0 },

     /* ---- the browse selector: one control, so one channel. The arrows rotate
      * it, Enter pushes it, and the wheel rotates it like the arrows do. The
      * step is the same +1/-1 map_jp21.c's handle_knob_pos() sends for a
      * clockwise/counter-clockwise turn of the panel's knob.
      *
      * The signs were inverted on 2026-09-26: with UP at +1 and DOWN at -1 the
      * operator reported the pair working backwards, which is the first
      * observation of the sign either way. It could not be measured here --
      * both keys provably arrive (`code=103/108 -> 0x420c rotate`) but the
      * SOURCE panel's frame is byte-identical after each press, because with
      * one USB attached there is nothing for the selector to move between. So
      * the sign is the operator's result, taken on a screen with a list in it.
      *
      * The WHEEL is left at +1 deliberately rather than flipped with them. It
      * was not reported as wrong, and +1 is what makes it agree with DOWN: a
      * wheel rolled away from the user moves down the list, which is the
      * natural correspondence. See docs/16-input-and-hotplug.md. ---- */
     { EV_KEY, KEY_UP,    K_SELECTOR, CH_GLOBAL, -1, 0 },
     { EV_KEY, KEY_DOWN,  K_SELECTOR, CH_GLOBAL, +1, 0 },
     { EV_KEY, KEY_ENTER, K_SELECTOR, CH_GLOBAL,  0, 0 },
     { EV_REL, REL_WHEEL, K_SELECTOR, CH_GLOBAL, +1, 0 },

     /* ---- BACK and SOURCE, on the keyboard and on the mouse's right button:
      * the two "leave this screen" keys are the ones a mouse is for. ---- */
     { EV_KEY, KEY_BACKSPACE, K_BACK,   CH_GLOBAL, 0, 0 },
     { EV_KEY, KEY_ESC,       K_SOURCE, CH_GLOBAL, 0, 0 },
     { EV_KEY, BTN_RIGHT,     K_BACK,   CH_GLOBAL, 0, 0 },
};

#define NBINDS ((int)(sizeof(binds) / sizeof(binds[0])))

/* A step is capped like map_jp21.c's rot_clamped(): a wheel that arrives with a
 * dozen notches at once must not become a dozen keycodes in one burst. */
static void kbd_rot(int key, int ch, int n)
{
     if (n > 16) n = 16;
     if (n < -16) n = -16;
     for (int i = 0; i < (n < 0 ? -n : n); i++)
          send_rx_key(key, OP_ROTATE, ch, (n < 0) ? -1 : 1);
}

/* What a triple turns into. Kept as a value rather than acted on inline so the
 * KNOB_VERBOSE line can say what actually happened -- an autorepeat that was
 * ignored must not read like a keycode that was sent. */
enum { ACT_IGNORE = 0, ACT_PRESS, ACT_RELEASE, ACT_ROTATE };
static const char *const act_name[] = { "ignored", "press", "release", "rotate" };

/* The unmapped-event log, rate-limited per (type, code).
 *
 * It is a diagnostic for a control nobody has bound yet, and it is worth keeping
 * for exactly that. What made it a problem is the mouse: motion arrives as
 * EV_REL/REL_X and REL_Y, which match no binding, and every one of those reports
 * used to log a line -- through klog(), which opens, writes and closes
 * /tmp/knobshim.log on every call. At 125 Hz that is ~250 lines and ~750
 * syscalls a second, at 1000 Hz it is eight times that, all of it on the single
 * thread that has to dispatch keypresses. A key pressed during a mouse move
 * therefore arrives late, which is one half of "the keyboard is sometimes laggy"
 * and is entirely self-inflicted by KNOB_VERBOSE=1 (docs/16).
 *
 * So: at most one line per (type, code) per second, with the number suppressed
 * in between. Eight slots, evicted least-recently-used; the realistic keyboard
 * and mouse together report well under that, and a device reporting more than
 * eight distinct unmapped codes is already worth looking at. */
#define UNMAPPED_SLOTS  8
#define UNMAPPED_PER_MS 1000

struct unmapped_slot {
     int type, code;
     unsigned long long last_ms;
     unsigned long suppressed;
};

static void log_unmapped(int type, int code, int value)
{
     static struct unmapped_slot slots[UNMAPPED_SLOTS];
     unsigned long long now = shim_now_ms();
     struct unmapped_slot *s = NULL, *free_slot = NULL, *oldest = NULL;

     for (int i = 0; i < UNMAPPED_SLOTS; i++) {
          struct unmapped_slot *c = &slots[i];

          if (c->last_ms == 0) {
               if (!free_slot)
                    free_slot = c;
               continue;
          }
          if (c->type == type && c->code == code) {
               s = c;
               break;
          }
          if (!oldest || c->last_ms < oldest->last_ms)
               oldest = c;
     }
     if (!s) {
          s = free_slot ? free_slot : oldest;
          s->type = type;
          s->code = code;
          s->last_ms = 0;
          s->suppressed = 0;
     }
     if (s->last_ms && now - s->last_ms < UNMAPPED_PER_MS) {
          s->suppressed++;
          return;
     }
     /* The first line of a burst carries the value; the repeats carry the count,
      * because after the first one the interesting number is how many were
      * dropped, not what the hundredth one's value happened to be. */
     if (s->suppressed)
          klog("knobshim2: kbd: unmapped evdev type=%d code=%d (%lu more in "
               "the last second)\n", type, code, s->suppressed);
     else
          klog("knobshim2: kbd: unmapped evdev type=%d code=%d value=%d\n",
               type, code, value);
     s->last_ms = now;
     s->suppressed = 0;
}

/* The name of a control this map can see but cannot drive yet. Only the two
 * browse keys with no keycode are in here (see the bindings above); naming them
 * is the difference between a log that says which control is dead and one that
 * says only which number was pressed. */
static const char *pending_name(int code)
{
     switch (code) {
     case KEY_8: return "PLAYLIST";
     case KEY_9: return "SEARCH";
     default:    return "?";
     }
}

/* One raw evdev triple, exactly as the kernel delivered it, from evdev_io.c's
 * reader thread. Never called before rbp's KeyManager exists: the front end
 * gates it the way it gates a sequencer event. */
static void kbd_input(int type, int code, int value)
{
     for (int i = 0; i < NBINDS; i++) {
          struct kbd_bind *b = &binds[i];
          int act = ACT_IGNORE, step = 0;

          if (b->type != type || b->code != code)
               continue;

          /* A declared control with no rbp keycode: nothing is sent, and the
           * press is named rather than dropped. Reached before the press/
           * release logic, because with no keycode there is no held state to
           * track and sending 0 would be a keycode rbp never asked for. */
          if (b->key == 0) {
               if (verbose)
                    klog("knobshim2: kbd: %s pressed (evdev code=%d) but rbp "
                         "has no keycode for it yet -- nothing sent; see the "
                         "TODO in map_kbd.c\n", pending_name(code), code);
               return;
          }

          if (type == EV_REL) {
               /* The wheel: `value` notches, the sign is the direction. */
               if (value != 0) {
                    act = ACT_ROTATE;
                    step = value * b->step;
               }
          } else if (b->step != 0) {
               /* A step: the kernel's autorepeat repeats the key, and repeating
                * the step is what makes holding an arrow scroll. The release
                * sends nothing -- there is nothing held down. */
               if (value == 1 || value == 2) {
                    act = ACT_ROTATE;
                    step = b->step;
               }
          } else if (value == 1 && !b->down) {
               act = ACT_PRESS;
               b->down = 1;
          } else if (value == 0 && b->down) {
               act = ACT_RELEASE;
               b->down = 0;
          }
          /* Everything else is deliberately nothing: the kernel's autorepeat
           * (value 2) on a button, or a release whose press was dropped by the
           * KeyManager gate. rbp already has the press, and a second press is
           * not what the operator asked for. */

          if (verbose)
               klog("knobshim2: kbd: evdev type=%d code=%d value=%d -> 0x%04x %s\n",
                    type, code, value, b->key, act_name[act]);

          if (act == ACT_ROTATE)
               kbd_rot(b->key, b->ch, step);
          else if (act != ACT_IGNORE)
               send_rx_key(b->key, act == ACT_PRESS ? OP_PRESS : OP_RELEASE,
                           b->ch, 0);
          return;
     }

     /* Rate-limited: see log_unmapped(). A mouse's motion is unmapped by design
      * and arrives thousands of times a second, and a log line per report is how
      * the keypress behind it is made to feel late. */
     if (verbose)
          log_unmapped(type, code, value);
}

static void kbd_build(void)
{
     /* A keyboard fills neither binding table: it has no note and no CC, and
      * neither table is ever consulted because this map has no event().
      *
      * Nor does it EMPTY them. It used to call ctrl_bindings_reset() here, which
      * was correct while a map was the only surface in the process; now that the
      * FLX4 can be live on the other selection, that call would wipe the FLX4's
      * bindings and put nothing in their place -- every controller button dead,
      * from a map that is working exactly as designed. The front end owns the
      * reset (ctrl_map.h), and this build is additive: it adds nothing. */

     /* No key is held either: a rebuild must not leave one latched down, or the
      * release would be sent as a key that was never pressed. */
     for (int i = 0; i < NBINDS; i++)
          binds[i].down = 0;

     klog("knobshim2: kbd: keyboard fallback: space/z/x and n/m/, = "
          "PLAY/CUE/SYNC deck 1/2 (w/s also PLAY 1/2), 1/2 = LOAD, "
          "arrows/wheel + Enter = selector, "
          "Backspace/Esc/right-button = BACK/SOURCE\n");
     /* Said at build time and not only under KNOB_VERBOSE=1, because the two
      * pending keys are otherwise indistinguishable from working ones until
      * someone presses them and watches nothing happen. */
     klog("knobshim2: kbd: 5/6/7/0 = SOURCE/BROWSE/TAG LIST/MENU; "
          "8/9 = PLAYLIST/SEARCH are declared but have no rbp keycode yet, "
          "so pressing them sends nothing (TODO in map_kbd.c)\n");
}

static void kbd_startup(void)
{
     /* Nothing to tell rbp, deliberately -- see the note at the top of the file:
      * the master level is asserted by rbp_vu.c for every map, and the mixer
      * input routing and Sound Color FX defaults jp21_startup() sends exist for
      * a surface that has DECK/LINE switches and a Sweep knob. */
     klog("knobshim2: kbd: no surface-side rbp setup (master level is the VU path's)\n");
}

static int kbd_devices(void)
{
     /* One non-MIDI source, and the reader opens every EV_KEY device it finds
      * rather than one node: the keyboard carries the keys and the mouse carries
      * BTN_RIGHT and REL_WHEEL, and those are two devices. The number is not a
      * target -- anything above zero is what tells the front end to start the
      * reader, and 0 (what a MIDI-only map returns) is what tells it not to. */
     return 1;
}

const struct ctrl_map map_kbd = {
     "kbd",
     kbd_build,
     kbd_startup,
     NULL,            /* no sequencer events: a keyboard is not a MIDI device */
     NULL,            /* no tick: nothing in this map is about time */
     kbd_devices,
     kbd_input,
};
