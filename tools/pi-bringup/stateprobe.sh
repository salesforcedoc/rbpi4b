#!/bin/sh
# Watch rbp's own state words and log them whenever they change.
#
#   sh stateprobe.sh <rbp-pid>
#
# Runs on the Pi, as root, and reads the player's memory through /proc/<pid>/mem.
# This is how the USB/SOURCE findings were made (docs/13-raspberrypi4.md): it is
# the difference between "the key was sent" (which the shim's log says) and "rbp
# acted on it" (which only rbp can say).
#
# The addresses are the ones the port has measured so far. They are not a
# supported interface -- they belong to one build of rbp -- so treat a run that
# reports all '?' as "this is a different rbp", not as "the state is zero".
#
#   0x03256888  det_usb1   USB1 detection flag
#   0x03256944  det_usb2   USB2 detection flag
#   0x326f8b4   media      media present (2 = enumerated)
#   0x326f8b8   mode       current screen (12 = SOURCE)
#   0x326f8bc   dev        selected device (3 = USB1)
#   0x326e128   refresh    redraw counter
#
# Find the pid without matching your own command line -- the launcher execs rbp
# through the loader, so `pkill -f /root/pdj/rbp` matches the SSH command string
# itself and kills the session (docs/13-raspberrypi4.md):
#
#   for p in /proc/[0-9]*/cmdline; do c=$(tr '\0' ' ' < "$p") || continue
#     case "$c" in /lib/ld-linux.so.3*/root/pdj/*) echo $(basename $(dirname $p));; esac
#   done | head -1
PID=$1
LOG=/tmp/stateprobe.log

[ -n "$PID" ] || { echo "usage: stateprobe.sh <rbp-pid>" >&2; exit 1; }
[ -r "/proc/$PID/mem" ] || { echo "no mem for $PID" >&2; exit 1; }

: > "$LOG"
echo "$(date +%H:%M:%S) start pid $PID" >> "$LOG"
prev=""

while :; do
  line=""
  for o in 0x03256888 0x03256944 0x326f8b4 0x326f8b8 0x326f8bc 0x326e128; do
    v=$(dd if=/proc/$PID/mem bs=1 skip=$((o)) count=4 2>/dev/null | od -An -tu4 | tr -d " \n")
    line="$line ${v:-?}"
  done
  if [ "$line" != "$prev" ]; then
    echo "$(date +%H:%M:%S) det1 det2 media mode dev refresh:$line" >> "$LOG"
    prev="$line"
  fi
  sleep 0.2
done
