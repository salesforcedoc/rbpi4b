/*
 * prompt_zone.h -- the USB STOP chooser: its box, its three rows, its gesture.
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
 * So the column no longer stops anything: it raises this. What the box offers is
 * a device to stop and a way out, and the eject only happens on a second, aimed
 * tap.
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
 * SO A ROW CAN BE DEAD, and it is rbp that says so: `[mgr+0x88] == 2` is "media
 * present and ready", and it is the same test `onUsbStopKey`'s press branch makes
 * before it does anything at all. A row whose device rbp reports absent is drawn in
 * the dim ink and tapping it sends nothing -- it closes the box, which is what makes
 * a dead button honest rather than a button that silently does nothing.
 *
 * ON THIS PORT USB 2 IS ALWAYS DEAD, and that is stated rather than hidden:
 * `scripts/device/usb-watch.sh` mounts ONE device, at `/media/usb1/sda1`, and writes
 * one FIFO. rbp has the whole second path -- `/proc/udev_usb2`, `/media/usb2` in the
 * chroot -- so the row is real and would light up by itself if the host side ever
 * fed it; nothing here has to change for that. Until then the operator sees the
 * device rbp can actually see, which is the honest answer.
 *
 * THE BOX IS ITS OWN PLANE, at ITS OWN SIZE. It is not the band at another size and
 * it is not the band's slot: `drmband.c` holds the fd and master once and refcounts
 * them, so a fourth `drm_band_setup()` is a buffer and a plane (the two drawers
 * already coexist, measured at planes 127 and 138). The band is shut by the time
 * this opens -- a tap on a menu button closes the panel -- so the two are never on
 * the glass together anyway.
 *
 * PURE, LIKE menu_zone.c AND util_zone.c. Nothing here reaches rbp, the framebuffer
 * or a clock of its own: the time arrives as an argument and the two device-liveness
 * bits arrive in a struct prompt_state. That is what lets test_prompt.c pin the
 * geometry, the latch, the dead row and the timeout on a host with no Pi, no panel
 * and no player -- and it is why the caller, not this file, is the one that sends.
 */
#ifndef RBLIVE4_PROMPT_ZONE_H
#define RBLIVE4_PROMPT_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* ------------------------------------------------------------------ geometry
 *
 * All logical, in the menu's own 1280x800 space, and all derived from the box
 * rather than written down as a table. The box is centred on the screen; the
 * arithmetic below is what test_prompt.c checks adds up to PR_H exactly, so a
 * change to any row's height moves the ones under it instead of running off the
 * bottom.
 *
 *     +-----------------------------------------------------+  PR_Y0  (black bed)
 *     |  PR_BORDER                             (a margin)   |
 *     |           USB STOP                     (PR_TITLE_H) |
 *     |  ___________________________________________________|  the rule
 *     |  PR_PAD                                             |
 *     |  +-----------------------+   +----------------------+|
 *     |  |        USB 1          |   |        USB 2         ||  row 1
 *     |  +-----------------------+   +----------------------+|
 *     |  PR_ROW_GAP                                         |
 *     |  +-----------------------+   +----------------------+|
 *     |  |         OK            |   |       CANCEL         ||  row 2
 *     |  +-----------------------+   +----------------------+|
 *     |  PR_PAD                                             |
 *     |  PR_BORDER                                          |
 *     +-----------------------------------------------------+  PR_Y1
 *
 * WHY TWO LINES AND NOT THREE, at the operator's ask of 2026-10-06:
 *
 *     "usb stop should be have usb 1 button next to usb 2 button on one line
 *      with ok cancel at the bottom"
 *
 * WHICH IS A CHANGE OF MEANING, not only of layout, and the two arrived together:
 * a row used to be an ANSWER, and a tap on `USB 1` ejected the stick on the spot.
 * With an OK under the devices, the devices are a CHOICE and OK is the answer -- so
 * a tap on `USB 1` now only ARMS it and the eject needs the second, deliberate tap
 * on OK. The operator's own safety ask of 2026-10-05 ("put up a prompt for USB1,
 * USB2 or Cancel") is intact and, if anything, better served: a mis-tap on the far
 * right of the band can no longer reach an eject in one more tap, because a tap on a
 * device is no longer an eject at all.
 *
 * NOTHING IS ARMED WHEN THE BOX OPENS. OK is drawn dead -- the palette's OFF pair --
 * until a live device has been chosen, and a tap on a dead OK closes the box and
 * sends nothing, exactly as a tap on a dead device does. That is the rule this box
 * already kept for the absent USB 2 (prompt_zone.c's row_act), extended to the one
 * other button that can have nothing to do.
 */
#define PROMPT_ROWS     2       /* the devices, then the answers */
#define PROMPT_COLS     2       /* left: a device / OK; right: a device / CANCEL */
#define PROMPT_CELLS    (PROMPT_ROWS * PROMPT_COLS)

/* The devices rbp can be asked to stop, which is NOT PROMPT_ROWS: they are what the
 * first line of the box offers, and the second line is the answers. pointsrc.c sizes
 * its walk of rbp's UsbStorageManager array by this, so it is the one number that
 * must not be derived from the button count. */
#define PROMPT_DEVICES  2

/* The four cells, 1-based and ROW-MAJOR, so PR_CELL_ROW/COL below are pure
 * arithmetic and the painter's and the hit test's loops are the same loop. */
#define PR_CELL_USB1    1       /* row 1 left   */
#define PR_CELL_USB2    2       /* row 1 right  */
#define PR_CELL_OK      3       /* row 2 left   */
#define PR_CELL_CANCEL  4       /* row 2 right  */

#define PR_W        560
#define PR_H        199         /* 2*3 + 40 + 1 + 2*8 + 2*64 + 1*8, exactly */
#define PR_X0       ((MZ_LOGICAL_W - PR_W) / 2)          /* 360 */
#define PR_Y0       ((MZ_LOGICAL_H - PR_H) / 2)          /* 300 */
#define PR_X1       (PR_X0 + PR_W - 1)                   /* 919 */
#define PR_Y1       (PR_Y0 + PR_H - 1)                   /* 498 */

/* The box's margin, inside it on all four sides. It was the frame's thickness until
 * 2026-10-06, when the operator asked for the white to go round each ROW rather than
 * round the whole box (prompt_paint.c's header); the number did not move, because it
 * was always doing both jobs -- keeping the title and the rows off the box's edge.
 * Nothing draws a mark here any more: the box's own edge is the black bed. */
#define PR_BORDER   3
#define PR_PAD      8           /* between the frame and what it holds */
#define PR_TITLE_H  40
#define PR_ROW_H    64
#define PR_ROW_GAP  8

/* The two-line block's own rect: both rows span this, and the two columns divide it.
 * The gap between the columns is PR_COL_GAP, the same number the rows are separated
 * by, so the four buttons and the space between them read as one grid. */
#define PR_ROW_X0   (PR_X0 + PR_BORDER + PR_PAD)          /* 371 */
#define PR_ROW_X1   (PR_X1 - PR_BORDER - PR_PAD)          /* 908 */
#define PR_COL_GAP  8

/* One button: an exact half of the block less half the gap. The odd pixel a 538-wide
 * block leaves over is dropped rather than handed to one column, so the two buttons
 * are the same width -- PR_ROW_X1 - PR_ROW_X0 + 1 - PR_COL_GAP is even here (530), and
 * test_prompt.c asserts the leftover is 0, which is what makes the two columns
 * interchangeable rather than one of them a pixel wider. */
#define PR_CELL_W   ((PR_ROW_X1 - PR_ROW_X0 + 1 - PR_COL_GAP) / PROMPT_COLS)   /* 265 */
#define PR_CELL_X0(c) (PR_ROW_X0 + (PR_CELL_COL(c) - 1) * (PR_CELL_W + PR_COL_GAP))
#define PR_CELL_X1(c) (PR_CELL_X0(c) + PR_CELL_W - 1)

/* The title's band and the rule under it. The title spans the whole inner width
 * rather than PR_ROW_X0..PR_ROW_X1: it is centred on the BOX, which is what makes
 * it read as the box's caption and not as a fourth row. */
#define PR_TITLE_Y0 (PR_Y0 + PR_BORDER)
#define PR_TITLE_Y1 (PR_TITLE_Y0 + PR_TITLE_H - 1)
#define PR_RULE_Y   (PR_TITLE_Y1 + 1)

/* Row i (0-based) top -- the arithmetic in one place so the painter, the hit test and
 * the test cannot disagree about where a row is. */
#define PR_ROW_TOP(i) (PR_RULE_Y + 1 + PR_PAD + (i) * (PR_ROW_H + PR_ROW_GAP))

/* The cell number's own row and column, 1-based, in the order the enum above lists
 * them. These two are the whole of "which cell is where": everything else is derived,
 * so a fifth cell (a third device, say) would be a constant and nothing else. */
#define PR_CELL_ROW(c) (((c) - 1) / PROMPT_COLS + 1)
#define PR_CELL_COL(c) (((c) - 1) % PROMPT_COLS + 1)

/* Cell `c`'s inclusive rect -- the same rect prompt_cell_at() tests, so the ink and
 * the hit target are the same pixels. */
#define PR_CELL_Y0(c)  PR_ROW_TOP(PR_CELL_ROW(c) - 1)
#define PR_CELL_Y1(c)  (PR_CELL_Y0(c) + PR_ROW_H - 1)

/* The box goes away on its own. The operator asked for a prompt, not for a modal
 * they have to escape: a prompt left on the glass covers the middle of the
 * performance screen, and nothing about the eject is urgent enough to be worth
 * that. Ten seconds is long enough to read four buttons and decide. */
#define PR_TIMEOUT_MS 10000

/* ------------------------------------------------------------- the answers */
enum {
    PR_ACT_NONE = 0,        /* no answer: the press missed, slid off, or the button
                             * asked had nothing to do */
    PR_ACT_USB1,            /* OK, with USB 1 armed */
    PR_ACT_USB2,            /* OK, with USB 2 armed */
    PR_ACT_CANCEL
};

/* What rbp says about the two devices, filled by the caller from its
 * UsbStorageManager objects. `live[i]` is "device i+1 has media present", i.e.
 * `[mgr+0x88] == 2`; a caller that cannot read rbp leaves both 0, which draws both
 * device buttons dim and sends nothing -- a refusal, not an assumption. */
struct prompt_state {
    int live[PROMPT_DEVICES];
};

/* ---------------------------------------------------------------- the state */

/* Raise the box, or re-raise it if it is already up (which restarts the clock).
 * `now_ms` is the caller's monotonic milliseconds; the module keeps no clock of
 * its own. A press in flight is NOT inherited: the box always starts with no press
 * behind it, so the tap that raised it is already over. Neither is an ARMING: a box
 * that has been away comes back with both devices unarmed and OK drawn dead, so
 * nothing the operator chose ten minutes ago can be what the next OK stops. */
void prompt_open(unsigned long long now_ms);

/* Put it away. Safe on a shut box, and safe from the tick as well as from the
 * touch thread -- see prompt_expire(). */
void prompt_close(void);

int prompt_is_open(void);

/* The wall-clock milliseconds at which the box closes itself. Only meaningful
 * while prompt_is_open(). */
unsigned long long prompt_deadline(void);

/* The self-dismissal, asked from wherever the clock is watched. Returns 1 on the
 * call that closed it and 0 otherwise, so a caller can log the transition once.
 *
 * IT IS NOT ONLY THE TOUCH THREAD THAT ASKS. The box has to be able to go away with
 * nobody touching anything, and pointsrc.c's read loop otherwise wakes only on an
 * event -- so pointsrc.c waits in slices while the box is up, exactly as it already
 * does for a held menu key, and asks this at each one. That keeps ONE writer of this
 * state (the touch thread) and needs no lock: menu_draw.c only ever READS it, the
 * same way it reads menu_is_open(). */
int prompt_expire(unsigned long long now_ms);

/* Drop everything, including a press in flight. pointsrc.c calls this when the
 * pointer device goes away, beside menu_reset() and the rest. */
void prompt_reset(void);

/* ---------------------------------------------------------------- the input */

/* The cell (1..PROMPT_CELLS) under a logical point, or 0 for anything else -- the
 * title, the gap between two buttons, the margin, or off the box entirely. */
int prompt_cell_at(int x, int y);

/* The cell the finger is on right now, or 0 -- what the painter highlights, and the
 * same question menu_pressed() answers for the band. It is the LIVE cell and not the
 * anchored one on purpose: a finger that has slid to another cell is highlighting
 * where it is, while what it will answer is still where it started (prompt_feed()'s
 * fire-on-release rule). */
int prompt_pressed(void);

/* The device cell the operator has ARMED, 1 or 2, or 0 for none -- what the painter
 * draws in the pressed palette so the operator can see which stick OK would stop. It
 * is a question about this module's own state, so unlike prompt_cell_live() it takes
 * no struct prompt_state. Stays armed while the box is up, and is cleared by
 * prompt_close() with everything else: reopening the box always starts unarmed. */
int prompt_selected(void);

/* Has this cell anything to do? CANCEL always has. The two device cells are rbp's
 * `live[]`; OK has nothing to do until a LIVE device is armed, which is what leaves
 * it drawn dead on a freshly opened box. Out of range answers 0 as well, so a caller
 * cannot read past the array. */
int prompt_cell_live(const struct prompt_state *S, int cell);

/* The cell's rect, logical, inclusive at both ends -- the same rect prompt_cell_at()
 * tests, so the ink and the hit target are the same pixels. */
void prompt_cell_rect(int cell, int *x0, int *y0, int *x1, int *y1);

/* The box's own rect, logical, inclusive at both ends. */
void prompt_box_rect(int *x0, int *y0, int *x1, int *y1);

/* The four label strings, published for the painter and for the test that checks
 * every character is in the font's set (menu_font.h draws a gap for one that is
 * not, which would ship as a dead-looking label rather than as a failure). */
const char *prompt_title(void);
const char *prompt_cell_label(int cell);

/* Which channel rbp's send wants for an answer: the device's own number, which is
 * `[mgr+0x84]` -- 1 for USB 1 and 2 for USB 2. PR_ACT_CANCEL and PR_ACT_NONE send
 * nothing and answer 0. */
int prompt_act_channel(int act);

/* Feed one pointer report, exactly as pointsrc.c's funnel hands them out.
 *
 * WHILE THE BOX IS UP IT OWNS EVERY REPORT, wherever it lands -- the same rule
 * menu_zone.c applies to an open band, and for the same reason: a press that
 * dismisses must not also press whatever is underneath it. The return is therefore
 * MZ_FEED_TAKEN for everything while open, and MZ_FEED_NONE the moment it is shut --
 * so rbp's stream is untouched everywhere else, by construction and not by a
 * rectangle test. A release with no press behind it is taken too but does NOT close
 * the box: nothing was dismissed, so there is nothing to dismiss.
 *
 * A RELEASE ANSWERS FOR THE CELL THE PRESS STARTED ON, AND ONLY IF THE RELEASE IS
 * STILL ON IT, with the cell taken from the RELEASE's own coordinates and not from
 * `cur_cell`: a fast flick can lift at a moved position with no move report in
 * between, and a release that moved must not answer for the cell the finger has
 * already left. `press_cell` is 0 for a press that began off a cell -- the title, the
 * gap, the margin -- so a miss can never answer.
 *
 * WHAT A QUALIFYING RELEASE THEN DOES DEPENDS ON WHICH CELL IT WAS, and the two
 * halves are the whole of the change this file's header describes:
 *
 *   a DEVICE cell   ARMS it and the box STAYS UP. This is the one release that does
 *                   not put the box away, because it is the middle of a two-tap
 *                   gesture rather than the end of one. A dead device cell has
 *                   nothing to arm, so it closes the box and answers NOTHING --
 *                   the same honest dead button this box already had.
 *   OK              sets `*act` to the armed device's answer and closes the box. Dead
 *                   when nothing live is armed, and then it closes and answers
 *                   nothing, exactly like a dead device cell.
 *   CANCEL          answers PR_ACT_CANCEL and closes the box. Always live.
 *
 * Every other release -- a miss, a slide off, the title, a dead cell -- puts the box
 * away and answers PR_ACT_NONE. That is the whole of "tap outside to dismiss". */
int prompt_feed(const struct prompt_state *S, int down, int x, int y, int *act);

#endif /* RBLIVE4_PROMPT_ZONE_H */
