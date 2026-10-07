/*
 * prompt_zone.h -- the USB STOP chooser: its box, its two buttons, its hold.
 * PURE.
 *
 * WHY IT EXISTS. The swipe-down menu's seventh column is rbp's own safe eject
 * (`pointsrc.c`'s menu_key[], rbp_abi.h's K_USBSTOP), and it was one tap. One tap
 * on the far right of the band unmounted the operator's stick -- the stick that
 * carries production media whose FAT already has pre-existing damage
 * (docs/10-usb.md). The operator's ask, 2026-10-05:
 *
 *     "when you have USB stop, put up a prompt for USB1, USB2 or Cancel"
 *
 * So the column no longer stops anything: it raises this. What the box offers is a
 * device to stop, and the eject takes a gesture that cannot be made by accident.
 *
 * THE GESTURE IS A THREE-SECOND HOLD, at the operator's ask of 2026-10-07:
 *
 *     "for the USB stop pop up just have two buttons USB1 (hold) or USB2 (hold),
 *      lit if it is active and when you press down have the button flash and if
 *      still held after 3 seconds have it ejected. if they release it before three
 *      seconds don't eject it."
 *
 * which REPLACES the two-tap gesture this file carried from 2026-10-06 (devices on
 * one line, OK and CANCEL under them). The buttons are lit when rbp says the device
 * is there; a press on a lit one flashes it and starts the hold; three seconds later
 * the eject goes out; a release before that sends nothing at all. There is no OK to
 * arm and no CANCEL to answer, so PR_ACT_CANCEL is gone with them -- the only answers
 * this box gives are "stop USB 1", "stop USB 2", and nothing.
 *
 * THE WAY OUT WITHOUT STOPPING ANYTHING IS TO TAP ANYWHERE THAT IS NOT A BUTTON --
 * the title, the rule, the margin, the glass outside the box entirely. That release
 * dismisses the box and sends nothing, and it is the same rule the box already had
 * for a press that missed ("tap outside to dismiss"), so it needed no new mechanism
 * when CANCEL went away. The ten-second PR_TIMEOUT_MS self-dismissal is still there
 * behind it for a box left alone.
 *
 * THE TWO DEVICES ARE rbp's OWN, NOT A LIST THIS FILE KEEPS. `ui::UsbStorageManager`
 * is constructed once per channel -- `UsbStorageManager(UiObject::Channel, Player&,
 * Player&, char const*)`, and `ui::BrowseUiIfDpl::regUsbManager()` proves the range
 * by refusing any channel outside {1,2}. Measured on the unit 2026-10-05, rbp holds
 * exactly two, and `[mgr+0x84]` is the channel:
 *
 *     [0] mgr 0xcd79a078  ch=1  media(+0x88)=2      <- USB 1, the operator's stick
 *     [1] mgr 0xcd79aa10  ch=2  media(+0x88)=0      <- USB 2, nothing in it
 *
 * and `ui::UsbStorageManager::onKey` @0x3259f4 drops the key unless
 * `[key+0xa] == [this+0x84]` (325a2c..325a3c), which is how a chooser stops ONE
 * device: `send_rx_key(K_USBSTOP, ..., ch, 0)` with ch the device's own number.
 * CH_GLOBAL is 1, which is why the old single-column binding ever worked on USB 1
 * and could never have reached USB 2.
 *
 * SO A BUTTON CAN BE DIM, and it is rbp that says so: `[mgr+0x88] == 2` is "media
 * present and ready", and it is the same test `onUsbStopKey`'s press branch makes
 * before it does anything at all. A button whose device rbp reports absent is drawn
 * in the dim ink and CANNOT START A HOLD -- pressing it does nothing whatever, and
 * saying so is the whole of what makes a dim button honest rather than a button that
 * silently does nothing. It does not dismiss the box either: the operator who pressed
 * the dim one probably wanted the other, and the box staying up is what lets them
 * have it.
 *
 * BOTH BUTTONS ARE LIT WHEN BOTH SLOTS ARE FILLED. Since 2026-10-07
 * `scripts/device/usb-watch.sh` feeds both of rbp's media slots -- the first stick
 * it finds carrying a rekordbox export takes `/media/usb1/sda1`, the second takes
 * `/media/usb2/sda1`, each on its own FIFO. rbp was already listening: it opens
 * `/proc/udev_usb2` and `/media/usb2` in the chroot at startup, so a button lights up
 * by itself the moment the host side feeds it, and nothing here had to change for
 * that. Measured 2026-10-07: with both sticks in, both `ui::UsbStorageManager`
 * objects read `media=2` and rbp holds all four databases open. A button is still dim
 * when its slot is empty -- that is rbp's answer, not a guess.
 *
 * THE BOX IS ITS OWN PLANE, at ITS OWN SIZE. It is not the band at another size and
 * it is not the band's slot: `drmband.c` holds the fd and master once and refcounts
 * them, so a fourth `drm_band_setup()` is a buffer and a plane (the two drawers
 * already coexist, measured at planes 127 and 138). The band is shut by the time
 * this opens -- a tap on a menu button closes the panel -- so the two are never on
 * the glass together anyway.
 *
 * PURE, LIKE menu_zone.c AND util_zone.c. Nothing here reaches rbp, the framebuffer
 * or a clock of its own: the time arrives as an argument, and the two device-liveness
 * bits and the two device names arrive in a struct prompt_state. The one thing it does
 * read is menu_font.h -- the atlas's own advances, which are a compile-time table and
 * nothing else -- because how wide a name draws is what decides how much of it a button
 * can hold. That is what lets test_prompt.c pin the geometry, the hold, the flash, the
 * dim button, the timeout and the two labels on a host with no Pi, no panel and no
 * player -- and it is why the caller, not this file, is the one that sends.
 */
#ifndef RBPI4B_PROMPT_ZONE_H
#define RBPI4B_PROMPT_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* ------------------------------------------------------------------ geometry
 *
 * All logical, in the menu's own 1280x800 space, and all derived from the box
 * rather than written down as a table. The box is centred on the screen; the
 * arithmetic below is what test_prompt.c checks adds up to PR_H exactly, so a
 * change to the row's height moves what is under it instead of running off the
 * bottom.
 *
 *     +-----------------------------------------------------+  PR_Y0  (black bed)
 *     |  PR_BORDER                             (a margin)   |
 *     |           USB STOP                     (PR_TITLE_H) |
 *     |  ___________________________________________________|  the rule
 *     |  PR_PAD                                             |
 *     |  +-----------------------+   +----------------------+|
 *     |  |      HOLD USB 1       |   |      HOLD USB 2      ||  the one row
 *     |  +-----------------------+   +----------------------+|
 *     |  PR_PAD                                             |
 *     |  PR_BORDER                                          |
 *     +-----------------------------------------------------+  PR_Y1
 *
 * ONE ROW, TWO BUTTONS, since 2026-10-07. The second line was OK and CANCEL; both are
 * gone and the box is exactly as tall as the one line of devices needs, which is
 * PR_H = 127 rather than 199. PR_ROW_GAP survives the change although nothing can
 * reach it at one row: it is PR_ROW_TOP()'s stride, and a third device would need it
 * and nothing else.
 *
 * THE LABELS SAY "HOLD USB 1", not "USB 1 (HOLD)". It is the gesture that has to be
 * on the button -- the operator has to know a hold is wanted before they make one --
 * and the font's character set has no parentheses (menu_font.h's CHARS is letters,
 * digits and "./-_:?=&@"), so a parenthesised label would mean re-baking the settled
 * 19 px Decker atlas for two punctuation glyphs. At 99 px the label has 164 px of the
 * 263 px inner cell to spare (measured off the atlas, pinned in test_prompt.c).
 *
 * AND THE NUMBER IS THE DEVICE'S NAME WHERE THERE IS ONE. The operator's ask of
 * 2026-10-07 --
 *
 *     "display the USB label name when on the USB stop menu"
 *
 * -- so a button reads "HOLD RBOX USB" where it used to read "HOLD USB 1". The name is
 * the stick's own volume label, the same string rbp's SOURCE screen shows, and it
 * arrives in this struct from the file `usb-watch.sh` leaves beside each slot's FIFO
 * (docs/10-usb.md, section *The device name*). A device with no label of its own, and
 * a slot with nothing in it at all, keep the number: "HOLD " + "USB 1" is exactly the
 * string this box shipped with, so a unit carrying no label files draws the picture it
 * always drew, byte for byte.
 *
 * A NAME IS LONGER THAN A NUMBER, AND IT IS THE OPERATOR'S TO CHOOSE, so two rules come
 * with it and both are in prompt_zone.c. The composed label is CLIPPED to the cell by
 * the atlas's own advances (prompt_text_clip()) -- a label that did not fit would trip
 * prompt_paint.c's size refusal and take the whole box off the glass, which is a far
 * worse answer than a shortened name. And every character the atlas cannot draw is
 * composed as '?' rather than shipped as a 5 px gap, which is the rule usb_label.h's own
 * sanitizer keeps for bytes above 0x7e extended to the printable ones the atlas lacks.
 */
#define PROMPT_ROWS     1       /* the devices: the box's only line */
#define PROMPT_COLS     2       /* one device each */
#define PROMPT_CELLS    (PROMPT_ROWS * PROMPT_COLS)

/* The devices rbp can be asked to stop. It is the same number as PROMPT_CELLS today
 * and it is still written separately: pointsrc.c sizes its walk of rbp's
 * UsbStorageManager array by THIS, and a box that grew a button that is not a device
 * (an OK, a Cancel -- the row this box used to have) must not change how far that
 * walk goes. */
#define PROMPT_DEVICES  2

/* The two cells, 1-based and ROW-MAJOR, so PR_CELL_ROW/COL below are pure arithmetic
 * and the painter's and the hit test's loops are the same loop. */
#define PR_CELL_USB1    1       /* the left button  */
#define PR_CELL_USB2    2       /* the right button */

#define PR_W        560
#define PR_H        127         /* 2*3 + 40 + 1 + 2*8 + 1*64, exactly */
#define PR_X0       ((MZ_LOGICAL_W - PR_W) / 2)          /* 360 */
#define PR_Y0       ((MZ_LOGICAL_H - PR_H) / 2)          /* 336 */
#define PR_X1       (PR_X0 + PR_W - 1)                   /* 919 */
#define PR_Y1       (PR_Y0 + PR_H - 1)                   /* 462 */

/* The box's margin, inside it on all four sides. It was the frame's thickness until
 * 2026-10-06, when the operator asked for the white to go round each ROW rather than
 * round the whole box (prompt_paint.c's header); the number did not move, because it
 * was always doing both jobs -- keeping the title and the rows off the box's edge.
 * Nothing draws a mark here any more: the box's own edge is the black bed. */
#define PR_BORDER   3
#define PR_PAD      8           /* between the frame and what it holds */
#define PR_TITLE_H  40
#define PR_ROW_H    64
#define PR_ROW_GAP  8           /* PR_ROW_TOP()'s stride; unreachable at one row */

/* The row's own rect: both buttons span this, and the two columns divide it. The gap
 * between the columns is PR_COL_GAP, the same number PR_ROW_GAP is, so the buttons and
 * the space between them read as one grid. */
#define PR_ROW_X0   (PR_X0 + PR_BORDER + PR_PAD)          /* 371 */
#define PR_ROW_X1   (PR_X1 - PR_BORDER - PR_PAD)          /* 908 */
#define PR_COL_GAP  8

/* One button: an exact half of the block less half the gap. The odd pixel a 538-wide
 * block would leave over is dropped rather than handed to one column, so the two
 * buttons are the same width -- PR_ROW_X1 - PR_ROW_X0 + 1 - PR_COL_GAP is even here
 * (530), and test_prompt.c asserts the leftover is 0, which is what makes the two
 * columns interchangeable rather than one of them a pixel wider. */
#define PR_CELL_W   ((PR_ROW_X1 - PR_ROW_X0 + 1 - PR_COL_GAP) / PROMPT_COLS)   /* 265 */
#define PR_CELL_X0(c) (PR_ROW_X0 + (PR_CELL_COL(c) - 1) * (PR_CELL_W + PR_COL_GAP))
#define PR_CELL_X1(c) (PR_CELL_X0(c) + PR_CELL_W - 1)

/* The title's band and the rule under it. The title spans the whole inner width
 * rather than PR_ROW_X0..PR_ROW_X1: it is centred on the BOX, which is what makes
 * it read as the box's caption and not as a third row. */
#define PR_TITLE_Y0 (PR_Y0 + PR_BORDER)
#define PR_TITLE_Y1 (PR_TITLE_Y0 + PR_TITLE_H - 1)
#define PR_RULE_Y   (PR_TITLE_Y1 + 1)

/* Row i (0-based) top -- the arithmetic in one place so the painter, the hit test and
 * the test cannot disagree about where a row is. */
#define PR_ROW_TOP(i) (PR_RULE_Y + 1 + PR_PAD + (i) * (PR_ROW_H + PR_ROW_GAP))

/* The cell number's own row and column, 1-based, in the order the enum above lists
 * them. These two are the whole of "which cell is where": everything else is derived,
 * so a third device would be a constant and nothing else. */
#define PR_CELL_ROW(c) (((c) - 1) / PROMPT_COLS + 1)
#define PR_CELL_COL(c) (((c) - 1) % PROMPT_COLS + 1)

/* Cell `c`'s inclusive rect -- the same rect prompt_cell_at() tests, so the ink and
 * the hit target are the same pixels. */
#define PR_CELL_Y0(c)  PR_ROW_TOP(PR_CELL_ROW(c) - 1)
#define PR_CELL_Y1(c)  (PR_CELL_Y0(c) + PR_ROW_H - 1)

/* THE INK'S MARGIN INSIDE A BUTTON, on each side, in font pixels -- the whole of what
 * a label may not use. It is TWO and not the frame's one, because the atlas is not
 * confined to its own advances: the Decker bake inks up to a pixel past the advance of
 * 'A' and ten other glyphs and a pixel before the pen of a good many more (measured off
 * menu_font_glyphs), so a line laid out to the advance exactly TOUCHES the frame -- and
 * the frame is one pixel of the cell's own ink. One for the frame, one for the overhang.
 *
 * ONE NUMBER, TWO USERS, and that is the point of writing it down. prompt_paint.c's
 * size refusal asks it about the cell's own default label, and prompt_text_clip() cuts
 * any longer name to what is left. The refusal is therefore always the STRICTER of the
 * two -- a view that hosts "HOLD USB 1" whole hosts 2*PR_LABEL_MARGIN px more than the
 * clip would ever use -- so no label can be clipped to something the box would have
 * taken whole, and a unit with no label files draws the pixels it always drew. */
#define PR_LABEL_MARGIN 2

/* THE HOLD, which is now the whole of the gesture. PR_HOLD_MS is the operator's own
 * three seconds. PR_HOLD_FLASH_MS is the HALF period of the blink a running hold
 * draws -- 250 ms, which is rbp's own house blink (a State-2 LedStat entry blinks at
 * its `+28`, and the deck PLAY lights are 500 ms a full cycle), so six full blinks
 * fit inside one hold. The button is drawn PRESSED for the first half period and
 * normal for the second, so a tap of any length at all still shows the operator that
 * their finger landed on something live. */
#define PR_HOLD_MS        3000
#define PR_HOLD_FLASH_MS  250

/* The box goes away on its own. The operator asked for a prompt, not for a modal
 * they have to escape: a prompt left on the glass covers the middle of the
 * performance screen, and nothing about the eject is urgent enough to be worth
 * that. Ten seconds is long enough to read two buttons and decide -- and long enough
 * that a three-second hold started in the last third of it still completes.
 *
 * IT IS SUSPENDED WHILE A FINGER IS DOWN, and by the same rule that suspends it
 * during a hold: a press the box swallowed has been withheld from rbp for its whole
 * life and rbp is going to be given its release ([[declined-press-must-still-see-release]]). */
#define PR_TIMEOUT_MS 10000

/* ------------------------------------------------------------- the answers */
enum {
    PR_ACT_NONE = 0,        /* nothing is to be stopped: the press missed, slid off,
                             * was let go before its three seconds, or landed on a
                             * button whose device rbp says is not there */
    PR_ACT_USB1,            /* a completed hold on USB 1 */
    PR_ACT_USB2             /* a completed hold on USB 2 */
};

/* What rbp says about the two devices -- and what the host says they are CALLED. Filled
 * by the caller, never by this module: prompt_zone.c is pure and opens no file.
 *
 * `live[i]` is "device i+1 has media present", i.e. `[mgr+0x88] == 2`; a caller that
 * cannot read rbp leaves both 0, which draws both buttons dim and can start no hold at
 * all -- a refusal, not an assumption.
 *
 * `label[i]` is device i+1's button text, ALREADY COMPOSED and already drawable, or
 * EMPTY, which means "this device has no name of its own and the button keeps its own
 * number". prompt_state_name() is the only thing that writes it and it is pure, so a
 * caller does not compose the string and this module does not read a file; the two
 * halves meet in test_prompt.c with neither a Pi nor a stick. A caller that fills
 * neither -- test_menu_dev.c's fake, and any caller that predates this -- gets the
 * defaults, which is the shipped picture. */
#define PR_LABEL_MAX 35         /* "HOLD " (5) + usb_label.h's USB_LABEL_MAX (30) */

struct prompt_state {
    int live[PROMPT_DEVICES];
    char label[PROMPT_DEVICES][PR_LABEL_MAX + 1];
};

/* ---------------------------------------------------------------- the state */

/* Raise the box, or re-raise it if it is already up (which restarts the clock).
 * `now_ms` is the caller's monotonic milliseconds; the module keeps no clock of
 * its own. A press in flight is NOT inherited: the box always starts with no press
 * behind it, so the tap that raised it is already over. Neither is a HOLD: a box that
 * has been away comes back with both buttons idle, so nothing the operator started
 * ten minutes ago can be what the next three seconds finish. */
void prompt_open(unsigned long long now_ms);

/* Put it away. Safe on a shut box, and safe from the tick as well as from the touch
 * thread -- see prompt_tick(). */
void prompt_close(void);

int prompt_is_open(void);

/* The wall-clock milliseconds at which the box closes itself. Only meaningful
 * while prompt_is_open(). */
unsigned long long prompt_deadline(void);

/* WHAT THE CLOCK HAS DONE TO THE BOX, asked on every slice of the caller's wait.
 * Returns one of PR_TICK_* below and is the module's only time entry point -- there is
 * no separate expiry call, because a hold that completes and a box that times out are
 * the same question asked of the same clock, and two doors into one state is how they
 * come to disagree.
 *
 * IT IS NOT ONLY THE TOUCH THREAD THAT ASKS. The box has to be able to go away with
 * nobody touching anything, and pointsrc.c's read loop otherwise wakes only on an
 * event -- so pointsrc.c waits in slices while the box is up, exactly as it already
 * does for a held menu key, and asks this at each one. That keeps ONE writer of this
 * state (the touch thread) and needs no lock: menu_draw.c only ever READS it, the
 * same way it reads menu_is_open(). */
enum {
    PR_TICK_NONE = 0,       /* the box is not up; nothing was done */
    PR_TICK_FLASH,          /* nothing but the blink moved; the box is still up */
    PR_TICK_EJECT,          /* A HOLD COMPLETED. `*act` names the device to stop and the
                             * caller sends it, once. THE BOX IS STILL UP: the finger
                             * that made the hold is by definition still down, so the
                             * release is still owed and the box has to be there to take
                             * it (prompt_zone.c's fire branch says why) */
    PR_TICK_TIMEOUT         /* the box dismissed itself, having stopped nothing */
};
int prompt_tick(const struct prompt_state *S, unsigned long long now_ms, int *act);

/* Drop everything, including a press in flight and a hold that is running.
 * pointsrc.c calls this when the pointer device goes away, beside menu_reset(). */
void prompt_reset(void);

/* ---------------------------------------------------------------- the input */

/* The cell (1..PROMPT_CELLS) under a logical point, or 0 for anything else -- the
 * title, the gap between the two buttons, the margin, or off the box entirely. */
int prompt_cell_at(int x, int y);

/* The cell the finger is on right now, or 0 -- what the painter highlights, and the
 * same question menu_pressed() answers for the band. It is the LIVE cell and not the
 * anchored one on purpose: a finger that has slid to the other button is highlighting
 * where it is, while what it will hold is still where it started.
 *
 * IT IS 0 FOR THE CELL WHOSE HOLD IS RUNNING. There the pressed face comes from the
 * flash instead (prompt_hold_flash()), which is the only way the button can blink: a
 * steady finger highlight and a blink are the same pair of pixels, so a cell that was
 * both would show the operator nothing at all. */
int prompt_pressed(void);

/* The cell whose HOLD IS RUNNING -- the operator's finger is down on it, it is live,
 * and the finger has not left it -- or 0 for none. This is the cell the eject will be
 * for, and it is a question about this module's own state, so unlike prompt_cell_live()
 * it takes no struct prompt_state. */
int prompt_hold_cell(void);

/* The flash, while a hold is running: 1 while the button draws PRESSED and 0 while it
 * draws its normal live face. Meaningless with no hold, and then 0. It advances only in
 * prompt_tick() -- the module has no clock, so the phase has to be carried in the state
 * the same way the hold itself is. */
int prompt_hold_flash(void);

/* How long the running hold has been held, in milliseconds, or 0 with no hold. It is
 * what the caller logs: "usb 1 held 3020 ms (hold is 3000 ms)" is the line that tells a
 * drill the threshold was crossed by a real finger and not by the tick's own slice. */
unsigned long long prompt_hold_ms(unsigned long long now_ms);

/* Has this cell anything to do? Exactly "rbp says device cell-1 has media", and
 * out of range answers 0 as well, so a caller cannot read past the array. */
int prompt_cell_live(const struct prompt_state *S, int cell);

/* The cell's rect, logical, inclusive at both ends -- the same rect prompt_cell_at()
 * tests, so the ink and the hit target are the same pixels. */
void prompt_cell_rect(int cell, int *x0, int *y0, int *x1, int *y1);

/* The box's own rect, logical, inclusive at both ends. */
void prompt_box_rect(int *x0, int *y0, int *x1, int *y1);

/* The label strings, published for the painter and for the test that checks every
 * character is in the font's set (menu_font.h draws a gap for one that is not, which
 * would ship as a dead-looking label rather than as a failure).
 *
 * prompt_cell_label() is the cell's OWN label -- the one that exists whether or not the
 * device in it has a name, and so the yardstick prompt_paint.c measures the button
 * against. prompt_cell_text() is what the button actually reads, which is that name
 * where there is one. Out of range answers 0 from both, so a caller cannot read past
 * the array. */
const char *prompt_title(void);
const char *prompt_cell_label(int cell);

/* What cell `cell`'s button reads: the state's own composed name for that device, or
 * prompt_cell_label() when the state names nothing -- an empty struct, a device with no
 * label on its stick, a slot with nothing in it. A NULL state is legal and is that same
 * answer, so a caller that has no snapshot draws the shipped box rather than nothing. */
const char *prompt_cell_text(const struct prompt_state *S, int cell);

/* Compose device `dev`'s button label into `S` from the device's own name (0-based, the
 * same index live[] uses). PURE: the name is an argument, this module reads no file.
 *
 * An empty or NULL name CLEARS the cell back to its default, which is what a slot whose
 * stick has been pulled must do -- a name left standing over an empty slot is a button
 * that names a device that is not there.
 *
 * The composition is "HOLD " + the name, so the fallback and the named form are one
 * string: "HOLD " + "USB 1" is the "HOLD USB 1" this box has always drawn. Every
 * character the 19 px atlas cannot draw becomes '?', and the result is cut at
 * PR_LABEL_MAX characters (the painter cuts it again, by pixels -- see
 * prompt_text_clip()). */
void prompt_state_name(struct prompt_state *S, int dev, const char *name);

/* `text` cut to at most `max_px` of the atlas's own advances, into `out` (at most `max`
 * bytes, NUL included). Answers the character count, which is 0 for a NULL text, a
 * max_px that cannot hold even one character, or a max of 0.
 *
 * IT CUTS FROM THE END, so what is lost is always the tail of the name and never the
 * "HOLD" that says what the button does -- that word is 53 px of the smallest budget the
 * size refusal will ever hand out. Not scaled: the atlas is drawn at 1:1 (menu_paint.c's
 * rule), so `max_px` is framebuffer pixels. Each character is charged the same advance
 * menu_font_adv() gives it, measured against the character that follows it in the whole
 * name; on the shipped Decker bake MENU_FONT_KERNED is 0, so that is the character's own
 * advance and the measure is exact. */
int prompt_text_clip(const char *text, int max_px, char *out, int max);

/* Which channel rbp's send wants for an answer: the device's own number, which is
 * `[mgr+0x84]` -- 1 for USB 1 and 2 for USB 2. PR_ACT_NONE sends nothing and answers 0. */
int prompt_act_channel(int act);

/* Feed one pointer report, exactly as pointsrc.c's funnel hands them out. `now_ms` is
 * the same monotonic clock prompt_open() took, and it is here because the hold is a
 * duration: a press has to be stamped where it lands.
 *
 * WHILE THE BOX IS UP IT OWNS EVERY REPORT, wherever it lands -- the same rule
 * menu_zone.c applies to an open band, and for the same reason: a press that
 * dismisses must not also press whatever is underneath it. The return is therefore
 * MZ_FEED_TAKEN for everything while open, and MZ_FEED_NONE the moment it is shut --
 * so rbp's stream is untouched everywhere else, by construction and not by a
 * rectangle test. A release with no press behind it is taken too but does NOT close
 * the box: nothing was dismissed, so there is nothing to dismiss.
 *
 * THE PRESS IS ANCHORED, as it always was: the cell that matters is the cell the press
 * STARTED on, and it stops mattering the moment the finger leaves it. A hold is a
 * finger held still on one button; a finger that slides off -- even back on again -- is
 * a different gesture, and the hold it was running is cancelled. It is not restarted
 * either: the hold is one continuous press or it is nothing.
 *
 * WHAT A RELEASE DOES DEPENDS ON WHERE THE PRESS BEGAN, and that is the whole of the
 * new gesture:
 *
 *   ON A LIVE BUTTON, after PR_HOLD_MS  the eject: `*act` names the device's own
 *                                       channel and the box closes. The release
 *                                       normally finds the hold already fired on the
 *                                       tick that crossed the threshold; this path is
 *                                       here so that a release arriving with no tick in
 *                                       between still ejects, which is what makes the
 *                                       three seconds a property of the clock and not
 *                                       of the caller's slicing.
 *   ON A LIVE BUTTON, before it         NOTHING is sent and the box STAYS UP, so the
 *                                       operator can take a fresh hold or wait for
 *                                       the timeout. This is the operator's "if they
 *                                       release it before three seconds don't eject
 *                                       it", and it is the one release in the module
 *                                       that leaves the box standing.
 *   ON A DIM BUTTON                    nothing at all, ever: that cell cannot start a
 *                                       hold (prompt_tick() refuses the device rbp
 *                                       reports gone), and its release dismisses
 *                                       nothing.
 *   OFF EVERY BUTTON -- the title, the rule, the gap, the margin, the glass --
 *                                     dismisses the box and answers PR_ACT_NONE. That
 *                                     is the whole of "tap outside to dismiss", with
 *                                     no second rule for it, and it is the way out
 *                                     that took CANCEL's place. */
int prompt_feed(const struct prompt_state *S, int down, int x, int y,
                unsigned long long now_ms, int *act);

#endif /* RBPI4B_PROMPT_ZONE_H */
