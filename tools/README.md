# tools/

Two kinds of tool live here:

| Tool | Runs on | Language | Purpose |
|---|---|---|---|
| [`patch-rbp/`](patch-rbp/) | workstation | Python | apply the interoperability patches to a stock `rbp` |
| [`build-directfb/`](build-directfb/) | workstation | C / patch | patched DirectFB 1.4.16 (core + fbdev + modules) |
| [`build-toolchain/`](build-toolchain/) | workstation | Docker | the soft-float armel cross build environment the shims are built in |
| [`fit-crosscheck.sh`](fit-crosscheck.sh) | workstation | shell + C | prove the driver's copy and the shim's copy of the fit rule still agree |
| [`fbdump.c`](fbdump.c) | **the Pi** | C | dump `/dev/fb0` geometry/format + say what it means for the present path |
| [`evdevdump.c`](evdevdump.c) | **the Pi** | C | enumerate input devices; find the pointer and its axis algebra |
| [`pcmprobe.c`](pcmprobe.c) | **inside the chroot on the Pi**, or a workstation under `qemu-arm` | C | name ALSA's `snd_pcm_format_t` values, list a card's accepted formats by name, and run the whole open→configure→write sequence against a named device, access and channel count |
| [`aseqdump2dump.py`](aseqdump2dump.py) | the Pi or a workstation | Python | turn an `aseqdump` capture into a replayable MIDI dump, and print the inventory and arithmetic a controller map is written from |
| [`pi-bringup/`](pi-bringup/) | **the Pi** | Python / shell | drive the input path on the unit: a virtual keyboard with a chosen hold time, a raw evdev reader, a state probe, a key-and-capture harness |

Firmware acquisition, decryption and key handling are out of scope for rbpi4b;
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

## `build-toolchain`

The `Dockerfile` for the image `scripts/shims` is built in: `debian:bookworm`
plus `gcc-arm-linux-gnueabi` (**armel/soft-float**, never `…hf`) and
`qemu-user`, so the cross compiler and the emulator that runs the static ARM
test binaries come from one place.

```bash
docker build -t rbpi4b-build tools/build-toolchain/
docker run --rm -v "$PWD:/src" -w /src rbpi4b-build \
    make -C scripts/shims RX3=/src/extracted/XDJRX3-rootfs test
```

Both commands work on a fresh clone — the context is the Dockerfile's own
directory and nothing is `COPY`ed, so the image needs no repository content at
all. Note the **absolute** `RX3=` path in the second one: `make -C scripts/shims`
changes directory before it reads the variable, so a path relative to the
repository root resolves against `scripts/shims/` and the build stops on a
missing `libdl.so.2`. The native equivalent is the same two packages from your
distribution's archive (`gcc-arm-linux-gnueabi`, `libc6-dev-armel-cross`, plus
`qemu-user`), which is what the `RUN` line installs; Docker is a convenience, not
a requirement, and it is the only route that needs nothing on the host.

There is deliberately **no `libasound2-dev`** in the image: no shim includes
`<alsa/…>` — every `snd_*` call is resolved with `dlsym(RTLD_DEFAULT, …)` — and
the one ALSA header used is `<sound/asequencer.h>`, a *kernel* header from
`linux-libc-dev`. Adding it would pull the build machine's (arm64) ALSA headers
into an armel build. Same reason [`pcmprobe.c`](pcmprobe.c) declares its own
`snd_pcm_*` prototypes and links the chroot's libasound.

## `fit-crosscheck.sh` — the two copies of one rule

Where the logical 1280x800 surface lands inside the real framebuffer is decided
by `fbdev_present_fit()` in the DirectFB fbdev driver and by `point_fit()` in
`scripts/shims/point_xform.c` — the same arithmetic twice, because the driver is
cross-built for the target and the shim is a separate `LD_PRELOAD` build, so the
two cannot share a header. They are held together by comments saying *a change to
one is a change to both*, which is a promise rather than a check, and the drift it
lets through is invisible on the unit as shipped: on a 1280x800 panel both
functions return the identity, so a divergence shows up only on the mismatched
panel the fit exists for — as an arrow in the wrong place on a screen the operator
has just swapped in.

```bash
sh tools/fit-crosscheck.sh        # 7.7M sizes, about a second, no target needed
```

It extracts the driver's copy from the **tracked** `directfb-full.diff` — never
from `work/dfb-src`, which `work/dfb-build.sh` rewrites from that diff at step 1,
so reading it would test whatever was last built on that machine — compiles it
beside the shim's real source, and sweeps both over six source shapes and
framebuffers from 16x16 to 4096x2400 in both fit and stretch. A divergence is a
non-zero exit that names both functions and the diff regeneration step.

It fails loudly when it cannot extract rather than skipping: a guard that quietly
does nothing when a path is missing keeps the build green while the reader
believes the two copies were compared. Its limits are worth stating — it checks
the arithmetic, not that the driver *calls* it correctly or that the cursor uses
the rectangle it returns. Those are pinned by each side's own log line: the
driver prints the rectangle in its one-shot `PRESENT:` line
(`/tmp/dfbdig9.log`) and `fb_cursor.c` logs its own, so the two numbers can be
compared on a mismatched panel.

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

## `pcmprobe` — asking libasound what its constants mean

`audioshim.so` deliberately does not link the target's ALSA headers — it links
only `libdl` and `libc`, so it can be built from the armel toolchain without a
sysroot for the chroot's libasound — so it spells out `snd_pcm_format_t` itself.
That hand-rolled copy had `SND_PCM_FORMAT_S24_3LE` as **10**, which is `S32_LE`:
the card answered with a bare `-EINVAL` that named nothing, and the symptom was
silence. The packed 24-bit formats are not adjacent to the padded ones — they
start at 32 — so the wrong value lands on a format that *exists*, which is what
makes it a silent failure rather than a compile error.

It links libasound, so it links the **chroot's** copy — the same one the shim
dlopens. Build it cross, then run it under `qemu-arm` at the workstation, or copy
it into the chroot on the Pi where `/dev/snd` is bound in:

```bash
make -C scripts/shims pcmprobe                                  # cross-build, not shipped
qemu-arm -L extracted/XDJRX3-rootfs scripts/shims/pcmprobe      # names every value
```

Run with no arguments it calls `snd_pcm_format_name()` for every candidate and
prints the mapping **as measured from libasound rather than transcribed**, with
each format's `width` and `physical` bytes — that difference is what `AUDIO_FMT`
actually names. Give it a device (`pcmprobe hw:CARD=DDJFLX4,DEV=0`) and it lists
that card's accepted formats by name, which is the same question
`aplay --dump-hw-params` answers, in the shim's own vocabulary.

It is built on demand and never installed: it is an instrument, not part of the
deploy. `check_format_constants()` in the shim does the same check at every
startup and logs the values it resolved, so this tool is for finding out what the
numbers are — the shim's own line is what catches them later.

Given a device and a format number, it instead **runs the whole configuration
sequence** and prints each step's result — `hw_params_any`, `set_access`,
`set_format`, `set_channels`, `set_rate_near`, the period and periods it
negotiated, `sw_params`, `prepare` and one 0.1 s write — dumping the format mask
after `hw_params_any` and again after `set_format` so a refusal names itself:

```bash
pcmprobe hw:CARD=vc4hdmi0,DEV=0 18 nonblock   # IEC958_SUBFRAME_LE, as the mirror opens it
```

That mode exists because the two questions are genuinely different, and the
difference cost a wrong design on 2026-09-27. `plughw:CARD=vc4hdmi0,DEV=0` on the
**host** advertises the full 32-format logical set and plays `S24_3LE` happily —
through the host's **newer** libasound. Inside the chroot, against the libasound
the shim dlopens, the same plug device offers **exactly one** format
(`IEC958_SUBFRAME_LE`) and answers `-22` at `set_format` for `S16_LE`, `S24_LE` and
`S24_3LE`. So a host `aplay` is not an oracle for the chroot, and the sequence
mode is what settles it: it links the chroot's libasound and reports the same
mask the shim's own open would see. The HDMI mirror's device and format defaults
in `rb.conf` come from that run ([09](../docs/09-audio.md#the-hdmi-mirror)).

Two more arguments follow the `nonblock` one: a fifth sets the **access** mode
explicitly, and a sixth sets the **channel count** (default 2, clamped to 1..8).
Both exist because of one measurement. `set_access()` can be *refused* and the
sequence carries on anyway — that is the point, not a bug in the tool — so the
write loop's verdict is only meaningful if the channel count is one the card can
actually run. The FLX4 is opened at 4 channels, so the two runs that settle it
are:

```bash
pcmprobe hw:CARD=DDJFLX4,DEV=0 32 nonblock 3 4   # set_access ok  → writes 4631 frames
pcmprobe hw:CARD=DDJFLX4,DEV=0 32 nonblock 4 4   # set_access FAILS → every write -EINVAL
```

Measured on the unit, 2026-09-27, with the second of those: `set_access(4)` returns
`-EINVAL`, and that return value is the **only** sign of it. The sequence proceeds
regardless, the params commit **succeeds**, `hw_params` and `prepare` both return
`0`, and then **every** `writei` returns `-EINVAL`. The negotiated line shows the
object left on `0 (MMAP_INTERLEAVED)` — the value `hw_params_any` starts from, which
a refused `set_access()` does not change. The first run, the same binary and the same
card one argument apart, writes 4631 frames.

That pair is why `replay_negotiation()` now asks for `RW_INTERLEAVED`
unconditionally instead of replaying the access the first negotiation recorded: on
this card it is the only access observed to carry frames, and it is what the shim's
write path — `snd_pcm_writei()` on an interleaved buffer — needs. What the probe
does **not** settle is which layer answers `-EINVAL` to the write. The refused
`set_access(4)` never reaches the card, so the second run above is really an
access-`MMAP_INTERLEAVED` run, and it fails exactly as the shim's cold-plugin replay
did. Reading `snd_pcm_hw_params_current()` from the handle, rather than the params
object, is what would name the committed access authoritatively; until that is done,
the rule the shim follows is the empirical one.

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
