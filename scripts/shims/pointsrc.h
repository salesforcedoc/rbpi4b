/*
 * pointsrc.h — where pointer input comes from.
 *
 * On the SC Live 4 this was a hardcoded /dev/input/event0 carrying an ILI2117
 * touchscreen. A Raspberry Pi driving an HDMI monitor has no touchscreen at all
 * unless one is plugged in, and what the operator will actually attach is a
 * mouse or a trackpad. So this file owns two device kinds behind one interface:
 *
 *   absolute — a touchscreen or tablet: the device reports *where*.
 *   relative — a mouse or trackpad: the device reports *how far*, and the shim
 *              accumulates that into a pointer position.
 *
 * Both end at tscfake_emit(), which is the only thing rbp sees. Discovery is
 * re-run whenever the current device disappears, so unplugging and replugging
 * works without restarting the player.
 */
#ifndef RBPI4B_POINTSRC_H
#define RBPI4B_POINTSRC_H

#include "prompt_zone.h"    /* struct prompt_state, for the snapshot below */

/* Start (or re-start) the discovery/reader thread. Idempotent and safe to call
 * from any thread; returns 0 if the thread is running. */
int pointsrc_start(void);

/* Read-only snapshot of what the pointer is currently doing, for the launcher's
 * status output and for debugging without a log file. */
void pointsrc_status(char *buf, unsigned long buflen);

/* The live pointer, for the fb cursor compositor (fb_cursor.c). Returns 1 when a
 * *relative* device is driving the pointer and fills in its logical position and
 * button state; 0 when nothing is attached, or when the device is an absolute
 * one — which reports where it is touched and needs no arrow drawn for it.
 *
 * Unlocked, like the status above: these are four `volatile int`s the reader
 * thread updates, and the consumer redraws at 30 Hz anyway, so the worst a torn
 * read can do is place the arrow one frame behind. */
int pointsrc_cursor(int *logical_x, int *logical_y, int *down);

/* Append one line to /tmp/pointsrc.log when POINT_DEBUG is set. Shared between
 * the reader thread and the compositor so both halves of the pointer path land
 * in one file, in the order things happened. */
void pointsrc_log(const char *fmt, ...);

/* What rbp currently says about the two USB devices -- and what the host says they are
 * called -- for the USB STOP chooser. `S->live[i]` is "device i+1 has media present and
 * ready" (rbp's own test, `[UsbStorageManager+0x88] == 2`); both are left 0 when rbp
 * cannot be read, which the painter and the gesture both treat as a refusal rather than
 * as an absence of information. Returns 1 if rbp was walked, 0 if not -- the return is
 * for the log, not for the caller's decision.
 *
 * `S->label[i]` is that device's button text, composed by prompt_state_name() from
 * `/tmp/udev_usbN.label` -- the file usb-watch.sh writes with the stick's own volume
 * label, the same one rbp's SOURCE screen shows. THE TWO HALVES DO NOT SHARE A GUARD:
 * the names are read first and are filled in full even when rbp could not be walked, so
 * a caller that gets 0 still gets buttons that name the devices they are refusing. An
 * absent, empty or whitespace-only file leaves the cell EMPTY, which is "this device has
 * no name of its own and the button keeps its number", and the shipped "HOLD USB 1".
 *
 * A read-only snapshot, like pointsrc_cursor(): it touches nothing of rbp's. Safe
 * from the drawer/chooser builder thread as well as from this one, which is why it
 * is not static. See prompt_zone.h for the derivation of the walk. */
int pointsrc_usb_state(struct prompt_state *S);

#endif /* RBPI4B_POINTSRC_H */
