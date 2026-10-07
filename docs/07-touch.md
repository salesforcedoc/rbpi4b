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

### The position the reader starts from (`seed_abs_position`)

**The kernel drops an `ABS` event whose value is the one the device already
holds**, so a report is not a complete description of where the finger is: it
carries *the axes that changed*. A loop that only ever assigns what it is handed
cannot tell "x is 0" from "no x was reported", and the absolute reader used to
start at `rx = ry = 0` — the top-left corner.

**Measured on the unit, 2026-09-28, and it cost a session.** After a service
restart, 18 consecutive injected taps at raw `x 83` were read by the shim as
`raw=(0,…)`: the device already held `x 83`, so every report's `ABS_X` was
filtered, and the reader's x was still the zero it started with. All 18 landed
outside the QUANTIZE zone (logical x 0) and none reached rbp as a tap. One report
that moved x — `poke.py move:400,900` — fixed it, which is what made the cause
legible. It was first read as "the tap is unreliable", and the runs that said so
are unusable as evidence for anything else.

The same hole applies to a real finger, and it is the first touch of a run that
shows it: the panel's stored x is the last position it reported, so the *first*
touch after a restart at that same pixel arrives with no `ABS_X` in it, and the
tap is delivered to rbp at logical x 0. Twice in a row is the same, from either
process, until some report moves the axis.

The fix is one ioctl each way at attach: the reader seeds `rx`/`ry` from
`EVIOCGABS(ABS_X/ABS_Y)` — the device's own stored values — which gives the loop
an invariant that then holds for its whole life:

> `rx`/`ry` equal the device's stored values, always. True at the start because
> of the seed; preserved afterwards because a value that changes always arrives
> as an event, and a value that does not is already held here.

Both axes are required of an absolute candidate (step 2 above), so neither ioctl
names an axis the device lacks. **Proven on the unit**: with the fix deployed, a
report containing *only* `MT_TRACKING_ID` and `BTN_TOUCH` — no position event at
all, the exact shape of a first touch on the point the panel last held — was read
as `logical=(55,754)` and logged as `deck1 QUANTIZE tap at (55,754)`. Before the
fix that same report was `(0,0)`.

The lesson for the instruments is the same fact from the other side:
`work/qtap.py`, `work/qprobe.py`, `work/qtrace.py` and `work/qwarm.py` all prime
with a `move` that changes **both** axes now. A prime that varies only y leaves
the press carrying no x, which is a silent way to measure the reader's x instead
of the thing under test — [12](12-troubleshooting.md).

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

`POINT_DEBUG=1` logs every report the panel produces — `(down, raw_x, raw_y, lx,
ly)`, written as each `SYN_REPORT` arrives and **before** anything downstream
decides what to do with it — and since 2026-09-27 also the `wire=(x,y)` the record
actually carries. Two readings follow from that, and the distinction is worth
keeping: `raw`/`logical` describe the finger and the transform, so this line is the
instrument for *where the finger is and what the shim makes of it*; it is **not**
evidence that rbp received anything, because a report the menu swallowed is logged
exactly like one that was passed on. What proves the swallow is a frame diff — and
for a *strip* tap it no longer can, since the replay gives rbp the press back 45 ms
later and both cases end with rbp reacting; there the replay's own log line is the
separator (`pointsrc: strip tap at … -> replayed to rbp as a press`). See "the
swipe-down top menu". The `wire` field exists for the other reason in "rbp
reflects x" below: `logical` and `wire` are not the same value, and printing both is
what makes the two separable in one line. Mirrored or transposed *output* is
corrected with the flags; anything affine is reachable without a rebuild at all via
the `TouchCalib_*.dat` files below.

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

**The real fix for the flicker, being attempted:** a second DRM plane on
`/dev/dri/card1`, which the display controller composites independently of
anything rbp paints. It would make the repair rate irrelevant.

*This paragraph used to name `/dev/dri/card0` and `drmModeSetCursor`, and both were
wrong:* `card0` is `v3d`, the render node, and `card1` is `vc4-drm`, which is what fb0
(`vc4drmfb`) scans out of. The device inventory, the two instrument traps and the
result are in [the overlay-plane section](#the-overlay-plane-on-card1--the-cure-that-does-not-go-through-rbp)
below; what belongs here is only the pointer, because this is the section a reader
arrives at when they ask why the arrow costs what it costs.

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
with `RB_POINT_DEBUG=1`, and **every logged coordinate is reproduced exactly,
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

## The two deck QUANTIZE boxes (`touch_zone.c`)

Every other touch in this document belongs to rbp: `pointsrc` hands the record to
`tscfake_emit()` and rbp decides what it hit. The deck QUANTIZE boxes are the one
place the shim supplies a binding instead, and it does so because rbp does not
have one — on the performance screen.

**Measured, 2026-09-28.** Records written into rbp's own record pipe (the
instrument S3.3 used) at the widget, at both x conventions, left the frame
**pixel-identical**, while a positive control in the same run (a tap on INFO)
toggled the screen — so the injection reaches rbp's handlers and the tap is
simply not wired to an on/off. rbp's binary agrees: it has a *touch class* for
the **Beat FX** quantize and, for the deck's, only value key handlers
(`UiKey_Shortcut_QuantizeValue{1,1_2,1_4,1_8}`) — there is a way to *set* the
value, and no way to *toggle* it by touch.

So `pointsrc.c` sends the keycode rbp already has for that button. The keycode is
derived from the binary rather than guessed: `ui::PlayerInnards::onPhysicalKey()`
@ `0x306b78` is rbp's keycode → handler dispatch (a GCC binary search over the
code at `IKeyInput+8` whose leaves tail-branch into the `onKey_*` handlers, and a
`PlayerInnards` vtable slot, which is how `IKeyManager::onKey` reaches it), and
**`0x410b` is the one code whose leaf reaches `onKey_Quantize()`** @ `0x3028bc`.
The same tree reproduces every keycode `rbp_abi.h` already names — PlayPause
`0x4101`, Vinyl `0x4104`, Sync `0x4112`, HotCue `0x4113`, Pad `0x4117..0x411e`,
LoopIn/Out/Reloop `0x410c/d/e`, jog `0x4305/0x4306` — so the reading is
cross-validated rather than inferred.

`onKey_Quantize` is the right route and not merely the tidy one. It never looks at
the keycode: it takes the deck from its own channel, flips
`DjEngineIF::setDeckQuantizing(ch-1, !isDeckQuantizing(ch-1))`, and then calls
into `IPlayerSetting` — and *that* last call is what repaints the widget. Driving
the engine directly leaves the screen behind; sending the key is the whole
gesture, display included.

### The rectangle, and the one screen that disagrees

The geometry is a measurement off a captured frame (`/dev/fb0` of the performance
screen, decoded as RGB565), not algebra — this is UI geometry like `point_fit`,
so it is literals in `touch_zone.c` and **not** env-configurable:

| | deck 1 | deck 2 |
|---|---|---|
| label `QUANTIZE` | x 27..88, y 736..746 | x 667..728 |
| value field | rules at y 754 and 776, x 26..86, digit at x 56 | x 666..726 |
| widget | x 26..88, y 736..776 | +640 px |

Deck 2 is **+640 px, not a mirror** about the centre — a mirror would put it at
x 1191..1253, and a mirror is what a reader would assume. The tap target is the
widget plus a 4 px margin (x 22..92, y 732..780), because the margin is what
covers the noise in where exactly the edge the finger is aiming at is.

**The bounds are inclusive, and the gesture is complete on the down edge.** A
press is emitted as a two-frame burst and a resting finger is a stream of identical
reports, so only the transition into *down* counts; the caller (`pointsrc.c`)
sends the key's press and its release together, exactly as `aloop_apply()` does
with a pad, so rbp's `KeyManager` bookkeeping is left clean and the toggle happens
under the finger rather than on lift. That removes the whole family of stuck-key
and stale-release states: a drag that passes *through* the box never started
there, and a device that reports a zeroed coordinate on release cannot complete a
gesture. `touch_zone_reset()` is kept for the one case that needs it — a pointer
unplugged while down would otherwise make the next device's first press look like
a continuation, and swallow one tap per replug.

**The burst has a first-touch case, and it was broken until 2026-09-29.** The
emitter's "nothing emitted yet" initialiser was `last_down = -1`, and `-1` is
truthy — so the up→down test that emits the second frame was false for the
process's *first* press, which went out as a single frame and was discarded by
rbp's hysteresis. The first touch after every start therefore did nothing, and it
is the one press that is always up→down because a device nobody is touching reads
released. `tscfake.c` now carries a separate `primed` flag; the same mistake is
worth watching for in anything else that encodes "unset" as a non-zero in a
variable that is also read as a boolean. Two measurements: with the gate off,
seven presses at rbp's INFO control differing only in length (150, 150, 45, 45, 30,
75, 150 ms) had #1 do nothing and #2..#7 each flip the view
(`work/menu17.sh`); after the fix, `work/menu19.sh`'s ON phase has the process's
first touch react with 549,764 px ([12](12-troubleshooting.md)).

**On the browse screen the same rectangle is not free, and this is the collision
to expect.** That screen's bottom band is the deck strip too, and rbp binds a
control of its own over the deck-1 time display there: a touch on it toggles
`TIME`/`REMAIN`. Measured by injection the same day, in one run —

| gesture (wire x 1224 = logical x 55) | frame |
|---|---|
| `null` ×1 | 416 px (the idle baseline) |
| tap y **755** | 2940 px — `REMAIN 03:39` → `TIME 01:47` |
| the same tap again | 2940 px — and back to `REMAIN 03:39` |
| tap y **745** (the label row) | 208 px — inert |
| tap y **760** | 2732 px |

Two identical injections flipped that label and flipped it back, so it is the
injection driving an rbp touch class rather than a stray finger, and the class
starts between those two rows. So a press there on the browse screen does **both**
things: rbp's own time-mode toggle, which is unchanged behaviour, and this
feature's quantize flip. The overlap is total in both axes and cannot be trimmed
away — the browse screen's live band begins immediately below the `QUANTIZE`
label row, so any rectangle that covers the field the operator aims at covers
rbp's control too, and any rectangle that misses it is off the field.

`RB_POINT_QUANTIZE_TAP=0` (or `POINT_QUANTIZE_TAP=0` for one run) turns the tap
off and leaves the whole screen to rbp, which is the way out if the double action
turns out to be worse than the feature is worth. The step-0 plan's other
fallback — narrowing the zone to dodge it — is the one the measurement rules out.

**The one thing no machine here could settle is now settled by a hand** — asked
directly, the operator's verdict on the feature is *"quantize works correctly"*
(2026-09-29). That covers the two halves injection could only ever proxy for: that
the target is comfortable under a finger, and that quantize really **engaged**
rather than only repainting the widget. The feature therefore has no item
outstanding, and the collision above is the live caveat rather than an open one.

## The swipe-down top menu (`menu_zone.c`, `menu_draw.c`)

The operator's ask: virtual buttons at the top of the screen — SOURCE, BROWSE,
TAG LIST, PLAYLIST, SEARCH, MENU — each mapped to the button it stands for, reachable
by a swipe down from the top edge. On this unit the hands are on an absolute touch
panel, and those six functions otherwise live only on the keyboard map ([08](08-controls.md))
or, for SOURCE alone, behind a shift-push on the FLX4.

**The seventh column, `USB STOP`, was the operator's second ask on the same panel**
— *"yes add a USB STOP button to the menu"* — and it is a different kind of button
from the other six in one way that matters: the six are conveniences for controls
this rig can already reach another way, where USB STOP is rbp's **safe eject**
(`K_USBSTOP 0x8002`, [08](08-controls.md)) and is reachable from **nowhere else at
all** on this hardware. The operator's reason for wanting it is why it exists:
*"rekordbox has a stop button just for that purpose to safe eject, but its not mapped
currently, so i really don't want to pull the USB as it might continue to corrupt the
USB stick"* — so the alternative to this button is yanking live media, and the media
already carries pre-existing FAT damage. It is the FLX4's missing button, on the glass.
Two things follow from that and are [08](08-controls.md)'s to state in full: this key
needs **press *and* repeat** (the press mutes and notifies, the repeat is what asks the
db to stop the device), and **a tap really does unmount the stick** — it is not a
rehearsal.

### The seventh column raises a chooser (`prompt_zone.c`, `prompt_paint.c`)

**The ask:** *"when you have USB stop, put up a prompt for USB1, USB2 or Cancel"*
(2026-10-05). One tap on the far right of the band used to unmount the operator's stick
outright, and the stick is production media whose FAT already carries pre-existing damage
([10](10-usb.md)). The fix is not to make the button safer — it is to make it ask first.
The seventh column now raises a **box**: title `USB STOP`, then **one line of two
buttons**, `HOLD USB 1 | HOLD USB 2` — or, since 2026-10-07, `HOLD <the stick's own volume
label>` on each, which is the same string rbp's SOURCE row shows in its DEVICE NAME column
([10](10-usb.md)). The gesture the first layout carried — two taps,
the first arming a device and `OK` sending it — was replaced the next day by the
operator's own ask, quoted in full in `prompt_zone.h`: *"for the USB stop pop up just have
two buttons USB1 (hold) or USB2 (hold), lit if it is active and when you press down have
the button flash and if still held after 3 seconds have it ejected. if they release it
before three seconds don't eject it."* So `OK` and `CANCEL` are gone, `PR_ACT_CANCEL` with
them, and what reaches an eject is a **three-second hold** on a lit button. The box is one
gesture rather than two steps, and what makes it safe is that nobody holds a button for
three seconds by accident.

**The rows are rbp's own two devices, not a list the shim made up.** rbp holds
`ui::UsbStorageManager` — **one instance per channel** — and its `onKey` @`0x3259f4`
drops any key whose channel byte is not its own (`[key+0xa] == [this+0x84]`). So a
chooser that names a device is a chooser that can *stop* one device, which is what makes
USB 1 and USB 2 real rows rather than two spellings of the same action: choosing USB 2
sends `K_USBSTOP` on **channel 2**, where the old binding could only ever have sent
`CH_GLOBAL` — which *is* USB 1's number. `pointsrc_usb_state()` in `pointsrc.h` is the
walk: it reads `[mgr+0x84]` for the channel and `[mgr+0x88] == 2` for "media present and
ready", and it returns both devices **absent** when rbp cannot be read at all — a
refusal, not an assumption of absence.

**A dead button is drawn dead, and says nothing.** A button whose device rbp reports absent
is painted in `MENU_BTN_OFF`/`MENU_LABEL_OFF`, **cannot start a hold**, and — unlike
the old dead row — does **not** close the box either: the operator who pressed the dim one
probably wanted the other, and the box staying up is what lets them have it. The refusal is
deliberate, and it is a refusal the operator can see rather than a button that silently does
nothing. Since 2026-10-07 both buttons are live whenever both slots are filled:
`usb-watch.sh` feeds `/media/usb1/sda1` **and** `/media/usb2/sda1`, so rbp answers for both
and the log reads `pointsrc: menu 'USB STOP' -> the USB STOP chooser (usb 1 ready 'HOLD
PHASIM_USB2', usb 2 ready 'HOLD RBOX USB')` — the labels in it are the ones the buttons
will read, word for word, so a drill's log says which stick each button offered without a
screenshot of the glass. Before that it fed one path only and USB 2 answered `usb 2 absent`
(measured 2026-10-05) — which is what the button still does when that slot is empty, because the
liveness is rbp's answer and not a guess. The liveness is re-read on **every report** and
again at the fire, so a device that goes away under a running hold takes the hold with it;
host coverage for that is `test_menu_dev.c`'s liveness leg (it flips the fake state, proves
with a `memcmp` that the rebuilt picture actually differs, and flips it back) and
`test_prompt.c`'s vanished-device leg.

**THE GESTURE IS ONE THREE-SECOND HOLD, AND THE THREE SECONDS ARE THE CLOCK'S AND NOT THE
CALLER'S.** A press on a **live** button stamps the instant and starts the hold; the button
**flashes** — the pressed pair for `PR_HOLD_FLASH_MS` (250 ms, rbp's own house half-period,
so six full blinks fit inside one hold) and its normal live face for the next — so the
operator can see the three seconds running. At `PR_HOLD_MS` (3000) the eject goes out on
**the device's own channel** and the box **stays up**. A release before that sends nothing
at all and leaves the box standing, which is the operator's *"if they release it before
three seconds don't eject it"*. Leaving the button cancels the hold and it does **not**
resume on a return: a hold is one continuous press or it is nothing, because a hold that
could be left and rejoined is one step from an eject the operator did not watch themselves
make. The module has no clock of its own ([32-bit long wrap](13-raspberrypi4.md)), so
`prompt_tick()` is handed `now_ms` on every slice of `pointsrc.c`'s read-loop wait and is
the only thing that advances the flash or fires the hold — a hold by definition produces no
further pointer reports, so nothing else would wake the loop. The three seconds are also
pinned on the **release** path, so a release arriving at 3001 ms with no tick in between
still ejects.

**The box stays up under the finger that made the hold**, and the reason is the one rule
this shim keeps everywhere: a press the box swallowed has been withheld from rbp for its
whole life, and rbp is going to be given its release
([declined press](15-flx4-midi.md)). So the fire **disowns** the press — anchor, highlight
and hold all cleared, box still up — and the release then arrives with nothing anchored to
it and takes the ordinary "began off every button" path, which closes the box and sends
nothing.

**The rest is rbp's rule, not a new one.** While the box is up it owns every report
unconditionally — it is asked **first** in `pointsrc.c`'s ladder, before the waveform swipe
and before the menu — so a finger that dismisses the box cannot also press whatever the
performance screen has underneath it. A stray release with no press behind it is taken but
does not close. The way out without stopping anything is a **tap anywhere that is not a
button** — the title, the rule, the margin, the gap between the two buttons, the glass
outside the box entirely — which is the "tap outside to dismiss" rule the box already had
for a press that missed, so it needed no new mechanism when `CANCEL` went away. Behind it,
the box still **times out on its own after `PR_TIMEOUT_MS` (10 s)**, and it refuses to
expire while a finger is down, so it can never hand rbp a release with no press behind it.
A hold that completes beats the timeout: three seconds of a finger is a decision, and
starting it late in the box's ten does not unmake it. Measured:
`pointsrc: usb stop chooser timed out after 10000 ms` and
`pointsrc: usb stop chooser -> usb 1 held 3020 ms (hold is 3000 ms) -> eject (channel 1)`.

**Where it is drawn.** The box is 560×127 logical px, centred, on its **own DRM overlay
plane** — the fourth `drm_band_setup()` on this rig, which is legal because `drmband.c`
holds the fd and DRM master once and refcounts them. It is built off-lock into a scratch
image and published by swapping two pointers, the same publish-a-finished-image rule the
band and the drawers follow; a fresh dumb buffer is uninitialised, so `drm_band_show()`
waits until a real image exists. A machine that refuses a box-sized plane falls back to
painting straight onto rbp's page (`prompt: no plane for the box -- it goes on the page`)
and keeps working.

**The box was restyled to match the bar the same day the bar was (2026-10-06)** — *"model
it this way for the USB stop menu as well"*. Its outer `PR_BORDER`-thick frame is gone,
so the box is the black `MENU_FILL` bed with a title, a rule and its buttons; each button
already wore its own one-pixel outline in its own ink colour (`pp_cell()`), which is what
remains. `PR_BORDER` itself did not move: it was always doing two jobs, and the one that
stays is the margin that keeps the title and the buttons off the box's edge.

**The second line went with the gesture, and every number in this section moved with it.**
The 2×2 grid (`USB 1 | USB 2` over `OK | CANCEL`) became one row of two, so `PR_H` went
199 → **127**, exactly `2*PR_BORDER + PR_TITLE_H + 1 + 2*PR_PAD + PR_ROW_H` — the row is the
box's only line, and `PR_ROW_GAP` survives with nothing able to reach it because it is
`PR_ROW_TOP()`'s stride and a third device would need it. The box is logical x 360..919 /
y 336..462, the rule is `PR_RULE_Y` **379**, and the row sits at `PR_ROW_TOP(0)` = **388**,
64 px tall. A button is still **265 px** wide: `PR_COL_GAP` (8) comes out of the inner span,
so the two are 371..635 and 644..908 and the gap between them is a **hit test miss**, like
the title and the margin. The cell's own geometry is derived in `prompt_zone.h` —
`PR_CELL_ROW`/`PR_CELL_COL` out of the row-major cell number — so the two buttons tile the
block rather than each carrying its own rectangle, and the leftover pixel a 538-wide block
would leave over is dropped rather than handed to one column (the test asserts the leftover
is 0, which is what makes the two interchangeable). `work/poke.py` re-derives all of it and
is the check that the drill is aiming at the same pixels the module draws.

**The labels say `HOLD USB 1`, not `USB 1 (HOLD)`,** and that is the font's doing: the 19 px
Decker atlas has no parentheses, and re-baking the settled face for two punctuation glyphs
would change every label in the shim ([the band](13-raspberrypi4.md)). The gesture has to be
on the button — the operator has to know a hold is wanted before they make one — and at
**99 px** of advance ("HOLD USB 1"; ink 94 px, and "HOLD USB 2" is 97) it has 164 px of the
265 px button to spare. `test_prompt.c` walks every character of both against the atlas,
because a character the font lacks ships as a **gap** rather than as a failure.

**And a button names its own stick when the host gave us one.** `pointsrc_usb_state()`
fills `S->label[i]` from `/tmp/udev_usbN.label` — the file `usb-watch.sh` writes with
`blkid`'s answer, the same one the SOURCE row uses ([10](10-usb.md)) — and
`prompt_zone.c` composes `"HOLD " + <label>` over it. An absent, empty or
whitespace-only file leaves the cell empty and the shipped `HOLD USB n` stands, so a unit
whose watcher writes no labels is unaffected. **The refusal and the clip are two
different questions and are asked of two different strings:** `pp_labels_fit()` measures
the cell's **default** label, because a name is the operator's to choose and a
state-dependent refusal would take the whole box off the glass the moment someone
formatted a stick with a long name; the name itself is cut to the cell at paint time
(`prompt_text_clip()`, budget = the cell's inner width less `PR_LABEL_MARGIN` a side).
Both use that one constant, which is 2 and not the frame's 1 because the thinned Decker
bake inks up to a pixel past its own advances. `test_prompt.c` pins the composition, the
per-device isolation, the `?` remap and the `PR_LABEL_MAX` cap, and then paints a
maximum-length name and proves **no pixel of it lands on the button's frame** — the frame
is drawn before the label, so an over-generous budget would ship as a name painted over
its own border and nothing else here would call that a failure.

**Two width refusals came with the grid, and the second one is why the labels' length
matters.** A button is half the row wide, so a page scaled far enough down can reach a
scale where the font's line box still fits but a label no longer does.
`prompt_paint_ok()` refuses both (`pp_labels_fit()` beside the line-height refusal), and
`test_prompt.c` pins them independently — a view at 560×81 is accepted and the same view at
40 px wide is refused, which is what proves they are two gates and not one. The longer
labels raised the per-button floor from `2 + 69` = 71 px to `2 * PR_LABEL_MARGIN + 99` =
**103 px**, so a page narrow enough that a button falls under 103 px now draws **no box at
all** where the old `CANCEL`-sized one still drew.

**The picture, captured from the publisher's own plane buffer.** `work/boxshot2.py` reads
the plane by framebuffer id from **outside** rbp — the obvious route, `/proc/pid/mem` at
the address the shim logs, does **not** work and the failure is worth knowing: vc4's dumb
buffers are `PFNMAP`-style mappings and `get_user_pages()` refuses them, so the read gives
`EIO` at a perfectly live address, for the band's plane as much as the box's. **Measured
on the unit 2026-10-07, on the hold build** (`fbshim.so` md5 `656fcc2f…`, which is the
build *before* the buttons learned the sticks' names — it reads the shipped `HOLD USB n`):
the box took
`plane 138 fb 725 560x127 16 bpp pitch 560 px`, and the captured frame is a black bed
with `USB STOP` at the top, the rule under it, and **both** buttons carrying their own
one-pixel outline with `HOLD USB 1` and `HOLD USB 2` legible inside them — because both
slots were filled, which is the same log's `usb 1 ready, usb 2 ready`. A button whose
device rbp does not report draws in the dim pair instead, and that is the face the
2026-10-05 capture showed when only slot 1 was fed. The **held** state is the second
picture and the more interesting one: `work/poke.py <dev> usbstop chooser:usb1` holds USB 1
and leaves the box up, and the capture then reads `HOLD USB 1` in the pressed pair —
which is the visible form of the flash. **That hold has deliberately not been run on the
unit**: `poke.py` refuses a dwell of `PR_HOLD_MS` or more, and anything shorter is still a
press on a live eject control, so the flash and the dim face are pinned at the desk
(`test_prompt.c`, `test_menu_dev.c`) and it is the operator's own press that will show
them on the glass.

**The name build is deployed but not yet captured.** `fbshim.so` md5 `06eadca1…` went to
`/opt/rblive4/fbshim.so` and the chroot's `/usr/lib/fbshim.so` on 2026-10-07, with the
`656fcc2f…` build kept beside each as `.prev`, and `rblive4.service` restarted onto it
(rbp back up, both `ld-linux.so.3` processes, and the shim's own log reporting
`device name … <- 'PHASIM_USB2'` and `<- 'RBOX USB'` into rbp's records). What is verified
is that the box **composes and fits** the unit's real labels, read back through the
production `prompt_zone.c`/`prompt_paint.c` at the unit's own
`/proc/<rbp>/root/tmp/udev_usbN.label` bytes; what is **not** yet verified is the picture,
because the box has to be raised to have one and raising it is a gesture on the glass.

**And a picture is only ever reached through the gesture.** Both the held picture and the
dim one are produced in `test_menu_dev.c` and `test_prompt.c` by feeding `prompt_feed()`
the reports the operator's finger would have made, never by passing a highlight in as an
argument: the highlight is `prompt_zone.c`'s, and a test that drew its own would be
asserting against itself. The same is true of the flash — it belongs to a running hold, so
the only way to a lit button in a test is to start one.

**`POINT_USBSTOP_PROMPT=0` puts the immediate eject back** for bench work, and it is a
keycode test rather than a column test (mirroring `menu_key_needs_repeat()`): whatever
column carries `K_USBSTOP` is the one that raises the box, and a column that no longer
carries it is unaffected. Both the tap and the hold are caught before either is sent, so
the eject is reachable **only** from the box.

It is the second thing the shim puts on the glass (the arrow is the first) and the
second place it supplies a binding rbp does not have for itself (the deck QUANTIZE
boxes above are the first), so it reuses both precedents rather than inventing a
third: `menu_zone.c` is pure geometry and gesture (`touch_zone.c`'s shape), `menu_paint.c`
is pure pixels, `menu_draw.c` is the driver, and the keycodes go out through the same
`rbp_key.c` path the QUANTIZE tap uses. `RB_POINT_MENU=0` turns the whole feature off
and is the honest baseline.

### The strip costs a touch its timing, over one third of its width

The top **55 logical px** (`MZ_STRIP_Y1`) **and the middle third of the width**
(`MZ_ENTRY_X0..MZ_ENTRY_X1`, logical `x 426..852`) no longer reach rbp *as they
happen*. A touch that *starts* there belongs to the shim for its whole life — rbp sees
no down, no move and no up — because the swipe has to be reliable, and a gesture that
rbp is also acting on is a gesture that fights the screen underneath it. What rbp does
get is a **replay**: a press that never became a swipe is handed back 45 ms later, at
the point the finger landed (the replay rule further down). So the price there is a
delay, not a loss, and it is only paid by taps — a swipe reaches rbp not at all, which
is the point of it.

**The rest of the band is rbp's again, on the frame it happened.** The left and right
thirds of the strip are not swallowed at all: no latch, no dwell, no replay, nothing
between the panel and rbp. That is the operator's fourth finding answered where it
actually was rather than patched downstream — *"touches when the bar is not displayed
don't work anymore, the info and timer buttons specifically"* — because **rbp's `INFO`
control is in the right third** and never needed taking in the first place. The
middle third is what is left of the band, and it is chrome on every screen rbp draws:
on BROWSE the centre of the title band and of the list header, on PERFORMANCE the
middle of the track/waveform header.

The strip's height is spent on keeping that delay short: 55 is the shortest band a
swipe can honestly start from. From this repo's own captures: on BROWSE the strip
covers the sidebar's first cell (USB1, x 8..100, y 8..50) and the title band
(y 12..35); on PERFORMANCE the title band (y 6..40). A 120 px strip would also take
BROWSE's PLAYLIST cell (y 52..118) — the left-hand icons that were already the
subject of one complaint — which is why 55 and not something more comfortable. The
**width** is spent the same way and for a stricter reason: rbp puts its own controls
at the two ends of its top bar, so the ends are the part that has to be given back.
`MZ_ENTRY_X0/X1` is the only place that width is written, and `menu_feed()`'s
closed-state down edge is its only reader.

**The one control the zone still covers is rbp's `TIME`/`REMAIN` bar** (x 7..484,
y 7..46), which overlaps it in x 426..484 and is replayed there. That is not a loss
worth counting: the bar never reacted in *either* phase of the A/B that measured the
strip — with this shim out of the path entirely, a tap on it left the frame
byte-identical — so it is a probe that proves nothing in either direction, and the one
control rbp binds inside the middle third is one it does not answer anyway.

**The casualty that used to be here, measured off the glass.** rbp's `◄ INFO` control
is drawn in the top-right corner on every screen it has, in framebuffer pixels (which
equal logical pixels here — 1280×800 fb, 1280×800 logical): a filled grey square,
`(49,48,49)`, at **x 1183..1206, rows 12..35**, holding the `◄` glyph (`(148,149,148)`,
x 1192..1197, rows 19..28), followed by the white `INFO` label at **x 1217..1258, rows
18..30** — so the control in ink spans `x 1183..1258 × rows 12..35`, byte-identical on
TAG LIST (`work/frames/a0.raw`) and on BROWSE (the 2026-09-29 capture), with the
label's bounding box identical on all 82 captured frames. **All of it lies outside
`MZ_ENTRY_X1` (852)**, so with the entry zone in place the control is rbp's own again:
answered on the frame it happens, with no dwell and no replay. Its history is the
reason the zone exists — under the strip's earlier full-width swallow the same tap was
answered 45 ms late, and before the replay existed it did nothing at all
(`bands_changed=[NONE]` with the gate on, **every band** with it off). That measurement
point is logical `(1211,25)`, which falls in the ten-pixel gap between the square and
the label, and the gap is a second finding: **the button's hit box is wider than its
ink**, so a reader who measures only the white glyphs places its left edge 34 px too
far right.

`rbp_abi.h` already carries **`K_INFO 0x020b`**, and it is referenced nowhere else in
the tree — declared, never sent, never mapped, the same standing as `K_REV` and
`K_EFFECTQUANT`. Giving INFO back as a panel button is therefore a keycode this shim
already names rather than a new measurement; what it is not is a decision this change
may take on its own, because one row of six is the operator's choice.

Two rules keep rbp's stream honest:

* **Latching.** `swallow` is decided once, at the down edge, and held until the
  release. `tscfake_emit()` tracks its own `last_down`, so swallowing whole presses
  and never a lone up is what stops rbp from seeing an unbalanced stream. A press
  that starts outside the entry zone is *never* touched — returned to rbp
  byte-identical end to end, which is pinned on the host by `test_point.c` (the same
  press produces the same bytes with the menu on and off) and not on the unit: the
  per-report `POINT_DEBUG` line is written for every report the *device* produced,
  **before and regardless of** the swallow, so it cannot show the difference. The
  unit-side proof of the swallow is a frame diff — see below — and since the replay
  it is the *swipe* that proves it: a swallowed-and-replayed tap ends with rbp
  reacting just as an unswallowed one does, while a gesture the menu keeps for itself
  leaves the frame untouched.
* **A swallowed tap in the entry zone is replayed, not lost.** `menu_feed()` answers
  `MZ_FEED_TAKEN` (rbp gets nothing) or `MZ_FEED_TAP` (the menu took the press,
  has no use for it, and hands it back), and `pointsrc.c`'s `menu_replay_tap()`
  emits the whole press itself — down, a 45 ms dwell, up, all at the point the
  finger *landed*. This is what keeps the controls rbp draws **inside** the zone
  alive, which since the entry zone landed is the middle of its top bar and nothing
  the operator has complained about; `◄ INFO` is in the right third and no longer
  needs the replay at all. It is the only place the shim emits a press it never
  received, so both edges are replayed (a bare up would be a release for a press rbp
  does not have) and the dwell is not optional — the same measured reason
  `POINT_MIN_DWELL_MS` exists. `test_point.c` pins the stream it produces, including
  that the pair leaves the wire released.
* **The toggle is on presses that began open.** A press that starts while the panel
  is closed can open it but can never close it, and a press that starts while it is
  open always closes it and fires nothing else — so the swipe that opened the panel
  cannot immediately dismiss it, and a tap that dismisses the panel does not also
  press the button under it.

### The gesture (all of it in `menu_zone.c`)

* **Closed, press starts in the entry zone:** swallowed and *armed*. It opens when the
  finger has moved `MZ_SWIPE_PX` (56) down **and** more vertically than horizontally
  — a drag along the strip is not a swipe. A tap in the zone does not open the
  panel — there is no travel, so there is no swipe — and it is **not** discarded:
  `MZ_FEED_TAP` hands it back and `pointsrc.c` replays the whole press at the point
  the finger landed. That replay is the whole reason the zone is a delay rather
  than a loss. It cannot fire *twice* however the finger drifts: the answer is given
  once, at the release, and the point replayed is where the press began — measured
  off the release, a drag along the strip that travelled 400 px still replays at the
  pixel it started on. **The x bounds are read only on this arm.** A press that
  starts while the panel is open is swallowed at the full width whatever its x, and
  so is one that started below the strip; the panel's own leftmost and rightmost
  columns are SOURCE and MENU, and a press on either that fell through to rbp
  instead would be a dead button.
* **Closed, press starts outside the entry zone** — either below the strip or in the
  left/right third of it: **not ours at all**, not swallowed, not tracked, returned
  to rbp unchanged on every report of the press. This is what makes rbp's `INFO`
  button, BROWSE's sidebar cells and every screen's title band native controls
  again, and it is the one arm a future change to the entry zone must not
  accidentally widen back to the full width.
* **Open, any press:** swallowed wherever it lands, and the button under the finger
  is tracked. Lifting on the same button the press started on fires that button's
  keycode and closes the panel; lifting on a different button, or on no button,
  closes it and fires nothing. The fire is on the **release**, not the down edge
  (which is `touch_zone.c`'s rule), because the shim owns the highlight here and
  slide-off-to-cancel is what a button is expected to do. Nothing is replayed while
  the panel is open: a press that dismisses it must not also press what is under it.
* **The fire has two forms, and the second is rbp's**: a press that was down for
  `MZ_HOLD_FINGER_MS` (350) or more sends the same keycode **held** for
  `MZ_HOLD_KEY_MS` (500 ms) instead of a press/release pair. The rule is
  `menu_hold_fires()` in this module — pure, host-pinned, and off when the threshold
  is ≤ 0 — while the clock that feeds it is `pointsrc.c`'s. It exists because rbp gives
  MENU two meanings on one keycode and runs the timer itself
  ([08](08-controls.md) note 13), so the shim's only job is to hold the key past rbp's
  threshold. Holding the other five buttons was measured to land where a tap lands,
  and the generic hold is deliberate: the alternative is a per-button table that would
  have to be re-measured every time rbp's firmware is.
* **And it fires while the finger is still down.** `menu_hold_pending()` answers the
  same question mid-press: the moment the threshold passes, the panel closes itself
  and the button fires, with the finger still on the glass, and the release that
  follows is the silent swallow it always was. The threshold is the same 350 ms, shared
  with the release path — this moved *when* the same hold is answered, not what counts
  as one. See below.
* **Dismissal:** a press that began open and travels `MZ_CLOSE_PX` (56) upward,
  predominantly vertically, closes the panel mid-press. It may start anywhere — the
  operator must be able to travel up without starting on the panel.
* **Device loss** clears the state (`menu_reset()`), so a panel whose pointer was
  unplugged cannot become un-dismissable.

The hit test is in **logical** coordinates (where touches are, `touch_zone.h`) and
the image is in **framebuffer** pixels, so glyphs are never resampled on a panel
that is not 1280×800. The two boundaries can disagree by ≤1 px, which is invisible
and is why the buttons tile x 0..1279 with no gap: seven columns of
`(i*1280)/7 .. ((i+1)*1280)/7 - 1`, y 8..47 — 182×40 logical px each (they were
213×40 while there were six, and 213×96 while the panel was 112 rows; see the note
under *What is measured on the unit* for why, and for the measurements that are the
record of the taller bar).

**Since 2026-10-06 the drawn button is not the whole column.** Each column is inset
`MENU_BTN_PAD_PX` (4 fb px) at both ends and wears a `MENU_BTN_BORDER_PX` (1 fb px)
ring of `MENU_BORDER`, and everything outside it is the `MENU_FILL` bed — which is
now **black**. The operator, having seen the bar with a frame round the whole of it:

> the drop down doesn't need the white border around the endire thing, it only needs
> a thin white border around each button with some padding in between and a black
> background. model it this way for the USB stop menu as well

So the bar's two-pixel frame and its one-pixel `MENU_DIV` seam are both gone. The six
dividers that were the only `MENU_DIV` pixels in the band — measured off the glass
with the panel open on 2026-10-01 as 6 runs of 39 rows = 234 px, which was the column
count read straight off the framebuffer — are now **8 px of black bed** between each
pair of buttons (two 4 px paddings meeting, since the columns are adjacent). The
COLUMN is still the hit target: `menu_zone.c` did not move, deliberately, because an
8 px gap is not a control and a finger that lands in one is aiming at a button. The
damage witness moved with the restyle — it sampled the frame's corners and the seams,
which are now bed, and the bed is a colour rbp's own UI is full of; fourteen of its
fifteen points are now the button outlines instead (`menu_paint.c`).

The ink and the widths below are the ATLAS's and move whenever it does: at the shipped
19 px (2026-10-06) each label's ink is 14 rows at rows 21..34, and the widths are
`SOURCE` 69, `BROWSE` 70, `TAG LIST` 66, `PLAYLIST` 66, `SEARCH` 66, `MENU` 49,
**`USB STOP` 76** — they were 11 rows at rows 23..33 and 56..65 px at the 16 px atlas
that 2026-10-01 dump caught. The widest label therefore sits in a 182 px column whose
button gives it a **172 px** inner width — 96 px of margin, against 106 px before the
restyle spent 10 of them on padding and outline — and the seventh costs
the other six 31 px each — the reason the drawn band was never narrowed to the entry
zone's third of the screen, which would have cut a column to 61 px and truncated
words ([menu_zone.h]'s `MZ_COLS` note).

**The panel briefly had an eighth cell, and it is gone again (2026-10-04).** It was a
globe in the band's last 48 logical px, and a tap on it opened the deck's browser
window — the operator's own asks (*"add a > button for an extended menu and have it open
up a browser window"*, and then *"change the chevron to a web icon"*). The browser was
**abandoned** the same day the window shipped (*"ok, you can abandon the exercise, i
don't need a browser"*), which left the cell opening a window with nothing behind it: a
live button that does nothing, in the band the operator looks at most. So the cell was
**removed rather than re-marked** — restoring the `>` would only make a dead button look
like the one that used to work — and the seven columns retiled over it. The 182×40 px
each described above is what they have again: `MZ_BTN_W` is `MZ_LOGICAL_W`, and
`MZ_EXPAND_W`, `MZ_BTN_EXPAND`, `menu_expand_x0/x1` and the globe's own drawing
(`menu_paint.c`'s `menu_web_cov()`) are deleted, with `test_menu.c`'s tiling assertions
and `test_menu_dev.c`'s slide putting the panel's right edge back on the last column.
The window module itself still ships and still works — `menu_window.c`,
`menu_window_paint.c`, `browser_link.c`, `tools/rbrowser/` — because what the operator
abandoned was the browser, not the code; what is gone is the menu's door to it, and
`MENU_WINDOW=1` in `menu_draw.c` is now the only way in — which is also how the window's
own close and minimize marks were pressed on the glass when the chrome was settled.

**Measured after the removal, on the unit by injection into the panel's own node**
(`fbshim.so` md5 `78c2d7420bcef0e80a8ef8fbaa294c5a`, 2026-10-05, `work/poke.py`'s `drag:`
to open the panel and `touch:` to press). A press at logical x **1079** — which the
seventh column owned before the retile (`USB STOP`, 1056..1231) — now logs `menu 'MENU'
held 361ms (hold is 350ms) -> key 0x206`, and the control at logical x **1004** (MENU in
both geometries) answers identically, so the instrument and the discriminator agree. The
globe's own pixels, logical 1232..1279, are the seventh column's now, and that is why this
drill stops at 1079: a press there is a press on `USB STOP`, and `USB STOP` stops the
operator's media — the exact reason the cell could not be left in place as a dead button
and the exact reason it is not tapped to prove it. What pins those pixels is the tiling
assertion in `test_menu.c` (every column, x 0..1279 with no gap), which the same run's
two points agree with. Host suite green at the same binary: `test_menu` 6,572 checks,
`test_menu_dev` 79 on both strides, `test_window` 258, `test_keyboard` 1,044.

**That run's warning is historical, and it is kept because it is why the chooser exists.**
The build it was taken on (`78c2d742…`) ejected the operator's stick on one tap, so a drill
had to stay 100 px clear of its own column. On the live build the same press **raises the
USB STOP chooser** and ejects nothing — the eject takes a three-second hold *inside the box*
— which is what makes the seventh column safe to inject at, and is why the note above is
left standing rather than deleted: it is the measurement that made the case for the box.
See *The seventh column raises a chooser* above.

**And confirmed off the glass by the operator's own eye, 2026-10-05** — *"yep it looks
good"*, swiping the bar down on the deployed `fbshim.so` `78c2d742…`. That is the one
thing injection could not say: a drill can prove which column a point now answers with,
but only the eye can say a mark is **absent**, and it is absent because the seven columns
own those pixels rather than because the drawing is broken.

`POINT_MENU_MOUSE=1` is the bench override that makes the menu answer a *relative*
device too. It is off by default because a mouse has no swipe, and because it keeps
the menu and the arrow mutually exclusive writers of the one framebuffer page.

### The frame it is drawn in

`FBIO_WAITFORVSYNC` is the boundary — **measured, and it corrected the plan.** The
feature was planned onto the `FBIOPAN_DISPLAY` interposer, on this doc's own note
that with a single buffer the pan is a no-op that still goes through the shim. On
this configuration there is no pan at all: every distinct framebuffer ioctl that
reached the shim over a whole session was logged at its first occurrence —
`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, `FBIOPUT_VSCREENINFO`, `FBIOGETCMAP`,
`FBIOPUTCMAP` and `FBIO_WAITFORVSYNC` — and `FBIOPAN_DISPLAY` (`0x4606`) is not among
them (the panstep early-out is not the obstacle: this fb reports `xpanstep`/`ypanstep`
of 1). The pan case in `fb_shim.c` is dead code on this unit, and the panel could
never have been drawn from it. What *is* called is the vsync wait, at a measured
**57.1/s**, and `menu_frame_tick()` runs at its **entry** — the far side of rbp's
draw either way its loop is written, with the whole inter-frame gap to paint in.

`menu_draw.c` also keeps a thread (0.1 ms default, `POINT_MENU_MS` for the bench),
for the two things the hook cannot do: mapping the framebuffer is a blocking
open/ioctl/mmap with retries, which may not run on rbp's RT render thread, and the
hook's side of the frame is a reasoned conclusion rather than an observed one. The
thread is what keeps the panel up if that conclusion is ever wrong. Whichever caller
arrives first paints; the other finds the image intact and does nothing.

**No save-under, and no restore on close.** rbp repaints its whole frame at ~57 Hz
into the one page that is also the visible page, so rbp's next frame *is* the
restore — the same physics as the arrow, over a larger region. Drawing nothing when
the panel closes is deliberate; the thing to watch for is a ghost that survives the
close, which would mean rbp's repaint is not as complete as [13](13-raspberrypi4.md)
says, and the answer then would be `cursor_paint.c`'s conditional restore.

The panel is painted as **pure pixel values** — every cell of its rectangle is a
function of the layout and the palette only, never of what was already there, which
is what makes the idempotence and conditional-restore rules in `cursor_paint.c`
apply unchanged. The one bug that rule has to avoid is that
`have != want → saved[slot] = have` is only correct while the *image* is unchanged:
highlighting the button under the finger changes the image, so the save-under slot
would capture our own old pixel. The discipline is `fb_cursor.c`'s — restore the old
image first, then paint the new one.

**The labels are set in Decker, thinned to a lighter weight**, and that is the one
thing on the panel that is not a single palette value. They were a hand-written 5×7
bitmap drawn at an integer scale, which on a 96-row button meant scale 4, i.e. every
source pixel replicated as a 4×4 block; the operator's verdict on the glass was *"the
font is too big and pixely"*. Decker is the **only Latin font file the XDJ-RX3
carries** (`extracted/XDJRX3/gui/fontdata/decker.ttf` — one static weight-700 face, 260
glyphs, no variable axis), so it is the closest thing on the device to a UI typeface.
It is rasterised **once**, on a build machine, into an 8-bit coverage atlas:
`scripts/shims/bake_menu_font.py` writes `scripts/shims/menu_font.h`, which is
committed, and the shim itself still has no font engine, no freetype and no file I/O.
**It is not, however, the face rbp draws its own text in** — an earlier statement here
said it was and the binary does not support it. rbp names no `.ttf` but the Japanese
one; every Latin glyph it paints comes from Pioneer's own baked bitmap tables,
`/root/gui/pset/fontdata/NS_FONT_ID_*.bin`, through `NS_FontTable_*`. See
[the font question](#the-fonts-the-device-actually-has) below.
Coverage is where the exactness rule lives: **coverage 0 writes exactly the button
colour and coverage 255 writes exactly the label colour** — the ends of the range are
palette values — and only the glyph edges between them are a per-channel blend of the
label over the button. The witness's 15 sample points are chosen to be glyph-free:
they are the **button band's own edges, one row in** (`menu_paint.c`'s
`menu_witness_point()`), which at 1280×800 with the 56-row panel is the ink at rows
21..34 and the sample rows 9 and 46 — 12 rows clear above the ink, 12 below (14 and 13
at the 16 px atlas, which is what the label size spends). The
rule they replace was 5/16 and 11/16 *of the panel*, i.e. rows 34 and 76 at 112 rows
and 17 and 37 at 56 — arithmetic that happens to clear a fixed-height font at one
panel size and does not at another, because a proportional sample row cannot follow a
fixed-size line box. So the points now come from the band, and `menu_view_ok()`
**refuses** a panel whose band cannot host the label's 24-row line box — the atlas is
19 px now, and the line box is the font's own ascent + descent, 24 rows (which is how
480×320, whose band is **16 rows**, 3..18, is refused rather than drawn — a refusal
that logs "leaves no room for the panel" and draws nothing). `test_menu.c` asserts both against the
production point list and across ten panel sizes rather than trusting the arithmetic,
because a witness that sampled a blend would read "damaged" on every tick and repaint
for ever.

**The face is thinned 1/6 of a pixel a side**, and that is the answer to the operator's
2026-10-06 question — *"is there another font that resembles the out of the box RX3
font?"* The player's own text is a regular-weight sans and the panel's was Decker
**Bold**, so weight is the difference the eye catches. There is no lighter cut of Decker
on the device to switch to, and Pioneer's own bitmap tables (`NS_FONT_ID_*`, decoded
below) are a **light** face at a 19 px cap whose `0` is 12 px wide — baking from those
would change every label's width (its `USB STOP` is 98 px against this atlas's 76) and
swap the face the operator had already approved, so it is not what shipped. So the
outlines are eroded: each glyph is
scaled up ×6, eroded one pixel there (a sixth of a pixel here, so the lightening is
sub-pixel), and box-filtered back down. `ImageFilter.MinFilter` is the wrong
structuring element for it — its window is square, so it eats diagonals about twice as
fast as straights (measured on the `O`: 308 cells left after `MinFilter(7)` against 634
after three 4-neighbour erosions at the same radius) — so the erosion is built from
four one-pixel shifts (`ImageChops.darker` over four pastes). **The thinning cannot move
a single metric**: advance, `left`, `top`, `w`, `h` and the whole `MENU_FONT_INK_*` band
are read from the font at 1× and are byte-identical to an unthinned bake, so only
`menu_font_coverage[]` changes — measured at 4,731 of 9,908 bytes, taking the set's ink
from 940,040 to 794,881 (85%). `… 0` bakes the unmodified bold. 2/6 is measurably
lighter and is one command away, but it thins the keyboard's `.` `/` `-` `_` `:` `=`
to nothing solid — at 2/6 the `_` and the `=` have no solid cell left at all — so the
default stops at 1. No bound in `test_menu.c` would catch that: the solid-cell floor
there is a floor under a glyph that rasterises to *nothing*, not a weight control.
The band was read back off its own overlay plane (fb id from the shim's `menu plane:`
log line, pixels fetched by `DRM_IOCTL_MODE_GETFB` from a second DRM client) and shows
all seven labels in the lighter face.

At the shipped **19 px** a capital's ink is 14 rows inside the font's 24-row line box,
and the whole set's band — the dot of an `i` down to the underscore — is 18 rows, rows
5..22. The labels come out **49..76 px wide in a 182 px column** at 1280×800 (the
seven-column band: `MENU` the narrowest at 49, `USB STOP` the widest at 76) — measured
off a host render of the panel, not estimated. There is no scale parameter left
anywhere in the interface: one baked size, centred on the column (`menu_text_width()`
over the atlas's own advances) and clipped to it — which is why `menu_view_ok()` now
refuses a picture whose **narrowest column cannot hold its own label whole**, rather
than drawing a clipped word: at 19 px that floor is a **535-px-wide** picture, so a
480-wide shape no longer draws the band (640 wide, with 91 px columns, still does) and
the refusal is logged, not silent. `test_menu.c` pins the ink band, the advances, the
fit and the floor.

**The size is the operator's, and the ladder is bounded by the bar.** They asked on
2026-10-06 to *"size up the font but keep the size of the bar the same"*, which is a
question the geometry answers rather than the eye: the bar is 56 rows
(`menu_zone.h`), 8 of frame at each end, so the button band is 40 rows at 1280×800 —
and the label's **line box** must fit that band on the SMALLEST picture the port draws
on. The 480-row panels (640×480, 800×480) give a 24-row band; the line box is 24 rows
at 19 px and 25 at 20, so **19 px is the ceiling** and one more pixel would cost those
panels their band. Against rbp's own text the ladder reads: the `INFO` label in rbp's
top bar is 13 rows of ink, Decker's capitals reach 12 rows at 17 px, 13 at 18 and 14
at 19 — so 19 px is a row over the player's own labels and the 16 px atlas was a row
under them. `bake_menu_font.py <px> <light>` sets both; `… 0` is the unmodified bold.

### The fonts the device actually has

Asked on 2026-10-06 — *"is there another font that resembles the out of the box RX3
font?"* — the answer is that there is exactly **one** Latin font file on the RX3, and
these labels are already set in it. The device's whole font inventory is
`gui/system/fontdata/decker.ttf` (Decker **Bold**, weight 700, 260 glyphs, a single
static face — no collection, no `fvar`) and `gui/system/fontdata/sazanami-gothic.ttf`
(Japanese). There is no Regular or Light companion to switch to.

**But rbp does not load the TTF.** `strings` over the shipped `rbp`
(`extracted_v120/XDJRX3/pdj/rbp`) names `/root/gui/system/fontdata/sazanami-gothic.ttf`
and eight `/root/gui/pset/fontdata/NS_FONT_ID_*.bin` tables, and nothing else;
`decker` appears in the binary only inside mangled C++ names (`…DeckER14DBIF_…`), and
nothing anywhere else in the rootfs or the `gui.tar.gz` refers to it. Its text engine
is `NS_FontTable_CreateFontTable` / `NS_FontTable_GetFont` and friends, i.e. Pioneer's
own baked bitmaps — so the letterforms on the glass are a **regular-weight** geometric
sans and the band's bold face is the visible difference, not the family.

**Those tables decode — and the bit depth is the whole story.**
`NS_FONT_ID_ISO8859_w.bin` is 79,758 B of fixed 189-byte cells — **27 rows × 7 bytes,
2 bits per pixel** — so a row is **28 px** and every byte is four 0..3 coverage
samples (an *antialiased* glyph, not a 1-bit mask); 422 cells, no header
(`79758 = 27 × 7 × 422` exactly). Cell *k* is character **0x20 + k** for cells 0..223
(ISO-8859-1, `space`..`ÿ`; `!` at 1, `A` at 33, `S` at 51 — each verified by rendering
it), and cells 224..421 are **Cyrillic** (which is why an `A`-shaped glyph also matches
at cells 263 and 338). Ink is rows 3..21 for a capital — a **19 px cap height**.

**Reading it as 1 bpp is a trap, and it caught this file.** At 1 bpp a row is 56 px,
so every glyph comes out **twice as wide** and aliased; that misreading is where an
earlier version of this paragraph got *"a wide face (its `0` is 24 px …)"* — the `0`
is **12 px**, not 24. Correctly decoded, the face is a *light, normal-width* regular
sans: at its own 19 px cap the `0` is 12 px wide, the `I` is a **4 px stem**, and
**"USB STOP" spans 98 px** — against this port's shipped 19 px Decker at **76 px**.
Nothing in the table reaches the 28th column, in any of the 422 cells (widest is 25 px
on a Cyrillic digraph), which is what settles the width. So the table is real and
usable; it is **not** what shipped, because the operator asked for a lighter *Decker*
and the band's columns are fitted to Decker's advances, not because the face was
unusable. The sibling port `../full-touch-ui-xdj-rx3` draws its button labels from this
very table at run time — area-averaging it down and boosting coverage **1.35×** to hold
the stem weight after scaling — which is the working proof that 2 bpp is the right
read.

### What is measured on the unit

**The swallow, as a frame diff** (`work/fbpanel.c`, `work/fbdiff.py`, `work/poke.py`).
With the panel closed, a swipe in the strip and the frame after it:

| pair | rows 0..111 | rows 112..799 |
|---|---|---|
| idle 2.5 min, no touch (`c0` vs `c1`) | 0 px | 0 px |
| the swipe that opens the panel (`c1` vs `c2`) | **143,360 px = 1280 × 112** | **0 px** |
| the same run ~30 s after a restart, panel open (`v0` vs `v2`) | the panel's cells | 416 px — rbp's own deck-number chevrons, below |

So the panel is drawn *over* rbp's picture, cell for cell, and rbp's own screen below
it does not move at all. The second row is the proof the plan asked for; the third is
its one honest caveat, and it is not about the menu: **rbp animates the two
deck-number chevrons on the PERFORMANCE screen for the first half-minute or so after
it starts.** Measured with nothing touched, in a 200-sample 0.1 s burst that begins
9 s after a restart: the only change in the whole frame is exactly **416 px**, in four
9-px-wide glyph clusters — x 32..40 and 77..85, and 672..680 and 717..725, rows
605..625, the two pairs either side of each deck's number — and the diff count takes
only the two values **0** and **416**. The toggle is fast: 85 transitions in 20 s,
i.e. **~2.1 Hz, a ~0.47 s period** (on ≈ 0.22 s, off ≈ 0.25 s), not the multi-second
phases a 1 s sampler suggests. **Then it stops**: a 3 s sampler running from 28 s
after the same restart saw one state for its first 48 s, one change to the other
state, and that state for the remaining 69 s — so the animation ends somewhere in the
first half-minute, and the one later flip is recorded rather than attributed. A pair
of frames caught a second apart right after a restart can therefore differ by 416 px
with no input at all; the 2.5-minute idle pair above was taken long after it settled.

**Read every row range in this section — here and in the restart table below — as the
record of the height it was taken at: the panel was halved on 2026-09-29.** The operator, having used the bar, asked for half its
height — *"also the swipe down menu is too tall, make it half the height"* — so
`MZ_PANEL_Y1` went 111 → 55 and `MZ_BTN_Y1` 103 → 47, keeping the 8 logical rows of
border at each end (a 2-px frame line plus 6 rows of fill, measured) and halving the
BUTTON band instead, 96 rows to 40. The same drills now report **1280 × 56 = 71,680 px**
in the panel and rows **56..799** below it, and the bar is 7 % of the screen. Measured
on the glass rather than assumed (a raw dump of rows 0..119 with the panel open): frame
0..1, fill 2..7, **buttons 8..47**, fill 48..53, frame 54..55, rbp's own black from
row 56 — the drawn band exactly, and not one row more. The label ink is **rows 23..33,
11 rows** (the 16 px atlas then in the shim; **rows 21..34, 14 rows** at the shipped
19 px), unmoved *by the halving*, because the font is a fixed line box and does not
follow the panel's proportions; that is what the witness's sample rows had to be re-derived
from (see `menu_paint.c`'s `menu_witness_point()`), since 5/16 and 11/16 of the panel
was a fraction and the ink is not.

**A restart with the panel open leaves no residue in rbp's own frames.** Six full
frames at 1 s intervals from the service start (`13:24:08`; the captures and
`analyse.py` are in `work/menuchk/`):

| frame | rows 0..111 |
|---|---|
| `p0`..`p3` | the panel, **byte-identical to the live open capture** (`c2.raw`) |
| `p4` (+4 s) | still the panel's bytes — rbp has started painting below (rows 125..776) but has not reached this band |
| `p5` (+5 s) | **none of the panel's seven palette colours**, and byte-identical to the closed baseline (`c1.raw`) |

*(That last cell is the record of a 2026-10-01 run and is left as measured. Since the
2026-10-06 restyle the bed is black, so "a colour the panel uses" is no longer a safe
discriminator on its own — rbp's own UI holds black too. The outlines are what
distinguish it now, which is also why the damage witness moved onto them.)*

`p5` differs from `c1` by 87 px in the whole 1280×800 frame, every one of them in
column x 0 rows 583..768 — rbp's own level-meter column, not the menu. So what a
restart leaves on the glass for those first ~5 s is the **last composite** (rbp's
final frame with the panel on top), because nothing repaints the one page until rbp
paints again; the panel does not survive rbp's first paint, and there is no
save-under restore or ghost anywhere in it. That is the plan's step 8 — "the pixels go
within one rbp frame" — true, with the qualification that the first rbp frame is ~5 s
of boot away and until it lands the previous picture is what the glass shows.

#### The press-and-hold, and the clock that decides it

The operator's second finding on the glass — *"if i long press menu it should go to
utility"* — is rbp's own behaviour on one keycode: a **press** of `0x0206` is rbp's
menu (MY SETTINGS, on the SOURCE screen), a **hold** is UTILITY, and it is rbp's
timer that decides ([08](08-controls.md) note 13 carries the threshold measurement).
The shim's part is only to measure the finger and then hold the key long enough that
rbp sees a hold — `menu_hold_fires()`, `MZ_HOLD_FINGER_MS` (350) and
`MZ_HOLD_KEY_MS` (500) in `menu_zone.h`, all three host-pinned by `test_menu.c`, with
the clock read at the press's two edges in `pointsrc.c` and nowhere else.
`POINT_MENU_HOLD_MS=0` turns the hold off at the bench (not in `SHIM_VARS`) and makes
every press a plain tap.

**The hold is generic across the six buttons, and that is a measurement, not a
default.** rbp is the firmware, so a held SOURCE does whatever rbp does with a held
SOURCE, and the panel must not start sending something new without looking
(`work/m23matrix.py`, 2026-09-29): each key tapped, then held 500 ms, each result
compared with its own tap at a 2×2 block mean — SOURCE 318, BROWSE 357, TAG LIST 243,
PLAYLIST 195 and SEARCH **0** differing blocks, every one of them in rbp's drifting
deck rows, against **103,264** for MENU, whose hold frame is UTILITY. So a hold of
the other five lands exactly where a tap does and only MENU has a second meaning.

**And the clock that measures the finger was wrong by 450 ms, one press in two.**
`pointsrc.c` computed the stamp as `(long)ts.tv_sec * 1000000000L + ts.tv_nsec`, and
`long` is **four bytes** on this target (ARM32; [05](05-chroot.md)), so a nanosecond
clock wraps every 4.29 s of uptime and reads *negative* for about half of it: a press
stamped in the negative half failed the `down_ns > 0` test at its own release, and a
real 450 ms finger measured **0 ms**. Measured on the unit with `work/m23clock.py`,
which compares the shim's number against the kernel's own delivery stamps taken from
`work/evwatch.py`'s independent read of the panel's node — **before**: 3 of 5 presses
read 0 ms (an 1100 ms finger among them, so the long press reached UTILITY only when
the phase happened to favour it); **after** the clock moved to a 64-bit
`shim_now_ms()` and the guard to an explicit `have_down` flag: **5 of 5**, within a
millisecond (301/451/600/801/1101 ms against the kernel's 300.6/450.6/600.6/800.6/
1101.2). The rel (mouse) loop carried the same wrap, where it was invisible because
its fallback was the same 45 ms the dwell wants — its real cost was that a genuinely
fast click was never *recognised* as short and so was never extended, which is the one
case that block exists for. The boundary on the real path, same instrument: **340.9 ms
→ tap, 360.3 ms → hold**, so the 350 ms finger threshold behaves as written. Nothing
on the host can catch a regression here — the host's `long` is 8 bytes, so both
spellings behave and the suite was green throughout — which is why the instrument is a
target-side drill and not a host test (`menu_draw.c`'s microsecond clock has the same
shape and is safe only because every use of it is a *difference*; that is written down
there, next to the reason it would stop being safe).

**The A/B is one variable, and both halves are visible.** With `POINT_MENU_HOLD_MS=0`
the *same* 800 ms injection (1101 ms real) logs `tapped (button 6) -> key 0x206
(finger 1101ms of 0ms)` — "of 0ms" is the knob being read — and its frame differs from
the short press's by **303,925 px, digit for digit the figure the short press produced
itself**, i.e. the same screen (rbp's MY SETTINGS) and not UTILITY's 583,886. With the
knob unset the same press reaches UTILITY, whose frame is byte-identical 2 s later
(0 px), so it is a screen and not a transient. On the glass, via `work/m23panel.py`
(panel swiped open, MENU column pressed at raw `(1758,36)`): a ~300 ms press on SOURCE
gives rbp's **MY SETTINGS** and an 1101 ms one gives **UTILITY**, both read off the
framebuffer as PNGs rather than inferred from a diff count. **On BROWSE the short
press changes only our own 56 rows — 71,680 px, the panel band — with the keycode
logged as sent**, which is the first finding answered: the panel does send rbp's own
`0x0206`, and this firmware binds no menu to a *press* of it on the browse screens
(the same 0-px result [08](08-controls.md) records), while the *hold* from that same
screen gives UTILITY (837,127 px).

A note for anyone reading these numbers: `work/poke.py`'s `touch:x,y,ms` holds the
touch for its own `drain(0.3)` **and then** the time asked for, so a press of `ms` is
really ~300 + `ms` long, and the tap above has to ask for **0** to land under the
threshold at all. That is why every figure here is quoted from the kernel's stamps
rather than from the injector's argument.

**The operator's finger settled the one preference in this feature, and it settled it
unchanged** — asked whether 350 ms is the right amount of delay, they said *"yes right
amount of delay"* (2026-09-29). So `MZ_HOLD_FINGER_MS` stays **350**, it is the shipped
default and not a bench setting, and it now rests on a finger as well as on the
boundary measurement above. The one thing still unasked is whether the ~500 ms the shim holds the key
for (`MZ_HOLD_KEY_MS`) feels like the player's own button — the finger decided when the
hold *starts*, not how long the key is then pressed.

**The same hold now fires the moment it becomes one, and the operator's own log is
what asked for it** (2026-10-04): *"for holding the MENU to get utility, after two
seconds the menu should disappear and it should just go to utility by itself"*. The
"two seconds" is a description of the old behaviour, not a new threshold, and the
shim's own log proves it — the line this port shipped with, from their finger:

```
pointsrc: menu 'MENU' held 3368ms (hold is 350ms) -> key 0x206 down for 500ms, rbp's own hold action
```

**3368 ms**: they held MENU for three and a third seconds, and the key did not go out
until they let go, because that was where the fire-on-release rule put it. rbp's timer
runs on the *key*, which the shim only presses at that moment, so the panel showed no
reaction for as long as the finger stayed down and the lift was the first thing that
told them anything. `menu_hold_pending()` now closes the panel and returns the button
at the threshold instead, and `read_loop_abs()` polls on a 20 ms tick
(`POINT_MENU_HOLD_TICK_MS`) rather than blocking in `read()` while such a press is
outstanding — the only press in that loop that can complete with no event to wake it.
The gate is `menu_pressed()`, so every other press is answered by the blocking read
exactly as before, and the `press_btn`/`cur_btn` guards mean the swipe that opens the
panel and a press on the panel's border can no more become a hold than they can fire
on release. The threshold did **not** move: 350 ms stands, as felt and approved above.

Measured on the unit by injection (`work/poke.py`, panel swiped open, `touch:1508,37,2000`
= logical `(1004,27)` = the MENU column, with the device silent for the whole 2 s —
which is the case a purely event-driven loop cannot answer):

```
pointsrc: menu button 6 held 360ms and the finger is still down -> the panel dismissed itself
pointsrc: menu 'MENU' held 360ms (hold is 350ms) -> key 0x206 down for 500ms, rbp's own hold action
menu: panel closed
```

360 ms of a 2000 ms hold, two log lines and nothing else for the remaining 1640 — no
second fire, no re-open, no tap replayed. The same drill on the USB STOP column
(`touch:1783,37,2000`) fires button 7 identically, which is the generic-hold rule
above still holding — with one addition since the chooser landed: button 7's fire is
caught **before** the hold branch, so a *held* USB STOP raises the box rather than
sending rbp a 500 ms key-down. There is no second meaning for a held USB STOP that is
worth the operator's media, and the box is then reached by a tap like any other.

#### UTILITY is not a touch screen

**The question came from the glass** — *"in the utility menu is the touch screen
supposed to work?"* — and the answer is **no, on this firmware it is not**, which is
rbp's own design rather than anything the port broke. Measured 2026-09-29, in one
session so that every null is bracketed by a live control:

| probe in UTILITY | result |
|---|---|
| tap a setting's **label** (NEEDLE LOCK, logical 130,177) | **0 px** |
| tap a setting's **value** (SLIP FLASHING, logical 793,437) | **0 px** |
| tap the **category row** (DECK, logical 400,63) | **0 px** |
| *control:* one step of rbp's own selector (`KEY_DOWN` → `K_SELECTOR`) | **126,396 px** |
| *control:* the next selector step | **126,330 px** |
| *control:* a tap on the SEARCH list, seconds later | **37,425 px** |

The ambient drift in UTILITY across the same 2 s windows is **0 px** — the decks were
stopped and nothing animates on that screen — so the null is a real null and not a
diff that cannot see. The screen is perfectly interactive; it is driven by rbp's
**selector** (`K_SELECTOR`), which on this port is the FLX4's browse rotary
(`map_flx4.c:589`) and its browse push (`:933`), or a keyboard's Up/Down/wheel and
Enter (`map_kbd.c:170-173`). rbp simply binds **no touch** to it, which is why the
panel's MENU hold gets you *there* and then has nothing to hand you.

Nothing in the shim is involved: the taps above landed below the strip and outside
the entry zone, and `/tmp/pointsrc.log` carried no `menu:` line for any of them.

**A trap this measurement set, worth more than the measurement.** The first attempt at
a positive control read **3,716 px** and looked like a reaction. It was not: rbp
**marquees** long track titles horizontally, and a 2 s no-touch drift control on the
same screen reads **3,427 px in the same 23 raw rows** (logical 119..135). So on the
browse screens a reaction must be bracketed against a *drift control of the same
length*, or a scrolling title will pass for a tap. The same screen's honest control —
selecting a different track — is 37,425 px, ten times the drift.

**If it is ever wanted**, it is the same family as the QUANTIZE tap
(`touch_zone.c`): map a tap's row to N `K_SELECTOR` steps and a tap on the value
column to an Enter. That is real work and it invents a UI rbp does not have, so it
was recorded and not started — **that paragraph is now out of date**: it has since
been built, as `util_zone.c`. See "Touch in rbp's own UTILITY screen" below for what
was measured and how the gesture works; the note above stands as the record of the
null that made it necessary.

#### The flicker

**The painting, measured two ways** — `work/fbpanel` on the glass, and the shim's
own per-pass timing, with the panel open and nothing touching it. **These were taken
at the 112-row band and the 0.5 ms tick it shipped with then**; the band was halved
and the tick re-priced below, so the duty figures here are the record of that height.

| what | number |
|---|---|
| `fbpanel`, 2000 samples at 500 µs | **96.2 %** of samples hold the panel's border on *both* its rows (top row 98.2 %), and **0** hold neither |
| the same at 1000 µs | **96.5 %** (top 98.3 %), 2 samples of 1000 with neither |
| passes while open over 30 s | ~2048 → **~68/s** |
| one steady-state pass | **236–432 µs, mean ~310** |
| the first pass after opening | **7391 µs** — that one classifies the whole image instead of copying a cached one |
| passes while closed | **0** in 60 s — a closed panel writes nothing at all |

So the shim's added work is ~68 × ~310 µs ≈ **21 ms per second, ~2 % of one core**,
on a player that sits at 43–53 % of a core while idle. The duty is not 100 % because
rbp repaints that band 57 times a second and a sampler can catch the instant before
the shim's next pass — which is the design: `.`/`F` alternation at the frame rate,
never a gap that lasts.

**Re-measured with the classify off the lock** (`work/menu18.sh`, 2026-09-29, 200
samples at 1000 µs, panel open, nothing touching it): **97.00 %** duty, top pair
**99.50 %**, **6 miss runs, the longest 1618 µs**, 7186 µs in total = 3.26 % of the
window — the same floor as before the change, as it has to be. The steady-state miss
is the one repair copy rbp's own repaint forces; the classify stall is a *different*
event, and a 1 ms sampler cannot resolve either of them. That is what the slide test
below is for.

**The press-boundary stall, measured with a sampler that can see it.** The slide
test of `work/menu13.sh`, whose fbpanel period here is **169 µs**, run once against
the shim as deployed before the change (`ab02c597`, tag `before`) and once after
(`eb2b935c`, tag `after`) — two passes each, in opposite directions, so what carries
the comparison is the count of transitions and not their direction. A second sampler
watches the six columns' highlight, both tools stamp every sample with
`CLOCK_MONOTONIC`, and `work/m13/align.py` then asks, per real transition, what the
longest non-`F` run starting within ±120 ms of it was:

| slide | at a switch | | elsewhere | |
|---|---|---|---|---|
| | max | >2000 µs | max | >2000 µs |
| before, left→right | **2090 µs** | 1 | 1013 µs | 0 |
| after, left→right | **1226 µs** | 0 | 1473 µs | 0 |
| before, right→left | **4234 µs** | 1 | 1509 µs | 0 |
| after, right→left | **1922 µs** | 0 | 1329 µs | 0 |

**Two things this settles, and one it corrects.** Settled: the highlight marches
1 → 6 in all four traces — six single-column transitions, ~0.6 s apart — so the
transitions really happened and the column-granular re-classify did not break them.
(That is the question `work/menu14.sh` exists to ask; this pair answers it in the
same run, and `menu14.sh`'s own control stands as the drill for it.) And the longest
run at a press boundary **fell by 42 % and 55 %**, with the before's two runs over
2 ms gone in the after. Corrected: **"~7.4 ms per button boundary" was never the
field number.** The 7391 µs first pass is the *cold* classify; the boundary
crossings are warm — 2.1 and 4.2 ms before, against a steady-state mean of ~550 µs
in the same trace. So the flicker the operator sees is not the panel vanishing for a
fifth of a frame on every press. It is the **~3 % duty shimmer** the floor already
had, 57 times a second, and that is precisely what F1/F2 did not touch: the
open-period miss fraction is **3.30/3.35 % before and 3.32/3.44 % after**, unchanged
in all four traces. Removing it is F5's job — the structural composite — and it
costs 1.4–2.4 ms of a 17.5 ms frame on rbp's own RT thread. That is a fact for the
operator to weigh, not a defect these fixes hide.

**The CPU A/B: run interleaved, it separates the direction but not the size.** The
first attempt was three closed windows in a row and it measured the instrument
rather than the feature — **3190, 2585 and 2568 ticks**, a ~20 % spread with nothing
at all changed, which is above the effect being looked for. Interleaving closed and
open in one session is what makes it readable, because the pairing removes the
drift the unpaired version was drowning in. rbp's own utime+stime, in scheduler
ticks (10 ms each), nothing touching the screen in any window:

| window | ticks / 60 s | `menu:` paint lines |
|---|---|---|
| closed | 2585 | 0 |
| **open** | **2925** | 15 |
| closed | 3262 | 0 |
| **open** | **3625** | 14 |
| closed | 1295 in 30 s → **2590** | 0 |

The two adjacent differences are **+340 and +363 ticks per 60 s** — 7 % apart, so the
*paired* difference repeats better than the baseline it is measured against (the
three closed windows still span 2585..3262, 26 %, on their own). Read that way the
open panel costs **~3.5 s of CPU per 60 s, ~6 % of one core**, and it is consistently
above closed in both pairs rather than sometimes above and sometimes below. What it
does *not* support is a claim tighter than that, and the counted repaints below
predict only ~127 ticks/60 s (2 %) — so ~2.7× of what the A/B finds is elsewhere:
most plausibly the 15-probe `menu_intact()` every witness wake while open, and the
second-order cost of keeping rbp's own render thread re-faulting lines the shim has
just copied over. Deterministic numbers first, A/B as the check that the panel is
not free. Its one firm result either way is the last column: the closed state logs
no passes at all.

**The same A/B after F1/F2/F3, and it lands on the number the repaints predict.**
`work/menu16.sh` repeats the pair three times per session and sets `POINT_MENU_MS`
per session, so it answers both "did the open panel get cheaper" and "is the shipped
tick still the right one". It was run against the old shim and the new one —
`work/m13/m16_before.txt` and `m16_before2.txt` (`ab02c597`) against
`work/m13/m16_after2.txt` (`eb2b935c`) — windows of 15 s, ticks per 60 s, each open
window paired with the closed window before it:

| `POINT_MENU_MS` | shim | closed | open | paired Δ | Δ on one core |
|---|---|---|---|---|---|
| 0.5 | before | 2636 / 2688 / 2716, 2720 / 2704 / 2628 | 3116 / 3060 / 3108, 3120 / 2988 / 3036 | **+415**, **+364** | 6.9 %, 6.1 % |
| 0.5 | after | 2744 / 2752 / 2868 | 2896 / 3012 / 2884 | **+143** | **2.4 %** |
| 0.05 | before | 2928 / 2976 / 2996, 2660 / 2864 / 2740 | 4452 / 4536 / 4524, 4388 / 4248 / 4284 | **+1537**, **+1552** | 25.6 %, 25.9 % |
| 0.05 | after | 2840 / 2764 / 2872 | 3464 / 3460 / 3524 | **+657** | **11.0 %** |

The three individual differences are the honest column and they are not tight:
**+152, +260, +16** at the shipped tick and **+624, +696, +652** at 0.05 ms, because
a 15 s window on a unit idling at ~45 % of a core carries the ±90-tick spread the
closed columns above already show. What the sets do settle is that they do not
overlap: **the largest after-difference is below the smallest before-difference in
both rows** (+260 against +284; +696 against +1384). So the open panel's cost fell
by roughly **60 %**, and the residual at the shipped tick is **+143 ticks/60 s
against the ~127 the counted repaints predict** — 1.1×, where the old shim's two
runs sat at 3.3× and 2.9×. That is F3's hoist landing where the plan said it would:
`menu_intact()` rebuilt the whole panel's geometry fifteen times per tick through
`menu_class_at()`, and now builds it once (`menu_draw.c:478`) and reads fifteen
points against it. The first A/B could only call that "most plausibly" the missing
cost; this one names it. **The tick is still a cheap lever after the fix** —
10× the wakes for 4.6× the cost over closed, against 4.0× before it — so the shipped
0.5 ms was never what made the panel expensive. **But "the period sweep has nothing
left to buy" was the wrong half of that conclusion, and the halving is what showed
it** — see below. One caveat, working against the finding rather than for it: the last
open window of each after-session ran with the governor at 1.1 GHz
(`freq 1800000->1100000kHz`), which inflates a window's ticks instead of deflating
it.

**The period sweep, re-run at the 56-row band, and it buys duty after all**
(`work/mm10.sh`, 2026-09-29, `work/mm10.txt`): the sentence above rests on the *cost*
being flat, which it is — but at 112 rows the miss was a 550 µs repair copy no period
can shorten, so there was nothing to buy. At 56 rows the copy is **82 µs**
(`work/fbcopy`) and what is left of the miss is mostly the **wait for the next tick**,
which is exactly what this knob sets. One script, one build, two phases, one variable,
the panel verifiably open in both (a `menu: panel open` line before each measurement),
and the same 12,000-sample fbpanel window in each:

| `POINT_MENU_MS` | duty | miss runs | longest run | total miss | paired (open − closed) |
|---|---|---|---|---|---|
| 0.5 (then shipped) | 97.64 % | 275 | 1465 µs | 2.62 % of the window | +62 ticks/15 s = **4.13 %** of one core |
| 0.1 (now shipped) | **99.11 %** | 107 | **908 µs** | **0.94 %** | +88 ticks/15 s = **5.87 %** |

The qualifier matters more than the duty: at 0.1 ms **not one of the 12,000 samples**
caught both of the panel's border pairs missing (`neither 0`, against **105** at
0.5 ms), so what is left is a repair *in flight* — a few hundred µs, 57 times a
second — rather than a band that is gone. Five times the wakes therefore buy **+1.5
points of duty**, halve the miss runs (275 → 107) and cut the worst one by a third
(1465 → 908 µs), at **+1.7 points of one core** on the panel's own cost: that pair of
numbers is the whole trade, and it is the operator's to reverse. `MENU_MS_DEFAULT` is
**0.1 ms** — not the 0.5 ms inherited from `fb_cursor.c`, where 0.5 ms is where the
*arrow* reaches ~97 % and the object is a different one — and the revert is either
that one literal or `POINT_MENU_MS=0.5` in the environment for a session. **100 % is
still F5's**, not this knob's: the remaining miss is rbp's repaint erasing the band
every frame, and a faster poll only shortens the repair.

**And the compiled default was then checked with nothing in the environment at all**
(`work/mm11.sh`, after the rebuild and a deploy; `POINT_MENU_MS=<unset>` printed as
its own first line, so the run cannot be reading a knob it thinks is not there):
**99.07 %** duty over 12,000 samples (112 miss runs, longest **669 µs**, `neither 0`)
and **98.99 %** over 30,000 (303 runs, longest 988 µs, `neither 1`, total miss
1.07 %) — i.e. the same place the environment-set phase measured, which is the point:
the default *is* the configuration the sweep priced. Two cautions on the CPU column.
It is a **within-session pair** (closed window, then the identical open one) and it
does not transfer between sessions — this port's own idle CPU spans ~±20 % across
runs, and `work/menu16.sh`'s +143 ticks/60 s (2.4 %) for the same feature was measured
at **112 rows on a different shim**, so the two are not a before/after and should not
be read as one. Within the sweep's own session the panel's paired cost is **4.13 %**
of one core at 0.5 ms and **5.87 %** at 0.1 ms; the run on the shipped default read
**5.07 %** (693 → 769 ticks/15 s).

**Where the flicker rests, and what reviving it looks like.** The operator's verdict
on this measured state is *"ok thats fine for now, just document so we can revisit
another time"* — so the panel stands as it is (56 rows, `MENU_MS_DEFAULT` 0.1 ms,
`99.07 %` and the 669 µs worst run as the floor to beat) and **F5 was not started**.
The trigger for reviving it is the eye and nothing else, and the question to put
first is whether the shimmer that remains — a repair of a few hundred µs, 57 times a
second, at 99.07 % duty — is the flicker that was reported. If it is, this is the
change and these are its terms:

- **What F5 is.** Give the panel its own *presented* frame, so the band is part of
  the frame rather than a repair applied after it: composite it into the driver's
  `rot_surface` at the vsync hook. Duty is then 100 % by construction, because there
  is no longer a window in which the panel is missing.
- **What it needs.** A present mode on — `fbdev_surface_pool.c:402-403` redirects
  `lock->addr` to `shared->rot_surface` only when `present_mode != PRESENT_OFF` — and
  a hook between rbp's own draw and `fbdev_present_primary`
  (`work/dfb-src/systems/fbdev/fbdev.c:469`, called at `:2379` with
  `shared->rot_surface + lock->offset` as its source). This repo already owns and
  ships that mechanism: `tools/build-directfb/directfb-full.diff`.
- **Two traps, and both are worse than the shimmer they would remove.** Leaving
  `menu_draw.c` as it is under a present mode means the driver's own blit wipes the
  panel microseconds after we paint it — the measured shape is **~94 %** duty, i.e.
  below what ships today. Compositing *after* the blit (from `FBIOPAN_DISPLAY`)
  instead puts the 2 MB present copy itself inside the miss window — **~86 %**. The
  composite has to land at the one point where it is part of the frame, and not
  before it or after it.
- **The price when it is done right.** **+1.4–2.4 ms of every 17.5 ms frame on
  rbp's own RT thread**, plus 2 MB/frame of extra memory traffic on a unit that is
  also the sound card. That is the number to weigh against a few hundred µs of
  shimmer, and it is the operator's to weigh.

**"Could we run it 60 fps and be rid of it?" — measured, and the answer is the
opposite** (`work/menu31.sh`, 2026-10-03: `fbpanel`, 6000 samples at a nominal
500 µs, the panel opened and closed by injection so no hand is in the run). The
question was the operator's, and it is the right question to ask of a *compositor*
and the wrong one for this design, because of one number: what ships is
`POINT_MENU_MS=0.1`, so the repair poll runs at **10,000 Hz** — ~167× faster than
60 fps and ~570× faster than the 57.1 Hz at which rbp erases the band. The panel is
not presented; it is **repaired**, and slowing the repair can only widen the window
that rbp's own repaint leaves erased.

| `POINT_MENU_MS` | rate | duty | worst miss run |
|---|---|---|---|
| 0.1 (shipped default) | 10,000 Hz | **99.07 %** | **857 µs** |
| 0.5 | 2,000 Hz | 97.05 % | 1807 µs |
| 10 | 100 Hz | 71.20 % | 6–10 ms |
| **16.67 ("60 fps")** | **60 Hz** | **53.83 %** | **16761 µs** |

At 60 fps the band is **absent for 47.12 % of the window** (194 miss runs, 47.12 %
total, `neither` — no border pixel at all — in 2700 of 6000 samples) against 0.96 %
in the same session at the shipped setting. That is **49× worse**, and the histogram
says why rather than merely that: 76 runs exceed 10 ms and the longest is **16761 µs,
one tick to the microsecond**. The miss is not random — it is bounded above by the
repair period, because the only thing that ever notices rbp's clobber is the poll.
Shortening the tick shortens the miss and lengthening it lengthens the miss, which is
precisely the property a frame-rate change cannot exploit. **60 fps cannot prevent
this flicker; it is the worst setting measured.** The cure has to stop the band being
erased — F5, or a presenter — and no tick is a substitute for it.

*The instrument was stale on the first attempt and nearly produced a plausible wrong
answer.* `/opt/rblive4/fbpanel` was built 2026-09-29 12:44, **before** `PANEL_ROWS`
was halved to 56, and it announced itself as `panel rows 0..111`: it read rows 110-111
— 56 rows *below* the 56-row panel — as the bottom border, so the bottom pair was
never ours and the trace came back as a `T`/`.` storm reading `duty 0.0%, bottom
0/row`. The tell was in the tool's own first line, not in the duty. Rebuilt from the
tree (`arm-linux-gnueabi-gcc -O2 -static -march=armv5t -mfloat-abi=soft`) and
redeployed, it reads `panel rows 0..55` and its first point reproduces the 99.07 %
floor above. **A measurement tool deployed on the unit is a deploy like any other**
— check its vintage before believing a number, and prefer the case where the tool
prints the constant it was built with.

**Nothing here is a defect left open.** The press-boundary blanking is fixed and
measured (above), the tick is priced on the unit, and what remains is bounded,
counted and cheaper than its cure. The plan that carries F5 in full is
`~/.claude/plans/joyful-gliding-abelson.md` step 7, which lives outside the tree —
so the terms above are the record, and the questions that are *not* the flicker
(swipe height, button size, label legibility, the 45 ms tap delay, the seventh
button, the replay in daily use) are listed in [13](13-raspberrypi4.md)'s S3.5 row
under **Only the operator can settle**.

#### The overlay plane on `card1` — the cure that does not go through rbp

Everything above treats the band as something the shim has to keep *putting back*.
There is a route that removes the contest instead of winning it: scan the band out of
a **second DRM plane**, which rbp's redraw cannot reach because rbp's redraw is a write
into the *primary* plane's memory and nothing else. `docs/07` has carried that as "the
real fix, not attempted" since the flicker was first measured. On 2026-10-03 it was
attempted, and the mechanism is on the glass.

**The device, corrected.** That note used to say `drmModeSetCursor` on `/dev/dri/card0`.
Measured with `work/drmplane.c probe`: **`card0` is `v3d`, the render node, with no CRTC
at all**; `card1` is `vc4-drm` (`brcm,bcm2711-vc5`, `OF_FULLNAME=/gpu`). fb0 is
`vc4drmfb`, and the probe finds **`plane 91, type=PRIMARY, BOUND crtc=102 fb=722`** —
`crtc[3]` is the one live CRTC at 1280×800, and fb 722 is the buffer rbp draws into.
So rbp's framebuffer *is* the vc4 primary plane, and the band wants an **overlay**
plane beside it, not a cursor plane.

The inventory is **60 plane objects** — 6 primary (ids 48, 67, 79, 91, 103, 115), 48
overlay (127…644), 6 cursor (655…710) — of which **16 overlays carry
`possible_crtcs=0x3e`**, so they can target the live CRTC, and all report 39 formats
with `XR24` first.

**Two instrument traps, both of which produced a confident wrong answer first.** A
client that has not raised `DRM_CLIENT_CAP_UNIVERSAL_PLANES` is shown **only the 48
overlays** — the primaries and cursors are absent from the list entirely, so the first
probe reported 48 planes and no primary, and read like hardware that had none. And
`drm_mode_get_plane` returns `EINVAL` when the caller's `count_format_types` is smaller
than the plane's real format count, which is **39** here, not the 32 that was assumed;
a `continue` on failure then turned that into a short plane list *and* an empty format
list, i.e. "this hardware has no XRGB8888 overlay". Ask the count with
`count_format_types = 0` first, then ask for that many.

**The third trap is a 32-bit one.** `mmap` of the dumb buffer failed with `EINVAL`
until the build added `-D_FILE_OFFSET_BITS=64`. DRM hands out fake mmap offsets
starting at `DRM_FILE_PAGE_OFFSET_START`, which is exactly `0x100000000` = 4 GiB — the
tool now prints `map_dumb offset=0x1001f4000` — and a default `off_t` on this target is
32 bits, so the offset truncated to 0 and `drm_gem_mmap` found no object there. The
symptom names the buffer; the cause is the argument. Compare
[13](13-raspberrypi4.md)'s `long`-is-4-bytes clock wrap: on this target, check the
width before believing the error.

**What the kernel says while it is held**, with the panel open and rbp redrawing, from
`/sys/kernel/debug/dri/1/state`:

```
plane[91]: plane-3   crtc=pixelvalve-2  fb=722  format=RG16  normalized-zpos=0
                     allocated by = [fbcon]
plane[127]: plane-6  crtc=pixelvalve-2  fb=723  format=XR24  normalized-zpos=1
                     allocated by = drmplane   crtc-pos=640x56+0+0
```

Both planes on the **same** CRTC, ours at the higher zpos, i.e. composited over rbp's
picture and out of its reach. On exit the plane reads `crtc=(null) fb=0` again, and the
primary is untouched throughout — so taking DRM master did not disturb the mode
DirectFB set up, which was the stated reason for never attempting this.

**PASS, on the operator's own eye, 2026-10-04.** The bar was deliberately only the
**left half** of the strip (`x 0..639`, rows 0..55) so the test carried its own
negative control in one glance, and that is exactly how it read: *"yep. i see the
magenta, its solid and the other side flickers"*. One half of one strip, one process,
both regimes — so the flicker is not a property of the panel, of the tick, or of the
band's content, and the cure is to stop the band being rbp's drawing at all. The drill
is `work/drmbar.sh`; it is not installed at boot, so a reboot always clears it, and
`pkill -x drmplane` (or `/opt/rblive4/drmplane off`) clears it sooner.

**What the follow-on costs is smaller than it looks, and one measured detail is why.**
The band is drawn by `menu_paint.c` in the *framebuffer's* pixel format, which on this
unit is **RG16** (RGB565, pitch 2560) — see `plane[91]`'s `format=RG16` above. The vc4
overlay planes list **`RG16` among their 39 formats**, so a plane can be created in
exactly the format the band is already drawn in: same pixels, same pitch, same
`menu_paint()` call, different buffer. The change is therefore *where the band's writes
land*, not what they contain.

That in turn retires the repair machinery rather than tuning it. `menu_draw.c`'s tick
exists to notice that the band is gone and put it back (its "witness", `menu_paint.h`),
and both of those are answers to rbp overwriting the band. If the band is not in rbp's
buffer, there is nothing to witness and nothing to repair, and the tick can fall back to
noticing only *content* changes — a tap, an open, a close. **The fallback must stay
exactly as it is today** for any machine where the plane cannot be set up: the DRM path
is an addition, and its failure has to leave the shipped behaviour untouched.

**The six buttons, with the positive control in the same run.** Each was tapped
through the panel with the shipped table, from a known screen:

| tap | log | frame |
|---|---|---|
| SOURCE | `-> key 0x201` | the SOURCE screen |
| BROWSE | `-> key 0x202` | the browse view |
| TAG LIST | `-> key 0x203` | the tag list |
| PLAYLIST | `'PLAYLIST' tapped (button 4) -> key 0x204` | 55,359 px — the sidebar becomes BANK 1..4 + DELETE |
| SEARCH | `'SEARCH' tapped (button 5) -> key 0x205` | 337,748 px — the list is replaced by a search pane and a QWERTY keyboard |
| MENU | `'MENU' tapped (button 6) -> key 0x206` | 0 px from BROWSE/TAG LIST/PERFORMANCE; 263,596 px each way on SOURCE (the MY SETTINGS toggle) |

PLAYLIST and SEARCH shipped with a `0` sentinel — drawn, named and sending nothing —
until those two keycodes were **measured**; the run is in `rbp_abi.h` with the
derivation, and `map_kbd.c`'s `8`/`9` rows were promoted in the same change
([08](08-controls.md)). `POINT_MENU_KEY_EXTRA` is the bench knob that measured them
(a comma-separated keycode list, applied in column order, `0` allowed); it is
deliberately **not** in `start-rb.sh`'s `SHIM_VARS`, because that loop exports every
listed name unconditionally and an ad-hoc value must not be overwritten with the
empty string. It survives the promotion: it is how the next candidate is measured.

MENU being narrow is worth repeating where a tap is being debugged: `0x0206` is a
real key with a correctly channelled effect, and off the SOURCE screen it changes
nothing at all. A MENU tap that "did nothing" is the screen it was pressed on.

**The gate off is the baseline, and now both halves of it are measured**
(`work/menu12.sh`, 2026-09-29 — the seventh run of this step and the first whose every
phase is *verified* before it is measured). The gate is set through the **shim name**
(`POINT_MENU=0`), not `RB_POINT_MENU`, because the deployed `start-rb.sh`'s `SHIM_VARS`
does not list it ([12](12-troubleshooting.md)).

The probe pair is what makes it readable. Both of rbp's own top-band controls — the
**`◄ INFO` control** (x 1183..1258, rows 12..35) and the **TIME/REMAIN bar** — lie
wholly inside the band **as it was first built**; the entry zone later added beneath
this table put the INFO control outside it entirely, which is the second fix and the
better one. The INFO tap point sits in the gap between that control's square
and its label, so the ON/OFF contrast below is also the evidence that the hit box is
wider than the ink. The positive control
is deck 2's QUANTIZE box at logical `(719,754)`, outside the strip. One session, minutes
apart, one variable:

| tap | logical | gate ON | gate OFF |
|---|---|---|---|
| QUANTIZE (control, outside) | 719,754 | `[500..750]` **CHANGED** | `[500..750]` **CHANGED** |
| rbp's INFO (in the band, later outside it) | 1211,25 | `[600]` **UNCHANGED** | all 16 bands **CHANGED** |
| rbp's TIME/REMAIN (overlapping the entry zone) | 199,25 | `[NONE]` **UNCHANGED** | `[600]` **UNCHANGED** |

(the idle-blink band 600 excluded; the control's bands are the deck lamps it toggles.)

The INFO row **is the swallow, at the wire**: the same touch at the same point leaves
the frame untouched with the gate on — literally no band but the blinking one — and
opens rbp's INFO panel with it off, while a control outside the strip reacts in *both*,
so an UNCHANGED verdict cannot be a dead reader. The TIME row is **not evidence either
way**: the bar reacted in neither phase, so it is a probe that cannot discriminate — the
same lesson as ZOOM on empty decks and the BEAT FX tab. An UNCHANGED verdict on a
rectangle that never reacts proves nothing.

**That table is the shim BEFORE the tap replay, and the replay moves one cell of it —
the operator's fourth finding.** The INFO row's ON verdict, UNCHANGED, *is* the
complaint: *"touches when the bar is not displayed don't work anymore, the info and
timer buttons specifically"*. With `MZ_FEED_TAP` in place the strip still takes the
press, and rbp gets it back 45 ms later at the same point, so the ON verdict for that
row should now read **CHANGED** as well — delayed, not lost. **Measured 2026-09-29,
and it does** (`work/menu19.sh`, both phases gated by their own signature, `POINT_MENU`
unset then `=0`): a tap at rbp's INFO control changed **549,764 px** in the ON phase
and **549,767 px** with the shim out of the path — the same screen transition to four
figures — while the replay's own log line counted **4** in the ON phase (INFO ×3 and
the TIME bar) and **0** in the OFF one, which is the proof the swallow-and-replay
happened rather than the press reaching rbp directly. rbp's INFO control is the
operator's complaint answered: **the strip delayed it by 45 ms and no longer took it.**
**And then it stopped being a cost at all** (`work/menu21.sh`, 2026-09-29, the entry
zone): INFO lies at x 1183..1258 and the entry zone ends at x 852, so the control is
**outside the swallow entirely** — a tap there changed **549,706 px** with the menu
live and **0** replay lines in the log, where `work/menu19.sh` before the entry zone
counted **4** for the same taps (INFO ×3 and the TIME bar), and a second tap closed it
again (549,498 px). That is the same screen transition with no dwell and no replay in
the path, which is the difference between repairing the complaint and removing it.
The same run's other three numbers are in
[the strip's timing section](#the-strip-costs-a-touch-its-timing-over-one-third-of-its-width).
Two consequences worth having in mind next time:

* **The row's contrast stops being the instrument.** With the replay, the frame diff
  cannot tell "the swallow happened and was replayed" from "the press was never
  swallowed at all", because both end with rbp's INFO panel open. What separates them
  is the *timing* and the log: `pointsrc: strip tap at (x,y) is not a swipe or a
  button -> replayed to rbp as a press (held 45ms)` is written only on the replay path,
  and `POINT_MENU_TAP_MS` (a bench knob, deliberately not in `SHIM_VARS`) makes the
  delay measurable without a rebuild.
* **The QUANTIZE control has to keep reacting in both phases.** It is the check that
  the replay did not become a second path into `touch_zone.c`'s boxes — `menu_replay_tap()`
  skips `quantize_tap()` on purpose, and a strip tap is nowhere near a box anyway
  (they are at y 732..780). It read **1518 px in rows 645..776 in *both* phases**, to
  the pixel: the same response with the strip swallowing and without, which is what a
  control outside the strip should look like. Note what that number is *not*: it is a
  lamp changing, not a screen — 1518 px against the 416 px of rbp's own dashed-rule
  drift is a real response, but it is three orders of magnitude under a view change,
  so a threshold calibrated for views reads it as "drift only". The instrument's
  verdict string is therefore the wrong label for this row; the number and the rows
  are the evidence, and the drill prints both.

The A/B that settled it is `work/menu19.sh` (2026-09-29), and its two controls read as
they should: rbp's INFO reacted in **both** phases while the panel stayed shut, and the
QUANTIZE box outside the strip read the same in both. **What it also settled is the other
half of the operator's complaint, and not in the strip's favour.** The TIME/REMAIN bar
was tapped at logical `(199,25)` in both phases and read **416 px (rows 605..625) with
the gate on and 0 px — byte-identical — with it off**. Zero is not "small": the shim was
out of the path entirely and the screen did not change at all, so the timer bar does not
answer a tap *at that point* with or without the menu. The strip therefore did not take
it, and had nothing to give back — this half of *"the info and timer buttons
specifically"* is a control that never responded, and it is a separate question (the
seventh/eighth panel button in [13](13-raspberrypi4.md)'s S3.5 carry-over, and the
`K_INFO 0x020b` keycode that exists and is never sent) rather than a defect in the menu.
Its 416 px with the gate on is rbp's own dashed rule at column 0, not a response.

The rest of the OFF half, from the same verified phase:

| what | number |
|---|---|
| the gate's own line | `menu: POINT_MENU=0 -- no panel is drawn and no touch is swallowed`, and it is the **only** `menu:` line the session writes |
| `fbpanel`, 200 samples at 1000 µs | **0.0 %** — `neither` 200 of 200, and **0/1280** border pixels on both rows |
| the same swipe that opens the panel with the gate on | `menu: panel open` **absent**, zero panel pixels |

So `POINT_MENU=0` is not a parallel code path: with it off the shim writes not one pixel
of the panel and not one `menu:` line but the gate's own, and the strip is rbp's again,
point for point.

**A drill lesson worth keeping, because it cost a run — and then a correction, because
it cost several more.** `menu_draw_start()` is called unconditionally from
`tscfake_open()` and on *every* path logs one of two lines, so a session showing neither
*looks* like a session whose gate had not run yet. And the **gate line is written before
the attach line** in program order — the gate from the main thread inside
`tscfake_open()`, the attach later from the reader thread — so a drill that waits for
`pointsrc: absolute device` has waited for the *later* of the two events. Wait for the
phase's **own** signature: `menu: POINT_MENU=0` for off, `drawing the panel` for on,
never a line both phases write.

That reasoning was right about the order and wrong about the absence, and the log's own
shape was the tell. `pointsrc_log()` opened its file with two variables under no lock —
`log_tried = 1` published before `log_fp = fopen(...)`, then a silent `return` on NULL —
so any call in flight while the winner was inside its `fopen()` was **dropped**, and
silently, because NULL is also how "the log could not be opened" is spelled. Every line
here is one-shot, so a dropped line is dropped for the life of the process: the phase
then waits out its timeout and reports "never reached the state it claimed", which is what
this section's lesson used to be read as. Measured 2026-09-29 (`work/menu19.sh` lost **both**
phases' signatures in one run; `work/menu20.sh` counts it before and after the
`pthread_once` fix): **the surviving lines are the ones written by threads that log
later** — a session holding the reader's attach line and the raw-range line but no `menu:`
line at all does not have a gate that has not run, it has a gate whose line lost a race
inside the logger. `shimutil.c`'s `klog()` has the same missing lock and is *not* wrong,
which is the distinction worth keeping: its flag and its value are one variable
(`log_fd < 0`), so the loser opens a second fd and still writes its line — a leaked fd,
no lost line. The fix is `pthread_once()` in `pointsrc_log()`.

## The edge drawers (`side_zone.c`, `side_paint.c`)

The band above is the shim's only browse surface. This is the second: a **drawer that
slides in from each edge**, the left carrying deck 1 / channel 1 and the right deck 2 /
channel 2. The operator asked for the original pair in exactly those words — *"can you
add a side swipe on each side with cue/play buttons for each deck and a fader control
for each channel? deck 1 on the left, deck 2 on the right"* — and it exists because
CUE, PLAY and the channel faders are otherwise reachable only from the FLX4, which is
not always attached. With no FLX4 there is no transport at all, and worse: rbp builds
its channel faders at **zero**, so a channel nobody reports is digital silence
(`09-audio.md`).

A later round added four things to that first design, and they change its shape:

1. *"allow for both side panels to be visable at the same time and to accept input"* —
   the two drawers are independent, so this needed **two overlay planes** and a funnel
   that routes between them (see *Both drawers at once*, below);
2. *"add a beat sync button at the top of the strip"* — `SYNC`;
3. *"move the play/cue button to the bottom"* — the transport now sits at the bottom
   edge, where a thumb rests;
4. *"add +/- buttons to nudge the track"* — a two-cell `-`/`+` pair.

Two choices from the first round are the operator's and still fixed: the fader
**jumps to where you touch** (absolute, not a relative nudge); and it is a **narrow
full-height drawer**, not a small box. The third — *"closed by tapping away"* — was
**reversed** by the operator in the round that added `SYNC`/nudge/transport: *"the
side bar should stay up until i swipe them away"*. The only act that closes a drawer
is now the outward background sweep; see *The gesture* below, and the reversal note
there.

### The geometry, and why the two sides are one set of rules

`SZ_W` is 180 logical px — 14 % of the width, leaving 920 px of centre glass
uncovered. The x values in `side_zone.h` are **panel-local**: local x 0 is the *outer*
edge on **both** sides, so `side_local_x()` / `side_abs_x()` are the only two places
the mirror exists and every other rule is written once.

| Region | left panel (local x) | right panel |
|---|---|---|
| panel | x 0..179, y 0..799 | x 1100..1279, y 0..799 |
| title `CH 1` / `CH 2` | x 16..163, y 12..31 | mirrored |
| **SYNC** | x 16..163, y 44..115 | mirrored |
| **nudge** `-` / `+` | `-` x 16..85, `+` x 94..163, y 128..199 | mirrored |
| value readout `0`..`100` | x 66..113, y 206..225 | mirrored |
| fader track (drawn) | x 74..105, y 240..600 | mirrored |
| fader **grab lane** | x 44..135, y 206..608 | mirrored |
| **CUE** | x 16..163, y 628..699 | mirrored |
| **PLAY** | x 16..163, y 712..783 | mirrored |

Top to bottom that is the operator's own order: SYNC above the nudge pair above the
readout above the fader above CUE above PLAY. `test_side.c` asserts each of those as a
strict inequality, and each one is a sentence in the request — a header edit that moved
one block past another would be caught rather than shipped.

Every box is a full-width button (`SZ_BTN_X0..SZ_BTN_X1`, the panel's inner 148 px)
except the nudge pair, which is two cells sharing one row with an **8 px seam** between
them (`SZ_NUDGE_GAP`). The seam is deliberately nobody's: a press that straddles the
middle is answered `SZ_HIT_BG` — nothing — rather than "whichever way the arithmetic
rounded", and the hit test loops every lx in the row to prove the two cells never
answer for each other.

CUE and PLAY are **stacked full-width** rather than side by side: the inner width is
148 px, and two 74 px cells would put "PLAY" against the atlas's own 65 px worst case
("USB STOP") with nothing to spare — a transport button is not where a 9 px margin is
wanted.

**The panel has no frame (2026-10-06).** The operator's *"you don't need a white border
on the side swipe menus adjust the layout accordingly"*, which is the same restyle the
band and the USB STOP chooser had the day before: the drawer is now the black
`MENU_FILL` bed edge to edge, and the only white on it is what each control draws for
itself — SYNC's and the transport's one-pixel `sp_frame`, the nudge cells', the fader
cap's, the well's. **No layout number moved**, and that is deliberate rather than
lazy: the padding the frame was standing in for is `SZ_PAD`, 16 px in from the panel's
outer edge, which is what holds the title, SYNC and the transport off the glass's edge
and was already there — see the table above, where every full-width control is
`SZ_BTN_X0..SZ_BTN_X1` = local 16..163 inside a 180 px panel. So removing the frame
changes the picture and nothing else, which is why every hit rectangle, every
measurement below and `SP_BORDER`'s other job (the too-small-to-draw guard in
`side_paint_ok()`) all still hold.

**The mirror is now what the paint test measures.** `test_paint_mirror()` used to prove
the mirror through the frame's thin bands, because they were the only asymmetric mark
in the picture — remove the frame and the proof would have gone with it, leaving the
one thing this panel's drawing can get wrong (panel-local x against a SCREEN-order
buffer) unpinned. The proof moved to the next mark out: **SYNC's own outline**, 16 px
in from the outer edge, so it lands on device column 16 on the left drawer and 163 on
the right. The frame's *absence* is asserted in the same loop, all four edges, both
drawers — a frame that survived on one edge is exactly the failure that change could
have left behind, and it is invisible to every hit test.

**The fader's grab lane is wider and taller than the track it draws**, so a fat finger
lands on it, and it is the one rectangle every button has to be disjoint from — a drag
that could begin on a nudge cell, or end on CUE, would press a control the operator
never aimed at. Two rows keep that apart:

```
SZ_FADER_GY0 (206) > SZ_NUDGE_Y1 (199)     a drag cannot begin on the nudge pair
SZ_FADER_GY1 (608) < SZ_CUE_Y0   (628)     a drag cannot end on the transport
```

The value readout sits **inside the lane's own top slack** (rows 206..225), which is
the whole of why touching the number moves the fader.

### The entry column, and the USB STOP collision under it

A swipe may only *start* in the outer **56** px while the drawer is shut — the same
runway `MZ_SWIPE_PX` gives the band, so one swipe is exactly one swipe long.

```
left : logical x 0..55       and y >= 56
right: logical x 1224..1279  and y >= 56
```

**The `y >= 56` gate is load-bearing, not tidiness.** The right entry column, logical
x 1224..1279, is *exactly the band's seventh column*, whose USB STOP cell sits at
y 8..47: a drawer that armed there would swallow presses aimed at the safe eject. With
the gate the band's entry zone (x 426..852 ∧ y ≤ 55) and the drawers' are disjoint in
**both** axes. Any future change to `MZ_STRIP_Y1` reopens the collision and must reopen
this gate.

**What that collision cost, and what it costs now.** The gate was written while a press
on the USB STOP cell still *ejected* — the note above said so in as many words ("a press
there **stops the operator's media**"), because it did. Since the chooser landed
(2026-10-05) the same press is harmless by itself: it raises the box and nothing is
unmounted until a three-second hold completes on a lit button, which arrived 2026-10-07.
The gate is **unchanged and still required** — the drawer must not take a press aimed at a
band column, whether or not that column's first act is destructive — and the reason is
simply better now than it was when it was written.

### The gesture: what opens a drawer, and what closes one

The gesture is the band's, rotated 90°. A press in the entry column is swallowed and
armed; it opens on travel **inward** of `SZ_SWIPE_PX` (56) **and** `dx > |dy|` —
predominantly horizontal, so a vertical drag from the edge is not a swipe and rbp's own
vertical gestures are untouched. An entry press that never swiped is replayed to rbp
from the point the finger *landed* (`side_replay_tap()`, `menu_replay_tap()`'s twin),
so a control rbp draws under the entry column is **delayed, never dropped**.

While the drawer is out, **no button closes it**. That was the first design's rule —
any release but the fader's put the panel away — and it is wrong now: with a SYNC
button, a nudge pair and the transport all on one panel, *"SYNC, then nudge, then
PLAY"* would be three swipes. **Exactly one act closes a drawer:**

- a **56 px outward swipe on the panel's own background** (`SZ_CLOSE_PX`), dismissed
  *mid-press* without waiting for the lift.

**The reversal, recorded.** The first round also closed a drawer on any press that
began off every open panel — the *"tap away to close"* in `side_feed_any()`'s old
"away" step, which closed **both** drawers at once. The operator removed it: *"the
side bar should stay up until i swipe them away."* So a press on the centre glass
while a drawer is out is no longer a dismissal at all — it is handed to rbp
untouched, on its down edge and its release alike, and the drawer that was already
out is still out afterwards. `side_feed_any()`'s step 3 is no longer a special case
but the ordinary one: *nothing here claimed it, return `MZ_FEED_NONE`*.

A press on the drawer's own background that does not travel is therefore not a
dismissal: it is swallowed and does nothing. A press that began on the fader lane is
the other exception that keeps the drawer out, because riding a fader is the drawer's
most common use and a re-swipe per nudge would make it useless.

**One consequence, and it is the operator's answer.** The top swipe-down band cannot
be opened while a drawer is out — the band and a drawer cannot share the one overlay
plane, and the band's own closed-entry arm is gated on `!side_any_open()`
(`menu_zone.c`). *"Swipe the drawer away first"* is the operator's chosen consequence.
The band, unlike a drawer, keeps its own tap-away dismissal: a stated divergence.

A release fires a button only for a press that began **and** ended on the same box
(roll-off cancels). The **nudge pair is the exception and it is deliberate**: a nudge
press *always* emits `SZ_ACT_NUDGE_STOP` on release, whether or not the finger is still
on the cell it started on. rbp keeps bending until it is told speed 0, so a bend that
outlived its press is the one failure here the operator could not undo by lifting a
finger; a wanders-off bend therefore cancels and fires no button.

### SYNC, the nudge pair, and the fader

**SYNC** is `K_SYNC 0x4112` — the same key the FLX4's own SYNC button sends — sent as
press + release on the side's channel. Plain press/release, and it fires **on the
lift**, like every other button here.

**The nudge pair is a bend, and there is no nudge keycode.** `K_JOG_ROT 0x4305` with
`OP_ROTATE` is the only mechanism rbp exposes: a signed rev/s float, with the position
argument bumped 128 per change. The bend **starts** on the nudge cell's down edge and
**stops** on its release — one message each, no repeat clock — because rbp holds the
speed until it is told otherwise. The magnitude is the acknowledged unknown in this
change and it is exposed for calibration on the glass:

```
RB_ENV SIDE_NUDGE_SPEED    rev/s, default 0.35
```

`side_nudging(side)` derives (-1 back, +1 forward, 0 none) from the press state rather
than remembering it, so the stop cannot be lost; `pointsrc.c`'s `side_bend_sync()`
reconciles the running bend against it on the two paths where no report will ever
arrive — the touch device going away, and start-up with `POINT_MENU=0`.

**CUE and PLAY** are plain keycodes — `K_CUE 0x4102`, `K_PLAY 0x4101` — sent as press +
release on `side_channel()` (1 for left, 2 for right) with **no `usleep`**, unlike the
band's synthesized hold. Measured on the unit by injection: left PLAY → `0x4101 ch1`,
left CUE → `0x4102 ch1`, right CUE → `0x4102 ch2`, right PLAY → `0x4101 ch2`, each with
rbp's own screen changing underneath in the HOT CUE pad row and the deck info row.

The fader needs no MIDI and no FLX4. `K_FADER 0x501e` is an **absolute value** key —
`send_rx_key_f(K_FADER, OP_VALUE, ch, v, fval)` — the same call the hardware fader
makes, and ten bits rather than the FLX4's 128 steps. Top is full:

```
clamp y to [240, 600];  v = 1023 * (600 - y) / 360
```

The down-edge on the lane **jumps** to the landing y; every later motion report while
down recomputes v and sends **only when it changes** (the same dedup the FLX4 map
makes), so a resting finger is silent. Measured by injection on the unit, a drag from
the top of the lane to the bottom: `→ 1000, 873, 749, 624, 498, 373, 246, 122, 0` —
nine sends and no repeats.

**The value drawn before the first touch** is `g_fader[ch]`, the same number rbp's
mixer is being sent, so what the drawer shows is what the channel is doing. That array
is the subject of the next subsection, and it is the sharpest trap in this change.

### One `g_fader` across both shims (`fader_state.c`)

The shim seeds both channel faders at unity for the first ~30 s of every rbp run
(`rbp_vu.c`'s `mixer_defaults_tick()`, gated on `g_fader_seen[]`), because rbp builds
them at zero and a machine with no absolute-control surface would otherwise play into
silence. That seed lives in **knobshim**; the drawer's fader send lives in
**fbshim**. Writing "the same" array from `pointsrc.c` would have created a **second
copy**, invisible to knobshim — and the seed would then silently overwrite anything the
operator did with the drawer for the first 30 s of every run.

So `g_fader[3]` and `g_fader_seen[3]` are defined exactly once, in `fader_state.c`,
with **default visibility**, and that object is linked into *both* shims. `fbshim.so`
is preloaded first (`start-rb.sh`), so the loader binds knobshim's references to
fbshim's copy.

That is an ELF property and it is worth checking rather than trusting, because
knobshim's objects are otherwise built `-fvisibility=hidden`, and a *hidden* reference
would bind locally and defeat the whole thing. Both shims carry
`R_ARM_GLOB_DAT` relocations for the two symbols — preemptible, resolved through the
global scope — and in the live process the two GOT entries point into fbshim's
mapping:

```
$ readelf -r knobshim.so | grep fader
000100c0  R_ARM_GLOB_DAT  g_fader
000100c8  R_ARM_GLOB_DAT  g_fader_seen
# both GOT slots read back as fbshim's addresses, not knobshim's:
knobshim.so  g_fader       GOT@0xf7aba0c0 -> 0xf7ad62ac   (fbshim base 0xf7abe000)
knobshim.so  g_fader_seen  GOT@0xf7aba0c8 -> 0xf7ad7650
```

One array, read by both. A fader move sets `g_fader[ch]` and `g_fader_seen[ch]` in
that shared copy, and the seed then skips that channel. **The FLX4 and the drawer both
write `K_FADER` for the same channel once the controller is reattached; last writer
wins.**

### The fader's flicker: build off-lock, publish bounded

The operator's first request was *"the volume slider flicker when i slide up or down"*,
and it was the band's old defect, arrived at from the other direction. `side_paint()`
rewrote **all 180×800 = 144,000 pixels** of the drawer's *live, single-buffered* overlay
plane buffer, entirely **under `g_lock`**. The `MENU_FILL` bed goes down over the whole
panel first, so mid-paint the panel reads as flat colour: one whole-panel flash per
repaint. Measured on the unit, a single drag produced **8** of them.

The cure is the band's, and it is copied rather than invented — the band had the
identical defect and was already fixed by building the image **off** the lock and
publishing it by swapping two pointers under it:

- **Two system-memory images per drawer** (`side_img[2][2]`, one front/back pair each),
  allocated beside the band's `g.front`/`g.back` in `menu_build()` and sized
  `180 × 800 × 2`. The pair is allocated independently of the band's and **never changes
  `menu_build()`'s return value** — that `-1` is the *band's* "no image cache" signal and
  would silently drop the band to the page. With the pair absent the drawers keep the
  old direct-paint path verbatim.
- **`menu_side_build(which)` — off lock**, called from `menu_thread()` between
  `menu_classify_publish()` and `menu_frame_tick()`, so the tick publishes the image the
  same iteration just made. It snapshots the pressed/value tuple, paints `side_paint()`
  into the back image, and takes `g_lock` **only** to swap the two pointers, record the
  published tuple and set `side_pending`.
- **`menu_side_publish(which)` — under `g_lock`**, called from `menu_side_live()`. On the
  unit it is a single `memcpy` (`g.side[which].pitch == 180 == side_buf_w`, logged as
  `180x800 16 bpp pitch 180 px`); a page-width plane takes the per-row loop `menu_blit()`
  already uses.
- **A fresh dumb buffer is uninitialised** (`drm_band_setup()` creates one), so the
  drawer's `drm_band_show()` is **deferred** until a valid, current front exists; the
  drawer is absent for at most one tick, and only in the case where the vsync beats the
  build.

**The view builder is a deliberate sibling, not a reuse.** `menu_side_build()` cannot
call `menu_side_view()`: that reads `g.side[which].pix/pitch`, which `menu_frame_tick`
mutates under the lock, so an off-lock read there is a real race that can hand
`side_paint` a dangling pointer. `menu_side_buf_view()` reads only the cached scalars.
The `fb_w`/`fb_h` fields are load-bearing — `side_paint_ok()` rejects the whole paint, in
silence, without them.

**The residual tear, stated.** The publish still rewrites every pixel of a live,
scanned-out, single-buffered plane — now a `memcpy` instead of a paint, but still not
atomic against the scan-out. That is the band's accepted trade. The only true cure is a
second dumb buffer plus a `SETPLANE` flip in `drmband.c`, which it does not support today
and which is shared by the band, the browser window and both drawers — bigger and
riskier. Two `MENU_VERBOSE`-gated lines price it on the glass, parity with the band's
`menu: classify` / `menu: blit`:

```
side: build %ld us    the paint the lock used to hold
side: blit  %ld us    the copy that replaced it
```

### Both drawers at once, and the two planes it needs

The **single-buffered overlay plane is opaque** — RGB565 has no alpha — so two drawers
cannot be composited into one full-width buffer. Two planes is the honest answer, and
the obstacle was never the plane count but the **DRM master**: it is held per open
*file*, so a second `drm_band_setup()` used to be refused `EBUSY`.

`drmband.c` now keeps a **refcounted shared device** (`g_dev` with `dev_acquire()` /
`dev_release()`): the first band opens `card1`, sets `UNIVERSAL_PLANES` and takes the
master; every later band just increments the count and owns only its own
buffer/framebuffer/plane. `pick_plane()` skips any plane whose `crtc_id != 0`, so the
second drawer lands on a *different* plane (card 1 exposes 60 plane objects, 48 of them
overlay type).

The band is still exclusive, and for a physical reason rather than a limitation: the
band and a drawer overlap at the top corners of the glass, and two overlays cannot
share a `zpos`. `menu_side_plane_sync()` therefore takes the band's plane **down**
while *any* drawer is out and brings it back — freshly read off the live page — when
the last one goes.

**The funnel's routing** is `side_feed_any()`, and it is four steps in this order:

0. a **latched** press owns all of its own reports, whichever side latched it;
1. a fresh down on **an open drawer's own panel** — and only this step can fire a
   control. The `side_hit(...) != SZ_HIT_NONE` test is what keeps a press in the middle
   of the glass *out* of this step; without it a drawer would swallow every press on
   the screen and "away" could never be reached with two panels out;
2. a fresh down in **a shut drawer's entry column**, so the second panel can be swiped
   out while the first is already up;
3. otherwise — a fresh down on rbp's own glass, the centre of the screen — **nothing
   here claims it**. The press is handed back untouched (`MZ_FEED_NONE`) on its down
   edge and on its release alike, and no drawer closes. Nothing latches it, so nothing
   has to unlatch it. (This step was the *"away"* dismissal until the operator reversed
   it; see *The gesture* above.)

Step 0's test is `was_down` and **not** `swallow`, and that is the difference between
"owns this press" and "was offered this press". A drawer that declined a report still
recorded the down edge — that *is* the latch rule, which stops a press that started
outside from becoming the drawer's by wandering into its entry column — so something
has to tell it the finger has gone. Getting that wrong is the one silent death this
module had: the declined drawer kept `was_down` set and read the *next* press on its
entry column as a motion of a press that ended long ago, leaving it dead with nothing
drawn wrong and nothing logged. `test_side.c`'s `test_entry_survives_declined()` pins
it, and fails without the fix.

### Two fingers at once: the pointer split, and what the kernel does to a still one

Both panels could be open and both could accept input (the SYNC cell above), but only
**one hand at a time**, and the operator said so in their own words: *"if i try to drag
both volume meters it gets confused and only one of them changes"*. The cause was the
pointer model, not the panels: every gesture module in `pointsrc.c` was fed from a
**single** contact's `(x, y, down)`, so two fingers were two reports into one state, and
whichever hand moved last owned the channel.

The repair is a **pointer dimension**. `side_zone.h` declares `SZ_PTRS 2`, with
`SZ_PTR_MAIN 0` and `SZ_PTR_ALT 1`; `read_loop_abs` keeps `rx[]`, `ry[]` and `down[]`
indexed by it. The primary contact is the only one **rbp, the band, the browser window,
the USB STOP chooser and the UTILITY gesture** ever see, because rbp has exactly one
pointer: `pointer_report_alt()` offers the second contact to the drawers **and to
nothing else**, and a report it does not take is **dropped, never forwarded**. Feeding
a second finger into rbp's single cursor would be a worse defect than the one being
fixed.

Drawer gesture state is now per-`(side, pointer)` — `static struct side_st st[2][SZ_PTRS]`
— while whether a drawer is *out* stays a per-panel fact, `static int sz_open[2]`,
because being out is a property of the panel and not of the hand that opened it. That
distinction is why the two halves of the repair are separate: two hands need two state
machines, but two hands must still agree on one drawer.

**Then the trap, which is one layer below the shim: the kernel de-duplicates an unchanged
`EV_ABS` per axis.** `input_get_disposition()` drops a value equal to the last one it
*accepted* for that code (`absinfo[code].value == value`), and it keeps **one** such
value per axis — not one per MT slot. Measured on the unit's own panel node
(`TSTP CTouch`, an MT-B device) on 2026-10-06: two contacts placed at the same height
delivered

```
MT_SLOT 1 / MT_TRACKING_ID 2 / SYN
```

and **nothing else**. The second contact's x and y were never sent at all, so the reader
left it at (0,0) — which is the **left drawer's own corner**, so the operator's right hand
moved the left fader. Two more consequences of the same rule, both measured: a batch whose
events are *all* de-duplicated emits **no `SYN` at all**, and a perfectly still finger's
repeat positions never reach the reader. A moving finger — a real one — always delivers.

The `seen` guard closes it. `int seen[POINT_SLOTS]` is cleared on every
`ABS_MT_TRACKING_ID` (down *and* up) and set by `ABS_MT_POSITION_X` / `_Y`, so the second
contact is offered as

```c
pointer_report_alt(down[1] && seen[1], x, rx[1], ry[1]);
```

— a contact that has never said where it is never acts. Without it the failure is the
kind this module has already had once (the declined-press latch, above): silent, nothing
drawn wrong, nothing logged.

**The drill had to be made faithful, and that cost real time.** `work/poke.py`'s
`bothfaders` verb first sent a *still* second finger and produced no second channel line
at all, which looks exactly like the module still being broken. A real finger moves, so
the verb now clears both slots' tracking ids first and steps each contact's x and y over
four reports. With that, five consecutive runs on the unit were **5/5 identical and
correct** — each hand moved only its own channel, deterministically:

```
left  fader ch1 -> 897
right fader ch2 -> 897 / 724 / 548 / 375 / 198 / 375 / 548 / 724 / 897
left  fader ch1 -> 724 / 548 / 375 / 198
```

The module's own proof is `test_side.c`'s `test_two_hands()`, which opens both drawers
with the **alternative** contact opening its own, drags each hand, and asserts that the
other hand's channel never moves — including the exact line the unit's log produced
wrongly before the fix.

### One surface at a time, and what is left of it

The funnel ladder is window → band → drawer → **rbp's own UTILITY screen**
(`pointsrc.c`'s `pointer_report()`), and only **two** gates remain: `menu_zone.c`'s
closed-entry arm gains `&& !side_any_open()` (and the same for the browser window), and
`side_zone.c`'s gains `&& !menu_is_open()`.

The side's own `&& !side_any_open()` — which existed when one plane meant one drawer at
a time — is **gone**, and its removal *is* request 1: it was what made the second panel
unreachable while the first was up. Band-first is still deliberate: an open band
swallows at full width and must answer first. Measured on the unit: with the band open,
a left-edge swipe closed the band and did **not** open the drawer.

UTILITY is asked **last of the four**, and it is asked only while the window is shut, the
band is closed and no drawer is out — `!menu_window_is_open() && !menu_is_open() &&
!side_any_open()`. The three feeders above already own their own rectangles; those three
terms are about the glass *between* them, where a scroll that moved a list the operator
cannot see (behind an open panel) would look like it worked and could not be aimed. It is
also the operator's own answer to the band — *swipe the drawer away first*. See the next
section for what the gesture itself refuses.

### What is measured on the unit

By injection through the panel's own node (`work/poke.py`), so the whole path runs —
kernel → `pointsrc` → `tscfake_emit()` → rbp. Measured 2026-10-05, all four requests in
one pass; the log is `/tmp/pointsrc.log`.

**Request 1 — both panels out at once.** Two distinct plane ids on one crtc. The second
drawer gets its own plane rather than silently re-using the first, which is the failure
the refcounted shared device exists to prevent:

```
pointsrc: side left drawer open
side: a drawer is out -- the plane is handed over from the band
menu plane: crtc 102 plane 127 fb 723 180x800 16 bpp pitch 180 px (288000 bytes) at 0xcb927000
pointsrc: side right drawer open
menu plane: crtc 102 plane 138 fb 725 180x800 16 bpp pitch 180 px (288000 bytes) at 0xc1a2f000
```

**Both panels accepting input, each on its own channel** — the SYNC on each drawer
reaches its own deck, with both drawers still out:

```
pointsrc: side left SYNC  -> key 0x4112 ch1
pointsrc: side right SYNC -> key 0x4112 ch2
```

**Request 4 — the nudge, and the half that is safety-critical.** The bend starts on the
down edge and **stops on the lift**; rbp holds the last speed until it is told zero, so
a missing stop would leave the track running away:

```
pointsrc: side left nudge forward -> key 0x4305 rotate speed +0.35 ch1 pos 128
pointsrc: side left nudge stop    -> key 0x4305 rotate speed +0.00 ch1 pos 128
pointsrc: side right nudge back   -> key 0x4305 rotate speed -0.35 ch2 pos 65408
pointsrc: side right nudge stop   -> key 0x4305 rotate speed +0.00 ch2 pos 65408
```

**The fader reaches rbp's mixer.** The objective half of the proof is not the log line
but the shared array the mixer reads, taken out of the running process:

```
pointsrc: side left  fader ch1 -> 0
pointsrc: side right fader ch2 -> 1023

$ readelf -sW /opt/rblive4/fbshim.so | grep -w 'g_fader'      # -> live at 0xf788e2b8
g_fader      (1023, 0, 1023)      # [master, ch1, ch2] -- ch1 held at ZERO
g_fader_seen (   0, 1,    1)      # both channel faders claimed by the drawer
```

Two things are being asserted there and both matter. `g_fader_seen[1] = 1` is what
disarms the 30 s unity seed (`09-audio.md`), and the reading is taken **minutes** after
the shim loaded — far past the window the seed runs in — with channel 1 still at `0`.
If the seed could still see a private copy of the array it would have re-asserted that
zero to `1023` by now. And the array really is one array: knobshim's `R_ARM_GLOB_DAT`
slot for `g_fader` resolves to `0xf788e2b8`, **byte-identical to fbshim's definition
address**, so the drawer's send and the seed's read meet in the same memory.

**The dismissal closes every open drawer and hands the plane back:**

```
pointsrc: side left drawer closed
pointsrc: side right drawer closed
side: left drawer closed -- its plane is released
side: right drawer closed -- its plane is released
menu plane: crtc 102 plane 127 fb 723 1280x56 16 bpp pitch 1280 px (143360 bytes) at 0xe9109000
side: no drawer is out -- the plane goes back to the band
```

The band's plane comes back at `1280x56` on its own id — a live plane re-read from the
page, not a stale image.

**What is still owed, and by whom.** CUE and PLAY were injected and the log names the
right key on the right channel (`side left PLAY -> key 0x4101 ch1`), but the unit was
sitting on the **browse screen with no track loaded on either deck**, so there was
nothing for them to move and `/dev/fb0` showed only its own clock ticking. Loading a
track to prove them on the glass would write the operator's `export.pdb`, so that step
is the operator's: with a track up, press CUE and PLAY in a drawer and watch the
transport. The same press is what settles the swipe's feel.

The drawers are on the **overlay plane**, so `/dev/fb0` does not contain them and a
screenshot cannot show them. To judge the look without standing at the panel, render one
with the production painter — `side_paint.c` into a 180×800 buffer, exactly the buffer
the plane gets (`work/fbdiff.py`'s neighbours; the drawing is a function of
`{side, pressed_hit, fader_v}` and of nothing else, which is also what makes painting
it twice paint it once).

**Both drawers have since been read back off their own planes** (2026-10-06), which is
the stronger witness and the one the borderless picture needed: `/proc/pid/mem` cannot
read a vc4 dumb buffer at all (`EIO` at a live address), so the pixels come from
**outside** rbp through the same `work/boxshot2.py` the chooser uses — open the card,
`GETFB` by the id in the shim's own log line, map, read. The drawer's line is
`menu plane: crtc 102 plane 127 fb 723 180x800 16 bpp pitch 180 px`, and `fb 725` is the
**second** drawer's own plane (`plane 138`), which is the two-master block solved in the
cell above showing up in the log. The captures are the four edges at `0x0000`
(`MENU_FILL`) with the controls outlined, both sides: `work/drawer_left_live.png`,
`work/drawer_right_live.png`.


## Touch in rbp's own UTILITY screen (`util_zone.c`)

The operator's third ask, in their own words: *"in the utility menu give the ability to
scroll up and down items and tap to click enter on an item (only do this in this menu)"*,
disambiguated as *"the utility menu where the need lock is"*. The section above records
the measurement that made it necessary — rbp binds **no touch** to that screen and it is
driven entirely by `K_SELECTOR` — and the paragraph it ended on used to say this was
"recorded and not started". This is it, started and shipped.

`util_zone.c` is the third of the pure gesture modules, next to `menu_zone.c` and
`side_zone.c`, and it is written to the same rule: no rbp address, no env, no I/O, no
clock. Everything rbp would tell it arrives in a `struct util_state` that `pointsrc.c`
fills in, and everything it wants sent comes back as an act and a signed count. The two
halves that touch rbp live in `pointsrc.c`: `util_read_state()` and `util_send()`.

### The gate: `getBrowseMode() == 7`

"Only in this menu" is one read. rbp gates its whole UTILITY key handling on the browse
mode equalling **7** — `UiBrowse_SetDispUtilityList` @0x13cfc8 opens the screen with
`setBrowseMode(7); setBackColor(9)`, and `Ui_BrowseCommTask` @0x1351a8 compares against
the literal 7 at 0x139124, 0x139180, 0x1391a4 and 0x1391bc — so `== 7` is rbp's own
test and not a guess (`rbp_abi.h`'s `BROWSE_MODE_UTILITY`).

The mode is the **first word of `uiBrowse`** at `0x0326f8b8`, and that is the whole
reason this is safe from the input thread: `uiBrowse` is a plain `.bss` singleton, so
there is no pointer chase and no structure built during rbp's start-up to walk. It is
the opposite case from `UI_PADMODE_HOLDER_GLOBAL`, whose guarded walk `rbp_led.c` needs.
The one guard that *is* required is the process test (`is_rbp_process()`, `rbp_key.c`):
those addresses are only mapped inside rbp, so a shim loaded anywhere else must not
dereference them at all. Confirmed against the glass 2026-10-05: the live mode read 7
while the list was on screen.

Two more words of the same singleton are read with it — `[uiBrowse+0xac]` and
`[+0xb8]`, which is `IsUtilityCalibrationOn()` @0x112e80 spelled as the two reads it
actually is. rbp draws its own touch marks on the **calibration** sub-screen and reads
touches there itself, so the gesture stands down while those are set even though the
browse mode has not changed.

The **cursor** comes from `getCursorNo(getActiveList())` — `[uiBrowse + 4*activeList +
0x6c]`, indexed the way rbp's own `getActiveCursorNo` @0x112790 indexes it. While
navigating that is list 0, and `activeList` is non-zero exactly while **editing**.

### The window, and why a tap is cheap

The list is **33 items drawn 12 rows at a time**, and a rotate moves the selected
absolute index by **exactly one, every time**. Measured by injection 2026-10-05
(`work/keysend.py` driving `K_SELECTOR` through `map_kbd.c`, `work/utilprobe.py` reading
`uiBrowse` back):

| what was driven | `cursor` | `initialNo` | `abs = initial + cursor` |
|---|---|---|---|
| four rotate steps | 0 → 4 | 0 | 0 → 4 |
| fourteen more | holds at **11** | 7 → 21 | 18 → 32 (the last of 33) |
| rotate back up | 11 → 0 in the window | — | walks back, window still |

So the cursor walks 0..11 and then **holds at 11** while `initialNo` — the window's
first item — increments; `setCursor` @0x113c24 is what clamps it to twelve. That is what
makes the tap cheap: the row the operator touched holds item `initialNo + row`, the
cursor is already on `initialNo + cursorNo`, and the travel is **`row − cursorNo`** with
the window cancelling out. `initialNo` is never read, and reading it would buy nothing.

The 33 items include blank rows and grey section headers (GENERAL is one); each occupies
an index slot like any other row.

### The geometry

Measured off `/dev/fb0` with the screen up, not eyeballed:

| quantity | value |
|---|---|
| row 0's top edge | **y = 50** |
| row pitch (2 px separator included) | **52 px** |
| rows drawn | **12** (y 50..673 inclusive) |
| the name cell / the value cell | x 0..729 / x 730..1279 — **one row**, so the whole width is one target |
| below the list | a gap, then rbp's own deck strip at ~712, which is **not ours** |

`row = (y − 50) / 52`, and `test_util.c` pins every boundary a pixel either side as
arithmetic. The six-column top band's strip is rows 0..55, so the first six pixels of
row 0 are band territory in the band's entry column; that is the band's pre-existing
behaviour and it is left alone.

### The gesture, and the two things it refuses

A press is **latched** at its down edge — swallowed for its whole life or not at all, so
one that starts off the list never becomes ours when it wanders on. Nothing is sent at
the down edge, so even a tap that never moves costs one report; there is no per-report
step to starve the loop the band, the window and the drawers share.

| the report | what happens |
|---|---|
| down on the list | swallowed and latched; the landing point is the anchor and the row under it is remembered |
| move | one `K_SELECTOR` rotation per **52 px of travel from the ANCHOR**, minus what this press has already sent — so a resting finger re-sends nothing and a direction change reverses by exactly the difference. Bounded by **16** per answer, then the remainder follows on the next report |
| release, wandered (>24 px) | swallowed, nothing sent: a drag is not a tap |
| release, on the anchor | **TAP**: `row_at_press − cursor` rotations, then `K_SELECTOR` press + release (Enter) |
| any report after the screen leaves UTILITY | **`MZ_FEED_NONE`** — the press is dropped and rbp is handed the report |

**What those rotations MEAN is rbp's decision, not this module's, and it changes with
rbp's state.** Not editing, they scroll the list. Editing, the very same `K_SELECTOR`
rotation **changes the highlighted item's value** — one injected rotate turned **LOAD
LOCK from UNLOCK to LOCK**. It is the same number on the same wire either way; only rbp's
cursor mode tells them apart.

**This was got backwards in the first release, and the operator found it.** That release
refused every rotation while editing, on the argument that a scroll which silently
rewrites a setting is worse than a gesture that does nothing. The hazard was real and the
remedy was wrong: it left the operator able to **enter** a value row and unable to change
it, which is precisely what came back —

> *"selecting items in utility menu also works, only issue is that i have no way of
> changing the values once selected"*

And the trade is not the shim's to make, because rbp makes it. On the RX3 the same encoder
both moves the highlight and edits the value, and **no control scrolls a list while an
item is in edit mode**. So the rule is now the operator's own, and it is the whole of the
gesture:

> **a tap enters edit mode; a drag changes the value; a tap leaves.**

The one thing kept from the refusal is on the release: while editing the tap's travel
count stays **zero**, so the Enter that leaves edit mode cannot also drag the highlight
onto whatever row the finger happened to be over.

**A drag never taps**, even one that ends back on its anchor: the list has already moved
under it, so "the row you touched" is no longer what the operator was aiming at.

**The screen can go away under a press.** The FLX4's BACK reaches rbp without passing
through this module, so both the move path and the release refuse once the mode has
changed — otherwise a drag left latched across a screen change would land its Enter on
whatever screen is now up. rbp never saw the press, so it is owed no release either.

The count is sent as a **burst with no sleep between the steps**: rbp's rotate is a
posted message and a wait here would starve the shared input loop (the same TRAP 2 this
file records for the band). `map_kbd.c`'s `kbd_rot` bounds and sends the same way; this
is that sender's second caller, with the count coming from a finger instead of a wheel.

None of this reaches rbp's touch path at all — a swallowed report never calls
`tscfake_emit()`, which is what keeps the six columns working over that screen.

### What is owed, and by whom

Everything above is code and host tests (`test_util.c`: 422 checks, every boundary, the
coalescing, the cap, the travel, the refusal when the screen changes, and the value-edit
drag). What has **not** been done is driving
the gesture on the glass with the operator's own finger. Injection has proven the
instruments — `keysend.py` reaches `map_kbd.c` and `utilprobe.py` reads the fields back —
so the drill is: open UTILITY, drive the new gesture through the panel's pointer node
(`work/poke.py`), and read rbp's frame diff **bracketed against a same-length drift
control**, because that screen is the one where a marqueeing title passes for a reaction
(the trap at the top of this file). The unit was left exactly as it was found —
`cursor=[0,0] initial=[0,0]`, LOAD LOCK back to `UNLOCK` — after the edit-mode
measurement above.


## The waveform swipe (`wave_zone.c`)

> *"can you add pinch to zoom in/out on the wave form in performance mode (whatever the
> one that is that displays the wave file)"* … *"can you build the pinch to zoom in the
> waveform view ?"* … and then, on the build:
>
> *"yeah, what you did is no good. it messes up scrolling everywhere else, instead of
> pinch can you just allow swiping on the waveform up/down for zoom in/out."*

The first two asks produced a pinch. **That pinch was rejected on the glass**, and the
rejection is the design of what replaced it — so both halves are written down here.

### Why the pinch regressed everything else

`pinch_zone.c` answered **`MZ_FEED_TAKEN`**. Its second contact latched the gesture and
*every* report after that was swallowed until **both** fingers lifted — including reports
with one finger or none. A phantom or stale slot therefore latched it, and from then on the
shim ate presses it never meant to own; a swallowed press is a press rbp never sees, and on
every screen but the performance one a single-finger drag is *the scroll*. The synthesized
`K_SELECTOR` rotation is itself not zoom-specific either: off the performance screen that
same keycode moves rbp's list cursor. The operator's own words for it are the heading above.

### What replaced it: a one-finger vertical swipe, and the additive rule

**`wave_zone.c` cannot take a report at all.** Its one door returns a **signed step count**
and nothing else — there is no vocabulary in the module with which to swallow anything. rbp
is handed exactly the stream it is handed today; the only new thing on the wire is a
`K_SELECTOR` rotation, and the gate below is what bounds where that can happen. There is no
latch that can be left behind, no release owed, and nothing to go deaf
(`declined-press-must-still-see-release` is a hazard this gesture structurally cannot have).

It is asked **first** in `pointer_report()`'s ladder, and that is not a tidiness preference:
every other feeder answers `MZ_FEED_*` and a TAKEN report returns early, so a module asked
later would not see the reports a surface swallowed — and a gesture that does not see every
report of a press cannot hold an anchor for one. It is re-gated on every report for the same
kind of reason: rbp can leave the screen under a press that is already down.

**The zoom is still encoder-only.** `CursorWaveZoom` @`0x102720` is the binary's *only*
caller of `setPlayModeWaveScale()` @`0x133734`, and its only caller is `BrowseEncoderRotate`
@`0x1212a4` — a selector rotation. The on-screen `− ZOOM  GRID` (logical x 1130..1250,
y 390..410) is an **indicator**: `ui::touch_panel::ZoomGrid`'s vtable at `0x004d8580` is
referenced nowhere in the code, and three taps and a drag across it were measured on the
glass to change nothing. So the swipe is spent as `K_SELECTOR` rotations — the same wire the
encoder uses and the same one the UTILITY gesture drives — and rbp then zooms through **its
own** code path, with its own clamping (0..4) and its own indicator.

| the report | what happens |
|---|---|
| anything, and the gate is closed | **0**, and rbp gets the report untouched. This is every report of every touch anywhere but the performance screen |
| down inside the rect, gate open | the press becomes **ours**; the landing point is the anchor and the current scale is the base. **0 sent** — a tap on the wave costs one report and does nothing |
| down anywhere else | not ours, and it never becomes ours when it wanders in |
| move, ours | whole steps out of the **vertical travel from the anchor**, minus what this press already sent, clamped to rbp's 0..4 ladder **from the base** — so a resting finger re-sends nothing, a wobble inside a band sends nothing, and a reversal pays back exactly what it overran |
| move, not vertical (`|dy| < |dx|`) | **0**. A drag *along* the wave is rbp's own gesture — scrubbing, searching — and it must not zoom |
| gate closes mid-press | the press stops being ours, silently; the rest of it is rbp's, and rbp has had every report of it all along |
| release | **0**; the zoom is delivered while the finger moves |

**Up is zoom IN.** `calcParticularWave` @`0x123014`'s samples-per-screen ladder is
{1600,800,400,200,100} for scales 0..4, so a **bigger** scale is zoomed further in: the
finger moving up makes the count positive. `WAVE_STEP_PX` is **40** logical px a step, and a
step is crossed exactly at a multiple, in both directions.

**The rect is `x 200..1080, y 60..480`**, measured off a live `/dev/fb0` capture of the
performance screen at 1280×800: the top bar is y 8..41, the DECK 1/2 panels x 10..183,
BEAT FX x 1090..1269 (the wave gesture must not reach into it — and *"rbp binds touch to
that one and it stays his"* was the belief here until 2026-10-06, when the two BEAT FX
controls the shim now supplies were measured and **rbp binds nothing to either**; the
section below has the diff), the wave canvas
y 47..490, the HOT CUE label row y 499..509 and the two pad rows y 518..567. The top edge is
y 60 and not 47 for a second reason: the shim's swipe-down band takes rows 0..55 and **opens**
on any press inside them, so a rect that reached into that strip could open the band out from
under the gesture.

### The gate

`ComputeCursorMode` @`0x113084` returns the wave-zoom cursor mode only from its mode-1
branch, and only when `uiBrowse[0x18] != 1` and the grid-adjust flag is clear. On that screen
a selector rotation is either a wave zoom or a **beat-grid move**, and the beat grid is the
analysis of the operator's own track — so the gate refuses the whole gesture while that flag
is set. It reads the **live** flag (`0x03254a9c`), not the mirror rbp caches at `0x0216bad0`:
the mirror is stale between rotations, and a stale copy here is a swipe that moves the grid.
`RB_WAVE_ZOOM_OK()` in `rbp_abi.h` is that branch spelled out, so a step is sent exactly when
rbp itself would have sent the rotation to the waveform.

One condition rbp knows nothing about is added: **nothing of the shim's may be in front of
that screen.** The band and the drawers are drawn *over* rbp's own, and zooming a waveform
the operator cannot see is the same defect the UTILITY gesture's "nothing is up" terms exist
to prevent. The operator's own answer applies — swipe the drawer away first.

### What is measured, and what is not

`test_wave.c` is **71 checks**: the four rect walls and the four regions the rect must stay
clear of (the band's dead rows, the deck panels, BEAT FX, the hot-cue row), the module's
private copy of the ladder pinned against `rbp_abi.h`'s, every term of the gate re-read
mid-press, the latch rules, the step size in both directions, the ratchet's convergence and
the exact payback of a reversal, the ladder's refusals at both ends, and the vertical-
dominance rule. All 71 pass on the host, and the whole suite (`make test`) passes unchanged.

**Not measured: the gesture on the operator's own finger.** The instruments are the ones
that proved the pinch's *sending* half — `work/poke.py` drives the panel's pointer node with
logical coordinates and the shim log answers with
`pointsrc: wave zoom IN 1 step(s) at (600,300)` — and `work/zoomprobe2.py` is deployed and
looping (`/tmp/zoomprobe2.log`, a known-good `ZoomGridDirection` marker plus the gate's three
words) if the scale is wanted as a second witness. **A track must be loaded** on a deck before
rbp will zoom at all: `IsAbleZoom` @`0x113574` returns 0 unless one of the two per-deck words
at `0x03267238 + 0x1610` / `+0x1670` (stride `0x60`) reads 7 or 18, and an empty deck reads 1.

**Stated limit, and it is the honest half of "additive".** Because the gesture does not
consume the report, whatever rbp itself binds under the rect still fires: a *drag* along the
wave reaching rbp's own needle-search path is exactly the case the dominance rule is there to
keep apart from the zoom, but a press that is not vertical-dominant is still rbp's, and if
that turns out to do something unwanted in the operator's hands the cure is to swallow the
press at the down edge — which is the pinch's design, and would need the rect to be right for
every screen rbp can be showing.

## The BEAT FX panel's three controls (`fx_zone.c`, `fx_paint.c`)

> *"when i touch the ch select in performance view let it toggle from ch 1 -> ch 2 ->
> master, when i touch the beat fx have a popup menu with all the fx available so i can
> select"* — 2026-10-06
>
> *"here's a tweak, when i press the beat fx label have the menu popup but if i press the
> actual label of the beat fx (eg delay) enable the beat fx and disable if i press it
> again"* — 2026-10-07

Three touch interactions on **rbp's own BEAT FX panel** in the performance view, and the
operator chose rbp's own drawing over shim-owned cells every time: a finger lands on the
label that is already there. The third is the 2026-10-07 tweak, which re-homes the picker
onto the grey `BEAT FX` header bar and gives the black effect-name cell a **power toggle**
of its own:

| gesture | what it does | fires on |
|---|---|---|
| the grey **`BEAT FX` header bar** | raises the 14-row picker | the **release** |
| the black **effect-name cell** (`DELAY`) | **powers the Beat FX on/off** | the **press** |
| the filled **CH SELECT value box** | cycles the target | the **release** |

**They were dead to touch because rbp has no control for any of them.** Its `ui::touch_panel`
family contains `BeatFxAndXPad`, `BeatFxMode_BeatFx`, `BeatFxMode_Status`,
`BeatFxSelectItem1..4`, `BeatFxSelectTrash` and `Shortcut_EffectQuantize_On/Off` — a
panel/mode toggle, the X-PAD and the in-panel quantize — and nothing that selects the
effect, assigns its channel or enables it. All three reach rbp only as keycodes, the ones
the FLX4's own buttons already send, so this is a **wiring** job:

| gesture | call | where it is proven |
|---|---|---|
| CH SELECT target | `send_rx_key(K_BFXCH 0x448c`, `OP_VALUE, CH_GLOBAL, want)`, `want` ∈ {0 deck 1, 1 deck 2, 5 MASTER} | `map_flx4.c:493` — the lever |
| effect type | `send_rx_key(K_BFXTYPE 0x448b`, `OP_VALUE, CH_GLOBAL, pos)`, `pos` 0..13 | `map_flx4.c:520` — FX SELECT |
| **Beat FX on/off** | `send_rx_key(K_BFX 0x448d, OP_PRESS, CH_GLOBAL, 0)` then `OP_RELEASE` | `map_flx4.c:1087` — the FX ON/OFF button |

**rbp owns the toggle state, so the shim keeps none and cannot desync.** A tap on the
effect-name cell sends exactly what the FLX4's FX ON/OFF button sends, so it is equivalent
to pressing that button whichever the operator used last — and the log line names the
*send*, not the resulting state, because there is no enable word to read (the type word
`+0x50` was already measured dead, `docs/13-raspberrypi4.md` S5.6).

### The three rects, and how to re-derive them

Measured off rbp's own frame — the y values of the lower two from the committed capture
`work/fb_fx2.raw` (2026-09-30), the x values corrected against the **live** frame on
2026-10-06, and the header bar read row by row off a live `/dev/fb0` on **2026-10-07**:

| what rbp draws | logical x | logical y | |
|---|---|---|---|
| the grey BEAT FX plate, margins included | 1090..1269 | 47..490 | the mode check |
| **the grey `BEAT FX` header bar** | **1090..1269** | **57..85** | `FX_HIT_HEADER` — raises the picker |
| **the black effect-name cell** (`DELAY` there) | **1100..1259** | **98..137** | `FX_HIT_NAME` — powers the Beat FX |
| the grey `CH SELECT` label | 1090..1269 | 150..158 | |
| **the filled CH SELECT value box** | **1100..1259** | **168..203** | `FX_HIT_CH` — steps the target |

**The header bar is the FULL plate width while the two boxes below it are inset 10 px a
side** — the 2026-10-07 sweep found (48,48,48) across x 1090..1269 at y 57..85, then the
plate's (32,32,32) from y 86, so it is rbp's own header for the panel and not a box. That
asymmetry is asserted in `test_geometry` rather than typed twice: `FX_HEADER_X0 ==
FX_PANEL_X0` and `FX_HEADER_X1 == FX_PANEL_X1`.

The two boxes are 160 × 40 logical px each, 30 rows of clear plate between them and above
the header. **The hit rect is the drawn box and not the plate**: the plate's 10 px of
margin on either side stays rbp's, because a tap there is not a tap on the control. The
value box is independently confirmed by `docs/13-raspberrypi4.md` S5.6, where the
`1`-vs-`MASTER` frame diff is **x 1134..1225, y 178..194** — inside it.

**The swallow was measured before a line of this was written, at all three rects.** An idle
capture diff at the two boxes, tapping each twice with the shim doing nothing, changed
**0 px in the panel and 0 px in the pad strip**, while the positive control (the segmented
STATUS/BEAT FX toggle) moved **7098 px** in the panel and **77921 px** in the pad strip.
The header bar got the same treatment on 2026-10-07 with a positive control in the *same
injection run*: a tap at its centre (logical 1179,71) changed **0 px** in the whole panel
while the CH box, tapped in the same run, changed **726 px** (rbp's `1`→`2`). So rbp binds
nothing to any of the three, and withholding them costs the operator nothing.

### The gesture

`fx_feed()` is the zone's one door and it is `prompt_zone.c`'s shape, with **one control
firing on the press instead of the release**:

| the report | what happens |
|---|---|
| a press inside any rect | **ours**, and `press_hit` anchors which one. On the **effect-name cell** only, the press answers `FX_ACT_POWER` here and its release then answers nothing |
| a press anywhere else | **not ours — and it never becomes ours.** A finger that starts on the glass and slides across the panel stays rbp's from its first report to its last; clearing the anchor on a miss instead would re-open the press wherever the finger had got to, one report later |
| move, ours | still ours, even after the finger has left the rect — a control that swallowed the press must see the release or it goes deaf (`declined-press-must-still-see-release`) |
| release, on the SAME rect the press anchored to | `FX_ACT_CH` or `FX_ACT_PICK` fires, once; on the effect-name cell the release is swallowed and fires **nothing** (`FX_ACT_NONE`) |
| release, elsewhere | nothing, still swallowed |

The rect is taken from the **release's own coordinates**, not from the tracked one: a
fast flick can lift at a moved position with no move report in between. `FX_ACT_CH`
cycles the target, `FX_ACT_PICK` raises the picker.

**The anchor is also what protects the toggle.** A press that begins on the header bar and
slides down onto the effect-name cell fires **neither** control: the release is not on the
header, and the press did not begin on the name cell. That is the drag the operator would
never aim and the one the first draft would have fired. And a **run of downs** on the name
cell — which is what a touch panel sends while a finger rests — is one toggle, not one per
sample, because only the up→down edge fires.

**The gate is `getBrowseMode() == BROWSE_MODE_PLAY`**, re-read on every report — rbp can
leave the performance screen under a press that is already down, and a swallowed press on
a screen that no longer has the panel is a control firing on nothing. Nothing of the
shim's may be in front of it either: the right-hand drawer covers all three rects
completely while it is out, and the band's panel is drawn across them.

### `FX_ACT_CH`: the cycle reads rbp's own word

`fx_ch_cycle()` steps **0 → 1 → 5 → 0** from what rbp says it is right now, and *not*
from a cursor of the shim's own — a private cursor would desync the moment the operator
touched the FLX4's lever, which drives the same value. The read is `pointsrc.c`'s, a
pointer chase that fbshim *can* do (`rbp_bridge.o` is not in this shim — `Makefile:292` —
so it is not `rbp_beatfx_type()`):

```
DjEngineIF::getBeatEffectSelectChannel @0x4d314
  ldr r4,[pc] -> ME_SINGLETON (0x011493c0)
  ldr r0,[r4] -> MixerEngine*
  ldr r0,[r0,#0x58] -> BeatEffectManager*
  ldr r0,[r0,#0x00] -> the channel     0 = deck 1, 1 = deck 2, 5 = MASTER
```

**Measured on the unit 2026-10-06** by driving the FLX4's own lever: CH1 → `0`, CH2 →
`1`, MASTER → `5`, back to CH1 → `0`. `+0x04` (the word `setBeatEffectSelectChannel`
writes) tracked `+0x00` exactly, so the read does **not** lag and no cursor fallback is
needed. Both interior pointers are NULL-checked and `is_rbp_process()` is asked first, so
a half-built or torn-down engine answers −1 and the cycle then starts at deck 1, rbp's
measured cold-start default, and says so in the log.

### `FX_ACT_PICK`: one column, fourteen rows

**The box is rbp's own BEAT FX plate's rectangle, drawn over the panel** — 180 × 444
logical px at x 1090..1269, y 47..490, the plate's own bounds (`FX_X0 == FX_PANEL_X0`
and the other three edges likewise, asserted rather than typed). It reads as the panel
turning into a list: the control the finger just left is behind the box, not beside it.
The operator's ask was *"the same dimensions of the beatfx box ... you don't need a title
menu saying beat fx either"*, and the first build instead centred a 340 × 535 box with a
title; the restyle deleted the title, the rule and the frame and took the plate's
rectangle.

**It is raised by the header bar since 2026-10-07, not by the effect-name cell** — the
operator's tweak moved it up one row so the cell could become a power button. Because the
box still covers the whole plate, a tap where the header was is picker **row 0** while the
box is up; that is the same overlap the name cell had until 2026-10-07 and the operator did
not ask to change it. The box still dismisses on a tap outside it, and on a 10 s timeout.
Turning the Beat FX on *from* the picker is out of scope — the toggle is its own control
now.

The height is **the exact sum of its parts** — a 6 px margin, fourteen 29 px cells and
thirteen 2 px gaps: 2·6 + 14·29 + 13·2 = 444, asserted rather than trusted. `FX_EDGE_X`
is 10 and not a round number of the shim's own, because it is **rbp's**: his black
effect-name cell is inset ten pixels from the plate on either side, so a row here lands on
exactly the same 160 px width as the cell it replaces. The rows wear rbp's own colours
(`menu_paint.h`'s `MENU_FX_PLATE`, `MENU_FILL`, `MENU_LABEL` and `MENU_FX_SEL`), all four
sampled off a live `/dev/fb0` capture, and **nothing draws a frame round a row** because
rbp's cells have none.

**Covering the panel costs nothing, and the reason is in `pointsrc.c`.** The picker is
asked first in the ladder, so while it is up every report over the panel is the picker's
and neither of the two hit rects underneath can fire. Its first nine rows and the
right-hand drawer are the same 180 px column at the same edge, and the first nine rows
also sit under the swipe-down band's strip; neither can be on the glass with it, because
the FX rung refuses while any drawer or the band is open and is asked before either can
open on a report the picker is holding. Nothing in `fx_zone.c` enforces that.

**The names are MEASURED, not derived.** The FLX4's own FX SELECT note was injected
fourteen times from a cold start with the shim logging each position as it sent it, and
the word read off rbp's effect-name cell after every step:

| pos | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| name | DELAY | ECHO | PING PONG | SPIRAL | HELIX | REVERB | FLANGER | PHASER | FILTER | TRANS | ROLL | SLIP ROLL | PITCH | VINYL BRAKE |

Three independent anchors agree — `rbp_abi.h:381`'s own ABI comment says *"0=Delay,
1=Echo"*; position 11 reads SLIP ROLL, the effect the panel was already showing; and
position 5 reads REVERB, the type an earlier `pos → type → icon-resource` derivation had
put at 0. That derivation scrambles, and reading rbp's display is the only thing that
settled it. Switch-position order is the presentation because it is the order the
operator's own FX SELECT button steps through.

**There is no "current effect" highlight**, and that is a measurement. rbp's type word
`BeatEffectManager+0x50` (`ldr r0,[r0,#0x50]`, the tail of
`DjEngineIF::getBeatEffectType @0x4d514`) reads **0 at every switch position** while the
panel shows SLIP ROLL, REVERB and VINYL BRAKE in turn; stepping the effect moved
`+0x140`, `+0x11c..+0x130` and the pointer at `+0x0c` and left `+0x50` at 0. The FLX4's
own cursor falls back for the same reason (`map_flx4.c` seeds from `rbp_beatfx_type()`
once and the log opens drill 1 at *"type position 1"*). A light pinned to a word that is
always 0 would sit on DELAY forever, so the box opens with nothing marked and the
operator's own FLX4 panel remains the honest witness for which effect is live.

### The picker's state machine

`prompt_zone.c`'s, deliberately down to the field names, because that shape has already
been through a drill on this unit:

- **Asked first in `pointer_report()`'s ladder**, above the band, the drawers and rbp, so
  while it is up it owns **every** report wherever it lands and a press that dismisses it
  cannot also press what is underneath. `pointer_report_alt()` refuses while it is up too.
- **Nothing is inherited.** It opens with no press behind it (the raising tap fired on a
  release) and with no row armed.
- **It fires on release**, and only when the release's own coordinates are on the row the
  press anchored to.
- **A miss, a slide, or a tap outside closes it and sends nothing** — an honest no-op, not
  a silent action.
- **It self-dismisses after 10 s** (`FX_TIMEOUT_MS`), asked from the read loop's slice the
  way `prompt_expire()` is, and **never while a finger is down** — rbp always gets the
  release for a press the box swallowed.
- **It can never be up at the same time as the USB STOP chooser.** The chooser is asked
  first in the ladder, so its own feed swallows a tap that would raise the picker; and the
  keycode path that raises the chooser closes the picker, because a keycode is not a tap
  and can arrive with the picker still on the glass. Both boxes are opaque.

### Drawing: its own plane

The picker is presented on a **fifth `drm_band`**, mirroring the chooser's
(`menu_draw.c`'s `menu_fxlist_*`), at the box's own size. It is its own slot rather than
the chooser's: sharing one slot would make the two boxes mutually exclusive by
construction, and getting *that* wrong shows one box's pixels in the other's buffer —
while a refused plane costs nothing, because the picker falls back to the page route the
chooser already uses. The fear that motivated sharing (a fifth plane refused by vc4) is
measured and dead: the unit's `/sys/kernel/debug/dri/1/state` lists plane[0] through
plane[29]+, and this shim already holds five.

**A COPY OF A SURFACE HAS ONE PIECE THAT CANNOT BE COPIED ALONG WITH THE REST, AND THIS
ONE WAS MISSED: the call site.** `menu_fxlist_build()` was written, its image was
host-tested, and **nothing ever called it** — the builder thread called
`menu_prompt_build()` and not its sibling. The picker therefore drew **nothing at all**:
with a plane available `g.fxlist_valid` never became 1, `menu_fxlist_live()` published an
image that did not exist, and the plane sat set up and unwritten. Everything except the
pixels agreed — the log printed its `menu plane: crtc 102 plane 127 fb 725 180x444` line,
taps answered rows, the effect changed on rbp's panel. The tell is that a live plane reads
`crtc=pixelvalve-2` and its fb id in `/sys/kernel/debug/dri/1/state` while the picker is
open, and this one read **`fb=0 crtc=(null)`**. Fixed by the one missing line in the
builder loop. **The compiler had been flagging it on every build** —
`warning: 'menu_fxlist_build' defined but not used [-Wunused-function]` — and it was lost
in a `tail` on the build's output, so the habit worth keeping is to read a build from the
top as well as the tail.

### The switch

`POINT_FX_TOUCH` (default on) turns all three interactions off and gives the whole panel
back to rbp: the zone is not fed at all, and a box left up from before is put away at
startup and on a pointer device that goes away, beside `menu_reset()`. It is in
`start-rb.sh`'s `SHIM_VARS`, so `rb.conf` can set it as `RB_POINT_FX_TOUCH`.

### What is measured, and what is not

`test_fx.c` is **632 checks**: all three rects inside rbp's plate and none touching
another; the header bar spanning the plate's full width and the two boxes below it inset;
the box's four edges EQUAL to the plate's, and its height the exact sum of its parts (a
margin, fourteen cells and thirteen gaps); every row inside the box, the right size, the
width of rbp's own effect-name cell and clear of its neighbour; the three hit rects' edges
inclusive and one pixel outside not; a press that began elsewhere never adopted, first
report to last; **a name-cell press answering `FX_ACT_POWER` on the PRESS and its release
answering nothing, with a run of downs firing once**; the header and CH box still firing on
the anchored release; a header press that slid onto the name cell firing neither; the
picker owning every report while it is up and answering for none while it is shut; a row
answering its own switch position; a release with no press behind it swallowed but not
closing the box; the timeout never closing under a finger; every pixel of the image
written, the paint idempotent at both depths, and a pressed row's difference confined to
that row's own rect. All 632 pass on the host and the whole suite passes unchanged. (It was
599 before the 2026-10-07 tweak, and 624 before the restyle; the checks the restyle removed
were the title, the rule, the centring and the mirror relations.)

**The two new rules are mutation-verified**, because a press-edge rule is exactly the kind
of thing a copy of the test would not notice: mapping the header to `FX_HIT_NONE` fails
**10** checks, and moving the effect-name cell back to firing on the release fails **6**
(including *"the name cell's RELEASE fired a second act — a tap must fire once"*).

**What a machine settled, 2026-10-06, before the operator was asked to look** — the same
prime → press → 130 ms → release injector drill 0 used, aimed at the finished build.
Three taps on the CH box took **rbp's own word** `0 → 1 → 5 → 0`, read through
`/proc/<pid>/mem` at the chase above rather than off the shim's log, with the log's
`beat fx ch select N -> …` line as the second witness; rbp's panel read `1` afterwards.
A tap on the effect-name cell raised the box (`beat fx picker raised (14 rows)`), the
picker took its own plane, and a tap on row 13 sent `type position 13 (VINYL BRAKE)` with
rbp's own effect-name cell reading **VINYL BRAKE** off the framebuffer afterwards. What no
machine settled is the comfort of the two rects under a thumb, and that is the operator's
own verdict — the same sentence the QUANTIZE zone needed.

**What the machine settled for the 2026-10-07 tweak, and how** — a true **press with no
release at all**, because that is the only injection a release-firing build cannot pass.
`poke.py`'s `finally:` emits an unconditional release, so this drill drove the panel node
directly: `prime → press → sleep → read the log → release → read the log`, at raw (1770,96)
for the header and raw (1770,159) for the name cell.

```
HEADER    press, no release   ->  (nothing)
          ...release          ->  pointsrc: beat fx picker raised (14 rows)
NAME CELL press, no release   ->  pointsrc: beat fx power toggle sent
          ...release          ->  (nothing)
```

The header's silence on the press and the name cell's *speech* on it are the two halves of
the edge rule, and the same run's later steps are the two negatives: a press that began on
the header and slid onto the name cell fired **neither** control, and a name-cell press
that slid off the plate fired exactly **one** toggle — on the press — with nothing on the
lift. The picker's overlap is unchanged and deliberate: while it is up a tap where the
header was is picker **row 0**, which is why the drill above waits out the 10 s timeout
between the two steps. Turning the Beat FX on from the picker is out of scope; the toggle
is its own control now.

**And the operator's own verdict, 2026-10-07:** *"ok, works correctly"* — the header, the
power toggle and its repeat-off all behave under a real finger on the glass. That is the
last step the feature needed; nothing here is waiting on a drill.

## The BPM cell as a momentary X/Y pad for the Beat FX (`fxpad_zone.c`, `fxpad_paint.c`)

The operator, 2026-10-07, on the same panel:

> in the beat fx window where the BPM detail is displayed allow it to be a x/y pad to
> control the beat fx x for level and y is for how many beats, engage when pressed down
> and allow dragging within the square to work and then when you release set it back to
> what the beat and level was before you pressed it.

Below the CH SELECT box rbp draws a black cell reading `125.0 BPM / 480 msec / 1 BEAT /
QUANTIZE`, and binds **no touch to any of it** (measured — the swallow table below). So the
one dead square on the panel becomes the place where the effect's two continuous values can
be flown by hand: X is the effect's **level** (its depth), Y is **how many beats**, it is
engaged while the finger is down, and letting go puts all of it back exactly where it was.

**Three answers are decisions and not defaults**, given when the operator was asked:

* **Y drives the discrete beat ladder** (`BEAT <` / `BEAT >`: halve / double), not the
  continuous TIME knob — the cell already prints the beat count, so the ladder is the value
  that can be read back while dragging.
* **The press also turns the Beat FX ON**, and the release puts the on/off back too. A
  momentary pad that changed the level of a switched-off effect would move nothing audible.
* **The pad draws a frame and a finger dot**, because the LEVEL has no readout anywhere on
  rbp's panel — only the beat count and the time are printed — so without a dot X is flown
  blind.

### The cell

Measured off a live `/dev/fb0` capture, 2026-10-07 (the BEAT FX panel on the glass):

| what rbp draws | x | y |
|---|---|---|
| the black BPM detail cell | **1100..1259** | **216..352** |

160 × 137 logical px — the **same ten-pixel inset** from the grey plate that the effect-name
cell and the CH SELECT box wear — and clear of all three `fx_zone.h` rects: the CH box ends
at y 203 and this cell begins at y 216, with thirteen rows of plate between them. Row 352 is
rbp's own `8,8,8` border, so the rectangle stops there rather than running into the plate
below. The `QUANTIZE` word inside the cell is rbp's already-measured-dead
`Shortcut_EffectQuantize_On/Off`.

### The swallow, measured before a line of this was written

The shim doing nothing, `work/tap.py` writing records straight into rbp's pipe, counting
changed pixels **inside the panel only** — a whole-frame count is useless here, because
rbp's deck animation moves ~46,000 px on its own, all of it outside x 1090..1269:

| region | idle | tap cell | tap cell | tap toggle |
|---|---|---|---|---|
| BPM cell (1100..1259 / 216..352) | 0 | **0** | **0** | 0 |
| STATUS/BEAT FX toggle | 0 | 0 | 0 | **7098** |
| whole panel (1090..1269) | 0 | 0 | 0 | 8178 |

Two taps in the cell: **0 px**, twice; the control in the same run moved **7098 px**. So
withholding these reports costs the operator nothing.

### rbp's own values, read not inferred

The pad moves rbp's effect, so it has to read it first — and the read is a pointer chase
`pointsrc.c` already performs for the CH cycle. Disassembled out of
`extracted/XDJRX3/pdj/rbp` on 2026-10-07 and then read live through `/proc/<pid>/mem`:

```
ME_SINGLETON 0x011493c0 -> MixerEngine
  MixerEngine +0x58       -> BeatEffectManager        <- fx_ch_read() is already here
    BeatEffectManager +0x00  int   select channel     = 5    (screen: MASTER)
    BeatEffectManager +0x08  ptr   BeatEffect*        <- the CURRENT effect object
      BeatEffect +0x20  float  level / depth          = 0.409   (later read 0.5044)
      BeatEffect +0x24  long   time, msec             = 480     (screen: "480 msec")
      BeatEffect +0x3c  byte   effect ON/OFF          = 0       (later read 1)
      BeatEffect +0x44  long   beat button rung        = 5       (screen: "1 BEAT")
      BeatEffect +0x48  long   beat button max         = 9
      BeatEffect +0x4c  long   beat button min         = 0
```

Every accessor behind those is a two-instruction load, so this is a plain chase with **no
function call into rbp** — the same shape and the same risk as `fx_ch_read()`, with both
interior pointers NULL-checked. The two readings differing is the point: **nothing here is a
constant**, and the pad **refuses to engage** at all when any link of the chase is missing,
because a pad that cannot read the old value cannot put it back.

### What the wire does to them, measured on the unit

Sent through the shim's own sequencer port (`seqinject2`), which runs `map_flx4.c`'s real
handlers — whose last hop is byte-identical to the two calls the pad makes — with rbp's
words read back out of `/proc/<pid>/mem` after each one:

| what | wire form | read back |
|---|---|---|
| level | `K_DEPTH 0x448f`, `OP_VALUE`, `CH_GLOBAL`, `v`, `v/1023.0f` | cc 100 → `+0x20` = **0.7879** (= 806/1023); cc 20 → **0.1574** (= 161/1023) |
| beats | `K_BEATNEXT 0x4491` / `K_BEATPREV 0x4490`, `OP_PRESS`, `d = +1 / -1` | rung **5 → 6 → 7**, and **7 → 6** |
| on/off | `K_BFX 0x448d` PRESS then RELEASE | not re-sent in that run — it is the audible one and the operator's own hand has already proven it |

Three facts worth carrying out of that run. **`K_DEPTH` is ABSOLUTE and exact** — one send
lands the level, one send restores it, no feedback loop needed. **The ladder's directions
are measured, not inferred from the button's name**, and one press moves exactly one rung.
And **rbp repaints the cell itself**: at 125.0 BPM the same square went `480 msec / 1 BEAT`
→ `960 / 2 BEAT` → `1920 / 4 BEAT`, following `+0x24` and `+0x44` — so the panel is a live
readout of this struct, rung 5 is "1 BEAT" and the ladder doubles visibly, and `+0x24` is
rbp's own recomputation rather than a second copy of the rung.

`OP_PRESS` on the two BEAT keys is **required**: `asEventCode` gates `0x4490`/`0x4491` on
`(op & 0xf) == 0` (`docs/08-controls.md`). Every one of the injected values was put back
before the run ended, and the unit was re-read afterwards to confirm it: depth, rung and
on/off all at their starting values.

### The state machine

* **The press point sets BOTH axes immediately**, which is what makes it an X/Y pad rather
  than a knob. A press inside the cell engages it; a press that began **outside** is rbp's
  for the whole of its gesture, however far it then travels across the panel. Every report
  until the release is then **ours**, including one that has slid off the cell — a feeder
  that swallowed the press must see the release or it goes deaf, silently.
* **X → level**: `round((x - 1100) / 159 × 1023)`, sent whenever it differs from what was
  last sent. `K_DEPTH` is absolute, so it needs no loop and restores in one send.
* **Y → beats**: the top of the cell is the **most** beats. The target is a rung index, and
  it is reached by **hill-climbing on rbp's own answer** — at most one rung per tick, in the
  direction that closes the gap, and no second step until rbp has moved off the rung the
  last step was taken from. That needs no knowledge of the ladder's spacing and cannot
  overshoot or double-step when rbp applies a key a tick late.
* **The snapshot** is taken on the first tick after the press — the only moment at which
  rbp's `on`, `depth` and `beat` are still "what they were before the finger touched". That
  snapshot *is* the operator's "set it back to what the beat and level was before you
  pressed it".
* **X, then Y, then the on/off.** The level is absolute and needs no loop at all; the ladder
  is climbed on rbp's answer; and the on/off, which is a **toggle**, is *asked for and then
  watched* — see below. It is the one axis whose wire form cannot be driven like a value.
* **The restore** is driven by the same loop, with the targets set back to the snapshot, and
  goes idle when they are home. It cannot live in the release report: there is no report
  after a release and rbp applies keys asynchronously, so climbing on rbp's own answer is
  the only honest way to land on the exact rung again. **Home means arrived, not asked for**
  — the level is the one exception, and only because asking for it *is* putting it there. A
  long drag unwinds in a few hundred ms.
* **It gives up rather than spins**: a ladder rung that does not answer within
  `2 × (max - min) + 4` ticks stops the beat axis for the rest of the gesture, and an on/off
  toggle rbp never answers stops after `FXPAD_ON_TRIES` asks, `FXPAD_ON_WAIT` ticks apart.
* **A tap between two ticks sends nothing at all** — no tick ever saw it engaged, so there
  is nothing to undo. That is not a special case: the whole module rests on the rule that
  the only thing worth undoing is something that was actually sent.

The tick block is the fourth of its kind in `pointsrc.c` (after the drawers', the chooser's
and the picker's) and has the same shape: while the pad is busy, read rbp, tick, send what
comes back, wait in 20 ms slices. That is what lets the ladder converge **with the finger
held still** and what services the restore after the finger is gone. The sends go out back
to back with **no sleep** — a wait in this loop starves the band, the window, the drawers and
all three boxes.

### The HUD: the dot, and the frame that was taken off it

`fxpad_paint.c` draws **a dot at the finger** — and, until 2026-10-07, a frame round the cell
as well. It is the family's **first module that copies** rather than fills, and the reason is
the measurement above: rbp repaints that cell itself and its `BPM / msec / BEAT` is the
operator's only readback — so the HUD must be drawn **over a copy of rbp's own pixels**, never
in place of them. A plane is RGB565, i.e. opaque, so each tick takes rbp's cell out of the
framebuffer into plane six's buffer and then draws the mark on top. The copy is the cell and no
more: 160 × 137 at the page's scale, ~44 KB a tick. On the page route there is nothing to copy
— the destination *is* rbp's pixels — so `fxpad_paint()` is handed a NULL source and writes
only the dot; **that route must never fill**, and `test_fxpad.c` pins it, because filling there
would black out the readout the operator is reading.

**The frame is gone, and the reason is worth keeping.** It was a one-pixel `MENU_BORDER`
rectangle drawn on the cell's outermost row and column — which are rbp's own `8,8,8` border,
chosen deliberately so that the ring replaced a line that was already there and cost the
readout no pixel. That reasoning was sound about *space* and wrong about *time*: rbp repaints
that border itself, so the same pixels were written by both of us at different rates and the
ring strobed. The operator, on the glass:

> you don't need to draw the white box outline, it flashes so its too distracting

It is out. The cell's border is rbp's and stays rbp's, and what is left is the one mark with
information in it — the level has no readout anywhere else on the panel. `test_fxpad.c` now
pins the **absence**: an unmarked paint must write rbp's pixels byte for byte and add nothing
at all, which is a stronger test than the one it replaced.

The pad's plane is the **sixth `drm_band`** and the only one that goes up for a **finger**
rather than for a box, so the tick services it **first**, before any route is decided: a pad
left up would be an opaque rectangle with a frozen dot sitting on the operator's own BPM
readout. It is **hidden and shown per gesture but set up once** — the five boxes tear their
plane down when they close, which they can afford because a box is up for a deliberate
session; the pad's lifetime is a tap, used dozens of times a set, and what actually takes the
rectangle off the glass is `drm_band_hide()` (one ioctl). It cannot be engaged while any box,
drawer or window is up — `pointsrc.c`'s press gate asks every one of them before a finger can
take the cell — so this plane and theirs are never up together.

### The press that did not trigger, and the loop it bought

The pad's first hour on the glass produced one more report, and it is the sharper of the two:

> when i change effects it doesn't seem to remember to trigger when i press until i turn it
> on off can you fix?

The pad's on/off was **one blind toggle, sent once, on the snapshot tick, and never checked**
— the module latched what it had *asked for* and read rbp's answer never again. Every other
axis here has a loop: the ladder climbs on rbp's own word. The on/off had none, and it is
exactly the axis whose wire form cannot be driven like a value, because `K_BFX` is a *flip*.
A flip rbp drops is indistinguishable, from the shim's side, from a flip it never needed — and
the operator's workaround, cycling the effect's own power, is what "until i turn it on off"
describes.

It is now **asked for and then watched**: the pad reads rbp's answer back and asks again —
up to `FXPAD_ON_TRIES` times, `FXPAD_ON_WAIT` ticks apart — while the answer still disagrees.
Three things fall out of that, and only the first was the point:

* A toggle rbp **drops** is re-asked, so the press is not silent for the whole gesture.
* While the finger is down, an effect rbp switches **off underneath it** is asked for again on
  the next tick instead of leaving the rest of the gesture dead. This is the shape the
  operator's "when i change effects" describes, and it costs nothing when it does not happen.
* The restore now needs rbp's answer to have **arrived**, so `beat fx pad unwound` is rbp's
  state and not the shim's intent. Read off the unit's log before this change, that line was
  reporting `effect on` immediately after a `-> effect off` it had not yet been applied — a
  witness line that was wrong in a way nothing would have caught.

Asking twice is safe here for a reason the level does not share: **the answer is read**, so a
second ask is a correction of a flip rbp did not make, not a blind repeat of one it may have.
The bound is what keeps a toggle rbp will never answer from flipping the effect under the
operator's hand for as long as they hold the pad.

**What this fix does not claim.** The log from the operator's own session shows every engage
after their effect change reading `effect on` in the snapshot — so the pad found nothing to ask
for. Whether that was rbp lying was, at the time, unanswerable from `+0x3c` alone: the answer is
the only truth the shim had. **It has since been answered, and the answer is that it *was*
lying** — see the next section.

### The flag that lies, and the second word that does not (2026-10-07, later the same day)

The operator's *"when i switch an effect and then press the x/y pad it doesn't engage, but it
does after i turn the newly selected effect on/off"* turned out not to be a dropped toggle at
all. **`BeatEffect+0x3c` reads ON on an effect that is not running**, and the pad believed it.

rbp's Beat FX state is two words, both of them one `ldr` off the same `BeatEffectManager`:

| word | accessor | what it says |
|---|---|---|
| `+0x50` | `getBeatEffectType()` @0x89acc — `ldr r0,[r0,#80]` | the effect **type**; **0 is the Off state**, the one entry of the 14-position table that maps to no position |
| `+0x3c` | `isBeatEffectOn()` @0x89964 — `ldrb r0,[r3,#60]` | the effect's own ON/OFF flag |

When the type is 0 the manager leaves a **`BeatEffectOff`** at `+0x08`, and that class has every
control the pad drives compiled out — `changeEffectStatusToOn` @0x8b0b8, `changeEffectStatusToOff`
@0x8b0b4, `changeLevelDepthValue` @0x8b0ac and `changeTimeValue` @0x8b0b0 are each a bare `bx lr`.
Nothing maintains its `+0x3c`, so it sits at **1**. rbp's own `setBeatEffectOnOff` @0x89940 reads
`ldrb r3,[r0,#60]; cmp r3,r1; popeq` — so even rbp's own ON *would* see "already on" and return,
if it were routed at the Off object; it is not, because rbp's ON/OFF button re-points `+0x08` at
the selected type's real object first, and *there* the flag means something. That is the
operator's workaround, exactly.

**Measured on the unit, one step at a time**, with a read-only `/proc/<rbp>/mem` walk — the
operator's own repro, one injected `FX SELECT` press:

```
before              type(+0x50)=0  +0x3c=0  obj -> BeatEffectPingPong (a real class, and off)
after ONE FX SELECT type(+0x50)=0  +0x3c=1  obj -> BeatEffectOff (every virtual `bx lr`)
```

So moving the effect selector **once**, while the effect is off, leaves the type at 0 *and*
flips the flag to 1. The pad read the flag, agreed with itself, sent no `K_BFX` — and every other
send it made, the level and the rung, was a no-op on a class that ignores both. The press did
nothing, anywhere. That is *"it doesn't engage"*.

**The fix is a conjunction, not a new send.** ON is `+0x50 != 0 && +0x3c != 0`, written once, in
`fxpad_zone.c`'s `live_on()` and exported as `fxpad_live_on()`. Nothing about the wire changed;
the pad simply asks for the toggle in the case it used to sit still for. `test_fxpad.c`'s
simulated rbp **models the lie** — it answers `+0x3c` from the type when the type is Off — which
is what makes the new test able to catch the old read; with the honest fixture it cannot.

**And it is a conjunction, not an equivalence**, which is the part worth keeping. A freshly
started rbp whose Beat FX has never been touched *also* reads type 0 — with a stale **real**
object at `+0x08` whose own class maintains the flag, so that one reads `+0x3c = 0` and is right.
Two type-0 states that say different things: `+0x3c` alone is meaningful in one and a lie in the
other, and `+0x50 == 0` answers "off" correctly in both. `rbp_abi.h` carries the table.

**A refusal is now named, and that is a second, separate fix.** There was no log line at all when
the pad declined to engage, and no line naming which link of the chase failed — so *"it never
engaged"*, *"it engaged and sent nothing"* and *"the shim was not running"* were the same silence,
which is what kept this from being a five-minute diagnosis. The read is now
`fx_state_read_why()`, and a press inside the cell that it refuses logs
`beat fx pad refused at (x,y) -- <the link>`. It fires only on a press that *would* have engaged,
so a release, a miss and a drag across the panel cost nothing.

### The switch

`POINT_FX_TOUCH` (default on) turns this off with the panel's other three interactions: the
pad is not fed and is reset, beside `fx_reset()` and the rest, at startup and on a pointer
device that goes away.

### What is measured, and what is not

**`test_fxpad.c` is 786 checks**: the restore exact in both directions of the ladder with the
on/off toggled exactly twice; **the flag that lies** — an Off-typed object reporting ON must read
OFF, and the press on it must send the toggle it never used to send, in a fixture that reproduces
the lie; the tick quiet when nothing has changed (a held-still finger is the normal state, and a
re-sent ON toggle would switch the effect back off); **a dropped toggle re-asked and then let go**,
and **an effect that goes off under the finger asked for again**; **a restore that waits for the
answer rather than for the ask** — asserted both ways, that a refused on/off is retried and
bounded and that an answered one still costs a single send and no extra ticks; both mappings at
their ends and across their span; the climb from both ends, against a rbp that applies each step
a tick late, and against one that never answers at all (give up, not spin); the latch — a press
this module swallowed stays ours until it lifts; the cell inside rbp's plate and overlapping none
of the three `fx_zone.h` rects; and the HUD, which is about pixels: **which of rbp's survive**.
That last section pins the copy (against a non-uniform page and a source with its own stride), the
dot's locality and its extreme, idempotence, the page route's promise that only the dot is written
— and the frame's **absence**, which is now a regression guard rather than a description.

The conjunction is pinned by a deliberate-break run: reverting `live_on()` to `live->on != 0`
turns `test_fxpad` red in **6 places**, one of them reading *"THE PRESS MUST ASK FOR THE EFFECT —
this is the K_BFX that never went out (got 0 sends)"*, which is the operator's symptom in the
test's own words.

**The pad has been used on the unit, and this is what the glass said.** The operator's own
session is in `/tmp/pointsrc.log`: 41 engagements, 3097 level sends, the ladder walked up and
down, and — read back at the top — a restore that landed on the level and the rung it started
from every time. That run is what produced the three reports above, the frame's removal and the
flag finding. What is still only theirs to settle: whether the press now triggers when they
change effects, the comfort of the two axes under a thumb, whether the press-point-sets-both-axes
choice is the right one (the alternative, "the first move sets the value", is a one-line change
to `fxpad_feed`), and whether a dot is enough to fly the level by. The pad's log lines — `beat fx
pad engaged at (x,y) -- type N, effect on/off`, one per axis sent, `beat fx pad refused at (x,y)`
when a press is declined, and `beat fx pad unwound` with the final state — are the instrument for
reading a drill back.

## The HOT CUE pad row (`hc_zone.c`)

The operator, 2026-10-06:

> also on the performance screen, make hotcues activatable with touch.  when i press a
> hotcue, but only if one is registered to it.  if there is no active hotcue do nothing
> and do not create a hotcue.

rbp draws a HOT CUE grid across the bottom of each deck — four cells to a row, two rows,
eight pads a deck, lettered A..D and E..H — and binds **no touch to any of it**, so the row
is inert under a finger today. This section makes the sixteen cells fire their own cue.

**The second sentence is the whole feature, not a nicety.** On real Pioneer gear an unlit
HOT CUE pad *stores* a cue at the playhead. So the failure this is gated against is not a
tap that did nothing: it is a tap that **wrote a cue onto the operator's own track**, on a
pad they believed was empty, mid-set. Every gate below is therefore built to fail towards
doing nothing, and `test_hc.c` names that direction in its failure messages so a future
edit cannot quietly reverse it.

### The sixteen cells

Re-derived 2026-10-06 from a live `/dev/fb0` capture of the performance screen, by scanning
the pad strip for its drawn edges; `docs/07-touch.md`'s performance-screen table above
carries the same two rows and is the independent check (one source would only be this
document agreeing with itself).

| what rbp draws | y |
|---|---|
| `HOT CUE` label row | 499..509 |
| row 1 cells (A..D) | **518..537** |
| gap | 538..547 |
| row 2 cells (E..H) | **548..567** |
| `DECK` strips | 581.. |

| deck | the four cells' x |
|---|---|
| 1 | **11..158, 168..315, 325..472, 482..629** |
| 2 | **651..798, 808..955, 965..1112, 1122..1269** |

148 px wide, a 9 px gutter between, pitch 157; 11 px of screen margin on the left and 10 on
the right, and 21 px separating the decks (deck 1 ends at 629, deck 2 starts at 651). Pad
numbering is rbp's own: row 1 is pads 1..4, row 2 is pads 5..8.

**The 20-row height is deliberately thin.** The drawn cell is the visual promise — the rule
`fx_zone.h` states for its two boxes — and padding these rects out to fill the gutter would
make a tap in the 10-row gap, which is visibly neither pad, fire one of them. A touch that
lands in the gutter, in the gap between the decks, or on the label row does nothing at all.

### The gate, and the three reads

`hc_may_fire(browse_mode, pad_mode, registered)` is the whole rule, and all three terms
must hold:

| term | read from | why it is there |
|---|---|---|
| `browse_mode == 1` | `uiBrowse`'s mode word | the pad row is only on the **performance** screen |
| `pad_mode == 0` | `UiGetPadMode(deck)` @`0xfd3cc` | **rbp's pad keycodes mean whatever the current mode makes them mean** |
| `registered > 0` | `isRegisteredHotCue` @`0x48b00` | the operator's rule: only a pad that already has a cue |

**The mode gate is not decoration.** `K_PAD1+p` is a hot cue in HOT CUE, a 1/8 beat loop in
AUTO BEAT LOOP and a beat jump in BEAT JUMP, so a tap on a drawn cell while the deck is in
any other mode would send *that* mode's action. Engaging a beat loop because a finger landed
on a pad is exactly as unwanted as creating a cue, and it is the same fix: a deck not in HOT
CUE mode gets nothing from this module.

**The registration read is a direct call, because there is nothing to chase.**
`djengine::DjEngineIF::isRegisteredHotCue` @`0x48b00` saves `r1` and `r2` and **reloads `r0`
from its own singleton** (`ldr r6,[pc,#152]` then `ldr r0,[r6]`, at `48b08`/`48b10`), so the
`this` argument is dead and `NULL` is passed because nothing is ever read from it. What it
forwards is the channel — the **0-based deck index**, which is what rbp passes — and the
**pad number 1..8**, not an index. `rbp_bridge.c`'s `hotcue_delete()` has been making this
same call on this same address since 2026-10-01, and that is the provenance. It was chosen
over the two alternatives deliberately: the `LedStat` `+20` word would need the 1.5 ms
double-read the LED mirror uses to avoid a torn read, which fbshim cannot afford on the
input path and whose torn value lands in the dangerous direction; and the `Player+0x479`
bitmask has comment-only provenance.

**And every read answers its failure as "no".** A pad mode that cannot be walked is `-1`; a
registration that cannot be asked, or is asked outside rbp, is `0`; a half-built engine
answers nothing. `hc_may_fire()` refuses on all of them, which is the operator's rule
arriving at the safe answer by construction rather than by a branch somebody has to
remember.

### The keycode arithmetic, which is the other way to write a cue

`hc_pad_keycode(pad)` returns `K_PAD1 + (pad - 1)`, and it is a function rather than an
inline expression precisely because of the off-by-one. rbp's own arithmetic is
`pad = keycode - 0x4116`, read straight off the instruction stream of
`ui::Player::onHotCueEvent` @`0x2f5720`:

```
2f575c: sub sl, r8, #16640   @ keycode - 0x4100
2f5760: sub r5, sl, #23      @ keycode - 0x4117, range-tested 0..7
2f578c: sub sl, sl, #22      @ keycode - 0x4116  =  the PAD NUMBER
```

So pad 1 is `K_PAD1` `0x4117` and pad 8 is `0x411e`. `K_BEATJUMP` `0x4116` is one below the
range: it is a pad-*mode* key, not pad 0. **Getting this wrong by one does not produce a pad
that does nothing — it triggers the neighbouring cue, and on an empty neighbour it creates
one**, which is the exact harm the whole feature is gated against, arriving through the
arithmetic instead of through the gate. `test_hc.c` pins all eight, in both directions
(`k - 0x4116 == pad`, and no pad produces a mode keycode).

The send is `map_flx4.c`'s and `aloop_apply()`'s: one `OP_PRESS` and one `OP_RELEASE`,
back to back, on rbp's own channel for the deck (`deck + 1`) — which is what keeps deck 1's
tap from firing deck 2's cue.

### The gesture

`hc_feed()` is `fx_zone.c`'s shape, and it is on the same rung of the ladder — after every
shim surface and before rbp's own screen — because these are pads rbp **paints**, so
anything drawn over them gets first refusal. That matters concretely: the right-hand drawer
(x 1100..1279, full height) lies across deck 2's last two cells while it is out.

- **A pad fires on the PRESS, not on the release.** A hot cue is a jump: on real gear the
  pad acts the moment it goes down. `hc_feed()` therefore answers the cell on the up→down
  edge that lands on one, and the release that follows answers `-1` — it is swallowed, never
  fired, so a tap sends exactly one key. This is the operator's own correction of the first
  build, 2026-10-07: *"it should trigger on the press not the release"*.
- **The anchor is set on the up→down edge only.** A touch panel sends a *run* of down
  reports while a finger is on the glass, so a second `down` is the same finger moving, not
  a second press. Re-anchoring on it makes the anchor track the finger and fires whichever
  pad it happened to be over when it lifted. A run fires **nothing** — the press it belongs
  to already did, and a second fire per finger is the double-trigger the edge rule prevents.
  `test_hc.c` pins both halves: a run of downs answers `-1`, and a mutation that lets the
  release fire is caught by five named checks.
- **A drag across the row fires the cell it STARTED on and not the one it reached.** The
  press fires pad 1 on the way down; the report over pad 2 is a run and fires nothing; the
  lift over pad 2 fires nothing. That is why the anchor is taken from the up→down edge's own
  coordinates.
- **A gesture that begins off the grid is rbp's for its whole life**, including a slide that
  crosses the pads: it is never adopted.
- **A press this module took keeps its release wherever it lands** — including after rbp has
  left the performance screen under the finger. The screen is re-read on every report, so
  the press gate is `hc_latched() || (on the performance screen && nothing else up)`; without
  the `hc_latched()` term rbp would be handed an up for a down it never saw
  (`declined-press-must-still-see-release`).
- **Never silent.** A qualifying press logs the deck, the pad, the keycode and the channel;
  a refused one logs the deck, the pad and all three gate values. A tap that did nothing
  because the pad is empty and a tap that did nothing because a read failed are the same
  pixels, and telling them apart is the whole of what the operator would want to know.

### The switch

`POINT_HOTCUE_TOUCH` (default on) gives the row back to rbp: the zone is not fed at all and
a latch left holding a press is dropped at startup. It is in `start-rb.sh`'s `SHIM_VARS`, so
`rb.conf` can set it as `RB_POINT_HOTCUE_TOUCH`. It is **not** gated on `POINT_MENU`, for
`POINT_FX_TOUCH`'s reason: these are rbp's own pads rather than a shim surface, so an
operator who turns the band off still gets them.

### What is measured, and what is not

**The swallow was measured first, and this time the run carried a positive control that
fired.** With the shim doing nothing, `work/tap.py` writing records straight into rbp's own
pipe, all **sixteen** cells — both decks, both rows — were tapped:

| | px changed |
|---|---|
| rbp's own `◈ INFO` (logical 1211,25) — **the control** | **549,690** open, **549,254** shut |
| all 16 pad cells | **416** — the idle blinker, i.e. no response |

The 416 px is the four small red marks at x 32..725, y 605..625 that toggle on their own, so
a run showing only those has shown nothing. The control's number matches the 549,706 px
`docs/07-touch.md` already records for that tap. rbp binds nothing to the pad row, so
withholding these reports costs the operator nothing — the same evidence the BEAT FX zone
rests on, with a control that this time actually moved.

**Note for anyone re-running it:** `tap.py` writes **wire** coordinates, and rbp reflects x.
`tscfake_wire_x(x) = 1279 - x`, so a tap aimed at logical x must be fed `1279 - x`; the
control above is fed `x=68` for logical 1211. Every early pad tap in this drill landed on the
mirror of the intended point until that was noticed.

`test_hc.c` is **374 checks**, linking the production `hc_zone.c`: all eight combinations of
the three gate terms, with the dangerous direction named in the message (`AN EMPTY PAD FIRED
-- this creates a hot cue on the operator's track`); all eight keycodes against rbp's own
arithmetic; the sixteen cells tiling their two spans exactly, with no overlap and no
double-counted gutter; every cell centre mapping back to its own deck and pad, and the
gutter, the deck gap, the label row, the row gap and both margins rejecting; **the press
firing and the release NOT firing, so a tap is exactly one key**; a run of downs firing
nothing a second time; a press that began elsewhere never adopted; a drag between two pads
firing only the one it started on; a release after a slide-off not firing; a later press
re-anchoring only after a release. All 374 pass on the host and the whole suite passes
unchanged.

**Mutation-verified, seven ways**, each caught: `registered >= 0` (1 failure), a keycode off
by one (25), the "began elsewhere" guard dropped (2), a re-anchor on every down (3), the
wrong column pitch (10), **the press not answering the cell — the old release-firing build
(8, every one naming the press rule)**, and **the release firing as well as the press (5,
`the RELEASE fired a pad -- a tap must fire once, on the press` first)**.

**What no machine here settles:** the pad-number ↔ cell mapping and the polarity of the
registration read both need a **track with hot cues on it**, which is the operator's own
media on the operator's own unit. Everything in this section that could be settled without
one has been.

**AND THE MACHINE HALF IS DONE — 2026-10-07, with the finished build loaded and the FLX4
off the bus (this feature needs only the panel).** Three taps injected into the panel's own
evdev node (`work/poke.py`, which runs the whole chain — kernel → `pointsrc` → the rung),
aimed at logical (85,527), (85,558) and (1196,558):

```
pointsrc: hot cue deck1 pad1 -> nothing sent (mode 1 pad mode 0 registered 0)
pointsrc: hot cue deck1 pad5 -> nothing sent (mode 1 pad mode 0 registered 0)
pointsrc: hot cue deck2 pad8 -> nothing sent (mode 1 pad mode 0 registered 0)
```

**Four things settled at once, and the log line is what settles them.** The rung is
reachable (the line exists at all — a wrong rectangle would produce no line); the
cell→pad mapping is right in **both rows** (row 2 names pad 5, not pad 1, which is the
`row*4+col+1` offset doing its job) and **both decks**; the browse mode reads 1 (the
performance screen) and the pad mode reads 0 (HOT CUE); and **the registration gate refused
all three, with nothing sent** — no keycode on the wire and no cue on the track. The last
one is the dangerous direction demonstrated closed on the unit rather than only in a test.

**Then the mode read was proved to be LIVE rather than a constant.**
`seqinject2 --dest 128:0 note 0 30 1` — deck 1's pad mode to AUTO BEAT LOOP through the
shim's own sequencer, so the panel heard no press — moved the log's field to
`pad mode 1`, and note 27 moved it back to `0`. **A trap for the next person: the shim's
injection port is `128:0`, not `128:1`** — the port names read backwards
(`rbp-knob2-in` is the one the outside world *writes* to) and `128:1` answers
`SUBSCRIBE … Operation not permitted`, which looks like a permission problem and is not.

**The press-not-release rule was then settled at the wire, and by a run that the old build
could not have passed.** A tap logs one line whichever edge fires it, so the three taps
above do not on their own separate the two. The decisive run is a **press with no release
at all** (`work/poke.py`'s `press:` verb):

```
press:127,753   ->  pointsrc: hot cue deck1 pad5 -> nothing sent (mode 1 pad mode 0 registered 0)
release         ->  (no line)
```

The line appears **at the press**, and the `release` that follows logs **nothing** — which
is exactly the behaviour the first, release-firing build could not produce.

What was left was the **positive**: a pad that *has* a cue, firing. That needs a track
with hot cues, and loading or cueing one is the operator's own media — which is also why
this drill could not simply make one. **The operator ran it on their own cue-bearing
track, 2026-10-07, and their verdict is the feature's close: *"yep it works"*** — a lit pad
jumps its cue **on the press**, under the finger, and an empty pad does nothing and creates
nothing.

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
