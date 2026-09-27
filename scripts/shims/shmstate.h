/*
 * shmstate.h — the contract for state shared *between* preloaded shims.
 *
 * Two of the three shims are not independent. The controls shim decides what
 * the mixer is doing (master level, cue mix/level, split cue) and the audio shim
 * applies what it can of it; the audio shim measures the master level and the
 * controls shim has to light the meters. They talk through plain globals in the
 * dynamic symbol table, because a shim cannot call into another shim it did not
 * link against.
 *
 * Which of those the audio side actually reads is target-dependent and noted per
 * symbol below: on the Pi the master gain is applied and the cue pair is routed
 * straight through rbp's own phone stream, so the cue mix/level knobs have no
 * reader in this shim and no speaker pair exists to consume g_speaker_* at all.
 *
 * That makes three things load-bearing, none of which the compiler checks:
 *
 *   1. LD_PRELOAD ORDER. The definitions live in the controls shim, so it must
 *      come first. See SHMSTATE_LOAD_ORDER.
 *   2. THE ABI. These are raw globals with no marshalling; if a type changes,
 *      every reader silently misinterprets the bytes. shmstate_abi_version is
 *      bumped whenever a type or meaning below changes, and consumers verify it
 *      at load time rather than reading garbage.
 *   3. EXACTLY ONE DEFINITION. Do not link shmstate.o into more than one shim.
 *      Two definitions both land in the global scope, the first preloaded one
 *      wins for everyone, and a reader silently ends up with its own private
 *      zero-filled copy instead of the writer's value — which is exactly the
 *      failure this file exists to make impossible to miss.
 *
 * The definitions live in shmstate.c; this header is the declarations, so the
 * controls shim and the audio shim can never drift apart by both writing out
 * their own `extern` line by hand (which is how it worked before).
 */
#ifndef RBLIVE4_SHMSTATE_H
#define RBLIVE4_SHMSTATE_H

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bump on any change to the type or meaning of a symbol below. Overridable so a
 * test build can deliberately mis-version a producer to exercise the check.
 *
 * v2: g_mirror_gain added. A symbol ADDED to the list is already caught from the
 * consumer's side -- a new audioshim's dlsym() finds nothing in an old producer
 * and shmstate_require() aborts with "missing symbol". The reverse mix is the
 * silent one: an old audioshim's list does not mention the new symbol, so it
 * resolves everything it knows about and happily ignores the value the new
 * knobshim is writing -- the mirror gain would be set and never read, with no
 * symptom but a dead knob. The bump is what makes that direction loud too, which
 * is the whole reason to bump for an addition. */
#ifndef SHMSTATE_ABI_VERSION
#define SHMSTATE_ABI_VERSION 2
#endif

/* The preload order that satisfies this contract; quoted in error messages so
 * the fix is in the message rather than in a doc. */
#define SHMSTATE_LOAD_ORDER \
    "/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so"

/* ---------- written by the controls shim, read by the audio shim ---------- */

/* master-out level (Main Vol). Applied to the main/XLR pair only, NOT to rbp's
 * master stream, so it does not affect the monitors or the headphones. */
extern volatile float g_master_gain;

/* The HDMI mirror's level, fed by the unit's own MASTER LEVEL knob (ch 6,
 * CC 8 MSB + CC 40 LSB, 14-bit). Read by audioshim.c's mirror fill and by
 * nothing else -- see the definition in shmstate.c for why it is a separate
 * symbol from g_master_gain rather than a second writer of it. Consumers should
 * still clamp it: it is a float written from a MIDI message. */
extern volatile float g_mirror_gain;

/* headphone cue mix and level. g_cue_mix: 0 = cue only, 1 = main only.
 * g_cue_gain: 0..1.
 *
 * On the Pi these have no reader in the audio path, deliberately. rbp's own
 * phone stream already carries cue and master summed and time-aligned, and
 * audioshim routes that stream to the headphone pair; the SC Live 4's
 * audioshim instead summed the two and combed. The cue buses the FLX4's knobs
 * drive still have to reach the mix, and that is a job for rbp's mixer engine,
 * in the controls shim (M3b). Until then these are written and unread. */
extern volatile float g_cue_gain;
extern volatile float g_cue_mix;

/* built-in speaker volume (0..1) and on/off switch. 1 = enabled.
 *
 * Only the SC Live 4 had built-in speakers. There is no consumer on a Pi and
 * no pair in RB_AUDIO_MAP for it, so these two are dead weight here: the FLX4
 * has no speaker output at all. Left in place with the ABI version unchanged
 * rather than removed quietly, so the deletion happens deliberately — dropping
 * them means bumping SHMSTATE_ABI_VERSION and updating the producers in the
 * controls shim, and both steps want to be a decision, not an accident. */
extern volatile float g_speaker_gain;
extern volatile int   g_speaker_on;

/* split-cue switch. 1 = left/right = cue/main. */
extern volatile int   g_split_cue;

/* ---------- written by the audio shim, read by the controls shim ---------- */

/* master VU peaks (full scale). The controls shim converts them to meter
 * messages; a target with no meters (the DDJ-FLX4) just ignores them. */
extern volatile int   g_vu_peak[2];

/* Defined once, in shmstate.c. Read (never written) by consumers. */
extern const int shmstate_abi_version;

/*
 * Everything a consumer must be able to resolve, by dynamic-symbol name. Kept
 * as one list so adding a symbol to the contract is a one-line change here.
 */
#define SHMSTATE_SYMBOL_LIST(X) \
    X("g_master_gain")          \
    X("g_mirror_gain")          \
    X("g_cue_gain")             \
    X("g_cue_mix")              \
    X("g_speaker_gain")         \
    X("g_speaker_on")           \
    X("g_split_cue")            \
    X("g_vu_peak")              \
    X("shmstate_abi_version")

/*
 * Verify the contract from a consumer's constructor.
 *
 * What this catches, and what it does not: an ABSENT producer is already caught
 * before we run, because these are data symbols and data relocations bind
 * eagerly -- you get `symbol lookup error: undefined symbol: g_vu_peak` from
 * ld.so at load time, and constructors never execute. The case ld.so cannot
 * catch is a producer that IS present but was built against a different version
 * of this contract: the symbols resolve, the types no longer agree, and the
 * consumer reads meaningful-looking garbage. That is what the version check
 * below is for, and it is why it runs even when every symbol resolved.
 *
 * Set SHMSTATE_STRICT=0 to downgrade to a warning (useful for fixture runs that
 * preload one shim on its own).
 *
 * Static-inline so consumers need no extra object; shmstate.o must NOT be linked
 * into a consumer (see point 3 above).
 */
static inline void shmstate_require(const char *who)
{
    char msg[768];
    int off = 0, missing = 0, badver = 0;

    off += snprintf(msg + off, sizeof(msg) - off,
                    "%s: shim shared-state contract check failed\n", who);

#define SHMSTATE_CHECK_SYM(name)                                        \
    do {                                                                \
        if (!dlsym(RTLD_DEFAULT, name)) {                               \
            off += snprintf(msg + off, sizeof(msg) - off,               \
                            "  missing symbol: %s\n", name);            \
            missing++;                                                  \
        }                                                               \
    } while (0);

    SHMSTATE_SYMBOL_LIST(SHMSTATE_CHECK_SYM)
#undef SHMSTATE_CHECK_SYM

    if (!missing) {
        /* Producer is present; make sure it agrees about the layout. */
        const int *ver = (const int *)dlsym(RTLD_DEFAULT,
                                            "shmstate_abi_version");
        if (ver && *ver != SHMSTATE_ABI_VERSION) {
            off += snprintf(msg + off, sizeof(msg) - off,
                            "  ABI mismatch: producer is v%d, this shim is v%d\n"
                            "  (mixed build: rebuild all shims together)\n",
                            *ver, SHMSTATE_ABI_VERSION);
            badver = 1;
        }
    }

    if (!missing && !badver)
        return;

    off += snprintf(msg + off, sizeof(msg) - off,
                    "  the controls shim must be preloaded BEFORE this one:\n"
                    "    LD_PRELOAD=%s\n", SHMSTATE_LOAD_ORDER);

    if (getenv("SHMSTATE_STRICT") && !strcmp(getenv("SHMSTATE_STRICT"), "0")) {
        off += snprintf(msg + off, sizeof(msg) - off,
                        "  SHMSTATE_STRICT=0: continuing anyway\n");
        write(2, msg, (size_t)off);
        return;
    }

    off += snprintf(msg + off, sizeof(msg) - off, "  aborting\n");
    write(2, msg, (size_t)off);
    _exit(1);
}

#endif /* RBLIVE4_SHMSTATE_H */
