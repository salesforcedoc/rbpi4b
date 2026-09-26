# tools/

Two kinds of tool live here:

| Tool | Runs on | Language | Purpose |
|---|---|---|---|
| [`patch-rbp/`](patch-rbp/) | workstation | Python | apply the interoperability patches to a stock `rbp` |
| [`build-directfb/`](build-directfb/) | workstation | C / patch | patched DirectFB 1.4.16 (core + fbdev + modules) |
| [`fbdump.c`](fbdump.c) | **the Pi** | C | dump `/dev/fb0` geometry/format + say what it means for the present path |
| [`evdevdump.c`](evdevdump.c) | **the Pi** | C | enumerate input devices; find the pointer and its axis algebra |
| [`aseqdump2dump.py`](aseqdump2dump.py) | the Pi or a workstation | Python | turn an `aseqdump` capture into a replayable MIDI dump, and print the inventory and arithmetic a controller map is written from |

Firmware acquisition, decryption and key handling are out of scope for rblive4;
start from the extracted assets described in
[`docs/04-firmware-assets.md`](../docs/04-firmware-assets.md).

## `patch-rbp`

`rbp_patch.py` contains the complete, verified instruction table that turns the
stock v1.20 `rbp` (md5 `4f2efcfc0c9e3f539289f863acfddcc6`) into `rbp-audio`
(md5 `3706c68f7242779d46afa09f35a39acf`). It is idempotent and validates the
stock words before writing. [`PATCHES.md`](patch-rbp/PATCHES.md) explains what
each patch does.

```bash
python3 tools/patch-rbp/rbp_patch.py /path/to/stock/rbp -o extracted/rbp-audio
```

The `getPcController()` patch is applied as a second stage by
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh) via
[`scripts/patch-rbp-nopc.py`](../scripts/patch-rbp-nopc.py).

## `build-directfb`

Contains `directfb-full.diff`, the complete patch against DirectFB 1.4.16. No
upstream DirectFB sources are shipped; fetch them and apply the diff, then
install to `work/dfb` (see the [README](build-directfb/README.md)).

## `fbdump` — the first thing to run on a new target

The patched fbdev driver makes its decisions from fields that differ per SoC and
per kernel: `bits_per_pixel` and the channel bitfields decide whether a pixel
conversion is needed at all, `smem_len` against `line_length × yres` is the
**page count** — how many whole frames the fb holds, which is what the buffer
mode is decided from — `line_length` is the physical stride, and `smem_len`
bounds the mmap. `ypanstep` is reported but no longer trusted for the buffer
mode: `vc4` reports `1/1` on a fb with one page, and believing it is what asked
for three buffers where one fits ([06](../docs/06-display.md#the-present-path)).
Guessing any of these wastes a build cycle; measuring them takes one command.

```bash
gcc -O2 -static -o fbdump fbdump.c     # on the Pi
./fbdump                               # defaults to /dev/fb0
```

It prints the raw `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` fields, the sysfs
mirror, and then a plain-language verdict: whether the format is a straight
`memcpy` or needs a 565→8888 convert, whether the fb is pannable, and whether
the geometry matches `rbp`'s logical 1280x800 so the present path can copy 1:1
or must letterbox. It is read-only — it never issues a mode-set.

## `evdevdump` — finding the pointer

On the SC Live 4 the touchscreen was always `/dev/input/event0`; on a Pi the
pointer is whatever is plugged into the USB port, so the shim discovers it by
capability instead of by name. This tool is how `POINT_DEV` and the axis algebra
get filled in.

```bash
gcc -O2 -static -o evdevdump evdevdump.c     # on the Pi
./evdevdump --list                           # every device: name, caps, absinfo
./evdevdump /dev/input/event2                # abs ranges + live events
```

`--list` ends with a summary that is directly usable as configuration: a
`POINT_KIND=abs|rel POINT_DEV=/dev/input/eventN` line for each pointer it finds
(absolute first — the shim prefers a touchscreen or tablet over a mouse), or a
note that only the keyboard map is available if there is no pointer at all.

The live mode prints one line per event with the code names resolved
(`BTN_LEFT`, `REL_X`, `ABS_MT_POSITION_X`, …) and collapses repeats on a single
axis so a drag stays readable. The coordinate transform is **not** derivable on
paper — the repo's two shim generations disagree about whether logical x is `py`
or `1279-py` — so the procedure is: `POINT_DEBUG=1`, click the four corners of
the UI, read the emitted logical coordinates, and correct with
`POINT_SWAP_XY`/`POINT_INVERT_X`/`POINT_INVERT_Y` (or the `TouchCalib_*.dat`
affine) before touching any code.

## `aseqdump2dump` — turning a capture into a fixture and a table

A map cannot be written from a datasheet, and the fixture `make -C scripts/shims
test` replays cannot be recorded without the shim running under `rbp` on the
device — which is not available during bring-up. `aseqdump` needs neither, so
this converts its output into the shim's own dump format
([`mididump.h`](../scripts/shims/mididump.h)) and prints what the map is written
from:

```bash
aseqdump -p DDJ-FLX4:0 | python3 tools/aseqdump2dump.py --stats -o flx4.dump
```

`--stats` lists every (channel, note) and (channel, CC) in the order the
controls were first pressed — so a press order read aloud can be matched to the
output — and for each relative control it decodes the value stream under both of
the conventions controllers use (**0x40-centred** and **`0x01`/`0x7F` two's
complement**) and reports which one the data supports. That verdict is the
useful part: under the wrong convention a steady turn decodes as alternating
steps near half the range, so the correct convention is the one whose largest
step is small.

Only a control that is genuinely relative gets a verdict. A fader or a knob is
not relative at all and decodes to large steps under *both* conventions, so it is
skipped rather than reported as a relative control that failed — and so is the
counts-per-revolution arithmetic, which `--revs N` prints for the platter's own
CC and nothing else (`--jog-cc`, default 34, the DDJ-FLX4's). A "counts per
revolution" for a crossfader is a number about nothing, and printing one makes a
report read as measured where it is not.

Two limits, both because `aseqdump` does not print them: it has **no
timestamps**, so the converted dump's times are synthetic (evenly spaced at
`--rate`; enough for every assertion a map fixture makes, since the jog's speed
is the one thing derived from real elapsed time and `test_flx4.c` pins only its
sign), and it does not carry the bytes of an event it does not name, so such a
line is written into the dump as a comment and counted on stderr rather than
dropped. A dump with **real** times comes from the shim's own `MIDI_DUMP` on the
device.

The converter's output is verified against the consumer, not against itself:
[`scripts/shims/mididump.c`](../scripts/shims/mididump.c) parses it, so
"the file loads" is observable rather than assumed.
