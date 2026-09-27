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

`POINT_DEBUG=1` logs every emitted `(down, raw_x, raw_y, lx, ly)`, and since
2026-09-27 also the `wire=(x,y)` the record actually carries — the two are not the
same value, for the reason in "rbp reflects x" below, and printing both is what
makes the two separable in one line. Mirrored or transposed *output* is corrected
with the flags; anything affine is reachable without a rebuild at all via the
`TouchCalib_*.dat` files below.

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

The *device* half of the arrow — where it is allowed to live — is pinned
separately, by `test_cursor_dev`, which links the shipping `fb_cursor.o` against a
faked kernel and asserts that on a panel that is not 1280×800 the arrow is mapped
into the picture rectangle and never into the bars around it. That rectangle is
the same one the present path draws into: `point_xform.c`'s `point_fit()`
transcribes `fbdev_present_fit()`, and `tools/fit-crosscheck.sh` is what keeps the
two transcriptions equal.

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

**The pointer on the unit now is an absolute touch panel, and it draws no
arrow — by design.** On 2026-09-27 the operator's USB touch monitor is what the
shim adopts: `pointsrc: absolute device /dev/input/event3 name='TSTP CTouch'` and
`pointsrc: raw range x=[0..1920] y=[0..1080] swap=0 inv_x=0 inv_y=0`. The panel
presents **two** event nodes (a touchscreen and a secondary interface); the one
with `EV_ABS` + `ABS_X`/`ABS_Y` + `BTN_TOUCH` is `event3`, and `event3` is the
node the mouse used to hold — the numbers are assigned at enumeration, so they are
not stable and neither reader matches on one.

Two consequences that follow from the *kind* being chosen once per process:

* **No arrow.** `pointsrc_cursor()` returns 0 for a non-REL device, so a
  touchscreen is driven by touching it. The 20-of-20 glyph scan above is the
  *mouse's* measurement and does not describe this device.
* **`auto` resolves to one device, and it prefers absolute.** With the panel
  present, a mouse plugged in later contributes nothing until the next restart —
  the mouse is the fallback, not a second pointer. The reader does rescan every
  500 ms, so the panel itself can be unplugged and replugged freely; what the
  restart buys is the *preference*, not possession of the node.

It was **not** found at all until a `RB_POINT_KIND=rel` pin left over from the
mouse was removed — the pin excluded the panel silently (`/tmp/pointsrc.log` had
no `pointsrc:` line at all, because the filter is applied before anything is
logged). A pin is a statement about the hardware as it was.

**The aim is now measured, and the transform is exact.** The panel's raw range is
1920×1080 against a 1280×800 fb, so the open question was whether the pixel a
touch lands on is the pixel under the finger. It is a `point_xform_abs()`
question, not a `point_fit()` one — that function fits the *logical surface* into
the fb for the cursor — and it answers with `range = max - min + 1`
(`point_xform.c:55`), i.e. `raw × 1280/1921` and `raw × 800/1081`. Note the `+1`:
it is the difference between 635 and 636 at the raw middle, and it is the term a
reader is most likely to drop. The operator tapped three places on 2026-09-27
with `RB_POINT_DEBUG=1`, and **every emitted coordinate is reproduced exactly,
with no residual**, by that arithmetic:

| tap | raw | logical, emitted | logical, predicted |
|---|---|---|---|
| top-left | (12, 9) | (7, 6) | (7, 6) |
| centre | (954, 477) | (635, 353) | (635, 353) |
| bottom-right | (1874, 1071) | (1248, 792) | (1248, 792) |

So there is nothing to calibrate: no mirroring, no transposition, no offset and no
scale error, and the affine `TouchCalib` hook is not needed for this panel. The
centre tap reads 47 px above the logical middle, and that is the *finger* rather
than the fit — the corner taps land at 0.8% and 99.2% of the panel, symmetric, and
a linear map preserves midpoints, so raw y 477 simply is 44.2% of 1080.

## rbp reflects x, and `tscfake_emit()` undoes it

That "no mirroring" is a statement about `point_xform_abs()` and it is true, but
it is **not** a statement about where a tap lands. The consumer is what is
mirrored: **rbp acts at `POINT_LOGICAL_W - 1 - x` of whatever x the record
carries**, so a shim that writes `lx` straight through puts every tap on the
mirror of the finger's pixel while getting y exactly right. The symptom is
precise and was the operator's report on 2026-09-27 — *"i can select certain
songs but i can't access the icons on the left"* — because the left sidebar
occupies x 0..100, which under the reflection is the INFO/LOAD column at the far
right, and **no tap can land on a sidebar cell at all**; the track list, being
full width, kept working, which is what made it look like a partial failure of
the touchscreen rather than a coordinate error.

It is rbp's own quirk, not the panel's and not this file's, and the proof is the
finger in the table above: at raw `(1874,1071)` the whole chain emits logical
`(1248,792)` — the far corner to the far corner, honest — so the reflection
happens *after* the transform, inside rbp. It was pinned at five points by
writing known records into the pipe rbp holds both ways (`work/tap.py`) and
reading the result off the framebuffer: x `1229` → the cell drawn at 8..50, `90`
→ deck 2's LOAD at 1152..1272, `42` → INFO at 1180..1275, `50` → fell *through*
the sidebar column into the list, `200` → fell outside that LOAD button. Slope −1,
intercept 1279.

It is undone **at the wire, in `tscfake_emit()`** (`tscfake.c`), which is where
rbp's other two consumer quirks already live — the duplicate suppression and the
two-frame press burst — for the same reason: they are properties of the consumer,
not of the device. Reflecting there and not in `point_xform.c` keeps
`point_xform_abs()` describing the *panel's* axes honestly, and keeps
`st_cursor_x`, `pointsrc_status()` and the composited arrow meaning "where the
finger is" rather than "where rbp will act". It applies to the relative path too,
and that is a property of rbp rather than a choice: an absolute panel and a mouse
both arrive as records on one pipe, and nothing in a record says which produced
it — so correcting only the absolute case would leave a mouse's click on the
mirror of the pixel its arrow points at.

Do not "un-mirror" this from the outside. The record's x is *supposed* to be
`1279 - lx`, and a reader who reverts it puts every tap back on the opposite side
of the screen. A tap landing on the wrong side is diagnosed in one line by the
`POINT_DEBUG` log: raw vs logical tests `point_xform_abs()`, logical vs wire tests
this quirk.

The *effect* — that tapping a control operates it — **was confirmed by the
operator's finger on 2026-09-27**, which is S3.3 in
[13](13-raspberrypi4.md#s32-pointing): the sidebar, deck 2's scrubbing and the
list rows all act where the finger is. The finger's own records are in
`/tmp/pointsrc.log` (25 points across logical x 0 → 1251, every one
`wire = 1279 − logical`), so the aim and the effect are measured by the same
line.

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
