#!/usr/bin/env python3
"""A virtual keyboard that stays up and presses keys on demand.

Runs **on the Pi**, as root. It creates a uinput device with a fixed key set and
then presses whatever it is told to, one keycode per line, from a FIFO:

    mkfifo /tmp/vkeyfifo
    setsid nohup python3 vkeyd.py >/tmp/vkeyd.log 2>&1 &
    echo 6 > /tmp/vkeyfifo           # press KEY_5 (SOURCE)
    echo "6@3000" > /tmp/vkeyfifo    # press KEY_5 and hold it 3000 ms

The reason this exists rather than a human at a keyboard: `map_kbd.c` tracks a
held state per key, and evdev_io.c's reader was dropping the release of any
press shorter than the window it spends with its devices closed. A human presses
a key for ~100 ms, so a human is exactly the wrong instrument for measuring
this. With a hold time you can dial, the release either arrives or it does not
(see docs/16-input-and-hotplug.md). The release defect is fixed; the hold time is
still what makes the *hot-plug* cases deterministic, which is the other thing
this daemon is for.

It used to be required to start **before** rbp: evdev_io.c discovered devices once
and then only rescanned when the device set changed, so a device created afterwards
was never enumerated by a running player. That was the hot-add defect and it is
fixed — the reader now notices a new device within a second regardless of traffic,
so this daemon can be started against an already-running rbp, which is what makes
it a hot-plug test rather than a startup test.

The FIFO is reopened after every EOF. A writer closing it is an EOF for us, and
a bare `for line in rf` would end the daemon the first time the shell closes the
fifo -- after which the next `echo > fifo` blocks forever, waiting for a reader
that no longer exists.
"""
import fcntl
import os
import struct
import time

UINPUT = "/dev/uinput"
FIFO = "/tmp/vkeyfifo"

EV_SYN, EV_KEY, SYN_REPORT = 0, 1, 0
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_SETUP = 0x405c5503
UI_DEV_CREATE = 0x5501

# The keys the maps can act on, plus a couple that are deliberately unbound so a
# test can prove the "unmapped evdev" path still reports what it sees.
# 1 Esc, 2/3 1/2 LOAD, 6/7/8 5/6/7 SOURCE/BROWSE/TAGLIST, 9/10 8/9
# PLAYLIST/SEARCH, 11 0 MENU, 14 Backspace BACK, 17/31 w/s, 28 Enter, 57 Space,
# 103/105/106/108 arrows. KEY_8/KEY_9 were added 2026-09-29 so the top menu's
# last two columns can be driven like the other four: map_kbd.c binds all six
# digit keys and there was no reason for the daemon to be able to reach only four.
KEYS = [1, 2, 3, 6, 7, 8, 9, 10, 11, 14, 17, 28, 31, 57, 103, 105, 106, 108]

# 32-bit host userland: struct input_event is time(2x long) + type + code +
# value = 16 bytes, which is what "llHHi" gives natively. A 64-bit userland
# would need a different format, and the Pi's is 32-bit (docs/05-chroot.md).
fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
for k in KEYS:
    fcntl.ioctl(fd, UI_SET_KEYBIT, k)
fcntl.ioctl(fd, UI_DEV_SETUP,
            struct.pack("HHHH80sI", 0x03, 0x1234, 0x5678, 1, b"rblive4-vkeyd", 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
print("vkeyd up, keys=%s" % KEYS, flush=True)


def emit(etype, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))


while True:
    rf = os.fdopen(os.open(FIFO, os.O_RDONLY))
    for line in rf:
        line = line.strip()
        if not line:
            continue
        hold = 0.12          # a human press, and the window that lost releases
        if "@" in line:
            code, ms = line.split("@")
            code, hold = int(code), float(ms) / 1000.0
        else:
            code = int(line)
        emit(EV_KEY, code, 1)
        emit(EV_SYN, SYN_REPORT, 0)
        time.sleep(hold)
        emit(EV_KEY, code, 0)
        emit(EV_SYN, SYN_REPORT, 0)
        print("pressed %d (hold %dms)" % (code, int(hold * 1000)), flush=True)
    rf.close()
