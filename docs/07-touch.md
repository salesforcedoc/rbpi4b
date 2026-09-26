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
the keyboard map to selector rotation and `K_BACK`, so the same mouse that drives
the cursor can also scroll a list and leave a screen. That map is selected by
`EVDEV_MAP`, which defaults to `kbd` — so the wheel and right-button work on this
target without configuration, and the only variable that turns them off is
`RB_EVDEV_MAP=none`. Two readers of one evdev node both get every event, so this
costs the pointer path nothing. See
[08 — Controls](08-controls.md#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone)
and [15 — FLX4 MIDI](15-flx4-midi.md).

### The arrow on the screen (`POINT_CURSOR`)

Accumulating a position is not enough on this target, because **rbp's UI is the
XDJ-RX3's, and that panel is a touchscreen: rbp draws no cursor of its own.**
There is nothing in the binary to reuse. Measured before the shim drew one: aiming
at the INFO button, the click landed at `(1279,0)` — the screen corner — because
the edge clamp had pinned an invisible pointer there. A mouse with no cursor is
not a usability nuisance, it is unusable.

So the shim composites a 12×19 arrow itself, from `fb_cursor.c`, at the live
pointer position. Two properties of this target define how it has to work:

- With `DFB_PRESENT=off` the DirectFB layer surface *is* the fb page, so rbp
  paints its UI directly into the buffer the cursor thread writes to. Nothing can
  be composited *under* rbp's painting — only written between its frames.
- rbp repaints the whole frame at about **60 Hz** (measured). Each repaint erases
  the arrow, and there is no way to tell an erased arrow from an intact one: its
  outline is black, and much of rbp's UI is black too.

Measured on the unit, by drawing a 12×19 block at (640,400) and counting the
redraw intervals that found it damaged:

| redraw period | found damaged | block on screen |
|---|---|---|
| 16 ms (rbp's own rate — the first guess, and wrong) | 91% | 9% |
| 1 ms | 6% | 94% |
| 0.5 ms (**the default**) | — | ~97% |
| 0.2 ms | 1.7% | 98% |

At 16 ms the arrow was on screen about 1.5 ms in every 16, and no framebuffer
capture ever contained one — which is exactly what "the pointer is still
invisible" looks like from the outside. `RB_POINT_CURSOR_MS` is therefore
fractional, and defaults to `0.5`. It is not the mouse's polling rate: the
position is read every tick whatever it says. Raise it toward 1 ms if CPU matters
more than the last few percent of visibility (the cost is in
[13](13-raspberrypi4.md#s32-pointing)).

The cost is bounded because painting is idempotent — a cell already holding the
pixel we would write is not written again — so a tick that finds the arrow intact
is a read pass over 228 cells and nothing else. The restore is conditional for the
same reason: a pixel is only put back where the buffer still holds exactly what
`paint()` wrote there, so rbp's fresh repaints are never pasted over with stale
pixels. Both rules are pinned by `test_cursor.c` under `qemu-arm`.

`RB_POINT_CURSOR=0` turns the arrow off. That is the control for any "is the arrow
what I am seeing?" question — with it off, a scan of `/dev/fb0` finds no arrow at
all, which is how the feature was verified rather than merely eyeballed.

**The real fix for the flicker, not attempted:** a hardware cursor plane
(`drmModeSetCursor` on `/dev/dri/card0`), which the display controller composites
independently of anything rbp paints. It would make the redraw rate irrelevant.
It is out of scope because it means taking DRM master from the mode DirectFB set
up, which is a good way to lose the display.

### What is verified on the Pi, and what is not

`/tmp/pointsrc.log` reads `relative device /dev/input/event7 name='Logitech G203
Prodigy Gaming Mouse'`, and a `12×19`-glyph scan of `/dev/fb0` finds the arrow in
**20 of 20 samples**, at `(640,400)` — the pointer's initial centre, which is where
the shim logs its first paint. With `RB_POINT_CURSOR=0` the same scan finds
nothing, so the scan is measuring the arrow and not the UI.

That the arrow *moves* with the mouse is established from the other side: the
`(1279,0)` click is only reachable by accumulating events from the real device
into the corner, so the event path and the accumulation were already measured
before the arrow existed. What is still the operator's to confirm (S3.2/S3.3 in
[13](13-raspberrypi4.md#s32-pointing)) is the *aim* — that the pixel a click lands
on is the pixel the arrow points at, which is the axis algebra above.

The keyboard half of the reader **has now run on the Pi**: it is selected by
`EVDEV_MAP`, which defaults to `kbd` on this target, and a virtual keyboard was
hot-plugged into a running player to measure the release and hot-add paths
([16](16-input-and-hotplug.md)).

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
