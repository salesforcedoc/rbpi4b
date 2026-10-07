/*
 * rbp_bridge.c -- the shim's side of the boundary with rbp.
 *
 * Everything that resolves one of rbp's objects or calls into one: the
 * MixerEngine singletons (cue, headphone split, master cue), the beat-loop entry
 * points, and the meter hook that patches rbp's own getLedValue prologue so we
 * can read its meter values instead of guessing at level units.
 *
 * The KeyManager and the keycode send path used to live here and are now in
 * rbp_key.c, which fbshim also links -- see that file's header for why.
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
#include "rbp_key.h"
#include "rbp_bridge.h"
#include "plinn_scan.h"

/* The key path -- is_rbp_process(), get_key_manager() and send_rx_key*() -- is
 * in rbp_key.c, because fbshim needs it too (the on-screen QUANTIZE tap) and
 * this object carries the meter hook, which must not exist in two copies. See
 * rbp_key.h. rbp_bridge.h includes it, so every declaration is unchanged here. */

/* ---------- generic helpers ---------- */

/* 7-bit CC (0..127) -> RX3 10-bit knob value (0..1023) */
int cc_to_10bit(int v)
{
     if (v < 0) v = 0;
     if (v > 127) v = 127;
     return (v << 3) | (v >> 4);
}

int rbp_beatfx_type(void)
{
     /* A plain function in rbp's text (map_jp21.c has called it this way since
      * the rotary was bound), so the only state it can be missing is rbp's:
      * the guard is the process, not a handle. */
     if (!is_rbp_process())
          return -1;
     return ((int (*)(void *))ADDR_GET_BFX_TYPE)(NULL);
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

/* The decision -- is this vtable match really a deck's innards, and which deck --
 * is in plinn_scan.c, and the arithmetic a failure reports is in the same module.
 * Not here, for the reason that module's header gives: this file is linked into
 * no test, so a rule that lives here is verified by a drill on the Pi or not at
 * all, and the rule that lives here is the one that shipped with an off-by-one
 * (2026-10-01). The vtable word itself stays this side of the boundary, so the
 * scan's read pattern is unchanged: still one word per 4-byte address, with the
 * three field reads taken only on a match. */
static void scan_plinn(void)
{
     FILE *f;
     char line[256];
     char tally[256];
     struct plinn_tally t;
     static unsigned long long last;
     unsigned long long now;
     if (g_plinn[0] && g_plinn[1])
          return;
     /* A deck that is not built yet keeps its slot NULL, so the scan has to stay
      * retryable for as long as the process lives. It used to be a hard cap of
      * five attempts (`if (scans++ > 4)`), and the cost of that is measured:
      * 2026-10-01 the log read `deck1=0xcde07280 deck2=(nil)`, so deck 2 had no
      * innards for the rest of the run and every plinn(1) went quiet for good --
      * which for hotcue_delete() is a SHIFT + pad on deck 2 that does nothing.
      * Time-limited instead: the scan walks every writable mapping, so it is
      * worth avoiding on every knob turn, but not worth giving up on. */
     now = shim_now_ms();
     if (now - last < 1000)
          return;
     last = now;
     memset(&t, 0, sizeof t);
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
          t.regions++;
          for (unsigned long a = (s + 3) & ~3UL; a + 0x90 <= e; a += 4) {
               struct plinn_cand c;
               enum plinn_refuse why;
               int d;
               t.words++;
               if (*(volatile unsigned int *)a != PVTABLE_PLAYERINNARDS)
                    continue;
               t.hits++;
               c.chan = *(volatile unsigned char *)(a + PLAYERINNARDS_CHAN_OFF);
               /* +0x74 discriminates the struct but is NOT the pad mode the UI
                * displays: measured 2026-09-30 to stay 0 through all four pad-mode
                * keycodes while rbp's grid verifiably switched.  Logged by its
                * offset, not by a name it has not earned. */
               c.mode = *(volatile unsigned char *)(a + PLAYERINNARDS_MODE_OFF);
               c.eng = *(volatile unsigned int *)(a + PLAYERINNARDS_ENGINE_OFF);
               /* The channel byte is 1-based -- deck 1 is 1, deck 2 is 2 -- and
                * the rule that reads it is in plinn_scan.c, with the 2026-10-01
                * off-by-one it shipped with written down beside it. */
               d = plinn_classify(&c, &why);
               if (d < 0) {
                    t.refused[why]++;
                    continue;
               }
               t.accepted[d]++;
               if (!g_plinn[d]) {
                    g_plinn[d] = (void *)a;
                    klog("knobshim2: PlayerInnards deck%d @%p (+0x74=%d)\n",
                         d + 1, (void *)a, c.mode);
               }
          }
     }
     fclose(f);
     if (!g_plinn[0] || !g_plinn[1]) {
          /* Pointers AND counts, in that order, because they answer different
           * questions: the pointers say what is still missing, the counts say
           * which test refused it. The line this replaces printed only the
           * pointers, so a scan that matched nothing and a scan that refused
           * everything read exactly alike -- and the 2026-10-01 off-by-one was
           * one of those, visible only as a deck that never appeared. */
          klog("knobshim2: PlayerInnards scan: deck1=%p deck2=%p; %s\n",
               g_plinn[0], g_plinn[1],
               plinn_tally_line(tally, sizeof tally, &t));
     }
}

void *plinn(int deck)
{
     if (!g_plinn[deck])
          scan_plinn();
     return g_plinn[deck];
}

/* The ui::Player for `deck` (0..1): rbp's own player array, walked exactly the
 * way IUiObjManager::getPlayer() does it (rbp_abi.h's UIOBJ_* constants).
 * UiObject::Channel is 1-based, so the deck index is the array index. */
static void *ui_player(int deck)
{
     void *holder, *players;
     int n;

     if (!is_rbp_process())
          return NULL;
     holder = *(void **)UIOBJ_HOLDER_GLOBAL;
     if (!holder)
          return NULL;
     players = *(void **)((char *)holder + UIOBJ_PLAYERS_OFF);
     n = *(int *)((char *)holder + UIOBJ_NPLAYERS_OFF);
     if (!players || n <= 0 || n > 8 || deck < 0 || deck >= n)
          return NULL;
     return *(void **)((char *)players + 4 * deck);
}

/* Delete one HOT CUE -- the gesture the FLX4 spells SHIFT + pad, and the one
 * thing on this surface rbp has no keycode for.
 *
 * THIS IS A DIRECT CALL, and that is not the first thing tried.  The smaller
 * route was to keep sending the pad key the map already sends and set the
 * selector on the deck's PlayerInnards for the duration: K_PAD1+p does reach
 * Player::onHotCueEvent, and only a selector of 3 takes its delete arm.  That
 * was built, deployed and measured on the unit (2026-10-01) -- and it does not
 * work.  Writing 7 to [innards+0x38], sending one plain K_PAD1 key and reading
 * the word back gives 0: rbp clears it on the key path before the handler looks
 * at it, so nothing written from outside survives the dispatch.
 *
 * So this calls ui::Player::onHotCueDeleteEvent(pad) directly.  It is a plain
 * concrete symbol (rbp_abi.h's ADDR_HOTCUE_DELETE), the pad number is the whole
 * argument, and the function guards itself at every step -- pad range, that
 * pad's own has-cue flag, a live TrackInfo, seacheHotCue() finding the cue, and
 * a per-pad state byte -- so an empty pad is a no-op rather than a stray write.
 *
 * It lives here rather than in the map because every address does: a map never
 * learns an RBP offset.  See memory `rbp-hot-cue-delete-route`.
 *
 * THE DELETE IS TWO HALVES, and the operator found the seam (2026-10-01): "the
 * light does go off though" -- the FLX4 pad darkens, which is rbp's own cue list
 * and DB agreeing the cue is gone, and yet the pad cell kept drawing.  That call
 * is the UI and DB half only.  The engine half is a second direct call
 * (rbp_abi.h's ADDR_ENGINE_CLEAR_HOTCUE), made below, and both are needed. */
int hotcue_delete(int deck, int pad)
{
     void *p;
     void *inn, *eng = NULL;

     if (pad < 1 || pad > 8)
          return -1;
     p = ui_player(deck);
     if (!p) {
          /* Never silent: a -1 that prints nothing is the failure that reads as
           * "the gesture is not bound" and sends the next session looking at the
           * map. */
          klog("knobshim2: hotcue delete deck%d pad%d: no ui::Player yet\n",
               deck + 1, pad);
          return -1;
     }

     ((void (*)(void *, int))ADDR_HOTCUE_DELETE)(p, pad);

     /* The SECOND half, and the one whose absence was visible on the glass: the
      * UI call above never touches the engine, so the engine kept the cue and
      * the pad grid kept drawing it -- a delete that looked like it had failed
      * while every other witness said it had worked.  rbp_abi.h's
      * ADDR_ENGINE_CLEAR_HOTCUE carries the derivation, including why the pad
      * number is passed as-is and why the channel is the 0-based deck index.
      *
      * rbp gates its own clear on isRegisteredHotCue(ch, pad), and this does not:
      * it asks and logs the answer instead.  The pad we most need to clear is the
      * one the UI half has already forgotten, and a gate that answers 0 for
      * exactly that pad would leave the ghost standing with nothing in the log to
      * say why -- the failure mode this whole route exists to avoid.  Clearing a
      * cue the engine does not hold is a no-op there, not a stray write: both
      * clearCuePosition @0x65f6c and backHotCueGate @0x66bf8 range-check first.
      *
      * Skipped only when the innards scan has not found this deck, which is
      * logged rather than passed over in silence. */
     inn = plinn(deck);
     if (inn)
          eng = *(void **)((char *)inn + PLAYERINNARDS_ENGINE_OFF);
     if (eng) {
          int reg = ((int (*)(void *, int, int))ADDR_ENGINE_IS_REGHOTCUE)
                         (eng, deck, pad);
          ((void (*)(void *, int, int))ADDR_ENGINE_CLEAR_HOTCUE)(eng, deck, pad);
          klog("knobshim2: hotcue delete deck%d pad%d @%p inn=%p eng=%p reg=%d\n",
               deck + 1, pad, p, inn, eng, reg);
     } else {
          klog("knobshim2: hotcue delete deck%d pad%d @%p: no engine yet -- the "
               "cue is gone from rbp's data but the pad grid will keep drawing "
               "it\n", deck + 1, pad, p);
     }
     return 0;
}

/* Put rbp into AUTO pad mode with [0x7a]=0, which selects the sensible
 * beat-loop size table (4, 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32 beats).
 *
 * CORRECTED 2026-09-30: K_ALOOP is NOT ignored - rbp acts on it, and its pad
 * grid visibly changes mode (measured one button per capture: note 27 -> HOT CUE,
 * note 30 -> BEAT LOOP, note 32 -> BEAT JUMP).  What was wrong is this file's
 * reading of the result: +0x74 is measured NOT to track the displayed pad mode
 * (polled 1.1 M times across all four mode buttons while the grid verifiably
 * switched, it never left 0).  So "the mode byte stayed 0 through 200 ms" never
 * meant the mode did not change, and the byte written below is not the mode rbp's
 * UI is reading.  Writing it still sticks, which is why the beat-loop path has
 * appeared to work; it is not, however, a way to *select* a mode. */
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
 * destructive.  Until that is solved this is gated behind BEATLOOP=1.
 *
 * CORRECTED 2026-09-30: "forcing the byte is not enough" is right, but for a
 * reason this comment did not have - the byte is not the mode (measured, see
 * force_auto_padmode above), so the write cannot put rbp into AUTO at all.  The
 * proven way to select a pad mode is to SEND ITS KEYCODE: K_HOTCUE / K_ALOOP /
 * K_BEATJUMP / K_SLIPLOOP each visibly switch rbp's grid.  A repair for this
 * path therefore starts by sending the mode key and then the pad, not by
 * writing +0x74.  Still gated behind BEATLOOP=1 until that is done. */
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
