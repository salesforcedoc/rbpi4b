/*
 * shmstate.c — the single definition site for state shared between shims.
 *
 * Link this into the CONTROLS shim only. The controls shim is first in
 * LD_PRELOAD, so its definitions win in the global scope and the audio shim
 * resolves to them. Linking it into a consumer too would give that consumer a
 * second, private copy that its own references would silently bind to -- see the
 * header for why that is worse than a link error.
 *
 * Consumers verify the contract with shmstate_require() from shmstate.h.
 */
#include "shmstate.h"

const int shmstate_abi_version = SHMSTATE_ABI_VERSION;

/* master-out level (Main Vol).  Applied to the main/XLR pair only, NOT to rbp's
 * master stream, so it does not affect the built-in monitors or the
 * headphones. */
volatile float g_master_gain = 1.0f;

/* headphone cue mix (0 = cue only, 1 = main only) / level (0..1). */
volatile float g_cue_gain = 1.0f;
volatile float g_cue_mix  = 1.0f;

/* built-in speaker volume (0..1), set by the booth/speaker knob.  Exported so
 * the audio shim can scale the built-in-speaker output pair. */
volatile float g_speaker_gain = 1.0f;

/* built-in monitor on/off switch.  1 = enabled. */
volatile int g_speaker_on = 1;

/* split-cue switch.  1 = left/right = cue/main. */
volatile int g_split_cue = 0;

/* Master VU peaks published by the audio shim (full scale).  The controls shim
 * converts them to meter messages; a controller with no meters just ignores
 * them. */
volatile int g_vu_peak[2] = { 0, 0 };
