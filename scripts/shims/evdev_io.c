/*
 * evdev_io.c -- see evdev_io.h.
 *
 * Layout: discovery (every /dev/input/event* with EV_KEY) -> one poll() loop
 * over all of them -> the handler. The reader thread wraps discovery in a retry
 * loop, so "nothing plugged in yet" and "a device was unplugged" are the same
 * state and a replug needs no special case -- the same shape fbshim.so's
 * pointsrc.c uses for the pointer.
 *
 * THREE RULES ABOUT THAT LOOP, and they are the whole of it. The first two are
 * fixes and the third is what makes them hold under a device that never stops
 * talking (docs/16-input-and-hotplug.md):
 *
 *  - NO LIVE fd IS EVER CLOSED ON A RESCAN. A device is opened once and left
 *    open; one that goes away is closed on its own. The loop used to run its
 *    discovery at the top of every iteration and close the whole set first, so
 *    every poll round closed and reopened every node -- and evdev queues events
 *    per open client, so a release that landed in that window was discarded.
 *    map_kbd.c's held-key latch then stayed set and the next press of that key
 *    was ignored: a human press is ~100 ms and was reliably lost.
 *  - A KEY HELD WHEN A DEVICE GOES AWAY IS RELEASED HERE. The kernel never sends
 *    a release for a device that is no longer there, so drop_dead() synthesises
 *    one for every key it still believes is down. That reuses the map's own
 *    tested release path, and costs this file one bit set per device.
 *  - THE DEADLINE IS ABSOLUTE, NOT A COUNTDOWN. "Has a device appeared?" is
 *    asked once a second against the clock, so a chatty mouse cannot postpone
 *    it. The old code re-armed a full second on every round, which meant a hot
 *    plug was noticed only after a quiet second -- and a 1000 Hz mouse made that
 *    never.
 *
 * Everything here is `static` except evdev_start(): the objects in this shim are
 * compiled -fvisibility=hidden, and nothing in this file belongs in the dynamic
 * symbol table (see the Makefile's note on knobshim.so's 17 exports).
 */
#define _GNU_SOURCE
#include "evdev_io.h"
#include "shimutil.h"     /* klog, env_text, shim_now_ms */
#include "syscalls.h"     /* real_open/read/ioctl/poll/close */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* --- the kernel's input ABI, declared here rather than taken from
 * <linux/input.h> ------------------------------------------------------------
 *
 * The same reasoning as pointsrc.c's copy: the shims are cross-compiled against
 * headers of one vintage and run against a kernel of another, and
 * <linux/input.h> already selects a different struct input_event between its
 * __USE_TIME_BITS64 branches. These two definitions are the small, stable
 * subset this module needs, so it does not care which branch the build host's
 * headers take. tools/evdevdump.c is the tool that reads the same events
 * through the real header.
 */
struct evdev_event {
    struct { uint32_t sec, usec; } time;   /* 8 bytes on arm32, either branch */
    uint16_t type, code;
    int32_t  value;
};

/* A wrong size here is a silent failure rather than a loud one: read() would
 * return short on every event and the loop below would spin without ever
 * dispatching one. Pin it. */
_Static_assert(sizeof(struct evdev_event) == 16,
               "unexpected struct input_event layout for arm32");

#define EV_KEY 0x01

/* _IOC(_IOC_READ, 'E', nr, size): 'E' is 0x45. EVIOCGBIT(0, len) is the set of
 * event types the device can report, which is the bit set this file filters on.
 * EVIOCGNAME is only here so the log can name what was opened. */
#define EVIOCGNAME(len)     (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | 0x06UL)
#define EVIOCGBIT(ev, len)  (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | (0x20UL + (unsigned long)(ev)))

/* /dev/input/event0..event31, and at most that many devices open at once. A
 * machine with more input devices than this has something wrong with it that a
 * bring-up shim is not the place to fix. */
#define EVDEV_SCAN_MAX 32

#define EVDEV_PATH_MAX 32
#define EVDEV_NAME_MAX 128

/* How long between two "has a device appeared?" checks, measured against the
 * clock rather than counted down per round. See the header. */
#define EVDEV_UNSEEN_MS 1000

/* How long to wait before looking again when NOTHING is open. This is the only
 * sleep left in the reader, and it is where there is nothing to lose: with an
 * empty device table, poll() returns immediately, so without this the thread
 * would spin at 100% CPU opening candidates. That state is reachable now in a
 * way it was not before -- a device can be dropped on its own without the rest
 * of the set being torn down -- which is why the branch using it is not
 * optional. */
#define EVDEV_EMPTY_SLEEP_MS 500

/* How many events to take from one device before giving the others a turn. This
 * is what keeps one chatty device (a mouse at 1000 Hz) from starving the rest;
 * the next poll() round picks up where this one stopped. */
#define EVDEV_DRAIN_MAX 64

/* The reader's own lines carry a millisecond stamp, because the questions asked
 * of them are about latency -- "how long after the plug did it appear?" -- and a
 * log with no clock cannot answer that. shim_now_ms() is monotonic, so the
 * numbers are comparable with each other and not with the wall clock; that is
 * the right pair of properties here and the wrong ones for lining a line up
 * against dmesg, which is why the stamp is this shim's own and says so. */
#define EVLOG(fmt, ...) klog("knobshim2: evdev[%llu] " fmt, shim_now_ms(), ##__VA_ARGS__)

/* EV_KEY codes run to KEY_MAX (0x2ff) in this ABI, so "which keys does this
 * device currently have down" is a 96-byte bit set. Static, like everything else
 * here: 32 devices' worth is 3 KB, and there is no reason to allocate for it. */
#define EVDEV_KEY_MAX 0x2ff
#define EVDEV_HELD_BYTES ((EVDEV_KEY_MAX + 8) / 8)

struct evdev_src {
    int  fd;
    char path[EVDEV_PATH_MAX];
    unsigned char held[EVDEV_HELD_BYTES];
};

static struct evdev_src sources[EVDEV_SCAN_MAX];
static int n_sources;

/* Set once, by evdev_start(), before the thread is created. Read-only after
 * that, so no lock: the handler pointer never changes while the thread runs. */
static void (*g_handler)(int type, int code, int value);

/* --- the held-key bit set -------------------------------------------------- */

static int held_test(const unsigned char *bits, int code)
{
    return code >= 0 && code <= EVDEV_KEY_MAX &&
           ((bits[code >> 3] >> (code & 7)) & 1);
}

static void held_set(unsigned char *bits, int code)
{
    if (code >= 0 && code <= EVDEV_KEY_MAX)
        bits[code >> 3] |= (unsigned char)(1u << (code & 7));
}

static void held_clear(unsigned char *bits, int code)
{
    if (code >= 0 && code <= EVDEV_KEY_MAX)
        bits[code >> 3] &= (unsigned char)~(1u << (code & 7));
}

/* --- discovery ------------------------------------------------------------- */

static int bit_set(const unsigned char *bits, unsigned int bit)
{
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

/* Open one node if it can report key events, else -1.
 *
 * EV_KEY is the whole filter, on purpose. It admits the keyboard and the mouse
 * (BTN_LEFT/BTN_RIGHT are EV_KEY codes) and also things like a Pi's gpio-keys
 * power button; the last group is harmless, because an unmapped code is dropped
 * by the map and nothing here ever writes to a device. */
static int open_source(const char *path, char *name_out, unsigned long name_len)
{
    unsigned char evbits[8];
    int fd;

    fd = real_open(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    memset(evbits, 0, sizeof evbits);
    if (real_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0 ||
        !bit_set(evbits, EV_KEY)) {
        real_close(fd);
        return -1;
    }

    name_out[0] = '\0';
    if (real_ioctl(fd, EVIOCGNAME(name_len - 1), name_out) < 0)
        name_out[0] = '\0';
    name_out[name_len - 1] = '\0';
    return fd;
}

/* One line per device, and one format for both the first pass and a hot plug:
 * "appeared; adding it" is true in both cases, the timestamp says which it was,
 * and a second format for the initial scan would be a branch whose only effect
 * is to make the log harder to grep. */
static void add_source(int fd, const char *path, const char *name)
{
    if (n_sources >= EVDEV_SCAN_MAX) {
        real_close(fd);
        return;
    }
    if (!name || !name[0])
        name = "(unnamed)";
    sources[n_sources].fd = fd;
    snprintf(sources[n_sources].path, sizeof sources[n_sources].path, "%s", path);
    memset(sources[n_sources].held, 0, sizeof sources[n_sources].held);
    n_sources++;
    EVLOG("%s '%s' appeared; adding it\n", path, name);
}

/* Close one device, releasing anything it was holding first.
 *
 * The release is the point. The kernel does not send a release for a key that
 * was down when its device disappeared, so map_kbd.c's held-key latch would stay
 * set and the next press of that key would be ACT_IGNORE -- the key looks dead
 * until rbp restarts. Emitting (EV_KEY, code, 0) here reuses the map's own
 * tested release path, and needs no new interface: the map never has to know
 * which device an event came from.
 *
 * The caveat, stated because it is real: if two devices report the same code,
 * the synthetic release from the dying one releases the survivor's hold too.
 * The keyboard and mouse have disjoint codes, and KBD_DEV pins to one node for a
 * bench, so this does not arise with the devices this port is aimed at -- but it
 * is the reason not to reach for this mechanism to do anything cleverer.
 *
 * The table is compacted so the caller can carry on draining the devices that
 * are still alive, in this round rather than the next. */
static void drop_dead(int idx, const char *why)
{
    struct evdev_src *s = &sources[idx];
    int released = 0;

    for (int c = 0; c <= EVDEV_KEY_MAX; c++) {
        if (!held_test(s->held, c))
            continue;
        held_clear(s->held, c);
        if (g_handler)
            g_handler(EV_KEY, c, 0);
        released++;
    }

    real_close(s->fd);
    EVLOG("%s went away (%s); released %d held key(s), %d device(s) left\n",
          s->path, why, released, n_sources - 1);

    for (int j = idx; j + 1 < n_sources; j++)
        sources[j] = sources[j + 1];
    n_sources--;
}

/* Open every EV_KEY node this reader does not already hold, and KEEP it open.
 * Returns how many were added.
 *
 * This replaces the old unseen_source(), which opened each candidate, closed it
 * again and returned at the first hit -- so a hot plug was noticed one device
 * and one whole rescan later, and the fd it had just proved valid was thrown
 * away and reopened. This walks the whole range and adds every new node in one
 * pass, so a keyboard and a mouse plugged in together both arrive at once.
 *
 * Cheap because it only probes what is NOT already open: a node we hold is left
 * alone (closing one is what loses a queued release), and a node we keep
 * rejecting -- the HDMI CEC interfaces have no EV_KEY -- costs one open and one
 * ioctl per pass. */
static int add_new_sources(const char *pin)
{
    int added = 0;

    if (pin) {
        /* A pinned node has nothing to discover, but it IS re-probed rather than
         * assumed, so a pinned keyboard that was unplugged and replugged is
         * picked up. Nothing can be open that is not the pin: this function is
         * the only place a source is ever added, and on this path it adds one
         * thing. */
        char name[EVDEV_NAME_MAX];
        int fd;

        if (n_sources > 0)
            return 0;
        fd = open_source(pin, name, sizeof name);
        if (fd < 0)
            return 0;
        add_source(fd, pin, name);
        return 1;
    }

    for (int i = 0; i < EVDEV_SCAN_MAX && n_sources < EVDEV_SCAN_MAX; i++) {
        char path[EVDEV_PATH_MAX];
        char name[EVDEV_NAME_MAX];
        int fd, have = 0;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        for (int s = 0; s < n_sources; s++)
            if (strcmp(sources[s].path, path) == 0) {
                have = 1;
                break;
            }
        if (have)
            continue;

        /* Opening it is the test: open_source() rejects anything without
         * EV_KEY, which is the same filter the first scan uses. */
        fd = open_source(path, name, sizeof name);
        if (fd >= 0) {
            add_source(fd, path, name);
            added++;
        }
    }
    return added;
}

/* --- reading --------------------------------------------------------------- */

enum { DRAIN_OK = 0, DRAIN_GONE, DRAIN_ERROR };

/* Take what is readable from one device.
 *
 * DRAIN_OK means it is still usable -- drained, or nothing queued. The other two
 * both mean this fd will not deliver another event, and the caller drops it: a
 * short read or EOF is how an unplug shows up, and anything else that is not
 * EINTR/EAGAIN is a device the kernel has given up on. The distinction is kept
 * because the two want different words in the log. */
static int drain_source(int idx)
{
    struct evdev_event ev;
    int taken = 0;

    while (taken < EVDEV_DRAIN_MAX) {
        ssize_t n = real_read(sources[idx].fd, &ev, sizeof ev);

        if (n == (ssize_t)sizeof ev) {
            /* Track what this device is holding before handing the event on:
             * the map keeps its own per-binding state, but it cannot know that
             * the device carrying it is about to disappear, and that is the one
             * thing drop_dead() has to answer for. A repeat (value 2) changes
             * nothing -- the key is already down. */
            if (ev.type == EV_KEY) {
                if (ev.value == 1)
                    held_set(sources[idx].held, ev.code);
                else if (ev.value == 0)
                    held_clear(sources[idx].held, ev.code);
            }
            if (g_handler)
                g_handler(ev.type, ev.code, ev.value);
            taken++;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return DRAIN_OK;       /* nothing more queued */
        return (n < 0) ? DRAIN_ERROR : DRAIN_GONE;
    }
    return DRAIN_OK;
}

/* One poll() round over every open device, for at most timeout_ms.
 *
 * It returns nothing, deliberately: the caller's decisions are all about the
 * clock rather than about what this round did, and an earlier version's
 * "0 = read something, 1 = deadline" return is exactly the countdown that made a
 * hot plug wait for a quiet second. */
static void poll_round(int timeout_ms)
{
    struct pollfd pfd[EVDEV_SCAN_MAX];
    int r;

    for (int i = 0; i < n_sources; i++) {
        pfd[i].fd = sources[i].fd;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    r = real_poll(pfd, (nfds_t)n_sources, timeout_ms);
    if (r < 0) {
        if (errno == EINTR)
            return;                /* a signal is not a reason to do anything */
        /* Not EINTR. The open set is left exactly as it is -- tearing it down is
         * what loses a queued release, and an error here is as likely to be
         * transient (ENOMEM) as to be about these fds. The sleep is what stops a
         * persistently failing poll() from becoming a spin. */
        klog("knobshim2: evdev: poll: %s; keeping the %d open device(s)\n",
             strerror(errno), n_sources);
        usleep(EVDEV_EMPTY_SLEEP_MS * 1000);
        return;
    }
    if (r == 0)
        return;                    /* the timeout: nothing was readable */

    for (int i = 0; i < n_sources; ) {
        if (pfd[i].revents & POLLNVAL) {
            drop_dead(i, "poll says the fd is not valid any more");
            continue;              /* the table shifted down under this index */
        }
        if (pfd[i].revents & (POLLIN | POLLERR | POLLHUP)) {
            int d = drain_source(i);

            if (d != DRAIN_OK) {
                char why[80];

                /* errno is still the one the failed read set: nothing has run
                 * since. Naming it rather than assuming -ENODEV is the rule
                 * everywhere a device can go away. */
                if (d == DRAIN_ERROR)
                    snprintf(why, sizeof why, "read: %s", strerror(errno));
                else
                    snprintf(why, sizeof why, "read returned short (unplugged?)");
                drop_dead(i, why);
                continue;
            }
        }
        i++;
    }
}

static void *evdev_thread(void *arg)
{
    const char *pin = env_text("KBD_DEV", NULL);
    unsigned long long deadline = 0;
    /* 1 until the first empty scan reports, so an input-less machine says so
     * once instead of either silently or every 500 ms. */
    int said_empty = 0;
    (void)arg;

    if (pin)
        EVLOG("KBD_DEV=%s: reading that node instead of scanning "
              "every /dev/input/event*\n", pin);

    for (;;) {
        unsigned long long now;
        int wait;

        /* Nothing open: look, and if that finds nothing, wait. Sleeping is
         * correct HERE and nowhere else -- see EVDEV_EMPTY_SLEEP_MS. */
        if (n_sources == 0) {
            if (add_new_sources(pin) <= 0) {
                if (!said_empty) {
                    EVLOG("no device with EV_KEY in %s; looking again every "
                          "%d ms\n", pin ? pin : "/dev/input/event0..31",
                          EVDEV_EMPTY_SLEEP_MS);
                    said_empty = 1;
                }
                usleep(EVDEV_EMPTY_SLEEP_MS * 1000);
                continue;
            }
            said_empty = 0;
            deadline = shim_now_ms() + EVDEV_UNSEEN_MS;
        }

        now = shim_now_ms();
        if (now >= deadline) {
            /* The absolute deadline. It is compared against the clock and not
             * counted down per round, so traffic cannot postpone it: this is
             * what makes "a device that appears is noticed within a second" true
             * while a mouse is moving, rather than true only when quiet. */
            deadline = now + EVDEV_UNSEEN_MS;
            add_new_sources(pin);
            now = shim_now_ms();
        }

        wait = deadline > now ? (int)(deadline - now) : 0;
        poll_round(wait);
    }
    return NULL;
}

int evdev_start(void (*on_event)(int type, int code, int value))
{
    static int started;
    static pthread_t tid;

    if (started)
        return 0;
    if (!on_event)
        return -1;
    g_handler = on_event;
    if (pthread_create(&tid, NULL, evdev_thread, NULL) != 0)
        return -1;
    /* Detached like the front end's other threads: it never returns, and the
     * process it lives in is rbp. */
    pthread_detach(tid);
    started = 1;
    return 0;
}
