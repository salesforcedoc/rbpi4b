/*
 * netshim.so -- rbp's Pro DJ Link stack, over the interface that is actually up.
 *
 * rbp carries Pioneer's whole Link implementation (network::ProDjLink,
 * LinkDeviceManager, SystemManagement*) but asks the kernel about ONE interface
 * BY NAME: the literal "eth0", from exactly twelve functions (string VA
 * 0x3FE678; the list is in docs/18-prodjlink.md). On this unit eth0 is DOWN
 * with carrier 0 and wlan0 is the live link at 192.168.1.239/24, so the stack
 * starts, finds no address, and stays silent. The operator's LAN, meanwhile, has
 * a CDJ-3000 and a rekordbox announcing on UDP 50000.
 *
 * Those twelve functions reach the kernel through exactly three libc calls, and
 * all three are undefined GLIBC_2.4 imports of rbp's, so all three are
 * interposable:
 *
 *   ioctl            eleven of the twelve (SIOCGIFADDR, SIOCGIFNETMASK,
 *                    SIOCGIFHWADDR, SIOCGIFINDEX, SIOCGIFFLAGS, ...)
 *   if_nametoindex   ui::MacAddressListener::run @0x340de0, which binds a raw
 *                    AF_PACKET/ETH_P_ALL socket to the index it gets back
 *   system           via juce_runSystemCommand: `udhcpc -i eth0 ...` and
 *                    `ifconfig | awk '$1 ~ /^eth/ {print $NF}'`
 *
 * NO socket is bound to an interface anywhere in rbp (there is no
 * SO_BINDTODEVICE in the binary), so UDP egress already follows the routing
 * table and leaves via wlan0. Only the eth0-*named* introspection is broken,
 * which is what makes a name substitution at these three calls sufficient.
 *
 * ---------------------------------------------------------------------------
 * LOAD ORDER -- this shim must be SECOND in RB_LD_PRELOAD, after crashcatch.so
 * and before fbshim.so.
 *
 * fbshim.so already defines `ioctl` (fb_shim.c:180) and is in the same
 * LD_PRELOAD list. A process resolves ioctl to the FIRST preloaded object that
 * defines it, so the order decides which one runs, and both wrong directions are
 * quiet:
 *
 *   netshim AFTER fbshim   fbshim's ioctl wins. if_nametoindex and system still
 *                          rewrite, so the shim looks half-alive and the Link
 *                          stack stays silent. doctor.sh checks this order.
 *   netshim BEFORE fbshim,
 *   and not delegating     fbshim's framebuffer emulation, its tsc2007 ioctls
 *                          and its /dev/mem and gpiodrv stubs never run -- the
 *                          display breaks.
 *
 * The second is impossible here because every call this file does not rewrite is
 * forwarded to dlsym(RTLD_NEXT, "ioctl"), which from netshim's position is
 * fbshim's -- and fbshim's non-framebuffer path hands SIOCGIF* (0x89xx) to the
 * kernel untouched ((request & 0xffff) < 0x4600 || > 0x4620). The chain is
 * netshim -> fbshim -> syscall(SYS_ioctl), and fbshim is never skipped.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS NEVER REWRITTEN. "SIOCGIF" is not a prefix to match, it is a split:
 * every SIOCGIF* has a SIOCSIF* twin that WRITES, and one of the twelve
 * functions is setIpAddr @0x39dfb4, whose name implies SIOCSIFADDR. Retargeting
 * that at wlan0 would assign rbp's address -- plausibly a 169.254.x.x link-local
 * when network::Autoip runs -- to the interface carrying the operator's own SSH
 * session and LAN. So the rewrite is a WHITELIST of read-only name-in requests
 * (netalias.c), and a setter reaches the kernel unchanged and fails harmlessly
 * on the carrierless interface. test_netalias.c walks each getter/setter pair.
 *
 * The same instinct shapes the `system` hook, which ships as an OBSERVING
 * PASS-THROUGH: it logs every distinct command line and runs it unchanged. The
 * refuse/rewrite verdicts exist and are tested, and are applied only when the
 * operator turns RB_NETALIAS_MAC_SCRAPE on. In particular
 * `udhcpc -i eth0 ...` may never be rewritten, because a DHCP client started on
 * wlan0 would solicit a NEW LEASE on the link the operator is using.
 *
 * ---------------------------------------------------------------------------
 * ENABLEMENT. Installed and listed in RB_LD_PRELOAD from the start, so the load
 * order and the doctor checks are exercised from day one, but the substitution
 * defaults OFF (RB_NETALIAS=0). With it off every hook is an identity
 * pass-through, so a failed experiment is inert rather than merely disabled.
 * Turn it on in rb.local.conf -- the file this tree designates for measured
 * on-unit values, because rb.conf is overwritten by every install.
 *
 * The four settings, all read with envutil.h's helpers because start-rb.sh's
 * SHIM_VARS loop exports every listed name including the unset ones, so a bare
 * getenv() != NULL test is always true:
 *
 *   RB_NETALIAS            0 (default) | 1     -- substitute at all
 *   RB_NETALIAS_IFACE      "" | <name>         -- an explicit choice; wins over
 *                                                 the auto-detection, and falls
 *                                                 through to it if absent
 *   RB_NETALIAS_MAC_SCRAPE 0 (default) | 1     -- apply the system verdicts
 *   RB_NETALIAS_LOG        0 | 1               -- write /tmp/netshim.log
 *
 * ---------------------------------------------------------------------------
 * THE LOG is /tmp/netshim.log, and it is a DRILL INSTRUMENT, not evidence: /tmp
 * is a 1.9 GB tmpfs and the file dies with the next reboot. Read it before one.
 * (crashcatch.so's log is on the SD card for the opposite reason -- a crash you
 * were not watching for is the one you need.) Every line is written at most ONCE
 * PER DISTINCT thing: this shim is on ioctl(), which is on the pointer path and
 * the ALSA path, and a per-call line would fill tmpfs at tick rate. The first
 * line is the known-good marker, so a silent log is distinguishable from a shim
 * that never loaded.
 */
#define _GNU_SOURCE
#include "netalias.h"
#include "envutil.h"
#include "rbp_abi.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* dlsym is in libdl, NOT libc, on this rootfs -- hence the explicit libdl.so.2
 * on the Makefile's link line and this pin. Same glibc-2.13 facts crashcatch.c
 * documents at length. */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");

#define NETALIAS_LOG_PATH "/tmp/netshim.log"

/* Names the shape of every line, so a log read months from now can tell which
 * build wrote it. Bump when a field changes meaning. */
#define NETALIAS_V 1

/* How many interfaces to consider. A unit with more than this has bigger
 * problems, and a truncated list can only cost a candidate, never correctness:
 * the worst case is the identity fallback. */
#define NA_IF_MAX 16

/* Bounded so a pathological caller cannot grow the log without limit. Each table
 * logs every distinct entry once and then says it is full -- the file stops
 * growing, and it says why rather than going quiet.
 *
 * SEEN_REQ_MAX bounds the two ioctl tables differently on purpose: the noise
 * table is capped by it, the signal table only uses it as a dedupe guard (see
 * note_req). SEEN_SYS_MAX likewise caps pass-throughs, not verdicts. */
#define SEEN_REQ_MAX 32
#define SEEN_SYS_MAX 16

/* --- state, all resolved once in the constructor ------------------------- */

static int  g_on;                     /* substitute at all (RB_NETALIAS) */
static int  g_log;                    /* write the log (RB_NETALIAS_LOG) */
static int  g_mac_scrape;             /* apply the system verdicts */
static int  g_active;                 /* g_on || g_log: any work at all */
static char g_alias[NA_NAME_MAX];     /* "" = identity, and the default */
static const char *g_alias_src = "none";  /* env | auto | none, for the log */

static int (*g_real_ioctl)(int, unsigned long, void *);
static unsigned int (*g_real_if_nametoindex)(const char *);
static int (*g_real_system)(const char *);

/* The one-shot Link bring-up. See the section before `load` for why it exists
 * and what it refuses to fire on. */
#define CONNECT_POLL_MS      250
#define CONNECT_FILE_DEFAULT "/tmp/rb_link.req"

static int   g_connect;                          /* arm the trigger (NETALIAS_CONNECT) */
static const char *g_connect_file = CONNECT_FILE_DEFAULT;
static pthread_t g_connect_tid;
static int   g_connect_said;                     /* last refusal logged, deduped */
static int   g_connect_player;                   /* this process is the player: see running_as_player() */

/* --- the log ------------------------------------------------------------- */

static void emit(const char *b, int n)
{
    int fd;

    fd = open(NETALIAS_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    (void)write(fd, b, (size_t)n);
    close(fd);
}

/* snprintf rather than crashcatch.c's hand-rolled formatting: that file formats
 * inside a signal handler, where async-signal-safety forces the byte-at-a-time
 * version. Nothing here is in a handler.
 *
 * Named emitf and NOT logf, which is a C builtin (float logf(float) from
 * <math.h>) and draws a -Wbuiltin-declaration-mismatch warning as a static
 * function of a different type. */
static void emitf(const char *fmt, ...)
{
    char b[320];
    va_list ap;
    int n;

    if (!g_log)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof b - 1)
        n = (int)sizeof b - 1;
    emit(b, n);
}

/* FNV-1a, for de-duplicating command lines of any length without storing them. */
static unsigned long fnv1a(const char *s)
{
    unsigned long h = 2166136261UL;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

/* A 16-byte ifr_name field as a C string, for the log and for comparing one
 * sighting with another. The field is NOT guaranteed to be terminated -- a
 * caller may use all 16 bytes -- so it is copied bounded rather than handed to
 * strcmp, which would read past the end of the caller's buffer. (A kernel
 * interface name is at most IFNAMSIZ-1 = 15 characters, so a 16-byte field can
 * never actually be a whole name and the cut cannot merge two.) */
static void label_of(const char *field, char *out)
{
    size_t n = 0;

    while (n < NA_NAME_MAX - 1 && field[n]) {
        out[n] = field[n];
        n++;
    }
    out[n] = '\0';
}

static void set_name(char *dst, const char *src)
{
    size_t n = strlen(src);

    if (n >= NA_NAME_MAX)
        n = NA_NAME_MAX - 1;
    memset(dst, 0, NA_NAME_MAX);   /* the field is the kernel's: it must be NUL-padded */
    memcpy(dst, src, n);
}

/* --- ioctl request inventory --------------------------------------------- *
 *
 * TWO tables, split by what a line is WORTH, because a cap that can hide the
 * thing the run exists to find is worse than no cap: it reads as an answer.
 *
 * A whitelisted request carries an ifreq name, so it is one of the codes the
 * twelve functions were predicted to send -- SIGNAL. There are only ever a
 * handful, and it is logged UNCONDITIONALLY. Its table is a dedupe guard, not a
 * limit; on the impossible overflow the line is still emitted, because a missing
 * signal line says "this never happened", which is a wrong answer rather than a
 * truncated one.
 *
 * Everything else is NOISE: the GPIO / InterCpuCom / touch-panel / ALSA ranges
 * rbp drives at tick rate. Recorded by NUMBER ONLY -- see the interposer for why
 * the name may not be read out of them -- and capped, because what those lines
 * are for is showing which ranges passed through, and the first ~32 distinct
 * codes settle that.
 *
 * The first version used ONE capped table for both. On the unit, Stage 1 filled
 * all 32 slots with noise before rbp's network code ran, so the log printed
 * `inventory full` and could not say whether SIOCGIFADDR had happened. Both
 * tables now say what they are, and the overflow line states the property that
 * matters: ifreq requests are still logged. */

static struct {
    unsigned long req;
    char label[NA_NAME_MAX];
} g_seen[SEEN_REQ_MAX];
static int g_seen_n;

static unsigned long g_noise[SEEN_REQ_MAX];
static int g_noise_n;
static int g_noise_full;

/* One line per distinct (request, name) for signal, per distinct request for
 * noise. This is what makes the shadow run worth doing: the request numbers rbp
 * actually sends, so a whitelist built from a disassembly can be checked against
 * the wire. */
static void note_req(unsigned long req, const char *label, int rewrote)
{
    int i;

    if (label != NULL) {
        for (i = 0; i < g_seen_n; i++)
            if (g_seen[i].req == req && strcmp(g_seen[i].label, label) == 0)
                return;
        if (g_seen_n < SEEN_REQ_MAX) {
            g_seen[g_seen_n].req = req;
            set_name(g_seen[g_seen_n].label, label);
            g_seen_n++;
        }
        emitf("NETALIAS v%d pid=%d ioctl req=0x%lx name=\"%s\" -> %s\n",
             NETALIAS_V, (int)getpid(), req, label,
             rewrote ? "rewritten" : (g_on ? "pass" : "pass (shadow)"));
        return;
    }

    for (i = 0; i < g_noise_n; i++)
        if (g_noise[i] == req)
            return;

    if (g_noise_n >= SEEN_REQ_MAX) {
        if (!g_noise_full) {
            g_noise_full = 1;
            emitf("NETALIAS v%d pid=%d ioctl noise inventory full (%d distinct), "
                 "further non-ifreq requests not logged -- ifreq requests still are\n",
                 NETALIAS_V, (int)getpid(), SEEN_REQ_MAX);
        }
        return;
    }

    g_noise[g_noise_n++] = req;
    emitf("NETALIAS v%d pid=%d ioctl req=0x%lx (not an ifreq) -> pass\n",
         NETALIAS_V, (int)getpid(), req);
}

/* --- the three interposers ----------------------------------------------- */

/* Everything this shim does not rewrite goes to the NEXT ioctl in the load
 * order, which is fbshim's -- never to a raw syscall, which would skip it and
 * break the display. If resolution somehow failed there is nothing correct left
 * to do, so the armed line says MISSING and this falls back to the syscall
 * rather than crashing. */
static int call_real_ioctl(int fd, unsigned long request, void *arg)
{
    if (g_real_ioctl)
        return g_real_ioctl(fd, request, arg);
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

int ioctl(int fd, unsigned long request, ...)
{
    struct na_ifreq *ifr;
    char orig[NA_NAME_MAX], label[NA_NAME_MAX];
    int whitelisted;
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (!g_active)
        return call_real_ioctl(fd, request, arg);

    /* THE ONLY PLACE arg IS DEREFERENCED, and it is gated on the request being
     * one that carries an ifreq. An arbitrary ioctl's third argument is as
     * often an integer as a pointer -- ioctl(fd, TIOCSBRK) passes none at all --
     * so reading 16 bytes of "name" out of it would be a fault in the pointer
     * loop or the ALSA path, for a name that does not exist. */
    whitelisted = arg != NULL && na_ioctl_whitelisted(request);
    if (whitelisted) {
        ifr = (struct na_ifreq *)arg;
        memcpy(orig, ifr->name, NA_NAME_MAX);
        label_of(orig, label);
    }

    if (g_on && whitelisted && na_name_rewrite(orig, g_alias)) {
        int r;

        ifr = (struct na_ifreq *)arg;
        set_name(ifr->name, g_alias);
        r = call_real_ioctl(fd, request, arg);
        /* Put the caller's own name back. The kernel has already answered into
         * the union; ifr_name is the caller's field, and leaving our alias in it
         * would be this shim editing a struct rbp may reuse or print. */
        memcpy(ifr->name, orig, NA_NAME_MAX);
        if (g_log)
            note_req(request, label, 1);
        return r;
    }

    if (g_log)
        note_req(request, whitelisted ? label : NULL, 0);
    return call_real_ioctl(fd, request, arg);
}

/* The index is NEVER cached: it is stable only while the interface exists, and
 * rbp's caller (MacAddressListener::run) already handles a 0 by not binding.
 * Answering 0 for a vanished interface is exactly what the real call would do. */
unsigned int if_nametoindex(const char *ifname)
{
    if (g_active && g_log && ifname && na_name_rewrite(ifname, g_alias))
        emitf("NETALIAS v%d pid=%d if_nametoindex \"%s\" -> %s\n",
             NETALIAS_V, (int)getpid(), ifname,
             g_on ? g_alias : "(shadow: not substituted)");

    if (g_on && ifname && na_name_rewrite(ifname, g_alias))
        return g_real_if_nametoindex ? g_real_if_nametoindex(g_alias) : 0;

    return g_real_if_nametoindex ? g_real_if_nametoindex(ifname) : 0;
}

static unsigned long g_sys_seen[SEEN_SYS_MAX];
static int g_sys_n;
static int g_sys_full;

int system(const char *cmd)
{
    if (g_active && g_log && cmd && *cmd) {
        unsigned long h = fnv1a(cmd);
        char out[NA_CMD_MAX];
        enum na_sys_verdict v = na_system_verdict(cmd, g_alias, out, sizeof out);
        /* The cap bounds the pass-throughs, not the verdicts -- and the same
         * reasoning as note_req applies: a REFUSE or REWRITE line that a full
         * table swallowed would read as "no command matched", which is the one
         * wrong answer this log must not give. */
        int act = v != NA_SYS_PASS;
        int i, dup = 0;

        for (i = 0; i < g_sys_n; i++)
            if (g_sys_seen[i] == h) {
                dup = 1;
                break;
            }

        if (!dup && (act || g_sys_n < SEEN_SYS_MAX)) {
            if (g_sys_n < SEEN_SYS_MAX)
                g_sys_seen[g_sys_n++] = h;
            if (v == NA_SYS_REWRITE)
                emitf("NETALIAS v%d pid=%d system %s \"%s\" -> would run \"%s\"\n",
                     NETALIAS_V, (int)getpid(), g_mac_scrape ? "armed" : "shadow",
                     cmd, out);
            else
                emitf("NETALIAS v%d pid=%d system %s \"%s\" -> %s\n",
                     NETALIAS_V, (int)getpid(), g_mac_scrape ? "armed" : "shadow",
                     cmd, na_sys_verdict_name(v));
        } else if (!dup && !g_sys_full) {
            g_sys_full = 1;
            emitf("NETALIAS v%d pid=%d system inventory full (%d distinct), further "
                 "pass-through commands not logged -- commands the shim would act on "
                 "still are\n", NETALIAS_V, (int)getpid(), SEEN_SYS_MAX);
        }
    }

    /* The verdicts are applied ONLY when the operator asked for it. With the
     * scrape off this is a pure pass-through with a log line, which is what
     * "observing" has to mean if the default is to change nothing. */
    if (g_on && g_mac_scrape && cmd) {
        char out[NA_CMD_MAX];
        enum na_sys_verdict v = na_system_verdict(cmd, g_alias, out, sizeof out);

        if (v == NA_SYS_REFUSE) {
            emitf("NETALIAS v%d pid=%d system REFUSED \"%s\"\n",
                 NETALIAS_V, (int)getpid(), cmd);
            /* The shell's "command not found", and errno deliberately untouched:
             * this is a refusal, not a failed exec. */
            return 127 << 8;
        }
        if (v == NA_SYS_REWRITE) {
            emitf("NETALIAS v%d pid=%d system rewrote \"%s\"\n",
                 NETALIAS_V, (int)getpid(), out);
            return g_real_system ? g_real_system(out) : -1;
        }
    }

    return g_real_system ? g_real_system(cmd) : -1;
}

/* --- interface selection ------------------------------------------------- */

static void choose_alias(void)
{
    struct ifaddrs *ifa = NULL, *p;
    struct na_iface ifs[NA_IF_MAX];
    const char *want = env_str("NETALIAS_IFACE", "");
    int n = 0;

    g_alias[0] = '\0';
    g_alias_src = "none";

    if (getifaddrs(&ifa) != 0 || ifa == NULL)
        return;   /* rule 4: identity */

    for (p = ifa; p != NULL; p = p->ifa_next) {
        int i;

        if (!p->ifa_name || !*p->ifa_name)
            continue;
        /* IFNAMSIZ is 16 and so is na_iface.name, so this cannot fire; it is
         * here so that a name that did NOT fit could never become a candidate
         * this shim then answers with. */
        if (strlen(p->ifa_name) >= sizeof ifs[0].name)
            continue;

        for (i = 0; i < n; i++)
            if (strcmp(ifs[i].name, p->ifa_name) == 0)
                break;
        if (i == n) {
            if (n >= NA_IF_MAX)
                continue;   /* no room: skip the interface rather than alias it */
            memset(&ifs[n], 0, sizeof ifs[n]);
            memcpy(ifs[n].name, p->ifa_name, strlen(p->ifa_name) + 1);
            n++;
        }

        /* One row per interface: getifaddrs returns one per ADDRESS, so the
         * flags are OR'd and "has an IPv4 address" is sticky. */
        if (p->ifa_flags & IFF_UP)       ifs[i].flags |= NA_IFF_UP;
        if (p->ifa_flags & IFF_RUNNING)  ifs[i].flags |= NA_IFF_RUNNING;
        if (p->ifa_flags & IFF_LOOPBACK) ifs[i].flags |= NA_IFF_LOOPBACK;
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET)
            ifs[i].has_v4 = 1;
    }
    freeifaddrs(ifa);

    if (na_pick_alias(ifs, n, want, g_alias, (int)sizeof g_alias))
        g_alias_src = (want[0] && strcmp(g_alias, want) == 0) ? "env" : "auto";
    else
        g_alias[0] = '\0';
}

/* --- the one-shot Link bring-up ------------------------------------------ */

/*
 * Is this process the PLAYER? Nothing below may run unless it is.
 *
 * rbp puts LD_PRELOAD in its OWN environment, so every process it starts through
 * `system()` inherits it and loads this file too -- MEASURED on the unit, not
 * supposed: a shell the player spawned logged a terminal-size ioctl (0x5413)
 * through this shim. In the player, NM_SINGLETON is inside its own .bss and
 * NM_FN_CONNECT inside its own .text; in a spawned shell the first is that
 * shell's heap or nothing at all and the second is unmapped. Reading the first
 * is a segfault and calling the second is a jump into whatever lives there. So a
 * child must never reach them.
 *
 * The decision itself is na_maps_line_is_code_at() in netalias.c, where
 * test_netalias can pin it against real /proc/self/maps lines; this is only the
 * read of the file. Everything it cannot parse answers "no", so the shim fails
 * closed.
 */
static int running_as_player(void)
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    int ok = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof line, f)) {
        if (na_maps_line_is_code_at(line, (unsigned long)NM_FN_CONNECT, RBP_BIN_NAME)) {
            ok = 1;
            break;
        }
    }

    fclose(f);
    return ok;
}

/*
 * The name substitution above makes rbp's NetworkMonitor able to answer with
 * wlan0's address. It does not by itself put the player on the Link, but NOT for
 * the reason an earlier version of this comment gave. That version said the call
 * that does it -- NetworkManager::operateConnectNetwork @0x38f830 -- "HAS NO CALLER
 * in the binary". It does have one: the function is virtual, and
 * NetworkMonitor::timerCallback @0x392160 reaches it through vtable slot +12 once a
 * second, off a timer main's NetworkManager::initialize() starts unconditionally.
 * What the tick tests before it calls is ui::PcController::isUsbBConnected()
 * (PcController+0x72), and while that byte is zero no connect is ever attempted,
 * however good the answer is.
 *
 * rbp sets that byte ITSELF, from its own USB FIFO protocol: the word `connect` on
 * /tmp/udev_usb1 is read by read_sf_rbp @0x3785b4, broadcast by
 * ui::UsbMountManager::run @0x320a28 as EnUsbMountMessage 3, and stored by
 * ui::PcController::handleUsbMountMessage @0x2e9700. That is the in-band route, and
 * an earlier version of this comment called it "the one to prefer".
 *
 * IT IS NOT ENOUGH, AND THAT WAS MEASURED. Run on the unit 2026-10-07: the word
 * `connect` really does drive the byte to 1 -- read back out of the running process,
 * on the PcController and again through both PcChController's +0x54 -- and rbp still
 * opens no socket. timerCallback reads that gate THROUGH
 * IUiObjManager::getPcController(), and the rbp-nopc stage stubbed that getter to
 * `mov r0,#0; bx lr` (a stub byte-identical to the thunks Pioneer ships for its own
 * unimplemented functions, which is why it read as harmless). With it stubbed the
 * tick computes r3 = 0 however the gate is set, so the connect branch is never taken
 * -- on this route OR this one. The stub is gone as of 2026-10-07 (the getters now
 * return NULL instead of faulting, keeping their stock 5-word shape; see
 * tools/patch-rbp/PATCHES.md section 11). THE IN-BAND DRILL HAS NOT BEEN RE-RUN on
 * the fixed build, so neither route is proven. docs/18-prodjlink.md has the whole
 * chain and the run.
 *
 * This block is the OUT-OF-BAND alternative: one direct call into rbp, kept for the
 * case where the gate itself is what is under test, and because a built and tested
 * route costs nothing while it stays disarmed. It is the smallest way to make that
 * call happen ON DEMAND and exactly ONCE. A request FILE, not a signal and not a
 * keystroke: the file is a thing the operator can create with one command and read
 * back, it needs no protocol with the shim, and a stale one is visible rather than
 * invisible.
 *
 *   echo > /tmp/rb_link.req          <- and watch /tmp/netshim.log
 *
 * A request that is REFUSED IS KEPT, so a file dropped before rbp has reached
 * NetworkManager::getInstance() still fires when it does; the refusal is logged
 * once per distinct verdict rather than at the poll rate. A request that fires
 * is UNLINKED BEFORE the call, so a crash inside rbp cannot loop it.
 *
 * This is the only code here that runs rbp's code, so it runs none of its own
 * without a reason: na_connect_gate() refuses while NetworkMonitor has no
 * address, because operateConnectNetwork returns at its first compare and the
 * firing would be a SILENT no-op -- the failure this module exists to avoid.
 */
static void *connect_thread(void *arg)
{
    (void)arg;

    for (;;) {
        unsigned int nm  = 0, ip = 0, flg = 0;
        enum na_connect_verdict v;

        usleep(CONNECT_POLL_MS * 1000);
        if (access(g_connect_file, F_OK) != 0)
            continue;

        /* Read the singleton ONCE into a local and use that same value for the
         * gate and the call. rbp's main is running concurrently and may publish
         * the singleton between the two reads; two reads could gate on an object
         * and then call a different one. */
        nm = *(volatile unsigned int *)NM_SINGLETON;
        if (nm != 0) {
            ip  = *(volatile unsigned int *)(nm + NM_OFF_IP);
            flg = *(volatile unsigned char *)(nm + NM_OFF_CONNECT_FLG);
        }

        v = na_connect_gate(nm, ip);
        if (v != NA_CONNECT_GO) {
            if ((int)v != g_connect_said) {
                g_connect_said = (int)v;
                emitf("NETALIAS v%d pid=%d connect refused verdict=%s nm=0x%08x "
                      "flg=%u ip=0x%08x -- request kept, will retry\n",
                      NETALIAS_V, (int)getpid(), na_connect_verdict_name(v),
                      nm, flg, ip);
            }
            continue;
        }

        g_connect_said = 0;
        unlink(g_connect_file);
        emitf("NETALIAS v%d pid=%d connect FIRED nm=0x%08x flg=%u ip=0x%08x "
              "-> NetworkManager::operateConnectNetwork\n",
              NETALIAS_V, (int)getpid(), nm, flg, ip);
        ((void (*)(void *, int))NM_FN_CONNECT)((void *)nm, 0);
        emitf("NETALIAS v%d pid=%d connect returned\n", NETALIAS_V, (int)getpid());
    }
    return NULL;
}

/* --- load ---------------------------------------------------------------- */

static void armed(void)
{
    emitf("NETALIAS v%d pid=%d armed log=%s on=%d mac_scrape=%d alias=%s src=%s "
         "real=ioctl:%s,if_nametoindex:%s,system:%s connect=%d player=%d file=%s\n",
         NETALIAS_V, (int)getpid(), NETALIAS_LOG_PATH, g_on, g_mac_scrape,
         g_alias[0] ? g_alias : "none", g_alias_src,
         g_real_ioctl ? "ok" : "MISSING",
         g_real_if_nametoindex ? "ok" : "MISSING",
         g_real_system ? "ok" : "MISSING",
         g_connect, g_connect_player, g_connect_file);
}

__attribute__((constructor)) static void init(void)
{
    /* The flag names carry no RB_ prefix HERE, and that is not an oversight:
     * rb.conf defines RB_NETALIAS and start-rb.sh's SHIM_VARS loop re-exports it
     * as NETALIAS. crashcatch.c reads CRASH_LOG for the same reason. */
    g_on = env_flag("NETALIAS", 0);
    g_log = env_flag("NETALIAS_LOG", 0);
    g_mac_scrape = env_flag("NETALIAS_MAC_SCRAPE", 0);
    g_connect = env_flag("NETALIAS_CONNECT", 0);
    g_connect_file = env_str("NETALIAS_CONNECT_FILE", CONNECT_FILE_DEFAULT);

    /* Arming the trigger turns the log ON, and it is not a convenience. This is
     * the one thing in the shim that CALLS INTO rbp, and the call is a no-op
     * unless NetworkMonitor already holds an address — so its two outcomes are
     * "it worked" and "silence", which read the same from outside. A run that
     * can write into the player without writing down what it did is not a drill
     * anyone can read afterwards. */
    if (g_connect)
        g_log = 1;

    /* Resolved UNCONDITIONALLY, even with the shim switched off: LD_PRELOAD
     * installs these interposers either way, so every ioctl in the process
     * passes through this file whatever the flags say. */
    *(void **)(&g_real_ioctl) = dlsym(RTLD_NEXT, "ioctl");
    *(void **)(&g_real_if_nametoindex) = dlsym(RTLD_NEXT, "if_nametoindex");
    *(void **)(&g_real_system) = dlsym(RTLD_NEXT, "system");

    if (!g_on && !g_log)
        return;   /* off and quiet: every hook is already an identity */

    /* Selection runs BEFORE g_active is raised, and that ordering is
     * deliberate: getifaddrs() is itself a caller of ioctl(SIOCGIFCONF), and
     * with the guard still down those calls take the fast path out of the
     * interposer. Raising it first would put our OWN probe into the inventory
     * log, where the next reader would take it for something rbp does. */
    /* Asked BEFORE the armed line, only when the trigger is armed, and it is what
     * that line's player= field reports. In every process the player spawns
     * through `system()` -- which inherits LD_PRELOAD -- the answer is no, and
     * the thread is then not created at all: see running_as_player(). */
    if (g_connect)
        g_connect_player = running_as_player();

    choose_alias();
    g_active = 1;
    armed();

    if (g_connect && g_connect_player
        && pthread_create(&g_connect_tid, NULL, connect_thread, NULL) != 0)
        emitf("NETALIAS v%d pid=%d connect=1 but the poll thread did not start; "
              "the trigger file will be ignored\n", NETALIAS_V, (int)getpid());
}
