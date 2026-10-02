#!/bin/sh
# Drive a key sequence through the virtual keyboard, capturing the framebuffer
# after each key, and leaving a state-probe log beside the captures.
#
#   sh drive-keys.sh            # on the Pi, as root
#
# Expects vkeyd.py, stateprobe.sh and a FIFO at /tmp/vkeyfifo, and restarts the
# player so the virtual device is enumerated (the reader does not notice a
# device that appears afterwards -- docs/16-input-and-hotplug.md).
#
# Why capture the screen at all: the shim's log can only say a keycode was sent.
# Whether rbp *did* something with it is a question about pixels, and reading
# /dev/fb0 is the only instrument the port has for it. Fetch the .raw files and
# compare them -- an unchanged screen after a key is a real result and is how
# the up/down sign turned out to be unmeasurable on the SOURCE panel:
#
#   scp root@<pi>:/tmp/cap_*.raw .
#   python3 - <<'PY'
#   import numpy as np, glob
#   for f in sorted(glob.glob("cap_*.raw")):
#       a = np.frombuffer(open(f,"rb").read(), dtype="<u2").reshape(800,1280)
#       r,g,b = (a>>11&31).astype(int)<<3, (a>>5&63).astype(int)<<2, (a&31).astype(int)<<3
#       print(f, "lit", int(((r+g+b)>120).sum()))
#   PY
#
# 1280x800 RGB565, stride 2560, one page: 2048000 bytes (docs/06-display.md).

set -u

FIFO=/tmp/vkeyfifo
[ -p "$FIFO" ] || { echo "no FIFO at $FIFO -- start vkeyd.py first" >&2; exit 1; }

systemctl restart rblive4
sleep 16

# The launcher execs rbp through the loader, so match the first argv element
# rather than the name -- `pgrep -f rbp` also matches this script's own text.
PID=$(for p in /proc/[0-9]*/cmdline; do c=$(tr '\0' ' ' < "$p" 2>/dev/null) || continue
      case "$c" in /lib/ld-linux.so.3*/root/pdj/*) basename "$(dirname "$p")";; esac
      done 2>/dev/null | head -1)
echo "rbp pid $PID"
grep -a "vkeyd" /tmp/knobshim.log | tail -1

setsid nohup sh "$(dirname "$0")/stateprobe.sh" "$PID" >/dev/null 2>&1 &

cap() { dd if=/dev/fb0 bs=2560 count=800 2>/dev/null > "/tmp/cap_$1.raw"; echo "captured $1"; }

cap 0_deck
echo 6 > $FIFO;   sleep 3; cap 1_source     # KEY_5 -> K_SOURCE
echo 108 > $FIFO; sleep 3; cap 2_down       # KEY_DOWN
echo 108 > $FIFO; sleep 3; cap 3_down2
echo 103 > $FIFO; sleep 3; cap 4_up         # KEY_UP
echo 28 > $FIFO;  sleep 4; cap 5_enter      # KEY_ENTER

echo "--- state probe ---"; cat /tmp/stateprobe.log
echo "--- what the map saw ---"; grep -a "kbd: evdev" /tmp/knobshim.log | tail -12
