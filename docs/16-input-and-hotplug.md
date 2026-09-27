# 16 — Keyboard input, hot-plug, and the FLX4 startup thump

Five observations the operator made on the unit on **2026-09-26**, and what
measuring them turned up. Each now has an answer, and for two of them the answer
was the same defect, found while measuring the others: a key release was being
thrown away, so every press/release key in the keyboard map worked once and then
went dead. That was the one worth fixing first, and it is fixed and verified.

Measuring it also turned up the *first* observation's real cause, which is a
different and much duller defect: **one variable chose one map**, and the map
that answers for the FLX4 declares no non-MIDI devices, so with the FLX4 selected
the evdev reader was never started at all and the keyboard could not have worked.
[`MIDI_MAP` and `EVDEV_MAP`](#two-selections-not-one) are now two selections, and
a controller, a keyboard and a mouse are all live with no configuration.

What is left is verification rather than work: the operator's ear on the audio
thump and on the arrow sign, and a real unplug to exercise the MIDI and audio
hot-swap paths end to end. The audio reopen ([§4](#4-hot-swap)) is the one
substantial piece of new code in here that has **not** been on the unit.

Status words are used the way the rest of these documents use them: **measured**
means it was seen on the hardware and the evidence is quoted below, **reported**
means the operator saw it and I did not reproduce it, **unverified** means
nobody has run it yet.

| # | Observation | Status |
|---|---|---|
| 1 | up/down are inverted | **fixed** — the two arrows flipped; the sign itself is the operator's observation, not a measurement |
| 2 | Enter should be a rotary push | **already was**; it was dead after one use because of the release defect, which is fixed |
| 3 | `w` = PLAY deck 1, `s` = PLAY deck 2 | **bound**, additive to SPACE/N |
| 4 | controllers should be hot-swappable | evdev: **fixed and verified** (per device, not whole-set); MIDI: verified on the unit; USB: was already; audio: **fixed in code, still unverified on the unit** ([§4](#4-hot-swap)) |
| 5 | the FLX4 pops/buzzes when rbp first opens the audio | **mute enabled by default** (1500/300) — **not yet heard with a card open**: the restarts on 08:39 and 08:41 ran with no FLX4 on the bus at all |
| — | **a key release was lost, so a second press did nothing** | **fixed and verified**: a 120 ms press now delivers its release |
| — | **the keyboard does nothing at all** | **fixed**: one variable chose one map, and under `flx4` no evdev reader was started ([below](#two-selections-not-one)) |

## The defect: the reader loses the release

`map_kbd.c` tracks a held state per binding (`b->down`, [map_kbd.c:255-261](../scripts/shims/map_kbd.c))
so that one physical press becomes exactly one rbp press and one rbp release. If
the release never arrives, `down` stays 1, and the *next* press of that key is
`ACT_IGNORE` — deliberately, because rbp already believes the key is down.

**The release arrives only if the key is held long enough.** Measured, with
`RB_KNOB_VERBOSE=1` and a synthetic keyboard:

```
hold 3000 ms:  kbd: evdev type=1 code=6 value=1 -> 0x0201 press
               kbd: evdev type=1 code=6 value=0 -> 0x0201 release   <-- delivered
hold  120 ms:  kbd: evdev type=1 code=6 value=1 -> 0x0201 press
               (no release line at all)                            <-- lost
```

A second press of the same key then logs `-> 0x0201 ignored`. The raw device is
not at fault: reading `/dev/input/event9` directly shows the kernel emitting
`value=1`, `value=0` for both holds, so the event exists and is being dropped on
the shim's side of the boundary.

**Why.** `evdev_thread`'s loop did its discovery at the *top of every
iteration*, which is the whole of it — this is the loop as it was, before the
fix two sections down:

```c
for (;;) {
    close_sources();
    if (scan_sources(pin) <= 0) { ...; continue; }
    if (poll_round() < 0) usleep(EVDEV_RESCAN_MS * 1000);
}
```

`poll_round()` returns after one round, so every round is followed by a
`close_sources()` + `scan_sources()`. The comment above `poll_round` reads as
though the rescan were the *error* path, and it is where a rescan belongs — but
the loop as written rescans after success too. evdev queues events per open
client, so a release that arrives in the closed window is discarded, and a
120 ms press is well inside it. The log confirms the churn: one keypress leaves
**8** enumeration lines (`evdev: /dev/input/...`) after it, and the file has 96
of them for 12 events.

**What it looks like to the operator.** Every press/release control in the
keyboard map works *once*, then goes dead until rbp restarts. Enter, the browse
keys, BACK, SOURCE — all of them. The arrows and the wheel are unaffected,
because they are step bindings (`b->step != 0`) and hold no state.

This is very likely the whole of observation 2. Enter is already bound as a
push; it simply stopped working after its first use.

**The fix, made and verified on the unit.** Discovery came out of the loop body:
the devices are scanned once, polled with a deadline (then `EVDEV_IDLE_MS`, 1000 ms)
instead of `-1`, and the set is re-checked only when a round times out or the poll
fails. One change for two defects — it also closes the hot-add gap in observation
4, because a device that appears while nothing else is happening was previously
never noticed at all (the loop was parked in `poll(..., -1)` and never
iterated).

> **That first fix has since been replaced, and the names above no longer exist.**
> It stopped the *churn*, which is what the lost release was, but it left three
> other ways to lose or delay an event — a rescan still closed every live device, a
> 500 ms blind window sat before each reopen, and the 1000 ms deadline re-armed
> every round so a chatty mouse postponed it without bound. The reader was rewritten
> around **no live fd is ever closed on a rescan** and **one absolute deadline**;
> `EVDEV_IDLE_MS` and `unseen_source()` are gone, and the current loop is described
> in [§4](#4-hot-swap). The section above is kept as the record of how the defect
> was found, not as a description of the code.

Verified on the unit, same synthetic keyboard, same 120 ms hold that used to be
lost:

```
hold  120 ms:  kbd: evdev type=1 code=6 value=1 -> 0x0201 press
               kbd: evdev type=1 code=6 value=0 -> 0x0201 release   <-- now delivered
```

Enter pressed twice in a row now logs `press`, `release`, `press`, `release`,
where the second press used to log `ignored`. The only `ignored` lines left are
step-key releases, which are `ACT_IGNORE` by design. The churn is gone with it:
enumeration lines fell from 96 to 20 for the same session.

## 1. Up and down are inverted

`map_kbd.c` sent `KEY_UP` as `+1` and `KEY_DOWN` as `-1` on `K_SELECTOR`
(0x420c), with the wheel at `+1` ([map_kbd.c:174-177](../scripts/shims/map_kbd.c)).
The sign has never been measured: the table's own comment says so
("the sign against rbp's list has never been observed"), and the operator's
report is the first evidence either way.

I could not reproduce it, and the attempt is worth recording because it says
which *screen* the sign can be read on. Both arrows arrive correctly
(`code=103/108 -> 0x420c rotate`), but on the SOURCE panel the frame is
**byte-identical** after each press — `changed_px=0`. With one USB attached
there is nothing for the selector to move between, so that panel cannot show
the sign at all. The operator's observation must therefore come from a screen
with a list in it.

**Flipped anyway**, on the operator's report, and recorded as their result
rather than a measurement: up is now `-1` and down `+1`. The unit has not been
checked on a screen with a list in it, so the flip rests entirely on the report
that arrived with the observation. It is one line each way if it turns out
wrong.

Note the wheel: it is `+1`, the same as `KEY_DOWN` now, and it was *not*
reported as inverted. Leaving it alone is what makes the pair agree — rolling
the wheel away from the user moves down the list, which is the natural
correspondence. Flipping the wheel with the arrows would have broken it.

## 2. Enter as a rotary push

Enter is already `K_SELECTOR` with `step == 0`
([map_kbd.c:176](../scripts/shims/map_kbd.c)), which is `OP_PRESS`/`OP_RELEASE`
on 0x420c — the same keycode the FLX4's browse-knob push sends
(`map_flx4.c` note 65, `add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL)`).
So the binding was not missing; it was the release defect above, and with that
fixed a second Enter press works. Verified: two consecutive Enters both log
`press`.

One thing that is **not** explained by the defect, and is still open: a
*first*, live Enter press on the SOURCE panel also produced a frame identical to
the one before it. Either the SOURCE panel does not consume the selector push,
or the push's effect is not visible there. Both are worth knowing before
declaring this closed.

## 3. `w` and `s` for PLAY

Bound on 2026-09-26
([map_kbd.c:114-118](../scripts/shims/map_kbd.c)), additive to SPACE and N,
which are unchanged:

```c
{ EV_KEY, KEY_W, K_PLAY, 1, 0, 0 },
{ EV_KEY, KEY_S, K_PLAY, 2, 0, 0 },
```

`K_PLAY` with the deck in the channel and not `CH_GLOBAL`, copying the
space/n idiom. The cross-check that the keycodes are right is the test
fixture: `test_kbd` pins `KEY_W` (17) and `KEY_S` (31) independently of the
`#define`s in the map, so a transcribed-wrong number is a failing expectation
rather than a dead key. `test_kbd` is at **360 checks, 0 failures**.

Not yet pressed by a human on the unit — it is the same `K_PLAY` path space and
n already use, so the risk is in the keycode, and the keycode is pinned.

## 4. Hot-swap

Five paths now, and the answers differ enough that they are worth keeping apart —
especially the *two* mouse paths, which are different code with the same device
under them.

**evdev (keyboard, mouse buttons, wheel) — rewritten, fixed, verified.** A hot
*add* used to be noticed only if some other event happened to arrive afterwards:
the loop rescanned on every iteration, but it was parked in `poll(..., -1)` when
nothing was happening, so there was no iteration to rescan in. That was one of
the four defects in the release bug above and it is the same loop, so it went with
the same rewrite.

Three things changed beyond "it works now", and each was a way for the reader to
drop something:

* **A rescan no longer closes a live device.** It used to close *every* fd and
  reopen the set, which is what discarded the queued release (evdev queues per
  open client). Now only a device that has actually failed is dropped, and only
  its own fd is closed; the others keep their queues and are drained in the same
  round.
* **The deadline is absolute.** `EVDEV_UNSEEN_MS` (1000 ms) is a wall-clock
  instant, not a timeout re-armed per round, so a device plugged in is noticed
  within a second *regardless of how much traffic is on the other devices*. With
  a 1000 Hz mouse the old per-round re-arm postponed the rescan without bound —
  the old code only ever delivered on this promise when the bus was quiet.
* **Losing a device releases the keys it was holding.** A device that goes away
  with a key down would otherwise leave the map's `b->down` latched (the same
  latch as the release defect), so the *next* press of that key would be ignored —
  a wired keyboard unplugged mid-press would look like a dead key after replug.
  The reader emits `(EV_KEY, code, 0)` for each held key before it closes the fd,
  which reuses the map's own tested release path and adds no interface.

Verified on the unit by creating a virtual keyboard while rbp was already
running. The reader's own line now carries the millisecond it happened at, so
this is a duration rather than "within a second":

```
knobshim2: evdev[41302] /dev/input/event11 'rblive4-vkeyd' appeared; adding it
```

Before the fix the device was never seen and every test had to begin with
`systemctl restart rblive4`.

**The mouse pointer is a separate path and was never broken.** `fbshim.so`'s
`pointsrc.c` holds one absolute device, rescans when that one dies, and clears its
touch state — but it is one device at a time and is unaffected by what the evdev
reader does. Do not read a fix in one as a fix in the other: the reader is what
carries the mouse's *buttons* and *wheel* to the keyboard map, and the pointer is
painted by the framebuffer shim. They are unrelated code that happen to open the
same node (both may; evdev gives every reader every event).

**MIDI (the FLX4) — already implemented, now verified.** `midi_connect_try()`
runs once a second and detects the surface going away, logging "control surface
'%s' disappeared (unplugged?); waiting for it to come back" and re-subscribing on
a new client number when it returns
([midi_io.c:200-280](../scripts/shims/midi_io.c)). Its own comment says the call
is cheap and idempotent, which is what lets the read loop call it forever.
**Verified on the unit with a real unplug**, and the one predicted defect did not
appear: ALSA recycles client ids from a low free id, so a re-enumerated FLX4 could
have landed on the same number and been left subscribed to a dead client — but the
measured re-enumeration gap is about 5 s and the walk runs every 1 s, so the
address had always changed. The test is a press on a pad afterwards, because
`find_surface` succeeding is not the same question as a pad working.

**USB media — was already fine.** `usb-watch.sh` polls every 2 s, so the latency
is that poll and the recovery is `USB1 detected -> registered (dev=3)`. Nothing
here changed; it is listed so the five paths are all accounted for.

**audio — measured 2026-09-26 as "does NOT come back"; now fixed in code, not yet
verified on the unit.** This is the largest thing in this document and the one
piece here that has not been on the hardware, so read the whole entry before
believing it.

The measurement first, because it is what shapes the fix. `audioshim.c` had a
prepare-retry after a failed write ([audioshim.c:1296](../scripts/shims/audioshim.c))
but no device-loss or reopen handling, and the unit demonstrated exactly what that
costs:

```
09:24:43  usb 1-1.2: USB disconnect, device number 3
09:24:48  usb 1-1.2: new high-speed USB device number 7 ... Product: DDJ-FLX4
```

The FLX4 dropped off the bus and re-enumerated five seconds later — the
under-voltage symptom this target already has ([13](13-raspberrypi4.md) S6), not
a bad connector. The card was back and `cat /proc/asound/cards` listed it, but the
PCM rbp held had been opened against the *old* device, so from the moment of the
drop every master write returned `-19`:

```
writei #175001 frames=64 bytes=768 written=-19 peak_m=1763519 mainvol=1.000
...                        <- and every write after it, ~5.1 million of them
```

Two things make this worth reading carefully rather than skimming:

* **`peak_m` stays non-zero.** The peak is measured from rbp's own buffer, so it
  goes on showing music while the card hears nothing. A healthy-looking audio log
  is therefore doubly misleading: zero peaks mean rbp is silent (the fader
  default, [09](09-audio.md#audibility-rbp-builds-the-channel-faders-at-zero)),
  and non-zero peaks do **not** mean the card is being written to.
* **The write count is the tell, not the peak.** `written=64` means the card took
  the block; `written=-19` means the handle is stale. This is the line to read
  first, and the log's own `open('hw:CARD=DDJFLX4,DEV=0') ... res=-19` is a
  different case again — that one is the card being absent at startup.

**The fix, and why it is shaped the way it is.** `snd_pcm_open` hands rbp the
real `snd_pcm_t *` itself, and `is_master`/`is_real` are pointer *equality*
against the shim's global — there is no shim-owned wrapper to swap underneath
rbp. So the reopen needs a **retired-handle set**, and that turns out to be the
smaller fix rather than a compromise: a stable token would have to be unwrapped
again at the ~13 sites that deliberately forward the *caller's own pointer*
(`is_forwardable`'s doctrine, which exists because gating on "is this the master"
alone once left the hw slave unconfigured and produced `written=-22` with nothing
in the log). The retired set is three predicate edits and **zero** call-site
edits:

* `is_master()` also accepts a retired handle, so a stale write still routes into
  the one place that writes through the *global* and needs no unwrapping;
* `is_real()` stays "the live master", because it gates the calls
  (`reset_stream_state`, `sw_params`, `prepare`, the channel negotiation) that
  must act on a live handle only;
* `is_forwardable()` **excludes** a retired handle — this is the one that matters,
  since otherwise a dead pointer reaches those thirteen sites and is used as a
  live `snd_pcm_t`.

In the write path, `-ENODEV` now calls `master_lost()`, which clears the global
before closing (so a re-entrant inner call cannot mistake itself for the master),
retires the pointer, closes it, records the absence, and logs **one** line:

```
audioshim: MASTER LOST: the card went away mid-stream (writei said No such device,
after 175001 write(s) on that handle). Audio is silent until it is back; this shim
is now looking for it. The stream map is kept
```

The stream map is deliberately *not* reset: `g_cfg.*` has already been clamped
against the real channel count, and clearing the channel count would silently
change what the pair indices mean. Recovery happens from the write path itself
under a 500 ms → 5 s backoff (no new thread; rbp calls it continuously), and the
recovered handle has the whole negotiation **replayed** through the shim's own
interposed setters — the format, the negotiated channel count and the rate, which
are exactly what the pair indices were resolved against:

```
audioshim: MASTER RECOVERED: reopened hw:CARD=DDJFLX4,DEV=0. Replaying the
negotiation on the new handle
audioshim: replay: access=3 channels=4 rate=44100 period=... periods=... ...
audioshim: the pair map is unchanged: master=0,1 headphones=2,3 ...
audioshim: startup mute released after 79424 frames
```

The line shapes above are from the source; **the values are not a capture** —
this sequence has not been run on the unit yet. The values shown are the ones
[09](09-audio.md) records from the working negotiation, which is what a correct
replay should reproduce exactly.

The last line is the startup mute being re-armed, because the card thumps on a
reopen exactly as it does on an open. It costs 1.8 s of silence per replug, which
is a choice against a pop the operator has heard. The recovery line preceding it
is what makes it unambiguous, where before it could have been the silent path
([§5](#5-the-pop-when-rbp-first-opens-the-flx4s-audio)).

**The cold case is the same mechanism**, which is the pleasant part: a unit booted
with no FLX4 already had `g_real_playback == NULL` and ran the no-device path, so
being *absent* at startup and being *lost* mid-session are the same state — only
the return was missing. One honest limit: in the cold case `resolve_pairs()` had
already latched the 2-channel fallback and dropped the headphones and booth pairs,
so plugging in a controller after boot recovers **master-pair audio only**, and
the recovery line says so and says to restart.

**Still unverified on the unit, and one hole that is deliberately not fixed.**
The on-unit sequence is in [13](13-raspberrypi4.md) S8.6: pull the FLX4 mid-play,
expect `MASTER LOST` **once** rather than per block, then on replug
`MASTER RECOVERED` and `written=64` with a non-zero `peak_m`. Until that has been
seen, this is code that passes its tests and has never met a card. Separately,
`snd_ctl_open` hands rbp a real `snd_ctl_t` for the card, which is just as stale;
nothing recovers it, and `snd_ctl_pcm_info` never forwards, so if rbp re-probes
the control interface it may learn nothing. A `grep -c snd_ctl_open` after a
replug settles whether it is inert — if the count does not grow, rbp never asks
again and the hole does not matter.

## 5. The pop when rbp first opens the FLX4's audio

The machinery exists and is exact: every output channel is held at zero for
`STARTUP_MUTE_MS` after the first write and then faded in over
`STARTUP_FADE_MS` ([audioshim.c:1176-1264](../scripts/shims/audioshim.c),
`mute_frames`/`fade_frames` at 191-192, the env reads at 549-550). It shipped
**disabled**:

```sh
: "${RB_STARTUP_MUTE_MS:=0}"     # scripts/device/rb.conf, before 2026-09-26
: "${RB_STARTUP_FADE_MS:=0}"     # scripts/device/rb.conf, before 2026-09-26
```

and the comment above those lines asserted that "USB audio has no codec power-up
transient, so unlike the SC Live 4 there is nothing to mute over" — which the
operator has now found to be false for this card. The comment was the thing that
was wrong; the code was fine.

**Now enabled by default**, because the card is the card the port is aimed at:
`RB_STARTUP_MUTE_MS=1500` and `RB_STARTUP_FADE_MS=300`
([rb.conf:201-202](../scripts/device/rb.conf)) — the values
[09 — Audio](09-audio.md) already gave, with the false comment replaced by one
that records the thump. The cost when a card does not need it is 1.5 s of
silence at startup; a card that is *late* rather than noisy wants them back at 0
in `rb.local.conf`.

The evidence that the values are in force is arithmetic rather than audible —
the shim's own two lines:

```
audioshim: config ... mute=1500ms fade=300ms
audioshim: startup mute released after 79424 frames
```

79424 frames at 44100 Hz is 1.8 s, which is 1500 ms + 300 ms. What it *sounds*
like is the one thing left, and it needs the operator's ears.

**And it is still unverified, because of a trap in those two lines.** The
mute-release line is printed by `flush_master()`, which the silent path also
runs, so it appears on a run with no audio at all. Checked on the unit later the
same day: **both restarts were silent** — the FLX4 was not on the USB bus, the
shim logged `NO OUTPUT DEVICE` with `open('hw:CARD=DDJFLX4,DEV=0') res=-19`
(-ENODEV), and there were **zero** `writei #` lines, which is the only line that
means a real device is being fed ([09 — Audio](09-audio.md) has the recipe for
telling the two apart).

So "no pop" on a restart is not evidence of anything until `writei #` is
appearing. Listen for the pop with the FLX4 connected and the card open.

## Reaching the unit's input path from a workstation

The measurements above need a keyboard whose keycodes and hold times can be
chosen, because the real keyboard can only be pressed by a human at human speed
— which is precisely the speed at which the release is lost. `tools/pi-bringup/`
has the four scripts this session used: a virtual keyboard driven through a FIFO
(`vkeyd.py`), a reader that shows what the kernel really emits (`rawread.py`), a
probe that watches rbp's own state words (`stateprobe.sh`) and a harness that
drives a key sequence and captures the framebuffer after each one
(`drive-keys.sh`). See its README.

Two traps that cost time here, both worth knowing before writing another one:

* **The virtual device had to exist before rbp started**, because a device
  created afterwards was never enumerated — so every test began with
  `systemctl restart rblive4`. This was the hot-add gap in observation 4 and it
  is now fixed, so the restart is no longer needed; the entry is kept because it
  is what made the gap visible, and because a test harness that *needs* a
  restart is evidence of a gap rather than a property of the harness.
* **Do not put a `pkill -f`/`pgrep -f` pattern in the same command as the plain
  name it is meant to match.** Three separate times in this session a pattern
  like `[v]keyd.py` matched the *command string itself* — because the same
  command line also contained the plain path, for the `cat` that was writing it
  — and killed the session. Split the kill into its own invocation, or match on
  a pid. This is the same trap `docs/13-raspberrypi4.md` records for
  `pkill -f "/root/pdj/rbp"`.
* **And it is not only your own `pkill` that does the matching — the launcher
  does too, on every restart.** A fourth occurrence, 2026-09-27: an SSH one-liner
  whose text happened to contain the player's path, and which ended in
  `systemctl restart rblive4`, was killed **mid-script** when the launcher's
  `cleanup()` ran `pids_matching` over the real command lines and found the
  session's own `bash -c` string. The tell is a bare `Exit code 255` with no
  output after the last command that printed. So the rule covers any command
  that *restarts the unit* as well as one that kills a process: if the command
  text names the player, the chroot, or a watcher, expect the restart to take
  your shell with it — put the restart last, or write the path split
  (`/root/pdj/r''bp`) so the argv bytes never contain it.

**Writing *into* a device, not reading from it.** `vkeyd.py` creates its own
input device, which is right for keys. For the *pointer* the panel's own node can
be written to instead: `work/poke.py` writes `struct input_event`s to
`/dev/input/event3` and the kernel's injection path feeds them to every client of
that device, so pointsrc's reader receives them and the whole chain runs —
kernel → pointsrc → `point_xform_abs()` → `tscfake_emit()` → rbp — rather than
only its last hop. That is what makes a shim-side change measurable without a
finger, and it is how the x reflection's fix was proven: the *same* injection
against the old and the new `fbshim.so`, on the same screen, one selecting the
right pane and the other the sidebar cell under the finger (S3.3 in
[13](13-raspberrypi4.md#s32-pointing), and [07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it)).
It pairs with `work/tap.py`, which writes into the record pipe *downstream* of
the shim: tap.py is the instrument for "what does rbp do with an x", poke.py for
"what does the shim write for a finger".

Three things about writing into an evdev node cost time here:

* **A position the device already holds is silently dropped.** The kernel ignores
  an ABS write whose value equals the current one, so a `touch:` at a point a
  previous run used injects a tap carrying **no position at all** — rbp then acts
  wherever the last real finger left the pointer, which reads exactly like "the
  tap did nothing". Measured 2026-09-27: an injection at raw `(75,209)` reached
  the shim as `raw=(0,0)` while the device's own state read `(75,209)`. `poke.py`
  grew `move:` for it (a position with no touch, to prime the value), and it
  prints back every event it read on the same fd — so an absent `MT_X`/`ABS_X`
  line means *the position was dropped*, not that rbp ignored a touch. **Read the
  echo, not the screen alone.**
* The per-event filter is per **device**, not per client, which is why that echo
  is a valid self-check even though a client's read queue is private. The struct
  is 16 bytes on this unit (two 32-bit timeval fields + u16 type + u16 code + s32
  value) — what a 32-bit reader gets from the 64-bit kernel's compat path, the
  same layout `pointsrc.c` declares for itself.
* `work/upsnap.py` takes the before/after framebuffer snapshots for a run driven
  this way, into `work/unit/` where `uimap.py` and `fbcompose.py` already read
  `tap.py`'s frames — so both instruments are read by the same tools and their
  results are comparable.

## What is temporary on the unit

The measurements — and the verification of the fixes — were taken with overrides
in `/opt/rblive4/rb.local.conf`, all marked TEMPORARY there and none of them a
fix. As of 2026-09-27 the file holds:

* `RB_MIDI_MAP=flx4` — **this one is now the default in `rb.conf` and the line is
  redundant**; it is kept only so the file records what changed and why. It was
  `kbd` for the earlier bring-up, and under the one-map rule that alone is what
  made the FLX4 look dead ([below](#two-selections-not-one)).
* `RB_KNOB_VERBOSE=1` — to see what the map receives. **Note what this now costs,
  because it changed:** the per-event trace still prints a line per mapped key,
  but an *unmapped* evdev event is rate-limited to one line per `(type, code)` per
  second, because a mouse's motion is unmapped by design and arrives thousands of
  times a second. Before that limit, leaving verbose on with a mouse attached
  filled the input thread's time with logging — which is the operator's "the keys
  arrive late" — so verbose is now cheap enough to leave on while working, though
  it still writes a line per keypress and is not what to measure latency under.
* `RB_MIDI_DUMP=/tmp/flx4.dump` — records every sequencer event the shim
  receives, so a control whose note number is only *published* is measured rather
  than assumed. Its one open question is note 66, the SHIFT + browse push
  ([15](15-flx4-midi.md)); a real press from the panel settles it, and the dump
  is what makes that a one-press answer instead of an argument.
* `RB_POINT_DEBUG=1` — **removed again on 2026-09-27, after the pass it was kept
  for**, so expect to re-add it rather than find it on. It is the pointer path's
  per-event line, which since this date prints **both ends of the transform**
  (`logical=` where the finger is, `wire=` what rbp consumes; they differ by
  `1279 - x` by design,
  [07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it)). It was turned on
  to find the reflection, and kept because the log is the only instrument that
  reads a *synthetic* touch and a real one the same way — which is exactly what
  the operator's finger pass on S3.3 needed, and it **passed**.

The dump's first use is also its clearest: it distinguishes **whose** event a
line is. `seqinject2` sends velocity **100** and the FLX4 sends **127**, so
`grep 'vel=127'` is the panel and `grep 'vel=100'` is the harness. Without that,
an injected press and a real one are the same line in the same file.

`rb.local.conf` is on the unit only and is not a tracked file, which is the point
of it. The fixes themselves are four tracked files —
`scripts/shims/evdev_io.c`, `scripts/shims/map_kbd.c`, `scripts/shims/audioshim.c`
and the constants in `scripts/device/rb.conf` — plus their tests and the docs.

### Two selections, not one

This replaces a section that said the opposite, and the correction matters because
the old text described a *configuration* fact that is now gone.

**What it used to be.** `MIDI_MAP` chose exactly one map, and `ctrlshim.c` started
the evdev reader only when that map declared non-MIDI devices. The FLX4's map does
not — a controller has no `/dev/input/event*` — so with `MIDI_MAP=flx4` **no evdev
reader was started at all**: the keyboard was not partially working, it was not
being read. `MIDI_MAP=kbd` was the same fact from the other side, and that is the
whole of "the keyboard does nothing while the controller works".

The old text also claimed that under `kbd` the FLX4 got no subscription. That was
wrong on both counts: `seq_setup()` runs unconditionally, so the FLX4 *was* found
and subscribed under `kbd` — it only looked ignored because the LED/meter output
route was disabled (`RB_LED_DISABLE=1` then; it is `0` now that the note numbers
live in the map rather than in `rbp_led.c`) and the keyboard map has no `event()`
to consume what was arriving.

**What it is now.** Two independent selections, one per event source:

| Variable | Chooses | Default |
|---|---|---|
| `RB_MIDI_MAP` / `MIDI_MAP` | the controller, on the ALSA sequencer: `flx4`, `jp21`, `kbd`, `none` | `flx4` |
| `RB_EVDEV_MAP` / `EVDEV_MAP` | the non-MIDI surface, on `/dev/input/event*`: `kbd`, `none` | `kbd` |

So a controller, a keyboard and a mouse are all live with no configuration, which
is what the operator asked for. Both knobs are ordinary: `EVDEV_MAP=none` is how
to ask for the old "controller only" behaviour without a rebuild, and `MIDI_MAP=kbd`
still means what it always did — no controller, keyboard only — because `kbd` is
in *both* tables and is then built once, not twice. The startup line says which is
which, and `EVDEV_MAP` has to be listed in `start-rb.sh`'s `SHIM_VARS` for
`RB_EVDEV_MAP` in `rb.conf` to reach the shim at all.

Three consequences worth knowing, all of them in the code rather than in a note
here:

* **A build is additive now.** The shared binding tables are cleared once by the
  front end before either map is built, and no map resets them itself. That is not
  tidiness: `kbd_build()` deliberately adds nothing, so a reset inside it would
  wipe the FLX4's bindings — a working keyboard would silently kill every button on
  the controller. `test_kbd` pins this directly now ("a second build must add, not
  wipe").
* **The evdev reader belongs to `EVDEV_MAP`, never to `MIDI_MAP`.** That is the
  fix, stated as a rule.
* **Two surfaces can drive the same rbp key.** The keyboard and the FLX4 map
  overlap on PLAY, CUE, SYNC, LOAD, SOURCE, BACK and the selector, each with its
  own held state. Holding the FLX4's PLAY and tapping SPACE therefore delivers two
  presses and one release, and whether rbp's KeyManager acts on the redundant press
  is not knowable from the source. **The operator ran it on 2026-09-26 and reported
  playback undisturbed — a pass, so no aggregator was built and none is owed**
  ([13](13-raspberrypi4.md) S8.5). One thing that pass does not carry is a log: the
  same reboot wiped `/tmp`, and no surviving window contains a `KEY_SPACE` event at
  all, so the result rests on the operator's report rather than on a record. The
  corroboration, if it is ever wanted, is one press of `space` with the log live.
  Absent that, the aggregator stays **unbuilt and unruled-out** — and it must not be
  built speculatively.

**Nothing here was fixed by the `kbd` map in the end.** The gap it was working
around — no SOURCE binding — is closed on `flx4` now, by two bindings of its own:
SHIFT + browse push is `K_SOURCE`, and CUE/LOOP CALL ◁ is `K_BACK`
([15](15-flx4-midi.md)). `kbd` has since found its own reason to exist — it is the
only map that reads a keyboard and a mouse — but that is what `EVDEV_MAP=kbd` now
says, on its own variable, without displacing the controller.
