/*
 * test_midi.c — the controls port's MIDI path, end to end, with no hardware.
 *
 * The shim has two halves that are checkable away from a deck. The other two
 * tests take the first one: s24pack's byte contract (test_audio.c) and the
 * pointer transform (test_point.c). This is the second. It replays a dump
 * through the REAL JP21 map — map_jp21.o, ctrl_map.o, mididump.o, shimutil.o and
 * shmstate.o are the objects that ship, not copies — and pins the keycodes that
 * come out.
 *
 * Three things are being checked, and they are worth keeping separate in your
 * head because they fail for different reasons:
 *
 *   1. The FORMAT round-trips. Every line of tests/midi_jp21.dump is parsed and
 *      re-rendered, and the two must be identical, so a change to the writer that
 *      the reader does not follow is caught here rather than by a dump nobody can
 *      replay.
 *
 *   2. MIDI_REPLAY reaches the map. The dispatch path below is the two steps
 *      ctrlshim.c's ctrl_dispatch() performs: the KeyManager gate, then the map.
 *      (The third step there — the MIDI_DUMP recorder — is deliberately above the
 *      gate and is not part of what a replay exercises.)
 *
 *   3. The map produces the keycodes its tables say it should. This is the
 *      assertion with teeth: a JP21 note that stops reaching rbp, a latch that
 *      stops latching, a CC that stops being quantised, a SYNC tap whose release
 *      leaks an early press — all of them show up as a row of the expectation
 *      table that does not match.
 *
 * What is NOT checked here, and cannot be:
 *
 *   - Anything that touches an absolute rbp address. The bridge is stubbed below,
 *     and the fixture avoids the two events that bypass it — see the fixture's
 *     header.
 *   - Anything that is timing. MIDI_REPLAY_SPEED's pacing is a nanosleep; the
 *     fixture's gaps are recorded so that a paced replay is exercised too, but
 *     its wall-clock behaviour is not asserted, because that is a stopwatch and
 *     not a contract. The same goes for the jog's speed, which is derived from
 *     real elapsed time: only its sign is pinned (inside the ±8 that rbp clamps
 *     to), not its magnitude.
 *
 * One side effect worth knowing about: klog() is unconditional, and the two
 * switches the fixture toggles log on every edge, so running this appends a few
 * lines to /tmp/knobshim.log on the build host.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sound/asequencer.h>

#include "rbp_abi.h"       /* the K_..., OP_... and CH_... names the map speaks */
#include "rbp_bridge.h"    /* the keycode calls, and the signatures to match */
#include "shmstate.h"      /* the gains and switches the map writes */
#include "rbp_led.h"       /* led_loop_armed */
#include "rbp_vu.h"        /* g_fader, g_fader_seen */
#include "ctrl_map.h"
#include "mididump.h"

#ifndef MIDI_FIXTURE
#error "MIDI_FIXTURE must name the dump to replay (-DMIDI_FIXTURE=...); see the Makefile"
#endif

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* ==========================================================================
 * The globals the map writes, defined here because the modules that own them
 * (rbp_led.c, rbp_vu.c, ctrlshim.c) are not part of this link.
 *
 * Each is declared by the header included above rather than re-declared, so a
 * change to one of those types fails to compile here instead of quietly
 * disagreeing about the size of an array.
 * ========================================================================== */

int verbose = 0;          /* every klog() in the map is gated on this */
int aloop_enabled = -1;   /* jp21_build() sets it; rbp_bridge.c gates on it */

int led_loop_armed[2];

int g_fader[3] = { 1023, 1023, 1023 };
int g_fader_seen[3];

/* ==========================================================================
 * The bridge: every call the map makes that would otherwise reach rbp or the
 * panel.
 * ========================================================================== */

/* ---- the output of the shim, recorded rather than sent ------------------- */

struct sent {
     int kind;        /* 0 = send_rx_key, 1 = send_rx_key_f, 2 = send_rx_key_fl */
     int key;
     int op;
     int ch;
     long param;
     float fval;
     long lval;
};

#define SENT_MAX 64
static struct sent sent[SENT_MAX];
static int sent_n;

static void record(int kind, int key, int op, int ch, long param, float fval,
                   long lval)
{
     if (sent_n >= SENT_MAX) {
          printf("FAIL: the fixture sent more than %d keycodes\n", SENT_MAX);
          exit(1);
     }
     sent[sent_n].kind = kind;
     sent[sent_n].key = key;
     sent[sent_n].op = op;
     sent[sent_n].ch = ch;
     sent[sent_n].param = param;
     sent[sent_n].fval = fval;
     sent[sent_n].lval = lval;
     sent_n++;
}

void send_rx_key(int keycode, int op, int ch, long param)
{
     record(0, keycode, op, ch, param, 0.0f, 0);
}

void send_rx_key_f(int keycode, int op, int ch, long param, float fval)
{
     record(1, keycode, op, ch, param, fval, 0);
}

void send_rx_key_fl(int keycode, int op, int ch, long param, float fval,
                    long lval)
{
     record(2, keycode, op, ch, param, fval, lval);
}

/* ---- rbp ---------------------------------------------------------------- */

/* A non-NULL sentinel: the dispatcher's gate only asks whether rbp's KeyManager
 * exists, and the map must be reached. */
void *get_key_manager(void)
{
     return (void *)&checks;
}

/* cc_to_10bit() is the one stub that is a formula rather than a no-op, and it is
 * copied from rbp_bridge.c on purpose: linking that object would put rbp's
 * absolute addresses into this process. The alternative — asserting nothing
 * about the quantisation — would leave the map's most-used conversion
 * unexamined. It is also why the expectation table below spells out 516: a
 * change to the real conversion fails a row here rather than passing quietly. */
int cc_to_10bit(int v)
{
     if (v < 0) v = 0;
     if (v > 127) v = 127;
     return (v << 3) | (v >> 4);
}

void *mixer_engine(void)             { return NULL; }

static int cue_calls, cue_last = -1;
int me_get_cue(int idx)              { (void)idx; return 0; }  /* nothing cued */
void me_set_cue(int idx, int on)     { (void)idx; cue_calls++; cue_last = on; }

static int master_cue_calls;
int me_get_master_cue(void)          { return 0; }
void me_set_master_cue(int on)       { (void)on; master_cue_calls++; }

static int stereo_calls, stereo_last = -1;
void me_set_stereo(int type)         { stereo_calls++; stereo_last = type; }

static int plinn_calls;
void *plinn(int deck)                { (void)deck; plinn_calls++; return NULL; }

static int looping_calls, apply_calls;
int aloop_is_looping(int deck)       { (void)deck; looping_calls++; return 0; }
void aloop_apply(int deck, void *p, int idx)
{
     (void)deck; (void)p; (void)idx;
     apply_calls++;   /* reaching here with nothing looping is the destructive
                       * case rbp_bridge.c's BEATLOOP gate exists to prevent */
}

/* ==========================================================================
 * The expectation table: the keycode stream the fixture must produce.
 *
 * One row per keycode, in dispatch order. The comment on each row names what
 * produces it, so a row that moves is traceable to one gesture rather than to
 * "the map changed somewhere".
 * ========================================================================== */

struct expect {
     int kind, key, op, ch;
     long param;
     float fval;
     long lval;
     int fval_sign;   /* compare the sign, not the value */
};

static const struct expect expect[] = {
     /* LOAD is on the GLOBAL channel, not on the deck's. */
     { 0, K_LOAD,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_LOAD,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* Mixer/global absolute controls. 64 -> 0x204 (516), the quantisation
      * cc_to_10bit() gives; the cue-mix knob also lands in g_cue_mix. */
     { 1, K_HPMIX,          OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_HPLEVEL,        OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 1, K_XFADER,         OP_VALUE,   1, 1023, 1.0f, 0, 0 },
     /* The channel fader, three times: a repeat is dropped, a new value is not.
      * The pair is also what fills g_fader[1] for the meter scaling. */
     { 1, K_FADER,          OP_VALUE,   1, 1023, 1.0f, 0, 0 },
     { 1, K_FADER,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     /* A mixer knob on the right strip -> deck 2's send channel. */
     { 1, K_EQM,            OP_VALUE,   2, 1023, 1.0f, 0, 0 },
     /* PLAY, pressed and released once, its repeats dropped. */
     { 0, K_PLAY,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PLAY,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* Manual loop in/out: the keycodes (and the LED latch, asserted below). */
     { 0, K_LOOPIN,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_LOOPOUT,        OP_PRESS,   1, 0,    0.0f, 0, 0 },
     /* SYNC tap. Nothing on the press, SYNC on the release: a press that leaked
      * through here would show as an extra row. */
     { 0, K_SYNC,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SYNC,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* A performance pad, a track skip and a track skip back. */
     { 0, K_PAD1,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_TRFWD,          OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_TRREV,          OP_PRESS,   1, 0,    0.0f, 0, 0 },
     /* A beat-loop knob turn and a beat-loop push reach no keycode: nothing is
      * looping, so nothing may be applied (see the stubs above). */
     /* TIME knob held, turned: the RX3's BEAT > button. */
     { 2, K_BEATNEXT,       OP_PRESS,   1, 1,    0.0f, 1, 0 },
     /* TIME knob released, turned: the ordinary time step. */
     { 2, K_TIME,           OP_ROTATE,  1, -1,   0.0f, -1, 0 },
     /* The jog: two 14-bit samples, the second one moving 5 counts. */
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,    1.0f, 5, 1 },
     /* The pitch fader at its detent (0x2000 of 0x3fff). */
     { 2, K_TEMPO_SLIDER,   OP_VALUE,   1, 511,  0.0f, 0x2000, 0 },
};

#define EXPECT_N ((int)(sizeof(expect) / sizeof(expect[0])))

/* ==========================================================================
 * The replay
 * ========================================================================== */

/* ctrlshim.c's ctrl_dispatch(), minus the MIDI_DUMP recording. */
static void dispatch(const struct snd_seq_event *ev)
{
     if (get_key_manager() && map_jp21.event)
          map_jp21.event(ev);
}

static unsigned long long us_of(double seconds)
{
     return (unsigned long long)(seconds * 1000000.0 + 0.5);
}

/* The fixture's replayable line count, filled in by round_trip(). */
static int fixture_events;

/* Read the fixture once: every line must parse the way the format says, and
 * every re-renderable line must come back byte-identical. */
static void round_trip(void)
{
     FILE *f = fopen(MIDI_FIXTURE, "r");
     char line[256], buf[256];
     int n_lines = 0;

     CHECK(f != NULL, "cannot open the fixture %s", MIDI_FIXTURE);
     if (!f)
          return;

     while (fgets(line, sizeof line, f)) {
          struct snd_seq_event ev;
          size_t len = strlen(line);
          double t;
          int ty;

          if (len && line[len - 1] == '\n')
               line[--len] = '\0';
          n_lines++;

          if (len == 0 || line[0] == '#')
               continue;

          if (sscanf(line, "%lf", &t) != 1) {
               CHECK(0, "fixture line %d has no timestamp: %s", n_lines, line);
               continue;
          }

          /* A SKIP line is the format's record of an event no replay can
           * reconstruct: it must re-render, and it must not replay. */
          if (sscanf(line, "%lf SKIP type=%d", &t, &ty) == 2) {
               CHECK(mididump_parse(line, &ev) == 0,
                     "fixture line %d: SKIP parsed as a replayable event",
                     n_lines);
               memset(&ev, 0, sizeof ev);
               ev.type = (unsigned char)ty;
               CHECK(mididump_format(&ev, us_of(t), buf, sizeof buf) == (int)len &&
                     strcmp(buf, line) == 0,
                     "fixture line %d: SKIP re-rendered as \"%s\", expected \"%s\"",
                     n_lines, buf, line);
               continue;
          }

          if (mididump_parse(line, &ev) != 1) {
               CHECK(0, "fixture line %d did not parse: %s", n_lines, line);
               continue;
          }
          fixture_events++;
          CHECK(mididump_format(&ev, us_of(t), buf, sizeof buf) == (int)len &&
                strcmp(buf, line) == 0,
                "fixture line %d: re-rendered as \"%s\", expected \"%s\"",
                n_lines, buf, line);
     }
     fclose(f);
     CHECK(fixture_events > 0, "the fixture carries no replayable events");
}

/* The lines the parser must refuse, and the three shapes it must accept. The
 * refusals matter as much as the acceptances: every one of them is a line that
 * appears in a dump that is still being written, or in a dump of a controller
 * doing something the format has no room for. */
static void parser_tolerance(void)
{
     static const struct { const char *line; int want; } t[] = {
          { "",                                     0 },  /* blank */
          { "\n",                                   0 },
          { "# just a comment\n",                   0 },
          { "not a dump line at all\n",             0 },
          { "0.500000 SKIP type=3\n",               0 },  /* recorded, not replayable */
          { "0.500000 NOTEON ch=0 note=1\n",        0 },  /* no velocity: no event */
          { "0.500000 CONTROLLER ch=0\n",           0 },
          { "0.500000 NOTEON ch=0 note=36 vel=127\n",   1 },
          { "0.500000 NOTEOFF ch=0 note=36 vel=0\n",    1 },
          { "0.500000 CONTROLLER ch=4 cc=17 val=64\n",  1 },
     };
     struct snd_seq_event ev;
     char buf[64], want[64];
     unsigned i;

     for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
          int got = mididump_parse(t[i].line, &ev);
          CHECK(got == t[i].want, "\"%s\" parsed as %d, expected %d",
                t[i].line, got, t[i].want);
     }

     /* The one field the format renders from the event rather than from a
      * switch arm: a type it cannot carry keeps its number, so a dump says what
      * arrived even when nothing can be done with it. */
     memset(&ev, 0, sizeof ev);
     ev.type = (unsigned char)SNDRV_SEQ_EVENT_CLOCK;
     snprintf(want, sizeof want, "0.000000 SKIP type=%d",
              (int)SNDRV_SEQ_EVENT_CLOCK);
     CHECK(mididump_format(&ev, 0, buf, sizeof buf) == (int)strlen(want) &&
           strcmp(buf, want) == 0,
           "an unreplayable type rendered as \"%s\", expected \"%s\"", buf, want);

     /* And a destination too small for the line is refused, not truncated. */
     CHECK(mididump_format(&ev, 0, buf, 4) < 0,
           "a too-small destination was not refused");
}

static void check_stream(void)
{
     int i;

     CHECK(sent_n == EXPECT_N,
           "the fixture produced %d keycodes, the table expects %d",
           sent_n, EXPECT_N);
     for (i = 0; i < EXPECT_N && i < sent_n; i++) {
          const struct sent *s = &sent[i];
          const struct expect *e = &expect[i];

          CHECK(s->kind == e->kind && s->key == e->key && s->op == e->op &&
                s->ch == e->ch,
                "keycode %d: got kind%d key=0x%04x op=%d ch=%d, expected "
                "kind%d key=0x%04x op=%d ch=%d",
                i, s->kind, s->key, s->op, s->ch,
                e->kind, e->key, e->op, e->ch);
          CHECK(s->param == e->param,
                "keycode %d (0x%04x): param %ld, expected %ld",
                i, e->key, s->param, e->param);
          if (e->fval_sign)
               CHECK((s->fval > 0.0f) == (e->fval > 0.0f) &&
                     s->fval <= 8.0f && s->fval >= -8.0f,
                     "keycode %d (0x%04x): fval %.3f, expected the sign of "
                     "%.1f inside rbp's +-8 clamp",
                     i, e->key, (double)s->fval, (double)e->fval);
          else
               CHECK(s->fval == e->fval,
                     "keycode %d (0x%04x): fval %.4f, expected %.4f",
                     i, e->key, (double)s->fval, (double)e->fval);
          CHECK(s->lval == e->lval,
                "keycode %d (0x%04x): lval %ld, expected %ld",
                i, e->key, s->lval, e->lval);
     }
}

/* Everything the map drives that is not a keycode. Each of these is a value the
 * audio shim reads or the LED bridge mirrors, so a map that stopped writing one
 * would still look fine from the keycode stream alone. */
static void check_state(void)
{
     CHECK(g_speaker_gain == 64.0f / 127.0f,
           "the speaker knob left g_speaker_gain at %.4f, expected %.4f",
           (double)g_speaker_gain, (double)(64.0f / 127.0f));
     CHECK(g_master_gain == 64.0f / 127.0f,
           "the Main Vol knob left g_master_gain at %.4f, expected %.4f",
           (double)g_master_gain, (double)(64.0f / 127.0f));
     CHECK(g_cue_mix == 516.0f / 1023.0f,
           "the cue-mix knob left g_cue_mix at %.4f, expected %.4f",
           (double)g_cue_mix, (double)(516.0f / 1023.0f));
     CHECK(g_cue_gain == 0.0f,
           "the cue-level knob left g_cue_gain at %.4f, expected 0",
           (double)g_cue_gain);

     /* The speaker switch and the split-cue switch are both toggles, and the
      * fixture drives them both ways; the *count* is what proves the on edge
      * arrived, since the last one wins. */
     CHECK(g_speaker_on == 0,
           "the speaker switch was left %s", g_speaker_on ? "ON" : "OFF");
     CHECK(g_split_cue == 0,
           "split cue was left %s", g_split_cue ? "ON" : "OFF");
     CHECK(stereo_calls == 2 && stereo_last == 1,
           "the split-cue switch called me_set_stereo() %d times (last %d), "
           "expected 2 with the last one stereo",
           stereo_calls, stereo_last);

     /* rbp's mixer channels 1/2: the left strip's fader moved and was seen, and
      * the right strip's was not touched by any of it. */
     CHECK(g_fader[1] == 0 && g_fader_seen[1] == 1,
           "the left fader is at %d (seen %d), expected 0 and seen",
           g_fader[1], g_fader_seen[1]);
     CHECK(g_fader[2] == 1023 && !g_fader_seen[2],
           "the right fader is at %d (seen %d); nothing in the fixture touches "
           "strip 2", g_fader[2], g_fader_seen[2]);

     /* The loop latch: LOOP IN sets it and LOOP OUT clears it — on deck 1 only,
      * which is also why deck 2's is asserted untouched. */
     CHECK(led_loop_armed[0] == 0,
           "the loop-in LED latch was left %d; LOOP OUT in the fixture clears it",
           led_loop_armed[0]);
     CHECK(led_loop_armed[1] == 0,
           "deck 2's loop-in LED latch was set by deck 1's LOOP IN");

     /* The PFL button has no keycode, so the mixer call is the whole of its
      * effect. */
     CHECK(cue_calls == 1 && cue_last == 1,
           "the PFL button called me_set_cue() %d times (last %d), expected 1 "
           "with cue on", cue_calls, cue_last);
     CHECK(master_cue_calls == 0,
           "the master cue was toggled %d times; the fixture's strips 3/4 are "
           "not touched", master_cue_calls);

     /* The beat-loop knob turned and the beat-loop button was pushed with
      * nothing looping: the push may ask for the player, and NOTHING may be
      * applied (aloop_apply() is what rbp_bridge.c gates behind BEATLOOP). */
     CHECK(looping_calls >= 1,
           "the beat-loop knob did not ask whether a loop was running");
     CHECK(plinn_calls == 1,
           "the beat-loop button asked for PlayerInnards %d times, expected 1",
           plinn_calls);
     CHECK(apply_calls == 0,
           "the beat-loop path applied a loop %d times with nothing looping",
           apply_calls);
}

/* ==========================================================================
 * The LED table the SC Live 4 map publishes.
 *
 * These numbers used to live in rbp_led.c, which made the SC Live 4's notes the
 * property of a bridge; they moved to map_jp21.c so that a panel's notes belong
 * to the surface and a bridge never learns one. The move is supposed to be a
 * PURE move, so this is the guard that it stayed one: a row dropped in the
 * shuffle is a dark LED on the previous target, which is the hardest kind of
 * defect to notice and the easiest to introduce. Every number is asserted
 * EXACTLY rather than merely "present", so neither a lost row nor an altered one
 * passes -- a "the table is non-empty" check would pass with half of it gone.
 *
 * What this cannot check is whether rbp_led.c still reads them: that file is not
 * in this link, because it needs rbp. Its half of the contract is held by review
 * and by the on-unit drills, not here.
 * ========================================================================== */
static void check_leds(void)
{
     const struct led_notes *n = map_jp21.leds;

     CHECK(n != NULL, "the SC Live 4 map publishes no LED table");
     if (!n)
          return;

     CHECK(n->deck_ch == 4,
           "deck LEDs go out on channel %d, were 4", n->deck_ch);
     CHECK(n->n_sync == 8 && n->n_cue == 9 && n->n_play == 10,
           "the transport LEDs are %d/%d/%d, were 8/9/10",
           n->n_sync, n->n_cue, n->n_play);
     CHECK(n->n_keylock == 34 && n->n_vinyl == 35 && n->n_slip == 36,
           "keylock/vinyl/slip are %d/%d/%d, were 34/35/36",
           n->n_keylock, n->n_vinyl, n->n_slip);
     CHECK(n->n_loopin == 37 && n->n_loopout == 38 && n->n_autoloop == 39,
           "loopin/loopout/autoloop are %d/%d/%d, were 37/38/39",
           n->n_loopin, n->n_loopout, n->n_autoloop);

     CHECK(n->pad_ch == 4 && n->pad_ch2 == 5 && n->n_pad_base[0] == 15,
           "pads are notes from %d on channels %d/%d, were 15 from 4/5",
           n->n_pad_base[0], n->pad_ch, n->pad_ch2);
     /* All four, and all the same one: this surface's pads keep their notes
      * across pad modes. That is what the old single `n_pad_first = 15` said, and
      * pinning it here is what stops the FLX4's per-mode table being copied onto
      * a surface whose pads have never been measured moving. */
     for (int m = 1; m < 4; m++)
          CHECK(n->n_pad_base[m] == n->n_pad_base[0],
                "the SC Live 4's pad base for mode %d is %d, not %d: its pads "
                "have never been measured re-addressing", m, n->n_pad_base[m],
                n->n_pad_base[0]);
     CHECK(n->pad_enc == LED_ENC_PRIME_6BIT,
           "the pad encoding is %d, was LED_ENC_PRIME_6BIT (%d)",
           n->pad_enc, (int)LED_ENC_PRIME_6BIT);

     CHECK(n->strip_ch_first == 0 && n->strip_count == 2 && n->n_strip_cue == 13,
           "the strip CUE LEDs are note %d on channels %d..%d, were 13 on 0..1",
           n->n_strip_cue, n->strip_ch_first,
           n->strip_ch_first + n->strip_count - 1);
     CHECK(n->master_ch_first == 2 && n->master_ch_count == 2 &&
           n->n_master_cue == 13,
           "the master CUE LED is note %d on channels %d..%d, was 13 on 2..3",
           n->n_master_cue, n->master_ch_first,
           n->master_ch_first + n->master_ch_count - 1);

     CHECK(n->fx_ch == 15, "the FX group is on channel %d, was 15", n->fx_ch);
     CHECK(n->n_fx[LED_FX_BFX_ONOFF] == 26 &&
           n->n_fx[LED_FX_CFX_FILTER] == 21 &&
           n->n_fx[LED_FX_CFX_DUBECHO] == 22 &&
           n->n_fx[LED_FX_CFX_NOISE] == 23 &&
           n->n_fx[LED_FX_CFX_SWEEP] == 24,
           "the FX notes are %d/%d/%d/%d/%d, were 26/21/22/23/24",
           n->n_fx[LED_FX_BFX_ONOFF], n->n_fx[LED_FX_CFX_FILTER],
           n->n_fx[LED_FX_CFX_DUBECHO], n->n_fx[LED_FX_CFX_NOISE],
           n->n_fx[LED_FX_CFX_SWEEP]);
}

int main(void)
{
     /* The map reads its calibration from the environment, and `make test`
      * inherits whatever the shell running it exports. Pin every name it reads,
      * so the expectation table is about the code and not about the developer's
      * environment. */
     static const char *const names[] = {
          "KNOB_SCALE", "JOG_SCALE", "JOG_PPR", "JOG_REV", "JOG_IDLE_MS",
          "JOG_VERBOSE", "TEMPO_VERBOSE", "TEMPO_REV", "BEATLOOP",
     };
     unsigned i;
     int n;

     for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
          unsetenv(names[i]);

     round_trip();
     parser_tolerance();

     /* build() is ADDITIVE (ctrl_bindings_reset() belongs to the front end, which
      * calls it once before building either surface), so the exact counts below
      * and in the flx4 suite are only meaningful because this is the first and
      * only build in a virgin process. Do not add a second surface's build above
      * this line without resetting first. */
     map_jp21.build();
     CHECK(note_map_n > 0 && abs_map_n > 0,
           "the map built %d notes and %d absolute controls",
           note_map_n, abs_map_n);

     /* speed <= 0 is "no pacing": the recording's gaps are real seconds, and
      * sleeping through them would make this a 40 ms test of nanosleep(). */
     n = mididump_replay(MIDI_FIXTURE, 0.0, dispatch);
     CHECK(n == fixture_events,
           "the replay dispatched %d events, the fixture has %d replayable lines",
           n, fixture_events);

     check_stream();
     check_state();
     check_leds();

     /* And once more with pacing on, which is the path MIDI_REPLAY_SPEED takes.
      * At this speed every sleep is zero-length, so only the count is asserted:
      * the state was consumed by the first pass. */
     sent_n = 0;
     n = mididump_replay(MIDI_FIXTURE, 1000000.0, dispatch);
     CHECK(n == fixture_events,
           "the paced replay dispatched %d events, expected %d",
           n, fixture_events);

     printf("test_midi: %d checks, %d failures\n", checks, failures);
     return failures == 0 ? 0 : 1;
}
