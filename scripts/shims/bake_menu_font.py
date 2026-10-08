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


# ---------------------------------------------------------------------------------
# THE OTHER FACE: Pioneer's own baked table, NS_FONT_ID_18030_JH_w.bin.
#
# This is the face the shipped atlas is baked from as of 2026-10-07. It is not a ttf
# at all -- it is a fixed-cell bitmap dump, the same kind of thing rbp itself draws
# its Latin from, and it is here because the operator, shown both faces on the glass
# in seven strips, took this one. Two things about it are not obvious and both were
# measured rather than assumed:
#
#   * 2 BITS PER PIXEL. A row is 6 bytes = 24 px of four 0..3 coverage samples per
#     byte, not 48 px of ink. Reading it as 1 bpp is where an earlier note got a
#     "WIDE face, about 2.5x" from; at 2 bpp it is a normal-width face.
#   * THE ASCII RUN IS AT cell = ord(c) + 155, and it is NOT a copy of ISO8859's.
#     It is consistently wider, and against rbp's own large text (a live "DELAY" at a
#     32-row cap, D 26 E 22 L 20 A 26 Y 28) it predicts widths with a mean error of
#     1.9 px where ISO8859_w is off by 2.8 -- so the letterforms rbp shows big look
#     like this table's. ../full-touch-ui-xdj-rx3 renders its labels from it at run
#     time, area-averaging each cell down with a 1.35x coverage boost to hold the
#     stem weight, and that recipe is what JH_WEIGHT below is.
#
# THE ALIGNMENT RULE, which is the one thing a re-bake must not get wrong: the cells
# of a baked table share ONE row grid, and that grid IS the font's vertical alignment.
# The whole cell is scaled and placed at a fixed origin; each glyph's ink is never
# re-aligned to a common top or bottom. Doing it the other way pushes every round
# letter below the baseline, and on the glass that reads as "the alignment is all
# janky" rather than as a bug in a table.
JH_BIN = os.path.join(ROOT, "extracted_v120/XDJRX3-gui/pset/fontdata/"
                            "NS_FONT_ID_18030_JH_w.bin")
JH_ROWS, JH_BPR = 23, 6              # 23 rows x 6 bytes x 4 samples = 24 px per row
JH_FIRST = 155                       # cell k = character ord(k) + JH_FIRST
JH_NATIVE_BASELINE = 20              # the cell row the flat capitals sit on
JH_WEIGHT = 1.35                     # coverage boost, as the sibling port uses

# The line-box row a capital's ink ends on, in the shipped Decker bake: MENU_FONT_LINE
# is 24, its ink band is 5..22 (the 22 is a descender) and its capitals are 5..18. The
# JH bake pins its own capitals to that same row, so both faces sit identically in the
# button and the ink band stays inside the 24-row line box the height floor depends on.
CAP_BOTTOM_ROW = 18

# What "fitted" means, and why it is a fraction of 19 rather than a row count. The
# band's line box is 24 rows and its CAP HEIGHT is what has to match: Decker's
# capitals are 14 rows at 19 px, and the operator took the JH face AT THAT HEIGHT.
# So the scale is 14/19 of the native cell -- both dimensions, both rounded the same
# way the preview instrument rounds them, because a bake that rounded differently
# would not be the picture that was approved.
FIT_ROWS = 14
FIT = FIT_ROWS / 19.0
JH_T = max(1, int(round(JH_ROWS * FIT)))          # 17 scaled cell rows
JH_W = max(1, int(round(JH_BPR * 4 * FIT)))       # 18 scaled cell px

# rbp's OWN SPACING, measured off its live text and held at both sizes: "CH SELECT"
# and "BEAT FX" are both 20-row caps and both give a uniform 2 px ink-to-ink gap,
# and H->S puts the word space's own advance at 8 px. rbp draws the ink box flush at
# the pen, so a glyph's advance is its ink width plus this one constant.
ADV_GAP = 2
NATIVE_WORD_SPACE = 8

# THE OPTICAL KERN. A uniform ink-BOX gap is not a uniform optical gap: the boxes sit
# exactly ADV_GAP apart, but the ink inside them recedes by different amounts, and the
# eye integrates the wedge. Measured across the fitted JH cells at that 2 px box gap,
# the MEAN per-row gap of a pair runs from 1.9 ("UR": two full-height stems) to 13.7
# ("LT": L's stem against T's arm) -- a 7x spread, which is what reads as "the kerning
# is off". The rule below holds the mean near KERN_TARGET instead, with the gap
# floored so ink boxes can never overlap and capped so a pair that shares no rows is
# not pushed apart. It is derived from the glyph profiles, not a hand-tuned pair list.
KERN_TARGET = 5.0                    # measured on the SCALED cells, not the native ones
KERN_LO, KERN_HI = 1, 4


def jh_cells():
    """The whole baked table, as a list of cells: 23 rows x 24 px of 0..255 coverage."""
    data = open(JH_BIN, "rb").read()
    size = JH_ROWS * JH_BPR
    if len(data) % size:
        sys.exit("%s is %d bytes, which is not a whole number of %d-byte cells"
                 % (JH_BIN, len(data), size))
    out = []
    for k in range(len(data) // size):
        base = k * size
        out.append([[(data[base + y * JH_BPR + x // 4] >> (6 - 2 * (x % 4)) & 3) * 85
                     for x in range(JH_BPR * 4)] for y in range(JH_ROWS)])
    return out


def scale_cell(cell, weight=JH_WEIGHT):
    """One cell, area-averaged to JH_T x JH_W -- grid, baseline and overshoot together.

    The coverage is boosted by `weight` (clamped at 255) and anything under 8 is
    dropped to zero, so a shrunk stroke keeps its weight instead of dissolving into
    a grey haze. Scaling the WHOLE cell is the point: the alignment rides along with
    it, which is what keeps every glyph on the one row grid.
    """
    R, C = JH_ROWS, JH_BPR * 4
    out = []
    for t in range(JH_T):
        r0 = (t * R) // JH_T
        r1 = max(r0 + 1, ((t + 1) * R) // JH_T)
        line = []
        for x in range(JH_W):
            c0 = (x * C) // JH_W
            c1 = max(c0 + 1, ((x + 1) * C) // JH_W)
            acc = cnt = 0
            for yy in range(r0, min(r1, R)):
                for xx in range(c0, min(c1, C)):
                    acc += cell[yy][xx]
                    cnt += 1
            v = min(255.0, (acc / float(cnt)) * weight) if cnt else 0.0
            line.append(int(round(v)) if v >= 8.0 else 0)
        out.append(line)
    return out


def ink_box(cell):
    p = [(x, y) for y in range(len(cell)) for x in range(len(cell[0])) if cell[y][x]]
    if not p:
        return None
    return (min(x for x, _ in p), min(y for _, y in p),
            max(x for x, _ in p), max(y for _, y in p))


def row_edges(cell):
    """Per-row ink edges relative to the glyph's own ink box: {row: (left, right)}."""
    box = ink_box(cell)
    if box is None:
        return {}
    x0 = box[0]
    out = {}
    for y in range(len(cell)):
        cols = [x for x in range(len(cell[0])) if cell[y][x]]
        if cols:
            out[y] = (min(cols) - x0, max(cols) - x0)
    return out


def mean_gap(ea, eb, g):
    """The MEAN per-row ink gap for a pair at a box gap of g px -- what the eye
    integrates, as against the bounding-box gap, which is g for every pair alike."""
    if not ea or not eb:
        return 0.0
    wa = max(r for _, r in ea.values()) + 1
    tot = n = 0
    for y, (_, ra) in ea.items():
        if y in eb:
            tot += wa + g + eb[y][0] - ra - 1
            n += 1
    return tot / float(n) if n else 0.0


def kern_delta(ea, eb):
    """The adjustment this pair's advance carries, in px, on top of ADV_GAP.

    Zero when either glyph is blank (a space is not kerned against) and -- the guard
    that matters -- zero when the two share no ink row at all, because mean_gap()
    answers 0 for "nothing to measure" and a 0 read as a measurement would push the
    pair apart to the cap instead of leaving it alone.
    """
    m = mean_gap(ea, eb, ADV_GAP)
    if m <= 0.0:
        return 0
    gap = max(KERN_LO, min(KERN_HI, int(round(ADV_GAP + KERN_TARGET - m))))
    return gap - ADV_GAP


def jh_scaled():
    """Every character's scaled cell, its per-row ink edges and its ink box.

    One place, so the atlas and the kern table cannot be built from two different
    scalings of the same table -- they must agree or the shipped advances are not the
    ones the kerning was measured against.
    """
    raw = jh_cells()
    cells, edges, boxes = {}, {}, {}
    for ch in CHARS:
        k = ord(ch) + JH_FIRST
        if k >= len(raw):
            sys.exit("the JH table has no cell %d, for %r" % (k, ch))
        c = scale_cell(raw[k])
        cells[ch], edges[ch], boxes[ch] = c, row_edges(c), ink_box(c)
    if len(cells[" "]) != JH_T:
        sys.exit("the space's cell did not scale")
    return cells, edges, boxes


def jh_bake():
    """The JH atlas: metrics and coverage.

    Every glyph's `top` and `h` are the SAME two numbers, and that is the alignment
    rule above showing through. One row band is chosen for the whole set -- the rows
    the scaled cells' ink actually occupies, which is where MENU_FONT_INK_TOP and
    _BOT come from -- and every glyph is cut to that band, so the cells keep the one
    grid they share and an empty row inside a glyph's box costs one byte and draws
    nothing. It is placed once: at the line-box row that puts the scaled baseline on
    the row Decker's capitals sit on, so the two faces sit identically in the button.
    Only the horizontal is per-glyph, cut to its own ink box, because a pen advance
    is a horizontal thing and the lead a cell carries is not a bearing rbp's own text
    has.
    """
    cells, _edges, boxes = jh_scaled()
    inked = [b for b in boxes.values() if b]
    if not inked:
        sys.exit("no cell in the JH table has any ink")
    r0 = min(b[1] for b in inked)         # the set-wide row band, over the scaled cells
    r1 = max(b[3] for b in inked)

    # The scaled row the baseline landed on: the last scaled row whose source span
    # still starts at or above the native baseline row.
    tb = 0
    for t in range(JH_T):
        if (t * JH_ROWS) // JH_T <= JH_NATIVE_BASELINE:
            tb = t
    # CAP_BOTTOM_ROW is the line-box row Decker's capitals end on (MENU_FONT_LINE
    # 24, ink band 5..22, capitals 5..18), so the JH capitals land on the same one.
    top = CAP_BOTTOM_ROW - tb + r0
    h = r1 - r0 + 1

    glyphs = []
    cov = bytearray()
    for ch in CHARS:
        c = cells[ch]
        box = boxes[ch]
        if ch == " " or box is None:
            glyphs.append((int(round(NATIVE_WORD_SPACE * FIT)), 0, 0, 0, 0, len(cov)))
            continue
        w = box[2] - box[0] + 1
        for y in range(r0, r1 + 1):
            cov += bytes(c[y][box[0]:box[2] + 1])
        glyphs.append((w + ADV_GAP, 0, top, w, h, len(cov) - w * h))
    return glyphs, bytes(cov), top, tb


def jh_kern_table(edges):
    """The full GLYPHS x GLYPHS delta table, derived from the scaled cells."""
    return [[kern_delta(edges[a], edges[b]) for b in CHARS] for a in CHARS]


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


def ink_rows(g, cov):
    """The rows of one glyph's box that really hold ink, as (first, last), or None.

    The Decker bake's ink box IS its ink, so its metrics say this directly. The JH
    bake's is not: every glyph is cut to one set-wide row band, so a capital's box is
    a row or two taller than its own ink and the capitals' height -- the number the
    operator chose the face for -- can only come from the coverage.
    """
    w, h, off = g[3], g[4], g[5]
    if not h:
        return None
    rows = [y for y in range(h) if any(cov[off + y * w + x] for x in range(w))]
    return (rows[0], rows[-1]) if rows else None


def kern_source(kern):
    """The pair table as C, one row per left-hand glyph.

    Each row carries its OWN braces. The array is [GLYPHS][GLYPHS], and a flat list in
    one big brace pair is the same table to the compiler but draws a
    -Wmissing-braces warning per row -- 73 of them, which buries any real one in the
    build log.
    """
    rows = []
    for r in kern:
        rows.append("    { " + " ".join("%3d," % v for v in r) + " },")
    return "\n".join(rows)


KERN_DECL = ("static const signed char "
             "menu_font_kern[MENU_FONT_GLYPHS][MENU_FONT_GLYPHS]")

KERN_ZERO_NOTE = """/* NO PAIR KERN IN THIS BAKE, and the array below is declared with no initializer to
 * say so: it is all zero, so menu_font_adv() returns each glyph's own advance and the
 * painters draw the same labels they drew before a kern table existed. The two
 * arguments stay because the painters are one codebase for both faces and must not
 * have to know which one they were built against. MENU_FONT_KERNED records the same
 * fact for test_menu.c, which would otherwise read a table that was never filled as
 * one the bake lost.
 *
 * The table and the prose about the 5 px optical target it aims at travel together, so
 * neither is emitted beside a face that did not earn it -- see the JH bake for both. */
%s;""" % KERN_DECL

KERN_PROSE = """/* THE PAIR KERN, in px, added to the left glyph's advance when the right one follows.
 * One row per left-hand glyph, one column per right-hand one, in menu_font_index()
 * order, so it is indexed [menu_font_index(a)][menu_font_index(b)].
 *
 * WHY A TABLE AND NOT TWO NUMBERS PER GLYPH. The obvious shape is a right bearing on
 * each glyph and a left one on the next, summed per pair -- two arrays of 73 instead
 * of one of 5329. It was tested and it does not hold: of the pairs in the seven labels
 * 180 agree and 143 disagree by a whole pixel, always looser on 'A', because a glyph's
 * ink recedes by a different amount on its left than on its right and one number per
 * side cannot say both. A pixel is the unit here, so a table it is.
 *
 * WHY IT IS SIGNED AND WHY IT IS NOT ZERO EVERYWHERE. A pair whose ink boxes are only
 * %(gap)d px apart still has a MEAN per-row gap -- the average of the wedge each pair
 * leaves down its height -- that runs from 1.9 ("UR", two full-height stems) to 13.7
 * ("LT", L's stem against T's arm) at that same %(gap)d px. The eye reads the mean, not
 * the boxes, and a 7x spread is what "the kerning is off" means. So each pair's entry
 * moves the box gap to hold the MEAN near %(kern_target)g px, floored at %(kern_lo)d so
 * two ink boxes can never overlap and capped at %(kern_hi)d so a pair with nothing to
 * measure -- two glyphs whose ink shares no row at all, or a space on either side -- is
 * left exactly alone rather than pushed apart by a 0 read as a measurement.
 *
 * It is derived from the glyph profiles by %(here)s, not tuned by hand. A bake at a
 * different size derives its own: the profile of a shrunk glyph is shallower, so the
 * native table would be the wrong one at the fitted size.
 *
 * The unkerned advance is still what each glyph's `adv` carries, so a caller that
 * ignores this table draws exactly the labels it drew before the table existed. */"""


def kern_section(kern):
    """The pair table and the prose that explains it, or the note that there is none.

    One face is kerned and one is not, and the header must not explain a 5 px optical
    target beside 5329 zeros -- so the explanation is emitted only with the table it
    describes, and an unkerned bake says plainly that it is unkerned.
    """
    if not any(v for r in kern for v in r):
        return KERN_ZERO_NOTE
    return (KERN_PROSE % dict(gap=ADV_GAP, kern_target=KERN_TARGET, kern_lo=KERN_LO,
                              kern_hi=KERN_HI, here=HERE)
            + "\n" + KERN_DECL + " = {\n" + kern_source(kern) + "\n};")


def label_width(s, glyphs, kern):
    """A label's width the way menu_text_width() walks it: kerned, no trailing space.

    It must be the same walk the shim does, or the number this prints is not the one
    menu_labels_fit() will refuse on.
    """
    w = 0
    for j, c in enumerate(s):
        i = CHARS.index(c)
        w += glyphs[i][0]
        if j + 1 < len(s):
            w += kern[i][CHARS.index(s[j + 1])]
    return w


def band_floor(widest):
    """The narrowest picture menu_labels_fit() would still draw the band on.

    menu_paint.c's own arithmetic, with MENU_BTN_PAD_PX 4 and MENU_BTN_BORDER_PX 1:
    a column's inner width is its span less those two rings, and the band is refused
    while any column's inner width is under the widest label. It is repeated here
    rather than read, because the bake is run on a machine with no build of the shim.
    """
    pad, border, cols, logical_w = 4, 1, len(LABELS), 1280
    for w in range(1, 4000):
        ok = True
        for i in range(cols):
            x0 = (i * logical_w) // cols * w // logical_w
            x1 = ((i + 1) * logical_w) // cols * w // logical_w - 1
            if (x1 - x0 + 1) - 2 * (pad + border) < widest:
                ok = False
                break
        if ok:
            return w
    return 0


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


def parse_args(argv):
    import argparse

    ap = argparse.ArgumentParser(
        description="Bake menu_font.h from one of the device's own two faces.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="The default is the face the header currently ships. --px and --light\n"
               "belong to the Decker face; the JH bake's size is FIT_ROWS.")
    ap.add_argument("--face", choices=("jh", "decker"), default="decker",
                    help="which face to bake: Decker Bold thinned from the ttf, which "
                         "is what ships (default), or the Pioneer 18030_JH cell table "
                         "the operator tried and turned down on 2026-10-07")
    ap.add_argument("--px", type=int, default=DEFAULT_PX,
                    help="Decker only: the pixel size to rasterise at (default %d)"
                         % DEFAULT_PX)
    ap.add_argument("--light", type=int, default=DEFAULT_LIGHT,
                    help="Decker only: supersampled pixels of erosion per side "
                         "(default %d; 0 is the unthinned bold)" % DEFAULT_LIGHT)
    return ap.parse_args(argv)


def light_note(light):
    """The two sentences the header needs about this bake's weight and its history."""
    if light:
        why = ("WHY IT IS THINNED, which is the one thing here that is not simply the "
               "device's font. Decker ships BOLD and only bold, and the operator asked "
               "on 2026-10-06 for \"a font that resembles the out of the box RX3 "
               "font\" -- which is a question about WEIGHT, because the player's own "
               "text is light. rbp does not draw with this file either: its Latin "
               "glyphs come from Pioneer's baked tables, gui/pset/fontdata/"
               "NS_FONT_ID_*.bin, and those are a LIGHT face. What is left is to thin "
               "Decker: each glyph's outline is eroded by %d/%d of a pixel on every "
               "side (that is %d of %d supersampled pixels), which takes the face from "
               "bold to a lighter weight without moving a single metric.\n"
               " *\n"
               " * WHAT THE THINNING CANNOT TOUCH. Every metric below -- advance, "
               "left, top, w, h and the whole MENU_FONT_INK_* band -- is read from the "
               "font at 1x and is IDENTICAL to an unthinned bake of the same size. "
               "Re-baking with `--light 0` changes bytes inside menu_font_coverage[] "
               "and nothing else, so every label keeps its width, its centring and its "
               "vertical position."
               % (light, SUPERSAMPLE, light, SUPERSAMPLE))
    else:
        why = ("WHY IT IS NOT THINNED. Decker ships BOLD and only bold, but this bake "
               "is that bold exactly as the device's file rasterises -- no erosion, no "
               "coverage boost. It is the unmodified face, kept because it is one "
               "command away and it is the honest baseline the thinned bake is a "
               "departure from.")
    return why


JH_ASCENT, JH_LINE = CAP_BOTTOM_ROW + 1, 24
JH_DESCENT = JH_LINE - JH_ASCENT

# The paragraph about what the bar costs, which is not the same story for the two
# faces: the Decker bake buys its size with the line box and hits the 480-row panels,
# while the JH bake fixes the line box at 24 and buys its width with the picture.
LADDER_DECKER = """ * The bar is not this file's to change: it is 56 rows (menu_zone.h's MZ_PANEL_H), 8
 * of frame at each end and so a 40-row button band at 1280x800, and the label's line
 * box has to fit that band on the SMALLEST picture the port supports. That is what
 * stops the ladder -- the line box is %(line)d rows here, %(line)d at %(px)d px and
 * %(line_up)d at %(px_up)d, and the 480-row panels (640x480, 800x480) give a 24-row
 * band. A larger atlas is a smaller supported panel, not a bigger bar.""" % dict(
    line=24, px=19, line_up=25, px_up=20)

LADDER_JH = """ * The bar is not this file's to change, and this bake does not touch it: it is 56
 * rows (menu_zone.h's MZ_PANEL_H), 8 of frame at each end and so a 40-row button band
 * at 1280x800. The line box here is the same %(line)d rows the Decker face's metrics
 * give, on purpose -- the height floor menu_paint.c refuses an undersized band on is
 * MENU_FONT_LINE, so keeping it identical keeps the 480-row panels (640x480, 800x480)
 * drawing the band exactly as they did. Only MENU_FONT_INK_TOP and _BOT moved, and
 * they moved inside the box rather than out of it.""" % dict(line=JH_LINE)


def main():
    args = parse_args(sys.argv[1:])
    check_alphabet()

    if args.face == "jh":
        if not os.path.exists(JH_BIN):
            sys.exit("%s is missing -- it is gitignored, so it exists only in a "
                     "checkout that has the firmware extracted" % JH_BIN)
        _cells, edges, _boxes = jh_scaled()
        glyphs, cov, top, tb = jh_bake()
        kern = jh_kern_table(edges)
        ascent, descent, line = JH_ASCENT, JH_DESCENT, JH_LINE
        px = FIT_ROWS
        px_note = "the cap height this atlas was fitted to, in rows"
        size_phrase = "the fitted %d-row cap" % FIT_ROWS
        light_word = ("fitted from the table's own 23-row cells at %.4f "
                      "(area-averaged, coverage boosted %.2fx)" % (FIT, JH_WEIGHT))
        face_src = ("area-averages Pioneer's own fixed-cell table %s -- the face rbp "
                    "itself draws its Latin from -- into an 8-bit coverage atlas whose "
                    "capitals are %d rows, and derives a pair-kern table from the "
                    "result" % (os.path.basename(JH_BIN), FIT_ROWS))
        face_howto = "\n".join([
            " *     python3 %s" % HERE,
            " *     python3 %s --face decker --px 19 --light 1" % HERE])
        face_why = ("WHY THIS FACE IS HERE AT ALL, AND WHY IT IS NOT WHAT SHIPS. Both "
                    "faces were baked and put on the glass in seven strips on "
                    "2026-10-07 at the band's own size, the operator took this one, it "
                    "was shipped -- and they looked at it and went back to Decker the "
                    "same day: \"the kerning is off switch back to decker\". So this "
                    "bake is kept as a capability, not as the shipped face: it is one\n"
                    " * command away and its own kern table is what the optical-kern "
                    "work is for, but `--face decker` is the default and the header on "
                    "disk is Decker. Do not re-ship it without putting the strips in "
                    "front of the operator again. It is not a ttf: it is a "
                    "plain fixed-cell dump, %d cells of %d rows x %d px, **2 bits per "
                    "pixel** -- a row is 6 bytes, 24 px of four 0..3 coverage samples, "
                    "NOT 1 bpp, and reading it as 1 bpp is where an earlier note got a "
                    "\"WIDE face, about 2.5x\" from. The ASCII run is at cell = "
                    "ord(c) + %d and is not a copy of the ISO8859 table's: against "
                    "rbp's own large text it predicts widths with a mean error of "
                    "1.9 px where that table is off by 2.8.\n"
                    " *\n"
                    " * WHAT \"FITTED\" MEANS AND WHAT IT COSTS. The cells are 23 rows "
                    "tall and the band's line box is %d; a capital is %d rows in the "
                    "shipped Decker bake, so the whole cell is scaled to %d/%d of its "
                    "size -- %.4f -- which gives a %d-row capital. The alignment rides "
                    "along with the cell: all %d scaled rows are kept and every glyph's "
                    "ink is placed on the one grid they share, never re-aligned to a "
                    "common top, because re-aligning pushes every round letter below "
                    "the baseline.\n"
                    " *\n"
                    " * The cost is WIDTH, and it is the reason this file's width is "
                    "worth reading before changing. This face's letters are wider than "
                    "Decker's at the same cap: \"USB STOP\" is %d px where the shipped "
                    "bake is 76, so the narrowest column that holds it is 117 px and "
                    "menu_paint.c's menu_labels_fit() -- which refuses the WHOLE band "
                    "on a picture whose narrowest column cannot hold its own label -- "
                    "now refuses every picture under 823 px wide. 480- and 640-wide "
                    "were already spent by the 19 px bake; 800-wide goes with this one. "
                    "That width is the standing cost of this face, and it is one of the "
                    "things the operator weighed when they went back to Decker -- with "
                    "the numbers in front of them, on 2026-10-07."
                    % (len(jh_cells()), JH_ROWS, JH_BPR * 4, JH_FIRST, JH_LINE,
                       FIT_ROWS, FIT_ROWS, 19, FIT, FIT_ROWS, JH_T,
                       107))
    else:
        px, light = args.px, args.light
        if not 0 <= light * 2 < SUPERSAMPLE:
            sys.exit("light %d of %d would thin a %d px glyph by more than half a "
                     "pixel on each side, which stops being a lighter weight and "
                     "starts being a broken one (at 3 of 6 the '-' and '=' rasterise "
                     "to nothing)" % (light, SUPERSAMPLE, px))
        if not os.path.exists(TTF):
            sys.exit("%s is missing -- it is gitignored, so it exists only in a "
                     "checkout that has the firmware extracted" % TTF)
        ascent, descent, line, glyphs, cov = bake(px, light)
        kern = [[0] * len(CHARS) for _ in CHARS]
        px_note = "the pixel size this atlas was baked at"
        size_phrase = "%d px" % px
        light_word = ("thinned %d/%d px a side" % (light, SUPERSAMPLE)) if light \
            else "unthinned (bold)"
        face_src = ("rasterises %s -- the only Latin font file the XDJ-RX3 carries "
                    "(gui/system/fontdata holds it and the Japanese "
                    "sazanami-gothic.ttf and nothing else) -- into an 8-bit coverage "
                    "atlas at %d px, %s"
                    % ("extracted/XDJRX3/gui/fontdata/decker.ttf", px, light_word))
        face_howto = "\n".join([
            " *     python3 %s --face decker --px %d --light %d" % (HERE, px, light),
            " *     python3 %s" % HERE])
        face_why = light_note(light)

    widths = {lb: label_width(lb, glyphs, kern) for lb in LABELS}
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
    # THE CAP HEIGHT, which is what a label is and what rbp's own text is measured
    # against above -- the set-wide band is as tall as its descender and its ascender
    # put together, and quoting that as "the size" would compare the labels to the
    # player's caps across two different things.
    #
    # Measured from the COVERAGE, not the boxes: on the JH face every glyph's box is
    # one set-wide row band, so a box says nothing about where that glyph's ink is
    # (see ink_rows()). And measured to the BASELINE rather than to the lowest cap,
    # because a capital may have a tail -- the JH 'Q' runs one row under the line the
    # other twenty-five capitals stand on, and taking the bottom-most cap would call
    # this face a 15-row capital when its cap height is 14, which is the number the
    # face was chosen for. The baseline is where MOST capitals end, which is what
    # makes it the baseline rather than an overshoot.
    cap_ext = [ink_rows(glyphs[CHARS.index(c)], cov)
               for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]
    cap_bottoms = [r[1] for r in cap_ext if r]
    cap_tops = [r[0] for r in cap_ext if r]
    cap_bot = max(set(cap_bottoms), key=cap_bottoms.count)
    cap_top = min(cap_tops)

    body = "\n".join("    { %2d, %3d, %3d, %2d, %2d, %5d }," % g for g in glyphs)

    src = '''/*
 * menu_font.h -- the panel's labels, in the device's own typeface. GENERATED.
 *
 * Do not edit this file. It is written by %(here)s, which %(face_src)s. To change
 * the size, the weight, the face or the character set, run
 *
%(face_howto)s
 *
 * WHY THIS REPLACED A 5x7 BITMAP. The first version of the panel's labels was a
 * hand-written 5x7 table drawn at an integer scale; on a 96-px button that meant
 * MENU_SCALE_MAX, i.e. every source pixel replicated as a 4x4 block, 28 rows of
 * blocks for 13 rows of letters. The operator put a finger on it and said the font
 * was "too big and pixely". At %(size_phrase)s this ink is %(ink_h)d rows tall, inside a
 * %(line)d-row line box (ink rows %(ink_top)d..%(inkbot)d of it).
 *
%(face_why)s
 *
 * HOW THE SIZE COMPARES TO rbp'S OWN TEXT, measured: rbp's own `INFO` label has 13
 * rows of ink, and these capitals have %(cap_h)d.
 *
%(ladder)s
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

#define MENU_FONT_PX      %(px)d    /* %(px_note)s */
#define MENU_FONT_ASCENT  %(ascent)d   /* above the baseline, from the ttf     */
#define MENU_FONT_DESCENT %(descent)d    /* below it                             */
#define MENU_FONT_LINE    (MENU_FONT_ASCENT + MENU_FONT_DESCENT)

/* space, '0'-'9', 'A'-'Z', 'a'-'z', then the punctuation -- in that order. See
 * menu_font_index() for the indices; the last run is listed there one case at a
 * time because it is not a contiguous ASCII range. */
#define MENU_FONT_GLYPHS  %(nglyph)d

/* Whether this bake's pair table is live. 0 means the table is all zero and every
 * advance is the glyph's own -- menu_font_adv() is the same two-argument call either
 * way, because the painters are one codebase for both faces and must not have to know
 * which one they were built against. test_menu.c gates its "the bake lost the table"
 * check on this rather than failing a face that was never kerned. */
#define MENU_FONT_KERNED  %(kerned)d

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

%(kern_section)s

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

/* The pen advance for `c` when `next` follows it -- the glyph's own advance plus the
 * pair's entry in menu_font_kern. `next` is the NEXT character, and a caller at the
 * end of a string passes the '\\0' that is already there, which has no index and so
 * adds nothing: a string's width does not depend on what happens to follow it in
 * memory.
 *
 * WHY EVERY WALKER CALLS THIS AND NOT `g->adv` DIRECTLY. On a kerned bake the two
 * disagree, and the walk that sums `adv` while the painter beside it advances by this
 * puts the text and its surroundings one pixel apart per pair -- exactly the defect the
 * table exists to remove. The shipped Decker bake carries no table (MENU_FONT_KERNED 0,
 * so this returns `adv` and the two are the same function), and the call is kept anyway
 * because the five painters are one codebase for both faces and must not have to know
 * which one they were built against.
 *
 * FOUR OF THE FIVE WALKERS ARE PINNED BY A PIXEL ORACLE against stepping something else:
 * a painter and its classifier share their walk, so mutating it moves both together and
 * every pixel still agrees with itself. test_menu.c's test_label_pens, test_fx.c's
 * test_paint_image, test_side.c's test_paint_label_pens and test_prompt.c's
 * test_paint_label_pens each paint their surface and check that the ink the painter laid
 * down is exactly the ink a walk of these advances over the same atlas puts there -- a
 * number the pen's origin cannot influence, so it needs no copy of the module's own
 * scaling. The fifth walker is menu_window_paint.c's, in the abandoned browser window: it
 * has no such test and no label it can be run on. */
static inline int menu_font_adv(unsigned char c, unsigned char next)
{
    int i = menu_font_index(c);
    int j = menu_font_index(next);

    if (i < 0 || j < 0)
        return (int)menu_font_glyph(c)->adv;      /* a gap is never kerned against */
    return (int)menu_font_glyph(c)->adv + (int)menu_font_kern[i][j];
}

/* The width in pixels of `s`: the sum of the kerned advances, with no trailing space,
 * because centring a label is the only caller and it needs this exact. The old
 * interface took a scale; there is no scale here by design (see the header). */
static inline int menu_text_width(const char *s)
{
    int w = 0;

    if (!s)
        return 0;
    while (*s) {
        w += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
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
           px_note=px_note, size_phrase=size_phrase,
           face_src=face_src, face_howto=face_howto, face_why=face_why,
           ladder=(LADDER_JH if args.face == "jh" else LADDER_DECKER),
           ink_h=ink_h, ink_top=ink_top, inkbot=inkbot, line=line, body=body,
           cap_h=cap_bot - cap_top + 1,
           cov=hexdump(cov), covlen=len(cov), index=index_source(),
           kern_section=kern_section(kern),
           kerned=1 if any(v for r in kern for v in r) else 0,
           punct=PUNCT)

    with open(OUT, "w") as f:
        f.write(src)

    # What the band costs, which is the number a reader of this header should not have
    # to work out: menu_paint.c refuses the whole band below this picture width, and
    # nothing in the shim re-checks it, so a face change moves it silently.
    widest = max(widths.values())
    print("wrote %s: %d glyphs, face %s, coverage %d bytes (%.1f KB of source)"
          % (OUT, len(CHARS), args.face, len(cov), len(src) / 1024.0))
    print("  weight: %s" % light_word)
    print("  ascent %d descent %d line %d; ink rows %d..%d (%d tall), capitals"
          " %d rows" % (ascent, descent, line, ink_top, inkbot, ink_h,
                        cap_bot - cap_top + 1))
    print("  advances %d..%d (%s; the pair table moves them by %d..%d)"
          % (min(g[0] for g in glyphs), max(g[0] for g in glyphs),
             "kerned" if any(v for r in kern for v in r) else "un kerned",
             min(v for r in kern for v in r), max(v for r in kern for v in r)))
    print("  %-9s %5s %5s   (column %d px)" % ("label", "width", "slack", COL_W))
    for lb in sorted(LABELS, key=lambda s: -widths[s]):
        print("  %-9s %5d %5d" % (lb, widths[lb], COL_W - widths[lb]))
    print("  widest %d px -> menu_labels_fit() refuses any picture under %d px wide"
          % (widest, band_floor(widest)))


if __name__ == "__main__":
    main()
