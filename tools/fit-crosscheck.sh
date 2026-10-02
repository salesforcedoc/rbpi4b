#!/bin/sh
# fit-crosscheck.sh — prove the two copies of the fit rule still agree.
#
# Where the logical 1280x800 rbp renders lands inside the real framebuffer is
# decided by ONE rule, and that rule is written down TWICE:
#
#   fbdev_present_fit()  systems/fbdev/fbdev.c, cross-built into the DirectFB
#                        fbdev module — decides where the present path blits
#   point_fit()          scripts/shims/point_xform.c — decides where the visible
#                        pointer draws, so the arrow follows the picture
#
# They cannot share a header: one is compiled for the target by the DirectFB
# build, the other is part of an LD_PRELOAD shim built separately, and the two
# trees do not include each other. So the copies are held together by comments
# that say "a change to one is a change to both" — which is a promise, not a
# check, and the failure it lets through is invisible on the unit as shipped: on
# a panel that IS 1280x800 both functions return the identity, so a drift shows
# up only on the mismatched panel the fit exists for, as an arrow in the wrong
# place, on a screen the operator has just swapped in.
#
# This is the check. It extracts the driver's copy from the TRACKED diff (never
# from work/dfb-src, which is a gitignored scratch tree that dfb-build.sh
# rewrites from the diff on every build — reading it would test whatever was
# last built on this machine), compiles it beside the shim's real source, and
# sweeps both over a few million sizes. A drift is a non-zero exit.
#
# It fails LOUDLY rather than skipping when it cannot find something. A guard
# that quietly does nothing when a path is missing is worse than no guard: the
# build stays green and the reader believes the two copies were compared.
#
# Runs on the host (macOS or Linux) with cc and python3. It needs no target, no
# Docker and no qemu, and it takes a second:
#
#     sh tools/fit-crosscheck.sh
#
# What it does NOT check: that the driver CALLS fbdev_present_fit() correctly,
# or that the cursor uses point_fit()'s rectangle correctly. Those are pinned by
# the caller's own log line (the driver's one-shot `PRESENT:` line prints the
# rectangle it used, and fb_cursor.c logs its own) — two numbers to compare on
# the unit when a mismatched panel is in front of you.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
DIFF="$ROOT/tools/build-directfb/directfb-full.diff"
SHIM_SRC="$ROOT/scripts/shims/point_xform.c"
SHIM_HDR="$ROOT/scripts/shims"

for f in "$DIFF" "$SHIM_SRC"; do
    if [ ! -f "$f" ]; then
        echo "fit-crosscheck: $f is missing." >&2
        echo "  Both are tracked files; a checkout without them cannot run this" >&2
        echo "  check, and saying so is the point -- do not skip it silently." >&2
        exit 1
    fi
done

command -v cc >/dev/null 2>&1 || { echo "fit-crosscheck: no cc on PATH." >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "fit-crosscheck: no python3 on PATH." >&2; exit 1; }

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

# --- the driver's copy, out of the diff --------------------------------------
python3 - "$DIFF" "$WORK/driver.c" <<'PY' || exit 1
import pathlib, sys

diff = pathlib.Path(sys.argv[1]).read_text().split("\n")

start = None
for i, line in enumerate(diff):
    if line.startswith("+fbdev_present_fit( int sw"):
        start = i
        break
if start is None:
    sys.exit("fit-crosscheck: the diff adds no fbdev_present_fit(); the name "
             "changed, or the function is no longer an addition.")

# The function is a contiguous block of added lines. Depth counting rather than
# "until the next hunk" so that trailing blank added lines and whatever follows
# the function are not swallowed into it.
body, depth = [], 0
for line in diff[start:]:
    if not line.startswith("+"):
        break
    text = line[1:]
    body.append(text)
    depth += text.count("{") - text.count("}")
    if depth == 0 and text.strip() == "}":
        break
else:
    sys.exit("fit-crosscheck: the added fbdev_present_fit() never closed.")

# A sanity check on the extraction itself, because a mis-slice that happens to
# compile would be worse than a failure: this is the 16.16 step the whole rule
# rests on, and it is the one line a "simplification" is most likely to touch.
if not any("unsigned long long s =" in l for l in body):
    sys.exit("fit-crosscheck: the extracted block is not the fit function.")

pathlib.Path(sys.argv[2]).write_text(
    "/* Extracted by tools/fit-crosscheck.sh from\n"
    " * tools/build-directfb/directfb-full.diff -- the driver's copy of the rule.\n"
    " * Do not edit: it is regenerated, and an edit here checks nothing. */\n"
    "static void " + "\n".join(body) + "\n"
    "void drv_fit(int sw, int sh, int fw, int fh, int st, int *a, int *b, int *c, int *d)\n"
    "{\n"
    "    fbdev_present_fit(sw, sh, fw, fh, st, a, b, c, d);\n"
    "}\n")
print("  driver copy: %d lines extracted from the diff" % len(body))
PY

# --- the sweep ---------------------------------------------------------------
cat > "$WORK/main.c" <<'EOF'
/*
 * Host cross-check of the two copies of the fit rule. The source sizes are the
 * logical one plus the shapes a monitor swap actually brings; the framebuffer
 * sizes sweep the whole useful range at strides that are coprime with the
 * common panel widths, so the rounding is hit at every phase rather than only
 * at the round numbers.
 */
#include <stdio.h>
#include "point_xform.h"

void drv_fit(int sw, int sh, int fw, int fh, int st,
             int *a, int *b, int *c, int *d);

int main(void)
{
    static const int sws[] = { 1280, 1024, 1366, 1920, 800, 640 };
    static const int shs[] = {  800,  768,  768, 1080, 600, 480 };
    long swept = 0, differ = 0;
    unsigned i, j;
    int st, fw, fh;

    for (st = 0; st <= 1; st++)
    for (i = 0; i < sizeof sws / sizeof *sws; i++)
    for (j = 0; j < sizeof shs / sizeof *shs; j++)
    for (fw = 16; fw <= 4096; fw += 13)
    for (fh = 16; fh <= 2400; fh += 7) {
        int a1, b1, c1, d1, a2, b2, c2, d2;

        drv_fit(sws[i], shs[j], fw, fh, st, &a1, &b1, &c1, &d1);
        point_fit(sws[i], shs[j], fw, fh, st, &a2, &b2, &c2, &d2);
        swept++;
        if (a1 != a2 || b1 != b2 || c1 != c2 || d1 != d2) {
            if (differ++ < 10)
                printf("DIFF src %dx%d fb %dx%d stretch=%d: "
                       "driver %dx%d at %d,%d  shim %dx%d at %d,%d\n",
                       sws[i], shs[j], fw, fh, st, a1, b1, c1, d1,
                       a2, b2, c2, d2);
        }
    }

    printf("  %ld sizes swept, %ld differ\n", swept, differ);
    if (differ) {
        printf("fit-crosscheck: THE TWO COPIES HAVE DRIFTED. Fix both --\n"
               "  fbdev_present_fit() in work/dfb-src/systems/fbdev/fbdev.c\n"
               "  point_fit() in scripts/shims/point_xform.c\n"
               "and regenerate directfb-full.diff (work/mkdiff.sh).\n");
        return 1;
    }
    return 0;
}
EOF

# -O1 because the point is agreement on the arithmetic, not speed, and an
# aggressive optimiser has more licence to reassociate than the target build
# would give it. -Wall -Wextra so a warning about the extraction is visible.
if ! cc -O1 -Wall -Wextra -I"$SHIM_HDR" -o "$WORK/fit" \
        "$WORK/main.c" "$WORK/driver.c" "$SHIM_SRC"; then
    echo "fit-crosscheck: the extracted driver copy did not compile." >&2
    echo "  The extraction is a guess about the diff's shape; if the function was" >&2
    echo "  reformatted or split across hunks, this is where it shows." >&2
    exit 1
fi

"$WORK/fit" || exit 1
echo "fit-crosscheck: the driver's copy and the shim's copy agree."
