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
the glass was "too big and pixely". rbp's own UI is set in Decker Bold -- the
typeface the XDJ-RX3 carries in its own font directory -- so the panel's labels
can be drawn in the device's type at the device's own text size instead of in a
grid of blocks. That needs a rasteriser at build time, which is exactly what this
script is: it runs once, on a machine, and its output is committed. The shim
itself still has no font engine, no freetype and no file I/O -- it gets a table.

    python3 scripts/shims/bake_menu_font.py [px]

The output is scripts/shims/menu_font.h: one 8-bit coverage bitmap and a
per-glyph metrics table. `px` defaults to 16, which is the operator's choice.

WHAT SIZE IS rbp'S OWN TEXT, MEASURED. The `INFO` label in rbp's top bar is 42 px
wide by 13 rows of ink (x 1217..1258, rows 18..30 of its control). Decker Bold's
cap ink measures

    px   12  13  14  15  16  17  18  19  20  21  22  23  24
    ink   9   9  10  11  11  12  13  14  14  15  16  16  17

so **18 px, not 16 px, is the size that reproduces rbp's own 13-row text**. 16 px
is 11 rows: a little under the player's own labels, and the operator picked it
after seeing the ladder, on the reasoning that "too big" is the complaint being
answered. It is one command to change, and the drills ask them to settle it by eye:

    python3 work/bake_menu_font.py 18

WHY 8-BIT COVERAGE AND NOT 1-BIT INK. Because it is the only way to get the
antialiasing that the operator asked for, and because it costs almost nothing:
under 6 KB for the whole 37-glyph set at any size in the ladder above. The painter
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

from PIL import ImageFont

# Paths are the repository's, resolved from this file's own location, so the script
# runs from any working directory: <root>/scripts/shims/bake_menu_font.py.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TTF = os.path.join(ROOT, "extracted/XDJRX3/gui/fontdata/decker.ttf")
OUT = os.path.join(ROOT, "scripts/shims/menu_font.h")
# What the generated header says it came from -- relative to the repository, since
# that is how a reader of the header will look for it.
HERE = "scripts/shims/bake_menu_font.py"
DEFAULT_PX = 16

# space, then '0'-'9', then 'A'-'Z' -- the same set, in the same order, as the 5x7
# table this replaces, so menu_font_index() keeps its meaning and every existing
# label keeps working. A label with a character outside this set draws a gap (see
# menu_font_index in the generated header), which is what the old table did too.
CHARS = " " + "0123456789" + "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

LABELS = ["SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH", "MENU"]
COL_W = 1280 // 6


def bake(px):
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
        data = bytes(mask)
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


def main():
    px = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PX
    if not os.path.exists(TTF):
        sys.exit("%s is missing -- it is gitignored, so it exists only in a "
                 "checkout that has the firmware extracted" % TTF)

    ascent, descent, line, glyphs, cov = bake(px)

    widths = {}
    for lb in LABELS:
        widths[lb] = sum(glyphs[CHARS.index(c)][0] for c in lb)
    ink_h = max(g[4] for g in glyphs)
    ink_top = min(g[2] for g in glyphs if g[4])
    # Inclusive of the last ink row, so INK_BOT - INK_TOP + 1 == INK_H reads as the
    # band it is -- the generator asserts that identity below and test_menu.c
    # asserts it against the table.
    inkbot = max(g[2] + g[4] for g in glyphs) - 1
    assert inkbot - ink_top + 1 == ink_h, (ink_top, inkbot, ink_h)

    body = "\n".join("    { %2d, %3d, %3d, %2d, %2d, %5d }," % g for g in glyphs)

    src = '''/*
 * menu_font.h -- the panel's labels, in the device's own typeface. GENERATED.
 *
 * Do not edit this file. It is written by %(here)s, which rasterises
 * extracted/XDJRX3/gui/fontdata/decker.ttf -- the Decker Bold the XDJ-RX3 carries
 * in its own font directory, and the typeface rbp draws its UI in -- into an 8-bit
 * coverage atlas at %(px)d px. To change the size or the character set, run
 *
 *     python3 %(here)s %(px)d
 *
 * WHY THIS REPLACED A 5x7 BITMAP. The first version of the panel's labels was a
 * hand-written 5x7 table drawn at an integer scale; on a 96-px button that meant
 * MENU_SCALE_MAX, i.e. every source pixel replicated as a 4x4 block, 28 rows of
 * blocks for 13 rows of letters. The operator put a finger on it and said the font
 * was "too big and pixely". At %(px)d px this ink is %(ink_h)d rows tall, inside a
 * %(line)d-row line box (ink rows %(ink_top)d..%(inkbot)d of it).
 *
 * HOW THAT COMPARES TO rbp'S OWN TEXT, measured: the `INFO` label in rbp's top bar
 * has 13 rows of ink, and Decker Bold reaches 13 rows at 18 px -- so %(px)d px is
 * two rows under the player's own labels. That is the operator's choice, made
 * against this ladder, and one command changes it:
 *
 *     python3 work/bake_menu_font.py 18
 *
 * WHY 8-BIT COVERAGE. Antialiasing is the other half of "pixely", and it costs
 * %(covlen)d bytes for all %(nglyph)d glyphs. Coverage 0 and coverage 255 are exact --
 * a pixel outside a glyph is the button colour and a fully covered one is the label
 * colour -- so menu_paint.h's "painting it twice is painting it once" and
 * menu_draw.c's conditional restore both still hold, pixel for pixel. Only the
 * glyph edges blend, and they blend against the button colour the painter already
 * knows.
 *
 * THE SET is space, '0'-'9' and 'A'-'Z' -- the same set in the same order as the
 * table this replaced, so menu_font_index() kept its meaning and every existing
 * label kept working. A character outside the set draws a gap of the space
 * advance's width rather than garbage, which is also what the old table did.
 *
 * THE LAYOUT IS THE FONT'S OWN METRICS, not a fixed cell. Each glyph carries its
 * pen advance and the position of its ink box relative to the pen origin and to the
 * line box top, so a label is centred by summing advances (menu_text_width) and is
 * placed vertically from the line box (MENU_FONT_LINE). Nothing is scaled: there is
 * no scale parameter anywhere in this interface, because a scaled bitmap is what
 * this file exists to stop doing.
 */
#ifndef RBLIVE4_MENU_FONT_H
#define RBLIVE4_MENU_FONT_H

#include <stddef.h>          /* size_t, for the table walk below */

#define MENU_FONT_PX      %(px)d    /* the pixel size this atlas was baked at */
#define MENU_FONT_ASCENT  %(ascent)d   /* above the baseline, from the ttf     */
#define MENU_FONT_DESCENT %(descent)d    /* below it                             */
#define MENU_FONT_LINE    (MENU_FONT_ASCENT + MENU_FONT_DESCENT)

/* space, then '0'-'9', then 'A'-'Z'. See menu_font_index(). */
#define MENU_FONT_GLYPHS  %(nglyph)d

/* The tallest ink in the set, and the band it occupies inside the line box: rows
 * MENU_FONT_INK_TOP .. MENU_FONT_INK_BOT inclusive. test_menu.c asserts these
 * against the table, and the witness in menu_draw.c relies on the label band being
 * where this says it is -- it samples 15 points that must not be ink. */
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
static inline int menu_font_index(unsigned char c)
{
    if (c == ' ')
        return 0;
    if (c >= '0' && c <= '9')
        return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z')
        return 11 + (c - 'A');
    return -1;
}

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

#endif /* RBLIVE4_MENU_FONT_H */
''' % dict(here=HERE, px=px, ascent=ascent, descent=descent, nglyph=len(CHARS),
           ink_h=ink_h, ink_top=ink_top, inkbot=inkbot, line=line, body=body,
           cov=hexdump(cov), covlen=len(cov))

    with open(OUT, "w") as f:
        f.write(src)

    print("wrote %s: %d glyphs at %d px, coverage %d bytes (%.1f KB of source)"
          % (OUT, len(CHARS), px, len(cov), len(src) / 1024.0))
    print("  ascent %d descent %d line %d; ink rows %d..%d (%d tall)"
          % (ascent, descent, line, ink_top, inkbot, ink_h))
    print("  advances %d..%d" % (min(g[0] for g in glyphs),
                                 max(g[0] for g in glyphs)))
    print("  %-9s %5s %5s   (column %d px)" % ("label", "width", "slack", COL_W))
    for lb in sorted(LABELS, key=lambda s: -widths[s]):
        print("  %-9s %5d %5d" % (lb, widths[lb], COL_W - widths[lb]))


if __name__ == "__main__":
    main()
