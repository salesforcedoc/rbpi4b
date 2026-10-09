/*
 * rbp_key.h -- rbp's key path: are we rbp, where is its KeyManager, and send it
 * a keycode.
 *
 * This is the part of rbp_bridge.c that more than one shim needs, split out for
 * that reason alone. fbshim needs it because the pointer path is where a finger
 * on the on-screen QUANTIZE box is seen, and the gesture that box stands for is a
 * keycode rbp already understands (K_QUANTIZE, rbp_abi.h) -- see
 * docs/08-controls.md.
 *
 * It is a *leaf*: rbp_abi.h and libc, nothing else. That matters, because the
 * obvious alternative -- linking rbp_bridge.o into fbshim.so as well -- would
 * duplicate that object's meter hook. getled_hook is deliberately
 * visibility("default") (rbp's patched prologue jumps to it by name), and fbshim
 * is loaded *before* knobshim, so the copy in the earlier library is the one the
 * dynamic linker would resolve that name to. Two objects, two copies, one of them
 * wrong: exactly what this split avoids.
 *
 * The guard is inside get_key_manager(), not at each call site: it answers NULL
 * unless the process is rbp, and every send_rx_key*() is a no-op on NULL. So a
 * caller does not have to know whether it is running inside the player.
 */
#ifndef RBPI4B_RBP_KEY_H
#define RBPI4B_RBP_KEY_H

/* Are we running inside rbp? Cheap (one /proc read), used as a guard by every
 * thread and by install_meter_hook(). */
int is_rbp_process(void);

/* rbp's KeyManager singleton, NULL until rbp has constructed it -- and NULL in
 * any process that is not rbp. */
void *get_key_manager(void);

/* rbp's keycode path. send_rx_key() is the common case; the _f/_fl forms carry
 * a float parameter (knobs and faders are 10-bit integers plus a normalised
 * float, and rbp's handlers read one or the other). All three are no-ops when
 * rbp is not up yet or not there at all.
 *
 * THIS IS ALSO WHERE KEY_DUMP RECORDS. send_rx_key_fl() is the one place every
 * command reaches rbp, from every source -- the maps, pointsrc's taps, the
 * bridge's own calls -- so it is the one place a recorder can see them all in
 * one order. See keylog.h; keylog_state.h is the file it writes. The recording
 * is off unless KEY_DUMP names a path, and a caller needs no gate for it. */
void send_rx_key(int keycode, int op, int ch, long param);
void send_rx_key_f(int keycode, int op, int ch, long param, float fval);
void send_rx_key_fl(int keycode, int op, int ch, long param, float fval,
                    long lval);

/* Label the commands this THREAD is about to send, for the key dump's `src=`
 * field. Called once beside each dispatcher ("midi" in ctrlshim.c's
 * dispatch, "touch" in pointsrc.c's pointer_report, "evdev" on the non-MIDI
 * path); NULL means no label. Per-thread, so one source cannot relabel another's
 * events, and per-library -- see the note in rbp_key.c -- so it always reaches
 * the copy of send_rx_key_fl its own caller uses. */
void keylog_from(const char *src);

#endif /* RBPI4B_RBP_KEY_H */
