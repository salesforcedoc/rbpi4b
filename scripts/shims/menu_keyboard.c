/*
 * menu_keyboard.c -- the key table and the geometry. See menu_keyboard.h.
 *
 * One writer, no lock: the touch thread writes the shift and the shown flag, the
 * paint thread reads them, and each is a single int. menu_zone.c's rule.
 */
#include "menu_keyboard.h"

/* The table, in reading order: five rows of ten. The forty-six character keys are
 * the alphabet once each and are asserted to be in test_keyboard.c rather than
 * trusted here -- a typo in one of these lines is a key that draws a gap, and a
 * duplicated character is a character the operator cannot type. */
#define K_(c)      { MK_CHAR, (c), 0 }
#define KS_(w)     { MK_SPACE, ' ', (w) }
#define KSH_       { MK_SHIFT, 0, "SHIFT" }
#define KB_        { MK_BACK,  0, "DEL" }
#define KOK_       { MK_OK,    0, "OK" }

static const struct menu_key keys[MW_KB_COLS * MW_KB_ROWS] = {
    K_('1'), K_('2'), K_('3'), K_('4'), K_('5'),
    K_('6'), K_('7'), K_('8'), K_('9'), K_('0'),

    K_('q'), K_('w'), K_('e'), K_('r'), K_('t'),
    K_('y'), K_('u'), K_('i'), K_('o'), K_('p'),

    K_('a'), K_('s'), K_('d'), K_('f'), K_('g'),
    K_('h'), K_('j'), K_('k'), K_('l'), K_('.'),

    K_('z'), K_('x'), K_('c'), K_('v'), K_('b'),
    K_('n'), K_('m'), K_('-'), K_('_'), K_('/'),

    K_(':'), K_('?'), K_('='), K_('&'), K_('@'),
    K_('#'), KS_("SPACE"), KSH_, KB_, KOK_
};

#undef K_
#undef KS_
#undef KSH_
#undef KB_
#undef KOK_

static int kb_up;
static int kb_shift;

int menu_keyboard_count(void)
{
    return MW_KB_COLS * MW_KB_ROWS;
}

const struct menu_key *menu_keyboard_key(int i)
{
    if (i < 0 || i >= menu_keyboard_count())
        return NULL;
    return &keys[i];
}

int menu_keyboard_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = MW_X;
    if (y0) *y0 = MW_Y + MW_KB_Y0;
    if (x1) *x1 = MW_X + MW_KB_COLS * MW_KB_KEY_W;
    if (y1) *y1 = MW_Y + MW_KB_Y0 + MW_KB_H;
    return 1;
}

int menu_keyboard_key_rect(int i, int *x0, int *y0, int *x1, int *y1)
{
    int col, row, x, y;

    if (i < 0 || i >= menu_keyboard_count())
        return 0;
    col = i % MW_KB_COLS;
    row = i / MW_KB_COLS;
    x = MW_X + col * MW_KB_KEY_W;
    y = MW_Y + MW_KB_Y0 + row * MW_KB_KEY_H;
    if (x0) *x0 = x;
    if (y0) *y0 = y;
    if (x1) *x1 = x + MW_KB_KEY_W;
    if (y1) *y1 = y + MW_KB_KEY_H;
    return 1;
}

int menu_keyboard_hit(int lx, int ly)
{
    int x0, y0, x1, y1, col, row;

    if (!kb_up)
        return -1;              /* a hidden keyboard has no keys */
    menu_keyboard_rect(&x0, &y0, &x1, &y1);
    if (lx < x0 || lx >= x1 || ly < y0 || ly >= y1)
        return -1;
    col = (lx - x0) / MW_KB_KEY_W;
    row = (ly - y0) / MW_KB_KEY_H;
    /* The rect is an exact multiple of the cell, so this cannot overshoot; the
     * clamp is here because a division that is correct by arithmetic is one edit
     * away from being out of range, and this function's answer indexes the table. */
    if (col >= MW_KB_COLS) col = MW_KB_COLS - 1;
    if (row >= MW_KB_ROWS) row = MW_KB_ROWS - 1;
    return row * MW_KB_COLS + col;
}

int menu_keyboard_is_up(void)
{
    return kb_up;
}

void menu_keyboard_show(void)
{
    kb_up = 1;
}

void menu_keyboard_hide(void)
{
    kb_up = 0;
    kb_shift = 0;       /* a hidden keyboard is not holding a shift for the next
                         * time it appears -- there is no way to see the state while
                         * it is down, so it must not survive it */
}

int menu_keyboard_shift(void)
{
    return kb_shift;
}

void menu_keyboard_shift_set(int on)
{
    kb_shift = on ? 1 : 0;
}

int menu_keyboard_is_letter(int i)
{
    const struct menu_key *k = menu_keyboard_key(i);

    return k && k->act == MK_CHAR && k->ch >= 'a' && k->ch <= 'z';
}

char menu_keyboard_char(int i)
{
    const struct menu_key *k = menu_keyboard_key(i);
    char c;

    if (!k)
        return 0;
    if (k->act == MK_SPACE)
        return ' ';
    if (k->act != MK_CHAR)
        return 0;
    c = k->ch;
    if (c >= 'a' && c <= 'z') {
        /* The one place the shift is consumed. A letter typed with the shift armed
         * is the capital AND disarms it, in that order, so the two cannot come
         * apart. */
        if (kb_shift) {
            c = (char)(c - 'a' + 'A');
            kb_shift = 0;
        }
    }
    return c;
}

const char *menu_keyboard_cap(int i, char *buf, int n)
{
    const struct menu_key *k = menu_keyboard_key(i);
    char c;

    if (!buf || n < 2)
        return "";
    buf[0] = '\0';
    if (!k)
        return buf;
    if (k->cap) {
        int j = 0;

        while (k->cap[j] && j < n - 1) {
            buf[j] = k->cap[j];
            j++;
        }
        buf[j] = '\0';
        return buf;
    }
    /* A character key. The letter's capital comes from the shift WITHOUT consuming
     * it -- the painter must show what the next tap will type, and a cap that read
     * the shift by consuming it would disarm it just by being drawn. */
    c = k->ch;
    if (c >= 'a' && c <= 'z' && kb_shift)
        c = (char)(c - 'a' + 'A');
    buf[0] = c;
    buf[1] = '\0';
    return buf;
}
