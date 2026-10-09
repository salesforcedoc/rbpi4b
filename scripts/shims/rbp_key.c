/*
 * rbp_key.c -- rbp's key path. See rbp_key.h for why it is its own object.
 *
 * Nothing here interprets a keycode: rbp's KeyManager dispatches, and which
 * handler a code reaches is rbp's business (rbp_abi.h records the ones we send).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "syscalls.h"

#include "rbp_abi.h"
#include "rbp_key.h"
#include "keylog_state.h"

int is_rbp_process(void)
{
     char cmd[128];
     int fd;
     ssize_t n;
     fd = real_open("/proc/self/cmdline", O_RDONLY, 0);
     if (fd < 0)
          return 0;
     n = real_read(fd, cmd, sizeof(cmd) - 1);
     real_close(fd);
     if (n <= 0)
          return 0;
     cmd[n] = '\0';
     for (ssize_t i = 0; i < n; i++)
          if (cmd[i] == '\0')
               cmd[i] = ' ';
     return strstr(cmd, "rbp") != NULL;
}
static int is_rbp_checked = -1;

void *get_key_manager(void)
{
     void **p;
     void *mgr;
     if (is_rbp_checked < 0)
          is_rbp_checked = is_rbp_process();
     if (!is_rbp_checked)
          return NULL;
     p = (void **)UI_OBJ_MGR_GLOBAL;
     if (!p)
          return NULL;
     mgr = *p;
     if (!mgr)
          return NULL;
     return *(void **)((char *)mgr + KEY_MANAGER_OFF);
}

typedef void (*sendkey_fn)(void *km, int keycode, int op, int ch,
                           long param, float f, long l);

/* WHO IS SENDING. The recorder wants a label ("midi", "touch", "evdev") and
 * send_rx_key*() has no argument to carry one -- adding a parameter would touch
 * every one of the ~90 call sites in four maps and pointsrc.c for a field the
 * caller sends no keycode with. So the label is set beside the dispatcher that
 * is about to send, and read here.
 *
 * `__thread`, and per-library, both on purpose. Per-thread because the MIDI
 * reader and the pointer source (and the VU and LED threads) are different
 * threads that can be sending at the same moment; a plain static would let a
 * touch relabel a controller press that was in flight. Per-library, which is the
 * subtle part, is not a limitation here but the reason this works at all: this
 * object is linked into BOTH fbshim.so and knobshim.so, and each library's
 * caller reaches its OWN copy of send_rx_key_fl (the symbols are hidden, so the
 * binding is made at link time -- see rbp_key.h). pointsrc.c, which sets
 * "touch", is in fbshim and sends through fbshim's copy; map_flx4.c, which sets
 * "midi", is in knobshim and sends through knobshim's. Setter and reader are
 * always in the same library, so a per-library TLS variable is exactly the right
 * scope -- and no cross-library TLS interposition has to work for it. */
static __thread const char *keylog_tag;

void keylog_from(const char *src)
{
     keylog_tag = src;
}

void send_rx_key_fl(int keycode, int op, int ch, long param,
                           float fval, long lval)
{
     void *km = get_key_manager();
     if (!km)
          return;
     void **vt = *(void ***)km;
     sendkey_fn fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD];
     if (!fn)
          return;
     /* Recorded BEFORE the call, and only when the call is really made. Every
      * line in a key dump is therefore a keycode rbp was handed -- a send from
      * a process where rbp's KeyManager is not up yet is not recorded, because
      * rbp received nothing and a log that said otherwise would be a log of the
      * shim's intentions rather than of the player's input. The record goes
      * first so that a dump from a run that died inside rbp still ends with the
      * command that killed it. */
     keylog_record(op, keycode, ch, param, fval, lval, keylog_tag);
     fn(km, keycode, op, ch, param, fval, lval);
}

void send_rx_key_f(int keycode, int op, int ch, long param, float fval)
{
     send_rx_key_fl(keycode, op, ch, param, fval, 0);
}

void send_rx_key(int keycode, int op, int ch, long param)
{
     send_rx_key_fl(keycode, op, ch, param, 0.0f, 0);
}
