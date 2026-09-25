/*
 * shmstate.h — the contract for state shared *between* preloaded shims.
 *
 * Two of the three shims are not independent. The controls shim decides what
 * the mixer is doing (cue mix, cue level, master level, split cue) and the audio
 * shim has to apply it; the audio shim measures the master level and the
 * controls shim has to light the meters. They talk through plain globals in the
 * dynamic symbol table, because a shim cannot call into another shim it did not
 * link against.
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
 * test build can deliberately mis-version a producer to exercise the check. */
#ifndef SHMSTATE_ABI_VERSION
#define SHMSTATE_ABI_VERSION 1
#endif

/* The preload order that satisfies this contract; quoted in error messages so
 * the fix is in the message rather than in a doc. */
#define SHMSTATE_LOAD_ORDER \
    "/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so"

/* ---------- written by the controls shim, read by the audio shim ---------- */

/* master-out level (Main Vol). Applied to the main/XLR pair only, NOT to rbp's
 * master stream, so it does not affect the monitors or the headphones. */
extern volatile float g_master_gain;

/* headphone cue mix and level. g_cue_mix: 0 = cue only, 1 = main only.
 * g_cue_gain: 0..1. */
extern volatile float g_cue_gain;
extern volatile float g_cue_mix;

/* built-in speaker volume (0..1) and on/off switch. 1 = enabled. */
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
