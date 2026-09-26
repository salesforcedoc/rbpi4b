# 16 — Keyboard input, hot-plug, and the FLX4 startup thump

Five observations the operator made on the unit on **2026-09-26**, and what
measuring them turned up. Each now has an answer, and for two of them the answer
was the same defect, found while measuring the others: a key release was being
thrown away, so every press/release key in the keyboard map worked once and then
went dead. That was the one worth fixing first, and it is fixed and verified.

What is left is verification rather than work: the operator's ear on the audio
thump and on the arrow sign, and a real unplug to exercise the MIDI and audio
hot-swap paths end to end.

Status words are used the way the rest of these documents use them: **measured**
means it was seen on the hardware and the evidence is quoted below, **reported**
means the operator saw it and I did not reproduce it, **unverified** means
nobody has run it yet.

| # | Observation | Status |
|---|---|---|
| 1 | up/down are inverted | **fixed** — the two arrows flipped; the sign itself is the operator's observation, not a measurement |
| 2 | Enter should be a rotary push | **already was**; it was dead after one use because of the release defect, which is fixed |
| 3 | `w` = PLAY deck 1, `s` = PLAY deck 2 | **bound**, additive to SPACE/N |
| 4 | controllers should be hot-swappable | evdev: **fixed** (hot-add verified on the unit); MIDI: already worked; audio: **still unverified** |
| 5 | the FLX4 pops/buzzes when rbp first opens the audio | **mute enabled by default** (1500/300) — needs the operator's ear to confirm |
| — | **a key release was lost, so a second press did nothing** | **fixed and verified**: a 120 ms press now delivers its release |

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

**The fix, made and verified.** Discovery came out of the loop body: the devices
are scanned once, polled with a deadline (`EVDEV_IDLE_MS`, 1000 ms) instead of
`-1`, and the set is re-checked only when a round times out or the poll fails.
One change for two defects — it also closes the hot-add gap in observation 4,
because a device that appears while nothing else is happening was previously
never noticed at all (the loop was parked in `poll(..., -1)` and never
iterated).

`poll_round()` now returns `1` for the deadline and `0` for "read something", so
the caller can tell the two apart; `unseen_source()` is what a deadline runs —
it looks for an `/dev/input/event*` not already open, and returns true the
moment one can be opened, which is the rescan trigger.

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

Three separate paths, three different answers.

**MIDI — already implemented.** `midi_connect_try()` runs once a second and
detects the surface going away, logging "control surface '%s' disappeared
(unplugged?); waiting for it to come back" and re-subscribing on a new client
number when it returns ([midi_io.c:200-280](../scripts/shims/midi_io.c)). Its
own comment says the call is cheap and idempotent, which is what lets the read
loop call it forever. **Not yet verified on the unit with an actual unplug** —
that is a 30-second test and should be done before telling anyone it works.

**evdev (keyboard, mouse) — fixed and verified.** A hot *add* used to be
noticed only if some other event happened to arrive afterwards: the loop
rescanned on every iteration, but it was parked in `poll(..., -1)` when nothing
was happening, so there was no iteration to rescan in. This was the same loop as
the release defect, and the same fix closed it.

Verified on the unit by creating a virtual keyboard while rbp was already
running:

```
knobshim2: evdev: /dev/input/event11 'rblive4-vkeyd' appeared; rescanning
```

The new device was enumerated, and a press on it reached rbp (state probe:
`mode=12 dev=3`). Before the fix the device was never seen and every test had to
begin with `systemctl restart rblive4`.

Also unchanged and still worth knowing: a device is opened **once** and left
open. That is what `unseen_source()` checks against — a path already in the open
set is not a reason to rescan.

**audio — unverified.** `audioshim.c` has a prepare-retry after a failed write
([audioshim.c:1296](../scripts/shims/audioshim.c)) but no device-loss or reopen
handling, so a PCM opened against a card that is unplugged has no obvious way
back. Whether rbp survives the FLX4 being unplugged mid-track, and whether
audio returns on replug, has not been tested. Nothing was changed here — this
one is still open, and is the least likely of the three to survive a replug.

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

## What is temporary on the unit

The measurements — and the verification of the fixes — were taken with two
bring-up overrides in `/opt/rblive4/rb.local.conf`, both marked TEMPORARY there
and neither of them a fix:

* `RB_MIDI_MAP=kbd` — because `map_flx4.c` binds no `K_SOURCE`/`K_BROWSE`, so
  the controller alone cannot open the Source list
  ([docs/13](13-raspberrypi4.md), the S6 row);
* `RB_KNOB_VERBOSE=1` — to see what the map receives.

Worth being explicit about what that means: the fixes above were verified with
**the FLX4 ignored entirely** and the keyboard driving everything, so what is
verified is the keyboard map and the reader, not the controller path. They
should go once `map_flx4.c` binds `K_SOURCE`, which needs note 66 measured with
a MIDI dump first.

`rb.local.conf` is on the unit only and is not a tracked file, which is the
point of it. The fixes themselves are three tracked files —
`scripts/shims/evdev_io.c`, `scripts/shims/map_kbd.c` and the two constants in
`scripts/device/rb.conf` — plus their tests and the docs.
