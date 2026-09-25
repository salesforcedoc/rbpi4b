/*
 * evdevdump.c — enumerate and watch Linux input devices.
 *
 * Replaces touchdump.c, which only ever looked at /dev/input/event0 because on
 * the SC Live 4 the touchscreen was always there. On a Pi with an HDMI monitor
 * the pointer is a USB mouse, a keyboard, a touchscreen, or nothing at all, so
 * the shim has to *discover* its device instead of hardcoding one. This tool is
 * how POINT_DEV and the axis algebra get filled in:
 *
 *   ./evdevdump --list                  # every device: name, caps, absinfo
 *   ./evdevdump /dev/input/event2       # abs ranges + live events
 *
 * The --list verdict lines at the end are directly usable as shim env:
 *   POINT_KIND=rel POINT_DEV=/dev/input/event3
 *
 * Runs natively on the Pi:
 *   gcc -O2 -static -o evdevdump evdevdump.c
 *
 * Read-only: opens O_RDONLY and never writes to the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define BITS_PER_LONG (8 * (int)sizeof(unsigned long))
#define NLONGS(x)     (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static int test_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

/* EVIOCGBIT returns the bitmask in place; returns bytes written or -1. */
static int get_bits(int fd, int evtype, unsigned long *bits, int nbits)
{
    memset(bits, 0, NLONGS(nbits) * sizeof(*bits));
    return ioctl(fd, EVIOCGBIT(evtype, NLONGS(nbits) * sizeof(*bits)), bits);
}

struct axis {
    int code;
    const char *name;
};

/* The axes that matter for pointing. ABS_MT_* are preferred when present. */
static const struct axis abs_axes[] = {
    { ABS_X, "ABS_X" },
    { ABS_Y, "ABS_Y" },
    { ABS_MT_POSITION_X, "ABS_MT_POSITION_X" },
    { ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y" },
    { ABS_MT_SLOT, "ABS_MT_SLOT" },
    { ABS_MT_TRACKING_ID, "ABS_MT_TRACKING_ID" },
    { ABS_PRESSURE, "ABS_PRESSURE" },
};

static const char *key_name(int code)
{
    switch (code) {
    /* NB: BTN_MOUSE is an alias for BTN_LEFT (both 0x110 in input.h), so it
     * cannot have its own case here or the switch fails to compile. */
    case BTN_LEFT:   return "BTN_LEFT";
    case BTN_RIGHT:  return "BTN_RIGHT";
    case BTN_MIDDLE: return "BTN_MIDDLE";
    case BTN_TOUCH:  return "BTN_TOUCH";
    case BTN_TOOL_FINGER: return "BTN_TOOL_FINGER";
    default: return NULL;
    }
}

static const char *rel_name(int code)
{
    switch (code) {
    case REL_X:     return "REL_X";
    case REL_Y:     return "REL_Y";
    case REL_WHEEL: return "REL_WHEEL";
    case REL_HWHEEL: return "REL_HWHEEL";
    default: return NULL;
    }
}

static const char *abs_name(int code)
{
    for (size_t i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); i++)
        if (abs_axes[i].code == code)
            return abs_axes[i].name;
    return NULL;
}

static const char *ev_name(int type)
{
    switch (type) {
    case EV_SYN: return "EV_SYN";
    case EV_KEY: return "EV_KEY";
    case EV_REL: return "EV_REL";
    case EV_ABS: return "EV_ABS";
    case EV_MSC: return "EV_MSC";
    case EV_SW:  return "EV_SW";
    case EV_LED: return "EV_LED";
    case EV_REP: return "EV_REP";
    default:     return NULL;
    }
}

static void show_axis(int fd, int axis, const char *name)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(axis), &ai) == 0)
        printf("    %-18s min=%-6d max=%-6d fuzz=%-4d flat=%-4d res=%d\n",
               name, ai.minimum, ai.maximum, ai.fuzz, ai.flat, ai.resolution);
}

/*
 * One device's full picture. Returns a classification for the summary:
 * 0 = not a pointer, 1 = absolute pointer, 2 = relative pointer.
 */
static int describe(const char *dev, int verbose)
{
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    char name[256] = {0}, phys[256] = {0}, uniq[256] = {0};
    struct input_id id;
    memset(&id, 0, sizeof(id));

    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        strcpy(name, "(unnamed)");
    if (ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) < 0)
        phys[0] = '\0';
    if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq) < 0)
        uniq[0] = '\0';
    if (ioctl(fd, EVIOCGID, &id) < 0)
        memset(&id, 0, sizeof(id));

    unsigned long evbits[NLONGS(EV_MAX + 1)];
    unsigned long keybits[NLONGS(KEY_MAX + 1)];
    unsigned long relbits[NLONGS(REL_MAX + 1)];
    unsigned long absbits[NLONGS(ABS_MAX + 1)];
    memset(keybits, 0, sizeof(keybits));
    memset(relbits, 0, sizeof(relbits));
    memset(absbits, 0, sizeof(absbits));

    if (get_bits(fd, 0, evbits, EV_MAX + 1) < 0) {
        close(fd);
        return -1;
    }
    if (test_bit(evbits, EV_KEY))
        get_bits(fd, EV_KEY, keybits, KEY_MAX + 1);
    if (test_bit(evbits, EV_REL))
        get_bits(fd, EV_REL, relbits, REL_MAX + 1);
    if (test_bit(evbits, EV_ABS))
        get_bits(fd, EV_ABS, absbits, ABS_MAX + 1);

    printf("%s\n", dev);
    printf("  name                %s\n", name);
    if (phys[0]) printf("  phys                %s\n", phys);
    if (uniq[0]) printf("  uniq                %s\n", uniq);
    printf("  id                  bustype=%04x vendor=%04x product=%04x version=%04x\n",
           id.bustype, id.vendor, id.product, id.version);

    printf("  events             ");
    for (int t = 0; t <= EV_MAX; t++)
        if (test_bit(evbits, t)) {
            const char *n = ev_name(t);
            if (n) printf(" %s", n);
            else   printf(" EV_%d", t);
        }
    printf("\n");

    /* Capability flags the shim's discovery actually keys off. */
    int has_btn_touch = test_bit(keybits, BTN_TOUCH);
    int has_btn_left  = test_bit(keybits, BTN_LEFT);   /* == BTN_MOUSE */
    int has_abs_xy    = test_bit(absbits, ABS_X) && test_bit(absbits, ABS_Y);
    int has_abs_mt    = test_bit(absbits, ABS_MT_POSITION_X) &&
                        test_bit(absbits, ABS_MT_POSITION_Y);
    int has_rel_xy    = test_bit(relbits, REL_X) && test_bit(relbits, REL_Y);

    if (has_abs_xy || has_abs_mt || has_rel_xy || has_btn_left || has_btn_touch) {
        printf("  pointer caps        ");
        if (has_abs_xy)    printf(" ABS_X/Y");
        if (has_abs_mt)    printf(" ABS_MT_POSITION_X/Y");
        if (has_rel_xy)    printf(" REL_X/Y");
        if (has_btn_left)  printf(" BTN_LEFT");
        if (has_btn_touch) printf(" BTN_TOUCH");
        printf("\n");
    }

    if (test_bit(evbits, EV_REL)) {
        printf("  rel axes           ");
        for (int c = 0; c <= REL_MAX; c++)
            if (test_bit(relbits, c)) {
                const char *n = rel_name(c);
                if (n) printf(" %s", n);
                else   printf(" REL_%d", c);
            }
        printf("\n");
    }

    if (test_bit(evbits, EV_ABS)) {
        printf("  abs axes\n");
        for (size_t i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); i++)
            if (test_bit(absbits, abs_axes[i].code))
                show_axis(fd, abs_axes[i].code, abs_axes[i].name);
    }

    /* A pointer is only useful if it can report a click. */
    int nkeys = 0;
    for (int c = 0; c <= KEY_MAX; c++)
        if (test_bit(keybits, c))
            nkeys++;
    printf("  keys                %d", nkeys);
    if (has_btn_left)  printf(" (incl. BTN_LEFT)");
    if (has_btn_touch) printf(" (incl. BTN_TOUCH)");
    printf("\n");

    if (verbose) {
        /* Not every axis gets a pretty name above; if nothing printed but the
         * device claims EV_ABS, say so rather than looking silently empty. */
        int printed = 0;
        for (size_t i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); i++)
            if (test_bit(absbits, abs_axes[i].code))
                printed = 1;
        if (test_bit(evbits, EV_ABS) && !printed)
            printf("  abs axes            (none of the pointing axes)\n");
    }

    int kind = 0;
    if (has_abs_xy || has_abs_mt)
        kind = 1;
    else if (has_rel_xy && has_btn_left)
        kind = 2;

    close(fd);
    return kind;
}

/* Scan /dev/input for event* nodes in numeric order. */
static int list_devices(void)
{
    char found[64][64];
    int n = 0;

    for (int i = 0; i < 64 && n < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, R_OK) == 0)
            snprintf(found[n++], sizeof(found[0]), "%s", path);
    }

    if (n == 0) {
        fprintf(stderr,
                "evdevdump: no /dev/input/event* nodes.\n"
                "  If a USB mouse/keyboard is plugged in and these are missing,\n"
                "  the input stack is not up: check dmesg for 'usbhid' and that\n"
                "  /sys/class/input exists. In a chroot, /dev must be bind-mounted.\n");
        return 1;
    }

    int kinds[64];
    for (int i = 0; i < n; i++) {
        printf("\n");
        kinds[i] = describe(found[i], 1);
    }

    printf("\n=== summary: what to put in rb.conf ===\n");
    int any = 0;
    for (int i = 0; i < n; i++) {
        if (kinds[i] == 1) {
            printf("POINT_KIND=abs POINT_DEV=%s\n", found[i]);
            any = 1;
        } else if (kinds[i] == 2) {
            printf("POINT_KIND=rel POINT_DEV=%s\n", found[i]);
            any = 1;
        }
    }
    if (!any) {
        printf("no pointer found (no ABS_X/Y or REL_X/Y+BTN_LEFT device).\n"
               "  Keyboard-only operation is still possible: the keyboard map\n"
               "  (map_kbd.c) drives playback with no pointing device at all.\n");
    } else {
        printf("\nThen run the learn procedure: POINT_DEBUG=1, click the four\n"
               "corners of the UI, and read the emitted logical coords. If they\n"
               "are mirrored or transposed, fix it with POINT_INVERT_X /\n"
               "POINT_INVERT_Y / POINT_SWAP_XY before touching any code.\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = NULL;
    int list = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "-l"))
            list = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: evdevdump --list\n"
                   "       evdevdump [device]     (default /dev/input/event0)\n");
            return 0;
        } else
            dev = argv[i];
    }

    if (list)
        return list_devices();

    if (!dev)
        dev = "/dev/input/event0";

    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "evdevdump: open %s: %s\n", dev, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "  run `evdevdump --list` to find the right node.\n");
        return 1;
    }

    printf("device: %s\n", dev);
    printf("=== abs ranges ===\n");
    for (size_t i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); i++)
        show_axis(fd, abs_axes[i].code, abs_axes[i].name);

    printf("=== live events ===\n");
    fflush(stdout);

    struct input_event ev;
    int last_code = -1;
    for (;;) {
        ssize_t r = read(fd, &ev, sizeof(ev));
        if (r == (ssize_t)sizeof(ev)) {
            const char *n = NULL;
            if (ev.type == EV_KEY) n = key_name(ev.code);
            else if (ev.type == EV_REL) n = rel_name(ev.code);
            else if (ev.type == EV_ABS) n = abs_name(ev.code);

            /* Collapse a repeated drag axis so the log stays readable; the
             * first value of a run is the one that matters. */
            if (ev.type == EV_ABS && ev.code == last_code)
                continue;
            last_code = ev.code;

            /* tv_sec/tv_usec are long on 32-bit ARM, so cast rather than
             * assuming unsigned int (this is the bug touchdump.c shipped). */
            if (n)
                printf("t=%lu.%06lu type=%u(%s) code=%u(%s) value=%d\n",
                       (unsigned long)ev.time.tv_sec,
                       (unsigned long)ev.time.tv_usec,
                       ev.type, ev_name(ev.type) ? ev_name(ev.type) : "?",
                       ev.code, n, ev.value);
            else
                printf("t=%lu.%06lu type=%u code=%u value=%d\n",
                       (unsigned long)ev.time.tv_sec,
                       (unsigned long)ev.time.tv_usec,
                       ev.type, ev.code, ev.value);
            fflush(stdout);
        } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "evdevdump: read: %s\n", strerror(errno));
            return 1;
        } else {
            usleep(10000);
        }
    }
    return 0;
}
