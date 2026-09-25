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
#ifndef RBLIVE4_POINTSRC_H
#define RBLIVE4_POINTSRC_H

/* Start (or re-start) the discovery/reader thread. Idempotent and safe to call
 * from any thread; returns 0 if the thread is running. */
int pointsrc_start(void);

/* Read-only snapshot of what the pointer is currently doing, for the launcher's
 * status output and for debugging without a log file. */
void pointsrc_status(char *buf, unsigned long buflen);

#endif /* RBLIVE4_POINTSRC_H */
