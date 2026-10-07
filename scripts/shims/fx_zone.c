/*
 * fx_zone.c -- the BEAT FX panel's three rects, the picker's gesture and the power
 * toggle.
 *
 * See fx_zone.h for what all three are for, where the rectangles come from and why this
 * is pure. Nothing here reaches rbp, the framebuffer or a clock; the whole of the module
 * is the three rects, the picker's box, the latches and the arithmetic that turns a press
 * into an answer.
 *
 * THE THREE INTERACTIONS LIVE IN ONE MODULE ON PURPOSE, exactly as prompt_zone.c holds
 * both a chooser's geometry and its state machine: the picker is raised BY the zone's own
 * tap on the header bar, the power toggle is the zone's own tap on the effect-name cell,
 * and all three are asked from adjacent rungs of the same ladder in pointsrc.c. Splitting
 * them would put "which report is mine" in one file and "what to do with it" in another
 * for no gain.
 */
#include "fx_zone.h"

/* ---------------------------------------------------------------------------
 * THE FOURTEEN NAMES, IN SWITCH-POSITION ORDER. MEASURED, 2026-10-06.
 *
 * This table is not a derivation and the header says so: rbp's own panel was read
 * fourteen times. The FLX4's FX SELECT note (`seqinject2 note 4 99 on/off`, the note
 * map_flx4.c:511 handles) was injected through the shim's own sequencer port from a
 * cold start at position 0, the shim logged each position as it sent it, and the word
 * in rbp's effect-name cell was read off the glass after every step:
 *
 *     pos   0        1     2          3       4       5       6
 *           DELAY    ECHO  PING PONG  SPIRAL  HELIX   REVERB  FLANGER
 *     pos   7        8       9       10     11         12      13
 *           PHASER   FILTER  TRANS   ROLL   SLIP ROLL  PITCH   VINYL BRAKE
 *
 * THREE INDEPENDENT ANCHORS AGREE, which is why this is trusted and the earlier
 * composite derivation is not: rbp_abi.h:381's own ABI comment says "0=Delay, 1=Echo";
 * position 11 reads SLIP ROLL, the effect the panel was already showing; and position 5
 * reads REVERB, which is where the old `pos -> type -> icon resource` chain had put
 * position 0. That chain scrambled, and reading rbp's display is the only thing that
 * settled it.
 *
 * IT IS DATA. If the operator finds the order unfamiliar on the glass, the same
 * fourteen rows presented alphabetically is a one-line change here and nothing else --
 * the value SENT is the position, and the position does not move.
 */
static const char *const fx_names[FXLIST_ROWS] = {
    "DELAY", "ECHO", "PING PONG", "SPIRAL", "HELIX", "REVERB", "FLANGER",
    "PHASER", "FILTER", "TRANS", "ROLL", "SLIP ROLL", "PITCH", "VINYL BRAKE"
};

/* ---------------------------------------------------------------------------
 * The zone's own state. `was_down` is the edge detector every feeder in this shim
 * carries (touch_zone.c:60-70's shape), `press_hit` is the anchor for the release-fired
 * controls and `cur_hit` is what nothing paints yet but what the release is checked
 * against. The effect-name cell is the exception: it fires on its PRESS and its release
 * only clears the anchor.
 *
 * The touch thread is the only writer, exactly as it is for menu_zone.c's panel and
 * prompt_zone.c's box.
 * ------------------------------------------------------------------------- */
static int was_down;
static int press_hit;
static int cur_hit;

/* The picker's state, prompt_zone.c's six fields verbatim and for the same reasons:
 * `press_row` anchors the fire, `cur_row` is the highlight, and `deadline_ms` is the
 * caller's clock. There is no arming here -- a row IS the answer, so unlike the USB
 * chooser there is no second tap to wait for. */
static int list_was_down;
static int list_open_state;
static int press_row;
static int cur_row;
static unsigned long long list_deadline_ms;

/* The clock is the caller's and it is 64 bits wide on purpose: `long` is four bytes on
 * this target and a monotonic millisecond count in one wraps every 49.7 days, which is
 * the trap pointsrc.c's own shim_now_ms() documents. Every comparison below is written
 * so it does not care where the two values sit on the ring. */
static int list_expired(unsigned long long now_ms)
{
    return (long long)(now_ms - list_deadline_ms) >= 0;
}

/* ------------------------------------------------------------------- the rects */

static int in_rect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

int fx_hit(int x, int y)
{
    /* A fixed order, so the answer for a point is the same on every call -- which is
     * what the release's own-coordinates rule depends on. The three rects are separated
     * by plate and cannot overlap, so the order changes no answer. */
    if (in_rect(x, y, FX_HEADER_X0, FX_HEADER_Y0, FX_HEADER_X1, FX_HEADER_Y1))
        return FX_HIT_HEADER;
    if (in_rect(x, y, FX_CH_X0, FX_CH_Y0, FX_CH_X1, FX_CH_Y1))
        return FX_HIT_CH;
    if (in_rect(x, y, FX_NAME_X0, FX_NAME_Y0, FX_NAME_X1, FX_NAME_Y1))
        return FX_HIT_NAME;
    return FX_HIT_NONE;
}

int fx_feed(int down, int x, int y, int *act)
{
    if (act)
        *act = FX_ACT_NONE;

    if (!down) {
        int hit;

        /* A release we never saw the press for is not ours. Unlike the picker this
         * module is not up over anything, so rbp may keep it. */
        if (!was_down || press_hit == FX_HIT_NONE) {
            was_down = 0;
            press_hit = 0;
            cur_hit = 0;
            return MZ_FEED_NONE;
        }
        was_down = 0;
        /* Fire on the release, and only for a press that began and ended on the SAME
         * rect -- see fx_zone.h. The rect is taken from the RELEASE's own coordinates
         * and not from `cur_hit` for the reason prompt_zone.c:250 gives: a fast flick
         * can lift at a moved position with no move report in between.
         *
         * THE NAME CELL ANSWERS NOTHING HERE: it fired on its press (below), so its
         * release has nothing left to do but be swallowed -- handing rbp a bare up would
         * be an event it never had a down for (declined-press-must-still-see-release). */
        hit = fx_hit(x, y);
        if (hit == press_hit && act)
            *act = (hit == FX_HIT_CH)     ? FX_ACT_CH
                 : (hit == FX_HIT_HEADER) ? FX_ACT_PICK
                 : FX_ACT_NONE;            /* FX_HIT_NAME: already fired */
        press_hit = 0;
        cur_hit = 0;
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        was_down = 1;
        press_hit = fx_hit(x, y);
        cur_hit = press_hit;
        /* THE EFFECT-NAME CELL FIRES HERE, ON THE UP->DOWN EDGE AND NOWHERE ELSE. A
         * power toggle is a button: it acts the moment the finger goes down, so the cell
         * is answered on the PRESS and its release answers nothing. This is the
         * operator's own answer of 2026-10-07 ("if i press the actual label of the beat
         * fx (eg delay) enable the beat fx and disable if i press it again") and the
         * same edge hc_zone.c's pads moved to. The header bar and the CH box keep the
         * fire-on-release anchor rule, which is also what stops a SLIDE from the header
         * onto this cell from toggling: the anchor is the header, and the release is
         * checked against it. */
        if (press_hit == FX_HIT_NAME && act)
            *act = FX_ACT_POWER;
        /* A press that began outside all three rects is NOT this module's. `press_hit`
         * stays FX_HIT_NONE for the WHOLE gesture -- it is not cleared until the release
         * -- so a finger that starts on the glass and then slides across the panel is
         * rbp's from its first report to its last. Adopting it on the slide would let a
         * drag that merely passed over the panel fire a control nobody aimed at, and
         * clearing it here instead and the NEXT move report would re-open the press at
         * wherever the finger had got to, which is the same mistake one report later. */
        return press_hit == FX_HIT_NONE ? MZ_FEED_NONE : MZ_FEED_TAKEN;
    }

    if (press_hit == FX_HIT_NONE)
        return MZ_FEED_NONE;       /* still a gesture that began elsewhere */

    /* Still the same press and it began inside a rect, so every report until the release
     * is ours -- including one that has slid off it, so the release lands on a feeder
     * that still knows a press is in flight (declined-press-must-still-see-release). A
     * run of downs fires NOTHING a second time: the press it belongs to already did. */
    cur_hit = fx_hit(x, y);
    return MZ_FEED_TAKEN;
}

void fx_reset(void)
{
    was_down = 0;
    press_hit = 0;
    cur_hit = 0;
}

/* -------------------------------------------------------------------- the picker */

int fxlist_row_at(int x, int y)
{
    int i;

    if (x < FXLIST_ROW_X0 || x > FXLIST_ROW_X1)
        return 0;
    for (i = 0; i < FXLIST_ROWS; i++) {
        if (y < FXLIST_ROW_Y0(i) || y > FXLIST_ROW_Y1(i))
            continue;
        return i + 1;
    }
    return 0;                      /* a gap, the plate's margin, or off the box */
}

void fxlist_row_rect(int row, int *x0, int *y0, int *x1, int *y1)
{
    if (row < 1 || row > FXLIST_ROWS) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = -1;
        if (y1) *y1 = -1;
        return;
    }
    if (x0) *x0 = FXLIST_ROW_X0;
    if (y0) *y0 = FXLIST_ROW_Y0(row - 1);
    if (x1) *x1 = FXLIST_ROW_X1;
    if (y1) *y1 = FXLIST_ROW_Y1(row - 1);
}

void fxlist_box_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = FX_X0;
    if (y0) *y0 = FX_Y0;
    if (x1) *x1 = FX_X1;
    if (y1) *y1 = FX_Y1;
}

const char *fxlist_row_label(int row)
{
    if (row < 1 || row > FXLIST_ROWS)
        return 0;
    return fx_names[row - 1];
}

void fxlist_open(unsigned long long now_ms)
{
    list_open_state = 1;
    list_deadline_ms = now_ms + FX_TIMEOUT_MS;
    /* No press is inherited: the tap that raised this fired on a release and is already
     * over, and a caller that raised it any other way must not adopt a finger it never
     * saw go down (prompt_zone.c's prompt_open() rule). Neither is a row. */
    list_was_down = 0;
    press_row = 0;
    cur_row = 0;
}

void fxlist_close(void)
{
    list_open_state = 0;
    list_was_down = 0;
    press_row = 0;
    cur_row = 0;
}

int fxlist_is_open(void)
{
    return list_open_state;
}

int fxlist_pressed(void)
{
    return list_open_state ? cur_row : 0;
}

unsigned long long fxlist_deadline(void)
{
    return list_deadline_ms;
}

int fxlist_expire(unsigned long long now_ms)
{
    /* NOT WHILE A FINGER IS DOWN -- prompt_zone.c:211's rule and the same consequence:
     * a press the box swallowed has been withheld from rbp for its whole life and rbp
     * is going to be given its release, so closing mid-press would hand that release
     * back as a report with no press behind it. Deferring costs nothing; the release
     * closes the box on its own path a moment later. */
    if (!list_open_state || list_was_down || !list_expired(now_ms))
        return 0;
    fxlist_close();
    return 1;
}

void fxlist_reset(void)
{
    fxlist_close();
    list_deadline_ms = 0;
}

int fxlist_feed(int down, int x, int y, int *act)
{
    if (act)
        *act = -1;

    if (!list_open_state)
        return MZ_FEED_NONE;      /* shut: this module answers for nothing at all */

    if (!down) {
        int row;

        if (!list_was_down)
            return MZ_FEED_TAKEN; /* a release we never saw the press for: still ours,
                                   * because the box is up and rbp must not hear
                                   * either edge of anything. */
        list_was_down = 0;
        row = fxlist_row_at(x, y);
        if (press_row && row == press_row && act)
            *act = press_row - 1; /* the position rbp's switch wants, 0..13 */
        /* Every release closes the box: a row answers, and a miss, a slide or a tap
         * outside answers nothing. That is the whole of "tap outside to dismiss", with
         * no second rule for it (prompt_zone.c:255's shape). */
        fxlist_close();
        return MZ_FEED_TAKEN;
    }

    if (!list_was_down) {
        list_was_down = 1;
        press_row = fxlist_row_at(x, y);
        cur_row = press_row;
        return MZ_FEED_TAKEN;
    }

    /* Still the same press: track which row the finger is over now. This is the
     * HIGHLIGHT and nothing else -- what fires is decided at the release. */
    cur_row = fxlist_row_at(x, y);
    return MZ_FEED_TAKEN;
}
