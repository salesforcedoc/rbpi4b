# 16 — Keyboard input, hot-plug, and the FLX4 startup thump

Five observations the operator made on the unit on **2026-09-26**, and what
measuring them turned up. Four of the five are still open; the fifth is a defect
found *while* measuring the others, and it is the one to fix first because it
explains two of the observations on its own.

Status words are used the way the rest of these documents use them: **measured**
means it was seen on the hardware and the evidence is quoted below, **reported**
means the operator saw it and I did not reproduce it, **unverified** means
nobody has run it yet.

| # | Observation | Status |
|---|---|---|
| 1 | up/down are inverted | reported; the sign is still unmeasured (see below) |
| 2 | Enter should be a rotary push | **already is**; what was actually wrong is the release defect |
| 3 | `w` = PLAY deck 1, `s` = PLAY deck 2 | not bound yet; both keycodes are free |
| 4 | controllers should be hot-swappable | MIDI: **already works**. evdev: gap. audio: unverified |
| 5 | the FLX4 pops/buzzes when rbp first opens the audio | cause identified: the mute that fixes it ships disabled |
| — | **a key release is lost, so a second press does nothing** | measured; fix identified, not yet made |

## The defect: the reader loses the release

`map_kbd.c` tracks a held state per binding (`b->down`, [map_kbd.c:236-242](../scripts/shims/map_kbd.c))
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

**Why.** `evdev_thread`'s loop does its discovery at the *top of every
iteration* ([evdev_io.c:251-265](../scripts/shims/evdev_io.c)):

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

**The fix** (not yet made) is to take discovery out of the loop body: scan once,
poll with a **timeout** instead of `-1`, and rescan only when a timeout says the
device set may have changed or when `poll_round()` reports a failure. That is
one change for two defects — it also closes the hot-add gap in observation 4,
because a device that appears while nothing else is happening is currently never
noticed at all (the loop is parked in `poll(..., -1)` and never iterates).

## 1. Up and down are inverted

`map_kbd.c` sends `KEY_UP` as `+1` and `KEY_DOWN` as `-1` on `K_SELECTOR`
(0x420c), with the wheel at `+1` ([map_kbd.c:155-158](../scripts/shims/map_kbd.c)).
The sign has never been measured: the table's own comment says so
("the sign against rbp's list has never been observed"), and the operator's
report is the first evidence either way.

I could not reproduce it, and the attempt is worth recording because it says
which *screen* the sign can be read on. Both arrows arrive correctly
(`code=103/108 -> 0x420c rotate`), but on the SOURCE panel the frame is
**byte-identical** after each press — `changed_px=0`. With one USB attached
there is nothing for the selector to move between, so that panel cannot show
the sign at all. The operator's observation must therefore come from a screen
with a list in it; the fix is to flip the two arrows and confirm there.

Note the wheel: it is `+1`, the same as `KEY_UP`, and it was *not* reported as
inverted. After flipping the arrows, `KEY_DOWN` and the wheel agree (`+1`), and
up is `-1` — rolling the wheel away from the user moves down the list, which is
the natural correspondence. Flip the arrows and leave the wheel alone.

## 2. Enter as a rotary push

Enter is already `K_SELECTOR` with `step == 0`
([map_kbd.c:157](../scripts/shims/map_kbd.c)), which is `OP_PRESS`/`OP_RELEASE`
on 0x420c — the same keycode the FLX4's browse-knob push sends
(`map_flx4.c` note 65, `add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL)`).
So the binding is not missing; it is the release defect above.

One thing that is **not** explained by the defect, and is still open: a
*first*, live Enter press on the SOURCE panel also produced a frame identical to
the one before it. Either the SOURCE panel does not consume the selector push,
or the push's effect is not visible there. Both are worth knowing before
declaring this closed.

## 3. `w` and `s` for PLAY

`KEY_W` (17) and `KEY_S` (31) are unbound and not yet `#define`d in
`map_kbd.c`; the deck-1/deck-2 idiom to copy is space/n
([map_kbd.c:102-110](../scripts/shims/map_kbd.c)) — `K_PLAY` with the deck in
the channel, not `CH_GLOBAL`. Add the two `#define`s and two rows:

```c
{ EV_KEY, KEY_W, K_PLAY, 1, 0, 0 },
{ EV_KEY, KEY_S, K_PLAY, 2, 0, 0 },
```

These are *additional* to SPACE and N, which stay as they are.

## 4. Hot-swap

Three separate paths, three different answers.

**MIDI — already implemented.** `midi_connect_try()` runs once a second and
detects the surface going away, logging "control surface '%s' disappeared
(unplugged?); waiting for it to come back" and re-subscribing on a new client
number when it returns ([midi_io.c:200-280](../scripts/shims/midi_io.c)). Its
own comment says the call is cheap and idempotent, which is what lets the read
loop call it forever. **Not yet verified on the unit with an actual unplug** —
that is a 30-second test and should be done before telling anyone it works.

**evdev (keyboard, mouse) — gap.** A hot *add* is only noticed if some other
event happens to arrive afterwards: the loop rescans on every iteration, but it
is parked in `poll(..., -1)` when nothing is happening, so there is no
iteration to rescan in. This is the same loop as the release defect, and the
same fix closes it.

**audio — unverified.** `audioshim.c` has a prepare-retry after a failed write
([audioshim.c:1296](../scripts/shims/audioshim.c)) but no device-loss or reopen
handling, so a PCM opened against a card that is unplugged has no obvious way
back. Whether rbp survives the FLX4 being unplugged mid-track, and whether
audio returns on replug, has not been tested. Treat audio as the least likely of
the three to survive a replug.

## 5. The pop when rbp first opens the FLX4's audio

The machinery exists and is exact: every output channel is held at zero for
`STARTUP_MUTE_MS` after the first write and then faded in over
`STARTUP_FADE_MS` ([audioshim.c:1176-1264](../scripts/shims/audioshim.c),
`mute_frames`/`fade_frames` at 191-192, the env reads at 549-550). It ships
**disabled**:

```sh
: "${RB_STARTUP_MUTE_MS:=0}"     # scripts/device/rb.conf:193
: "${RB_STARTUP_FADE_MS:=0}"     # scripts/device/rb.conf:194
```

and the comment above those lines asserts that "USB audio has no codec power-up
transient, so unlike the SC Live 4 there is nothing to mute over" — which the
operator has now found to be false for this card. The comment is the thing that
is wrong; the code is fine.

`docs/09-audio.md:262` already gives the values to try
(`RB_STARTUP_MUTE_MS=1500 RB_STARTUP_FADE_MS=300`). This is a property of the
card and not of the port, so it belongs in `rb.local.conf` on the unit — but the
`rb.conf` comment must be corrected either way, because as written it tells the
next person not to bother.

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

* **The virtual device must exist before rbp starts.** Combined with the
  hot-add gap above, a device created afterwards is never enumerated, so every
  test begins with `systemctl restart rblive4`.
* **Do not put a `pkill -f`/`pgrep -f` pattern in the same command as the plain
  name it is meant to match.** Three separate times in this session a pattern
  like `[v]keyd.py` matched the *command string itself* — because the same
  command line also contained the plain path, for the `cat` that was writing it
  — and killed the session. Split the kill into its own invocation, or match on
  a pid. This is the same trap `docs/13-raspberrypi4.md` records for
  `pkill -f "/root/pdj/rbp"`.

## What is temporary on the unit

The measurements were taken with two bring-up overrides in
`/opt/rblive4/rb.local.conf`, both marked TEMPORARY there and neither of them a
fix:

* `RB_MIDI_MAP=kbd` — because `map_flx4.c` binds no `K_SOURCE`/`K_BROWSE`, so
  the controller alone cannot open the Source list
  ([docs/13](13-raspberrypi4.md), the S6 row);
* `RB_KNOB_VERBOSE=1` — to see what the map receives.

Both should go once the permanent fix exists, and the map override in particular
means **the FLX4 is currently ignored entirely**.
