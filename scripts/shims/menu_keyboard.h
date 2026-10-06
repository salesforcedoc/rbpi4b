/*
 * menu_keyboard.h -- the popup keyboard's keys and geometry. PURE.
 *
 * menu_window.h's sibling, to the same rule: everything here is a function of the
 * key index and of the point handed in, never of what is on the glass, and nothing
 * touches a framebuffer, a file or a clock. test_keyboard.c pins the whole table on
 * a host with no vc4, no browser and no device.
 *
 * WHY IT IS UNIFORM. Ten columns by five rows, every cell the same size, because a
 * hit test that is a division cannot disagree with the marks the painter drew and an
 * operator aiming at a 44-px key with a fingertip is not served by a layout whose
 * cells are 3 px narrower in one row. The window is 1120 logical px wide, which is
 * 112 px per column exactly -- so the grid divides with nothing left over and no
 * cell is a remainder.
 *
 *     +----+----+----+----+----+----+----+----+----+----+
 *     | 1  | 2  | 3  | 4  | 5  | 6  | 7  | 8  | 9  | 0  |   digits
 *     +----+----+----+----+----+----+----+----+----+----+
 *     | q  | w  | e  | r  | t  | y  | u  | i  | o  | p  |
 *     +----+----+----+----+----+----+----+----+----+----+
 *     | a  | s  | d  | f  | g  | h  | j  | k  | l  | .  |
 *     +----+----+----+----+----+----+----+----+----+----+
 *     | z  | x  | c  | v  | b  | n  | m  | -  | _  | /  |
 *     +----+----+----+----+----+----+----+----+----+----+
 *     | :  | ?  | =  | &  | @  | #  |SPACE|SHIFT| DEL | OK |
 *     +----+----+----+----+----+----+----+----+----+----+
 *
 * THE CHARACTER KEYS ARE EXACTLY THE ATLAS'S ALPHABET, ONCE EACH. The forty-six
 * character keys are '0'-'9', 'a'-'z' and menu_font.h's MENU_FONT_PUNCT, and
 * test_keyboard.c asserts that set equality -- so a key that would draw a gap, and a
 * character that got lost from the table, are both host failures rather than
 * something found on the glass. bake_menu_font.py checks the same thing from the
 * other side at bake time.
 *
 * SHIFT IS ONE-SHOT, ON PURPOSE. Tapping it arms the next character key and that key
 * disarms it -- the phone convention, and the one shape of this that cannot be left
 * stuck on by an operator who forgets. A held or locked shift on a keyboard with no
 * key-up event (this one has a finger, not a switch) is a mode with no way out that
 * an operator can see.
 */
#ifndef RBLIVE4_MENU_KEYBOARD_H
#define RBLIVE4_MENU_KEYBOARD_H

#include "menu_window.h"    /* MW_W, MW_CONTENT_Y, MW_CONTENT_H */

#define MW_KB_COLS   10
#define MW_KB_ROWS    5
#define MW_KB_KEY_H  44
#define MW_KB_KEY_W  (MW_W / MW_KB_COLS)          /* 112 px, and it divides exactly */
#define MW_KB_H      (MW_KB_ROWS * MW_KB_KEY_H)   /* 220 */

/* The keyboard sits at the BOTTOM of the window's content area, over the page --
 * the popup metaphor the operator asked for ("a popup keyboard"), and what every
 * phone browser does. The page is not re-rendered smaller when it appears: the
 * bottom of the page is covered while it is up, exactly as it is on a phone. */
#define MW_KB_Y0  (MW_CONTENT_Y + MW_CONTENT_H - MW_KB_H)

/* What a key does. MK_CHAR covers the letters, the digits and the punctuation --
 * everything whose cap is one character; the four others are the keys whose cap is
 * a word. */
enum {
    MK_NONE = 0,
    MK_CHAR,
    MK_SPACE,
    MK_SHIFT,
    MK_BACK,
    MK_OK
};

struct menu_key {
    unsigned char act;      /* MK_* */
    char ch;                /* MK_CHAR/MK_SPACE: the character. Letters are stored
                             * LOWERCASE and shifted at cap time, so the table has
                             * one entry per letter rather than two. */
    const char *cap;        /* MK_SPACE/MK_SHIFT/MK_BACK/MK_OK: the word on the cap,
                             * drawn from the same atlas the panel's labels use. */
};

int  menu_keyboard_count(void);                 /* MW_KB_COLS * MW_KB_ROWS */
const struct menu_key *menu_keyboard_key(int i);

/* The whole keyboard's rect and one key's, in the menu's logical space -- the same
 * space menu_window.h's boxes are in, so the painter and the hit test ask one
 * function each rather than repeating the arithmetic. */
int  menu_keyboard_rect(int *x0, int *y0, int *x1, int *y1);
int  menu_keyboard_key_rect(int i, int *x0, int *y0, int *x1, int *y1);

/* The key index at (lx, ly), or -1 when the point is not on a key. It answers
 * WHETHER THE KEYBOARD IS UP as well: a hidden keyboard has no keys, so a point in
 * its rectangle reads -1 and the press falls through to the page underneath. */
int  menu_keyboard_hit(int lx, int ly);

/* Shown and hidden by menu_window.c, which owns the edit target; the state lives
 * here because the caps' shift depends on it. */
int  menu_keyboard_is_up(void);
void menu_keyboard_show(void);
void menu_keyboard_hide(void);

/* The one-shot shift. Read by the painter (for the SHIFT cap's highlight) and by
 * menu_window.c (for what a character key emits), and consumed by the character key
 * itself -- menu_keyboard_char() below clears it. */
int  menu_keyboard_shift(void);
void menu_keyboard_shift_set(int on);

/* The character key `i` would emit right now: the shifted letter when shift is armed
 * (and then shift is DISARMED), 0 for a key that emits no character. This is the
 * only place the shift is consumed, so a key can never emit a shifted character and
 * leave shift on. */
char menu_keyboard_char(int i);

/* What is painted on key `i`'s cap, NUL-terminated, in the caller's buffer of at
 * least 8 bytes: the character (shifted if it is a letter and shift is armed) for a
 * character key, the word for the four others, "" for a key index out of range. */
const char *menu_keyboard_cap(int i, char *buf, int n);

/* True when key `i` is a letter -- the painter uses it for nothing, and
 * test_keyboard.c uses it to assert the shift moves exactly the twenty-six. */
int  menu_keyboard_is_letter(int i);

#endif /* RBLIVE4_MENU_KEYBOARD_H */
