#!/usr/bin/env python3
"""Catch a display artifact: a burst of frames around an operator action.

Why a burst and not a screenshot. The artifact appears when rbp repaints a
region, and loading a track is the largest repaint there is. A single shot taken
afterwards may catch a picture that has already healed, and a shot taken
*before* tells you nothing. So the loop runs continuously while the operator
works and the moment is caught rather than synchronised with -- the same rule
the on-glass probes follow.

How it stays cheap, and why it is not a ring. Each frame is compared with the
one before it, and only a frame that *differs* is reported, with the row range
it differs in -- that is what says when something happened. The moment a frame
differs, it and its two predecessors are written to the unit immediately, and
the client pulls exactly the names the unit reports it kept.

The first version held a ring in RAM and wrote it out at the end. On the drill's
first live run the ring was smaller than the window, so the frames it had just
reported were evicted before the pull -- and the pull then computed its last
name from the ring's length rather than the frame counter and asked for a file
that had never been written. Writing on change is three frames of RAM and a
handful of writes for any window length, so the failure mode is gone rather than
sized around.

Read the result like this:

  * a change spanning rows 0..55        -> the swipe menu's band (docs/07)
  * a small change at the pointer's last position -> the arrow (fb_cursor.c)
  * a large, structured change across a deck      -> rbp's own redraw, which a
                                           real RX3 does too, i.e. not ours

    python3 tools/fbwatch.py                      # 120 s at 1 fps
    python3 tools/fbwatch.py --secs 60 --fps 2   # a tighter window

Ctrl-C stops it and kills the remote loop by pid; the remote process only ever
reads /dev/fb0.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbimg  # noqa: E402

REMOTE = r'''
import glob, os, sys, time

W, H, STRIDE = 1280, 800, 2560
FRAME = STRIDE * H
SECS, FPS, OUT = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]

os.makedirs(OUT, exist_ok=True)
for old in glob.glob(os.path.join(OUT, "*.raw")):
    os.unlink(old)                 # a re-run must not inherit an old window
open("/tmp/fbwatch.pid", "w").write("%d\n" % os.getpid())
fd = os.open("/dev/fb0", os.O_RDONLY)
period = 1.0 / FPS

print("MARKER fbwatch start: %dx%d stride=%d secs=%g fps=%g"
      % (W, H, STRIDE, SECS, FPS), flush=True)
print("GO -- load a track NOW if that is the gesture.", flush=True)

saved = []
def keep(n, b):
    """Write one frame out now. Called only on a change and at the end, so a
    long window costs a handful of 2 MB writes, not one per sample."""
    name = "%04d.raw" % n
    if name in saved:
        return
    saved.append(name)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(b)

prev2 = prev1 = None
t0 = time.time()
i = 0
prev = None
while time.time() - t0 < SECS:
    tick = time.time()
    b = os.pread(fd, FRAME, 0)
    if len(b) < FRAME:
        print("FRAME %d short read (%d bytes)" % (i, len(b)), flush=True)
        break
    if prev is None:
        print("FRAME %d first" % i, flush=True)
    elif b != prev:
        # Cheap localisation: XOR the two frames as one big integer and take
        # the first and last differing byte. Rows are contiguous, so that is
        # the row range -- the whole 2 MB comparison happens in C this way.
        # Only the RANGE is reported; two disjoint changes read as one span,
        # which is fine, because the pictures say which is which.
        # Byte k of the buffer is bit (FRAME-1-k)*8 of the big integer, so a
        # bit index becomes a byte index by >>3 first and only THEN a row.
        x = int.from_bytes(prev, "big") ^ int.from_bytes(b, "big")
        first_byte = FRAME - 1 - ((x.bit_length() - 1) >> 3)
        last_byte = FRAME - 1 - (((x & -x).bit_length() - 1) >> 3)
        print("FRAME %d changed rows %d..%d"
              % (i, first_byte // STRIDE, last_byte // STRIDE), flush=True)
        # The moment, with its two predecessors for context -- written now,
        # while they are still in hand, not held for a later sweep.
        if prev2 is not None:
            keep(i - 2, prev2)
        keep(i - 1, prev1)
        keep(i, b)
    prev2, prev1, prev = prev1, prev, b
    i += 1
    slack = period - (time.time() - tick)
    if slack > 0:
        time.sleep(slack)

keep(i - 1, prev)                  # the last frame of the window, calm or not
print("WROTE %d frames to %s" % (len(saved), OUT), flush=True)
print("KEPT %s" % " ".join(saved), flush=True)
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="root@192.168.1.239")
    ap.add_argument("--key", default="/Users/andrewsim/.ssh/cdj_root_key")
    ap.add_argument("--secs", type=float, default=120.0)
    ap.add_argument("--fps", type=float, default=1.0)
    ap.add_argument("--remote-dir", default="/tmp/fbseq")
    ap.add_argument("--out", default="work/fbseq")
    a = ap.parse_args()

    ssh = ["ssh", "-i", a.key, "-o", "StrictHostKeyChecking=no", a.host]
    proc = subprocess.Popen(
        ssh + ["python3", "-", str(a.secs), str(a.fps), a.remote_dir],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    wrote, kept = 0, []
    try:
        proc.stdin.write(REMOTE)
        proc.stdin.close()
        for line in proc.stdout:
            line = line.rstrip("\n")
            print(line, flush=True)
            if line.startswith("WROTE "):
                wrote = int(line.split()[1])
            elif line.startswith("KEPT "):
                kept = line.split()[1:]
    except KeyboardInterrupt:
        print("\nfbwatch: stopping the remote loop", file=sys.stderr)
        subprocess.run(ssh + ["sh", "-c",
                              'kill "$(cat /tmp/fbwatch.pid)" 2>/dev/null'],
                       capture_output=True)
        return 130
    proc.wait()

    if not wrote:
        print("fbwatch: the unit wrote no frames; nothing to pull")
        return 1

    # The unit names what it kept -- the changed frames with their two
    # predecessors, plus the window's last frame. Pull exactly those, rather
    # than recomputing the list here from a counter that is not the same
    # number as the file names.
    os.makedirs(a.out, exist_ok=True)
    names = sorted(kept)
    print("pulling %d of %d frames: %s" % (len(names), wrote, " ".join(names)))
    tar = subprocess.run(
        ssh + ["tar", "-C", a.remote_dir, "-cf", "-"] + names,
        stdout=subprocess.PIPE, check=True).stdout
    subprocess.run(["tar", "-xf", "-", "-C", a.out], input=tar, check=True)

    for name in names:
        raw = os.path.join(a.out, name)
        with open(raw, "rb") as f:
            fb = f.read()
        bmp = fbimg.write_bmp(fb, raw[:-4] + ".bmp")
        print(fbimg.to_png(bmp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
