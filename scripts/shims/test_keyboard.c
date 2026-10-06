/*
 * test_keyboard.c -- the popup keyboard's unit test: no Pi, no device, no browser.
 *
 * It links the PRODUCTION menu_keyboard.c, the same rule test_menu.c and
 * test_window.c are written to: the table pinned here is exactly the kind that gets
 * quietly edited in a copy, and a key whose cap draws a gap or whose character is
 * missing is a fault that only shows itself under a fingertip.
 *
 * Four things are pinned.
 *
 * 1. THE TABLE. The forty-six character keys are EXACTLY the atlas's keyboard
 *    alphabet -- '0'-'9', 'a'-'z' and menu_font.h's MENU_FONT_PUNCT -- once each.
 *    Set equality both ways is the whole point: a character that got lost from the
 *    table is a character the operator cannot type, a duplicate is a key that only
 *    exists to be pressed twice, and a key whose character has no glyph in
 *    menu_font.h draws an empty cap that looks like a broken key rather than a
 *    missing one. Every cap -- the four words included -- is checked against the
 *    atlas for the same reason.
 *
 * 2. THE GEOMETRY. Ten columns by five rows at a fixed 112x44, and the grid has to
 *    tile the keyboard's rect with no gap (a gap is a dead seam under the finger)
 *    and no overlap (an overlap makes the hit test depend on which check ran first).
 *    Both edges are checked one pixel either side, because an off-by-one here moves
 *    every key and nothing on the glass would ever say so.
 *
 * 3. THE HIT TEST, which answers WHETHER THE KEYBOARD IS UP as well as where the
 *    finger is. A hidden keyboard must read -1 for a point that is dead centre of a
 *    key, because that is precisely the case where the press has to fall through to
 *    the page underneath -- get it wrong and a keyboard that has just been dismissed
 *    swallows the next tap on the page behind it.
 *
 * 4. THE ONE-SHOT SHIFT, and the two halves of it that must not come apart:
 *    menu_keyboard_char() consumes the shift, menu_keyboard_cap() does not. A cap
 *    that consumed it would disarm the shift merely by being DRAWN -- thirty times a
 *    second -- so the key would never type a capital; a char that did not consume it
 *    would leave shift stuck on with nothing on the glass to say so.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_keyboard test_keyboard.c
 *            menu_keyboard.c   &&   qemu-arm ./test_keyboard
 */
#define _GNU_SOURCE
#include "menu_keyboard.h"
#include "menu_font.h"

#include <stdio.h>
#include <string.h>

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

/* The alphabet the keyboard's character keys must be, exactly: the ten digits, the
 * twenty-six letters and the atlas's punctuation run. Built rather than written out
 * so the punctuation half comes from menu_font.h -- the same constant the table and
 * bake_menu_font.py both name, so the three cannot drift into three lists that agree
 * today. */
static const char *alphabet(void)
{
    static char buf[128];
    static int built;
    int i, n = 0;

    if (built)
        return buf;
    for (i = 0; i < 10; i++)
        buf[n++] = (char)('0' + i);
    for (i = 0; i < 26; i++)
        buf[n++] = (char)('a' + i);
    for (i = 0; MENU_FONT_PUNCT[i]; i++)
        buf[n++] = MENU_FONT_PUNCT[i];
    buf[n] = '\0';
    built = 1;
    return buf;
}

static int in_alphabet(char c)
{
    return strchr(alphabet(), c) != NULL;
}

/* A character key's cap must have a glyph, or the painter draws nothing where the
 * operator is aiming. Shared by the character check and the four words below. */
static int cap_draws(const char *s)
{
    int i;

    for (i = 0; s[i]; i++)
        if (menu_font_index((unsigned char)s[i]) < 0)
            return 0;
    return 1;
}

static void test_table(void)
{
    const char *alpha = alphabet();
    int seen[128];
    int i, chars = 0, specials = 0, letters = 0;
    int n = menu_keyboard_count();

    memset(seen, 0, sizeof seen);

    CHECK(n == MW_KB_COLS * MW_KB_ROWS,
          "the table is %d keys, not %d columns x %d rows", n, MW_KB_COLS,
          MW_KB_ROWS);
    CHECK(n == MW_KB_COLS * MW_KB_ROWS, "the keyboard is not a whole grid");

    for (i = 0; i < n; i++) {
        const struct menu_key *k = menu_keyboard_key(i);

        CHECK(k != NULL, "key %d does not exist", i);
        if (!k)
            continue;
        CHECK(k->act != MK_NONE, "key %d has no action", i);

        if (k->act == MK_CHAR) {
            chars++;
            /* LETTERS ARE STORED LOWERCASE -- one table entry per letter, shifted at
             * cap time, so the shift cannot be lost by a table that carries both
             * cases. The digits and the punctuation have one case and are stored as
             * they are typed. */
            if (k->ch >= 'A' && k->ch <= 'Z')
                CHECK(0, "character key %d stores the capital '%c' -- letters are "
                      "stored LOWERCASE and shifted at cap time", i, k->ch);
            else
                CHECK(1, "a character key is not a capital");
            CHECK(in_alphabet(k->ch),
                  "character key %d is '%c', which the atlas's alphabet does not "
                  "have -- this key draws a gap or is a duplicate", i, k->ch);
            CHECK((unsigned char)k->ch < 128 && seen[(unsigned char)k->ch] == 0,
                  "character key %d is '%c' again -- a character typed twice",
                  i, k->ch);
            if ((unsigned char)k->ch < 128)
                seen[(unsigned char)k->ch] = 1;
            CHECK(k->cap == NULL,
                  "character key %d has both a character and a word", i);
        } else {
            specials++;
            CHECK(k->cap != NULL, "special key %d has no cap word", i);
            if (k->cap) {
                CHECK(cap_draws(k->cap),
                      "special key %d's cap '%s' has a character the atlas does "
                      "not carry -- it would draw a gap", i, k->cap);
                CHECK(strlen(k->cap) > 0 && strlen(k->cap) < 8,
                      "special key %d's cap '%s' is not a short word", i, k->cap);
            }
        }
    }

    /* The set equality, both ways. Every character in the alphabet is a key... */
    for (i = 0; alpha[i]; i++)
        CHECK((unsigned char)alpha[i] < 128 && seen[(unsigned char)alpha[i]],
              "the atlas carries '%c' but no key types it", alpha[i]);
    /* ...and every character key is in the alphabet, which the per-key check above
     * already made -- this is the count that would catch a table with a character
     * key that in_alphabet() and seen[] both let through by arithmetic accident. */
    CHECK((size_t)chars == strlen(alpha),
          "%d character keys for a %d-character alphabet", chars,
          (int)strlen(alpha));
    CHECK(specials == 4, "%d special keys, not SPACE/SHIFT/DEL/OK", specials);

    /* The four specials, named and placed -- the bottom row's last four cells, in
     * the order the layout draws them. A reorder here is a tap on DEL that deletes
     * nothing and a tap on OK that deletes a character. */
    {
        const struct menu_key *s;
        int base = MW_KB_COLS * (MW_KB_ROWS - 1) + 6;

        s = menu_keyboard_key(base);
        CHECK(s && s->act == MK_SPACE && s->ch == ' ',
              "the bottom row's 7th cell is not SPACE");
        s = menu_keyboard_key(base + 1);
        CHECK(s && s->act == MK_SHIFT, "the bottom row's 8th cell is not SHIFT");
        s = menu_keyboard_key(base + 2);
        CHECK(s && s->act == MK_BACK && s->cap &&
              strcmp(s->cap, "DEL") == 0,
              "the bottom row's 9th cell is not DEL");
        s = menu_keyboard_key(base + 3);
        CHECK(s && s->act == MK_OK && s->cap && strcmp(s->cap, "OK") == 0,
              "the bottom row's 10th cell is not OK");
    }

    /* The letters are exactly the twenty-six, and is_letter() says so. */
    for (i = 0; i < n; i++)
        letters += menu_keyboard_is_letter(i);
    CHECK(letters == 26, "is_letter() is true for %d keys, not 26", letters);
    for (i = 0; i < n; i++) {
        const struct menu_key *k = menu_keyboard_key(i);

        if (k && k->act == MK_CHAR && k->ch >= 'a' && k->ch <= 'z')
            CHECK(menu_keyboard_is_letter(i), "key %d is a letter and says not", i);
        else
            CHECK(!menu_keyboard_is_letter(i),
                  "key %d is not a letter and says it is", i);
    }

    /* Out of range, both ends: an index the table does not have is NULL, not a
     * neighbouring key -- this function's answer indexes memory. */
    CHECK(menu_keyboard_key(-1) == NULL, "key -1 exists");
    CHECK(menu_keyboard_key(n) == NULL, "key %d exists", n);
}

static void test_geometry(void)
{
    int kx0, ky0, kx1, ky1, x0, y0, x1, y1;
    int i, row, col;

    CHECK(menu_keyboard_rect(&kx0, &ky0, &kx1, &ky1), "no keyboard rect");
    CHECK(kx1 - kx0 == MW_W, "the keyboard is %d px of a %d px window",
          kx1 - kx0, MW_W);
    CHECK(ky1 - ky0 == MW_KB_H, "the keyboard is %d px tall, not %d",
          ky1 - ky0, MW_KB_H);
    CHECK(kx0 == MW_X, "the keyboard does not start at the window's left edge");
    /* The bottom of the keyboard is the bottom of the window, and the top of it is
     * inside the content area -- over the page, which is the popup the operator
     * asked for rather than a fourth band. */
    CHECK(ky1 == MW_Y + MW_H, "the keyboard does not reach the window's bottom");
    CHECK(ky0 >= MW_Y + MW_CONTENT_Y,
          "the keyboard starts above the page it is supposed to cover");
    CHECK(ky0 + MW_KB_H == MW_Y + MW_CONTENT_Y + MW_CONTENT_H,
          "MW_KB_Y0 and MW_KB_H do not put the keyboard at the bottom of the page");

    /* Every key's rect, and the tiling: column n's right edge is column n+1's left
     * edge, row n's bottom edge is row n+1's top edge, exactly. */
    for (i = 0; i < menu_keyboard_count(); i++) {
        CHECK(menu_keyboard_key_rect(i, &x0, &y0, &x1, &y1),
              "key %d has no rect", i);
        CHECK(x1 - x0 == MW_KB_KEY_W, "key %d is %d px wide, not %d", i,
              x1 - x0, MW_KB_KEY_W);
        CHECK(y1 - y0 == MW_KB_KEY_H, "key %d is %d px tall, not %d", i,
              y1 - y0, MW_KB_KEY_H);
        CHECK(x0 >= kx0 && x1 <= kx1 && y0 >= ky0 && y1 <= ky1,
              "key %d is outside the keyboard rect", i);

        col = i % MW_KB_COLS;
        row = i / MW_KB_COLS;
        if (col > 0) {
            int px0, py0, px1, py1;

            menu_keyboard_key_rect(i - 1, &px0, &py0, &px1, &py1);
            CHECK(x0 == px1, "key %d overlaps or leaves a seam to key %d", i, i - 1);
            CHECK(y0 == py0 && y1 == py1, "key %d is not level with key %d", i, i - 1);
        } else {
            CHECK(x0 == kx0, "key %d does not start at the keyboard's left edge", i);
        }
        if (row > 0) {
            int px0, py0, px1, py1;

            menu_keyboard_key_rect(i - MW_KB_COLS, &px0, &py0, &px1, &py1);
            CHECK(y0 == py1, "key %d overlaps or leaves a seam above it", i);
            CHECK(x0 == px0 && x1 == px1, "key %d is not in a column with the one "
                  "above it", i);
        } else {
            CHECK(y0 == ky0, "key %d does not start at the keyboard's top", i);
        }
    }

    /* Out of range: no rect, and nothing written. */
    CHECK(!menu_keyboard_key_rect(-1, &x0, &y0, &x1, &y1), "key -1 has a rect");
    CHECK(!menu_keyboard_key_rect(menu_keyboard_count(), &x0, &y0, &x1, &y1),
          "a key past the end has a rect");
}

static void test_hit(void)
{
    int kx0, ky0, kx1, ky1, x0, y0, x1, y1;
    int i;

    menu_keyboard_hide();
    CHECK(!menu_keyboard_is_up(), "the keyboard is up after hide()");
    menu_keyboard_rect(&kx0, &ky0, &kx1, &ky1);

    /* Hidden: a point dead centre of every key is not on the keyboard. This is the
     * case that matters -- the press has to reach the page underneath. */
    for (i = 0; i < menu_keyboard_count(); i++) {
        menu_keyboard_key_rect(i, &x0, &y0, &x1, &y1);
        CHECK(menu_keyboard_hit((x0 + x1) / 2, (y0 + y1) / 2) < 0,
              "a hidden keyboard answered a hit at key %d's centre", i);
    }

    menu_keyboard_show();
    CHECK(menu_keyboard_is_up(), "the keyboard is not up after show()");

    /* Shown: each key's two INSIDE corners are that key. The bottom-right corner is
     * the next key's top-left, so it is checked against the grid, not against i. */
    for (i = 0; i < menu_keyboard_count(); i++) {
        menu_keyboard_key_rect(i, &x0, &y0, &x1, &y1);
        CHECK(menu_keyboard_hit(x0, y0) == i,
              "key %d's top-left corner answered %d", i, menu_keyboard_hit(x0, y0));
        CHECK(menu_keyboard_hit(x1 - 1, y1 - 1) == i,
              "key %d's bottom-right pixel answered %d", i,
              menu_keyboard_hit(x1 - 1, y1 - 1));
        CHECK(menu_keyboard_hit((x0 + x1) / 2, (y0 + y1) / 2) == i,
              "key %d's centre answered %d", i,
              menu_keyboard_hit((x0 + x1) / 2, (y0 + y1) / 2));
    }

    /* One pixel outside the whole keyboard, on each side, is not on it -- and the
     * left and top edges are inside the window, so these are page points. */
    CHECK(menu_keyboard_hit(kx0 - 1, ky0 + 1) < 0,
          "one pixel left of the keyboard answered a key");
    CHECK(menu_keyboard_hit(kx0 + 1, ky0 - 1) < 0,
          "one pixel above the keyboard answered a key");
    CHECK(menu_keyboard_hit(kx1, ky0 + 1) < 0,
          "one pixel right of the keyboard answered a key");
    CHECK(menu_keyboard_hit(kx0 + 1, ky1) < 0,
          "one pixel below the keyboard answered a key");

    /* The four corners of the keyboard ARE keys, so the grid covers its rect whole
     * -- a corner that answered -1 would be a dead pixel at the edge of the popup. */
    CHECK(menu_keyboard_hit(kx0, ky0) == 0, "the keyboard's own top-left is not a key");
    CHECK(menu_keyboard_hit(kx1 - 1, ky0) == MW_KB_COLS - 1,
          "the keyboard's top-right is not the last column");
    CHECK(menu_keyboard_hit(kx0, ky1 - 1) == MW_KB_COLS * (MW_KB_ROWS - 1),
          "the keyboard's bottom-left is not the last row's first key");
    CHECK(menu_keyboard_hit(kx1 - 1, ky1 - 1) == menu_keyboard_count() - 1,
          "the keyboard's bottom-right is not the last key");
}

static void test_shift(void)
{
    char buf[16];
    int i, sp = -1, up = -1, back = -1, ok = -1, digit = -1, letter = -1;
    int punct = -1;

    for (i = 0; i < menu_keyboard_count(); i++) {
        const struct menu_key *k = menu_keyboard_key(i);

        if (!k)
            continue;
        if (k->act == MK_SPACE) sp = i;
        if (k->act == MK_SHIFT) up = i;
        if (k->act == MK_BACK) back = i;
        if (k->act == MK_OK) ok = i;
        if (k->act == MK_CHAR && k->ch == '7') digit = i;
        if (k->act == MK_CHAR && k->ch == 'q') letter = i;
        if (k->act == MK_CHAR && k->ch == '/') punct = i;
    }
    CHECK(sp >= 0 && up >= 0 && back >= 0 && ok >= 0 && digit >= 0 &&
          letter >= 0 && punct >= 0, "the table is missing a key this test names");

    menu_keyboard_show();
    menu_keyboard_shift_set(0);
    CHECK(!menu_keyboard_shift(), "shift is armed before it was set");

    /* The unshifted alphabet: a letter is lower case, and the cap agrees. */
    CHECK(menu_keyboard_char(letter) == 'q', "the 'q' key typed '%c'",
          menu_keyboard_char(letter));
    CHECK(strcmp(menu_keyboard_cap(letter, buf, sizeof buf), "q") == 0,
          "the 'q' cap read '%s'", buf);

    /* Arm it. The cap SHOWS the capital and the shift survives being drawn -- this
     * is the assertion that stands in front of a painter that disarms the shift by
     * painting it. */
    menu_keyboard_shift_set(1);
    CHECK(strcmp(menu_keyboard_cap(letter, buf, sizeof buf), "Q") == 0,
          "the armed 'q' cap read '%s', not 'Q'", buf);
    CHECK(menu_keyboard_shift(), "drawing the cap consumed the shift");
    /* And a SECOND draw is still the capital -- i.e. the painter can draw it every
     * frame and the shift is still there for the tap. */
    CHECK(strcmp(menu_keyboard_cap(letter, buf, sizeof buf), "Q") == 0,
          "the armed 'q' cap is not stable across draws");
    CHECK(menu_keyboard_shift(), "the second draw consumed the shift");

    /* The tap: a capital, AND the shift is gone -- one character, one shot. */
    CHECK(menu_keyboard_char(letter) == 'Q', "the armed 'q' key typed '%c'",
          menu_keyboard_char(letter));
    CHECK(!menu_keyboard_shift(), "the shift survived the character it armed");
    CHECK(strcmp(menu_keyboard_cap(letter, buf, sizeof buf), "q") == 0,
          "the cap is still the capital after the shift was consumed");

    /* A digit and the punctuation are unaffected by the shift, and — the point of
     * checking them — do NOT consume it: 'a' shift then '7' then 'q' must give
     * "7Q", which is what a phone keyboard does and what a table that shifted the
     * punctuation would get wrong. */
    menu_keyboard_shift_set(1);
    CHECK(menu_keyboard_char(digit) == '7', "the armed '7' key typed '%c'",
          menu_keyboard_char(digit));
    CHECK(menu_keyboard_shift(), "a digit consumed the shift");
    CHECK(menu_keyboard_char(punct) == '/', "the armed '/' key typed '%c'",
          menu_keyboard_char(punct));
    CHECK(menu_keyboard_shift(), "punctuation consumed the shift");
    CHECK(menu_keyboard_char(letter) == 'Q', "the still-armed 'q' typed '%c'",
          menu_keyboard_char(letter));
    CHECK(!menu_keyboard_shift(), "the shift survived its letter");

    /* The four others emit no character at all, whatever the shift says. */
    menu_keyboard_shift_set(1);
    CHECK(menu_keyboard_char(sp) == ' ', "SPACE typed 0x%02x",
          (unsigned char)menu_keyboard_char(sp));
    CHECK(menu_keyboard_shift(), "SPACE consumed the shift");
    CHECK(menu_keyboard_char(up) == 0, "SHIFT typed a character");
    CHECK(menu_keyboard_shift(), "SHIFT consumed the shift");
    CHECK(menu_keyboard_char(back) == 0, "DEL typed a character");
    CHECK(menu_keyboard_shift(), "DEL consumed the shift");
    CHECK(menu_keyboard_char(ok) == 0, "OK typed a character");
    CHECK(menu_keyboard_shift(), "OK consumed the shift");
    menu_keyboard_shift_set(0);

    /* The words, and their stability across a shift. */
    CHECK(strcmp(menu_keyboard_cap(sp, buf, sizeof buf), "SPACE") == 0,
          "SPACE's cap read '%s'", buf);
    CHECK(strcmp(menu_keyboard_cap(up, buf, sizeof buf), "SHIFT") == 0,
          "SHIFT's cap read '%s'", buf);
    CHECK(strcmp(menu_keyboard_cap(back, buf, sizeof buf), "DEL") == 0,
          "DEL's cap read '%s'", buf);
    CHECK(strcmp(menu_keyboard_cap(ok, buf, sizeof buf), "OK") == 0,
          "OK's cap read '%s'", buf);

    /* A HIDDEN KEYBOARD HOLDS NO SHIFT. It cannot be seen while it is down, so a
     * shift that outlived a hide would be a mode the operator could not see and had
     * no way to clear. */
    menu_keyboard_shift_set(1);
    menu_keyboard_hide();
    CHECK(!menu_keyboard_shift(), "hide() left the shift armed for next time");
    menu_keyboard_show();
    CHECK(!menu_keyboard_shift(), "the shift came back when the keyboard did");

    /* Out of range: no key, no character, and an empty cap rather than a stale
     * buffer or a read off the end of the table. */
    CHECK(menu_keyboard_char(-1) == 0, "key -1 typed a character");
    CHECK(menu_keyboard_char(menu_keyboard_count()) == 0,
          "a key past the end typed a character");
    buf[0] = 'X';
    CHECK(strcmp(menu_keyboard_cap(-1, buf, sizeof buf), "") == 0,
          "key -1's cap is not empty");
    CHECK(buf[0] == '\0', "key -1's cap left the buffer untouched, not empty");
    /* A buffer too small for even one character plus the NUL is refused, rather
     * than written past -- the painter passes a real one, but this is the boundary
     * a caller gets wrong. */
    CHECK(strcmp(menu_keyboard_cap(letter, buf, 1), "") == 0,
          "cap() wrote into a one-byte buffer");
    CHECK(strcmp(menu_keyboard_cap(letter, NULL, 16), "") == 0,
          "cap() with no buffer did not return empty");
}

int main(void)
{
    test_table();
    test_geometry();
    test_hit();
    test_shift();

    printf("test_keyboard: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
