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
#ifndef RBPI4B_RBP_BRIDGE_H
#define RBPI4B_RBP_BRIDGE_H

/* The key path -- is_rbp_process(), get_key_manager(), send_rx_key*() -- lives
 * in rbp_key.{h,c} and is included here so that every caller of this header
 * still sees it. It is its own object because fbshim links it as well; see
 * rbp_key.h. */
#include "rbp_key.h"

/* MIDI's 7-bit CC value -> rbp's 10-bit knob/fader range. */
int cc_to_10bit(int v);

/* The Beat FX type rbp is currently on: the internal effect number, not the
 * 14-position switch position (rbp_abi.h documents both mappings and carries
 * bfx_type_to_pos[], which turns this number into a position). -1 when the
 * shim is not running inside rbp at all.
 *
 * A map uses it to seed its own FX-select cursor from what is already playing,
 * so the first move steps on from the current effect rather than jumping to
 * position 0. Reading it here rather than calling ADDR_GET_BFX_TYPE at the
 * point of use is what keeps the maps free of rbp addresses -- and is what lets
 * a host test stub this and pin the seeding, since an absolute address in the
 * test process is a segfault under qemu-arm, not a failed check.
 *
 * Like every getter here it is a read of state rbp owns, so it answers for the
 * process rather than for the engine: "rbp has not built its effect engine yet"
 * is not distinguishable from a real answer, and does not need to be -- a map's
 * event handler only runs once rbp's KeyManager exists. */
int rbp_beatfx_type(void);
/* Same current-object beat rung used by rbp's beat display and X-PAD, even
 * while FX is off. -1 before the manager/effect exists or for invalid data. */
int rbp_beatfx_beat(void);
/* Raw RX3 pad mode 0..7, or -1 while its status chain is unavailable. */
int rbp_deck_pad_mode(int deck);
/* Resize an existing loop only (deck 0/1): halve != 0 -> 1/2x, else 2x.
 * Returns -1 before the engine exists, 0 for no loop/refusal, 1 on success. */
int rbp_loop_resize(int deck, int halve);
/* Loaded deck only: from_cue -> back to main cue and play; otherwise pause
 * at track time zero. Returns 1 accepted, 0 refused, -1 unavailable. */
int rbp_transport_shortcut(int deck, int from_cue);
/* Complete queued cue-and-play after native cue buffering. cancel != 0 clears
 * pending requests without starting playback (used on connection changes). */
void rbp_transport_shortcut_tick(int cancel);
void rbp_transport_shortcut_cancel(int deck);

struct rbp_deck_feedback {
     int loaded, loading;
     long samples;
     unsigned int music_hi, music_lo;
};
int rbp_read_deck_feedback(int deck, struct rbp_deck_feedback *state);
/* Toggle the called memory loop's saved Active flag via native DB handling.
 * 1 = request accepted, 0 = no memory loop/refused, -1 = unavailable. */
int rbp_toggle_active_loop(int deck);

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
int me_set_crossfader_assign(int idx, int assign);
int me_get_crossfader_assign(int idx);
int me_set_crossfader_curve(int curve);
void me_set_color_parameter(float value);
int me_get_crossfader_curve(void);
/* USB slots 1/2: native media state (2 ready, 0 absent), -1 unavailable. */
int rbp_usb_media_state(int slot);
/* Native recorder flag: 0 stopped, 1 recording, -1 engine unavailable. */
int rbp_recording(void);

/* The per-deck ui::PlayerInnards, and the beat-loop knob's way in. The cursor
 * (which loop length is selected) is ours: rbp has no delta handler for the
 * Opus Quad's beat-loop knob, so we keep the position and call
 * execAutoBeatLoop() ourselves. */
void *plinn(int deck);
int aloop_is_looping(int deck);
void aloop_apply(int deck, void *p, int idx);

/* Delete HOT CUE `pad` (1..8) on `deck` (0..1) -- the one gesture on this
 * surface rbp has no keycode for.  It calls rbp's own
 * ui::Player::onHotCueDeleteEvent(pad) directly (rbp_abi.h's ADDR_HOTCUE_DELETE),
 * because the key path cannot reach it: the selector rbp switches on is cleared
 * before the handler reads it, which was measured, not assumed.  Returns -1 when
 * the deck's ui::Player is not found yet or the pad is out of range, 0 otherwise
 * -- "0" means rbp was handed the gesture, not that a cue was removed, because
 * rbp decides that from the pad's own state and a pad with no cue is a no-op.
 *
 * The delete is TWO halves and this makes both: that call does rbp's cue list and
 * the DbProxy delete, and a second direct call to the engine
 * (ADDR_ENGINE_CLEAR_HOTCUE) removes the cue from the engine.  Without the second
 * the cue is gone from rbp's data but the pad cell keeps drawing it -- which is
 * exactly what the operator saw, and what the log's `reg=` reports. */
int hotcue_delete(int deck, int pad);

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

#endif /* RBPI4B_RBP_BRIDGE_H */

