#!/usr/bin/env python3
"""bake_menu_font.py -- turn the device's own typeface into menu_font.h.

WHERE THIS LIVES, AND WHY IT IS NOT IN work/. work/ is scratch -- it is gitignored
and holds the drills, the instruments and the one-off probes, all of which the docs
cite as a record rather than as a build input. This script is the opposite: its
output, menu_font.h, is COMMITTED, and the header it writes names it as its source.
A generated file whose generator lives in an untracked directory is a file nobody
can regenerate, so the generator sits beside the header it writes, as
scripts/patch-rbp-nopc.py sits beside the patcher. Running it needs Python, Pillow
and the device's ttf, none of which a BUILD needs -- a build of the shim reads the
committed header and never opens a font file.

WHY A BAKED ATLAS AND NOT A SCALED BITMAP. The panel's first labels were a
hand-written 5x7 table drawn at an integer scale, which on a 96-px button meant
scale 4: every source pixel replicated as a 4x4 block. The operator's verdict on
the glass was "too big and pixely". Decker is the typeface the XDJ-RX3 carries in
its own font directory -- and it is the ONLY Latin font file on the device, since
gui/system/fontdata holds just decker.ttf and the Japanese sazanami-gothic.ttf --
so the panel's labels can be drawn as text instead of in a grid of blocks. That
needs a rasteriser at build time, which is exactly what this script is: it runs
once, on a machine, and its output is committed. The shim itself still has no font
engine, no freetype and no file I/O -- it gets a table.

    python3 scripts/shims/bake_menu_font.py [px] [light]

The output is scripts/shims/menu_font.h: one 8-bit coverage bitmap and a
per-glyph metrics table. `px` defaults to 19, which is the operator's choice.

THE ATLAS IS THINNED, AND THAT IS NOT A STYLE CHOICE. Asked on 2026-10-06 for "a
font that resembles the out of the box RX3 font", what the operator is looking at
is WEIGHT: the player's own text is light and the panel's was Decker Bold, and
Decker on this device is bold and nothing else (usWeightClass 700, one static
face, no fvar -- and there is no second file to switch to). rbp does not draw with
decker.ttf either; its Latin glyphs come from Pioneer's own baked tables
(gui/pset/fontdata/NS_FONT_ID_*.bin), and those decode -- 27 rows x 7 bytes x 422
cells, cell k = character 0x20+k -- but they are a LIGHT face and baking them
would have changed every label's width.
**THEY ARE 2 BITS PER PIXEL, NOT 1** (corrected 2026-10-06): each byte is four
0..3 coverage samples, so a row is 28 px, not 56. Reading them as 1 bpp stretches
every glyph 2x sideways and makes the face look far wider and coarser than it is --
that misreading is where "a WIDE face, about 2.5x" came from, and it is wrong.
Measured at 2 bpp: cap 19 rows, '0' is 12 px wide, 'I' is a 4 px stem, "USB STOP"
spans 98 px against this atlas's 76 at 19 px. So the table is real and usable
(../full-touch-ui-xdj-rx3 draws its labels from it at run time, area-averaging it
down with a 1.35x coverage boost to hold the stem weight), but it is ~1.3x wider
than Decker at the same cap, and the operator asked for a lighter Decker, not for
rbp's own face. What is left is to
take Decker down in weight, which is what `light` does: the outlines are thinned
by that many SUPERSAMPLED pixels on every side, so the default 1 of 6 is a sixth
of a pixel on each side -- about a fifth less ink over the set. The metrics are
untouched by it -- only the coverage table changes -- so a label keeps its
advance, its position and its box. 2 of 6 is measurably lighter still and the
shipped atlas is the one command below away from it, but it thins the keyboard's
`.` `/` `-` `_` `:` `=` to nothing solid and drops the capital `I` under the
solid-cell bound test_menu.c relies on, so the default stops at 1.

    python3 scripts/shims/bake_menu_font.py 16 0     # the unmodified bold

WHAT SIZE IS rbp'S OWN TEXT, MEASURED. The `INFO` label in rbp's top bar is 42 px
wide by 13 rows of ink (x 1217..1258, rows 18..30 of its control), and Decker's
capitals reach 12 rows at 17 px, 13 at 18 px and 14 at 19 px. So the shipped 19 px
is a row over the player's own labels, and 16 px (11 rows) was a row under them --
which is what the operator was looking at on 2026-10-06 when they asked to "size up
the font but keep the size of the bar the same".

WHY 19 IS THE CEILING AND NOT AN ARBITRARY PICK. The bar is fixed at 56 rows
(menu_zone.h's MZ_PANEL_H) and 8 of those are the frame at each end, so the button
band the labels live in is 40 rows at 1280x800. Two pinned properties bound the
size from below that:

  * the HEIGHT. menu_paint.c refuses a picture whose button band cannot host the
    line box (ascent + descent), so an atlas is only usable on every panel this port
    supports while its line box fits the SMALLEST supported band. The 480-row panels
    (640x480, 800x480) give a 24-row band, and the line box is 24 rows at 19 px and
    25 at 20 -- so 19 px is the last size at which the width of the font does not
    cost a panel a band it draws today. Measured across the ladder:

        px    16  17  18  19  20  22  24  26  28
        line  20  22  23  24  25  28  30  33  34

  * the WIDTH, which the size does cost: menu_paint.c also refuses a picture whose
    NARROWEST column cannot hold the widest label whole, because the painter clips at
    the column bound and a clipped word is worse than no band. "USB STOP" is 65 px at
    16 px and 76 at 19, and a 480-px-wide picture gives 68-px columns -- so 480-wide
    stops drawing the band at this size, while 640 wide (91 px columns, and 800 wide's
    114) keeps it. That is the one panel shape the size-up spends, and it is a shape
    no display here has; the refusal is logged, not silent.

Going further is a decision about which small displays to drop rather than a font
choice, which is why the default stops here. One command changes it either way:

    python3 scripts/shims/bake_menu_font.py 19

WHY 8-BIT COVERAGE AND NOT 1-BIT INK. Because it is the only way to get the
antialiasing that the operator asked for, and because it costs almost nothing:
9.7 KB for the whole 73-glyph set at 19 px, and it scales with the square of the
size, so about 21 KB at the top of the ladder above -- 37 of those glyphs are the
panel's labels, which is what the first version of this file baked, and 36 more were
added on 2026-10-04 for the browser window's URL bar and popup keyboard. The painter
blends each covered pixel against the button colour; coverage 0 and coverage 255
write *exactly* the background and label colours, so the panel's idempotence and
conditional-restore rules (menu_paint.h) survive the change untouched.

The ttf is NOT in the repository -- extracted/ is gitignored -- and it does not
need to be: this script is a work tool, and menu_font.h is the artifact. A build
of the shim never runs Python and never opens a font file.

NOTE ON REPRODUCIBILITY. The rendered bytes depend on the FreeType version doing
the rasterising, so this script is not bit-reproducible across machines. That is
fine and deliberate: the generated header is committed and reviewed, exactly like
the table it replaces, and the shim's build never regenerates it.
"""
import os
import sys

from PIL import Image, ImageChops, ImageFont

# Paths are the repository's, resolved from this file's own location, so the script
# runs from any working directory: <root>/scripts/shims/bake_menu_font.py.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TTF = os.path.join(ROOT, "extracted/XDJRX3/gui/fontdata/decker.ttf")
OUT = os.path.join(ROOT, "scripts/shims/menu_font.h")
# What the generated header says it came from -- relative to the repository, since
# that is how a reader of the header will look for it.
HERE = "scripts/shims/bake_menu_font.py"
DEFAULT_PX = 19

# HOW THE ATLAS IS THINNED, and why it is done this way. The device's only Latin
# face is bold, so a lighter cut has to be synthesised: each glyph is scaled up by
# SUPERSAMPLE, eroded by LIGHT_STEPS whole pixels THERE (which is a fraction of a
# pixel here, so the thinning is sub-pixel), and box-filtered back down. Eroding
# the 1px mask directly would not work at all -- Decker's stems at 19 px are two
# or three pixels wide, so one pixel off either side erases the glyph -- and
# ImageFilter.MinFilter would not work either: its window is SQUARE, so it eats
# diagonals about twice as fast as it eats straights (measured on the 'O': 308
# cells left after MinFilter(7) against 634 after three cross erosions at the same
# radius), which makes a round letter thin quicker than a straight one for no
# reason a reader can see. A cross is what a lighter weight wants: every edge
# moves in by the same amount.
SUPERSAMPLE = 6
LIGHT_STEPS = 1
DEFAULT_LIGHT = LIGHT_STEPS

# space, then '0'-'9', then 'A'-'Z' -- the same set, in the same order, as the 5x7
# table this replaces, so menu_font_index() keeps its meaning and every existing
# label keeps working. A label with a character outside this set draws a gap (see
# menu_font_index in the generated header), which is what the old table did too.
#
# THE SET WAS EXTENDED ONCE, ON 2026-10-04, AND ONLY BY APPENDING. Lowercase and
# ten punctuation marks went on the END, for the browser window's URL bar and
# its popup keyboard (menu_keyboard.h): everything the panel already draws keeps
# the index it had, so the seven menu labels are byte-for-byte the glyphs they
# were -- verified by re-baking the old set on the machine that extended it and
# diffing the header, which came back identical. That is the property to preserve
# if this ever grows again, and it is why the punctuation is in this order and not
# in ASCII order.
PUNCT = "./-_:?=&@#"

CHARS = (" " + "0123456789" + "ABCDEFGHIJKLMNOPQRSTUVWXYZ" +
         "abcdefghijklmnopqrstuvwxyz" + PUNCT)

# The keyboard's own alphabet, which is what the URL bar and any page input are
# typed with, as opposed to the panel's labels above. It is checked against CHARS
# below, so a key that would draw a gap is a failed bake rather than a keyboard
# with a dead key on it.
KEY_ALPHABET = ("0123456789" "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz" + PUNCT)

# The panel's labels and their column, for the report below only -- the shipped
# geometry is menu_zone.c's. They were six columns of 213 px for as long as the
# panel had six; the USB STOP chooser made it seven (menu_zone.h's MZ_COLS) and
# this printout kept saying six, which is exactly the kind of stale number that
# gets quoted back as a measurement.
LABELS = ["SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH", "MENU", "USB STOP"]
COL_W = 1280 // 7


def erode(im, steps):
    """`steps` pixels of four-neighbour erosion, in PIL alone.

    Each step is the minimum of the image with itself shifted one pixel each way,
    and ImageChops.darker is that minimum. A shifted paste leaves the vacated edge
    at zero, which is what an erosion wants -- the glyph's ink box is tight, so
    there is nothing beyond its edge to hold it up. (`MinFilter` would be one call
    instead of four, and it is the wrong shape: see SUPERSAMPLE above.)
    """
    for _ in range(steps):
        w, h = im.size
        out = im.copy()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            sh = Image.new("L", (w, h), 0)
            sh.paste(im, (dx, dy))
            out = ImageChops.darker(out, sh)
        im = out
    return im


def lighten(mask, size, steps):
    """The same glyph, `steps` supersampled pixels thinner on every side.

    THE BOX DOES NOT MOVE. The array that comes back is exactly `size` bytes, the
    same box the unthinned rasteriser produced, because the thinning happens in the
    supersampled copy and is only filtered back down afterwards. Nothing here can
    change an advance or a `left`/`top`, so a bake that differs only in `steps`
    differs only inside the coverage table -- which is the property that makes this
    change reviewable, and the reason the metrics are read from `font` at 1x and
    never from the scaled copy (the supersampled font's own bbox does NOT agree
    with the 1x box scaled up: at 96 px Decker's 'A' is 63 px of ink where six
    copies of its 11-px box say 66, so aligning on it would shift every glyph).

    The outermost pixels of the box are allowed to fall to zero, and that is the
    point -- a glyph does not grow a lighter edge by keeping one.
    """
    w, h = size
    if steps <= 0:
        return mask
    base = Image.frombytes("L", size, mask)
    big = base.resize((w * SUPERSAMPLE, h * SUPERSAMPLE), Image.BILINEAR)
    big = big.point(lambda v: 255 if v > 127 else 0)   # a threshold, not a tint:
    big = erode(big, steps)                            # erosion is a shape operation
    return big.resize(size, Image.BOX).tobytes()


def bake(px, light=DEFAULT_LIGHT):
    font = ImageFont.truetype(TTF, px)
    ascent, descent = font.getmetrics()
    line = ascent + descent

    glyphs = []
    cov = bytearray()
    for ch in CHARS:
        bbox = [int(v) for v in font.getbbox(ch)]
        adv = int(round(font.getlength(ch)))
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        if w <= 0 or h <= 0:
            glyphs.append((adv, 0, 0, 0, 0, len(cov)))
            continue
        # getmask()/getbbox() agree: the mask is exactly the ink box, and Pillow's
        # bbox y is already relative to the top of the line box (the ascender), so
        # `top` needs no further correction. bytes(mask) is the grayscale coverage;
        # the ImagingCore has no tobytes() in this Pillow.
        mask = font.getmask(ch, mode="L")
        assert mask.size == (w, h), (ch, mask.size, (w, h))
        data = lighten(bytes(mask), (w, h), light)
        assert len(data) == w * h, (ch, len(data), w * h)
        cov += data
        glyphs.append((adv, bbox[0], bbox[1], w, h, len(cov) - len(data)))
    return ascent, descent, line, glyphs, bytes(cov)


def hexdump(data, per_line=16):
    out = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        out.append("    " + " ".join("0x%02x," % b for b in chunk))
    return "\n".join(out)


def index_source():
    """The generated lookup, built FROM CHARS so the map and the table cannot drift.

    Three contiguous runs are ranges and the rest is a switch -- which is the shape
    the hand-written version had, and it is worth keeping the ranges because they
    read as what they are. What is new is that the offsets are computed here rather
    than typed: appending to CHARS moves nothing that comes before it, and a run
    inserted in the middle would change the numbers below without anyone editing
    them, which is the failure this function exists to make impossible.
    """
    lines = []
    lines.append("static inline int menu_font_index(unsigned char c)")
    lines.append("{")
    lines.append("    if (c == ' ')")
    lines.append("        return 0;")
    for lo, hi in (("0", "9"), ("A", "Z"), ("a", "z")):
        lines.append("    if (c >= '%s' && c <= '%s')" % (lo, hi))
        lines.append("        return %d + (c - '%s');" % (CHARS.index(lo), lo))
    lines.append("    switch (c) {")
    for i, ch in enumerate(CHARS):
        if ch.isalnum() or ch == " ":
            continue
        lines.append("    case '%s':" % ("\\'" if ch == "'" else ch))
        lines.append("        return %d;" % i)
    lines.append("    }")
    lines.append("    return -1;")
    lines.append("}")
    return "\n".join(lines)


def check_alphabet():
    """Every key the popup keyboard can draw must be a glyph in the atlas."""
    missing = [c for c in KEY_ALPHABET if c not in CHARS]
    if missing:
        sys.exit("KEY_ALPHABET names %r, which CHARS does not carry -- those keys "
                 "would draw a gap" % ("".join(missing),))
    if len(set(CHARS)) != len(CHARS):
        sys.exit("CHARS has a duplicate in it")
    for c in "abcdefghijklmnopqrstuvwxyz":
        if CHARS.index(c) != CHARS.index("a") + ord(c) - ord("a"):
            sys.exit("the lowercase run is not contiguous")


def main():
    px = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PX
    light = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_LIGHT
    if not 0 <= light * 2 < SUPERSAMPLE:
        sys.exit("light %d of %d would thin a %d px glyph by more than half a pixel "
                 "on each side, which stops being a lighter weight and starts being "
                 "a broken one (at 3 of 6 the '-' and '=' rasterise to nothing)"
                 % (light, SUPERSAMPLE, px))
    if not os.path.exists(TTF):
        sys.exit("%s is missing -- it is gitignored, so it exists only in a "
                 "checkout that has the firmware extracted" % TTF)
    check_alphabet()

    ascent, descent, line, glyphs, cov = bake(px, light)

    widths = {}
    for lb in LABELS:
        widths[lb] = sum(glyphs[CHARS.index(c)][0] for c in lb)
    # THE BAND, not the tallest glyph. Those were the same number for as long as the
    # set was capitals, digits and a space -- every glyph had the same 11-row ink box,
    # so the tallest ink was the band and INK_H, INK_TOP and INK_BOT were three ways
    # of saying one thing. Lowercase brought descenders (the underscore reaches four
    # rows under the baseline, 'i' and 'j' four rows above the capitals) and the two
    # came apart: no single glyph spans rows 4..19, so INK_H as "the tallest ink"
    # would be a number that describes nothing the layout can use. The band is what
    # the name promises and what the witness needs, so the band is what it is, and the
    # tallest glyph is only asserted to fit inside it.
    ink_top = min(g[2] for g in glyphs if g[4])
    inkbot = max(g[2] + g[4] for g in glyphs) - 1
    ink_h = inkbot - ink_top + 1
    assert max(g[4] for g in glyphs) <= ink_h, (ink_top, inkbot, ink_h)
    # The CAPITALS' own box, which is what a label is and what rbp's own text is
    # measured against above -- the set-wide band is as tall as its descender and its
    # ascender put together, and quoting that as "the size" would compare the labels
    # to the player's caps across two different things.
    cap_top = min(glyphs[CHARS.index(c)][2] for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    cap_bot = max(glyphs[CHARS.index(c)][2] + glyphs[CHARS.index(c)][4]
                  for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ") - 1

    body = "\n".join("    { %2d, %3d, %3d, %2d, %2d, %5d }," % g for g in glyphs)

    if light:
        light_desc = ("each glyph's outline is eroded by %d/%d of a pixel on every "
                      "side (that is %d of %d supersampled pixels), which takes the "
                      "face from bold to a lighter weight without moving a single "
                      "metric" % (light, SUPERSAMPLE, light, SUPERSAMPLE))
    else:
        light_desc = ("this bake is NOT thinned: it is Decker Bold exactly as the "
                      "device's file rasterises")

    light_word = ("thinned %d/%d px a side" % (light, SUPERSAMPLE)) if light \
        else "unthinned (bold)"
    src = '''/*
 * menu_font.h -- the panel's labels, in the device's own typeface. GENERATED.
 *
 * Do not edit this file. It is written by %(here)s, which rasterises
 * extracted/XDJRX3/gui/fontdata/decker.ttf -- the only Latin font file the XDJ-RX3
 * carries (gui/system/fontdata holds it and the Japanese sazanami-gothic.ttf and
 * nothing else) -- into an 8-bit coverage atlas at %(px)d px, %(light)s. To change
 * the size, the weight or the character set, run
 *
 *     python3 %(here)s %(px)d %(light_steps)d
 *
 * WHY THIS REPLACED A 5x7 BITMAP. The first version of the panel's labels was a
 * hand-written 5x7 table drawn at an integer scale; on a 96-px button that meant
 * MENU_SCALE_MAX, i.e. every source pixel replicated as a 4x4 block, 28 rows of
 * blocks for 13 rows of letters. The operator put a finger on it and said the font
 * was "too big and pixely". At %(px)d px this ink is %(ink_h)d rows tall, inside a
 * %(line)d-row line box (ink rows %(ink_top)d..%(inkbot)d of it).
 *
 * WHY IT IS THINNED, which is the one thing here that is not simply the device's
 * font. Decker ships BOLD and only bold, and the operator asked on 2026-10-06 for
 * "a font that resembles the out of the box RX3 font" -- which is a question about
 * WEIGHT, because the player's own text is light. rbp does not draw with this file
 * either: its Latin glyphs come from Pioneer's baked tables,
 * gui/pset/fontdata/NS_FONT_ID_*.bin (27 rows x 7 bytes x 422 cells, **2 bits per
 * pixel -- 28 px per row of 0..3 coverage, NOT 1 bpp**, cell k = character
 * 0x20+k), and those are a LIGHT face: at the table's own 19 px cap the '0' is
 * 12 px wide and the 'I' is a 4 px stem. Reading the table as 1 bpp stretches
 * every glyph 2x sideways and is where an earlier note here got "a WIDE face,
 * about 2.5x" from -- it is wrong. Measured correctly, "USB STOP" spans 98 px
 * against this atlas's 76, so a bake would be ~1.3x wider as well as lighter:
 * usable, and not what the operator asked for. What is left is to thin Decker:
 * %(light_desc)s.
 *
 * WHAT THE THINNING CANNOT TOUCH. Every metric below -- advance, left, top, w, h
 * and the whole MENU_FONT_INK_* band -- is read from the font at 1x and is
 * IDENTICAL to an unthinned bake of the same size. Re-baking with `0` as the
 * second argument changes bytes inside menu_font_coverage[] and nothing else, so
 * every label keeps its width, its centring and its vertical position.
 *
 * HOW THE SIZE COMPARES TO rbp'S OWN TEXT, measured: rbp's own `INFO` label has 13
 * rows of ink, and these capitals have %(cap_h)d. The size is the operator's choice
 * -- they asked on 2026-10-06 to "size up the font but keep the size of the bar the
 * same" -- and one command changes it:
 *
 *     python3 %(here)s %(px)d
 *
 * The bar is not this file's to change: it is 56 rows (menu_zone.h's MZ_PANEL_H), 8
 * of frame at each end and so a 40-row button band at 1280x800, and the label's line
 * box has to fit that band on the SMALLEST picture the port supports. That is what
 * stops the ladder -- the line box is %(line)d rows here, 24 at 19 px and 25 at 20,
 * and the 480-row panels (640x480, 800x480) give a 24-row band. A larger atlas is a
 * smaller supported panel, not a bigger bar.
 *
 * WHY 8-BIT COVERAGE. Antialiasing is the other half of "pixely", and it costs
 * %(covlen)d bytes for all %(nglyph)d glyphs. Coverage 0 and coverage 255 are exact --
 * a pixel outside a glyph is the button colour and a fully covered one is the label
 * colour -- so menu_paint.h's "painting it twice is painting it once" and
 * menu_draw.c's conditional restore both still hold, pixel for pixel. Only the
 * glyph edges blend, and they blend against the button colour the painter already
 * knows.
 *
 * THE SET is space, '0'-'9', 'A'-'Z', 'a'-'z' and %(punct)s -- with the first
 * three in the same order as the table this replaced, so menu_font_index() kept its
 * meaning and every existing label kept working. Lowercase and the punctuation were
 * APPENDED, for the browser window's URL bar and its popup keyboard, and appending
 * is the rule: everything the panel already draws keeps the index it had. A
 * character outside the set draws a gap of the space advance's width rather than
 * garbage, which is also what the old table did.
 *
 * THE SET IS ALSO THE KEYBOARD'S. menu_keyboard.h's keys name characters from it,
 * and bake_menu_font.py's KEY_ALPHABET is checked against this same string at bake
 * time -- so a key that would draw a gap fails the bake instead of shipping as a
 * dead key.
 *
 * THE LAYOUT IS THE FONT'S OWN METRICS, not a fixed cell. Each glyph carries its
 * pen advance and the position of its ink box relative to the pen origin and to the
 * line box top, so a label is centred by summing advances (menu_text_width) and is
 * placed vertically from the line box (MENU_FONT_LINE). Nothing is scaled: there is
 * no scale parameter anywhere in this interface, because a scaled bitmap is what
 * this file exists to stop doing.
 */
#ifndef RBPI4B_MENU_FONT_H
#define RBPI4B_MENU_FONT_H

#include <stddef.h>          /* size_t, for the table walk below */

#define MENU_FONT_PX      %(px)d    /* the pixel size this atlas was baked at */
#define MENU_FONT_ASCENT  %(ascent)d   /* above the baseline, from the ttf     */
#define MENU_FONT_DESCENT %(descent)d    /* below it                             */
#define MENU_FONT_LINE    (MENU_FONT_ASCENT + MENU_FONT_DESCENT)

/* space, '0'-'9', 'A'-'Z', 'a'-'z', then the punctuation -- in that order. See
 * menu_font_index() for the indices; the last run is listed there one case at a
 * time because it is not a contiguous ASCII range. */
#define MENU_FONT_GLYPHS  %(nglyph)d

/* The punctuation run, in index order, appended after the lowercase -- the marks a
 * URL needs and a label never did. menu_keyboard.h's bottom row is built from it, so
 * the keyboard and the atlas name the same characters rather than two lists that
 * agree today; test_menu.c uses it to place the runs. */
#define MENU_FONT_PUNCT   "%(punct)s"

/* The band the whole set's ink can occupy: rows MENU_FONT_INK_TOP ..
 * MENU_FONT_INK_BOT inclusive, MENU_FONT_INK_H rows tall. It is the union over the
 * glyphs and not one glyph's box -- a descender sets the bottom and the dot of an
 * 'i' sets the top, and no single glyph touches both. It runs past MENU_FONT_ASCENT
 * now that the set carries descenders, and still ends inside MENU_FONT_LINE.
 *
 * test_menu.c asserts these against the table, and the witness in menu_draw.c
 * depends on the labels' ink being where the layout puts it -- it samples 15 points
 * that must not be ink. */
#define MENU_FONT_INK_H    %(ink_h)d
#define MENU_FONT_INK_TOP  %(ink_top)d
#define MENU_FONT_INK_BOT  %(inkbot)d

/* One glyph's metrics. `left`/`top` are the ink box's origin relative to the pen
 * origin (left) and to the line box top (top), and `off` indexes the coverage
 * bitmap, which holds `w * h` bytes row-major. A glyph with no ink (the space) has
 * w and h zero and an offset that is never read. */
struct menu_glyph {
    unsigned char adv;      /* pen advance in pixels */
    signed char   left;     /* ink box left, from the pen origin  */
    signed char   top;      /* ink box top, from the line box top */
    unsigned char w, h;     /* ink box size in pixels            */
    unsigned short off;     /* byte offset into menu_font_coverage[] */
};

static const struct menu_glyph menu_font_glyphs[MENU_FONT_GLYPHS] = {
%(body)s
};

static const unsigned char menu_font_coverage[] = {
%(cov)s
};

/* Where a character lives in the table, or -1 when this font has no glyph for it.
 * Unsupported characters are not an error: the painter draws them as a gap of the
 * space advance's width, so an unexpected character in a label is a gap rather
 * than a smear. */
%(index)s

/* The glyph for a character, or the space when the font has none -- so a caller
 * never has to ask twice, and a gap is always the same width as a space. */
static inline const struct menu_glyph *menu_font_glyph(unsigned char c)
{
    int idx = menu_font_index(c);

    return &menu_font_glyphs[idx < 0 ? 0 : idx];
}

/* Coverage of one cell of one glyph, 0..255, relative to the glyph's ink box.
 * Out of range is 0, never a read past the bitmap -- the same "ask a question with
 * an answer" shape as menu_font_cell() in the table this replaced. */
static inline int menu_font_cov(const struct menu_glyph *g, int col, int row)
{
    if (!g || col < 0 || row < 0 || col >= (int)g->w || row >= (int)g->h)
        return 0;
    return menu_font_coverage[g->off + (size_t)row * g->w + col];
}

/* The width in pixels of `s`: the sum of the advances, with no trailing space,
 * because centring a label is the only caller and it needs this exact. The old
 * interface took a scale; there is no scale here by design (see the header). */
static inline int menu_text_width(const char *s)
{
    int w = 0;

    if (!s)
        return 0;
    while (*s) {
        w += menu_font_glyph((unsigned char)*s)->adv;
        s++;
    }
    return w;
}

/* The check the generator cannot make and a reviewer should not have to: every ink
 * box really is inside its own coverage run, every advance is positive, and the
 * bitmap is exactly as long as the metrics say. Returns 1 when the table is well
 * formed; test_menu.c fails on 0, and production never calls it (it would only be
 * re-checking a compile-time constant). */
static inline int menu_font_selfcheck(void)
{
    size_t end = 0;
    int g;

    for (g = 0; g < MENU_FONT_GLYPHS; g++) {
        const struct menu_glyph *p = &menu_font_glyphs[g];

        if (p->adv < 1)
            return 0;
        if ((size_t)p->off + (size_t)p->w * p->h > sizeof menu_font_coverage)
            return 0;
        if (p->off < end && p->w * p->h != 0)
            return 0;                       /* overlapping ink runs */
        end = (size_t)p->off + (size_t)p->w * p->h;
    }
    return end == sizeof menu_font_coverage;
}

#endif /* RBPI4B_MENU_FONT_H */
''' % dict(here=HERE, px=px, ascent=ascent, descent=descent, nglyph=len(CHARS),
           ink_h=ink_h, ink_top=ink_top, inkbot=inkbot, line=line, body=body,
           cap_h=cap_bot - cap_top + 1,
           cov=hexdump(cov), covlen=len(cov), index=index_source(),
           punct=PUNCT, light=light_word, light_steps=light, light_desc=light_desc)

    with open(OUT, "w") as f:
        f.write(src)

    print("wrote %s: %d glyphs at %d px, coverage %d bytes (%.1f KB of source)"
          % (OUT, len(CHARS), px, len(cov), len(src) / 1024.0))
    print("  weight: %s" % light_word)
    print("  ascent %d descent %d line %d; ink rows %d..%d (%d tall), capitals"
          " %d rows" % (ascent, descent, line, ink_top, inkbot, ink_h,
                        cap_bot - cap_top + 1))
    print("  advances %d..%d" % (min(g[0] for g in glyphs),
                                 max(g[0] for g in glyphs)))
    print("  %-9s %5s %5s   (column %d px)" % ("label", "width", "slack", COL_W))
    for lb in sorted(LABELS, key=lambda s: -widths[s]):
        print("  %-9s %5d %5d" % (lb, widths[lb], COL_W - widths[lb]))


if __name__ == "__main__":
    main()
