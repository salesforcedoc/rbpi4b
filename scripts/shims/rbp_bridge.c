/*
 * rbp_bridge.c -- the shim's side of the boundary with rbp.
 *
 * Everything that resolves one of rbp's objects or calls into one: the
 * KeyManager and the keycode send path, the MixerEngine singletons (cue,
 * headphone split, master cue), the beat-loop entry points, and the meter hook
 * that patches rbp's own getLedValue prologue so we can read its meter values
 * instead of guessing at level units.
 *
 * All of rbp's addresses and offsets live in rbp_abi.h; this file is the code
 * that uses them. The exported functions here are the shim's internal API --
 * hidden in the dynamic symbol table, but callable across the modules.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>
#include <sys/mman.h>
#include <sound/asequencer.h>

/* State shared with the audio shim. This shim is first in LD_PRELOAD, so it is
 * the one that defines them (via shmstate.o) and owns them. */
#include "shmstate.h"

#include "syscalls.h"

#include "shimutil.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"

/* ---- rbp integration ---- */
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

/* ---------- generic helpers ---------- */

/* 7-bit CC (0..127) -> RX3 10-bit knob value (0..1023) */
int cc_to_10bit(int v)
{
     if (v < 0) v = 0;
     if (v > 127) v = 127;
     return (v << 3) | (v >> 4);
}

void *mixer_engine(void)
{
     return *(void **)ME_SINGLETON;
}

int me_channel_input(void *engine, int idx)
{
     void **begin, **end;
     int i = 0;
     if (!engine)
          return -1;
     begin = *(void ***)((char *)engine + 12);
     end   = *(void ***)((char *)engine + 16);
     if (!begin || !end || begin > end)
          return -1;
     for (void **p = begin; p < end; p++) {
          void *ch = *p;
          if (!ch)
               continue;
          if (i == idx)
               return *(int *)((char *)ch + 20);
          i++;
     }
     return -1;
}

int me_channel_count(void)
{
     void *engine = mixer_engine();
     void **begin, **end;
     int n = 0;
     if (!engine)
          return 0;
     begin = *(void ***)((char *)engine + 12);
     end   = *(void ***)((char *)engine + 16);
     if (!begin || !end || begin > end)
          return 0;
     for (void **p = begin; p < end; p++)
          if (*p)
               n++;
     return n;
}

void me_set_cue(int idx, int on)
{
     void *engine = mixer_engine();
     int input;
     if (!engine)
          return;
     input = me_channel_input(engine, idx);
     if (input < 0)
          return;
     ((void (*)(void *, int, int))ME_SET_CUE)(engine, input, on);
}

int me_get_cue(int idx)
{
     void *engine = mixer_engine();
     int input;
     if (!engine)
          return -1;
     input = me_channel_input(engine, idx);
     if (input < 0)
          return -1;
     return ((int (*)(void *, int))ME_GET_CUE)(engine, input);
}
void me_set_stereo(int type)
{
     void *engine = mixer_engine();
     if (!engine)
          return;
     ((void (*)(void *, int))ME_SET_STEREO)(engine, type);
}
void me_set_master_cue(int on)
{
     void *engine = mixer_engine();
     if (!engine)
          return;
     ((void (*)(void *, int))ME_SET_MASTER_CUE)(engine, on);
}

int me_get_master_cue(void)
{
     void *engine = mixer_engine();
     if (!engine)
          return -1;
     return ((int (*)(void *))ME_GET_MASTER_CUE)(engine);
}
static void *g_plinn[2];              /* ui::PlayerInnards per deck */

/* Locate the per-deck ui::PlayerInnards by scanning writable mappings for its
 * vtable pointer.  The UiObjManager's player array holds ui::Player objects
 * (channel byte 1/2), NOT the innards, so there is no getter to use. */
/* The object's vptr points at vtable+8 (Itanium ABI: offset-to-top + RTTI come
 * first), so `vtable for ui::PlayerInnards` @0x4d1958 means we scan for
 * 0x4d1960 - exactly how the ui::Player object reads 0x4d1488 for its
 * vtable @0x4d1480. */
#define PVTABLE_PLAYERINNARDS 0x004d1960UL

static void scan_plinn(void)
{
     FILE *f;
     char line[256];
     static int scans;
     if (g_plinn[0] && g_plinn[1])
          return;
     if (scans++ > 4)
          return;                       /* don't rescan on every turn */
     f = fopen("/proc/self/maps", "r");
     if (!f)
          return;
     while (fgets(line, sizeof(line), f)) {
          unsigned long s = 0, e = 0;
          char perms[8];
          if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3)
               continue;
          if (perms[0] != 'r' || perms[1] != 'w')
               continue;
          if (e <= s || (e - s) > (512UL << 20))
               continue;
          for (unsigned long a = (s + 3) & ~3UL; a + 0x90 <= e; a += 4) {
               unsigned char chan, mode;
               unsigned int eng;
               int d;
               if (*(volatile unsigned int *)a != PVTABLE_PLAYERINNARDS)
                    continue;
               chan = *(volatile unsigned char *)(a + PLAYERINNARDS_CHAN_OFF);
               mode = *(volatile unsigned char *)(a + PLAYERINNARDS_MODE_OFF);
               eng = *(volatile unsigned int *)(a + 0x30);
               if ((chan != 2 && chan != 3) || mode > 3 || eng < 0x80000000u)
                    continue;
               d = chan - 2;
               if (!g_plinn[d]) {
                    g_plinn[d] = (void *)a;
                    klog("knobshim2: PlayerInnards deck%d @%p (padmode=%d)\n",
                         d + 1, (void *)a, mode);
               }
          }
     }
     fclose(f);
     if (!g_plinn[0] || !g_plinn[1])
          klog("knobshim2: PlayerInnards scan: deck1=%p deck2=%p\n",
               g_plinn[0], g_plinn[1]);
}

void *plinn(int deck)
{
     if (!g_plinn[deck])
          scan_plinn();
     return g_plinn[deck];
}

/* Put rbp into AUTO pad mode with [0x7a]=0, which selects the sensible
 * beat-loop size table (4, 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32 beats).
 *
 * We tried driving this with the K_ALOOP key first, but rbp ignored it (the
 * mode byte stayed 0 through 200 ms of polling) and execAutoBeatLoop then
 * returns immediately without starting a loop.  Writing the two bytes rbp
 * itself reads ([0x74] = pad mode, [0x7a] = table select) is deterministic, and
 * the UI reads the same bytes for its pad LEDs, so it stays consistent. */
static void force_auto_padmode(void *p)
{
     int before = *(volatile unsigned char *)((char *)p + PLAYERINNARDS_MODE_OFF);
     *(volatile unsigned char *)((char *)p + PLAYERINNARDS_MODE_OFF) = 1;
     *(volatile unsigned char *)((char *)p + 0x7a) = 0;
     if (verbose && before != 1)
          klog("knobshim2: beat loop: pad mode %d -> AUTO\n", before);
}

int aloop_is_looping(int deck)
{
     return ((int (*)(void *, int))ALOOP_PE_ISLOOPING)
              (*(void **)ALOOP_PLAYENGINE_GLOBAL, deck);
}

/* Apply a loop length: set rbp's pad mode/table, then trigger the pad key.
 *
 * WARNING: only meaningful while rbp is actually in its AUTO/LOOPS pad mode.
 * rbp's UI re-asserts its own pad mode, so forcing the byte is not enough -
 * the pad key then fires a HOT CUE instead of a beat loop, which is
 * destructive.  Until that is solved this is gated behind BEATLOOP=1. */
int aloop_enabled = -1;

void aloop_apply(int deck, void *p, int idx)
{
     if (!aloop_enabled)
          return;
     force_auto_padmode(p);
     send_rx_key(K_PAD1 + idx, OP_PRESS, deck + 1, 0);
     send_rx_key(K_PAD1 + idx, OP_RELEASE, deck + 1, 0);
}
volatile unsigned int g_meter_bits[3];   /* 0=master 1=ch1 2=ch2 */
static unsigned int (*g_orig_getled)(void *, unsigned char);
static unsigned char *g_tramp;

__attribute__((visibility("default"))) unsigned int getled_hook(void *self, unsigned char level);

void install_meter_hook(void)
{
     unsigned char *p = (unsigned char *)ADDR_GETLEDVALUE;
     unsigned char saved[8];
     unsigned long pg;
     const size_t pgsz = 4096;

     if (!is_rbp_process())
          return;
     if (*(volatile uint32_t *)p != PROLOGUE_GETLED) {
          klog("knobshim2: meter hook: unexpected prologue at %p\n", (void *)p);
          return;
     }
     memcpy(saved, p, 8);

     g_tramp = mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
     if (g_tramp == MAP_FAILED) {
          g_tramp = NULL;
          klog("knobshim2: meter hook: mmap failed\n");
          return;
     }
     memcpy(g_tramp, saved, 8);                 /* displaced instructions */
     *(uint32_t *)(g_tramp + 8) = 0xE51FF004u;  /* ldr pc,[pc,#-4] */
     *(uint32_t *)(g_tramp + 12) = (uint32_t)(p + 8);
     g_orig_getled = (unsigned int (*)(void *, unsigned char))g_tramp;

     pg = (unsigned long)p & ~(unsigned long)(pgsz - 1);
     if (mprotect((void *)pg, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
          klog("knobshim2: meter hook: mprotect rw failed\n");
          g_orig_getled = NULL;
          return;
     }
     *(uint32_t *)(p + 0) = 0xE51FF004u;
     *(uint32_t *)(p + 4) = (uint32_t)&getled_hook;
     mprotect((void *)pg, pgsz, PROT_READ | PROT_EXEC);
     __builtin___clear_cache((char *)p, (char *)p + 8);
     klog("knobshim2: meter hook installed (tramp=%p)\n", (void *)g_tramp);
}

unsigned int getled_hook(void *self, unsigned char level)
{
     void *lr = __builtin_return_address(0);
     unsigned int v = g_orig_getled ? g_orig_getled(self, level) : 0;
     if (lr == (void *)RET_MASTER)
          g_meter_bits[0] = v;
     else if (lr == (void *)RET_CH1)
          g_meter_bits[1] = v;
     else if (lr == (void *)RET_CH2)
          g_meter_bits[2] = v;
     return v;
}
