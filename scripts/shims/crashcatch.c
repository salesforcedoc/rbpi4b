/*
 * crashcatch.so -- the exit witness.
 *
 * rbp quits on its own every 1-2.5 h of use (7 times between 2026-09-27 and
 * 2026-10-05) and leaves no trace anywhere: no dmesg line, no core, no OOM, no
 * memory climb, and the launcher is not the killer (docs/13 S3.7 has the whole
 * measurement). This preload is the only thing that can say why, so it logs
 * BOTH ends of a process's life:
 *
 *   - the fatal signals, as this file always did (pc/lr/addr/regs), and
 *   - the deliberate-termination paths -- `exit()`, `_exit()` and `_Exit()` --
 *     with the status and the caller's return address, which is the one case
 *     that leaves a kernel log and a core file *both* empty.
 *
 * A death with NO line is therefore evidence too: it was SIGKILLed (uncatchable
 * by design, and what start-rb.sh's `cleanup` sends), so the absence is what
 * rules the launcher in or out.
 *
 * The log goes to DISK and not /tmp, which is a 1.9 GB tmpfs that loses the
 * evidence on the next reboot. The path is CHROOT-RELATIVE, and it has to be:
 * rbp runs inside $RB_CHROOT, whose /opt is empty, so the host's own
 * /opt/rblive4/log is simply not there to write to -- a shim naming that path
 * falls back to tmpfs without saying so. /root/pdj is inside the chroot AND on
 * the SD card (it is the directory rbp and the shims themselves live in), so it
 * is the one place both conditions hold. Host-side that file is
 * $RB_DEPLOY_ROOT/$RB_CHROOT-name/root/pdj/crash.log.
 *
 * Three glibc-2.13 facts, each measured against this unit's own rootfs rather
 * than assumed, because a shim that calls a symbol the vendor libc lacks links
 * *fine* and then kills rbp at startup with "symbol lookup error":
 *   - `dlsym` is in libdl, NOT libc, hence the explicit libdl.so.2 on the link
 *     line and the version pin below;
 *   - `atexit` is not exported at all -- so the "did it exit cleanly?" question
 *     is answered by interposing `exit`, never by registering a handler;
 *   - `stat` is absent -- so the size cap below measures the file with lseek.
 *
 * Load order: this must come FIRST in RB_LD_PRELOAD. Its constructor is what
 * arms the signal handlers and resolves the real `exit`, and anything loaded
 * before it could fault or exit before either exists.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/syscall.h>

__asm__(".symver dlsym, dlsym@GLIBC_2.4");

/* Chroot-relative on purpose -- see the header. CRASH_LOG overrides it (rb.conf
 * defines RB_CRASH_LOG, which start-rb.sh hands over as CRASH_LOG). */
#define CRASH_LOG_DISK "/root/pdj/crash.log"
#define CRASH_LOG_TMP  "/tmp/crash.log"

/* A cap, because the unit runs Restart=always with StartLimitIntervalSec=0: a
 * crash loop would otherwise append a line every 10 s forever onto the SD card.
 * 256 KB is years of ordinary restarts and nothing in a loop. */
#define CRASH_LOG_MAX  (256 * 1024)

/* Names the shape of every line below, so a log read months from now can tell
 * which witness wrote it. Bump when a field changes meaning. */
#define CRASH_WITNESS_V 1

static char g_path[128];              /* resolved once, in the constructor */
static void (*g_real_exit)(int);      /* libc's exit, resolved the same way */
static volatile sig_atomic_t g_in;    /* re-entry guard, see fatal() */

/* --- async-signal-safe formatting ---------------------------------------- */

static int put(char *b, int i, const char *s) {
    while (*s) b[i++] = *s++;
    return i;
}

static int puthex(char *b, int i, unsigned v) {
    static const char h[] = "0123456789abcdef";
    b[i++] = '0'; b[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) b[i++] = h[(v >> s) & 15];
    return i;
}

static int putdec(char *b, int i, int v) {
    char t[12];
    int n = 0;
    if (v < 0) { b[i++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) b[i++] = t[--n];
    return i;
}

/* One line, appended. open/write/close only: this runs from a signal handler. */
static void emit(const char *b, int n) {
    if (!g_path[0]) return;
    int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) fd = open(CRASH_LOG_TMP, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    (void)write(fd, b, (size_t)n);
    close(fd);
}

/* The real thing. Called instead of `_exit()` on purpose: by the time we are
 * here `_exit` is ours, and going through it would log twice and recurse. */
__attribute__((noreturn)) static void die(int status) {
    syscall(SYS_exit_group, status);
    for (;;) { }
}

/* --- the three kinds of line --------------------------------------------- */

/* Deliberate termination. `lr` is the caller of exit(), which is the address
 * you want: it names the site inside rbp that decided to quit. */
static void reason(const char *what, int status, unsigned lr) {
    char b[192];
    int i = 0;
    i = put(b, i, "CRASH v");  i = putdec(b, i, CRASH_WITNESS_V);
    i = put(b, i, " pid=");    i = putdec(b, i, (int)getpid());
    i = put(b, i, " ");        i = put(b, i, what);
    i = put(b, i, " status="); i = putdec(b, i, status);
    i = put(b, i, " caller="); i = puthex(b, i, lr);
    b[i++] = '\n';
    emit(b, i);
}

/* Load-time marker. This is the known-good line: without it a silent log is
 * indistinguishable from a witness that was never loaded at all. */
static void armed(void) {
    char b[256];
    int i = 0;
    i = put(b, i, "CRASH v");   i = putdec(b, i, CRASH_WITNESS_V);
    i = put(b, i, " pid=");     i = putdec(b, i, (int)getpid());
    i = put(b, i, " armed log="); i = put(b, i, g_path);
    i = put(b, i, " real_exit=");
    i = put(b, i, g_real_exit ? "resolved" : "MISSING (exit() will not flush)");
    b[i++] = '\n';
    emit(b, i);
}

static void fatal(int sig, siginfo_t *si, void *uc) {
    /* A fault inside this handler would re-enter it and walk the stack to
     * death; SA_RESETHAND has already reset the disposition, and this makes the
     * intent explicit rather than relying on the flag alone. */
    if (g_in) die(1);
    g_in = 1;

    ucontext_t *u = (ucontext_t *)uc;
    const char *name = (sig == SIGSEGV) ? "SIGSEGV"
                     : (sig == SIGBUS)  ? "SIGBUS"
                     : (sig == SIGABRT) ? "SIGABRT"
                     : (sig == SIGFPE)  ? "SIGFPE"
                     : (sig == SIGILL)  ? "SIGILL" : "signal";

    char b[512];
    int i = 0;
    i = put(b, i, "CRASH v");  i = putdec(b, i, CRASH_WITNESS_V);
    i = put(b, i, " pid=");    i = putdec(b, i, (int)getpid());
    i = put(b, i, " ");        i = put(b, i, name);
    i = put(b, i, " sig=");    i = putdec(b, i, sig);
    i = put(b, i, " addr=");   i = puthex(b, i, (unsigned)(unsigned long)si->si_addr);
    i = put(b, i, " pc=");     i = puthex(b, i, (unsigned)u->uc_mcontext.arm_pc);
    i = put(b, i, " lr=");     i = puthex(b, i, (unsigned)u->uc_mcontext.arm_lr);
    i = put(b, i, " sp=");     i = puthex(b, i, (unsigned)u->uc_mcontext.arm_sp);
    i = put(b, i, " fp=");     i = puthex(b, i, (unsigned)u->uc_mcontext.arm_fp);

    unsigned regs[10] = { u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1,
                          u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3,
                          u->uc_mcontext.arm_r4, u->uc_mcontext.arm_r5,
                          u->uc_mcontext.arm_r6, u->uc_mcontext.arm_r7,
                          u->uc_mcontext.arm_r8, u->uc_mcontext.arm_r9 };
    for (int r = 0; r < 10; r++) {
        i = put(b, i, " r"); b[i++] = (char)('0' + r); b[i++] = '=';
        i = puthex(b, i, regs[r]);
    }
    b[i++] = '\n';
    emit(b, i);

    die(1);
}

/* --- the deliberate-termination interposers ------------------------------ */

/* __builtin_return_address(0) is the caller of exit(), and it is reliable on
 * this target: ARM keeps the return address in LR, so it does not depend on
 * frame pointers the way x86 would. */
void exit(int status) {
    reason("exit", status, (unsigned)(unsigned long)__builtin_return_address(0));
    if (g_real_exit) g_real_exit(status);   /* flushes stdio and runs rbp's own
                                             * exit handlers: skipping this would
                                             * lose the very buffered output we
                                             * are here to catch */
    die(status);
}

void _exit(int status) {
    reason("_exit", status, (unsigned)(unsigned long)__builtin_return_address(0));
    die(status);
}

void _Exit(int status) {
    reason("_Exit", status, (unsigned)(unsigned long)__builtin_return_address(0));
    die(status);
}

/* --- load ---------------------------------------------------------------- */

static int oversized(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    off_t e = lseek(fd, 0, SEEK_END);
    close(fd);
    return e > (off_t)CRASH_LOG_MAX;
}

__attribute__((constructor)) static void init(void) {
    const char *env = getenv("CRASH_LOG");
    if (env && *env && strlen(env) < sizeof g_path) {
        strcpy(g_path, env);
    } else {
        int fd = open(CRASH_LOG_DISK, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { close(fd); strcpy(g_path, CRASH_LOG_DISK); }
        else         strcpy(g_path, CRASH_LOG_TMP);   /* deploy root moved, or
                                                       * the card is full */
    }
    if (oversized(g_path)) {
        int fd = open(g_path, O_WRONLY | O_TRUNC);
        if (fd >= 0) close(fd);
    }

    /* Resolved here, not in the interposer: exit() can be reached from a
     * context where dlsym is not safe to call. Never left NULL in practice --
     * if it were, `armed` says so and exit() degrades to the raw syscall. */
    *(void **)(&g_real_exit) = dlsym(RTLD_NEXT, "exit");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fatal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS,  &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGFPE,  &sa, 0);
    sigaction(SIGILL,  &sa, 0);

    armed();
}
