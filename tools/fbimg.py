#!/usr/bin/env python3
"""Framebuffer frame decoding: raw RGB565 -> BMP -> PNG.

One encoder, used by both `work/fbshot.py` (a single live frame) and
`tools/fbwatch.py` (a burst around an operator action), so there is one place
where the pixel format is written down.

BMP is deliberate, not laziness: an earlier hand-rolled PNG encoder in this
project silently swapped R and B and made whole screens read as black, which
cost a debugging session. BMP is trivial and `sips` reads it exactly.

The unit's page is 1280x800 RGB565 at stride 2560 -- two bytes per pixel with
no row padding -- and BMP rows run bottom-up with BGR pixels.
"""
import struct
import subprocess

W, H, STRIDE = 1280, 800, 2560
FRAME = STRIDE * H


def decode(fb):
    """A raw frame -> a bottom-up BGR24 body."""
    if len(fb) < FRAME:
        raise ValueError("short frame: %d bytes, wanted %d" % (len(fb), FRAME))
    rows = []
    for y in range(H - 1, -1, -1):
        off = y * STRIDE
        row = bytearray()
        for x in range(W):
            px = fb[off + 2 * x] | (fb[off + 2 * x + 1] << 8)
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            row += bytes(((b << 3) | (b >> 2),
                          (g << 2) | (g >> 4),
                          (r << 3) | (r >> 2)))
        rows.append(bytes(row))
    return b"".join(rows)


def write_bmp(fb, path):
    body = decode(fb)
    hdr = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(body),
                      2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 14 + len(hdr) + len(body), 0, 0, 54))
        f.write(hdr)
        f.write(body)
    return path


def to_png(bmp, png=None):
    png = png or bmp.rsplit(".", 1)[0] + ".png"
    subprocess.run(["sips", "-s", "format", "png", bmp, "--out", png],
                   capture_output=True, check=True)
    return png
