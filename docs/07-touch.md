# 07 — Touch / pointing

An HDMI monitor has no touchscreen, so on the Pi this subsystem is really
**pointing**: `rbp` is driven by a mouse, a trackball, or a USB touchscreen if
you have one. The rbp-facing contract does not change — it still expects a
tsc2007 resistive panel at `/dev/tsc2007_2-0048` — but where the coordinates
come from, and how they are transformed, is now discovered rather than assumed.

## Stack

`fbshim.so` is built from four sources that used to be one file
(`fbshim-tsc.c`):

| File | Job |
|---|---|
| `fb_shim.c` | the fb ioctl lies, `/dev/mem` → `EACCES`, the env-gated pan pacer, the `gpiodrv` read-zero/poll-sleep stubs |
| `tscfake.c` | the fake `/dev/tsc2007_2-0048` — the RX3 protocol, unchanged |
| `pointsrc.c` | finding the pointing device |
| `point_xform.c` | raw device coordinates → logical UI coordinates |

`rbp` opens `/dev/tsc2007_2-0048`; `tscfake.c` serves it a pipe that a reader
thread fills with the 6-byte RX3 record. Discovery, scaling and the relative-
mouse path all sit on the *source* side of that pipe, so none of it is visible
to `rbp`.

## The tsc2007 facade — unchanged, deliberately

This is rbp-facing ABI and target-independent, so it is byte-for-byte what it
was: the ioctl magics (**`0x6b`**), max X = 3 / max Y = 3900, the 6-byte
`{u8 flag; u8 pad; u16 x; u16 y}` record, the fd table + pipe, and the 2-frame
debounce burst.

The protocol contract is the thing to hold on to: **the shim pushes
`(logical_x, logical_y)`** — already in the UI's 1280×800 space. Everything
that differs between a touchscreen and a mouse is resolved before the write.

## Finding the device (`pointsrc.c`)

The previous target's shim looked for `/dev/input/event0` by name, which was
correct because the ILI2117 was always there. Nothing is guaranteed on a Pi, so
the shim discovers instead:

1. scan `/dev/input/event*`, read the name with `EVIOCGNAME`;
2. match the capability set — `BTN_TOUCH` + `ABS_X` + `ABS_Y` for an absolute
   device, `REL_X` + `REL_Y` + `BTN_LEFT` for a relative one;
3. re-scan on `ENODEV`, so unplugging and replugging works.

`POINT_KIND=auto|abs|rel|none` picks the preference (default `auto`: absolute
first, on the grounds that a touchscreen or tablet is a better DJ input than a
mouse). `POINT_DEV=/dev/input/eventN` pins one explicitly.

## The transform (`point_xform.c`)

Each axis is scaled from its `EVIOCGABS` min/max into the logical dimension:
`scale = dim / (max - min + 1)`, with `POINT_SWAP_XY`, `POINT_INVERT_X` and
`POINT_INVERT_Y` as the correction flags. This is a strict generalization of
the previous target's math — that behaviour is reproducible with `swap=1`,
`min=0`, `max=2047`.

**The transform is not derivable on paper, and this document will not pretend
otherwise.** The repo's own two shim generations disagree about whether logical
x is `py` or `1279-py`, which is exactly why the source discovery and the axis
maths were split into separate files with separate env knobs. The procedure is
measurement:

```sh
POINT_DEBUG=1 POINT_KIND=rel POINT_DEV=/dev/input/event2 sh /opt/rblive4/start-rb.sh
#   then click the four corners of the UI and read /tmp/pointsrc.log
```

`POINT_DEBUG=1` logs every emitted `(down, raw_x, raw_y, lx, ly)`. Mirrored or
transposed output is corrected with the flags; anything affine is reachable
without a rebuild at all via the `TouchCalib_*.dat` files below.

## Relative pointing (mouse)

Since the monitor has no touch panel, `REL_*` events accumulate into an
absolute cursor: it starts centred, moves by `delta × POINT_MOUSE_SPEED`
(default `1.0`), and `BTN_LEFT` is down/up.

`POINT_MIN_DWELL_MS` (default `45`) holds a too-short tap open long enough for
rbp's debounce and UI thread to see it. **Without it, fast clicks are
swallowed** — the single most likely "the pointer doesn't work" report on this
target, and the first thing to raise if taps seem to need holding.

The wheel and right-button do not go through the pointer path: they are bound in
the keyboard map (`RB_MIDI_MAP=kbd`) to selector rotation and `K_BACK`, so the
same mouse that drives the cursor can also scroll a list and leave a screen.
Two readers of one evdev node both get every event, so this costs the pointer
path nothing. See [08 — Controls](08-controls.md#the-keyboard-map-rb_midi_mapkbd)
and [15 — FLX4 MIDI](15-flx4-midi.md).

As with the rest of the port, that binding is written, cross-compiled and
fixture-tested against synthetic events — it has **never been run with a real
mouse or keyboard on the Pi**.

## Required files

`rbp` reads `root/settings/TouchCalib_User.dat` (fallback
`TouchCalib_Factory.dat`). The shim's transform assumes an **identity**
calibration, so both files must contain:

```
0
0
320
200
1280
800
```

Line order is load-bearing (offX, offY, scaleX, scaleY, checkX=1280,
checkY=800). `scripts/build-chroot.sh` installs them. A non-identity calib is
the escape hatch for any affine error the transform flags cannot express.

## Measuring it: `tools/evdevdump`

```sh
gcc -O2 -static -o evdevdump tools/evdevdump.c     # on the Pi
./evdevdump --list                                 # name, caps, absinfo per device
./evdevdump /dev/input/event2                      # abs ranges + live events
```

`--list` ends with a summary directly usable as configuration — a
`POINT_KIND=abs|rel POINT_DEV=/dev/input/eventN` line per pointer, absolute
first — or a note that only the keyboard map is available if there is no
pointer at all. It replaces `touchdump.c`, which only ever looked at
`/dev/input/event0`.

## For reference: the previous target

The SC Live 4's **ILI2117** capacitive panel sat on `/dev/input/event0` with a
2048×2048 raw range and reported `ABS_MT_POSITION_X`/`ABS_MT_POSITION_Y`. Its
raw events were confirmed with the (now removed) `tools/touchdump`:

```
axis ABS_MT_POSITION_X min=0 max=2048
axis ABS_MT_POSITION_Y min=0 max=2048
... type=3 code=53 (X) value=970, code=54 (Y) value=1608 ...
```

That range is why the transform generalizes so cleanly: with `swap=1`,
`min=0`, `max=2047` it reproduces the old algebra exactly.

## Notes

* The browse-caution gate patches (`0x2dc228`/`0x2dc46c`) are in `rbp-audio`;
  the controls shim additionally clears the caution id at `0x05a191fc` while
  USB1 is mounted.
* `/dev/mem` returns `EACCES` from the shim, not from the kernel — `rbp` maps
  i.MX6 physical registers and must be blocked from succeeding.
