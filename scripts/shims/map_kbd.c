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
 * this file needs fifteen constants out of it. They are NOT rbp keycodes -- the
 * K_* names below are -- and they are not ASCII: KEY_SPACE is 57, not 32.
 *
 * The numbers are pinned a second time, independently, in tests/test_kbd.c, so a
 * typo here is a failing expectation row rather than a key that does nothing.
 */
#define EV_KEY 0x01
#define EV_REL 0x02

#define KEY_ESC       1
#define KEY_1         2
#define KEY_2         3
#define KEY_BACKSPACE 14
#define KEY_ENTER     28
#define KEY_Z         44
#define KEY_X         45
#define KEY_N         49
#define KEY_M         50
#define KEY_COMMA     51
#define KEY_SPACE     57
#define KEY_UP        103
#define KEY_DOWN      108

#define BTN_RIGHT     0x111    /* 273: the mouse's right button */
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
     int key;     /* rbp keycode */
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

     /* ---- LOAD: the keys the deck numbers are printed on. LOAD is rbp's
      * global keycode with the deck in the channel -- map_jp21.c sends it the
      * same way (its notes 1/2 on the global receive channel, send channel
      * 1/2), which is why the channel here is 1/2 and not CH_GLOBAL. ---- */
     { EV_KEY, KEY_1,     K_LOAD, 1, 0, 0 },
     { EV_KEY, KEY_2,     K_LOAD, 2, 0, 0 },

     /* ---- the browse selector: one control, so one channel. The arrows rotate
      * it, Enter pushes it, and the wheel rotates it like the arrows do. The
      * step is the same +1/-1 map_jp21.c's handle_knob_pos() sends for a
      * clockwise/counter-clockwise turn of the panel's knob; the WHEEL and the
      * ARROWS agree with each other, and the sign against rbp's list has never
      * been observed (see the note under the table). ---- */
     { EV_KEY, KEY_UP,    K_SELECTOR, CH_GLOBAL, +1, 0 },
     { EV_KEY, KEY_DOWN,  K_SELECTOR, CH_GLOBAL, -1, 0 },
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

     if (verbose)
          klog("knobshim2: kbd: unmapped evdev type=%d code=%d value=%d\n",
               type, code, value);
}

static void kbd_build(void)
{
     /* A keyboard fills neither binding table: it has no note and no CC. The
      * reset is still the right thing -- the tables are shared storage and a map
      * switch is a rebuild from nothing rather than an append (ctrl_map.h) --
      * and neither table is ever consulted, because this map has no event(). */
     ctrl_bindings_reset();

     /* No key is held either: a rebuild must not leave one latched down, or the
      * release would be sent as a key that was never pressed. */
     for (int i = 0; i < NBINDS; i++)
          binds[i].down = 0;

     klog("knobshim2: kbd: keyboard fallback: space/z/x and n/m/, = "
          "PLAY/CUE/SYNC deck 1/2, 1/2 = LOAD, arrows/wheel + Enter = selector, "
          "Backspace/Esc/right-button = BACK/SOURCE\n");
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
