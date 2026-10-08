/*
 * netalias.c — the decisions netshim.so makes, with nothing in them that needs
 * a device. See netalias.h for why the module exists and what each rule refuses
 * to do.
 *
 * Nothing here calls a syscall, reads a clock, or knows an address of rbp's, and
 * that is load-bearing: it is what lets test_netalias.c run the whole policy on
 * the host, statically, with no rootfs and no interface in sight.
 */
#include "netalias.h"

#include <stdio.h>
#include <string.h>

/* --- the ioctl whitelist ------------------------------------------------- */

int na_ioctl_whitelisted(unsigned long request)
{
    /* Exact equality, never a mask. Every one of these is a plain 0x89xx number
     * with the size and direction fields of the ioctl encoding empty, so the
     * full request and its low half are the same value — and an exact compare
     * means a caller passing some _IOR-wrapped variant we have never seen gets
     * a pass-through rather than a rewrite. Passing through is always safe. */
    switch (request) {
    case NA_SIOCGIFINDEX:
    case NA_SIOCGIFADDR:
    case NA_SIOCGIFNETMASK:
    case NA_SIOCGIFHWADDR:
    case NA_SIOCGIFFLAGS:
    case NA_SIOCGIFBRDADDR:
    case NA_SIOCGIFDSTADDR:
    case NA_SIOCGIFMETRIC:
    case NA_SIOCGIFMTU:
    case NA_SIOCGIFTXQLEN:
        return 1;
    default:
        return 0;
    }
}

int na_name_rewrite(const char *name, const char *alias)
{
    if (!name)
        return 0;
    if (!alias || !*alias)
        return 0;
    /* An alias equal to the name is an identity that has already been decided;
     * saying "no" here keeps the caller's swap-and-restore from being a no-op
     * that still has to be undone. */
    if (strcmp(alias, NA_FROM) == 0)
        return 0;

    /* Bounded, and stopping at the field's NUL, which is how the kernel reads
     * ifr_name: "eth0" followed by anything, or by the field's zero padding,
     * both name eth0. "eth0x" does not. */
    return strncmp(name, NA_FROM, NA_NAME_MAX) == 0;
}

int na_ioctl_rewrite(unsigned long request, const char *name, const char *alias)
{
    if (!na_ioctl_whitelisted(request))
        return 0;
    return na_name_rewrite(name, alias);
}

/* --- interface selection ------------------------------------------------- */

int na_iface_present(const struct na_iface *ifs, int n, const char *name)
{
    int i;

    if (!ifs || n <= 0 || !name || !*name)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(ifs[i].name, name) == 0)
            return 1;
    return 0;
}

/* Copy only if the WHOLE name fits. A truncated interface name is a name that
 * does not exist, and rewriting rbp's queries to one that does not exist is the
 * single outcome this module must never produce — so a name that will not fit
 * is refused rather than shortened. */
static int copy_name(char *out, int out_max, const char *name)
{
    int k = 0;

    if (out_max <= 0)
        return 0;
    while (name[k] && k < out_max - 1) {
        out[k] = name[k];
        k++;
    }
    if (name[k]) {
        out[0] = '\0';
        return 0;
    }
    out[k] = '\0';
    return k > 0;
}

/* Rule 3's test: a live interface that is not the one rbp names and that
 * actually holds an address to answer with. */
static int usable(const struct na_iface *f)
{
    return f->name[0] != '\0'
        && (f->flags & (NA_IFF_UP | NA_IFF_RUNNING)) == (NA_IFF_UP | NA_IFF_RUNNING)
        && !(f->flags & NA_IFF_LOOPBACK)
        && f->has_v4
        && strcmp(f->name, NA_FROM) != 0;
}

/* `wl`-prefixed. Linux's wireless naming convention (wlan0, wlp3s0, wlx...);
 * the preference exists so a unit with both a spare wired port and the live
 * wireless link picks the one that is actually carrying the LAN. */
static int wireless(const struct na_iface *f)
{
    return f->name[0] == 'w' && f->name[1] == 'l';
}

int na_pick_alias(const struct na_iface *ifs, int n, const char *want,
                  char *out, int out_max)
{
    int i, pass;

    if (out && out_max > 0)
        out[0] = '\0';
    if (!out || out_max <= 0 || !ifs || n <= 0)
        return 0;

    /* 1. An explicit choice, if it is there. Note that a choice naming an
     * interface that is ABSENT falls through to the measurements below rather
     * than to identity — the caller logs the miss either way, and answering
     * identity would turn a typo into a feature that silently does nothing. */
    if (want && *want && na_iface_present(ifs, n, want))
        return copy_name(out, out_max, want);

    /* 2. rbp's own interface is live: stand down entirely. */
    for (i = 0; i < n; i++)
        if (strcmp(ifs[i].name, NA_FROM) == 0
            && (ifs[i].flags & (NA_IFF_UP | NA_IFF_RUNNING))
                   == (NA_IFF_UP | NA_IFF_RUNNING))
            return 0;

    /* 3. Two passes so a wireless name wins without a second list. */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < n; i++)
            if (usable(&ifs[i]) && (pass == 1 || wireless(&ifs[i])))
                return copy_name(out, out_max, ifs[i].name);

    /* 4. Nothing usable. */
    return 0;
}

/* --- the system hook ----------------------------------------------------- */

enum na_sys_verdict na_system_verdict(const char *cmd, const char *alias,
                                      char *out, int out_max)
{
    const char *hit;
    int pre, tail, alen, need;

    if (out && out_max > 0)
        out[0] = '\0';
    if (!cmd || !*cmd)
        return NA_SYS_PASS;
    if (!alias || !*alias || strcmp(alias, NA_FROM) == 0)
        return NA_SYS_PASS;

    /* The DHCP refusal comes first, before anything is considered rewritable.
     * See netalias.h: a lease solicited on wlan0 is the one class of change
     * this module may not make. */
    if (strstr(cmd, "udhcpc"))
        return NA_SYS_REFUSE;

    /* The awk scrape. BOTH halves are required — the reader tool and the filter
     * it is piped into — so a command that merely contains the pattern is left
     * alone rather than edited on a guess. */
    if (!strstr(cmd, "ifconfig"))
        return NA_SYS_PASS;
    hit = strstr(cmd, "/^eth/");
    if (!hit)
        return NA_SYS_PASS;

    /* Nowhere to put the result, or a command too long for the buffer: PASS,
     * because a truncated command line is a command that runs with arguments
     * missing, which is worse than one that reports the wrong interface. */
    if (!out || out_max <= 0)
        return NA_SYS_PASS;
    pre  = (int)(hit - cmd);
    tail = (int)strlen(hit + 6);          /* past the literal "/^eth/" */
    alen = (int)strlen(alias);
    need = pre + 2 + alen + 1 + tail + 1; /* prefix / ^ alias / tail NUL */
    if (need > out_max)
        return NA_SYS_PASS;

    memcpy(out, cmd, (size_t)pre);
    out[pre] = '/';
    out[pre + 1] = '^';
    memcpy(out + pre + 2, alias, (size_t)alen);
    out[pre + 2 + alen] = '/';
    memcpy(out + pre + 2 + alen + 1, hit + 6, (size_t)tail + 1); /* keeps the NUL */

    return NA_SYS_REWRITE;
}

const char *na_sys_verdict_name(enum na_sys_verdict v)
{
    switch (v) {
    case NA_SYS_PASS:    return "pass";
    case NA_SYS_REWRITE: return "rewrite";
    case NA_SYS_REFUSE:  return "refuse";
    }
    return "?";
}

/* --- "is this process the player?" ---------------------------------------- */

int na_maps_line_is_code_at(const char *line, unsigned long addr, const char *want)
{
    unsigned long start, end;
    char perms[8];
    const char *path, *base;
    size_t n;

    if (!line || !want || !*want)
        return 0;
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
        return 0;
    if (addr < start || addr >= end)
        return 0;
    /* The third character of the permission field is the execute bit; a perms
     * field too short to have one reads its NUL and answers no. */
    if (perms[2] != 'x')
        return 0;

    /* No field before the pathname may contain a '/', so the first one on the
     * line begins it -- and the LAST one separates the name we compare. */
    path = strchr(line, '/');
    if (!path)
        return 0;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    /* Stop at the newline, and at the space before a " (deleted)" suffix: neither
     * belongs to the name, and a deleted player's mapping is not one to jump
     * into anyway -- but comparing "rbp (deleted)" against "rbp" would answer no
     * for the wrong reason, and the reason is what a drill log has to carry. */
    n = strcspn(base, " \t\r\n");

    return strlen(want) == n && strncmp(base, want, n) == 0;
}

/* --- the one-shot Link bring-up ------------------------------------------ */

int na_connect_gate(unsigned int singleton, unsigned int ip)
{
    /* Order matters for the log, not for the answer: the singleton is checked
     * first so that "no-ip" can only ever be said about an object that exists,
     * and a reader is never left wondering which of the two it was. */
    if (singleton == 0)
        return NA_CONNECT_NO_SINGLETON;
    if (ip == 0)
        return NA_CONNECT_NO_IP;
    return NA_CONNECT_GO;
}

const char *na_connect_verdict_name(enum na_connect_verdict v)
{
    switch (v) {
    case NA_CONNECT_GO:           return "go";
    case NA_CONNECT_NO_SINGLETON: return "no-singleton";
    case NA_CONNECT_NO_IP:        return "no-ip";
    }
    return "?";
}
