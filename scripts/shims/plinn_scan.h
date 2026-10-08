/*
 * plinn_scan.h -- which candidate the PlayerInnards scan may accept, and what a
 * failed scan has to say about itself.
 *
 * rbp_bridge.c's scan_plinn() walks every readable+writable mapping looking for
 * the ui::PlayerInnards vtable word. Matching that word is necessary and is NOT
 * sufficient: the same four bytes occur in any other mapping by accident, and
 * the object's own fields have to agree before the address is taken as a deck's
 * innards. That decision, and the arithmetic a failure reports, live here.
 *
 * WHY THIS IS A SEPARATE MODULE, and it is the same reason led_table.c is one.
 * The scan shipped with a channel off-by-one -- `(chan != 2 && chan != 3)` with
 * `d = chan - 2`, when deck 1's innards carries 1 and deck 2's carries 2 -- so
 * deck 1 was rejected outright, deck 2 was filed AS deck 1, and every
 * plinn() write, the beat-loop path included, landed on the wrong deck for a
 * whole evening (2026-10-01). It did not crash. The tell was one log line,
 * `deck1=0xcbaaa780 deck2=(nil)`, which says what was found and nothing about
 * why -- and rbp_bridge.c is linked into no test at all, so a fix there is
 * verified by a drill on the Pi or not at all.
 *
 * So the DECISION is lifted out of the shim into a function with no address of
 * rbp's anywhere, no I/O and no globals, and test_plinn_scan.c pins it --
 * including the exact deck-1 case that shipped broken.
 *
 * The sibling port (Rx3-flx4) makes the same split for the same reason and
 * writes both halves down: validate the candidate structurally before accepting
 * it, and when a scan fails report the counts, so "it found nothing" is a number
 * rather than a mystery (control-shim.c:34-41, :84-86). This tree read the idea
 * there; the fields validated are this port's own.
 */
#ifndef RBPI4B_PLINN_SCAN_H
#define RBPI4B_PLINN_SCAN_H

/* The bounds a candidate's fields have to sit inside. They are not addresses of
 * rbp's -- they are what this port has measured, and naming them is what lets a
 * test pin the same numbers the scan uses. */

/* ui::PlayerInnards+0x74 is a byte that only ever reads 0..3 here. It is NOT
 * the pad mode rbp's UI displays -- rbp_abi.h's PLAYERINNARDS_MODE_OFF records
 * that measurement (polled 1.1 M times across all four pad-mode buttons, it
 * never left 0 while rbp's grid verifiably switched) -- and nothing reads it as
 * one. Its whole job is to discriminate the struct during this scan. */
#define PLINN_MODE_MAX 3

/* The deck's djengine::DjEngineIF*, at +0x30. A live one is always in rbp's own
 * heap or a mapped library, which on this 32-bit layout is the upper half; a
 * word below this is the vtable match landing on something that is not the
 * struct. */
#define PLINN_ENGINE_MIN 0x80000000u

/* The fields the scan reads out of a vtable match. Nothing here is an address. */
struct plinn_cand {
    unsigned char chan;   /* at PLAYERINNARDS_CHAN_OFF -- 1-based: 1 = deck 1 */
    unsigned char mode;   /* at PLAYERINNARDS_MODE_OFF -- the +0x74 byte       */
    unsigned int  eng;    /* at PLAYERINNARDS_ENGINE_OFF                       */
};

/* Why a candidate that matched the vtable word was still refused. The order is
 * the order the tests run in, so the first disagreement is the one reported. */
enum plinn_refuse {
    PLINN_OK = 0,
    PLINN_BAD_CHAN,       /* the channel byte is not 1 or 2 */
    PLINN_BAD_MODE,       /* +0x74 is outside 0..PLINN_MODE_MAX */
    PLINN_BAD_ENGINE,     /* the engine pointer is below PLINN_ENGINE_MIN */
    PLINN_REFUSE_COUNT    /* not a reason: how many there are */
};

/* The deck this candidate belongs to (0 or 1), or -1 with *why naming the test
 * that refused it. `why` may be NULL when the caller does not care. */
int plinn_classify(const struct plinn_cand *c, enum plinn_refuse *why);

/* A short name for a refusal, for the scan's own line. Never NULL. */
const char *plinn_refuse_name(enum plinn_refuse why);

/* One scan pass, counted. Every address tested is a `word`, every vtable match
 * is a `hit`, every hit is either an `accepted` seat or one `refused` reason --
 * so the parts add up and a line built from this can be checked rather than
 * believed. `accepted` is per deck; a seat found twice is counted twice, because
 * this counts what the scan SAW, not what it kept. */
struct plinn_tally {
    unsigned long regions;                 /* readable+writable mappings walked */
    unsigned long words;                   /* addresses tested */
    unsigned long hits;                    /* candidates whose vtable matched */
    unsigned long accepted[2];             /* hits that passed, by deck */
    /* Indexed by the refusal itself, so the slot a caller increments is the enum
     * value it was handed. [PLINN_OK] is therefore unused -- PLINN_OK is not a
     * refusal and has no name -- and the reader of the line skips it, so the
     * counts printed partition `hits` and nothing else. */
    unsigned long refused[PLINN_REFUSE_COUNT];
};

/* The line a failure prints: every count above, in one string, with the deck
 * seats spelled out. This is the half the 2026-10-01 bug needed and did not
 * have -- `deck1=0xcbaaa780 deck2=(nil)` names no counts, so a scan that found
 * nothing and a scan that refused everything look the same. Writes at most `n`
 * bytes and always NUL-terminates; returns `buf`. */
char *plinn_tally_line(char *buf, unsigned long n,
                       const struct plinn_tally *t);

#endif /* RBPI4B_PLINN_SCAN_H */
