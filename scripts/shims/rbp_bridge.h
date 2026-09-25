/*
 * rbp_bridge.h -- what the rest of the shim may ask rbp to do.
 *
 * The addresses these functions use are in rbp_abi.h. They are deliberately not
 * exported from the shim (the objects are compiled -fvisibility=hidden): an
 * LD_PRELOAD library's global symbols enter the process's dynamic scope, so
 * anything that does not have to be visible must not be. rbp resolves its own
 * IPowerManager stubs by name, and the meter hook's getled_hook has to stay
 * visible because rbp's patched prologue jumps to it; those two are annotated
 * explicitly in ctrlshim.c and rbp_bridge.c.
 */
#ifndef RBLIVE4_RBP_BRIDGE_H
#define RBLIVE4_RBP_BRIDGE_H

/* Are we running inside rbp? Cheap (one /proc read), used as a guard by every
 * thread and by install_meter_hook(). */
int is_rbp_process(void);

/* rbp's KeyManager singleton, NULL until rbp has constructed it. */
void *get_key_manager(void);

/* rbp's keycode path. send_rx_key() is the common case; the _f/_fl forms carry
 * a float parameter (knobs and faders are 10-bit integers plus a normalised
 * float, and rbp's handlers read one or the other). */
void send_rx_key(int keycode, int op, int ch, long param);
void send_rx_key_f(int keycode, int op, int ch, long param, float fval);
void send_rx_key_fl(int keycode, int op, int ch, long param, float fval,
                    long lval);

/* MIDI's 7-bit CC value -> rbp's 10-bit knob/fader range. */
int cc_to_10bit(int v);

/* djengine::MixerEngine. Everything here tolerates the engine not existing yet
 * and returns a neutral value (-1 from the getters, a no-op from the setters),
 * because the shim starts before rbp has built it. */
void *mixer_engine(void);
int me_channel_input(void *engine, int idx);
int me_channel_count(void);
void me_set_cue(int idx, int on);
int me_get_cue(int idx);
void me_set_stereo(int type);
void me_set_master_cue(int on);
int me_get_master_cue(void);

/* The per-deck ui::PlayerInnards, and the beat-loop knob's way in. The cursor
 * (which loop length is selected) is ours: rbp has no delta handler for the
 * Opus Quad's beat-loop knob, so we keep the position and call
 * execAutoBeatLoop() ourselves. */
void *plinn(int deck);
int aloop_is_looping(int deck);
void aloop_apply(int deck, void *p, int idx);

/* BEATLOOP gate: -1 = not yet decided, 1 = the beat-loop knob is live, 0 = it
 * is inert. ctrlshim.c reads the environment once and sets it. */
extern int aloop_enabled;

/* rbp's own meter bitmasks, filled by the getLedValue hook: [0] master,
 * [1] ch1, [2] ch2. Read by rbp_vu.c; rbp_meter_segments() in rbp_abi.h is the
 * width of rbp's meter, and the scaling from it to the panel's 6 segments lives
 * in rbp_vu.c. */
extern volatile unsigned int g_meter_bits[3];

/* Patch rbp's ui::Mixer::MonoLvMeter::getLedValue prologue to jump to
 * getled_hook(), which records the bitmask and then runs the original through a
 * trampoline. No-op (and harmless) if the prologue is not the one we expect. */
void install_meter_hook(void);

#endif /* RBLIVE4_RBP_BRIDGE_H */

