/*
 * netcheck.c -- netshim.so's drill: does the alias actually do anything?
 *
 * A shim whose failure mode is "silently rewrites nothing" cannot be verified by
 * looking at it. This asks the questions netshim interposes, prints what came
 * back, and prints the CALLER's struct afterwards -- which is the half a
 * rewrite most easily gets wrong, because putting the substituted name back is a
 * separate step from substituting it.
 *
 * The interface to alias is passed in RB_NETALIAS_IFACE (envutil's contract
 * applies only to the shim; this reads NETALIAS_IFACE, which is what start-rb.sh
 * exports). Point it at an interface that exists and whose address differs from
 * eth0's -- `lo` is the portable choice -- and every line below changes.
 *
 * NOT in TESTS, and it must NOT be -static: LD_PRELOAD needs a dynamic loader.
 * Run it in the chroot, or under `qemu-arm -L <rootfs>`, where this rootfs's
 * loader and soft-float ABI apply -- the same reason ccexit is built this way.
 *
 *   NETALIAS=1 NETALIAS_IFACE=lo NETALIAS_LOG=1 \
 *       LD_PRELOAD=/usr/lib/netshim.so ./netcheck
 *
 * What a PASS looks like: eth0 comes back with the ALIAS's answers and the drill
 * says so (lo's 127.0.0.1), every query still reports its own name unchanged, a
 * name that is neither eth0 nor the alias is untouched AND still fails, and the
 * setter is passed through rather than aimed at the alias. The two negative
 * cases are what make a "pass" mean something: without them a shim that rewrote
 * every name it was given would pass too.
 *
 * Run it twice. Once without the shim is the baseline, and the eth0 line moving
 * between the two runs is the whole result.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fd;

/* The address as the kernel wrote it into the union, as four bytes -- printed by
 * hand rather than through inet_ntoa, because this is a diagnostic and it should
 * not be able to fail for a reason of its own.
 *
 * `expect` is what the caller believes should happen, and both directions are
 * checked. The negative cases are not decoration: a rewrite that substituted too
 * eagerly would make `nosuch0` succeed, and the ONLY thing that catches that is
 * asserting it fails. Saying `expect = 0` is how this drill states it -- treating
 * an expected failure as a failure is the bug this signature exists to prevent. */
static void addr_of(const char *name, int expect, int *ok, unsigned char out[4])
{
    struct ifreq ifr;
    int rc;

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
    errno = 0;
    rc = ioctl(fd, SIOCGIFADDR, &ifr);

    if (rc < 0) {
        if (out)
            memset(out, 0, 4);
        printf("  SIOCGIFADDR   %-8s -> FAILED errno=%d (%s)%s\n", name, errno,
               strerror(errno), expect ? "   <-- SHOULD HAVE SUCCEEDED" : "");
        if (expect)
            *ok = 0;
        return;
    }
    if (!expect) {
        if (out)
            memset(out, 0, 4);
        printf("  SIOCGIFADDR   %-8s -> SUCCEEDED   <-- SHOULD HAVE FAILED (a name "
               "was rewritten that is not eth0)\n", name);
        *ok = 0;
        return;
    }
    {
        const unsigned char *b =
            (const unsigned char *)&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
        printf("  SIOCGIFADDR   %-8s -> %u.%u.%u.%u   name field now \"%s\"%s\n",
               name, b[0], b[1], b[2], b[3], ifr.ifr_name,
               strcmp(ifr.ifr_name, name) == 0 ? "" : "   <-- NOT RESTORED");
        if (out)
            memcpy(out, b, 4);
        if (strcmp(ifr.ifr_name, name) != 0)
            *ok = 0;
    }
}

static void index_of(const char *name, int expect, int *ok)
{
    unsigned int i = if_nametoindex(name);

    printf("  if_nametoindex %-8s -> %u%s\n", name, i,
           expect ? (i == 0 ? "   <-- SHOULD HAVE RESOLVED" : "")
                  : (i != 0 ? "   <-- SHOULD HAVE BEEN 0" : ""));
    if (expect ? (i == 0) : (i != 0))
        *ok = 0;
}

/* When NETALIAS_IFACE is unset the shim chooses for itself, and its armed line is
 * where it says so. Reading that back is not a shortcut: it is the only way this
 * drill can test the AUTO path, which is what a default deploy uses. Set
 * NETALIAS_IFACE and the explicit path is tested instead, so both are reachable. */
#define NETCHECK_LOG "/tmp/netshim.log"

static int alias_from_log(char *out, int out_max)
{
    char line[512];
    FILE *f = fopen(NETCHECK_LOG, "r");

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, " alias=");
        int i = 0;
        if (!p)
            continue;
        p += 7;
        while (p[i] && p[i] != ' ' && p[i] != '\n' && p[i] != '\r' &&
               i < out_max - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        fclose(f);
        return i > 0;
    }
    fclose(f);
    return 0;
}

int main(void)
{
    const char *alias = getenv("NETALIAS_IFACE");
    char chosen[64];
    struct ifreq ifr;
    unsigned char eth[4], want[4];
    int ok = 1;

    if (!alias || !*alias) {
        if (alias_from_log(chosen, sizeof chosen)) {
            alias = chosen;
            printf("netcheck: alias read back from %s (auto-selection)\n",
                   NETCHECK_LOG);
        }
    }

    printf("netcheck: NETALIAS=%s NETALIAS_IFACE=%s NETALIAS_LOG=%s\n",
           getenv("NETALIAS") ? getenv("NETALIAS") : "(unset)",
           getenv("NETALIAS_IFACE") ? getenv("NETALIAS_IFACE") : "(unset)",
           getenv("NETALIAS_LOG") ? getenv("NETALIAS_LOG") : "(unset)");

    if (!alias || !*alias) {
        printf("  neither NETALIAS_IFACE nor an armed line in %s -- set the env\n"
               "  var to a live interface (e.g. lo) and re-run\n", NETCHECK_LOG);
        return 2;
    }
    if (strcmp(alias, "eth0") == 0) {
        printf("  the alias is eth0, which is the identity case -- every line below\n"
               "  would pass without the shim doing anything, so it proves nothing.\n"
               "  Point NETALIAS_IFACE at another interface (lo is the portable one).\n");
        return 2;
    }
    printf("  the alias is \"%s\", so eth0 must come back with ITS answers\n\n",
           alias);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 2;
    }

    /* The substitution, and the restore. */
    addr_of("eth0", 1, &ok, eth);
    /* The alias asked for by its own name: a rewrite must not touch it, and this
     * is the control that says the line above is about the substitution. */
    addr_of(alias, 1, &ok, want);
    /* A name that is neither. Must reach the kernel untouched and fail. */
    addr_of("nosuch0", 0, &ok, NULL);

    index_of("eth0", 1, &ok);
    index_of("nosuch0", 0, &ok);

    /* The claim in one line: the answer for eth0 is the ALIAS's answer. Printing
     * them adjacent invites reading it as "two addresses"; this says it. */
    if (memcmp(eth, want, 4) == 0) {
        printf("\n  eth0's answer IS %s's answer -- the substitution is real\n",
               alias);
    } else {
        printf("\n  eth0 answered %u.%u.%u.%u but %s answered %u.%u.%u.%u\n"
               "        <-- THE REWRITE DID NOT HAPPEN (or was not delegated)\n",
               eth[0], eth[1], eth[2], eth[3], alias, want[0], want[1], want[2],
               want[3]);
        ok = 0;
    }

    /* A SETTER. This is the one that must never be retargeted: aimed at the
     * alias it would write onto a live interface. Here the call fails for lack
     * of privilege, which is the point -- and what is checked is that the name
     * the caller passed is still its own, i.e. nothing was substituted. */
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "eth0");
    errno = 0;
    printf("\n  SIOCSIFADDR   eth0     -> %s errno=%d (%s)   name field now \"%s\"\n",
           ioctl(fd, SIOCSIFADDR, &ifr) < 0 ? "refused" : "ACCEPTED", errno,
           strerror(errno), ifr.ifr_name);
    if (strcmp(ifr.ifr_name, "eth0") != 0) {
        printf("        <-- A SETTER WAS RETARGETED. This is the failure the\n"
               "            whitelist exists to prevent.\n");
        ok = 0;
    }

    /* The system hook, observing. */
    printf("\n");
    fflush(stdout);
    printf("  system()      -> %d\n", system("echo netcheck-system-ran"));

    printf("\n%s\n", ok ? "netcheck: ok" : "netcheck: FAILURES above");
    close(fd);
    return ok ? 0 : 1;
}
