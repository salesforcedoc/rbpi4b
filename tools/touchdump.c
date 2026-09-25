/*
 * touchdump.c — tiny static tool to inspect an evdev touchscreen.
 * Prints the ABS ranges (absinfo) for the axes the fbshim cares about, then
 * dumps live events so the raw coordinate scale can be verified on the device.
 *
 * Build (static armhf, runs directly on the SC Live 4):
 *   arm-linux-gnueabihf-gcc -O2 -static -o touchdump touchdump.c
 *
 * Usage:
 *   ./touchdump [device]        # default /dev/input/event0
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

static void show_axis(int fd, int axis, const char *name)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(axis), &ai) == 0)
        printf("axis %-18s min=%d max=%d fuzz=%d flat=%d\n",
               name, ai.minimum, ai.maximum, ai.fuzz, ai.flat);
    else
        printf("axis %-18s (not supported)\n", name);
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event0";
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("device: %s\n", dev);
    printf("=== abs ranges ===\n");
    show_axis(fd, ABS_X, "ABS_X");
    show_axis(fd, ABS_Y, "ABS_Y");
    show_axis(fd, ABS_MT_POSITION_X, "ABS_MT_POSITION_X");
    show_axis(fd, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y");
    show_axis(fd, ABS_MT_TRACKING_ID, "ABS_MT_TRACKING_ID");
    show_axis(fd, ABS_MT_SLOT, "ABS_MT_SLOT");

    printf("=== live events ===\n");
    fflush(stdout);

    struct input_event ev;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            printf("t=%u.%06u type=%u code=%u value=%d\n",
                   ev.time.tv_sec, ev.time.tv_usec,
                   ev.type, ev.code, ev.value);
            fflush(stdout);
        } else if (n < 0) {
            usleep(10000);
        }
    }
    return 0;
}
