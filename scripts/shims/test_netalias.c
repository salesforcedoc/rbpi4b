/*
 * test_netalias.c -- every verdict netshim.so makes, off the unit. No Pi, no
 * interface, no rbp, no address of rbp's anywhere in this file.
 *
 * It links the PRODUCTION netalias.c, which is pure, and pins the things that
 * would otherwise only ever be seen as a Link screen that stays empty — or, in
 * the one case that matters more than all the others, as an operator who has
 * lost SSH to the unit:
 *
 * 1. THE SETTERS. Every SIOCGIF* this module retargets has a SIOCSIF* twin that
 *    writes instead of reads, and one of rbp's twelve eth0 functions is
 *    setIpAddr @0x39dfb4. Retargeting an address assignment at wlan0 would write
 *    rbp's idea of an address onto the interface carrying the operator's own
 *    session. The test walks the pairs: the getter is accepted, its setter is
 *    refused. This is the first test because it is the only verdict here whose
 *    failure is not merely a feature that does not work.
 *
 * 2. THE CONSTANTS. The numbers in netalias.h are written out rather than
 *    included, so that the module compiles with no kernel header. This file
 *    includes the real ones and asserts they agree — struct layout and flags
 *    too — so a value that ever drifted is a red test rather than a shim that
 *    silently rewrites nothing.
 *
 * 3. THE NAME MATCH. "eth0" yes; "eth1", "eth", "eth0x" and a field whose fifth
 *    byte is not the terminator all no. A rewrite that fired on the wrong name
 *    would answer an interface rbp did not ask about.
 *
 * 4. THE SELECTION. The four rules of na_pick_alias(), including the one this
 *    unit is actually in — eth0 present but DOWN with carrier 0 — and the two
 *    identity fallbacks. Note the case that is NOT identity: an explicit
 *    RB_NETALIAS_IFACE naming an interface that is not there falls through to
 *    the measurements rather than standing the shim down.
 *
 * 5. THE DHCP REFUSAL. `udhcpc -i eth0 ...` may never be rewritten, because a
 *    lease solicited on wlan0 is the one change this module may not make to the
 *    link the operator is using.
 *
 * 6. THE LINK GATE. na_connect_gate() stands between a request file and a direct
 *    call into the player, and both of its refusals are what make that call
 *    readable afterwards: without them, "nothing happened" would mean either
 *    "rbp was not up yet" or "the substitution did not take". The order of the
 *    two is asserted, because it decides which of those the log names.
 *
 * 7. THE PLAYER TEST. rbp exports LD_PRELOAD to every process it starts through
 *    `system()`, so a shell it spawns loads this shim too and must not read or
 *    call the player's addresses. na_maps_line_is_code_at() is what tells the two
 *    apart, and the lines it is tested against are copied from the live unit's
 *    /proc/<rbp>/maps — including the anonymous one the singleton actually sits
 *    in, which must answer no.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "netalias.h"

#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

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

/* --- 1. the constants ---------------------------------------------------- */

static void test_constants(void)
{
    /* The request numbers, against glibc's own <sys/ioctl.h>. If any of these
     * ever disagreed, the whitelist would be a list of numbers no caller ever
     * sends: the module would compile, load, log nothing and rewrite nothing. */
    CHECK(NA_SIOCGIFNAME    == SIOCGIFNAME,    "SIOCGIFNAME");
    CHECK(NA_SIOCGIFCONF    == SIOCGIFCONF,    "SIOCGIFCONF");
    CHECK(NA_SIOCGIFFLAGS   == SIOCGIFFLAGS,   "SIOCGIFFLAGS");
    CHECK(NA_SIOCGIFADDR    == SIOCGIFADDR,    "SIOCGIFADDR");
    CHECK(NA_SIOCGIFDSTADDR == SIOCGIFDSTADDR, "SIOCGIFDSTADDR");
    CHECK(NA_SIOCGIFBRDADDR == SIOCGIFBRDADDR, "SIOCGIFBRDADDR");
    CHECK(NA_SIOCGIFNETMASK == SIOCGIFNETMASK, "SIOCGIFNETMASK");
    CHECK(NA_SIOCGIFMETRIC  == SIOCGIFMETRIC,  "SIOCGIFMETRIC");
    CHECK(NA_SIOCGIFMTU     == SIOCGIFMTU,     "SIOCGIFMTU");
    CHECK(NA_SIOCGIFHWADDR  == SIOCGIFHWADDR,  "SIOCGIFHWADDR");
    CHECK(NA_SIOCGIFINDEX   == SIOCGIFINDEX,   "SIOCGIFINDEX");
    CHECK(NA_SIOCGIFTXQLEN  == SIOCGIFTXQLEN,  "SIOCGIFTXQLEN");

    /* The interface flags and the name field width, likewise. */
    CHECK(NA_IFF_UP       == IFF_UP,       "IFF_UP");
    CHECK(NA_IFF_RUNNING  == IFF_RUNNING,  "IFF_RUNNING");
    CHECK(NA_IFF_LOOPBACK == IFF_LOOPBACK, "IFF_LOOPBACK");
    CHECK(NA_NAME_MAX     == IFNAMSIZ,     "IFNAMSIZ is %d, NA_NAME_MAX is %d",
          IFNAMSIZ, NA_NAME_MAX);

    /* The struct. netshim.c hands the kernel a na_ifreq through an ioctl, so a
     * shorter one would be a read past the end of the caller's own buffer; a
     * longer one would be a write past it. */
    CHECK(sizeof(struct na_ifreq) == sizeof(struct ifreq),
          "na_ifreq is %u bytes, ifreq is %u",
          (unsigned)sizeof(struct na_ifreq), (unsigned)sizeof(struct ifreq));
    CHECK(offsetof(struct na_ifreq, name) == offsetof(struct ifreq, ifr_name),
          "the name field is not at the same offset as ifr_name");
}

/* --- 2. the whitelist, and its setters ----------------------------------- */

/* Every pair: the read this module answers for, and the write it must not.
 * Walked as a table so a request added to one column without the other is
 * visible here rather than in the field. */
static const struct {
    const char *what;
    unsigned long get;
    unsigned long set;
} PAIRS[] = {
    { "addr",    SIOCGIFADDR,    SIOCSIFADDR    },
    { "dstaddr", SIOCGIFDSTADDR, SIOCSIFDSTADDR },
    { "brdaddr", SIOCGIFBRDADDR, SIOCSIFBRDADDR },
    { "netmask", SIOCGIFNETMASK, SIOCSIFNETMASK },
    { "metric",  SIOCGIFMETRIC,  SIOCSIFMETRIC  },
    { "mtu",     SIOCGIFMTU,     SIOCSIFMTU     },
    { "hwaddr",  SIOCGIFHWADDR,  SIOCSIFHWADDR  },
    { "flags",   SIOCGIFFLAGS,   SIOCSIFFLAGS   },
    { "txqlen",  SIOCGIFTXQLEN,  SIOCSIFTXQLEN  },
};

static void test_whitelist(void)
{
    unsigned i;

    for (i = 0; i < sizeof PAIRS / sizeof PAIRS[0]; i++) {
        CHECK(na_ioctl_whitelisted(PAIRS[i].get),
              "SIOCGIF%s (%s) should be answered for", PAIRS[i].what, "(get)");
        CHECK(!na_ioctl_whitelisted(PAIRS[i].set),
              "SIOCSIF%s (%s) must NEVER be retargeted -- see netalias.h",
              PAIRS[i].what, "set");
        /* And the refusal has to hold at the rewrite, not only in the table:
         * this is the exact call that would move the operator's link. */
        CHECK(!na_ioctl_rewrite(PAIRS[i].set, NA_FROM, "wlan0"),
              "a setter reached na_ioctl_rewrite and was accepted");
    }

    /* Renaming an interface, which has no getter twin at all. */
    CHECK(!na_ioctl_whitelisted(SIOCSIFNAME), "SIOCSIFNAME must not be answered for");
    CHECK(!na_ioctl_rewrite(SIOCSIFNAME, NA_FROM, "wlan0"), "SIOCSIFNAME accepted");

    /* The two read requests that are refusals with a reason of their own:
     * SIOCGIFCONF writes the interface list into ifr_name, and SIOCGIFNAME
     * answers with a name rather than taking one. */
    CHECK(!na_ioctl_whitelisted(SIOCGIFCONF), "SIOCGIFCONF must not be retargeted");
    CHECK(!na_ioctl_whitelisted(SIOCGIFNAME), "SIOCGIFNAME must not be retargeted");

    /* Requests that are not ifreqs at all, so that arg must not be read. The
     * first is fbshim's own framebuffer range and the second is the tty one the
     * pointer loop uses; both pass through this shim to reach fbshim. */
    CHECK(!na_ioctl_whitelisted(0x4600), "FBIOGET_VSCREENINFO must pass through");
    CHECK(!na_ioctl_whitelisted(0x5401), "TCGETS must pass through");
    CHECK(!na_ioctl_whitelisted(0),      "request 0 must pass through");
}

/* --- 3. the name match --------------------------------------------------- */

/* A 16-byte field as the kernel holds it: the name, then whatever the caller
 * left. Built here rather than as a string literal so the byte after the
 * terminator is under the test's control. */
static void field(char *out, const char *name, char filler)
{
    memset(out, filler, NA_NAME_MAX);
    memcpy(out, name, strlen(name));
    out[strlen(name)] = '\0';
}

static void test_name_match(void)
{
    char f[NA_NAME_MAX];

    field(f, NA_FROM, 0x00);
    CHECK(na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"), "exactly \"eth0\" must be rewritten");

    /* The terminator is the end of the name: anything after it is the caller's
     * leftovers and must not be read as part of it. */
    field(f, NA_FROM, (char)0xa5);
    CHECK(na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"),
          "garbage after the NUL is not part of the name");

    field(f, "eth1", 0x00);
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"), "eth1 must not be rewritten");
    field(f, "eth", 0x00);
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"), "a prefix of eth0 is not eth0");
    /* The one that a plain strncmp(.., 4) would get wrong: "eth0" as the first
     * four bytes of a LONGER name. */
    field(f, "eth0x", 0x00);
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"), "eth0x is a different interface");
    field(f, "", 0x00);
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, "wlan0"), "an empty name is not eth0");
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, NULL, "wlan0"), "a NULL name must not be read");

    /* The alias side: nothing to substitute, or a substitution that is the same
     * name, is not a rewrite. */
    field(f, NA_FROM, 0x00);
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, ""),       "an empty alias is identity");
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, NULL),     "a NULL alias is identity");
    CHECK(!na_ioctl_rewrite(SIOCGIFADDR, f, NA_FROM),  "alias == eth0 is identity");
}

/* --- 4. the selection ---------------------------------------------------- */

static struct na_iface IF(const char *name, unsigned flags, int v4)
{
    struct na_iface f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", name);
    f.flags = flags;
    f.has_v4 = v4;
    return f;
}

#define UP (NA_IFF_UP | NA_IFF_RUNNING)
#define LOOPBACK_UP (NA_IFF_UP | NA_IFF_RUNNING | NA_IFF_LOOPBACK)

static void test_selection(void)
{
    char out[NA_NAME_MAX];

    /* The unit's actual state: eth0 present, DOWN, no carrier, no address; wlan0
     * the live link. This is the case the whole module exists for. */
    struct na_iface unit[2] = { IF("eth0", 0, 0), IF("wlan0", UP, 1) };
    CHECK(na_pick_alias(unit, 2, NULL, out, sizeof out) && strcmp(out, "wlan0") == 0,
          "eth0 down + wlan0 up must alias to wlan0, got \"%s\"", out);

    /* Cable in: stand down entirely, whatever else is up. */
    struct na_iface wired[2] = { IF("eth0", UP, 1), IF("wlan0", UP, 1) };
    CHECK(!na_pick_alias(wired, 2, NULL, out, sizeof out),
          "eth0 up must be identity, got \"%s\"", out);

    /* eth0 up but WITHOUT carrier (RUNNING clear) -- a cable that is in the
     * socket and not connected. Not rbp's interface in any usable sense. */
    struct na_iface nocarrier[2] = { IF("eth0", NA_IFF_UP, 0), IF("wlan0", UP, 1) };
    CHECK(na_pick_alias(nocarrier, 2, NULL, out, sizeof out) && strcmp(out, "wlan0") == 0,
          "eth0 up without carrier is not a link, got \"%s\"", out);

    /* An explicit choice is taken as given -- present is the only test, and it
     * is not second-guessed on flags or on an address. */
    struct na_iface many[3] = { IF("eth0", UP, 1), IF("wlan0", UP, 1), IF("usb0", 0, 0) };
    CHECK(na_pick_alias(many, 3, "usb0", out, sizeof out) && strcmp(out, "usb0") == 0,
          "an explicit choice wins even over a live eth0, got \"%s\"", out);

    /* ...and a choice naming an interface that is NOT there falls through to
     * the measurements. Answering identity would turn a typo into a feature
     * that silently does nothing. (The list here has eth0 DOWN, because with a
     * live eth0 rule 2 answers identity and the fall-through would be
     * indistinguishable from it.) */
    struct na_iface down[2] = { IF("eth0", 0, 0), IF("wlan0", UP, 1) };
    CHECK(na_pick_alias(down, 2, "wlan9", out, sizeof out) && strcmp(out, "wlan0") == 0,
          "an absent choice must fall through, got \"%s\"", out);

    /* Two passes: a wireless name wins over a usable wired one. */
    struct na_iface both[3] = { IF("eth0", 0, 0), IF("enx00e04c", UP, 1), IF("wlp3s0", UP, 1) };
    CHECK(na_pick_alias(both, 3, NULL, out, sizeof out) && strcmp(out, "wlp3s0") == 0,
          "a wl* name must be preferred, got \"%s\"", out);
    /* ...and the preference is a preference, not a requirement: with no wireless
     * name at all, the usable wired one is still better than nothing. */
    struct na_iface nowl[2] = { IF("eth0", 0, 0), IF("enx00e04c", UP, 1) };
    CHECK(na_pick_alias(nowl, 2, NULL, out, sizeof out) && strcmp(out, "enx00e04c") == 0,
          "a usable non-wireless interface must still be taken, got \"%s\"", out);

    /* Loopback, an addressless interface and a down interface are each not
     * candidates, alone and together. */
    struct na_iface bad[3] = { IF("lo", LOOPBACK_UP, 1), IF("eth1", UP, 0), IF("eth2", 0, 1) };
    CHECK(!na_pick_alias(bad, 3, NULL, out, sizeof out),
          "loopback / addressless / down must all be identity, got \"%s\"", out);

    /* eth0 down and nothing else at all. */
    struct na_iface alone[1] = { IF("eth0", 0, 0) };
    CHECK(!na_pick_alias(alone, 1, NULL, out, sizeof out), "nothing usable must be identity");

    /* Degenerate inputs answer identity, and none of them may read `ifs`. */
    CHECK(!na_pick_alias(NULL, 0, NULL, out, sizeof out), "a NULL list is identity");
    CHECK(!na_pick_alias(unit, 0, NULL, out, sizeof out), "an empty list is identity");
    CHECK(!na_pick_alias(unit, -1, NULL, out, sizeof out), "a negative count is identity");
    CHECK(!na_pick_alias(unit, 2, NULL, NULL, 0), "no output buffer is identity");

    /* A name that will not fit is REFUSED, never truncated: a shortened
     * interface name is a name that does not exist, and answering rbp's queries
     * about one that does not exist is worse than not answering at all. */
    struct na_iface wide[1] = { IF("wlan0", UP, 1) };
    char small[4];
    CHECK(!na_pick_alias(wide, 1, NULL, small, sizeof small),
          "a name too long for the buffer must be refused, not cut to \"%s\"", small);
    CHECK(small[0] == '\0', "a refused name must leave the buffer empty");

    /* na_iface_present, which is rule 1's own test. */
    CHECK(na_iface_present(many, 3, "usb0"), "usb0 is present in the list");
    CHECK(!na_iface_present(many, 3, "usb1"), "usb1 is not present in the list");
    CHECK(!na_iface_present(many, 3, NULL),   "a NULL name is never present");
    CHECK(!na_iface_present(many, 3, ""),     "an empty name is never present");
}

/* --- 5. the system hook -------------------------------------------------- */

static void test_system(void)
{
    char out[NA_CMD_MAX];

    /* The DHCP client. REFUSE, and note the rule is broader than the one command
     * line measured -- every udhcpc is refused, because the protection wanted is
     * "no lease is ever solicited on the operator's link" and a narrower pattern
     * is a pattern to get wrong. */
    const char *udhcpc = "udhcpc -i eth0 -T 2 -t 3 -n -q";
    CHECK(na_system_verdict(udhcpc, "wlan0", out, sizeof out) == NA_SYS_REFUSE,
          "udhcpc must be refused");
    CHECK(na_system_verdict("busybox udhcpc -i eth0", "wlan0", out, sizeof out)
              == NA_SYS_REFUSE,
          "any udhcpc must be refused");
    /* Not even asked to rewrite it: the refusal is not a substitution. */
    CHECK(out[0] == '\0', "a refused command must not leave a rewritten line behind");

    /* The MAC scrape, rewritten in place with the redirect kept verbatim. */
    const char *scrape = "ifconfig | awk '$1 ~ /^eth/ {print $NF}'";
    CHECK(na_system_verdict(scrape, "wlan0", out, sizeof out) == NA_SYS_REWRITE,
          "the awk scrape must be rewritten");
    CHECK(strcmp(out, "ifconfig | awk '$1 ~ /^wlan0/ {print $NF}'") == 0,
          "rewrote to \"%s\"", out);

    /* Both halves are required, so neither alone is touched. */
    CHECK(na_system_verdict("awk '$1 ~ /^eth/ {print}'", "wlan0", out, sizeof out)
              == NA_SYS_PASS,
          "the pattern without ifconfig must not be rewritten");
    CHECK(na_system_verdict("ifconfig -a", "wlan0", out, sizeof out) == NA_SYS_PASS,
          "ifconfig without the pattern must not be rewritten");

    /* Everything else passes through. rbp reaches the kernel through `system`
     * for more than these two, and a shim that edited an unrelated command line
     * would be a shim editing shell. */
    CHECK(na_system_verdict("mount -t vfat /dev/sda1 /mnt", "wlan0", out, sizeof out)
              == NA_SYS_PASS, "an unrelated command must pass through");
    CHECK(na_system_verdict("", "wlan0", out, sizeof out) == NA_SYS_PASS,
          "an empty command must pass through");
    CHECK(na_system_verdict(NULL, "wlan0", out, sizeof out) == NA_SYS_PASS,
          "a NULL command must pass through");

    /* No alias, or an alias that is eth0 itself: nothing is decided. In
     * particular an INACTIVE shim must not refuse the DHCP client -- it is
     * observing only, and the command belongs to rbp. */
    CHECK(na_system_verdict(udhcpc, "", out, sizeof out) == NA_SYS_PASS,
          "with no alias, udhcpc must pass through");
    CHECK(na_system_verdict(udhcpc, NULL, out, sizeof out) == NA_SYS_PASS,
          "with no alias, udhcpc must pass through");
    CHECK(na_system_verdict(udhcpc, NA_FROM, out, sizeof out) == NA_SYS_PASS,
          "an alias of eth0 is identity");

    /* A buffer with no room is a PASS, not a truncation: a command line cut
     * short is a command that runs with arguments missing. */
    char tiny[8];
    CHECK(na_system_verdict(scrape, "wlan0", tiny, sizeof tiny) == NA_SYS_PASS,
          "a too-small buffer must pass the command through");
    CHECK(na_system_verdict(scrape, "wlan0", NULL, 0) == NA_SYS_PASS,
          "no buffer must pass the command through");

    /* The names, which are what the log line prints. */
    CHECK(strcmp(na_sys_verdict_name(NA_SYS_PASS), "pass") == 0, "verdict name pass");
    CHECK(strcmp(na_sys_verdict_name(NA_SYS_REWRITE), "rewrite") == 0, "verdict name rewrite");
    CHECK(strcmp(na_sys_verdict_name(NA_SYS_REFUSE), "refuse") == 0, "verdict name refuse");
}

/* --- 6. the one-shot Link bring-up gate ------------------------------------ */

/* The gate stands between a request file and a CALL INTO rbp, so its two
 * refusals are the whole of what keeps a drill readable: without them a request
 * dropped too early would look identical to one that fired and did nothing.
 *
 * The order is asserted too, and it is a decision rather than an accident: the
 * singleton is asked about first, so "no-ip" is only ever said about an object
 * that EXISTS. Inverting the two would report a missing address for a player
 * that has not started its network at all, which is a different fault with a
 * different answer -- drop the file later, not re-check the substitution. */
static void test_connect(void)
{
    /* Nothing there yet: main has not reached getInstance(). Refuse, and do not
     * confuse it with the address case even though ip reads 0 as well. */
    CHECK(na_connect_gate(0, 0) == NA_CONNECT_NO_SINGLETON,
          "no singleton must be no-singleton");
    CHECK(na_connect_gate(0, 0x0100007f) == NA_CONNECT_NO_SINGLETON,
          "a stray address with no singleton is still no-singleton");

    /* The object exists but NetworkMonitor holds no address -- the substitution
     * did not take. Firing now is a SILENT no-op, which is exactly the outcome
     * this module exists to make impossible. */
    CHECK(na_connect_gate(0x1000, 0) == NA_CONNECT_NO_IP,
          "a singleton with no address must be no-ip");

    /* Both present: go. The address's VALUE is never inspected -- any non-zero
     * word is NetworkMonitor saying it has one, and the gate is not a router. */
    CHECK(na_connect_gate(0x1000, 0xc0a801ef) == NA_CONNECT_GO,
          "singleton and address must be go");

    /* The names, which are what the refusal log line prints -- a reader has to
     * be able to tell the two refusals apart from the log alone. */
    CHECK(na_connect_verdict_name(NA_CONNECT_GO)           != NULL, "go name not NULL");
    CHECK(na_connect_verdict_name(NA_CONNECT_NO_SINGLETON) != NULL, "no-singleton name not NULL");
    CHECK(na_connect_verdict_name(NA_CONNECT_NO_IP)        != NULL, "no-ip name not NULL");
    CHECK(strcmp(na_connect_verdict_name(NA_CONNECT_GO), "go") == 0,
          "verdict name go");
    CHECK(strcmp(na_connect_verdict_name(NA_CONNECT_NO_SINGLETON), "no-singleton") == 0,
          "verdict name no-singleton");
    CHECK(strcmp(na_connect_verdict_name(NA_CONNECT_NO_IP), "no-ip") == 0,
          "verdict name no-ip");

    /* GO is 0, so a caller that zero-initialises a verdict has one to overwrite
     * before it means anything. Pinned because the shim stores it as an int. */
    CHECK(NA_CONNECT_GO == 0, "GO must be 0");
}

/* --- 6. "is this process the player?" -------------------------------------- */

/* These lines are COPIED from the live unit's /proc/<rbp>/maps, not invented, and
 * the addresses in them are the ones rbp_abi.h carries. The three that must
 * answer NO are the whole point: rbp exports LD_PRELOAD to every process it
 * starts through `system()`, so a shell spawned by the player loads this shim
 * too, and the shim must not read or call those addresses there. */
static void test_player_test(void)
{
    /* 0x38f830 lives here in the player: executable, file-backed by rbp. */
    const char *rx  = "002d1000-0050b000 r-xp 002c9000 b3:02 381935   "
                      "/opt/rblive4/rbx3-run/root/pdj/rbp\n";
    /* the same file, but NOT executable -- .data, not code */
    const char *rw  = "00512000-0053e000 rw-p 00502000 b3:02 381935   "
                      "/opt/rblive4/rbx3-run/root/pdj/rbp\n";
    /* the player's .bss tail, where NM_SINGLETON actually is: anonymous, and the
     * kernel labels it [heap] because the brk begins there. It is NOT code and
     * must never be treated as such. */
    const char *heap = "0053e000-05b5b000 rw-p 00000000 00:00 0                          "
                       "        [heap]\n";
    /* a mapping of the same shape belonging to something else */
    const char *other = "76f00000-76f20000 r-xp 00000000 b3:02 1234    /usr/bin/busybox\n";
    /* A pathname with no directory part at all. The kernel never prints one -- a
     * file mapping's path is absolute and an anonymous mapping has no pathname
     * field -- so a line that looks like this is a line we do not understand, and
     * it answers NO rather than being read on a guess. */
    const char *nodir = "00010000-00020000 r-xp 00000000 b3:02 1234    rbp\n";

    CHECK(na_maps_line_is_code_at(rx, 0x38f830UL, "rbp"), "the player's text mapping must match");
    CHECK(!na_maps_line_is_code_at(rw, 0x38f830UL, "rbp"), "a non-executable mapping must not match");
    CHECK(!na_maps_line_is_code_at(heap, 0x26873d8UL, "rbp"), "the anonymous heap must not match");
    CHECK(!na_maps_line_is_code_at(other, 0x38f830UL, "rbp"), "another program must not match");

    /* The address must be INSIDE the range: the end is exclusive, the start is not. */
    CHECK(na_maps_line_is_code_at(rx, 0x2d1000UL, "rbp"), "the first byte of the mapping is in it");
    CHECK(!na_maps_line_is_code_at(rx, 0x50b000UL, "rbp"), "the end of the mapping is not in it");
    CHECK(!na_maps_line_is_code_at(rx, 0x2d0fffUL, "rbp"), "one byte before is not in it");

    /* A different name of the same length, and the same name of a different
     * length, are both no -- the comparison is on the name AND its length, so
     * "rbpx" is not "rbp" and neither is "rb". */
    CHECK(!na_maps_line_is_code_at(rx, 0x38f830UL, "rbpx"), "a longer name must not match");
    CHECK(!na_maps_line_is_code_at(rx, 0x38f830UL, "rb"), "a shorter name must not match");

    /* The "(deleted)" suffix the kernel appends: the name is still read, which is
     * the point -- "no, because it is not rbp" and "no, because it is a deleted
     * rbp" are different answers and only one of them is a bug in the file. */
    const char *del = "002d1000-0050b000 r-xp 002c9000 b3:02 381935   "
                      "/opt/rblive4/rbx3-run/root/pdj/rbp (deleted)\n";
    CHECK(na_maps_line_is_code_at(del, 0x38f830UL, "rbp"), "a deleted rbp is still named rbp");

    /* A pathname with no directory part at all: not a maps line we understand, so
     * it fails closed rather than matching on a guess. */
    CHECK(!na_maps_line_is_code_at(nodir, 0x18000UL, "rbp"),
          "a pathname with no directory part must not match");

    /* Everything malformed fails closed rather than matching on a guess. */
    CHECK(!na_maps_line_is_code_at("", 0x38f830UL, "rbp"), "an empty line must not match");
    CHECK(!na_maps_line_is_code_at(NULL, 0x38f830UL, "rbp"), "a NULL line must not match");
    CHECK(!na_maps_line_is_code_at(rx, 0x38f830UL, NULL), "a NULL name must not match");
    CHECK(!na_maps_line_is_code_at(rx, 0x38f830UL, ""), "an empty name must not match");
    CHECK(!na_maps_line_is_code_at("not a maps line at all\n", 0x38f830UL, "rbp"),
          "a line that is not a mapping must not match");
    CHECK(!na_maps_line_is_code_at("002d1000-0050b000 r-xp\n", 0x38f830UL, "rbp"),
          "a truncated line must not match");
}

int main(void)
{
    test_constants();
    test_whitelist();
    test_name_match();
    test_selection();
    test_system();
    test_connect();
    test_player_test();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
