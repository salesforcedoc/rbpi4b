/*
 * ccexit -- the test subject for crashcatch.so.
 *
 * crashcatch works by interposing `exit`/`_exit`/`_Exit` and the fatal signals,
 * so it can only be exercised by LD_PRELOADing it into a process that then dies
 * on purpose. That is all this program is.
 *
 * It is deliberately NOT in TESTS. Every test in the host suite is built -static,
 * and a static binary has no dynamic loader, so LD_PRELOAD is ignored and the
 * witness cannot be reached at all -- there is no host-suite test for it by
 * construction. It is verified on the unit instead, inside the chroot, where the
 * loader and the soft-float ABI are the real ones (docs/13 S3.7).
 *
 *   ccexit exit    -> exit(7)      through the PLT: interposed
 *   ccexit _exit   -> _exit(9)     through the PLT: interposed
 *   ccexit segv    -> a nil dereference: the signal handler
 *   ccexit abort   -> abort(): the signal handler
 *   ccexit ok      -> returns from main. This is the case to MEASURE rather than
 *                     assume: glibc reaches exit() from inside itself on this
 *                     path, and an intra-libc call is bound internally, so it is
 *                     the one normal termination the witness may not see. rbp
 *                     never returns from main, so it does not matter here -- but
 *                     a silent log would be a puzzle without knowing it.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *m = (argc > 1) ? argv[1] : "ok";
    if (!strcmp(m, "exit"))  exit(7);
    if (!strcmp(m, "_exit")) _exit(9);
    if (!strcmp(m, "abort")) abort();
    if (!strcmp(m, "segv"))  { *(volatile int *)0 = 1; }
    return 0;
}
