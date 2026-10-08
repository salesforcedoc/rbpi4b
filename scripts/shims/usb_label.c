/*
 * usb_label.c -- the text rules of a media device's name. See usb_label.h for the
 * field they feed and why they are pure; test_usblabel.c is where they are pinned.
 */
#include <string.h>

#include "usb_label.h"

/* What may surround a label and is never part of one. A label file is written by
 * `printf '%s'`, so no newline is expected -- but a file made by hand, or by a
 * tool that appends one, must not put a CR into a device name. */
static int usb_label_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

int usb_label_sanitize(const char *raw, int n, char *out, int max)
{
    int begin = 0, end = 0, i, k = 0;

    if (max <= 0)
        return 0;

    /* The useful length: to the first NUL if there is one, then whitespace removed
     * from both ends. A label that is nothing but whitespace is nothing. */
    for (i = 0; i < n && raw[i] != '\0'; i++)
        end = i + 1;
    while (begin < end && usb_label_space(raw[begin]))
        begin++;
    while (end > begin && usb_label_space(raw[end - 1]))
        end--;

    for (i = begin; i < end && k < max; i++) {
        unsigned char c = (unsigned char)raw[i];
        out[k++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    out[k] = '\0';
    return k;
}

void usb_label_pack(const char *text, unsigned char *field, int bytes)
{
    int n = 0, i;

    if (field == NULL || bytes <= 0)
        return;

    /* One byte of the field per character, and one more for the terminator. */
    if (text != NULL)
        while (text[n] != '\0' && n < bytes / 2 - 1)
            n++;

    for (i = 0; i < n; i++) {
        field[2 * i] = (unsigned char)text[i];
        field[2 * i + 1] = 0;
    }
    memset(field + 2 * n, 0, (size_t)(bytes - 2 * n));
}
