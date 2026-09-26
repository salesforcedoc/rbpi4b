#!/usr/bin/env python3
"""Print every raw event from one /dev/input node, for a few seconds.

Runs **on the Pi**. This is the control for the virtual keyboard: it answers
"did the kernel emit that event at all?" independently of the shims, which is
how the lost-release defect in docs/16-input-and-hotplug.md was localised to the
shim rather than to uinput.

    python3 rawread.py /dev/input/event9        # 12 seconds, then exit

It reads the node directly and does not interfere with rbp: evdev delivers to
every client that has the device open.

Find the node a virtual keyboard landed on with:

    grep -l rblive4-vkeyd /sys/class/input/event*/device/name
"""
import os
import struct
import sys
import time

SECONDS = 12

path = sys.argv[1] if len(sys.argv) > 1 else "/dev/input/event0"
fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
print("reading %s for %ds" % (path, SECONDS), flush=True)

deadline = time.time() + SECONDS
while time.time() < deadline:
    try:
        data = os.read(fd, 16)
    except BlockingIOError:
        time.sleep(0.05)
        continue
    if len(data) == 16:
        _sec, _usec, etype, code, value = struct.unpack("llHHi", data)
        print("ev type=%d code=%d value=%d" % (etype, code, value), flush=True)
