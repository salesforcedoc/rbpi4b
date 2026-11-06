/*
 * map_flx6.c -- two-deck DDJ-FLX6 input map for the RX3 clone.
 *
 * Input addresses follow the operator's XML and official FLX6 MIDI list.
 * Documented LEDs/meters are enabled; startup position query follows Mixxx.
 * Hardware decks 3/4 supply auxiliary buttons for RX3 decks 1/2, not additional
 * playback decks. All eight physical pad banks feed RX3's eight logical modes.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_led.h"
#include "rbp_vu.h"
#include "pitch_state.h"
#include "ctrl_map.h"
#include "syscalls.h"

enum {
     CH_DECK1 = 0, CH_DECK2 = 1, CH_AUX1 = 2, CH_AUX2 = 3,
     CH_FXA = 4, CH_FXB = 5, CH_MIX = 6,
     CH_PADS1 = 7, CH_PADS1S = 8, CH_PADS2 = 9, CH_PADS2S = 10,
};

enum {
     N_PLAY = 0x0b, N_CUE = 0x0c, N_LOOPIN = 0x10, N_LOOPOUT = 0x11,
     N_JOG_TOUCH = 0x36, N_MEMORY = 0x3d, N_MEMDELETE = 0x3e,
     N_SHIFT = 0x3f, N_RELOOP = 0x4d, N_CH_CUE = 0x54,
     N_SYNC = 0x58, N_MASTER = 0x5c, N_QUANTIZE = 0x5d,
     N_TEMPO_RANGE = 0x60,
     N_BROWSE_PUSH = 0x41, N_BROWSE_BACK = 0x42,
     N_BACK = 0x65, N_MENU = 0x66, N_SOURCE = 0x68, N_VIEW = 0x7a,
     N_LOAD1 = 0x46, N_LOAD2 = 0x47,
     N_LOAD1_SHIFT = 0x58, N_LOAD2_SHIFT = 0x59,
     N_PLAY_SHIFT = 0x47, N_CUE_SHIFT = 0x48, N_JOG_TOUCH_SHIFT = 0x67,
     N_FX_BEATPREV = 0x06, N_FX_BEATNEXT = 0x07,
     N_FX_AUTO = 0x66, N_FX_TAP = 0x6b,
     N_CALLPREV = 0x51, N_CALLNEXT = 0x53,
     N_CALLPREV_SHIFT = 0x61, N_CALLNEXT_SHIFT = 0x62,
     N_VINYL = 0x17, N_JOG_CUTTER = 0x1c,
     N_CH_CUE_SHIFT = 0x68,
     N_LOOPIN_SHIFT = 0x4c, N_LOOPOUT_SHIFT = 0x77, N_RELOOP_SHIFT = 0x50,
     N_MERGE_PUSH = 0x2e, N_MERGE_SELECT = 0x2f, N_MERGE_SELECT_SHIFT = 0x30,
     N_MERGE_PUSH_SHIFT = 0x31,
};

enum {
     CC_TEMPO_MSB = 0x00, CC_TEMPO_LSB = 0x20,
     CC_TRIM = 0x04, CC_EQH = 0x07, CC_EQM = 0x0b,
     CC_EQL = 0x0f, CC_FADER = 0x13,
     CC_JOG_SIDE = 0x21, CC_JOG_VINYL = 0x22,
     CC_JOG_NOVINYL = 0x23, CC_JOG_SHIFT = 0x29, CC_JOG_SIDE_SHIFT = 0x26,
     CC_HP_MIX = 0x0c, CC_CFX1 = 0x17, CC_CFX2 = 0x18, CC_CFX_PARAM = 0x19,
     CC_XFADER = 0x1f, CC_XFADER_LSB = 0x3f, CC_BROWSE = 0x40,
     CC_BROWSE_SHIFT = 0x64,
};

static int knob_scale = 1;
static int jog_scale = 1;
/* Operator measured three ring turns per hand turn. Scale BOTH native pulse
 * position and speed, retaining fractional pulses rather than dropping small
 * MIDI deltas. The ring itself continues to follow actual track position. */
static int jog_divisor = 3;
/* The inherited FLX4 value was 720 for a 111.6 mm wheel. Scale it for the
 * FLX6's 206 mm wheel: 720 * 206 / 111.6 = 1329.03. */
static int jog_ppr = 1329;
static int jog_rev;
static int jog_idle_ms = 120;
static int jog_verbose;
static int tempo_verbose;
static int tempo_rev;
static int xfader_assigned;
static unsigned int xfader_assign_wait_ticks;
static volatile int fx_led_slot;
/* Optional toolkit bridge. Null preserves the standalone map. */
static int (*toolkit_control)(unsigned int, unsigned int, unsigned int);
static int (*toolkit_pad)(unsigned int, unsigned int, unsigned int, unsigned int);
static unsigned char toolkit_pad_down[2][8], toolkit_pad_mode[2][8];
/* SHIFT belongs to a physical side, including when its deck layer changes. */
static int shift_down[2];
static int cue_down[2], reloop_down[2], color_down[4], resize_down[2][2];
static int transport_down[2][2], load_shift_down[2], merge_align_down;
static int note_first, note_end;
static int fx_depth_last = -1;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static int power_down, power_fired;
static unsigned long long power_since;
static pid_t power_child = -1;

/* rbp runs as root in the firmware chroot. Enter PID 1's host root in a
 * separate child, then exec the HOST shutdown command with no shim preload.
 * Only async-signal-safe raw syscalls are used between fork and exec. systemd
 * owns the actual stop/unmount/poweroff sequence; never invoke reboot(2). */
static void flx6_request_shutdown(void)
{
     if (power_child > 0) return;
     int root = real_open("/proc/1/root", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
     if (root < 0) {
          klog("knobshim2: FLX6 shutdown: cannot open host root: %s\n", strerror(errno));
          return;
     }
     pid_t child = fork();
     if (child == 0) {
          char *const args[] = { "shutdown", "-h", "now", NULL };
          char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", NULL };
          if (syscall(SYS_fchdir, root) < 0 || syscall(SYS_chroot, ".") < 0 ||
              syscall(SYS_chdir, "/") < 0)
               syscall(SYS_exit, 125);
          syscall(SYS_close, root);
          syscall(SYS_execve, "/sbin/shutdown", args, env);
          syscall(SYS_execve, "/usr/sbin/shutdown", args, env);
          syscall(SYS_exit, 126);
          __builtin_unreachable();
     }
     int saved_errno = errno;
     real_close(root);
     if (child < 0) {
          klog("knobshim2: FLX6 shutdown: fork failed: %s\n", strerror(saved_errno));
          return;
     }
     power_child = child;
     klog("knobshim2: FLX6 five-second left Merge FX hold: host shutdown -h now requested\n");
}

static void flx6_shutdown_tick(unsigned long long now)
{
     if (power_child > 0) {
          int status;
          pid_t done = waitpid(power_child, &status, WNOHANG);
          if (done > 0) {
               if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    klog("knobshim2: FLX6 shutdown helper failed: status=0x%x (exit125=host root, exit126=exec)\n", status);
               power_child = -1;
          } else if (done < 0 && errno == ECHILD)
               power_child = -1;
     }
     if (power_down && !power_fired && now - power_since >= 5000) {
          power_fired = 1;
          flx6_request_shutdown();
     }
}
/* Right-hand Merge FX selector, same 1..4 counter used by the working Mixxx
 * script. Numbered preset lamps advance locally; no output codes are guessed.
 * Start with the physical preset on 1 before starting/restarting the player. */
static volatile int merge_usb_mode;
static int merge_select_down[2], merge_push_down;
static volatile unsigned int merge_mark_until;

static int flx6_power_ring_value(unsigned long long now_ms)
{
     int value = 0;
     /* LED polling is a different thread; take a consistent 64-bit timestamp
      * and hold state on the 32-bit Pi. Never perform shutdown from this path. */
     pthread_mutex_lock(&map_lock);
     if (power_down && power_fired)
          value = 127; /* Five-second hold completed, not a poweroff confirmation. */
     else if (power_down && now_ms >= power_since) {
          unsigned long long elapsed = now_ms - power_since;
          unsigned interval = elapsed < 2000 ? 500 : elapsed < 4000 ? 250 : 100;
          value = (elapsed / interval) & 1 ? 0 : 127;
     }
     pthread_mutex_unlock(&map_lock);
     return value;
}

static int flx6_utility_ring_value(unsigned long long now_ms)
{
     int mode = merge_usb_mode;
     if (mode < 2) {
          int media = rbp_usb_media_state(mode + 1);
          if (media == USB_MGR_MEDIA_READY) return 127;
          if (media > 0) return (now_ms / 250) & 1 ? 127 : 0;
          return 0;
     }
     if (rbp_recording() <= 0) return 0;
     if (mode == 3) return (now_ms / 500) & 1 ? 127 : 0;
     /* Mode 3: recording makes Track Mark available. Briefly darken the ring
      * on a request; this acknowledges the press, not successful file creation. */
     unsigned int deadline = merge_mark_until;
     int remaining = (int)(deadline - (unsigned int)now_ms);
     return deadline && remaining > 0 && remaining <= 250 ? 0 : 127;
}

static void flx6_merge_utility_note(int note, int on)
{
     if (note == N_MERGE_SELECT || note == N_MERGE_SELECT_SHIFT) {
          int reverse = note == N_MERGE_SELECT_SHIFT;
          if (on && !merge_select_down[reverse]) {
               merge_usb_mode = (merge_usb_mode + (reverse ? 3 : 1)) % 4;
               klog("knobshim2: FLX6 right Merge FX utility mode=%d\n", merge_usb_mode + 1);
          }
          merge_select_down[reverse] = on;
          return;
     }
     if (!on) { merge_push_down = 0; return; }
     if (merge_push_down) return;
     merge_push_down = 1;
     int mode = merge_usb_mode;
     if (mode < 2) {
          int slot = mode + 1;
          klog("knobshim2: FLX6 Merge FX utility: USB%d safe eject request\n", slot);
          /* Exact native sequence already used by the touch settings panel. */
          send_rx_key(K_USBSTOP, OP_PRESS, slot, 0);
          send_rx_key(K_USBSTOP, OP_REPEAT, slot, 0);
          send_rx_key(K_USBSTOP, OP_RELEASE, slot, 0);
     } else {
          int key = mode == 2 ? K_TRACK_MARK : K_USB_REC;
          if (mode == 2 && rbp_recording() > 0)
               merge_mark_until = (unsigned int)(shim_now_ms() + 250);
          klog("knobshim2: FLX6 Merge FX utility: %s request\n",
               mode == 2 ? "TRACK MARK" : "MASTER RECORD toggle");
          send_rx_key(key, OP_PRESS, CH_RECORDER, 0);
          send_rx_key(key, OP_RELEASE, CH_RECORDER, 0);
     }
}

static int flx6_fx_led_slot(void)
{
     return fx_led_slot;
}

/* Official FLX6 MIDI list p3: B4/B5 64 dd, 03=1/4, 04=1/2,
 * 05=1, 06=2 (lights 1 and 4), 07=4, 08=8, 09=16, 0a=32.
 * Operator's hardware probe confirms 21=3/4, 2a=1/4 blinking,
 * 2d=1/4 slowly blinking, 37=32 slowly blinking. Unit outputs did not combine
 * in the paired probe, so the operator chose slow 1/4 blink for 1/8 and regular
 * 1/4 blink for 1/16. Use slow 32 blink for the over-range 64-beat rung. */
static int flx6_beat_led_value(int rung, unsigned long long now_ms)
{
     (void)now_ms;
     static const int values[BFX_BEAT_RUNG_COUNT] = {
          0x2a, 0x2d, 3, 4, 0x21, 5, 6, 7, 8, 9, 10, 0x37
     };
     if (rung < 0 || rung >= BFX_BEAT_RUNG_COUNT) return -1;
     return values[rung];
}

/* Mode LED writes on the FLX6 can SELECT a hardware bank. Do not force its
 * primary buttons from rbp's logical family: SHIFT selects a different bank. */
static int pad_base[2];
static int pad_want[2] = { -1, -1 };
static int pad_wait[2], pad_tries[2];
static const int pad_mode_notes[8] = { 0x1b, 0x1e, 0x20, 0x22,
                                      0x69, 0x6b, 0x6d, 0x6f };
static const int pad_mode_keys[4] = { K_HOTCUE, K_ALOOP, K_SLIPLOOP, K_BEATJUMP };
static int pad_mode_down[2][8];

static int flx6_pad_base(int deck)
{
     return pad_base[deck];
}

static void flx6_pad_mode_tick(void)
{
     for (int d = 0; d < 2; d++) {
          if (pad_want[d] < 0) continue;
          if (pad_wait[d] > 0) { pad_wait[d]--; continue; }
          int cur = rbp_deck_pad_mode(d);
          if (cur == pad_want[d]) { pad_want[d] = -1; continue; }
          if (cur < 0) continue;
          if (pad_tries[d] >= 3) {
               klog("knobshim2: FLX6 pad mode deck%d request=%d read=%d timed out\n",
                    d + 1, pad_want[d], cur);
               pad_want[d] = -1;
               continue;
          }
          int key = pad_mode_keys[pad_want[d] % 4];
          send_rx_key(key, OP_PRESS, d + 1, 0);
          send_rx_key(key, OP_RELEASE, d + 1, 0);
          pad_tries[d]++;
          pad_wait[d] = 4; /* Let rbp publish the first family change before
                           * requesting its secondary bank with another tap. */
     }
}

struct menu_tap {
     int note, key, pressed;
     const char *name;
};
static struct menu_tap menu_taps[] = {
     { N_BACK,     K_BACK,     0, "BACK" },
     { N_MENU,     K_MENU,     0, "MENU" },
     { N_SOURCE,   K_SOURCE,   0, "SOURCE" },
     { N_VIEW,     K_BROWSE,   0, "BROWSE" },
};

struct jog_ctrl {
     int rch, sch, started;
     unsigned long long last_ms;
     unsigned int vpos;
     int moving;
     int remainder;
};
static struct jog_ctrl jog_state[2] = {
     { .rch = CH_DECK1, .sch = 1 },
     { .rch = CH_DECK2, .sch = 2 },
};

struct pitch_ctrl { int sch, have_hi, pos; };
static struct pitch_ctrl tempo[2] = {
     { 1, 0, 0x2000 }, { 2, 0, 0x2000 },
};

static struct {
     int have_hi, pos, last;
} xfader = { 0, 0x2000, -1 };

struct value14 {
     int have_hi, pos;
};

/* Two MIDI channels times three selected effect slots. Only one is physically
 * active at once, but separate assembly prevents an MSB from one slot being
 * combined with an LSB from another while the selection changes. */
static struct value14 fx_depth[2][3];
static struct value14 mixer_values[CTRL_NABS];

struct fx_position {
     int ch, note, value, pressed;
     const char *name;
};

/* The FLX6's single FX SELECT button reports the selected slot, not merely a
 * generic "next" edge. Values are RX3's authoritative 14-position switch
 * positions. The CH SELECT button likewise reports its five resting slots. */
static struct fx_position fx_types[] = {
     { CH_FXA, 0x70, 1, 0, "ECHO" },
     { CH_FXA, 0x71, 3, 0, "SPIRAL" },
     { CH_FXA, 0x72, 5, 0, "REVERB" },
     { CH_FXB, 0x70, 9, 0, "TRANS" },
     { CH_FXB, 0x71, 6, 0, "FLANGER" },
     { CH_FXB, 0x72, 7, 0, "PHASER" },
};

static struct fx_position fx_channels[] = {
     { CH_FXA, 0x1e, 3, 0, "A" },
     { CH_FXA, 0x1c, 0, 0, "1" },
     { CH_FXA, 0x1d, 1, 0, "2" },
     { CH_FXA, 0x1f, 4, 0, "B" },
     { CH_FXA, 0x14, BFX_CH_MASTER, 0, "MASTER" },
     { CH_FXB, 0x1e, 3, 0, "A" },
     { CH_FXB, 0x1c, 0, 0, "1" },
     { CH_FXB, 0x1d, 1, 0, "2" },
     { CH_FXB, 0x1f, 4, 0, "B" },
     { CH_FXB, 0x14, BFX_CH_MASTER, 0, "MASTER" },
};

static struct fx_position bpm_modes[] = {
     { CH_FXA, N_FX_TAP,  1, 0, "TAP" },
     { CH_FXB, N_FX_TAP,  1, 0, "TAP" },
     { CH_FXA, N_FX_AUTO, 0, 0, "AUTO" },
     { CH_FXB, N_FX_AUTO, 0, 0, "AUTO" },
};

/* Pioneer documents 127 for selection/press and 0 for deselection/release.
 * Cold-start deselections must never select the old effect or target. */
static int fx_position_edge(struct fx_position *p, int on)
{
     if (on) {
          int fire = !p->pressed;
          p->pressed = 1;
          return fire;
     }
     p->pressed = 0;
     return 0;
}

static void flx6_note(int ch, int note, int on)
{
     if (ch == CH_FXA && (note == N_MERGE_PUSH || note == N_MERGE_PUSH_SHIFT)) {
          /* SHIFT+push never arms shutdown; either release address cancels.
           * Repeated ON reports must not restart the five-second clock. */
          if (note == N_MERGE_PUSH && on) {
               if (!power_down) {
                    power_down = 1;
                    power_fired = 0;
                    power_since = shim_now_ms();
                    klog("knobshim2: FLX6 left Merge FX hold: shutdown in 5 seconds; release to cancel\n");
               }
          } else {
               power_down = power_fired = 0;
               power_since = 0;
          }
          return;
     }
     if (ch >= CH_DECK1 && ch <= CH_AUX2 && note == N_SHIFT) {
          shift_down[ch & 1] = on;
          if (toolkit_control)
               toolkit_control(0x4103u, on ? OP_PRESS : OP_RELEASE, (ch & 1) + 1);
          return;
     }
     if (ch == CH_FXB && note == N_MERGE_PUSH_SHIFT) {
          if (on && !merge_align_down) {
               merge_usb_mode = 0;
               klog("knobshim2: FLX6 utility aligned to preset 1; physical preset must be 1 before SHIFT+knob press\n");
          }
          merge_align_down = on;
          return;
     }
     if (ch == CH_MIX && (note == N_LOAD1_SHIFT || note == N_LOAD2_SHIFT)) {
          int deck = note == N_LOAD2_SHIFT;
          if (on && !load_shift_down[deck]) {
               rbp_transport_shortcut_cancel(deck);
               /* Use the same native double-tap path as physical double LOAD. */
               for (int tap = 0; tap < 2; tap++) {
                    send_rx_key(K_LOAD, OP_PRESS, deck + 1, 0);
                    send_rx_key(K_LOAD, OP_RELEASE, deck + 1, 0);
               }
          }
          load_shift_down[deck] = on;
          return;
     }
     if ((ch == CH_DECK1 || ch == CH_DECK2) &&
         (note == N_PLAY_SHIFT || note == N_CUE_SHIFT)) {
          int side = note == N_CUE_SHIFT;
          if (on && !transport_down[ch][side])
               klog("knobshim2: FLX6 deck%d %s request=%d\n", ch + 1,
                    side ? "track start" : "cue and play",
                    rbp_transport_shortcut(ch, !side));
          transport_down[ch][side] = on;
          return;
     }
     if (on && (ch == CH_DECK1 || ch == CH_DECK2) &&
         (note == N_PLAY || note == N_CUE))
          rbp_transport_shortcut_cancel(ch);
     if (on && ch == CH_MIX && (note == N_LOAD1 || note == N_LOAD2))
          rbp_transport_shortcut_cancel(note == N_LOAD2);
     /* Firmware changes touch/pad addresses under SHIFT. Share the native
      * held binding so changing SHIFT before release cannot strand it. */
     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_JOG_TOUCH_SHIFT)
          note = N_JOG_TOUCH;
     if ((ch == CH_PADS1S || ch == CH_PADS2S) && note >= 0 && note < 0x78 &&
         (note & 0x0f) < 8 && (note >= 0x10 || !on)) {
          flx6_note(ch - 1, note, on);
          return;
     }
     /* 0x17 is SHIFT+JOG CUTTER / VINYL state, including unsolicited startup
      * reports. Neither edge has a binding now. The real unshifted button is
      * 0x1c; never drive its LED, which enables hardware Jog Cutter. */
     if (ch >= CH_DECK1 && ch <= CH_AUX2 &&
         (note == N_JOG_CUTTER || note == N_VINYL)) {
          if (on && note == N_VINYL) return;
          for (int i = note_first; i < note_end; i++) {
               struct note_ctrl *n = &note_map[i];
               if (n->note != N_JOG_CUTTER || (n->rch & 1) != (ch & 1) || !n->pressed)
                    continue;
               /* Capture the physical side, not the layer/SHIFT state at
                * release. Changing either while held must release the old key. */
               if (on) return;
               n->pressed = 0;
               send_rx_key(n->key, OP_RELEASE, n->sch, 0);
          }
          if (!on) return;
     }
     if (ch == CH_FXB && (note == N_MERGE_PUSH || note == N_MERGE_SELECT ||
                         note == N_MERGE_SELECT_SHIFT)) {
          flx6_merge_utility_note(note, on);
          return;
     }
     /* ON/OFF itself identifies the physical slot, even if FX SELECT was
      * changed before our MIDI subscription or no startup report arrived. */
     if (on && (ch == CH_FXA || ch == CH_FXB) && note >= 0x47 && note <= 0x49)
          fx_led_slot = (ch - CH_FXA) * 3 + note - 0x47;
     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_RELOOP_SHIFT) {
          if (on && !reloop_down[ch]) {
               int result = rbp_toggle_active_loop(ch);
               klog("knobshim2: FLX6 deck%d SHIFT+RELOOP Active Loop request=%d (call a saved loop first)\n", ch + 1, result);
          }
          reloop_down[ch] = on;
          return;
     }
     /* Physical strip order is 3,1,2,4. Selection is global on the RX3;
      * preserve both edges and suppress repeated Note Ons. */
     if (ch >= CH_DECK1 && ch <= CH_AUX2 && note == N_CH_CUE_SHIFT) {
          static const int keys[4] = { K_SWEEP, K_NOISE, K_DUBECHO, K_FILTER };
          if (on != color_down[ch]) {
               color_down[ch] = on;
               for (int deck = 1; deck <= 2; deck++)
                    send_rx_key(keys[ch], on ? OP_PRESS : OP_RELEASE, deck, 0);
          }
          return;
     }
     if ((ch == CH_DECK1 || ch == CH_DECK2) &&
         (note == N_CALLPREV_SHIFT || note == N_CALLNEXT_SHIFT)) {
          int deck = ch - CH_DECK1, side = note == N_CALLNEXT_SHIFT;
          if (on && !resize_down[deck][side]) {
               int result = rbp_loop_resize(deck, !side);
               klog("knobshim2: FLX6 deck%d loop %s result=%d\n",
                    deck + 1, side ? "2x" : "1/2x", result);
          }
          resize_down[deck][side] = on;
          return;
     }
     if (ch == CH_DECK1 || ch == CH_DECK2) {
          int deck = ch - CH_DECK1;
          for (int mode = 0; mode < 8; mode++) {
               if (note != pad_mode_notes[mode]) continue;
               if (on && !pad_mode_down[deck][mode]) {
                    if (toolkit_control)
                         toolkit_control(pad_mode_keys[mode & 3], OP_PRESS, deck + 1);
                    pad_base[deck] = mode * 0x10;
                    pad_want[deck] = mode;
                    pad_wait[deck] = pad_tries[deck] = 0;
                    klog("knobshim2: FLX6 pad mode deck%d -> %d\n", deck + 1, mode);
               }
               pad_mode_down[deck][mode] = on;
               return;
          }
     }
     if ((ch == CH_PADS1 || ch == CH_PADS2 ||
          ch == CH_PADS1S || ch == CH_PADS2S) &&
         note >= 0 && note < 0x78 && (note & 0x0f) < 8) {
          int deck = ch == CH_PADS2 || ch == CH_PADS2S;
          /* The controller's actual pad message settles its bank, including
           * firmware variants whose shifted bank numbering differs. */
          pad_base[deck] = note & 0x70;
          if (toolkit_control) {
               int pad = note & 0x0f;
               int mode = note >> 4;
               /* Releases retain the press's mode across bank changes. */
               if (!on && toolkit_pad_down[deck][pad])
                    mode = toolkit_pad_mode[deck][pad];
               int consumed = toolkit_pad
                    ? toolkit_pad(K_PAD1 + pad, on ? OP_PRESS : OP_RELEASE, deck + 1, mode)
                    : toolkit_control(K_PAD1 + pad, on ? OP_PRESS : OP_RELEASE, deck + 1);
               if (consumed || !on) {
                    toolkit_pad_down[deck][pad] = on;
                    toolkit_pad_mode[deck][pad] = mode;
               }
               if (consumed)
                    return;
          }
     }
     /* The operator's FLX6 reports these menu buttons as 0x96 <note> 0x00:
      * zero-velocity Note Ons with no preceding nonzero edge. Accept normal
      * pairs too, but turn a lone zero edge into the complete tap rbp expects. */
     for (unsigned int i = 0; ch == CH_MIX && i < sizeof menu_taps / sizeof menu_taps[0]; i++) {
          struct menu_tap *b = &menu_taps[i];
          int lone_release = 0;
          if (note != b->note)
               continue;
          if (on) {
               if (!b->pressed)
                    send_rx_key(b->key, OP_PRESS, CH_GLOBAL, 0);
               b->pressed = 1;
          } else if (b->pressed) {
               send_rx_key(b->key, OP_RELEASE, CH_GLOBAL, 0);
               b->pressed = 0;
          } else {
               lone_release = 1;
               send_rx_key(b->key, OP_PRESS, CH_GLOBAL, 0);
               send_rx_key(b->key, OP_RELEASE, CH_GLOBAL, 0);
          }
          if (verbose)
               klog("knobshim2: FLX6 %s note 0x%02x on=%d -> key 0x%04x%s\n",
                    b->name, note, on, b->key,
                    lone_release ? " synthetic tap" : "");
          return;
     }

     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_CH_CUE) {
          if (on && !cue_down[ch]) {
               int cur = me_get_cue(ch);
               me_set_cue(ch, cur == 0);
          }
          cue_down[ch] = on;
          return;
     }

     if ((ch == CH_PADS1S || ch == CH_PADS2S) && note >= 0 && note < 8) {
          if (on)
               hotcue_delete(ch == CH_PADS1S ? 0 : 1, note + 1);
          return;
     }

     for (unsigned int i = 0; i < sizeof fx_types / sizeof fx_types[0]; i++) {
          struct fx_position *p = &fx_types[i];
          if (ch != p->ch || note != p->note)
               continue;
          if (fx_position_edge(p, on)) {
               fx_led_slot = (int)i;
               send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, p->value);
               klog("knobshim2: FLX6 FX SELECT -> %s (position %d)\n",
                    p->name, p->value);
          }
          return;
     }
     for (unsigned int i = 0; i < sizeof fx_channels / sizeof fx_channels[0]; i++) {
          struct fx_position *p = &fx_channels[i];
          if (ch != p->ch || note != p->note)
               continue;
          if (fx_position_edge(p, on)) {
               send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, p->value);
               klog("knobshim2: FLX6 FX CH SELECT -> %s (target %d)\n",
                    p->name, p->value);
          }
          return;
     }
     for (unsigned int i = 0; i < sizeof bpm_modes / sizeof bpm_modes[0]; i++) {
          struct fx_position *p = &bpm_modes[i];
          if (ch != p->ch || note != p->note)
               continue;
          if (fx_position_edge(p, on)) {
               /* K_TAP maps OP_PRESS to TAP mode and OP_RELEASE to AUTO. */
               send_rx_key(K_TAP, p->value ? OP_PRESS : OP_RELEASE,
                           CH_GLOBAL, 0);
               klog("knobshim2: FLX6 Beat FX BPM mode -> %s\n", p->name);
          }
          return;
     }
     if ((ch == CH_FXA || ch == CH_FXB) &&
         (note == N_FX_BEATPREV || note == N_FX_BEATNEXT)) {
          if (on) {
               int prev = note == N_FX_BEATPREV;
               long d = prev ? -1 : 1;
               send_rx_key_fl(prev ? K_BEATPREV : K_BEATNEXT,
                              OP_PRESS, CH_GLOBAL, d, 0.0f, d);
          }
          return;
     }

     for (int i = 0; i < note_map_n; i++) {
          struct note_ctrl *n = &note_map[i];
          if (n->rch != ch || n->note != note)
               continue;
          if (!n->key)
               return;
          if (on && !n->pressed) {
               n->pressed = 1;
               if ((ch == CH_DECK1 || ch == CH_DECK2) &&
                   (n->key == K_LOOPIN || n->key == K_LOOPOUT))
                    led_loop_armed[ch - CH_DECK1] = n->key == K_LOOPIN;
               send_rx_key(n->key, OP_PRESS, n->sch, 0);
               if (n->repeat)
                    send_rx_key(n->key, OP_REPEAT, n->sch, 0);
          } else if (!on && n->pressed) {
               n->pressed = 0;
               send_rx_key(n->key, OP_RELEASE, n->sch, 0);
          }
          return;
     }
     if (verbose)
          klog("knobshim2: FLX6 unmapped ch%d note%d on=%d\n", ch, note, on);
}

static void flx6_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          struct abs_ctrl *a = &abs_map[i];
          if (a->rch != ch || (a->cc != cc && a->cc + 0x20 != cc))
               continue;
          struct value14 *s = &mixer_values[i];
          if (cc == a->cc) {
               s->have_hi = 1;
               s->pos = (s->pos & 0x7f) | ((val & 0x7f) << 7);
               return;
          }
          s->pos = (s->pos & 0x3f80) | (val & 0x7f);
          if (!s->have_hi) return;
          int v = s->pos >> 4;
          if (v == a->last)
               return;
          if (a->last < 0)
               klog("knobshim2: FLX6 absolute report ch%d cc=0x%02x value=%d\n", ch, cc, val);
          a->last = v;
          float f = (float)v / 1023.0f;
          if (a->key == K_FADER && a->sch >= 1 && a->sch <= 2) {
               g_fader[a->sch] = v;
               g_fader_seen[a->sch] = 1;
          }
          if (ch == CH_MIX && a->cc == CC_CFX_PARAM) {
               me_set_color_parameter(f);
          } else
               send_rx_key_f(a->key, OP_VALUE, a->sch, v, f);
          return;
     }
     if (verbose)
          klog("knobshim2: FLX6 unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

static void flx6_browse(int val)
{
     if (val < 0 || val > 127)
          return;
     int delta = val >= 64 ? val - 128 : val;
     if (delta)
          send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL,
                      delta * knob_scale < 0 ? -1 : 1);
}

static void flx6_jog(int ch, int val)
{
     struct jog_ctrl *s = &jog_state[ch == CH_DECK2];
     int d = val - 0x40; /* Official MIDI list: clockwise 0x41+, anticlockwise 0x3f-. */
     if (!d)
          return;
     unsigned long long now = shim_now_ms();
     if (!s->started) {
          s->started = 1;
          s->last_ms = now;
          return;
     }
     if (jog_rev)
          d = -d;
     int numerator = d * jog_scale + s->remainder;
     int dp = numerator / jog_divisor;
     s->remainder = numerator % jog_divisor;
     float dt = (float)(long long)(now - s->last_ms) / 1000.0f;
     s->last_ms = now;
     if (dt < 0.0005f)
          dt = 0.0005f;
     s->vpos = (s->vpos + (unsigned int)dp) & 0xffffu;
     float speed = (float)d / ((float)jog_ppr * (float)jog_divisor) / dt;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->moving = 1;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: FLX6 jog ch%d d=%d speed=%.2f\n",
               ch, d, (double)speed);
}

static void flx6_pitch(int ch, int cc, int val)
{
     int deck = ch == CH_DECK2;
     struct pitch_ctrl *s = &tempo[deck];
     if (cc == CC_TEMPO_MSB) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | ((val & 0x7f) << 7);
          return;
     }
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;
     float norm = ((float)s->pos - 8192.0f) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev) norm = -norm;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     pitch_state_set(deck, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, s->sch, v10, norm, s->pos);
     if (tempo_verbose)
          klog("knobshim2: FLX6 tempo deck%d pos=%d norm=%.3f\n",
               deck + 1, s->pos, (double)norm);
}

static void flx6_xfader(int cc, int val)
{
     if (cc == CC_XFADER) {
          xfader.have_hi = 1;
          xfader.pos = (xfader.pos & 0x7f) | ((val & 0x7f) << 7);
          return;
     }
     xfader.pos = (xfader.pos & 0x3f80) | (val & 0x7f);
     if (!xfader.have_hi)
          return;

     /* Mixxx declares CC 0x1f/0x3f as a 14-bit MSB/LSB pair. RX3 wants a
      * 10-bit absolute value, so discard the four least-significant bits. */
     int v10 = xfader.pos >> 4;
     if (v10 == xfader.last)
          return;
     xfader.last = v10;
     send_rx_key_f(K_XFADER, OP_VALUE, CH_GLOBAL, v10,
                   (float)v10 / 1023.0f);
     if (verbose)
          klog("knobshim2: FLX6 crossfader pos=%d -> v10=%d\n",
               xfader.pos, v10);
}

static int flx6_is_fx_depth_cc(int cc)
{
     int base = cc & 0x1f;
     return base == 0x02 || base == 0x04 || base == 0x06;
}

static void flx6_fx_depth(int ch, int cc, int val)
{
     int base = cc & 0x1f;
     int slot = base / 2 - 1;
     struct value14 *s = &fx_depth[ch - CH_FXA][slot];

     if (cc == base) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | ((val & 0x7f) << 7);
          return;
     }
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;

     int v10 = s->pos >> 4;
     if (v10 == fx_depth_last)
          return;
     fx_depth_last = v10;
     send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL, v10,
                   (float)v10 / 1023.0f);
     if (verbose)
          klog("knobshim2: FLX6 Beat FX depth ch%d cc=0x%02x pos=%d -> v10=%d\n",
               ch, base, s->pos, v10);
}

static void flx6_build(void)
{
     toolkit_control = (int (*)(unsigned int, unsigned int, unsigned int))
          dlsym(RTLD_DEFAULT, "rx3_pi_control");
     toolkit_pad = (int (*)(unsigned int, unsigned int, unsigned int, unsigned int))
          dlsym(RTLD_DEFAULT, "rx3_pi_pad");
     memset(toolkit_pad_mode, 0, sizeof toolkit_pad_mode);
     memset(toolkit_pad_down, 0, sizeof toolkit_pad_down);
     note_first = note_map_n;
     shift_down[0] = shift_down[1] = 0;
     merge_usb_mode = 0;
     fx_depth_last = -1;
     knob_scale = env_num("KNOB_SCALE", 1); if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1); if (jog_scale < 1) jog_scale = 1;
     jog_divisor = env_num("JOG_DIVISOR", 3); if (jog_divisor < 1) jog_divisor = 1;
     jog_ppr = env_num("JOG_PPR", 1329); if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120); if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);

     for (int d = 0; d < 2; d++) {
          int rch = CH_DECK1 + d, sch = 1 + d;
          add_note(rch, N_PLAY, K_PLAY, sch);
          add_note(rch, N_CUE, K_CUE, sch);
          add_note(rch, N_LOOPIN, K_LOOPIN, sch);
          add_note(rch, N_LOOPOUT, K_LOOPOUT, sch);
          add_note(rch, N_LOOPIN_SHIFT, K_LOOPIN, sch);
          add_note(rch, N_LOOPOUT_SHIFT, K_LOOPOUT, sch);
          add_note(rch, N_RELOOP, K_RELOOP, sch);
          add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
          add_note(rch, N_SYNC, K_SYNC, sch);
          add_note(rch, N_MASTER, K_MASTER, sch);
          add_note(rch, N_JOG_CUTTER, K_SLIP, sch);
          add_note(rch, N_MEMORY, K_MEMORY, sch);
          add_note(rch, N_MEMDELETE, K_MEMDELETE, sch);
          add_note(rch, N_CALLPREV, K_CALLPREV, sch);
          add_note(rch, N_CALLNEXT, K_CALLNEXT, sch);
          add_note(rch, N_QUANTIZE, K_QUANTIZE, sch);
          add_note(rch, N_TEMPO_RANGE, K_TEMPO_RANGE, sch);
          add_note(rch, N_SHIFT, 0, sch);
          add_abs(rch, CC_TRIM, K_TRIM, sch);
          add_abs(rch, CC_EQH, K_EQH, sch);
          add_abs(rch, CC_EQM, K_EQM, sch);
          add_abs(rch, CC_EQL, K_EQL, sch);
          add_abs(rch, CC_FADER, K_FADER, sch);

          /* Hardware decks 3/4 are extra button layers for RX3 decks 1/2. */
          int aux = CH_AUX1 + d;
          add_note(aux, N_MASTER, K_MT, sch);
          add_note(aux, N_JOG_CUTTER, K_REV, sch);
          add_note(aux, N_CALLPREV, K_SRREV, sch);
          add_note(aux, N_CALLNEXT, K_SRFWD, sch);
          add_note(aux, N_CALLPREV_SHIFT, K_TRREV, sch);
          add_note(aux, N_CALLNEXT_SHIFT, K_TRFWD, sch);
     }
     for (int d = 0; d < 2; d++) {
          int rch = d ? CH_PADS2 : CH_PADS1;
          for (int p = 0; p < 8; p++) {
               for (int bank = 0; bank < 8; bank++)
                    add_note(rch, bank * 0x10 + p, K_PAD1 + p, 1 + d);
          }
     }
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
     /* Physical channel 3 FILTER is the shared CFX parameter (e.g. filter
      * resonance), using MixerEngine's native per-input setter, not K_COLOR. */
     add_abs(CH_MIX, CC_CFX_PARAM, 0, 0);
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     /* Measured with rbp stopped: the hardware MIX knob and MASTER CUE
      * already mix USB pairs 0/1 and 2/3. Do not also mix them in rbp. */
     add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL);
     add_note(CH_MIX, N_BROWSE_BACK, K_BACK, CH_GLOBAL);
     /* Measured on the operator's FLX6: status 0x96 (rch 6), with lone
      * zero-velocity Note Ons. flx6_note() completes those into taps. */
     add_note(CH_MIX, N_BACK, K_BACK, CH_GLOBAL);
     add_note(CH_MIX, N_MENU, K_MENU, CH_GLOBAL);
     add_note(CH_MIX, N_SOURCE, K_SOURCE, CH_GLOBAL);
     add_note(CH_MIX, N_VIEW, K_BROWSE, CH_GLOBAL);
     add_note(CH_MIX, N_LOAD1, K_LOAD, 1);
     add_note(CH_MIX, N_LOAD2, K_LOAD, 2);
     /* K_LOAD is explicitly recognized by ui::KeyInput::isDoubleTapKey()
      * at 0x37dfb4. Forward every edge; rbp owns double-click Instant Doubles. */
     for (int ch = CH_FXA; ch <= CH_FXB; ch++)
          for (int note = 0x47; note <= 0x49; note++)
               add_note(ch, note, K_BFX, CH_GLOBAL);
     note_end = note_map_n;
}

static void flx6_startup(void)
{
     klog("knobshim2: FLX6 right Merge FX utility starts in mode 1 (USB1 eject); set physical preset 1 then SHIFT+right knob to realign if needed\n");
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
     for (int ch = 1; ch <= 2; ch++) {
          send_rx_key(K_FILTER, OP_PRESS, ch, 0);
          send_rx_key(K_FILTER, OP_RELEASE, ch, 0);
          send_rx_key_f(K_COLOR, OP_VALUE, ch, 512, 0.5f);
     }
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     xfader_assigned = me_set_crossfader_assign(0, XFADER_ASSIGN_A) &&
                       me_set_crossfader_assign(1, XFADER_ASSIGN_B) &&
                       me_set_crossfader_curve(XFADER_CURVE_SHARP);
     if (xfader_assigned)
          klog("knobshim2: FLX6 crossfader assigned deck1=A deck2=B curve=SHARP\n");
}

static void flx6_tick_unlocked(void)
{
     rbp_transport_shortcut_tick(0);
     flx6_pad_mode_tick();
     unsigned long long now = shim_now_ms();
     flx6_shutdown_tick(now);
     if (!xfader_assigned) {
          int a = me_set_crossfader_assign(0, XFADER_ASSIGN_A);
          int b = me_set_crossfader_assign(1, XFADER_ASSIGN_B);
          int curve = me_set_crossfader_curve(XFADER_CURVE_SHARP);
          xfader_assigned = a && b && curve;
          if (xfader_assigned)
               klog("knobshim2: FLX6 crossfader assigned deck1=A deck2=B curve=SHARP\n");
          else if (++xfader_assign_wait_ticks % 50 == 1) {
               void *me = mixer_engine();
               klog("knobshim2: FLX6 crossfader assignment waiting: "
                    "engine=%p chans=%d inputs=%d,%d assigns=%d,%d curve=%d\n",
                    me, me_channel_count(),
                    me ? me_channel_input(me, 0) : -1,
                    me ? me_channel_input(me, 1) : -1,
                    me_get_crossfader_assign(0),
                    me_get_crossfader_assign(1),
                    me_get_crossfader_curve());
          }
     }
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (s->started && s->moving && now - s->last_ms >= (unsigned)jog_idle_ms) {
               s->moving = 0;
               send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, s->vpos);
          }
     }
}

static void flx6_event_unlocked(const struct snd_seq_event *ev)
{
     if (ev->type == SNDRV_SEQ_EVENT_CONTROLLER) {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;
          if (ch == CH_MIX && cc == CC_HP_MIX)
               return; /* Hardware owns headphone mixing; rbp stays cue-only. */
          if ((ch == CH_DECK1 || ch == CH_DECK2) &&
              (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB))
               flx6_pitch(ch, cc, val);
          else if (ch == CH_MIX &&
                   (cc == CC_XFADER || cc == CC_XFADER_LSB))
               flx6_xfader(cc, val);
          else if ((ch == CH_FXA || ch == CH_FXB) &&
                   flx6_is_fx_depth_cc(cc))
               flx6_fx_depth(ch, cc, val);
          else if ((ch == CH_DECK1 || ch == CH_DECK2) &&
                   (cc == CC_JOG_SIDE || cc == CC_JOG_VINYL ||
                    cc == CC_JOG_NOVINYL || cc == CC_JOG_SHIFT || cc == CC_JOG_SIDE_SHIFT))
               flx6_jog(ch, val);
          else if (ch == CH_MIX && (cc == CC_BROWSE || cc == CC_BROWSE_SHIFT))
               flx6_browse(val);
          else
               flx6_cc_abs(ch, cc, val);
     } else if (ev->type == SNDRV_SEQ_EVENT_NOTEON) {
          flx6_note(ev->data.note.channel, ev->data.note.note,
                    ev->data.note.velocity > 0);
     } else if (ev->type == SNDRV_SEQ_EVENT_NOTEOFF) {
          flx6_note(ev->data.note.channel, ev->data.note.note, 0);
     }
}

static void flx6_tick(void)
{
     pthread_mutex_lock(&map_lock);
     flx6_tick_unlocked();
     pthread_mutex_unlock(&map_lock);
}

static void flx6_event(const struct snd_seq_event *ev)
{
     pthread_mutex_lock(&map_lock);
     flx6_event_unlocked(ev);
     pthread_mutex_unlock(&map_lock);
}

/* Run on both disappearance and a fresh subscription: ALSA may reuse the
 * same client address after a brief re-enumeration missed by absence polling. */
static void flx6_connection(int connected)
{
     static const int color_keys[4] = { K_SWEEP, K_NOISE, K_DUBECHO, K_FILTER };
     pthread_mutex_lock(&map_lock);
     rbp_transport_shortcut_tick(1);
     power_down = power_fired = 0;
     power_since = 0;
     for (int i = note_first; i < note_end; i++) {
          struct note_ctrl *n = &note_map[i];
          if (n->pressed && n->key)
               send_rx_key(n->key, OP_RELEASE, n->sch, 0);
          n->pressed = 0;
     }
     for (int ch = 0; ch < 4; ch++)
          if (color_down[ch])
               for (int deck = 1; deck <= 2; deck++)
                    send_rx_key(color_keys[ch], OP_RELEASE, deck, 0);
     for (unsigned int i = 0; i < sizeof menu_taps / sizeof menu_taps[0]; i++) {
          if (menu_taps[i].pressed)
               send_rx_key(menu_taps[i].key, OP_RELEASE, CH_GLOBAL, 0);
          menu_taps[i].pressed = 0;
     }
     for (int deck = 0; deck < 2; deck++) {
          struct jog_ctrl *s = &jog_state[deck];
          if (s->moving)
               send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, s->vpos);
          s->started = s->moving = 0;
          s->remainder = 0;
          tempo[deck].have_hi = 0;
          pad_want[deck] = -1;
          pad_wait[deck] = pad_tries[deck] = 0;
     }
     if (toolkit_control) {
          for (int deck = 1; deck <= 2; deck++) {
               for (int pad = 0; pad < 8; pad++)
                    if (toolkit_pad_down[deck - 1][pad]) {
                         if (toolkit_pad)
                              toolkit_pad(K_PAD1 + pad, OP_RELEASE, deck,
                                          toolkit_pad_mode[deck - 1][pad]);
                         else
                              toolkit_control(K_PAD1 + pad, OP_RELEASE, deck);
                    }
               toolkit_control(0x4103u, OP_RELEASE, deck);
          }
     }
     memset(toolkit_pad_down, 0, sizeof toolkit_pad_down);
     memset(shift_down, 0, sizeof shift_down);
     memset(cue_down, 0, sizeof cue_down);
     memset(reloop_down, 0, sizeof reloop_down);
     memset(color_down, 0, sizeof color_down);
     memset(resize_down, 0, sizeof resize_down);
     memset(transport_down, 0, sizeof transport_down);
     memset(load_shift_down, 0, sizeof load_shift_down);
     memset(pad_mode_down, 0, sizeof pad_mode_down);
     memset(merge_select_down, 0, sizeof merge_select_down);
     merge_push_down = merge_align_down = 0;
     merge_mark_until = 0;
     memset(mixer_values, 0, sizeof mixer_values);
     memset(fx_depth, 0, sizeof fx_depth);
     fx_depth_last = -1;
     xfader.have_hi = 0;
     xfader.last = -1;
     ctrl_abs_invalidate();
     for (unsigned int i = 0; i < sizeof fx_types / sizeof fx_types[0]; i++)
          fx_types[i].pressed = 0;
     for (unsigned int i = 0; i < sizeof fx_channels / sizeof fx_channels[0]; i++)
          fx_channels[i].pressed = 0;
     for (unsigned int i = 0; i < sizeof bpm_modes / sizeof bpm_modes[0]; i++)
          bpm_modes[i].pressed = 0;
     klog("knobshim2: FLX6 %s: held inputs released, position caches invalidated; utility preset=%d (align physical preset 1 with SHIFT+right Merge push if needed)\n",
          connected ? "connected" : "disconnected", merge_usb_mode + 1);
     pthread_mutex_unlock(&map_lock);
}

/* Transport and mixer CUE addresses come from the working Mixxx XML outputs;
 * normal/shift loop aliases come from setLoopButtonLights/startLoopLightsBlink
 * in Pioneer-DDJ-FLX6-script.js. Other output groups remain unavailable (-1).
 * The official MIDI list documents SHIFT+SYNC (quantize) on 0x5d and shifted
 * LOOP OUT on 0x77; the latter differs from the Mixxx script's 0x4e. */
static const struct led_notes flx6_leds = {
     .deck_ch = CH_DECK1,
     .n_sync = 0x58, .n_cue = 0x0c, .n_play = 0x0b, .n_keylock = -1,
     .n_vinyl = -1, .n_slip = -1, .n_loopin = 0x10, .n_loopout = 0x11,
     .n_master = 0x5c,
     .n_quantize = 0x5d,
     .transport_shift_aliases = 1, .n_play_shift = N_PLAY_SHIFT, .n_cue_shift = N_CUE_SHIFT,
     .n_loopin_shift = 0x4c, .n_loopout_shift = 0x77,
     .aux_deck_leds = 1, .aux_deck_ch = CH_AUX1,
     .aux_n_keylock = 0x5c, .aux_n_keylock_shift = 0x60,
     /* Jog Cutter is an input only: lighting it changes hardware behavior. */
     .aux_n_slip = -1, .aux_n_slip_shift = -1,
     /* Primary mode LED writes change the physical bank and used to cancel
      * SHIFT mode selections. Let the controller retain its selected mode. */
     .n_autoloop = -1, .n_mode = { -1, -1, -1, -1 },
     .pad_ch = CH_PADS1, .pad_ch2 = CH_PADS2,
     .pad_shift_enabled = 1,
     .pad_shift_ch = CH_PADS1S, .pad_shift_ch2 = CH_PADS2S,
     .pad_note_base = flx6_pad_base,
     .n_pad_base = { 0x00, 0x10, 0x20, 0x30 }, .pad_enc = LED_ENC_NONE,
     .strip_ch_first = CH_DECK1, .strip_count = 2, .n_strip_cue = 0x54,
     .master_ch_first = -1, .master_ch_count = 0, .n_master_cue = -1,
     /* toggleFxLight() addresses each slot separately. Clear inactive slots;
      * only the controller's currently selected slot follows rbp's effect. */
     .fx_ch = CH_FXA,
     .n_fx = { 0x47, 0x68, 0x68, 0x68, 0x68 },
     .fx_channels_enabled = 1,
     .fx_channels = {
          [LED_FX_BFX_ONOFF] = CH_FXA,
          [LED_FX_CFX_FILTER] = CH_AUX2,
          [LED_FX_CFX_DUBECHO] = CH_AUX1,
          [LED_FX_CFX_NOISE] = CH_DECK2,
          [LED_FX_CFX_SWEEP] = CH_DECK1,
     },
     .jog_display_enabled = 1, .jog_position_ch = 11, .jog_load_ch = 15,
     .jog_aux_mirror = 1,
     .bfx_extra_count = 5,
     .bfx_selected_slot = flx6_fx_led_slot,
     .bfx_beat_value = flx6_beat_led_value,
     .bfx_beat_cc = 0x64, .bfx_beat_ch = { CH_FXA, CH_FXB },
     .bfx_beat_resend_ms = 3000,
     .utility_ring_value = flx6_utility_ring_value,
     .utility_ring_ch = CH_FXB, .utility_ring_cc = 0x10,
     .power_ring_value = flx6_power_ring_value,
     .power_ring_ch = CH_FXA, .power_ring_cc = 0x10,
     .bfx_extra = {
          { CH_FXA, 0x48 }, { CH_FXA, 0x49 },
          { CH_FXB, 0x47 }, { CH_FXB, 0x48 }, { CH_FXB, 0x49 },
     },
     /* Official MIDI list, p5, CH LEVEL METER: B0/B1 02 dd. The five
      * cumulative bands match the FLX4 encoder exactly: 26/41/57/65/77
      * (hex) mark green1/green2/orange1/orange2/red; below 26 is dark. */
     .meter_ch_first = CH_DECK1, .n_meter_cc = 0x02,
     .meter_enc = METER_ENC_FLX4_LEVEL,
     .meter_pre_fader = 1, .meter_master_ch = -1,
     .n_meter_master_l = -1, .n_meter_master_r = -1,
};

const struct ctrl_map map_flx6 = {
     "flx6", flx6_build, flx6_startup, flx6_event, flx6_tick,
     NULL, NULL, &flx6_leds, 1, flx6_connection,
};
