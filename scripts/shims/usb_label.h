/*
 * usb_label.h -- a USB stick's volume label, as one of rbp's device rows can
 * draw it.
 *
 * rbp names its two media devices with its own literals "USB1" and "USB2", and
 * the field it would use instead -- a 64-byte UTF-16 name on the device's
 * property record -- is empty for the life of the process on this build. See
 * ctrlshim.c's usb_auto_thread for what that field is and how it was measured;
 * this module is only the text handling around it.
 *
 * Everything here is pure: no file, no rbp address, no global. The label's
 * SOURCE (a volume label, read on the host, where blkid knows which of FAT12/16,
 * FAT32 and exFAT is in play) and its DESTINATION (a field of the player's) are
 * both somebody else's problem, which is what lets test_usblabel.c pin the rules
 * below on a host with neither.
 */
#ifndef RBPI4B_USB_LABEL_H
#define RBPI4B_USB_LABEL_H

/* rbp's device-name field, in bytes. ConvertBrowseUi2Gui copies exactly these 64
 * bytes into the row when the field is non-empty and calls the result 32
 * characters long, so a name may be at most 31 characters plus its terminator. */
#define USB_LABEL_FIELD 64

/* The longest label this port will carry: 31 possible, less one so a label can
 * never fill the field to its last byte. A stick labelled longer than this shows
 * its first 30 characters, which is better than a truncated tail being read as
 * part of the name. */
#define USB_LABEL_MAX 30

/* `raw`/`n` is the contents of a label file; `out` receives at most `max`
 * characters and a NUL, and is always terminated. Returns the character count,
 * and 0 when there is nothing worth showing -- an empty file, whitespace only,
 * or `max` of zero -- in which case the caller leaves the player's own name
 * alone.
 *
 * Bytes above printable ASCII become '?' rather than being decoded: udev's LABEL
 * is UTF-8, and rbp's baked label font is a Latin atlas, so a Japanese stick name
 * would draw as boxes. One '?' per byte is the honest version of the same thing,
 * and the port has no evidence of a non-ASCII label to design against. */
int usb_label_sanitize(const char *raw, int n, char *out, int max);

/* `text` packed into `field`: UTF-16LE, NUL-terminated, and the rest of the
 * `bytes` bytes zeroed.
 *
 * Every one of the `bytes` bytes is written on every call, and that is the point
 * rather than tidiness. The player copies the WHOLE field into the row and calls
 * it 32 characters long -- measured; it is the fallback "USB2" literal that is
 * five -- so a shorter name written over a longer one would leave the longer one's
 * tail inside a buffer the row says is 32 characters wide. Whether that tail is
 * drawn or merely carried is not something this port has measured; a field that is
 * byte-for-byte a function of its label is one the caller can also compare against,
 * which is how the shim decides whether rbp has since cleared it. */
void usb_label_pack(const char *text, unsigned char *field, int bytes);

#endif
