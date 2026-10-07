/*
 * test_usblabel.c -- the text rules of a media device's name. No Pi, no rbp, no
 * address of rbp's anywhere in this file.
 *
 * It links the PRODUCTION usb_label.c, which is pure, and pins the four things
 * that would otherwise only ever be seen by a stick in the slot:
 *
 * 1. THE TAIL. A shorter label written over a longer one must leave nothing of
 *    the longer one behind. The player copies all 64 bytes of the name field and
 *    calls the result 32 characters long, so every byte has to be a function of
 *    the label -- "PHASIM_USB2" replaced by "RBOX" must not still carry "IM_USB2".
 *    This is the failure the module exists to prevent, so it is the first test.
 *
 * 2. THE EMPTY ANSWER. An empty file, one holding only whitespace, and one whose
 *    text is behind a NUL all come back as 0 characters, which is the caller's
 *    instruction to leave rbp's own "USB1"/"USB2" in place. A label of "   " is
 *    not a label.
 *
 * 3. THE LENGTH. The operator's own two sticks are 8 and 11 characters, and a
 *    label long enough to matter (100) is cut at USB_LABEL_MAX rather than running
 *    into the field's terminator.
 *
 * 4. THE DRAWABLE RANGE. A byte outside printable ASCII becomes one '?'. That is
 *    a decision and not an accident -- udev's LABEL is UTF-8 and rbp's label font
 *    is a Latin atlas, so a decoded name would be boxes, and an undeclared
 *    substitution would be a name that is silently not the one on the stick.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "usb_label.h"

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

/* The name as the player's row sees it: the UTF-16 field decoded back to text. */
static void unpack(const unsigned char *field, int bytes, char *out, int max)
{
    int i, k = 0;

    for (i = 0; i + 1 < bytes && k < max - 1; i += 2) {
        unsigned int c = (unsigned int)field[i] | ((unsigned int)field[i + 1] << 8);
        if (c == 0)
            break;
        out[k++] = (c < 0x80) ? (char)c : '?';
    }
    out[k] = '\0';
}

/* Pack a label and hand back both the text the row would show and the whole
 * field, so a test can ask about the bytes past the terminator as well. The
 * field starts as a canary rather than as zeros: a packer that skipped its zero
 * fill would otherwise pass by accident. */
#define CANARY 0xa5
static void round_trip(const char *text, unsigned char *field, char *shown, int max)
{
    memset(field, CANARY, USB_LABEL_FIELD);
    usb_label_pack(text, field, USB_LABEL_FIELD);
    unpack(field, USB_LABEL_FIELD, shown, max);
}

static void test_tail(void)
{
    unsigned char field[USB_LABEL_FIELD];
    char shown[64];
    int i;

    round_trip("PHASIM_USB2", field, shown, sizeof shown);
    CHECK(strcmp(shown, "PHASIM_USB2") == 0,
          "11 characters must survive the round trip, got \"%s\"", shown);

    /* Now the short one over it. "RBOX" is the operator's other stick. */
    round_trip("RBOX", field, shown, sizeof shown);
    CHECK(strcmp(shown, "RBOX") == 0, "a short label must show as itself, got \"%s\"", shown);

    /* Byte for byte: the name, the terminator, then zeros to the end of the field.
     * This is what the zero fill is for. */
    CHECK(field[0] == 'R' && field[1] == 0 && field[6] == 'X' && field[7] == 0,
          "RBOX must pack to UTF-16LE followed by a terminator");
    for (i = 8; i < USB_LABEL_FIELD; i++)
        CHECK(field[i] == 0, "byte %d of an overwritten field is 0x%02x, not zero",
              i, field[i]);

    /* And the other direction: a label written into a field that held nothing. */
    round_trip("", field, shown, sizeof shown);
    CHECK(shown[0] == '\0', "an empty label shows as empty, got \"%s\"", shown);
    for (i = 0; i < USB_LABEL_FIELD; i++)
        CHECK(field[i] == 0, "an empty label leaves byte %d at 0x%02x", i, field[i]);
}

static void test_empty(void)
{
    char out[USB_LABEL_MAX + 1];

    memset(out, CANARY, sizeof out);
    CHECK(usb_label_sanitize("", 0, out, USB_LABEL_MAX) == 0, "an empty file is no label");
    CHECK(out[0] == '\0', "an empty file must still terminate the buffer");

    CHECK(usb_label_sanitize("   \r\n", 5, out, USB_LABEL_MAX) == 0,
          "whitespace only is no label");
    CHECK(usb_label_sanitize("\0junk", 5, out, USB_LABEL_MAX) == 0,
          "a NUL ends the label, and nothing before one is no label");
    CHECK(usb_label_sanitize("USB1", 4, out, 0) == 0,
          "a buffer with no room must refuse rather than write");
}

static void test_length(void)
{
    char out[USB_LABEL_MAX + 1];
    unsigned char field[USB_LABEL_FIELD];
    char shown[64];
    char long100[101];
    int n;

    n = usb_label_sanitize("PHASIM_USB2", 11, out, USB_LABEL_MAX);
    CHECK(n == 11 && strcmp(out, "PHASIM_USB2") == 0,
          "the operator's second stick is 11 characters, got %d \"%s\"", n, out);

    n = usb_label_sanitize("RBOX USB", 8, out, USB_LABEL_MAX);
    CHECK(n == 8 && strcmp(out, "RBOX USB") == 0,
          "the space inside a label is part of it, got %d \"%s\"", n, out);

    /* A label past the limit is cut, and the cut one still has to land in the
     * field with a terminator after it -- which is why USB_LABEL_MAX is not 31. */
    memset(long100, 'A', 100);
    n = usb_label_sanitize(long100, 100, out, USB_LABEL_MAX);
    CHECK(n == USB_LABEL_MAX, "a 100-character label is cut at %d, got %d",
          USB_LABEL_MAX, n);

    round_trip(out, field, shown, sizeof shown);
    CHECK(strlen(shown) == USB_LABEL_MAX, "a full-length label survives the pack");
    CHECK(field[2 * USB_LABEL_MAX] == 0 && field[2 * USB_LABEL_MAX + 1] == 0,
          "a full-length label still leaves a terminator");
}

static void test_drawable(void)
{
    char out[USB_LABEL_MAX + 1];
    int n;

    /* Whitespace at the edges is trimmed; whitespace inside is part of the name,
     * and rbp has no glyph for a tab, so it becomes one '?'. */
    n = usb_label_sanitize("\tRBOX\tUSB\n", 10, out, USB_LABEL_MAX);
    CHECK(n == 8 && strcmp(out, "RBOX?USB") == 0,
          "an interior tab is one '?', got %d \"%s\"", n, out);

    /* UTF-8 bytes in, one '?' each out. udev would hand us bytes, not characters,
     * and this port has no evidence of a non-ASCII label to design against. */
    n = usb_label_sanitize("\xe6\x97\xa5\xe6\x9c\xac", 6, out, USB_LABEL_MAX);
    CHECK(n == 6 && strcmp(out, "??????") == 0,
          "UTF-8 bytes are not decoded; got %d \"%s\"", n, out);

    /* The boundary bytes, both sides: 0x20 and 0x7e are drawable, 0x1f and 0x7f
     * are not. The space has to be INTERIOR to say anything -- at either end it is
     * trimmed, which is what "RBOX USB " becoming "RBOX USB" is about. */
    n = usb_label_sanitize("A ~B", 4, out, USB_LABEL_MAX);
    CHECK(n == 4 && strcmp(out, "A ~B") == 0,
          "0x20 and 0x7e are drawable, got %d \"%s\"", n, out);
    n = usb_label_sanitize("A\x7f\x1f" "B", 4, out, USB_LABEL_MAX);
    CHECK(n == 4 && strcmp(out, "A??B") == 0,
          "0x7f and 0x1f are not drawable, got \"%s\"", out);
}

int main(void)
{
    test_tail();
    test_empty();
    test_length();
    test_drawable();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
