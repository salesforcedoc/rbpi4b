#!/usr/bin/env python3
"""Put a KNOWN picture on the unit's screen, so the panel can be read off the glass.

Two patterns, both loops:

    fbscreen.py clock          a ticking HH:MM:SS on the second boundary
    fbscreen.py solid          white -> red -> green -> blue -> black, 1 Hz

A loop, not a one-shot, on purpose. A single paint cannot be told apart from
"the write landed and something repainted over it", and this unit has several
things that repaint. A clock that ticks, or a colour that cycles, is a
known-good marker: the picture itself says whether the framebuffer is alive.

How to read it off the glass:

  * rbp healthy          -> the picture STROBES. rbp repaints over it ~46/s,
                            so you see the clock flicker through, not sit.
  * rbp in the blank
    cold-boot state      -> the picture STICKS, clean and still, because rbp is
                            running its loop and drawing nothing to compete
                            with. That is the discriminator this tool exists
                            for: it separates "the panel is broken" from "rbp
                            never produced a frame", and only the second one is
                            what this unit does (see docs/, the cold-boot note).

    python3 fbscreen.py clock          # Ctrl-C stops it
    python3 fbscreen.py clock --once   # one frame, for a timestamped capture
    python3 fbscreen.py solid --colour red

The format, and the two traps this file is written around.

`/dev/fb0` is vc4drmfb, 1280x800, **16 bpp RGB565 at stride 2560** -- two bytes
per pixel with no row padding, so the frame is 2,048,000 contiguous bytes and
pixel (x, y) sits at (y*1280 + x) * 2. rbp is told 32 bpp by the fbshim
(`RB_FB_LIE_BPP`); a raw write from the shell must not believe it. The lie is
visible in `fix.smem_len` = 2048000, which is 16 bpp and not 32.

  * `write(2)` on /dev/fb0 **advances f_pos**, and the driver refuses a write
    that would run past the end of the screen. So a loop built on `write()`
    paints exactly one frame and then dies -- silently, if the caller ignores
    the return. This file uses an **mmap**, which has no file offset to get
    wrong. (An earlier version of the solid pattern made exactly that mistake.)

  * A mis-strided write does not look like a bug, it looks like a clean colour.
    The border frame, and the black bars in `solid`, make bad geometry show up
    as geometry.

Runs natively on the unit:

    python3 fbscreen.py clock
"""
import argparse
import mmap
import os
import struct
import sys
import time

W, H, STRIDE, BPP = 1280, 800, 2560, 2
SIZE = W * H * BPP                      # 2,048,000 -- matches fix.smem_len

# RGB565
COLOURS = {
    "white": 0xF800 | 0x07E0 | 0x001F,
    "red":   0xF800,
    "green": 0x07E0,
    "blue":  0x001F,
    "black": 0x0000,
}

# A 5x7 bitmap font, one string per scanline, '1' = lit, leftmost char = left
# column. Only what a clock needs: the ten digits and the two separators. A
# character with no entry (a space) still advances -- it just draws nothing.
FONT = {
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
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
}
GLYPHS = {ch: tuple(int(r, 2) for r in rows) for ch, rows in FONT.items()}

GLYPH_W, GLYPH_H, ADVANCE = 5, 7, 6      # in font cells; the 1 is the gap
EDGE = 0x0200                            # a dim green, for the frame
DIM = 0x03E0


def text_width(s, scale):
    return len(s) * ADVANCE * scale - scale


def fill_rect(buf, x, y, w, h, colour):
    chunk = struct.pack("<H", colour) * w
    for k in range(h):
        off = ((y + k) * W + x) * 2
        buf[off:off + len(chunk)] = chunk


def draw_text(buf, s, x0, y0, scale, colour):
    """Blit `s` with its top-left at (x0, y0). Rows go down as runs, so a glyph
    costs one slice assignment per lit scanline rather than one per pixel."""
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


def border(buf):
    """Frame the whole 1280x800 area, so a partial or mis-strided write reads
    as geometry rather than looking like a clean picture."""
    fill_rect(buf, 0, 0, W, 4, EDGE)
    fill_rect(buf, 0, H - 4, W, 4, EDGE)
    fill_rect(buf, 0, 0, 4, H, EDGE)
    fill_rect(buf, W - 4, 0, 4, H, EDGE)


def render_clock(now):
    buf = bytearray(SIZE)                       # all black
    border(buf)
    clock = time.strftime("%H:%M:%S", now)
    stamp = time.strftime("%Y-%m-%d", now)
    ts, ds = 20, 8
    y0 = (H - (GLYPH_H * ts + 40 + GLYPH_H * ds)) // 2
    draw_text(buf, clock, (W - text_width(clock, ts)) // 2, y0, ts, COLOURS["green"])
    draw_text(buf, stamp, (W - text_width(stamp, ds)) // 2,
              y0 + GLYPH_H * ts + 40, ds, DIM)
    return buf


def render_solid(px):
    one = struct.pack("<H", px)
    buf = bytearray(one * (W * H))              # no padding: STRIDE == W * BPP
    black = struct.pack("<H", 0x0000)
    for y in range(H):
        buf[y * STRIDE:y * STRIDE + 8] = black * 4      # a bar up the left edge
    for y in range(H - 8, H):                           # and along the bottom
        buf[y * STRIDE:(y + 1) * STRIDE] = black * (STRIDE // 2)
    return buf


def open_fb():
    try:
        fd = os.open("/dev/fb0", os.O_RDWR)
    except OSError as e:
        sys.exit("fbscreen: cannot open /dev/fb0: %s" % e)
    try:
        return fd, mmap.mmap(fd, SIZE, mmap.MAP_SHARED,
                             mmap.PROT_READ | mmap.PROT_WRITE)
    except (OSError, ValueError) as e:
        sys.exit("fbscreen: cannot mmap %d bytes of /dev/fb0: %s" % (SIZE, e))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="pattern", required=True)

    sub.add_parser("clock", help="a ticking HH:MM:SS") \
        .add_argument("--once", action="store_true", help="one frame, then exit")

    s = sub.add_parser("solid", help="cycling solid colours")
    s.add_argument("--colour", choices=sorted(COLOURS),
                   help="hold one colour instead of cycling")
    s.add_argument("--once", action="store_true", help="one frame, then exit")

    a = ap.parse_args()
    fd, mm = open_fb()

    if a.pattern == "clock":
        def render(n):
            # localtime() once, and hand the same moment to the name and the
            # picture -- otherwise a tick landing mid-render prints the second
            # after the one that was drawn.
            now = time.localtime()
            return time.strftime("%H:%M:%S", now), render_clock(now)
    else:
        cycle = [a.colour] if a.colour else ["white", "red", "green", "blue", "black"]

        def render(n):
            name = cycle[n % len(cycle)]
            return name, render_solid(COLOURS[name])

    if a.once:
        mm[:] = render(0)[1]
        return 0

    n = 0
    while True:
        t = time.time()
        name, buf = render(n)
        mm[:] = buf
        print("fbscreen: %s" % name, flush=True)
        n += 1
        # Sleep to the next second boundary, so a clock's seconds digit ticks
        # cleanly instead of drifting a little later every frame.
        time.sleep(max(0.02, 1.0 - (t % 1.0)))


if __name__ == "__main__":
    sys.exit(main())
