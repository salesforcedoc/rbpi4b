#!/usr/bin/env python3
"""bootscreen.py — draw the boot stages on /dev/fb0, from first light to rbp's
first frame.

WHY THIS EXISTS. The unit is black from the real framebuffer coming up
(`vc4drmfb`, ~6.7 s) until rbp paints its first frame (~13-18 s) -- a ~10 s
silent gap, and on the blank cold boot (docs/, "the cold boot never flips")
a black screen that is the *symptom* of a defect with no other on-glass sign.
This unit starts early in boot, tails the launcher's stage messages, and turns
that gap into a readable one.

WHAT IT DRAWS. A title with the boot uptime, a scrolling log of the launcher's
stages (from /run/rblive4/boot.log, which start-rb.sh appends to via
rb_bootmsg()), and a 1 Hz heartbeat. The heartbeat is deliberate: a static
picture cannot be told apart from a frozen one, and this whole file exists to be
*read off the glass* -- the same rule tools/fbscreen.py states for its own loop.

WHEN IT STOPS. The moment rbp paints its first frame. If rbp never paints -- the
blank cold boot -- it STAYS UP with an honest line ("rbp is running, drawing
nothing"), which is the operator's choice: leave only at the moment a real frame
arrives. Every exit path is exit 0, so this unit can never break boot.

THE THREE TRAPS, all of which the first draft got wrong:

  1. WAIT FOR vc4drmfb BEFORE OPENING. `fb0: simplefb registered!` at ~0.78 s is
     the firmware framebuffer; the real one is `vc4drmfb` at ~6.74 s, and the
     kernel hands over in between. An fd opened on the simplefb is a mapping of a
     torn-down framebuffer. So poll /sys/class/graphics/fb0/name FIRST, and only
     then open + mmap.

  2. THE EXIT TEST MUST ASK *WHOSE* PIXELS. "Redraw while fb0 is black, exit when
     it is not" cannot both hold: this daemon's own paint makes fb0 non-black, so
     it would exit after one frame. Keep a SHADOW of the last frame written, and
     exit only on a NON-BLACK frame that is not ours. An all-black frame that is
     not ours (rbp's own start-up clear, a display baseline) reads as all-black
     and is correctly ignored. The *first* tick paints unconditionally, because
     what is on fb0 when we start -- a frame from a previous rbp run, or the
     firmware's, before the handover -- is stale evidence, not a live frame, and
     a screen that refused to draw over it would silently never appear.

  3. VERIFY THE GEOMETRY, DO NOT ASSUME IT. A mis-strided write does not look
     like a bug, it looks like a clean picture (tools/fbscreen.py's own warning).
     Read virtual_size / stride / bits_per_pixel from sysfs and refuse to paint on
     a mismatch. sysfs, not FBIOGET_*: no ioctl is issued at all, so this process
     cannot reset the mode, blank, or pan -- it only ever reads and writes the
     shared mapping.

The format is 1280x800 RGB565 at stride 2560 = 2,048,000 contiguous bytes, so
pixel (x, y) is at (y*1280 + x) * 2. rbp is *told* 32 bpp by the fbshim
(RB_FB_LIE_BPP); believing that here writes garbage. tools/fbscreen.py carries the
same constants and the write(2)-advances-f_pos trap; this file uses an mmap, which
has no file offset to get wrong.

Run by rblive4-boot.service (ExecStart=/usr/bin/python3 .../bootscreen.py).
"""
import mmap
import os
import struct
import sys
import time

W, H, STRIDE, BPP = 1280, 800, 2560, 2
SIZE = W * H * BPP                      # 2,048,000 -- matches fix.smem_len

FB = os.environ.get("RB_FB", "/dev/fb0")
FB_NAME = os.environ.get("RB_FB_NAME", "/sys/class/graphics/fb0/name")
FB_SYSFS = os.environ.get("RB_FB_SYSFS", "/sys/class/graphics/fb0")
BOOT_LOG = os.environ.get("RB_BOOT_LOG", "/run/rblive4/boot.log")
RUN_DIR = os.environ.get("RB_RUN_DIR", "/run/rblive4")
STAGE_FILE = os.path.join(RUN_DIR, "boot.stage")

WAIT_FB_S = 15.0                        # how long to wait for vc4drmfb
TICK_S = 0.25                           # redraw rate (~4 Hz)
# How long after "launching rbp" the screen waits before it says the honest
# "rbp is running, drawing nothing" line. Overridable only so the host test can
# exercise that path in a second instead of ten.
RB_DRAW_S = float(os.environ.get("RB_DRAW_S", "10.0"))

# RGB565
BLACK = 0x0000
WHITE = 0xFFFF
GREEN = 0x07E0
DIM = 0x03E0                            # a dim green, for timestamps
AMBER = 0xFDA0                          # for the honest final line
EDGE = 0x0200

# A 5x7 bitmap font, uppercase only -- one string per scanline, '1' = lit,
# leftmost char = left column. The log carries lowercase paths; they are
# uppercased on the way in. A character with no entry (a space) still advances;
# it just draws nothing. tools/fbscreen.py's digits are reused so the two files
# cannot disagree about what an '8' looks like.
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("01110", "00100", "00100", "00100", "00100", "00100", "01110"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "10001", "11001", "10101", "10011", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    ":": ("00000", "01100", "01100", "00000", "01100", "01100", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "_": ("00000", "00000", "00000", "00000", "00000", "00000", "11111"),
    "/": ("00001", "00001", "00010", "00100", "01000", "10000", "10000"),
    "=": ("00000", "00000", "11111", "00000", "11111", "00000", "00000"),
    "+": ("00000", "00100", "00100", "11111", "00100", "00100", "00000"),
    "(": ("00010", "00100", "01000", "01000", "01000", "00100", "00010"),
    ")": ("01000", "00100", "00010", "00010", "00010", "00100", "01000"),
    ">": ("10000", "01000", "00100", "00010", "00100", "01000", "10000"),
    "<": ("00001", "00010", "00100", "01000", "00100", "00010", "00001"),
    ",": ("00000", "00000", "00000", "00000", "00100", "00100", "01000"),
}
GLYPHS = {ch: tuple(int(r, 2) for r in rows) for ch, rows in FONT.items()}

GLYPH_W, GLYPH_H, ADVANCE = 5, 7, 6      # in font cells; the 1 is the gap
TITLE_SCALE = 4
BODY_SCALE = 2
MARGIN = 16
LINE_H = GLYPH_H * BODY_SCALE + 6        # 20 px
HEADER_H = 64
FOOTER_H = 56
MAX_COLS = (W - 2 * MARGIN) // (ADVANCE * BODY_SCALE)   # 100
BODY_ROWS = (H - HEADER_H - FOOTER_H) // LINE_H         # 34


def fill_rect(buf, x, y, w, h, colour):
    chunk = struct.pack("<H", colour) * w
    for k in range(h):
        off = ((y + k) * W + x) * 2
        buf[off:off + len(chunk)] = chunk


def draw_text(buf, s, x0, y0, scale, colour):
    """Blit `s` with its top-left at (x0, y0). Runs, so a glyph costs one slice
    assignment per lit scanline rather than one per pixel."""
    chunk = struct.pack("<H", colour) * scale
    cx = x0
    for ch in s:
        g = GLYPHS.get(ch)
        if g is not None:
            for r, row in enumerate(g):
                for col in range(GLYPH_W):
                    if row & (1 << (GLYPH_W - 1 - col)):
                        px, py = cx + col * scale, y0 + r * scale
                        for k in range(scale):
                            off = ((py + k) * W + px) * 2
                            buf[off:off + len(chunk)] = chunk
        cx += ADVANCE * scale


def _split(line):
    """rb_bootmsg writes ' 12.34  message'. Return (stamp, message); a line with
    no leading number is all message."""
    parts = line.split("  ", 1)
    if len(parts) == 2:
        head = parts[0].strip()
        try:
            float(head)
            return head, parts[1]
        except ValueError:
            pass
    return "", line


def render(lines, tick, status, uptime):
    """Build one full frame. Pure: no fd, no clock, no I/O -- so it can be
    rendered and checked on a workstation with no /dev/fb0."""
    buf = bytearray(SIZE)                        # all black
    fill_rect(buf, 0, 0, W, 3, EDGE)
    fill_rect(buf, 0, H - 3, W, 3, EDGE)

    title = "RBLIVE4 BOOT"
    draw_text(buf, title, MARGIN, 16, TITLE_SCALE, WHITE)
    x = MARGIN + len(title) * ADVANCE * TITLE_SCALE + 12
    draw_text(buf, "T+%dS" % int(uptime), x, 16 + GLYPH_H * (TITLE_SCALE - BODY_SCALE),
              BODY_SCALE, DIM)
    fill_rect(buf, MARGIN, HEADER_H - 8, W - 2 * MARGIN, 3, EDGE)

    # Body: the last BODY_ROWS lines, oldest at the top.
    for i, line in enumerate(lines[-BODY_ROWS:]):
        y = HEADER_H + i * LINE_H
        stamp, msg = _split(line)
        x = MARGIN
        if stamp:
            draw_text(buf, stamp, x, y, BODY_SCALE, DIM)
            x += len(stamp) * ADVANCE * BODY_SCALE + 8
        draw_text(buf, msg[:MAX_COLS].upper(), x, y, BODY_SCALE, GREEN)

    # Footer: a 1 Hz heartbeat block (so a static screen cannot be mistaken for a
    # frozen one) and a status word.
    fy = H - FOOTER_H
    fill_rect(buf, MARGIN, fy, W - 2 * MARGIN, 3, EDGE)
    if tick % 2:
        fill_rect(buf, MARGIN, fy + 20, 24, 24, GREEN)
    else:
        fill_rect(buf, MARGIN, fy + 20, 24, 24, EDGE)
    status_up = status.upper()
    colour = AMBER if status_up.startswith("RBP") else GREEN
    draw_text(buf, status_up, MARGIN + 40, fy + 26, BODY_SCALE, colour)
    return buf


def stage(word, detail=""):
    """A breadcrumb for 'the screen did not appear'. Best-effort only.

    It mkdir's its own parent: the paths that most need a breadcrumb (a geometry
    refusal, a failed open) are exactly the ones that return BEFORE main() makes
    the run directory, so without this the message that explains the blank screen
    would itself be lost -- a failure indistinguishable from no failure at all."""
    try:
        os.makedirs(RUN_DIR, exist_ok=True)
        with open(STAGE_FILE, "w") as f:
            f.write("%s %s\n" % (word, detail))
    except OSError:
        pass
    sys.stderr.write("bootscreen: %s %s\n" % (word, detail))


def uptime():
    try:
        with open("/proc/uptime") as f:
            return float(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return time.monotonic() - _START


_START = time.monotonic()


def fb_is_vc4drmfb():
    try:
        with open(FB_NAME) as f:
            return f.read().strip() == "vc4drmfb"
    except OSError:
        return False


def sysfs_int(name):
    try:
        with open(os.path.join(FB_SYSFS, name)) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def geometry_ok():
    """The real geometry, read from sysfs -- not the shim's 32-bpp lie and not a
    guess. Returns the frame size in bytes, or None on any mismatch."""
    vs = None
    try:
        with open(os.path.join(FB_SYSFS, "virtual_size")) as f:
            vs = f.read().strip()
    except OSError:
        return None
    stride = sysfs_int("stride")
    bpp = sysfs_int("bits_per_pixel")
    if vs != "%d,%d" % (W, H) or stride != STRIDE or bpp != BPP * 8:
        stage("bad-geom", "vs=%s stride=%s bpp=%s" % (vs, stride, bpp))
        return None
    return stride * H


def read_log():
    try:
        with open(BOOT_LOG, "r", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def main():
    t0 = time.monotonic()
    while not fb_is_vc4drmfb():
        if time.monotonic() - t0 > WAIT_FB_S:
            stage("no-fb", "fb0 never became vc4drmfb within %ds" % int(WAIT_FB_S))
            return 0
        time.sleep(0.2)

    try:
        fd = os.open(FB, os.O_RDWR)
    except OSError as e:
        stage("no-open", str(e))
        return 0
    try:
        size = geometry_ok()
        if size is None:
            return 0
        mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    except (OSError, ValueError) as e:
        stage("no-mmap", str(e))
        return 0

    os.makedirs(RUN_DIR, exist_ok=True)
    stage("up", "fb0=vc4drmfb size=%d" % size)

    black = b"\x00" * size
    shadow = None
    launcher_line = None                    # when "launching rbp" first appeared
    tick = 0
    first = True
    while True:
        cur = mm[:]
        # Exit only on a NON-BLACK frame that is not the one we last wrote: an
        # all-black clear that is not ours is ignored (trap 2), and our own paint
        # must not read as "someone else drew".
        #
        # The FIRST tick skips the test and paints regardless. At boot fb0 is
        # *expected* to be black (fbcon never binds it), but a frame left by a
        # previous rbp run -- or by the firmware, before the vc4drmfb handover --
        # is stale evidence, not "rbp has drawn", and refusing to draw over it
        # would make the screen silently never appear: the worst failure for a
        # diagnostic, because it is indistinguishable from the unit being black.
        # The cost is bounded and only reachable by an explicit mid-session
        # `systemctl start rblive4-boot`: if rbp is live, we overwrite its frame
        # for at most one tick (~250 ms) before rbp's own next flip, well before
        # the shadow rule exits us. At boot the ordering (Before=rblive4) means
        # rbp cannot have painted yet, so this is a no-op there.
        if not first and cur != black and (shadow is None or cur != shadow):
            stage("rbp-drew", "uptime=%.1f" % uptime())
            return 0
        first = False

        lines = read_log()
        if launcher_line is None:
            for ln in lines:
                if "launching rbp" in ln:
                    launcher_line = time.monotonic()
                    break
        # The blank cold boot: rbp was launched, it is drawing nothing we can see.
        if launcher_line is not None and time.monotonic() - launcher_line > RB_DRAW_S:
            status = "rbp is running, drawing nothing"
        else:
            status = "waiting for rbp"

        frame = render(lines, tick, status, uptime())
        mm[:] = frame
        shadow = frame
        tick += 1
        time.sleep(TICK_S)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except BaseException as e:              # never break boot, whatever happens
        try:
            stage("fault", repr(e))
        except Exception:
            pass
        sys.exit(0)
