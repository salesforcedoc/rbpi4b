/*
 * netalias.h — which `eth0` to answer for, and what to do about it, as pure
 * functions.
 *
 * rbp's whole Pro DJ Link stack asks the kernel about ONE interface by name:
 * the literal "eth0" (string VA 0x3FE678, referenced from exactly twelve
 * functions — see docs/18-prodjlink.md). On this unit eth0 is DOWN with no
 * carrier and wlan0 is the live link, so the stack comes up, finds nothing, and
 * stays silent. The cure is a substitution at the three libc calls those twelve
 * functions reach the kernel through (ioctl, if_nametoindex, system), and this
 * file holds every decision that substitution makes.
 *
 * The substitution is NECESSARY and NOT SUFFICIENT: it makes rbp read an address,
 * but rbp still will not open UDP 50000 until its own USB-attach gate is set. See
 * the "one-shot Link bring-up" block far below for that, and for the correction to
 * an earlier comment here that got it backwards.
 *
 * It is here rather than inside netshim.c for the reason mirror_policy.c is
 * beside audioshim.c: netshim.c cannot be linked into a test at all (its
 * constructor dlsyms and it interposes the process's ioctls), so any rule left
 * inside it is a rule only a Pi drill can check. What is here is the arithmetic
 * and the tables, with no syscall, no kernel header, no clock and no address of
 * rbp's — so test_netalias.c pins the verdicts on the host, with no interface,
 * no rootfs and no rbp.
 *
 * The two verdicts that matter most, because either one wrong is a unit the
 * operator cannot reach:
 *
 *   SIOCSIFADDR and every other SIOCSIF* -> NEVER rewritten. One of the twelve
 *   functions is setIpAddr @0x39dfb4, and an address assignment retargeted at
 *   wlan0 would write rbp's idea of an address — plausibly a 169.254.x.x
 *   link-local when network::Autoip runs — onto the interface carrying the
 *   operator's own SSH session and LAN. The whitelist below is read-only
 *   requests only, so a setter reaches the kernel unchanged and fails
 *   harmlessly on the carrierless interface.
 *
 *   no alias active -> every hook passes through. Rewriting "eth0" to an
 *   interface that does not exist would make rbp STRICTLY worse off, so the
 *   identity answer is the only one that is always correct and it is what every
 *   fallback returns.
 */
#ifndef RBPI4B_NETALIAS_H
#define RBPI4B_NETALIAS_H

/* The interface name rbp asks about. Not a setting: this is the literal baked
 * into the player, and a build that asked about something else would not need
 * this module at all. */
#define NA_FROM "eth0"

/* Linux's IFNAMSIZ. The name field of an ifreq, and the whole of what we touch. */
#define NA_NAME_MAX 16

/* The longest `system` command line this module will rewrite. Longer than any
 * command rbp builds (the two measured are 31 and 40 characters); a longer one
 * is left alone rather than truncated, because a truncated command line is a
 * command that runs with arguments missing. */
#define NA_CMD_MAX 512

/* -------------------------------------------------------------------------
 * Linux's ioctl request numbers, written out rather than included.
 *
 * The values are from <linux/sockios.h> and are identical on every Linux
 * architecture (they carry no _IOR/_IOW encoding — they predate it), but the
 * point of writing them here is that the module compiles with no kernel header
 * at all. test_netalias.c includes the real headers on the host and asserts
 * every constant below matches, so a value that ever drifted is a red test
 * rather than a silently inert shim.
 *
 * WHICH ARE WHITELISTED, and why it is a whitelist:
 *
 *   SIOCGIFINDEX     the ONE measured need — ui::MacAddressListener::run
 *                    @0x340de0 gets an index here and binds a raw
 *                    AF_PACKET/ETH_P_ALL socket to it
 *   SIOCGIFADDR      get_ipaddr, get_ip_inf, get_myip_info, getCurrentIpAddress
 *   SIOCGIFNETMASK   get_subnet_inf
 *   SIOCGIFHWADDR    getMacAddr, getMacAddressUint8, PcControlMacAddress
 *   SIOCGIFFLAGS     not measured, but the obvious companion: a "is eth0 up?"
 *                    test that answered "no" would veto everything else, so
 *                    leaving it out would be a shim that reports the right
 *                    address to a caller that has already given up
 *   SIOCGIFBRDADDR   the subnet mask's partner, asked wherever a mask is
 *   SIOCGIFDSTADDR   the remaining read-only ifreq requests. Harmless: the
 *   SIOCGIFMETRIC    kernel answers about the substituted interface and rbp
 *   SIOCGIFMTU       reads an MTU/metric/queue length it has no opinion about
 *   SIOCGIFTXQLEN
 *
 * DELIBERATELY NOT WHITELISTED, and each for its own reason:
 *
 *   SIOCGIFCONF      ifr_name is an OUTPUT buffer here. The kernel WRITES the
 *                    interface list into it; there is no name to substitute, and
 *                    reading one would be reading uninitialised stack.
 *   SIOCGIFNAME      the opposite direction — an index in, a name out. It has
 *                    no ifr_name to match on, and its answer is the kernel's.
 *   every SIOCSIF*   the setters. See the header comment above; this is the
 *                    one verdict in this file that can cost the operator access
 *                    to the unit.
 *   everything else  not an ifreq. arg must not be dereferenced at all.
 * ------------------------------------------------------------------------- */
#define NA_SIOCGIFNAME     0x8910
#define NA_SIOCGIFCONF     0x8912
#define NA_SIOCGIFFLAGS    0x8913
#define NA_SIOCGIFADDR     0x8915
#define NA_SIOCGIFDSTADDR  0x8917
#define NA_SIOCGIFBRDADDR  0x8919
#define NA_SIOCGIFNETMASK  0x891b
#define NA_SIOCGIFMETRIC   0x891d
#define NA_SIOCGIFMTU      0x8921
#define NA_SIOCGIFHWADDR   0x8927
#define NA_SIOCGIFINDEX    0x8933
#define NA_SIOCGIFTXQLEN   0x8942

/* Linux's `struct ifreq` on a 32-bit target: a 16-byte name then a 16-byte
 * union. Only the name half is defined, because only the name half is ours —
 * every whitelisted request WRITES the union and this shim never reads it.
 * test_netalias.c asserts this matches the host's own struct ifreq. */
struct na_ifreq {
    char name[NA_NAME_MAX];
    unsigned char arg[16];
};

/* Is `request` one of the read-only, name-in requests above? */
int na_ioctl_whitelisted(unsigned long request);

/* Is this interface name one we substitute for? Yes when `name` is exactly
 * NA_FROM (as a bounded 16-byte field, stopping at its NUL, which is how the
 * kernel reads ifr_name too) and `alias` is a non-empty name that differs from
 * NA_FROM. Shared by the ioctl and if_nametoindex hooks, which ask the same
 * question about the same kind of argument. */
int na_name_rewrite(const char *name, const char *alias);

/* Should this ioctl's ifreq name be substituted? Yes only when the request is
 * whitelisted as well — see na_ioctl_whitelisted(). Anything else answers no,
 * and no is always safe: it means the call is made exactly as rbp made it. */
int na_ioctl_rewrite(unsigned long request, const char *name, const char *alias);

/* -------------------------------------------------------------------------
 * Interface selection.
 *
 * netshim.c does the one getifaddrs() call and hands the result here as plain
 * facts; the policy is this function. The interface flags are the module's own
 * so that no kernel header is needed — test_netalias.c asserts they match
 * <net/if.h>.
 *
 * These are the KERNEL's bits and they are not adjacent, which is worth saying
 * because guessing them is easy and this file did: IFF_UP is 1<<0, IFF_LOOPBACK
 * is 1<<3 (IFF_DEBUG and IFF_BROADCAST sit between), and IFF_RUNNING is 1<<6.
 * The test caught exactly that, which is why the test asserts them rather than
 * trusting the comment.
 * ------------------------------------------------------------------------- */
#define NA_IFF_UP       0x1
#define NA_IFF_RUNNING  0x40
#define NA_IFF_LOOPBACK 0x8

struct na_iface {
    char name[NA_NAME_MAX];
    unsigned flags;
    int has_v4;   /* holds an IPv4 address right now */
};

/* Choose the name to answer NA_FROM with. Fills `out` (a NA_NAME_MAX buffer)
 * and returns 1 when an alias is chosen, or returns 0 for IDENTITY — leave
 * every hook passing through.
 *
 * The order, and each rule is a decision:
 *
 *   1. `want` (RB_NETALIAS_IFACE) names an interface that is present -> use it.
 *      An explicit operator choice beats every measurement below, including the
 *      eth0 rule: someone naming an interface has a reason this function cannot
 *      see.
 *   2. NA_FROM is present and UP|RUNNING -> IDENTITY. If ethernet is ever
 *      plugged in, rbp works natively and this shim must get out of its way.
 *      Note this is deliberately flags-only and does not require an address: the
 *      moment between a cable going in and DHCP answering is exactly when a
 *      shim that had taken over would be fighting the link rbp is about to use.
 *   3. the first UP|RUNNING, non-loopback, non-NA_FROM interface that holds an
 *      IPv4 address, preferring a wireless name — two passes, wl* then anything.
 *   4. nothing -> IDENTITY.
 *
 * `n` is the number of entries in `ifs`; a NULL or empty array is rule 4.
 */
int na_pick_alias(const struct na_iface *ifs, int n, const char *want,
                  char *out, int out_max);

/* A name that is present in the list, whatever its state. Rule 1's test, split
 * out because netshim.c wants to say in its log that an explicit choice named
 * nothing. */
int na_iface_present(const struct na_iface *ifs, int n, const char *name);

/* -------------------------------------------------------------------------
 * The `system` hook.
 *
 * rbp reaches two shell command lines through juce_runSystemCommand:
 * `udhcpc -i eth0 -T 2 -t 3 -n -q` (network::Autoip) and
 * `ifconfig | awk '$1 ~ /^eth/ {print $NF}'` (ui::MacAddressHolder).
 *
 * This hook SHIPS AS AN OBSERVING PASS-THROUGH. netshim.c logs each distinct
 * command line and runs it unchanged; the verdicts below are applied only when
 * the operator turns RB_NETALIAS_MAC_SCRAPE on. The reason is that neither
 * command needs to be touched for Link to work (the MAC comes from
 * SIOCGIFHWADDR, which the ioctl hook already answers), so the default is the
 * one with no behaviour change at all.
 * ------------------------------------------------------------------------- */
enum na_sys_verdict {
    NA_SYS_PASS = 0,  /* run it exactly as given */
    NA_SYS_REWRITE,   /* run `out` instead — see below */
    NA_SYS_REFUSE     /* do not run it at all */
};

/* Decide what to do with a command line, with `alias` active (NULL or "" means
 * no alias, which is NA_SYS_PASS for everything).
 *
 *   any command containing "udhcpc"                        -> REFUSE
 *   a command containing BOTH "ifconfig" and "/^eth/"      -> REWRITE into `out`
 *   anything else                                          -> PASS
 *
 * The DHCP rule is deliberately broader than the one command line measured. The
 * command that matters is `udhcpc -i eth0 ...`, and the reason it may not be
 * rewritten is that a DHCP client started on wlan0 would solicit a NEW LEASE on
 * the interface carrying the operator's live WiFi — the one class of change this
 * whole module exists to avoid. Refusing every udhcpc is the same protection
 * with no pattern to get wrong, and costs nothing: rbp only ever builds it for
 * eth0, and on this unit eth0 has no carrier to lease from anyway. REFUSE must
 * be reported to the caller as `127 << 8` (the shell's "command not found"),
 * with errno untouched.
 *
 * The awk rule is the opposite shape: that command only READS, and its output
 * goes to a file JUCE opens afterwards, so passing it through with the filter
 * still matching /^eth/ would silently report eth0's (absent) address. The only
 * correct non-pass verdict is an in-place rewrite of the filter, `out` keeps the
 * redirect and everything else verbatim. Only the literal "/^eth/" is replaced —
 * this is a substitution, not a shell parser, and a command that filters some
 * other way is left alone rather than guessed at. Both halves are required, so
 * a command that merely mentions the pattern is never edited on a guess.
 *
 * `out` must hold NA_CMD_MAX bytes; a command longer than that is PASSed
 * unchanged rather than truncated.
 */
enum na_sys_verdict na_system_verdict(const char *cmd, const char *alias,
                                      char *out, int out_max);

/* For the log line. Never NULL. */
const char *na_sys_verdict_name(enum na_sys_verdict v);

/* -------------------------------------------------------------------------
 * "Is this process the player?"
 *
 * rbp puts LD_PRELOAD in its OWN environment, so every process it starts through
 * `system()` inherits it and loads netshim.so too — measured on the unit, a shell
 * the player spawned logged a terminal-size ioctl (0x5413) through the shim. In
 * the player, NM_SINGLETON sits inside its own .bss and NM_FN_CONNECT inside its
 * own .text; in a spawned shell the first is that shell's heap or nothing at all
 * and the second is unmapped. Reading the first is a segfault and calling the
 * second is a jump into whatever lives there, so the Link bring-up must refuse to
 * run anywhere but in the player.
 *
 * This decides one line of /proc/self/maps. A line describes one mapping:
 *
 *   002d1000-0050b000 r-xp 002c9000 b3:02 381935   /opt/rblive4/…/root/pdj/rbp
 *
 * and the answer is yes only when `addr` falls inside it, the mapping carries
 * the EXECUTE bit, and the file backing it is named `want`. That is exactly the
 * property the call needs — "this address is code belonging to that file" —
 * rather than a proxy for it. It fails CLOSED on every malformed input: no
 * parse, no address in range, no pathname, a name of the wrong length. The cost
 * of a wrong "no" is a drill that does not happen and says so; the cost of a
 * wrong "yes" is a jump into an unmapped address.
 */
int na_maps_line_is_code_at(const char *line, unsigned long addr, const char *want);

/* -------------------------------------------------------------------------
 * The one-shot Link bring-up.
 *
 * Answering the twelve eth0 queries gets rbp's NetworkMonitor as far as an
 * address, and stops there — but NOT because the call is unreachable. An earlier
 * version of this comment said NetworkManager::operateConnectNetwork @0x38f830
 * "HAS NO CALLER anywhere in the binary". That was read off a scan for a `bl` to
 * that address, and it is false: the function is VIRTUAL, so rbp reaches it with
 * `blx` through NetworkMonitor's vtable slot +12, from NetworkMonitor::
 * timerCallback @0x392160 — which main's NetworkManager::initialize() starts
 * unconditionally, once a second. What the timer tests first is
 * ui::PcController::isUsbBConnected() (PcController+0x72), and while that is zero
 * no connect is ever attempted however well the substitution works. rbp raises
 * that flag itself, from the word `connect` on its own /tmp/udev_usb1 FIFO — the
 * in-band route, and the one to prefer (docs/18-prodjlink.md). This shim's direct
 * call is the out-of-band alternative: it reaches the function without an
 * attached-"PC" state, and its gate below is what keeps it honest.
 *
 * So the bring-up is a direct call into rbp, and this is its GATE. It is pure
 * because what it decides is exactly what the drill log has to explain when the
 * call does nothing:
 *
 *   singleton == 0   rbp's NetworkManager does not exist yet — main has not
 *                    reached getInstance(). There is nothing to call; retry.
 *   ip == 0          NetworkMonitor holds no address, because it asked eth0 and
 *                    the substitution did not take (or eth0 really is the only
 *                    interface). operateConnectNetwork returns at its first
 *                    compare, so firing here would be a SILENT no-op — the
 *                    failure mode this whole module exists to avoid.
 *   otherwise        GO.
 *
 * There is deliberately no third input. rbp's [+0xc4] — the bool main passed to
 * initialize(), 0 with -a — looks like a precondition and is not one: it selects
 * whether the Autoip object at [+0xc0] exists, and operateConnectNetwork reads
 * [+0xc0] only on the branch [+0xc4] guards. Whichever way it reads, the call is
 * safe; taking it as an argument here would only invite the reader to think it
 * had to be a particular value. The shim logs it instead.
 * ------------------------------------------------------------------------- */
enum na_connect_verdict {
    NA_CONNECT_GO = 0,
    NA_CONNECT_NO_SINGLETON,
    NA_CONNECT_NO_IP
};

int na_connect_gate(unsigned int singleton, unsigned int ip);

/* For the log line. Never NULL. */
const char *na_connect_verdict_name(enum na_connect_verdict v);

#endif /* RBPI4B_NETALIAS_H */
