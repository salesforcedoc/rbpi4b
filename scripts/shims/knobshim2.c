/*
 * knobshim2.c — LD_PRELOAD shim mapping the Denon SC Live 4 control
 * surface (ALSA sequencer client 16:0 "Control Surface") onto the
 * XDJ-RX3's rekordbox player (rbp) key space, and mirroring rbp's LED and
 * VU state back to the panel.
 *
 * SC Live 4 side (Engine OS JP21_Controller_Assignments.qml — the file Engine
 * loads for the SC Live 4's hardware->MIDI assignment; all channels 0-based):
 *   Global  (ch 15): Back n3, FWD n4, Browse push n6/CC5, View n14, Menu n13,
 *                    SweepFX select n21..24 (DualFilter/DubEcho/Noise/Wash),
 *                    crossfader CC14, cueMix CC18, cueGain CC19, master CC20,
 *                    DJFX: select push n30/CC35, time push n25/CC36,
 *                    wet/dry CC4, activate n26, assign CC40
 *   Decks   (ch 4/5): Load n1/2, trackSkip n4/5, beatjump n6/7, Sync n8,
 *                    Cue n9, Play n10, modes n11..14, pads n15..22,
 *                    parameter n23/24, Shift n28, pitch bend n29/30,
 *                    Layer n31, jog touch n33, jog CC 0x11/0x31 (14-bit),
 *                    KeyLock n34, Vinyl n35, Slip n36, loop in/out n37/38,
 *                    autoloop push n39/CC32, stopTime CC37,
 *                    pitch fader CC 0x1F/0x4B (14-bit, invert)
 *   Mixer   (ch 0..3): pfl n13, trim CC3, treble CC4, mid CC6, bass CC8,
 *                    fader CC14, sweep fx knob CC11, Thru n15, VU CC10
 *
 * RX3 side (keycodes decoded from rbp's allinone_debug button/knob tables,
 * shift-corrected and verified against the handlers in rbp itself):
 *   sendKey(keycode, op, ch, param, float f, long l)
 *   op 0=push 2=release 4=relative-rotate 5=absolute-value
 *   Verified live: BROWSE 0x0202, SOURCE 0x0207, SELECTOR 0x420c.
 *
 * Jog / tempo model (decompiled from rbp — do NOT change casually):
 *   ui::JogSpeedGuesser  (0x366180) turns jog wheel samples into keys.
 *   JogSpeedGuesser::timerCallback (0x3663cc) sends:
 *     sendKey(0x4305, op 4, ch, 0, f, l)   key 0x4305 = jog WHEEL rotate,
 *     f = jog speed in rev/s (clamped +-8), l = jog position.
 *   ui::PlayerInnards::onKey_Jog (0x302c9c): op must be 4; it reads the f
 *     and l args out of the IKeyInput, then calls:
 *       DjEngineIF::setJogPulse(ch, l)   (0x4736c)
 *       DjEngineIF::setJogSpeed(ch, f)   (0x46d2c)  [when not searching]
 *     Player::setJogSpeed clamps to +-8.0 and treats units as rev/s.
 *   jog TOUCH = key 0x4306 push/release -> PlayerInnards::onKey_JogTouch
 *     (0x3042d8) -> DjEngineIF::touchJog(ch, bool) (0x46de4).
 *   Tempo (pitch) fader = key 0x4107 with op 5 and f = fader position in
 *     [-1..+1] (0 = detent center) -> onKey_TempoSlider (0x302a30) ->
 *     DjEngineIF::setTempoSlider(ch, f) (0x45e6c).
 *
 * Build (soft-float, glibc-2.13 chroot compatible):
 *   # sysroot from the extracted RX3 rootfs (glibc 2.13 — /tmp is ephemeral):
 *   mkdir -p /tmp/arm213sysroot/lib /tmp/arm213sysroot/usr/lib
 *   cp extracted/XDJRX3-rootfs/lib/{libc.so.6,libpthread.so.0,ld-linux.so.3} \
 *      /tmp/arm213sysroot/lib/
 *   cd /tmp/arm213sysroot/lib && for f in libc libpthread libm libdl librt; do
 *     ln -sf $f.so.6 $f.so; done
 *   arm-linux-gnueabi-gcc -O2 -mfloat-abi=soft -fno-stack-protector \
 *       -fPIC -shared -o knobshim2.so knobshim2.c -lpthread -lc \
 *       -L/tmp/arm213sysroot/lib -Wl,-rpath-link,/tmp/arm213sysroot/lib
 *   (must resolve to GLIBC_2.4-only symbols — verify with
 *    arm-linux-gnueabi-objdump -T knobshim2.so | grep GLIBC)
 *
 * Env:
 *   KNOB_SCALE=n    selector ticks per knob step (default 1)
 *   JOG_SCALE=n     jog ticks per 14-bit delta step (default 1)
 *   JOG_PPR=n       jog counts per full revolution (default 128)
 *   JOG_REV=1       reverse jog direction (default 0)
 *   JOG_IDLE_MS=n   ms of inactivity before a speed-0 jog key is sent
 *                   (default 120)
 *   JOG_VERBOSE=1   log jog keys
 *   KNOB_VERBOSE=1  log every received MIDI event to /tmp/knobshim.log
 *   LED_VERBOSE=1   log every LED change to the SC Live 4 panel
 *   LED_DISABLE=1   do not drive the panel LEDs at all
 *   LED_DEBUG_LOOP=1 log rbp loop state (loop/canrel/armed/adjust) on change
 *   LED_DUMP=1      dump rbp's whole LedStat table (id/ch/state) on change
 *   BEATLOOP=1      enable the experimental beat-loop knob path
 *                   (see docs/08-controls.md)
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

/* ---- real syscalls (bypass libc interposition) ---- */
static int real_open(const char *p, int flags)
{
     return syscall(SYS_openat, AT_FDCWD, p, flags, 0);
}
static int real_ioctl(int fd, unsigned long req, void *arg)
{
     return syscall(SYS_ioctl, fd, req, arg);
}
static ssize_t real_read(int fd, void *buf, size_t n)
{
     return syscall(SYS_read, fd, buf, n);
}
static int real_close(int fd)
{
     return syscall(SYS_close, fd);
}

/* ---- rbp integration ---- */
static int is_rbp_process(void)
{
     char cmd[128];
     int fd;
     ssize_t n;
     fd = real_open("/proc/self/cmdline", O_RDONLY);
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

#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#define KEY_MANAGER_OFF   100
#define SENDKEY_VTABLE_WORD 2

#define OP_PRESS      0
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5        /* absolute value: payload is the float f arg */

#define CH_GLOBAL     1        /* browse/source/selector used ch 1 (deck1) live */

/* ---- RX3 keycodes (see MAPPING.md) ---- */
#define K_SELECTOR   0x420c
#define K_BROWSE     0x0202
#define K_SOURCE     0x0201    /* RX3 SOURCE menu key */
#define K_USB1       0x0209    /* RX3 USB1 direct browse key */
#define K_LINK       0x0207    /* RX3 LINK key */
#define K_REKORDBOX  0x0208    /* RX3 REKORDBOX key */
#define K_TAGLIST    0x0203
#define K_MENU       0x0206
#define K_INFO       0x020b
#define K_BACK       0x420d    /* RX3 BACK key */
#define K_LOAD       0x4311
#define K_PLAY       0x4101
#define K_CUE        0x4102
#define K_SYNC       0x4112
#define K_VINYL      0x4104
#define K_JOG_TOUCH  0x4306    /* jog plate touch  (push/release) */
#define K_JOG_ROT    0x4305    /* jog wheel rotate: f=speed(rev/s) l=pos */
#define K_TEMPO_RANGE 0x4107   /* tempo range select (+-6/10/16/WIDE) */
#define K_MT         0x4108    /* master tempo (key lock) toggle */
#define K_TEMPO_SLIDER 0x4109  /* tempo slider (pitch fader) */
#define K_ALOOP      0x4114
#define K_HOTCUE     0x4113
#define K_SLIPLOOP   0x4115
#define K_BEATJUMP   0x4116
#define K_TRFWD      0x4214    /* XDJ TRACK SEARCH > (track skip next) */
#define K_TRREV      0x4215    /* XDJ TRACK SEARCH < (track skip prev) */
#define K_SRFWD      0x411f    /* XDJ SEARCH > (beat jump next) */
#define K_SRREV      0x4120    /* XDJ SEARCH < (beat jump prev) */
#define K_PAD1       0x4117
#define K_LOOPIN     0x410c
#define K_LOOPOUT    0x410d
#define K_RELOOP     0x410e
#define K_REV        0x410f
#define K_SLIP       0x4110
#define K_MASTER     0x4111
#define K_MASTERCUE  0x4407
#define K_TRIM       0x5019
#define K_EQH        0x501a
#define K_EQM        0x501b
#define K_EQL        0x501c
#define K_FADER      0x501e
#define K_XFADER     0x6017
#define K_HPMIX      0x4405
#define K_HPLEVEL    0x4406
#define K_MASTERLVL  0x4403    /* mixer MASTER LEVEL knob (CC20) */
#define K_COLOR      0x509d    /* Sound Color FX knob (per channel) */
#define K_FILTER     0x50a6    /* Sound Color FX: Filter button */
#define K_DUBECHO    0x50a2    /* Sound Color FX: Dub Echo button */
#define K_SWEEP      0x50a3    /* Sound Color FX: Sweep button */
#define K_NOISE      0x50a4    /* Sound Color FX: Noise button */
#define K_SPACE      0x50a5    /* Sound Color FX: Space button */
#define K_CRUSH      0x50a1    /* Sound Color FX: Crush button */
#define K_BFXTYPE    0x448b    /* Beat FX Type Select (0=Delay, 1=Echo, etc.) */
#define K_BFXCH      0x448c    /* Beat FX Channel Assign (5 = MASTER) */
#define BFX_CH_MASTER 5
#define K_BFX        0x448d    /* Beat FX Enable (ON / OFF toggle) */
#define K_TIME       0x448e    /* Beat FX Time */
#define K_DEPTH      0x448f    /* Beat FX Level/Depth (Wet/Dry intensity) */
#define K_BEATPREV   0x4490    /* Beat FX Beat Fraction < (halve) */
#define K_BEATNEXT   0x4491    /* Beat FX Beat Fraction > (double) */
#define K_TAP        0x4492    /* Beat FX Tap */
#define K_EFFECTQUANT 0x0493
#define K_MIC        0x0814

static int is_rbp_checked = -1;

static void *get_key_manager(void)
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

static void send_rx_key_fl(int keycode, int op, int ch, long param,
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

static void send_rx_key_f(int keycode, int op, int ch, long param, float fval)
{
     send_rx_key_fl(keycode, op, ch, param, fval, 0);
}

static void send_rx_key(int keycode, int op, int ch, long param)
{
     send_rx_key_fl(keycode, op, ch, param, 0.0f, 0);
}

#define LOG_PATH "/tmp/knobshim.log"
static void klog(const char *fmt, ...)
{
     char buf[256];
     va_list ap;
     va_start(ap, fmt);
     int n = vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);
     if (n > 0) {
          int fd = real_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
          if (fd >= 0) {
               (void)write(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
               real_close(fd);
          }
     }
}

/* ---- ALSA sequencer ---- */
#define SEQ_DEV        "/dev/snd/seq"
#define SURFACE_CLIENT 16
#define SURFACE_PORT   0

static int seq_fd = -1;
static int seq_client = -1;
static int seq_port = -1;
static int knob_scale = 1;
static int jog_scale = 1;
static int verbose = 0;

static void seq_setup(void)
{
     struct snd_seq_client_info cinfo;
     struct snd_seq_port_info pinfo;
     struct snd_seq_port_subscribe sub;
     int ver;

     seq_fd = real_open(SEQ_DEV, O_RDWR | O_NONBLOCK);
     if (seq_fd < 0) {
          klog("knobshim2: open %s failed: %s\n", SEQ_DEV, strerror(errno));
          return;
     }
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_PVERSION, &ver) < 0) {
          klog("knobshim2: PVERSION failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &seq_client) < 0) {
          klog("knobshim2: CLIENT_ID failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }

     memset(&cinfo, 0, sizeof(cinfo));
     cinfo.client = seq_client;
     cinfo.type = USER_CLIENT;
     snprintf(cinfo.name, sizeof(cinfo.name), "rbp-knob2");
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &cinfo) < 0) {
          klog("knobshim2: SET_CLIENT_INFO failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }

     memset(&pinfo, 0, sizeof(pinfo));
     pinfo.addr.client = seq_client;
     snprintf(pinfo.name, sizeof(pinfo.name), "rbp-knob2-in");
     pinfo.capability = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
     pinfo.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
     pinfo.midi_channels = 16;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pinfo) < 0) {
          klog("knobshim2: CREATE_PORT failed: %s\n", strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     seq_port = pinfo.addr.port;

     memset(&sub, 0, sizeof(sub));
     sub.sender.client = SURFACE_CLIENT;
     sub.sender.port = SURFACE_PORT;
     sub.dest.client = seq_client;
     sub.dest.port = seq_port;
     sub.queue = SNDRV_SEQ_QUEUE_DIRECT;
     if (real_ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) {
          klog("knobshim2: SUBSCRIBE %d:%d -> %d:%d failed: %s\n",
               SURFACE_CLIENT, SURFACE_PORT, seq_client, seq_port,
               strerror(errno));
          real_close(seq_fd); seq_fd = -1; return;
     }
     klog("knobshim2: seq ok client=%d port=%d subscribed to %d:%d\n",
          seq_client, seq_port, SURFACE_CLIENT, SURFACE_PORT);
}

/* ---------- generic helpers ---------- */

/* 7-bit CC (0..127) -> RX3 10-bit knob value (0..1023) */
static int cc_to_10bit(int v)
{
     if (v < 0) v = 0;
     if (v > 127) v = 127;
     return (v << 3) | (v >> 4);
}

/* clamp rotate burst to avoid flooding the key queue */
static void rot_clamped(int key, int ch, int n)
{
     if (n > 16) n = 16;
     if (n < -16) n = -16;
     for (int i = 0; i < (n < 0 ? -n : n); i++)
          send_rx_key(key, OP_ROTATE, ch, (n < 0) ? -1 : 1);
}

/* ---------- per-control state ---------- */
#define NKEYS 96
static struct {
     int rch;         /* receive channel (0-based seq channel) */
     int note;
     int key;
     int sch;         /* send channel for sendKey */
     int pressed;
} note_map[NKEYS];
static int note_map_n = 0;

static void add_note(int rch, int note, int key, int sch)
{
     if (note_map_n >= NKEYS) return;
     note_map[note_map_n].rch = rch;
     note_map[note_map_n].note = note;
     note_map[note_map_n].key = key;
     note_map[note_map_n].sch = sch;
     note_map[note_map_n].pressed = 0;
     note_map_n++;
}

/* absolute CC knob: {rch, cc, key, sch}; send 10-bit value on change */
#define NABS 40
static struct {
     int rch, cc, key, sch;
     int last;
} abs_map[NABS];
static int abs_map_n = 0;

static void add_abs(int rch, int cc, int key, int sch)
{
     if (abs_map_n >= NABS) return;
     abs_map[abs_map_n].rch = rch;
     abs_map[abs_map_n].cc = cc;
     abs_map[abs_map_n].key = key;
     abs_map[abs_map_n].sch = sch;
     abs_map[abs_map_n].last = -1;
     abs_map_n++;
}

/* jog wheel: 14-bit absolute position assembled from CC 0x37 (hi) + 0x4D (lo).
 * The RX3 wants per-move keys 0x4305/op4 carrying:
 *   f = jog speed in revolutions/sec (Player::setJogSpeed clamps to ±8)
 *   l = jog position as a wrap-around counter (JogPulse::update expects a
 *       16-bit counter wrapping at 65536, JOG_POS_TH = 0xFB7F).
 * The Prime GO jog reports an absolute 14-bit position that wraps at 16384;
 * we unwrap it into a continuous virtual u16 counter (vpos) and compute
 * speed from the delta per sample time. */
struct jog_ctrl {
     int rch;
     int pos;            /* last full 14-bit position */
     int have_hi;
     int have_lo;
     int prev;           /* previous full position for delta */
     int ready;          /* saw at least one full sample */
     unsigned long long last_ms;  /* monotonic ms of previous completed sample */
     unsigned int vpos;  /* continuous virtual jog counter (u16 space) */
     int moving;         /* jog currently moving / nonzero speed sent */
     float speed;        /* last computed speed (rev/s) */
     int sch;
};
static struct jog_ctrl jog_state[2] = { {4,0,0,0,0,0,0,0,0,0.0f,1},
                                        {5,0,0,0,0,0,0,0,0,0.0f,2} };

static int jog_ppr = 128;      /* Prime GO counts per revolution (calibrate) */
static int jog_rev = 0;        /* invert jog direction */
static int jog_idle_ms = 120;
static int jog_verbose = 0;

/* pitch fader: 14-bit, CC 0x1F (hi) + 0x4B (lo), inverted */
struct pitch_ctrl {
     int rch;
     int pos;
     int have_hi;
     int have_lo;
     int ready;
};
static struct pitch_ctrl pitch_state[2] = { {4,0,0,0,0}, {5,0,0,0,0} };
static int tempo_verbose = 0;

/* monotonic ms */
static unsigned long long now_ms(void)
{
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return (unsigned long long)ts.tv_sec * 1000ULL +
            (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* browse knob position (global CC5) */
static int knob_pos = -1;
static int shift_down = 0;

/* TIME knob push (ch15 note 25) held: turning the TIME knob then acts as the
 * RX3's BEAT < / BEAT > buttons instead of changing the time. */
static int fx_time_btn = 0;

/* "loop-in armed" latch per deck (0 = deck 1), driven by the LOOP IN/OUT keys;
 * the LED bridge turns it into the SC Live 4 blink pattern. */
static int led_loop_armed[2];

/* Last channel-fader position (10-bit, index 1/2 = deck 1/2).  rbp's channel
 * meter is PRE-fader, so the VU bridge scales it by this to behave like the
 * SC Live 4 (Engine OS meters are post-fader). */
static int g_fader[3] = { 1023, 1023, 1023 };
static int g_fader_seen[3];      /* set once the panel has reported a fader */

/* SYNC hold -> MASTER (the SC Live 4 has no MASTER button).  rbp's
 * onKey_Sync only acts on release (op 2) and onKey_Master only on press
 * (op 0), so we can decide at release time without adding latency. */
#define SYNC_HOLD_MS 600
static unsigned long long sync_press_ms[2];
static volatile int sync_held[2];        /* button is currently down */
static volatile int sync_hold_fired[2];  /* MASTER already sent for this hold */

/* built-in speaker volume (0..1), set by the booth/speaker knob (CC15, ch15).
 * Exported (non-static) so audioshim.so can read it and scale the ch6/7
 * (built-in speaker) output. */
volatile float g_speaker_gain = 1.0f;

/* master-out level (Main Vol, CC20 ch15).  Applied to ch0/1 (XLR/main) only,
 * NOT to rbp's master stream, so it does not affect the built-in monitors or
 * the headphones. */
volatile float g_master_gain = 1.0f;

/* built-in monitor on/off switch (ch15 note 41).  1 = enabled. */
volatile int g_speaker_on = 1;

/* split-cue switch (ch15 note 11).  1 = left/right = cue/main. */
volatile int g_split_cue = 0;

/* headphone cue mix (CC18) / level (CC19) on ch15, exported so audioshim.so
 * can blend rbp's cue bus with the master and scale the ch4/5 headphone out.
 * g_cue_mix: 0 = cue only, 1 = main only.  g_cue_gain: 0..1. */
volatile float g_cue_gain = 1.0f;
volatile float g_cue_mix  = 1.0f;

/* Master VU peaks published by audioshim.so (S24_LE full scale).  knobshim
 * converts them to the SC Live 4 meter CCs: CC32 (L) / CC33 (R) on ch15,
 * value = bitmask of lit segments (SC Live 4: [0,1,3,7,15,31,63]). */
volatile int g_vu_peak[2] = { 0, 0 };

/* ---------- event handlers ---------- */

/* Source-menu -> USB1-open remap.
 * On the real RX3 the drive is opened from the Source menu by the dedicated
 * hardware USB1 source button (key 0x0209 -> BrowseUiIfDpl::onKey case 0x0209
 * -> BrowseUiIf::InputKey(UKEY_USB1=3) -> UiKey_Usb1 -> ChangeBrowseDevice(3)
 * -> DEV_SEL R232c messages -> browse list population).  The RX3 engine
 * deliberately IGNORES the browse-encoder push in the Source menu (mode 12),
 * and the Prime GO has no USB1 button, so pushing the browse knob (note 6) or
 * pressing FWD (note 4) while the Source menu is shown is remapped to key
 * 0x0209 so the drive can actually be opened. */
static int source_menu_with_usb1(void)
{
     if (*(volatile uint32_t *)0x326f8b8 != 12)   /* browseMode != 12 */
          return 0;
     if (access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) != 0)
          return 0;
     return 1;
}

/* ---- direct headphone-cue (PFL) control ------------------------------
 * rbp's XDJ-RX3 panel has no per-channel PFL/CUE keycode (only deck CUE
 * 0x4102 and MASTER CUE 0x4407), so the SC Live 4's PFL buttons drive rbp's
 * mixer engine directly.  MixerEngine keeps a vector of channel pointers at
 * +12/+16; each channel stores its djengine::EnMixerInput at +20.
 *   MixerEngine singleton          @0x011493c0
 *   setMixerChHeadphoneCue(input,b)@0x000575a0
 *   getMixerChHeadphoneCue(input)  @0x000575e8
 */
#define ME_SINGLETON   0x011493c0UL
#define ME_SET_CUE     0x000575a0UL
#define ME_GET_CUE     0x000575e8UL

static void *mixer_engine(void)
{
     return *(void **)ME_SINGLETON;
}

static int me_channel_input(void *engine, int idx)
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

static int me_channel_count(void)
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

static void me_set_cue(int idx, int on)
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

static int me_get_cue(int idx)
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

/* Headphone stereo type: 0 = stereo, 1 = mono split (L = cue, R = master).
 * rbp's HeadPhone mixes this internally, so the split stays time-aligned. */
#define ME_SET_STEREO  0x0005768cUL

static void me_set_stereo(int type)
{
     void *engine = mixer_engine();
     if (!engine)
          return;
     ((void (*)(void *, int))ME_SET_STEREO)(engine, type);
}

/* master cue (RX3 MASTER CUE button; SC Live 4 has none - strips 3/4 use it) */
#define ME_SET_MASTER_CUE 0x0005766cUL
#define ME_GET_MASTER_CUE 0x0005767cUL

static void me_set_master_cue(int on)
{
     void *engine = mixer_engine();
     if (!engine)
          return;
     ((void (*)(void *, int))ME_SET_MASTER_CUE)(engine, on);
}

static int me_get_master_cue(void)
{
     void *engine = mixer_engine();
     if (!engine)
          return -1;
     return ((int (*)(void *))ME_GET_MASTER_CUE)(engine);
}

static void handle_note(int ch, int note, int on)
{
     /* SC Live 4 mixer PFL buttons (strips 1/2 = ch 0/1, note 13): toggle
      * rbp's headphone cue directly (rbp has no PFL keycode).  Latching. */
     if ((ch == 0 || ch == 1) && note == 13) {
          if (on) {
               int cur = me_get_cue(ch);
               int want = (cur == 0) ? 1 : 0;
               me_set_cue(ch, want);
               if (verbose)
                    klog("knobshim2: PFL ch%d -> cue=%d (engine=%p)\n",
                         ch + 1, want, mixer_engine());
          }
          return;
     }

     /* SC Live 4 built-in monitor on/off switch (ch15 note 41): gates the
      * ch6/7 speaker output only (rbp/booth/monitors unaffected). */
     if (ch == 15 && note == 41) {
          g_speaker_on = on ? 1 : 0;
          klog("knobshim2: speaker switch note41 velocity-on=%d -> speakers %s\n",
               on, g_speaker_on ? "ON" : "OFF");
          return;
     }

     /* SC Live 4 has no MASTER CUE button: strips 3/4 PFL (ch 2/3 note 13)
      * toggle rbp's master cue. */
     if ((ch == 2 || ch == 3) && note == 13) {
          if (on) {
               int cur = me_get_master_cue();
               int want = (cur == 0) ? 1 : 0;
               me_set_master_cue(want);
               klog("knobshim2: master cue (strip %d) -> %s\n",
                    ch + 1, want ? "ON" : "OFF");
          }
          return;
     }

     /* SC Live 4 split-cue switch (ch15 note 11): headphones L = cue, R = main.
      * rbp's EnHeadphoneStereoType: 0 = mono split, 1 = stereo. */
     if (ch == 15 && note == 11) {
          g_split_cue = on ? 1 : 0;
          me_set_stereo(g_split_cue ? 0 : 1);
          klog("knobshim2: split cue note11 velocity-on=%d -> split %s\n",
               on, g_split_cue ? "ON" : "OFF");
          return;
     }

     if ((ch == 4 || ch == 5) && note == 28)
          shift_down = on;
     if (ch == 15 && note == 25)
          fx_time_btn = on;

     /* SC Live 4 global Sound Color FX select (ch15 notes 21..24) -> both mixer
      * channels (Filter/DubEcho/Noise/Sweep). */
     if (ch == 15 && note >= 21 && note <= 24) {
          int fxkey = 0;
          switch (note) {
          case 21: fxkey = K_FILTER;  break;  /* DualFilter */
          case 22: fxkey = K_DUBECHO; break;  /* DubEcho */
          case 23: fxkey = K_NOISE;   break;  /* NoiseSweep */
          case 24: fxkey = K_SWEEP;   break;  /* Wash */
          }
          if (fxkey) {
               send_rx_key(fxkey, on ? OP_PRESS : OP_RELEASE, 1, 0);
               send_rx_key(fxkey, on ? OP_PRESS : OP_RELEASE, 2, 0);
               if (verbose)
                    klog("knobshim2: ch15 note%d %s -> SCFX 0x%04x (both ch)\n",
                         note, on ? "on" : "off", fxkey);
               return;
          }
     }

     /* Source menu + mounted Rekordbox stick: knob push and FWD become the
      * USB1 source button.  Swallow BOTH edges so the generic SELECTOR/SOURCE
      * mapping below never fires for this gesture. */
     if (ch == 15 && (note == 4 || note == 6)) {
          if (source_menu_with_usb1()) {
               send_rx_key(K_USB1, on ? OP_PRESS : OP_RELEASE, CH_GLOBAL, 0);
               if (verbose)
                    klog("knobshim2: ch15 note%d %s -> USB1 select 0x0209\n",
                         note, on ? "on" : "off");
               return;
          }
     }

     /* SYNC (deck note 8): tap = SYNC, hold = MASTER.  The SC Live 4 has no
      * MASTER button, and rbp keeps the two keycodes distinct.
      *
      * We deliberately send NOTHING on note-on.  If we sent the SYNC press
      * early and then suppressed the release for a hold, rbp saw a stuck SYNC
      * press and ran its own long-press action - instant double, i.e. it
      * loaded the other deck's track.  Deciding on release is free because
      * rbp's onKey_Sync only acts on release (op 2) and onKey_Master only on
      * press (op 0). */
     if ((ch == 4 || ch == 5) && note == 8) {
          int d = ch - 4;
          if (on) {
               sync_press_ms[d] = now_ms();
               sync_hold_fired[d] = 0;
               sync_held[d] = 1;
          } else {
               sync_held[d] = 0;
               /* if the hold already fired MASTER, the release does nothing;
                * otherwise this was a tap -> SYNC */
               if (!sync_hold_fired[d]) {
                    send_rx_key(K_SYNC, OP_PRESS, d + 1, 0);
                    send_rx_key(K_SYNC, OP_RELEASE, d + 1, 0);
               }
          }
          return;
     }

     for (int i = 0; i < note_map_n; i++) {
          if (note_map[i].rch == ch && note_map[i].note == note) {
               if (note_map[i].key == 0) {
                    if (verbose)
                         klog("knobshim2: ch%d note%d (log-only)\n", ch, note);
                    return;
               }
               int key = note_map[i].key;
               int *p = &note_map[i].pressed;
               if (on && !*p) {
                    *p = 1;
                    if (ch == 4 || ch == 5) {
                         if (key == K_LOOPIN)
                              led_loop_armed[ch - 4] = 1;
                         else if (key == K_LOOPOUT)
                              led_loop_armed[ch - 4] = 0;
                    }
                    send_rx_key(key, OP_PRESS, note_map[i].sch, 0);
                    if (verbose || key == K_FILTER || key == K_SWEEP ||
                        key == K_BFX || key == K_BEATPREV || key == K_BEATNEXT ||
                        key == K_TRFWD || key == K_TRREV ||
                        key == K_SRFWD || key == K_SRREV)
                         klog("knobshim2: ch%d note%d -> 0x%04x press (sch%d)\n",
                              ch, note, key, note_map[i].sch);
               } else if (!on && *p) {
                    *p = 0;
                    send_rx_key(key, OP_RELEASE, note_map[i].sch, 0);
                    if (verbose || key == K_FILTER || key == K_SWEEP ||
                        key == K_BFX || key == K_BEATPREV || key == K_BEATNEXT ||
                        key == K_TRFWD || key == K_TRREV ||
                        key == K_SRFWD || key == K_SRREV)
                         klog("knobshim2: ch%d note%d -> 0x%04x release (sch%d)\n",
                              ch, note, key, note_map[i].sch);
               }
               return;
          }
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d note%d %s\n", ch, note, on ? "on" : "off");
}

static void handle_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          if (abs_map[i].rch == ch && abs_map[i].cc == cc) {
               int v = cc_to_10bit(val);
               if (v != abs_map[i].last) {
                    abs_map[i].last = v;
                    if (abs_map[i].key == K_FADER && abs_map[i].sch >= 1 &&
                        abs_map[i].sch <= 2) {
                         g_fader[abs_map[i].sch] = v;
                         g_fader_seen[abs_map[i].sch] = 1;
                    }
                    float fval = (float)v / 1023.0f;
                    if (abs_map[i].key == K_HPMIX)
                         g_cue_mix = fval;
                    else if (abs_map[i].key == K_HPLEVEL)
                         g_cue_gain = fval;
                    /* absolute knobs/faders use OP_VALUE; only the
                     * detented ALOOP encoder uses OP_ROTATE. */
                    int op = (abs_map[i].key == K_ALOOP) ? OP_ROTATE : OP_VALUE;
                    send_rx_key_f(abs_map[i].key, op, abs_map[i].sch, v, fval);
                    if (verbose || (abs_map[i].key == K_COLOR && tempo_verbose) ||
                        abs_map[i].key == K_DEPTH ||
                        abs_map[i].key == K_HPMIX || abs_map[i].key == K_HPLEVEL ||
                        abs_map[i].key == K_MASTERLVL)
                         klog("knobshim2: ch%d cc%d -> 0x%04x val=%d f=%.3f (sch%d)\n",
                              ch, cc, abs_map[i].key, v, (double)fval, abs_map[i].sch);
               }
               return;
          }
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

/* browse knob (global CC5): relative delta (1 = +1 step, 127 = -1 step) */
static void handle_knob_pos(int v)
{
     if (v < 0 || v > 127)
          return;
     int delta = (v >= 64) ? (v - 128) : v;
     if (delta == 0)
          return;
     rot_clamped(K_SELECTOR, CH_GLOBAL, delta * knob_scale);
     if (verbose)
          klog("knobshim2: browse knob v=%d delta=%d\n", v, delta);
}

/* jog: assemble 14-bit pos; on each completed sample emit the RX3 jog
 * wheel key 0x4305 / op4 with f = speed (rev/s) and l = virtual position.
 * Speed sign follows the jog direction; jog_rev flips it. */
static void handle_jog(int ch, int cc, int val)
{
     int idx = -1;
     for (int i = 0; i < 2; i++)
          if (jog_state[i].rch == ch) { idx = i; break; }
     if (idx < 0)
          return;
     struct jog_ctrl *s = &jog_state[idx];
     if (cc == 0x11) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);   /* store hi half now */
     } else if (cc == 0x31) {
          s->have_lo = 1;
          s->pos = (s->pos & 0x3f80) | val;        /* store lo half now */
     } else {
          return;
     }
     if (!s->have_hi || !s->have_lo)
          return;                  /* need both halves */
     int pos = s->pos;
     s->have_hi = s->have_lo = 0;  /* consume the pair */
     if (!s->ready) {
          s->ready = 1;
          s->prev = pos;
          s->last_ms = now_ms();
          return;
     }
     int d = pos - s->prev;
     s->prev = pos;
     if (d > 8192) d -= 16384;
     if (d < -8192) d += 16384;
     if (d == 0) {
          s->last_ms = now_ms();
          return;
     }
     unsigned long long t = now_ms();
     float dt = (float)(long long)(t - s->last_ms) / 1000.0f;
     s->last_ms = t;
     if (dt < 0.0005f) dt = 0.0005f;
     if (jog_rev)
          d = -d;
     int dp = d * jog_scale;               /* scaled pulse delta */
     if (dp > 4096) dp = 4096;
     if (dp < -4096) dp = -4096;
     /* continuous virtual counter in 16-bit space (wrap 65536) */
     s->vpos = (unsigned int)(s->vpos + (unsigned int)dp) & 0xFFFFu;
     /* speed in rev/s of the Prime GO jog */
     float speed = (float)dp / (float)(jog_ppr * jog_scale) / dt;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->moving = 1;
     s->speed = speed;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: jog ch%d delta=%d speed=%.2f pos=%u (sch%d)\n",
               ch, d, (double)speed, s->vpos, s->sch);
}

/* jog idle watcher: when the wheel has not moved for jog_idle_ms, send a
 * speed-0 key so the engine ends the pitch bend / jog state. */
static void *jog_idle_thread(void *arg)
{
     (void)arg;
     for (;;) {
          usleep(30000);   /* 30 ms */
          unsigned long long t = now_ms();
          for (int i = 0; i < 2; i++) {
               struct jog_ctrl *s = &jog_state[i];
               if (!s->ready || !s->moving)
                    continue;
               if ((unsigned long long)(long long)(t - s->last_ms) <
                   (unsigned long long)jog_idle_ms)
                    continue;
               s->moving = 0;
               s->speed = 0.0f;
               send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
               if (jog_verbose)
                    klog("knobshim2: jog ch%d idle -> speed 0\n", s->rch);
          }
     }
     return NULL;
}

static int tempo_rev = 0;

/* pitch fader: 14-bit, CC 0x1F (hi) + 0x4B (lo).
 * Prime GO hardware: 0x0000 = bottom (+), 0x3FFF = top (-)
 * RX3 tempo slider = key 0x4109 op 5, payload = float fader position in
 * [-1.0 .. +1.0] (0 = detent center).
 * -1.0 = slower (top), +1.0 = faster (bottom).
 */
static void handle_pitch(int ch, int cc, int val)
{
     int idx = -1;
     for (int i = 0; i < 2; i++)
          if (pitch_state[i].rch == ch) { idx = i; break; }
     if (idx < 0)
          return;
     struct pitch_ctrl *s = &pitch_state[idx];
     if (cc == 0x1F) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);
     } else if (cc == 0x4B) {
          s->have_lo = 1;
          s->pos = (s->pos & 0x3f80) | val;
     } else {
          return;
     }
     if (!s->ready) {
          if (s->have_hi && s->have_lo)
               s->ready = 1;
          else
               return;
     }
     /* Only dispatch on CC 0x4B (the low byte, which always arrives right after 0x1F) */
     if (cc != 0x4B)
          return;

     int pos = s->pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;

     /* Prime GO:
      * physical top (slower): pos = 0x3FFF (16383)
      * physical detent (center): pos = ~0x2000 (8192)
      * physical bottom (faster): pos = 0x0000 (0)
      * Pioneer:
      * float: -1.0 at top (slower), 0.0 at center, +1.0 at bottom (faster)
      */
     float norm = ((float)0x2000 - (float)pos) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev)
          norm = -norm;

     /* pitch_state[0] = rch 4 (left deck) -> deck 1,
      * pitch_state[1] = rch 5 (right deck) -> deck 2 */
     int sch = idx + 1;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;

     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, sch, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: pitch ch%d (deck %d) pos=%d -> tempo norm=%.3f v10=0x%03x\n",
               ch, sch, pos, (double)norm, v10);
}

/* ---- DJ FX: channel assign, effect select, time/parameter -----------------
 * SC Live 4 panel (JP21_Controller_Assignments.qml):
 *   DJFxAssign: note 40, velocity = position [0,1,2,3,127] =
 *               ['Channel3','Channel1','Channel2','Channel4','Main']
 *   DJFxSelect: turnCC 35 (endless, 1 = +1, 127 = -1)
 *   DJFxTime:   turnCC 36 (endless, 1 = +1, 127 = -1)
 *
 * rbp side:
 *   EnBeatEffectSelectChannel (djengine::c_str):
 *     0=PLAYER_0 1=PLAYER_1 2=MIC_0 3=ASSIGN_A 4=ASSIGN_B 5=MASTER 6=AUX
 *   onEv_BeatEffectType(SW_BFX_TYPE) is a 14-position switch:
 *     pos -> internal type: 0->6 1->5 2->13 3->7 4->14 5->1 6->4
 *                           7->9 8->10 9->2 10->3 11->12 12->8 13->11
 *   Beat FX time is an index in [getBeatEffectMinTime(), getBeatEffectMaxTime()]
 *   per effect type, so we can step it without tracking our own value.
 */
#define BFX_TYPE_POSITIONS 14
#define ADDR_GET_BFX_TYPE  0x4d514

/* inverse of the switch table above: internal type -> switch position */
static const signed char bfx_type_to_pos[15] = {
     -1, 5, 9, 10, 6, 1, 0, 3, 12, 7, 8, 13, 11, 2, 4
};

static int g_fx_type_pos = -1;         /* current SW_BFX_TYPE position */
static volatile int g_bfxch_user_set;  /* user moved the assign knob */

static void handle_fx_assign(int vel)
{
     int param;
     switch (vel) {
     case 1:   param = 0; break;        /* Channel 1 -> PLAYER_0 */
     case 2:   param = 1; break;        /* Channel 2 -> PLAYER_1 */
     case 127: param = 5; break;        /* Main      -> MASTER   */
     default:                           /* Ch3 / Ch4: rbp has only 2 players */
          g_bfxch_user_set = 1;
          if (verbose)
               klog("knobshim2: fx assign pos vel=%d -> no rbp channel\n", vel);
          return;
     }
     g_bfxch_user_set = 1;
     send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, param);
     if (verbose)
          klog("knobshim2: fx assign vel=%d -> BFXCH %d\n", vel, param);
}

static void handle_fx_select(int val)
{
     int d = (val == 127) ? -1 : (val == 1 ? 1 : 0);
     int t;
     if (!d)
          return;
     if (g_fx_type_pos < 0) {
          t = ((int (*)(void *))ADDR_GET_BFX_TYPE)(NULL);
          g_fx_type_pos = (t >= 0 && t <= 14) ? bfx_type_to_pos[t] : 0;
     }
     g_fx_type_pos += d;
     if (g_fx_type_pos < 0)
          g_fx_type_pos = BFX_TYPE_POSITIONS - 1;
     if (g_fx_type_pos >= BFX_TYPE_POSITIONS)
          g_fx_type_pos = 0;
     send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, g_fx_type_pos);
     if (verbose)
          klog("knobshim2: fx select -> position %d\n", g_fx_type_pos);
}

static void handle_fx_time(int val)
{
     int d = (val == 127) ? -1 : (val == 1 ? 1 : 0);
     if (!d)
          return;
     /* TIME-knob push (or SHIFT) + TIME knob = the RX3's BEAT < / BEAT >
      * buttons, which the SC Live 4 does not have.  rbp: onEv_BeatFxBeat(long)
      * with -1 = halve, +1 = double the beat fraction. */
     if (shift_down || fx_time_btn) {
          int key = (d < 0) ? K_BEATPREV : K_BEATNEXT;
          /* ui::Mixer::asEventCode only produces the BEAT events for
           * 0x4490/0x4491 when the op is PRESS (0):
           *   0x4490 -> 0x2015, 0x4491 -> 0x2016, both `tst op,#15; movne 0`.
           * Sending OP_VALUE made rbp drop them. */
          send_rx_key_fl(key, OP_PRESS, CH_GLOBAL, d, 0.0f, d);
          if (verbose)
               klog("knobshim2: shift+time -> %s\n",
                    (d < 0) ? "BEAT<" : "BEAT>");
          return;
     }
     /* plain turn: rbp's onEv_BeatFxTime(long) -> BeatFxTimeKnob(value,
      * absolute=false), so the argument is a ROTATION DELTA and rbp itself
      * steps/clamps (it adds value*n to the current percent).  Sending an
      * absolute position did nothing - every turn re-read the old value. */
     send_rx_key_fl(K_TIME, OP_ROTATE, CH_GLOBAL, d, 0.0f, d);
     if (verbose)
          klog("knobshim2: fx time step %d\n", d);
}

/* ---- Beat-loop knob -------------------------------------------------------
 * The RX3 has no beat-loop knob: it triggers loops from its pads in the AUTO
 * pad mode.  rbp does have the machinery though:
 *
 *   ui::PlayerInnards::execAutoBeatLoop(short padIndex, bool)   @0x300c64
 *
 * which maps a pad index 0..7 through a per-mode table to a beat fraction and
 * calls playengine::Loop::startAutoBeatLoop(numer, denom, ...):
 *
 *   AUTO (pad mode 1): [5, 7, 6, 13, 14, 15, 16, 17]
 *   SLIP (pad mode 2): [0, 1, 2, 4, 7, 8, 3, 5]
 *   codes -> (numer,denom): 0:16/1 1:8/1 2:4/1 3:3/1 4:2/1 5:4/3 6:2/3 7:1/1
 *                           8:1/2 10:1/8 11:1/16 12:1/32 13:1/3 14:1/5
 *                           15:1/6 16:1/7 17:1/9
 *
 * The Opus Quad has a dedicated delta handler (BeatLoopHandler::
 * selectBeatLoopLengthChange) which rbp does not, so we keep our own cursor
 * and call execAutoBeatLoop() directly.  The per-deck PlayerInnards is reached
 * the same way IUiObjManager::getPlayer() does it:
 *   uiobj = *(0x026867c0); arr = *(uiobj+64); count = *(uiobj+72);
 *   player = arr[ch-1]     (UiObject::Channel is 1-based)
 */
#define ADDR_EXEC_AUTOBEATLOOP 0x300c64
#define UIOBJ_HOLDER_GLOBAL    0x026867c0UL   /* *(here) = IUiObjManager */
#define UIOBJ_PLAYERS_OFF      64
#define UIOBJ_NPLAYERS_OFF     72
#define PLAYERINNARDS_CHAN_OFF 0x26
#define PLAYERINNARDS_MODE_OFF 0x74
#define ALOOP_POSITIONS        8

/* playengine::PlayEngine singleton + isLooping(EnPlayerChannel) (see the LED
 * section below, which defines these later in the file) */
#define ALOOP_PLAYENGINE_GLOBAL 0x011497d0UL
#define ALOOP_PE_ISLOOPING      0x5eadc

static int g_aloop_idx[2] = { -1, -1 };
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

static void *plinn(int deck)
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

static int aloop_is_looping(int deck)
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
static int aloop_enabled = -1;

static void aloop_apply(int deck, void *p, int idx)
{
     if (!aloop_enabled)
          return;
     force_auto_padmode(p);
     send_rx_key(K_PAD1 + idx, OP_PRESS, deck + 1, 0);
     send_rx_key(K_PAD1 + idx, OP_RELEASE, deck + 1, 0);
}

/* knob turn: change the selected loop length; if a loop is running, apply it
 * immediately so the live loop changes size (otherwise just remember it) */
static void handle_aloop(int deck, int val)
{
     static int last[2] = { -1, -1 };
     void *p;
     int d;

     /* relative encoder: 127 = counter-clockwise = SHORTER, 1 = clockwise =
      * LONGER.  Index 0 is the longest loop (4 beats), index 7 the shortest
      * (1/32), so "shorter" means a higher index. */
     if (val == 127)
          d = 1;
     else if (val == 1)
          d = -1;
     else if (last[deck] < 0)
          d = 0;
     else
          d = (val > last[deck]) ? 1 : ((val < last[deck]) ? -1 : 0);
     if (val != 1 && val != 127)
          last[deck] = val;
     if (!d)
          return;

     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;                 /* default 4 beats */
     g_aloop_idx[deck] += d;
     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;
     if (g_aloop_idx[deck] >= ALOOP_POSITIONS)
          g_aloop_idx[deck] = ALOOP_POSITIONS - 1;

     if (!aloop_is_looping(deck)) {
          if (verbose)
               klog("knobshim2: beat loop deck%d select idx=%d (not looping)\n",
                    deck + 1, g_aloop_idx[deck]);
          return;
     }
     p = plinn(deck);
     if (!p)
          return;
     aloop_apply(deck, p, g_aloop_idx[deck]);
     if (verbose)
          klog("knobshim2: beat loop deck%d -> idx=%d (was looping), now=%d\n",
               deck + 1, g_aloop_idx[deck], aloop_is_looping(deck));
}

/* button push: engage a beat loop of the selected length */
static void handle_aloop_button(int deck)
{
     void *p;
     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;                 /* default 4 beats */
     p = plinn(deck);
     if (!p) {
          if (verbose)
               klog("knobshim2: beat loop deck%d push: no PlayerInnards\n",
                    deck + 1);
          return;
     }
     {
          int m0 = *(volatile unsigned char *)((char *)p + PLAYERINNARDS_MODE_OFF);
          aloop_apply(deck, p, g_aloop_idx[deck]);
          if (verbose)
               klog("knobshim2: beat loop deck%d push idx=%d mode_before=%d "
                    "looping=%d chan=%d\n",
                    deck + 1, g_aloop_idx[deck], m0, aloop_is_looping(deck),
                    *(volatile unsigned char *)((char *)p +
                                                PLAYERINNARDS_CHAN_OFF));
          return;
     }
}

static void handle_event(const struct snd_seq_event *ev)
{
     if (!get_key_manager())
          return;
     if (aloop_enabled < 0)
          aloop_enabled = getenv("BEATLOOP") != NULL;
     switch (ev->type) {
     case SNDRV_SEQ_EVENT_CONTROLLER: {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;
          if (ch == 15 && cc == 15) {
               g_speaker_gain = (float)val / 127.0f;
               if (verbose)
                    klog("knobshim2: speaker knob cc15=%d -> gain %.3f\n",
                         val, (double)g_speaker_gain);
          }
          else if (ch == 15 && cc == 20) {
               g_master_gain = (float)val / 127.0f;
               if (verbose)
                    klog("knobshim2: master knob cc20=%d -> gain %.3f\n",
                         val, (double)g_master_gain);
          }
          else if (ch == 15 && cc == 5)
               handle_knob_pos(val);
          else if (ch == 15 && cc == 35)
               handle_fx_select(val);
          else if (ch == 15 && cc == 36)
               handle_fx_time(val);
          else if ((ch == 4 || ch == 5) && cc == 32)
               handle_aloop(ch - 4, val);      /* beat-loop knob */
          else if (cc == 0x11 || cc == 0x31)
               handle_jog(ch, cc, val);
          else if (cc == 0x1F || cc == 0x4B)
               handle_pitch(ch, cc, val);
          else
               handle_cc_abs(ch, cc, val);
          break;
     }
     case SNDRV_SEQ_EVENT_NOTEON: {
          int on = ev->data.note.velocity > 0;
          /* FX channel assign carries its position in the velocity */
          if (ev->data.note.channel == 15 && ev->data.note.note == 40) {
               if (on)
                    handle_fx_assign(ev->data.note.velocity);
               break;
          }
          /* beat-loop knob push: engage a loop of the selected length */
          if ((ev->data.note.channel == 4 || ev->data.note.channel == 5) &&
              ev->data.note.note == 39) {
               if (on)
                    handle_aloop_button(ev->data.note.channel - 4);
               break;
          }
          handle_note(ev->data.note.channel, ev->data.note.note, on);
          break;
     }
     case SNDRV_SEQ_EVENT_NOTEOFF:
          handle_note(ev->data.note.channel, ev->data.note.note, 0);
          break;
     default:
          break;
     }
}

static void *bfx_init_thread(void *arg)
{
     (void)arg;
     for (int i = 0; i < 6 && !g_bfxch_user_set; i++) {
          usleep(500000); /* 500ms */
          send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, BFX_CH_MASTER);
     }
     klog("knobshim2: Beat FX Channel master enforcement complete\n");
     return NULL;
}

static void build_maps(void)
{
     /* ---- global (ch 15) — send ch 1 ---- */
     /* LOAD streams on the GLOBAL channel (verified live: deck 1 LOAD =
      * ch15 note1; Prime GO is the same).  It is NOT on the deck channel. */
     add_note(15, 1, K_LOAD, 1);               /* deck 1 LOAD */
     add_note(15, 2, K_LOAD, 2);               /* deck 2 LOAD */
     add_note(15, 3,  K_BACK, CH_GLOBAL);      /* BACK */
     add_note(15, 4,  K_SOURCE, CH_GLOBAL);    /* FWD -> source */
     add_note(15, 6,  K_SELECTOR, CH_GLOBAL);  /* browse knob push */
     add_note(15, 13, K_MENU, CH_GLOBAL);      /* MENU */
     add_note(15, 14, K_BROWSE, CH_GLOBAL);    /* VIEW */
     add_abs(15, 14, K_XFADER, CH_GLOBAL);     /* crossfader */
     add_abs(15, 18, K_HPMIX, CH_GLOBAL);      /* cue mix */
     add_abs(15, 19, K_HPLEVEL, CH_GLOBAL);    /* cue gain */
     /* CC20 (Main Vol) is handled in the CC dispatch as g_master_gain (ch0/1
      * only) - NOT sent to rbp's master level, which would affect everything. */

     /* ---- decks (rch 4 = left -> deck1, rch 5 = right -> deck2) ---- */
     for (int d = 0; d < 2; d++) {
          int rch = 4 + d;
          int sch = 1 + d;
          /* CENSOR (ch4/ch5 note1) is remapped to the RX3 RELOOP/EXIT key:
           * exits a running loop, and re-enters it when a loop is stored. */
          add_note(rch, 1, K_RELOOP, sch);
          add_note(rch, 8,  K_SYNC, sch);
          add_note(rch, 9,  K_CUE, sch);
          add_note(rch, 10, K_PLAY, sch);
          add_note(rch, 11, K_HOTCUE, sch);    /* mode CUES/STEMS */
          add_note(rch, 12, K_ALOOP, sch);     /* mode LOOPS/AUTO */
          add_note(rch, 13, K_SLIPLOOP, sch);  /* mode ROLL/SAMPLER */
          add_note(rch, 14, K_BEATJUMP, sch);  /* mode SLICER -> rbp BEAT JUMP */
          for (int p = 0; p < 8; p++)
               add_note(rch, 15 + p, K_PAD1 + p, sch);
          add_note(rch, 29, K_TEMPO_RANGE, sch); /* pitch bend - -> tempo range */
          add_note(rch, 30, K_MT, sch);          /* pitch bend + -> master tempo */
          add_note(rch, 33, K_JOG_TOUCH, sch); /* jog touch */
          add_note(rch, 34, K_MT, sch);        /* key lock */
          add_note(rch, 35, K_VINYL, sch);
          add_note(rch, 36, K_SLIP, sch);
          add_note(rch, 37, K_LOOPIN, sch);    /* manual loop in */
          add_note(rch, 38, K_LOOPOUT, sch);   /* manual loop out */
          add_note(rch, 39, K_ALOOP, sch);     /* auto loop push */
          add_note(rch, 4, K_TRREV, sch);      /* track skip <: XDJ TRACK REV */
          add_note(rch, 5, K_TRFWD, sch);      /* track skip >: XDJ TRACK FWD */
          add_note(rch, 6, K_SRREV, sch);      /* beat jump <:  XDJ SEARCH REV */
          add_note(rch, 7, K_SRFWD, sch);      /* beat jump >:  XDJ SEARCH FWD */
          /* CC 32 (auto/beat-loop knob) is handled by handle_aloop(), which
           * drives rbp's execAutoBeatLoop() directly - rbp's onKey_AutoBeatLoop
           * rejects any op but PRESS, so a rotate-based mapping is dead. */
     }

     /* ---- mixer channels ----
      * The SC Live 4 has 4 strips but rbp is a 2-channel mixer, so only strips
      * 1/2 drive decks 1/2.  Mapping strips 3/4 onto the SAME decks (the old
      * ch1->deck1, ch2->deck2, ch3->deck1, ch4->deck2) made two strips fight
      * over one rbp channel: setting strip 1's fader was immediately
      * overwritten by strip 3's position (and vice versa), which also broke
      * the startup absolute-control re-assert. */
     for (int m = 0; m < 2; m++) {
          int rch = m;
          int sch = 1 + m;         /* strip 1 -> deck 1, strip 2 -> deck 2 */
          add_abs(rch, 3,  K_TRIM, sch);
          add_abs(rch, 4,  K_EQH, sch);
          add_abs(rch, 6,  K_EQM, sch);
          add_abs(rch, 8,  K_EQL, sch);
          add_abs(rch, 14, K_FADER, sch);
          add_abs(rch, 11, K_COLOR, sch);      /* sweep fx knob -> Color knob */
          add_note(rch, 13, 0, sch);           /* PFL: no RX3 code, log-only */
     }

     /* ---- DJ FX (global ch 15) ----
      * (turn-encoders CC35/CC36/CC40 need relative-encoder handling and are
      *  handled separately below.) */
     add_note(15, 26, K_BFX, CH_GLOBAL);       /* FX activate (0x448d) */
     add_abs(15, 4,  K_DEPTH, CH_GLOBAL);      /* FX wet/dry knob (0x448f) */
     /* ch15 note 25 (TIME-knob push) is deliberately NOT mapped to TAP: it is
      * used as the modifier for BEAT < / BEAT > instead (see handle_fx_time).
      * Both BEAT buttons need op == press: asEventCode gates 0x4490/0x4491
      * (events 0x2015/0x2016) on (op & 0xf) == 0. */
}

static void *midi_thread(void *arg)
{
     struct snd_seq_event ev;
     const char *s;
     (void)arg;

     if (!is_rbp_process())
          return NULL;

     s = getenv("KNOB_SCALE");
     if (s) knob_scale = atoi(s);
     if (knob_scale < 1) knob_scale = 1;
     s = getenv("JOG_SCALE");
     if (s) jog_scale = atoi(s);
     if (jog_scale < 1) jog_scale = 1;
     s = getenv("JOG_PPR");
     if (s) jog_ppr = atoi(s);
     if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = getenv("JOG_REV") != NULL;
     s = getenv("JOG_IDLE_MS");
     if (s) jog_idle_ms = atoi(s);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = getenv("JOG_VERBOSE") != NULL;
     tempo_verbose = getenv("TEMPO_VERBOSE") != NULL;
     tempo_rev = getenv("TEMPO_REV") != NULL;
     verbose = getenv("KNOB_VERBOSE") != NULL;

     build_maps();
     klog("knobshim2: thread started (maps: %d notes, %d abs knobs) g_speaker_gain addr=%p cue_gain=%p cue_mix=%p\n",
          note_map_n, abs_map_n, (void *)&g_speaker_gain,
          (void *)&g_cue_gain, (void *)&g_cue_mix);

     for (int i = 0; i < 300; i++) {
          if (get_key_manager())
               break;
          usleep(100000);
     }
     if (!get_key_manager()) {
          klog("knobshim2: KeyManager never became ready\n");
          return NULL;
     }
     klog("knobshim2: KeyManager ready, opening sequencer...\n");

     /* Ensure audio routing in djengine::MixerRouteMngr:
      * On real RX3, physical DECK/LINE switches assign input routing.
      * On Prime GO without subucom switches, default routes left Channel 2 to Player 0.
      * Fix: permanently route Input 0 -> Player 0 (Deck 1) and Input 1 -> Player 1 (Deck 2).
      */
     *(volatile uint32_t *)0x01149f50 = 0x01149f08; /* Input 0 -> Player 0 */
     *(volatile uint32_t *)0x01149f54 = 0x01149f10; /* Input 1 -> Player 1 */
     klog("knobshim2: routed Mixer Ch1 -> Deck1, Ch2 -> Deck2\n");

     /* Initialize Sound Color FX to Filter on both channels so Sweep FX knob works out of the box */
     send_rx_key(K_FILTER, OP_PRESS, 1, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 1, 0);
     send_rx_key(K_FILTER, OP_PRESS, 2, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 2, 0);
     send_rx_key_f(K_COLOR, OP_VALUE, 1, 512, 0.5f);
     send_rx_key_f(K_COLOR, OP_VALUE, 2, 512, 0.5f);
     klog("knobshim2: Sound Color FX initialized to Filter on Ch1 & Ch2\n");

     /* Always set Beat FX Channel to MASTER (channel 5) */
     send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, BFX_CH_MASTER);
     klog("knobshim2: Beat FX Channel set to MASTER (5)\n");

     /* Set rbp's master level to unity.  The SC Live 4's Main Vol (CC20) is
      * applied by audioshim to ch0/1 (XLR) only, and the built-in monitors /
      * headphones have their own controls, so rbp's internal master level must
      * sit at a fixed reference or the master VU/level lags the channels. */
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     klog("knobshim2: master level set to unity\n");

     pthread_t bfx_tid;
     pthread_create(&bfx_tid, NULL, bfx_init_thread, NULL);
     pthread_detach(bfx_tid);

     seq_setup();
     if (seq_fd < 0) {
          klog("knobshim2: sequencer setup failed, giving up\n");
          return NULL;
     }
     klog("knobshim2: reading sequencer events (full Prime GO surface)\n");

     for (;;) {
          struct pollfd pfd;
          pfd.fd = seq_fd;
          pfd.events = POLLIN;
          int pr = poll(&pfd, 1, 1000);
          if (pr <= 0)
               continue;
          ssize_t n = real_read(seq_fd, &ev, sizeof(ev));
          if (n == (ssize_t)sizeof(ev))
               handle_event(&ev);
          else if (n < 0 && errno == EINTR)
               continue;
     }
     return NULL;
}

/* Stub out Pioneer PowerManager callbacks.
 * Prime GO lacks Pioneer's power manager hardware, so [UsbStorageManager+80] is NULL.
 * When a USB drive mounts/unmounts, rbp calls notifyPermissionChanged(NULL), etc. which segfaults. */
void _ZN3uif13IPowerManager23notifyPermissionChangedEv(void *this) { (void)this; }
void _ZN3uif13IPowerManager23notifyPreparedToStandbyEi(void *this, int a) { (void)this; (void)a; }
void _ZN3uif13IPowerManager21notifyAutoStandbyTimeEii(void *this, int a, int b) { (void)this; (void)a; (void)b; }
void _ZN3uif13IPowerManager22notifyScreenSaverModeEb(void *this, int a) { (void)this; (void)a; }
void _ZN3uif13IPowerManager12regPermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
void _ZN3uif13IPowerManager15removePermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
void _ZN3uif13IPowerManager12reqStandbyOnEv(void *this) { (void)this; }
void _ZN3uif13IPowerManager16prepareToStandyEv(void *this) { (void)this; }

/* USB stick auto-detection watcher thread.
 * On the Denon Prime GO there is only ONE rear USB-A port, but the Pioneer RX3
 * firmware has 2 USB ports: USB1 (kind 2 in UI, device 3) and USB2 (kind 3 in UI, device 2).
 * Pioneer's internal engine (Total_MainUsbMessageProc) writes to kind 3 (USB2), which causes
 * the UI to display a blank/phantom "USB2" and hides the USB1 stick label.
 * This thread continuously:
 *   1. Monitors for mounted Rekordbox stick (/media/usb1/sda1/PIONEER/rekordbox/export.pdb).
 *   2. Automatically redirects any kind 3 (USB2) detect flags and property info into kind 2 (USB1).
 *   3. Keeps kind 3 cleared to 0 so phantom USB2 is NEVER reported.
 *   4. Ensures USB1 detect flag = 2, uiConnectedMedia = 2 (USB1 only), browseDevice = 3 (USB1).
 *   5. Opens the Source menu on fresh attach, and clears state on detach.
 */
static void *usb_auto_thread(void *arg)
{
     (void)arg;
     int last_mounted = 0;
     for (;;) {
          usleep(100000); /* 100 ms */
          if (!get_key_manager())
               continue;

          int mounted = access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0;
          if (mounted) {
               volatile uint32_t *p_det_usb1 = (volatile uint32_t *)0x03256888;
               volatile uint32_t *p_det_usb2 = (volatile uint32_t *)0x03256944;
               volatile uint32_t *p_media    = (volatile uint32_t *)0x326f8b4;
               volatile uint32_t *p_mode     = (volatile uint32_t *)0x326f8b8;
               volatile uint32_t *p_dev      = (volatile uint32_t *)0x326f8bc;
               volatile uint32_t *p_refresh  = (volatile uint32_t *)0x326e128;

               /* Suppress phantom USB2 (kind 3) on Prime GO since it has only 1 physical port */
               if (*p_det_usb2 != 0) {
                    *p_det_usb2 = 0;
                    *p_refresh = 1;
               }

               /* When USB1 is ready (kind2=2), ensure UI knows media 2 is connected */
               if (*p_det_usb1 == 2) {
                    if (*p_media != 2) {
                         *p_media = 2;
                         *p_refresh = 1;
                    }
                    /* Ensure browse caution message is cleared so touchscreen is active */
                    volatile uint32_t *p_caution = (volatile uint32_t *)0x05a191fc;
                    if (*p_caution != 0) {
                         *p_caution = 0;
                    }
               }

               if (!last_mounted) {
                    *p_det_usb1 = 2;
                    *p_det_usb2 = 0;
                    *p_media = 2;
                    *p_dev = 3;   /* Device 3 = USB 1 */
                    *p_refresh = 1;
                    klog("knobshim2: USB1 detected -> registered (dev=3)\n");
               } else if (*p_mode == 12 && *p_dev == 0) {
                    /* On Source menu: keep USB1 (device 3) active so the stick label shows */
                    *p_media = 2;
                    *p_dev = 3;
                    *p_refresh = 1;
               }
          } else if (last_mounted) {
               /* Stick unplugged: clear detect flags */
               *(volatile uint32_t *)0x03256888 = 0;
               *(volatile uint32_t *)0x03256944 = 0;
               *(volatile uint32_t *)0x326f8b4 = 0;
               *(volatile uint32_t *)0x326e128 = 1;
               klog("knobshim2: USB removed\n");
          }
          last_mounted = mounted;
     }
     return NULL;
}

/* =====================================================================
 * SC Live 4 LED output bridge
 * ---------------------------------------------------------------------
 * On the SC Live 4 EVERY front-panel LED is driven by MIDI: Engine OS sends
 * Note On/Off to the "Control Surface" (rawmidi hw:0,0 = seq 16:0).  rbp,
 * however, drives the XDJ-RX3's EUP/SUB micons over /dev/subucom_spi*.0,
 * which fix-dev.sh creates as dead FIFOs here — so rbp computes LED state
 * (uif::LedStat, uif::panel_protocol::EupMiconTx/SubMiconTx) but it never
 * reaches the panel.
 *
 * This bridge reads rbp's own engine state through the PlayEngine singleton
 * (the object djengine::DjEngineIF::isPlaying()/isSyncOn()/... delegate to)
 * and mirrors the transport LEDs onto the SC Live 4 notes for the same
 * buttons (deck channels 4/5).
 *
 * Verified on-device: Note On ch4/note10 vel 0x7F = PLAY LED bright,
 * Note Off = dark; no loopback into the Control Surface input path.
 * ===================================================================== */
#define MIDI_LED_DEV       "/dev/snd/midiC0D0"
#define PLAYENGINE_GLOBAL  0x011497d0UL   /* djengine PlayEngine singleton */
#define PE_ISPLAYING       0x5d880
#define PE_ISLOADED        0x5d3b0
#define PE_ISSYNCON        0x5ff28
#define PE_ISMASTERTEMPO   0x5dfa8
#define PE_ISVINYLMODE     0x5e260
#define PE_ISSLIPMODEON    0x5fc60
#define PE_ISLOOPING       0x5eadc
#define PE_ISCANRELOOP     0x5eab0
#define PE_ISAUTOBEATLOOP  0x5ef74
#define PE_ISLOOPINADJ     0x5f000
#define PE_ISLOOPOUTADJ    0x5f02c

/* JP21 note numbers of the deck LEDs (same notes the buttons send) */
#define LED_N_SYNC      8
#define LED_N_CUE       9
#define LED_N_PLAY      10
#define LED_N_KEYLOCK   34
#define LED_N_VINYL     35
#define LED_N_SLIP      36
#define LED_N_LOOPIN    37
#define LED_N_LOOPOUT   38
#define LED_N_AUTOLOOP  39

#define LED_COUNT       9

static const int led_notes[LED_COUNT] = {
     LED_N_SYNC, LED_N_CUE, LED_N_PLAY, LED_N_KEYLOCK, LED_N_VINYL,
     LED_N_SLIP, LED_N_LOOPIN, LED_N_LOOPOUT, LED_N_AUTOLOOP
};

static int led_fd = -1;
static int led_verbose = 0;
static int led_disabled = 0;
static int led_debug_loop = 0;
static int led_dump = 0;
static int led_sweep = 0;
static unsigned long led_tick = 0;        /* 50 ms ticks, for blink */
static signed char led_last[2][LED_COUNT]; /* [deck][led] -1 = unknown */
static int led_prev_looping[2];
static int led_dbg_last[2];               /* last logged loop-state bitmask */
static int led_blink_phase;               /* current blink phase (0/1) */
static signed char led_pfl_last[2] = { -1, -1 };  /* mixer PFL LED state */

/* JP21 RGB performance pads: notes 15..22 per deck.  rbp's pad LEDs are
 * LedDef::ID 18..25 (confirmed in ui::Player::checkLedStat, which calls
 * checkHotCueLedState(..., 18..25)); other pad modes reuse the same ids and
 * just change state/color. */
#define LED_PAD_FIRST   18
#define LED_PAD_COUNT   8
static int led_pad_last[2][LED_PAD_COUNT];  /* last MIDI velocity, -1 unknown */

/* rbp LedStat ids we have identified (channels 1/2 = deck 1/2):
 *   49 = deck PLAY  (state 2 while paused  -> panel must blink)
 *    4 = SYNC       3 = VINYL      6 = KEY LOCK      11 = SLIP */
#define LEDSTAT_PLAY   49
#define LEDSTAT_SYNC   4
#define LEDSTAT_VINYL  3
#define LEDSTAT_MT     6
#define LEDSTAT_SLIP   11

/* ---- rbp's own LED table (uif::LedStat) -------------------------------
 * IUiObjManager::getLedManager() (0x31deb0) is:
 *    r3 = *(0x026867c0);  r3 = *(r3 + 104);  return r3;
 * and LedManager::refStatesNoUpdate() (0x33e3ec) is `add r0,r0,#0x30`, so
 * the LedStat lives at LedManager+0x30.  Its layout (from
 * LedStat::setLedState / Led::setState):
 *    +4  u16   number of Led entries
 *    +8  Led*  array base
 *    +14 u16   stride (u16 slots per LedDef::ID)
 *  and each Led entry (0x2c bytes) begins { +0 u32 id, +4 u32 channel,
 *  +16 u32 State }.  Every LED write in rbp funnels through
 *  LedStat::setLedState (45 sites) / setLedState_Color (31 sites), so this
 *  table is the complete, authoritative LED state. */
#define LEDMGR_HOLDER_GLOBAL 0x026867c0UL
#define LEDMGR_OFF_LEDSTAT   104
#define LEDSTAT_OFF          0x30
#define LED_ENTRY_SIZE       0x2c
#define LED_DUMP_MAX         256

static unsigned char *ledstat_ptr(void)
{
     void *holder = *(void **)LEDMGR_HOLDER_GLOBAL;
     void *ledmgr;
     if (!holder)
          return NULL;
     ledmgr = *(void **)((char *)holder + LEDMGR_OFF_LEDSTAT);
     if (!ledmgr)
          return NULL;
     return (unsigned char *)ledmgr + LEDSTAT_OFF;
}

/* Look up rbp's own state for one (id, channel) LED.
 * Returns 0=off, 1=solid, 2=blink, 3=dim, or -1 if rbp has no such entry. */
static int ledstat_state(unsigned int id, unsigned int ch)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return -1;
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return -1;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          if (*(unsigned int *)(e + 0) == id && *(unsigned int *)(e + 4) == ch)
               return (int)*(unsigned int *)(e + 16);
     }
     return -1;
}

/* Read the stored RGB (3 bytes at Led entry +40/+41/+42; rbp's ColorLed::Rgb
 * is 0..255 per channel).  Returns 0 if rbp has no entry for (id, ch). */
static int ledstat_rgb(unsigned int id, unsigned int ch, unsigned char *out)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return 0;
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return 0;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          if (*(unsigned int *)(e + 0) == id && *(unsigned int *)(e + 4) == ch) {
               out[0] = e[40]; out[1] = e[41]; out[2] = e[42];
               return 1;
          }
     }
     return 0;
}

static void led_dump_scan(void)
{
     /* keyed by (id, channel) so table reordering does not produce false
      * changes; state 0xff = "not seen yet" */
     static unsigned char s_state[256][4];
     static int s_init = 0;
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;

     if (!ls)
          return;
     if (!s_init) {
          memset(s_state, 0xff, sizeof(s_state));
          s_init = 1;
     }
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return;

     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          unsigned int id = *(unsigned int *)(e + 0);
          unsigned int ch = *(unsigned int *)(e + 4);
          unsigned int st = *(unsigned int *)(e + 16);
          if (id >= 256 || ch >= 4 || st > 0xff)
               continue;
          if (s_state[id][ch] == (unsigned char)st)
               continue;
          klog("knobshim2: leddump id=%u ch=%u state %u -> %u rgb=%u,%u,%u\n",
               id, ch, s_state[id][ch] == 0xff ? 0 : s_state[id][ch], st,
               e[40], e[41], e[42]);
          s_state[id][ch] = (unsigned char)st;
     }
}

static int led_send(int midi_ch, int note, int vel)
{
     unsigned char m[3];
     if (led_fd < 0)
          return 0;
     m[0] = (unsigned char)((vel > 0 ? 0x90 : 0x80) | (midi_ch & 0x0f));
     m[1] = (unsigned char)(note & 0x7f);
     m[2] = (unsigned char)(vel & 0x7f);
     return write(led_fd, m, 3) == 3;
}

static void led_apply(int deck, int idx, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (led_last[deck][idx] == want)
          return;
     if (!led_send(4 + deck, led_notes[idx], on ? 0x7f : 0x00))
          return;                        /* retry next tick */
     led_last[deck][idx] = want;
     if (led_verbose)
          klog("knobshim2: led deck%d note%d %s\n",
               deck + 1, led_notes[idx], on ? "on" : "off");
}

/* Prefer rbp's own state for an LED; fall back to a derived value when rbp
 * has no entry for it yet.  State 2 is rbp's blink request (e.g. SYNC blinks
 * when synced but the platter was nudged off beat), so we drive the panel
 * blink ourselves at the same cadence. */
static void led_from_table(int deck, int idx, unsigned int id, int fallback)
{
     int st = ledstat_state(id, (unsigned int)deck + 1);
     int on;
     if (st < 0)
          on = fallback;
     else if (st == 2)
          on = led_blink_phase;
     else
          on = (st != 0);
     led_apply(deck, idx, on);
}

static int midi_note(int midi_ch, int note, int vel)
{
     unsigned char m[3];
     if (led_fd < 0)
          return 0;
     m[0] = (unsigned char)((vel > 0 ? 0x90 : 0x80) | (midi_ch & 0x0f));
     m[1] = (unsigned char)(note & 0x7f);
     m[2] = (unsigned char)(vel & 0x7f);
     return write(led_fd, m, 3) == 3;
}

/* SC Live 4 pad colour = Note On velocity.  Per the Engine OS Prime LED
 * convention, bits 4-5 = red, 2-3 = green, 0-1 = blue (2 bits each).  rbp
 * keeps 0..255 per channel, so take the top 2 bits.  Some builds want bit 6
 * (0x40) set for the bright range; PAD_BRIGHT=1 enables that (default: pure
 * 6-bit colour). */
static int pad_bright_bit = -1;

static unsigned char pad_encode_rgb(int r, int g, int b)
{
     unsigned char v = (unsigned char)(((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6));
     if (pad_bright_bit < 0) {
          const char *s = getenv("PAD_BRIGHT");
          pad_bright_bit = (s && *s) ? (atoi(s) != 0) : 0;
     }
     if (pad_bright_bit)
          v = (unsigned char)(v | 0x40);
     return v;
}

static void led_pad_apply(int deck, int pad, unsigned char vel)
{
     if (led_pad_last[deck][pad] == (int)vel)
          return;
     if (!midi_note(4 + deck, 15 + pad, vel))
          return;                        /* retry next tick */
     led_pad_last[deck][pad] = vel;
     if (led_verbose)
          klog("knobshim2: pad deck%d pad%d vel=0x%02x\n",
               deck + 1, pad + 1, vel);
}

/* Global (channel-15) panel LEDs, driven straight from rbp's LedStat.
 * The LedStat id is LedDef::ID + 8 for this group (verified live):
 *   EffectOnOff 40 -> 48, CfxFilter 33 -> 41, CfxSweep 34 -> 42,
 *   CfxDubEcho 35 -> 43, CfxNoise 36 -> 44.
 * State 2 = rbp wants a blink (e.g. the FX ON/OFF LED blinks while the
 * effect is active), so we drive the panel blink ourselves. */
static const struct { unsigned int id; int note; const char *name; } led_g_tab[] = {
     { 48, 26, "BfxOnOff" },
     { 41, 21, "CfxFilter" },
     { 43, 22, "CfxDubEcho" },
     { 44, 23, "CfxNoise" },
     { 42, 24, "CfxSweep" },
};
#define LEDG_COUNT ((int)(sizeof(led_g_tab) / sizeof(led_g_tab[0])))
static signed char led_last_g[LEDG_COUNT];

static void led_apply_g(int idx, int note, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (led_last_g[idx] == want)
          return;
     if (!midi_note(15, note, on ? 0x7f : 0x00))
          return;
     led_last_g[idx] = want;
     if (led_verbose)
          klog("knobshim2: led global note%d %s (%s)\n",
               note, on ? "on" : "off", led_g_tab[idx].name);
}

static void led_refresh(void)
{
     void *pe;
     int blink;
     if (led_disabled || led_fd < 0)
          return;
     blink = (led_tick & 8) ? 1 : 0;      /* ~400 ms on / off */
     led_blink_phase = blink;

     /* global LEDs, straight from rbp (id/ch -> panel note) */
     for (int g = 0; g < LEDG_COUNT; g++) {
          int st = ledstat_state(led_g_tab[g].id, 0);
          int on = (st < 0) ? 0 : (st == 2 ? blink : (st != 0));
          led_apply_g(g, led_g_tab[g].note, on);
     }

     /* mixer PFL LEDs (SC Live 4 strips 1/2, note 13) from rbp's cue state */
     for (int m = 0; m < 2; m++) {
          int cue = me_get_cue(m);
          if (cue < 0)
               continue;
          if (led_pfl_last[m] != (signed char)cue) {
               if (midi_note(m, 13, cue ? 0x7f : 0x00))
                    led_pfl_last[m] = (signed char)cue;
          }
     }
     /* master cue LED on strips 3/4 (note 13) */
     {
          static signed char mc_last = -1;
          int mc = me_get_master_cue();
          if (mc >= 0 && mc_last != (signed char)mc) {
               if (midi_note(2, 13, mc ? 0x7f : 0x00) &&
                   midi_note(3, 13, mc ? 0x7f : 0x00))
                    mc_last = (signed char)mc;
          }
     }

     pe = *(void **)PLAYENGINE_GLOBAL;
     if (!pe)
          return;
     for (int i = 0; i < 2; i++) {
          int playing = ((int (*)(void *, int))PE_ISPLAYING)(pe, i) != 0;
          int loaded  = ((int (*)(void *, int))PE_ISLOADED)(pe, i) != 0;
          int sync    = ((int (*)(void *, int))PE_ISSYNCON)(pe, i) != 0;
          int mt      = ((int (*)(void *, int))PE_ISMASTERTEMPO)(pe, i) != 0;
          int vinyl   = ((int (*)(void *, int))PE_ISVINYLMODE)(pe, i) != 0;
          int slip    = ((int (*)(void *, int))PE_ISSLIPMODEON)(pe, i) != 0;
          int looping = ((int (*)(void *, int))PE_ISLOOPING)(pe, i) != 0;
          int canrel  = ((int (*)(void *, int))PE_ISCANRELOOP)(pe, i) != 0;
          int aloop   = ((int (*)(void *, int))PE_ISAUTOBEATLOOP)(pe, i) != 0;
          int armed;

          /* a loop that ends clears the "loop-in armed" latch */
          if (led_prev_looping[i] && !looping)
               led_loop_armed[i] = 0;
          led_prev_looping[i] = looping;
          /* Blink only while a loop is actually running, or while a loop-in
           * point has been set but not yet closed.  A loop that still exists
           * after exit (isPossibleToReLoop) must NOT keep the LED blinking. */
          armed = led_loop_armed[i];

          /* SYNC comes from rbp's own LED state (id 4), so the three states
           * survive: off / solid (locked) / blink (synced but nudged off
           * beat).  Falls back to isSyncOn() if rbp has no entry. */
          led_from_table(i, 0, LEDSTAT_SYNC, sync);
          led_apply(i, 1, loaded && !playing);        /* CUE */
          /* PLAY: solid while playing, blinks while paused on a loaded
           * track, dark with nothing loaded. */
          led_apply(i, 2, playing ? 1 : (loaded ? blink : 0));
          led_apply(i, 3, mt);                        /* KEY LOCK */
          led_apply(i, 4, vinyl);                     /* VINYL */
          led_apply(i, 5, slip);                      /* SLIP */
          /* SC Live 4 convention (verified against Engine OS on video):
           *   idle            -> both LEDs solid ON
           *   loop-in set     -> LOOP IN blinks, LOOP OUT solid
           *   loop running    -> both blink */
          led_apply(i, 6, (looping || armed) ? blink : 1);   /* LOOP IN */
          led_apply(i, 7, looping ? blink : 1);              /* LOOP OUT */
          led_apply(i, 8, aloop);                            /* AUTO LOOP */

          /* RGB performance pads: rbp LedDef::ID 18..25 -> notes 15..22 */
          for (int p = 0; p < LED_PAD_COUNT; p++) {
               unsigned char rgb[3];
               int st = ledstat_state(LED_PAD_FIRST + (unsigned)p, (unsigned)i + 1);
               if (st <= 0 ||
                   !ledstat_rgb(LED_PAD_FIRST + (unsigned)p, (unsigned)i + 1, rgb)) {
                    led_pad_apply(i, p, 0);
               } else if (st == 2) {
                    led_pad_apply(i, p, blink
                         ? pad_encode_rgb(rgb[0], rgb[1], rgb[2]) : 0);
               } else {
                    led_pad_apply(i, p, pad_encode_rgb(rgb[0], rgb[1], rgb[2]));
               }
          }

          if (led_debug_loop) {
               int bits = (looping << 0) | (canrel << 1) | (armed << 2);
               if (bits != led_dbg_last[i]) {
                    led_dbg_last[i] = bits;
                    klog("knobshim2: loopdbg d%d loop=%d canrel=%d armed=%d "
                         "inadj=%d outadj=%d aloop=%d loaded=%d "
                         "ls49=%d ls53=%d ls55=%d\n",
                         i + 1, looping, canrel, armed,
                         ((int (*)(void *, int))PE_ISLOOPINADJ)(pe, i),
                         ((int (*)(void *, int))PE_ISLOOPOUTADJ)(pe, i),
                         aloop, loaded,
                         ledstat_state(49, (unsigned int)i + 1),
                         ledstat_state(53, (unsigned int)i + 1),
                         ledstat_state(55, (unsigned int)i + 1));
               }
          }
     }
}

/* full table snapshot (for sweep attribution) */
static void led_dump_full(const char *tag)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return;
     count = *(unsigned short *)(ls + 4);
     klog("knobshim2: snapshot %s count=%u\n", tag, count);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          klog("knobshim2: snap %s id=%u ch=%u state=%u\n", tag,
               *(unsigned int *)(e + 0), *(unsigned int *)(e + 4),
               *(unsigned int *)(e + 16));
     }
}

/* Discovery sweep: drive rbp's own keycodes and watch which LedStat entries
 * change, so each LED id can be mapped to its JP21 note unambiguously. */
static const struct { int key; const char *name; } led_sweep_tab[] = {
     { 0x4101, "PLAY" },   { 0x4102, "CUE" },    { 0x410c, "LOOPIN" },
     { 0x410d, "LOOPOUT" },{ 0x4112, "SYNC" },   { 0x4114, "ALOOP" },
     { 0x4110, "SLIP" },   { 0x4108, "MT" },     { 0x4104, "VINYL" },
     { 0x4113, "HOTCUE" }, { 0x4115, "SLIPLOOP" },{ 0x4116, "BEATJUMP" },
     { 0x410e, "RELOOP" }, { 0x410f, "REV" },    { 0x4111, "MASTER" },
};

static void led_sweep_run(void)
{
     unsigned int n = sizeof(led_sweep_tab) / sizeof(led_sweep_tab[0]);
     unsigned int i;
     int w;

     /* wait until rbp's LED table is populated and stable */
     for (w = 0; w < 150; w++) {
          unsigned char *ls = ledstat_ptr();
          if (ls && *(unsigned short *)(ls + 4) >= 8)
               break;
          usleep(200000);
     }
     usleep(1500000);

     for (int pass = 0; pass < 2; pass++) {
     for (i = 0; i < n; i++) {
          char tag[32];
          snprintf(tag, sizeof(tag), "p%d%s-0", pass, led_sweep_tab[i].name);
          led_dump_full(tag);
          klog("knobshim2: sweep >>> p%d %s 0x%04x\n", pass,
               led_sweep_tab[i].name, led_sweep_tab[i].key);
          send_rx_key(led_sweep_tab[i].key, OP_PRESS, 1, 0);
          usleep(300000);
          send_rx_key(led_sweep_tab[i].key, OP_RELEASE, 1, 0);
          usleep(1800000);
          snprintf(tag, sizeof(tag), "p%d%s-1", pass, led_sweep_tab[i].name);
          led_dump_full(tag);
     }
     }
     klog("knobshim2: sweep done\n");
}

static void *led_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     led_verbose = getenv("LED_VERBOSE") != NULL;
     led_disabled = getenv("LED_DISABLE") != NULL;
     led_debug_loop = getenv("LED_DEBUG_LOOP") != NULL;
     led_dump = getenv("LED_DUMP") != NULL;
     led_sweep = getenv("LED_SWEEP") != NULL;
     if (led_disabled)
          return NULL;

     for (int i = 0; i < 300 && led_fd < 0; i++) {
          led_fd = real_open(MIDI_LED_DEV, O_WRONLY | O_NONBLOCK);
          if (led_fd >= 0)
               break;
          usleep(100000);
     }
     if (led_fd < 0) {
          klog("knobshim2: LED device %s unavailable\n", MIDI_LED_DEV);
          return NULL;
     }
     memset(led_last, -1, sizeof(led_last));
     memset(led_pad_last, -1, sizeof(led_pad_last));
     /* dump rbp's mixer channel -> EnMixerInput map once the engine exists */
     for (int w = 0; w < 100 && !mixer_engine(); w++)
          usleep(100000);
     {
          void *me = mixer_engine();
          klog("knobshim2: mixer engine=%p chans=%d inputs=", me, me_channel_count());
          for (int i = 0; i < 8; i++) {
               int in = me ? me_channel_input(me, i) : -1;
               if (in >= 0)
                    klog("%d ", in);
          }
          klog("\n");
     }
     klog("knobshim2: LED bridge up (%s)\n", MIDI_LED_DEV);

     for (;;) {
          if (led_dump)
               led_dump_scan();
          led_refresh();
          led_tick++;
          usleep(50000);   /* 20 Hz */
     }
     return NULL;
}

static void *led_sweep_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     usleep(4000000);          /* let the UI settle first */
     led_sweep_run();
     return NULL;
}

static int midi_cc(int midi_ch, int cc, int val)
{
     unsigned char m[3];
     if (led_fd < 0)
          return 0;
     m[0] = (unsigned char)(0xB0 | (midi_ch & 0x0f));
     m[1] = (unsigned char)(cc & 0x7f);
     m[2] = (unsigned char)(val & 0x7f);
     return write(led_fd, m, 3) == 3;
}

/* Ask the panel to report every absolute control (faders/knobs) — Engine OS
 * does this at startup (`queryAbsoluteControls()` in JP21_Controller_Device.qml).
 * Without it the fader positions are unknown until first moved, which made the
 * channel meters ignore the fader. */
static void led_query_absolute(void)
{
     static const unsigned char sysex[10] = {
          0xF0, 0x00, 0x02, 0x0B, 0x7F, 0x12, 0x04, 0x00, 0x00, 0xF7
     };
     int i;
     if (led_fd < 0)
          return;
     /* Invalidate the CC cache first: the panel answers with the SAME physical
      * values, so handle_cc_abs()'s "only send on change" test would drop the
      * reply and a fader/EQ that rbp reset during its later mixer init would
      * never be re-applied (the control then reads default until moved once). */
     for (i = 0; i < abs_map_n; i++)
          abs_map[i].last = -1;
     (void)write(led_fd, sysex, sizeof(sysex));
}

/* Map a linear master peak (S24 full scale) to the SC Live 4 meter bitmask.
 * dBThresholds from JP21_Controller_Assignments.qml (Master VUMeter). */
static int vu_segments(int peak)
{
     static const double th[7] = { -45.7, -25.7, -12.2, -7.2, -4.2, -0.2, 20.0 };
     double db;
     int n = 0, i;
     if (peak <= 0)
          return 0;
     db = 20.0 * log10((double)peak / 8388607.0);
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               n = i + 1;
     if (n > 6)
          n = 6;                    /* ledCCValues has 6 usable segments */
     return (1 << n) - 1;
}

/* ---- meter hook: ui::Mixer::MonoLvMeter::getLedValue(unsigned char) -----
 * rbp computes every meter's LED bitmask in this one function, and
 * ui::Mixer::checkLedStat calls it three times:
 *   master @0x2d093c   ch1 @0x2d0a40   ch2 @0x2d0a88
 * We patch the function prologue to jump to our hook, so we get rbp's own
 * meter reading (no guessing at level units), while still running the
 * original via a trampoline.  The caller's return address tells us which
 * meter it is. */
#define ADDR_GETLEDVALUE   0x2d07a8UL
#define RET_MASTER         0x2d0940UL
#define RET_CH1            0x2d0a44UL
#define RET_CH2            0x2d0a8cUL
#define PROLOGUE_GETLED    0xe92d4070u   /* push {r4,r5,r6,lr} */

static volatile unsigned int g_meter_bits[3];   /* 0=master 1=ch1 2=ch2 */
static unsigned int (*g_orig_getled)(void *, unsigned char);
static unsigned char *g_tramp;

unsigned int getled_hook(void *self, unsigned char level);

static int popcount32(unsigned int v)
{
     int n = 0;
     while (v) { n += v & 1; v >>= 1; }
     return n;
}

static void install_meter_hook(void)
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
     g_orig_getled = (unsigned char (*)(void *, unsigned char))g_tramp;

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

/* ui::Mixer's cached meter levels: uiobj = *(0x026867c0); mixer = *(uiobj+80);
 * lvl = *(long**)(mixer+0xb8)  (see IUiObjManager::getMixer @0x31e054 and
 * ui::Mixer::checkLedStat @0x2d0874). */
#define UI_OBJ_MGR_HOLDER 0x026867c0UL
#define MIXER_OFF         80
#define MIXER_LVLOFF      0xb8

/* Map a meter level (rbp's dB-ish unit) to the SC Live 4 channel-meter
 * bitmask; thresholds from the JP21 Channel VUMeter. */
static int vu_db_to_segments(long db)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n = 0, i;
     for (i = 0; i < 7; i++)
          if ((double)db >= th[i])
               n = i + 1;
     if (n > 6)
          n = 6;
     return (1 << n) - 1;
}

/* Channel meter.  rbp's meter has 11 segments (LED_TABLE = (1<<n)-1 for
 * n = 0..11); the SC Live 4 panel has 6 (4 white + 1 blue + 1 orange).  So
 * rescale 11 -> 6 (never cap - capping made the panel hit full at 6/11), then
 * apply the channel fader (rbp meters pre-fader; 0 dB top -> -60 dB bottom). */
#define RBP_METER_SEGMENTS 11
static int vu_channel_segments(unsigned int bits, int fader10)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n_rbp = popcount32(bits);
     int n_sc, i, out = 0;
     double db, f;
     if (n_rbp <= 0)
          return 0;
     if (n_rbp > RBP_METER_SEGMENTS)
          n_rbp = RBP_METER_SEGMENTS;
     n_sc = (n_rbp * 6 + 5) / RBP_METER_SEGMENTS;    /* round to 0..6 */
     if (n_sc <= 0)
          return 0;
     if (n_sc > 6)
          n_sc = 6;
     db = th[n_sc - 1];                     /* approx dB of top lit segment */
     f = (double)fader10 / 1023.0;
     if (f <= 0.0)
          return 0;
     if (f > 1.0)
          f = 1.0;
     db -= 60.0 * (1.0 - f);                /* fader attenuation */
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               out = i + 1;
     if (out > 6)
          out = 6;
     return (1 << out) - 1;
}

/* Master meter: use rbp's OWN master meter (same source as the channel meters,
 * so they read consistently), with the Main Vol applied in dB.  Falls back to
 * the audioshim peak only if rbp's master meter is not captured. */
static int vu_master_segments(void)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n_rbp = popcount32(g_meter_bits[0]);
     int n_sc, i, out = 0;
     double db, g = (double)g_master_gain;
     if (n_rbp <= 0)
          return 0;
     if (n_rbp > RBP_METER_SEGMENTS)
          n_rbp = RBP_METER_SEGMENTS;
     n_sc = (n_rbp * 6 + 5) / RBP_METER_SEGMENTS;
     if (n_sc <= 0)
          return 0;
     if (n_sc > 6)
          n_sc = 6;
     db = th[n_sc - 1];
     if (g <= 0.0001)
          return 0;
     if (g > 1.0)
          g = 1.0;
     db += 20.0 * log10(g);                 /* Main Vol attenuation */
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               out = i + 1;
     if (out > 6)
          out = 6;
     return (1 << out) - 1;
}

static void *vu_thread(void *arg)
{
     unsigned long t = 0;
     int last_l = -1, last_r = -1;
     int last_c[3] = { -1, -1, -1 };
     int vu_test, vu_debug;
     (void)arg;
     if (!is_rbp_process() || getenv("VU_DISABLE"))
          return NULL;
     vu_test = getenv("VU_TEST") != NULL;
     vu_debug = getenv("VU_DEBUG") != NULL;
     for (int i = 0; i < 300 && led_fd < 0; i++)
          usleep(100000);
     if (led_fd < 0)
          return NULL;
     install_meter_hook();
     /* Ask for the physical control positions only once rbp can accept them:
      * if the reply lands before rbp's mixer exists, the values are dropped and
      * rbp initialises the faders/EQs to their defaults (the fader then reads
      * "down" until it is moved once). */
     for (int i = 0; i < 300 && !get_key_manager(); i++)
          usleep(100000);
     led_query_absolute();
     klog("knobshim2: VU bridge up%s\n", vu_test ? " (TEST sweep)" : "");

     for (;;) {
          int l, r;
          /* enable rbp's master cue once the engine exists, so the headphone
           * bus contains the master (SC Live 4 has no MASTER CUE button) */
          static int mc_init = 0;
          if (!mc_init && mixer_engine()) {
               mc_init = 1;
               me_set_master_cue(1);
               me_set_stereo(1);     /* default: stereo (0 = mono split) */
               klog("knobshim2: master cue enabled at startup\n");
          }
          if (vu_test) {
               int seg = (int)((t / 40) % 8);      /* 0..7 segments, ~1 s/step */
               l = r = (seg > 6) ? 63 : ((1 << seg) - 1);
          } else {
               if (g_meter_bits[0]) {
                    l = r = vu_master_segments();
               } else {
                    l = vu_segments(g_vu_peak[0]);
                    r = vu_segments(g_vu_peak[1]);
               }
          }
          if (l != last_l) {
               midi_cc(15, 32, l);
               last_l = l;
          }
          if (r != last_r) {
               midi_cc(15, 33, r);
               last_r = r;
          }

          /* Channel meters 1/2 come straight from rbp's own meter bitmask
           * (g_meter_bits, filled by the getLedValue hook): the number of
           * segments rbp lit is scaled to the SC Live 4 6-segment meter. */
          for (int m = 1; m <= 2; m++) {
               int v = vu_channel_segments(g_meter_bits[m], g_fader[m]);
               if (v != last_c[m]) {
                    midi_cc(m - 1, 10, v);     /* CC10 on ch0 = ch1, ch1 = ch2 */
                    last_c[m] = v;
               }
          }
          if (vu_debug && (t % 40) == 0)
               klog("knobshim2: vudbg master=%03x ch1=%03x ch2=%03x seg=%d/%d "
                    "fader=%d/%d seen=%d/%d\n",
                    g_meter_bits[0], g_meter_bits[1], g_meter_bits[2],
                    last_c[1], last_c[2],
                    g_fader[1], g_fader[2], g_fader_seen[1], g_fader_seen[2]);
          /* Re-assert the physical fader/EQ/trim positions for the first
           * ~30 s.  rbp finishes initialising its mixer well after we are
           * loaded, and any value sent before that is lost - which is why a
           * fader already up at startup played back quiet until moved once.
           * The window is deliberately short: the panel just reports where
           * the controls physically are, but we don't want to keep re-sending
           * while the user is actively working. */
          if (t < 1200 && (t % 80) == 0) {
               led_query_absolute();
               /* rbp's mixer inits after we load, so the one-shot unity master
                * level in init() is dropped - re-assert it for ~30 s. */
               send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
          } else if ((!g_fader_seen[1] || !g_fader_seen[2]) && (t % 80) == 0)
               led_query_absolute();
          t++;
          usleep(25000);   /* 40 Hz */
     }
     return NULL;
}

/* Fires SYNC-hold -> MASTER as soon as the hold threshold is reached, without
 * waiting for the button to be released. */
static void *sync_hold_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     for (;;) {
          for (int d = 0; d < 2; d++) {
               if (sync_held[d] && !sync_hold_fired[d] &&
                   now_ms() - sync_press_ms[d] >= SYNC_HOLD_MS) {
                    sync_hold_fired[d] = 1;
                    send_rx_key(K_MASTER, OP_PRESS, d + 1, 0);
                    send_rx_key(K_MASTER, OP_RELEASE, d + 1, 0);
                    if (verbose)
                         klog("knobshim2: deck%d SYNC held -> MASTER 0x4111\n",
                              d + 1);
               }
          }
          usleep(20000);   /* 50 Hz */
     }
     return NULL;
}

__attribute__((constructor))
static void knobshim2_init(void)
{
     pthread_t tid;
     led_sweep = getenv("LED_SWEEP") != NULL;   /* must be known before
                                                 * spawning the sweep thread */
     if (pthread_create(&tid, NULL, vu_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, sync_hold_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, midi_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, led_thread, NULL) == 0)
          pthread_detach(tid);
     if (led_sweep && pthread_create(&tid, NULL, led_sweep_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, jog_idle_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, usb_auto_thread, NULL) == 0)
          pthread_detach(tid);
}
