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
record of the taller bar). Measured off the glass with the panel open (2026-10-01):
the six dividers between them are the only `MENU_DIV` pixels in the band — 6 runs of
39 rows = 234 px, which is the column count read straight off the framebuffer — each
label's ink is 11 rows at rows 23..33, and the widths are `SOURCE` 58, `BROWSE` 57,
`TAG LIST` 59, `PLAYLIST` 57, `SEARCH` 56, `MENU` 40, **`USB STOP` 65**. The widest
label therefore sits in a 182 px column with 117 px of margin, and the seventh costs
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

**The labels are the device's own typeface**, and that is the one thing on the panel
that is not a single palette value. They were a hand-written 5×7 bitmap drawn at an
integer scale, which on a 96-row button meant scale 4, i.e. every source pixel
replicated as a 4×4 block; the operator's verdict on the glass was *"the font is too
big and pixely"*. They are now Decker Bold — the face the XDJ-RX3 carries in its own
font directory (`extracted/XDJRX3/gui/fontdata/decker.ttf`) and the one rbp draws its
own UI in — rasterised **once**, on a build machine, into an 8-bit coverage atlas:
`scripts/shims/bake_menu_font.py` writes `scripts/shims/menu_font.h`, which is
committed, and the shim itself still has no font engine, no freetype and no file I/O.
Coverage is where the exactness rule lives: **coverage 0 writes exactly the button
colour and coverage 255 writes exactly the label colour** — the ends of the range are
palette values — and only the glyph edges between them are a per-channel blend of the
label over the button. The witness's 15 sample points are chosen to be glyph-free:
they are the **button band's own edges, one row in** (`menu_paint.c`'s
`menu_witness_point()`), which at 1280×800 with the 56-row panel is the ink at rows
23..33 and the sample rows 9 and 46 — 14 rows clear above the ink, 13 below. The
rule they replace was 5/16 and 11/16 *of the panel*, i.e. rows 34 and 76 at 112 rows
and 17 and 37 at 56 — arithmetic that happens to clear a fixed-height font at one
panel size and does not at another, because a proportional sample row cannot follow a
fixed-size line box. So the points now come from the band, and `menu_view_ok()`
**refuses** a panel whose band cannot host the label's 20-row line box (which is how
480×320, whose band is **16 rows**, 3..18, is refused rather than drawn — a refusal
that logs "leaves no room for the panel" and draws nothing). `test_menu.c` asserts both against the
production point list and across ten panel sizes rather than trusting the arithmetic,
because a witness that sampled a blend would read "damaged" on every tick and repaint
for ever.

At the default **16 px** the ink is 11 rows in a 20-row line box, and the labels come
out 56..59 px wide in a 213 px column at 1280×800 — measured off a host render of the
panel, not estimated. There is no scale parameter left anywhere in the interface: one
baked size, centred on the column (`menu_text_width()` over the atlas's own advances)
and clipped to it, so a panel too small to hold a label truncates it rather than
spilling it into its neighbour. `test_menu.c` pins the ink band, the advances and the
fit; `bake_menu_font.py` is one command to change the size (`… 18` is the size whose
13 rows match rbp's own top-bar text, and 16 px is two rows under it — the operator
picked 16 against that ladder and it is their eye that settles it).

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
11 rows**, unchanged, because the font is a fixed 20-row line box and does not follow
the panel's proportions; that is what the witness's sample rows had to be re-derived
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
above still holding.

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
column to an Enter. That is real work and it invents a UI rbp does not have, so it is
recorded and not started.

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
