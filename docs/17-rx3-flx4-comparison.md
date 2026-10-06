# 17 — the sibling port: what `Rx3-flx4` does differently

A comparison against the other public attempt at running XDJ-RX3 firmware on a
Raspberry Pi, read on **2026-10-02**.

- Upstream: `https://github.com/mutlisensor/Rx3-flx4`, working copy read at
  `../rx3-flx4` (branch `main`, HEAD `c565988`).
- Their target is the **same vendor binary**: their shim calls
  `ui::KeyInput::keyCodeAsText()` at `0x37cde4`, which is the address
  [15 — flx4-midi](15-flx4-midi.md) and [08 — controls](08-controls.md) derive
  their keycodes from. So this is a like-for-like comparison of two designs
  against one binary, not two ports of different firmware.
- Their hardware differs (Pi 5 in the README, Pi 4 supported later; HDMI or the
  Touch Display 2), and they support two controllers — the FLX4 and the DDJ-400.

**Why this document exists.** Their tree solves several problems this port has
not solved yet. This is written so a later session does not have to re-read
their repository to find them.

**§2 records what their tree does; §4 is the decision.** Read §4 before acting on
anything in §2.

**Their read-only USB overlay is discarded as something this port would do.**
It was read on 2026-10-02 and declined by the operator on 2026-10-06 (*"remove the
readonly stick, that's not useful"*) — so it is not a candidate here, not a parked
plan, and not to be re-proposed. For the record, the `EROFS` it works around is
caused by their own read-only presentation: all it demonstrates is that the
firmware *asks* to write, which [10 — USB](10-usb.md) already records from this
side. The only trace kept is the media row in the table above.

**How to read it.** Every claim below is anchored to a file and line in their
tree as read on the date above. Those line numbers will move; the *design* is
what is being recorded. Nothing here has been run on our hardware, so nothing
here is marked "measured" in this tree's sense — it is a reading of their code
and of the claims their own documents make. Their firmware is not in their
repository, and none of it is copied here.

## 1. The two architectures

Both ports load a shim into the vendor player process. After that they part:

| | this port | `Rx3-flx4` |
|---|---|---|
| Control input | MIDI arrives on an ALSA **sequencer** port inside the process; `map_flx4.c` turns it into keycodes and calls `sendKey` **through a vtable word** (`rbp_key.c:65`) | a **host Python process** reads the controller's rawmidi device and writes a 24-byte record into a chroot FIFO; an in-chroot shim reads it and calls the dispatcher at a **hardcoded `0x37ad64`** |
| Binding changes | edit C → Docker cross-build → `scp` → restart the service | edit Python → restart the bridge |
| Media | `rbp` mounts the operator's stick itself, and rewrites `export.pdb` on it while playing | `ro` lower mount + `fuse-overlayfs`; the firmware's writes land on the Pi |
| LED state | read `LedStat` at addresses this tree derives; meter via a hook on `getLedValue` | read `LedStat` out of the live heap, **found by a vtable scan**, published through a file |
| Tests | host suite under qemu-arm (`test_flx4`: 4127 checks) | **none** — no fixtures, no assertions, no harness |

## 2. What they do better

Ordered by what it would be worth here.

### 2.1 A non-mutating `install.sh doctor`, and a `clean` that unmounts first

`doctor` is the ordinary install path with a failure accumulator, and it exits
**before the first `sudo -v`**, so it genuinely changes nothing while checking
layout, ownership, the connected controller, every prerequisite (mapping a tool
name to its real apt package), strays, recovered-firmware state and host install
state — then prints one paste-ready `apt install` line.

`install.sh clean` unmounts every `findmnt` match under the root and USB dirs
**before** `rm -rf` (`install.sh:148-149`). That is the direct antidote to the
defect this port hit, where a bind-mounted `rm -rf` emptied the *host's* `/dev`;
their build-time `rm -rf` targets sit inside a chroot with no binds at all.

They also carry a `strays` subcommand that sweeps directories left behind by a
real shipped bug — a `$R`/`$RX3_ROOT` expanded inside a quoted heredoc — with
`doctor` warning and `clean` sweeping. Self-healing from a defect they shipped,
rather than a note asking the user to clean up.

### 2.2 Objects found at run time, validated structurally, failing loudly

They do not hardcode the `LedManager` address. They scan their own readable and
writable mappings for the `PanelComController` vtable, read the manager from
`+88`, and then **validate the candidate** — the LedMgr vtable *and* that the
LedStat header's ids/slots/pointers are in range — before accepting it
(`control-shim.c:34-41`). When the scan fails they log the region count, the scan
result and the hit count, so a failure is a number rather than a mystery
(`control-shim.c:84-86`).

This port does scan by vtable too — `scan_plinn()` — and that scan shipped with a
channel off-by-one that silently rejected deck 1 and filed deck 2 as deck 1, so
every `plinn()` write, the beat-loop path included, landed on the wrong deck for
a whole evening. The lesson worth keeping is the *validation and the loud
failure*, not the scan.

Their trick for making the scan possible is worth copying if this port ever
chroots: they bind-mount a **real procfs at `/hostproc`** so the shim can read
its own maps while the firmware keeps its fake `/proc` (`mount-rx3.sh:9-11`).

### 2.3 The LED source is the firmware's *computed* state, with a torn-read filter

They decode the `LedStat` the firmware rebuilds every 20 ms regardless of whether
anything is listening on the panel bus (`control-shim.c:24-30, 60-64`), and
publish state, brightness, RGB and the **blink period in ms** per LED.

Two ideas here are better than this port's equivalents:

- **A torn read is rejected by requiring the state to be identical twice**,
  1.5 ms apart, before publishing (`control-shim.c:94-95`); they then skip the
  write entirely if it has not changed since the last publish (`:97`). This port
  discovered the same class of hazard the hard way — a pad record read while rbp
  was clearing it.
- **Blink is driven host-side from the firmware's own period** as the half-duty
  timer (`controller-bridge.py:250`), rather than a period the shim invents. This
  port hand-rolled a blink because the FLX4 has no brightness; the firmware's own
  period is the better source for it. **This port now does the same** — see §4
  item 2 for the measurement and what `rbp_led.c` does with it.
- Their **meter** is a direct port of the firmware's own segmented table from
  `ui::Mixer::MonoLvMeter::calcLedValue`, sent as CC `0x02` in the panel's bands
  — a cleaner derivation than this port's rescale of an 11-segment bitmask, and
  the same route [15 — flx4-midi](15-flx4-midi.md) records as the panel's
  host-driven meter.

Their brightness rule is honest and documented: `"dim"` reads as off, because the
FLX4's buttons are single-colour and keep their own low backlight.

### 2.4 One table per controller, feeding everything

`controllers.py` is a single dict keyed by USB id, and it is the source of truth
for detection, `doctor`, **generated udev rules** (`install.sh:262-267`) and the
install — so permissions and detection cannot drift from the mapping. A
per-controller `keepalive` field carries the vendor SysEx cadence (the FLX4 needs
one every 200 ms; the DDJ-400 needs none and gets one init instead).

Their hotplug split is economical: the bridge **exits** on device loss, udev
restarts it, and the helper decides between *restart only the bridge* (player
alive, already on that card) and *restart the service* (`controller-hotplug.sh:14-17`).

### 2.5 Smaller things worth knowing

- A **jog watchdog** emitting a synthetic zero tick after 80 ms of idle
  (`controller-bridge.py:77-87`) — a compensation for a controller that is not an
  RX3.
- The tempo slider's sign fix, `(val - 0.5) * 2.0`, because the engine wants a
  signed value centred on zero.
- **`deck_showing()` reads the firmware's own pixels** to decide whether the
  browse knob should become BROWSE — *"self-correcting, unlike tracking screen
  changes we cannot all see"*. A framebuffer read used as a control input, not
  just as a test.
- `pi-clock.S` is spliced into the i.MX6-built binary by their patch step: it
  implements `clock_gettime` and returns a monotonic ~3 MHz tick, and neutralises
  interrupts for GPIOs that do not exist with `bx lr` — hardware tolerance
  instead of SoC emulation. This port's clock trouble went the other way (a
  32-bit `long` wrapping a nanosecond `CLOCK_MONOTONIC` every 4.29 s), so the
  contrast is instructive: they *supplied* a clock rather than adapting to one.
- Completion-sentinel idempotency: their extraction writes its state file
  **last and atomically**, and `doctor` keys off exactly that pair to tell
  "incomplete" from "complete".
- `uhubctl` is in their dependency list — the USB power cut they use to revive a
  FLX4 that comes up dead at boot.

## 3. Where they are behind, for calibration

Listed so the comparison is not read as a verdict on the whole design.

- **No test suite for the mapping.** No fixtures, no assertions, no qemu
  harness; the nearest thing is a manual mode that reads MIDI from stdin and
  prints the key it *would* send. This port's `test_flx4` drives the real
  dispatch over the real map. The gap matters directly because:
- **Their control FIFO has no framing** — 24 raw bytes on a byte stream, with no
  magic and no length prefix, reassembled by a byte counter (`control-shim.c:151`).
  A single stray byte desynchronises it permanently. A fixture-driven test is
  what makes that safe.
- **Two duplicated keycode dicts** (`controller-bridge.py` and `rx3-control.py`)
  — a drift hazard of exactly the kind one source of truth prevents.
- **The dispatcher is a hardcoded concrete address** (`0x37ad64`), and they must
  fake the panel to release the firmware's startup input gate before each
  dispatch. This port resolves the same call through a vtable word.
- **A device unplug restarts a process.** Their bridge exits and udev brings it
  back; the worst case is a service restart mid-set. This port's in-process
  design survives the unplug (see [16 — input and hotplug](16-input-and-hotplug.md)).
- **Their eject has the same limit this port measured**: a press stops the media
  in the player but does not release the host mount.
- **Documentation drifts**: `STATUS.md` is dated 2026-09-11 and cites a
  `DEPLOY.md` that does not exist; `.gitignore` still carries a `host/bin/` rule
  for a directory the tree does not have.
- **No Docker cross-build** — the shim is compiled on the Pi with
  `arm-linux-gnueabi-gcc` from apt.

## 4. What to port, and in what order

1. ~~**`doctor` and an unmount-before-`rm -rf` `clean` (2.1).**~~ **Done,
   2026-10-06.** `scripts/device/doctor.sh` is the check-only path in this
   tree's idiom, reachable as `sh install.sh doctor`; it exits before touching
   anything and prints one paste-ready fix block. Its headline check is the one
   this tree has most often got wrong — each shim's deployed build against the
   copy `rbp` is actually loading, read from the running process's
   `/proc/<pid>/maps`. The `clean` half needed nothing: the dangerous
   `rm -rf "$RB_CHROOT/dev"` went on 2026-09-27 (`fix-dev.sh:49-64`), and this
   tree now deletes nothing at all where the sibling unmounts first.
2. ~~**The torn-read filter and the firmware's own blink period (2.3).**~~ **Done,
   2026-10-06.** Both landed in `scripts/shims/rbp_led.c`, with the decision split
   into a pure module (`led_table.c/.h`) so it is testable without a Pi
   (`test_led_table`, 40 checks).

   The **period is real, and it was measured rather than taken from the sibling**:
   rbp's `uif::LedStat` entry carries a blink period in ms at `+28`
   (`rbp_abi.h`'s `LED_ENTRY_OFF_PERIOD`), read live out of `/proc/<pid>/mem`
   (51 entries; `+28` = 0 ×48, 250 ×2, 500 ×1, and the three non-zero ones are
   exactly the three entries whose State is 2 — deck 1 PLAY 500 ms, deck 2 PLAY
   250 ms, CfxFilter 250 ms). The offset and the unit agree with the sibling's
   decode. Two things the reading settled that the sibling's code does not say:
   it is a **full cycle** (so half duty is `(now % p) < p / 2`), and rbp does
   **not** toggle State to blink — a 6 s watch showed State-2 entries never
   change, and an entry caught starting to blink went `0` → `300` in the same
   rebuild that set State 2. So the state word says *that* it blinks and `+28`
   says *how fast*, and both are needed.

   The **filter** is a within-tick double read of the whole table 1.5 ms apart
   (the sibling's interval), keeping only entries that agree; an entry in flux
   carries the value it had in the previous settled table, matched by
   (id, channel) rather than position, so a momentary disagreement cannot blink an
   LED dark. It sits behind the existing `led_disabled || !midi_out_ready()` guard,
   so it costs its 1.5 ms only when there is a panel to light, and it logs under
   `LED_VERBOSE` (change-gated) so the filter is observable — a filter that never
   fires is indistinguishable from one that is not there.

   **The fallback did not move.** `LED_BLINK_FALLBACK_MS` is 800 — the old
   `led_tick & 8` cadence, 400 on / 400 off — so the only LEDs whose cadence
   changes are the ones rbp actually asks to blink, which is what makes a
   before/after drill readable.

   **The drill ran, and it found a bug in the filter's own wiring.** With the FLX4
   attached, the `LED_VERBOSE` line revealed that the carry-forward had **never
   executed**: `led_snapshot()` zeroed `led_snap_count` to mark "no reading" and
   then passed that same variable as the merge's `prev_count`, so the history
   lookup always ran with a count of zero and every entry in flux was dropped
   instead of carried. It neither crashed nor logged anything wrong — the only
   tell was the arithmetic in its own line, `23 of 42 disagreed; 19 in the
   settled table` (42 − 23 = 19, and none of the 23 carried). Fixed by capturing
   the count before the reset; the same tick now reads `19 of 28 ... 28 in the
   settled table`. The pure module was correct and its 40 tests passed — the bug
   was in a **call site no test links**, since `rbp_led.c` is linked into nothing.

   The drill also measured the tearing: on about half the ticks **19 of 28
   entries disagree** between two reads 1.5 ms apart, and on the other half all
   28 agree — the tick phase striding against rbp's 20 ms rebuild. In the steady
   state the filter costs nothing (flux 0, all entries settled; `led_snap` read
   directly out of the live process).

   **Which LEDs can show the new cadence on this surface is narrower than
   expected**, and that is a finding rather than a gap: of the three State-2
   entries, deck PLAY is driven by `PlayEngine`'s play flag and ignores rbp's
   State (and rbp asks for deck 2's blink even with nothing loaded, which the
   shim's `loaded` refuses), and **CfxFilter's blink never goes to the wire at all
   because `map_flx4.c` sets `.fx_ch = -1`** — `led_apply_g()` returns before it
   sends. Seeing the cadence at the wire needs a deck **loaded and cued, not
   playing**.

   **CONFIRMED AT THE WIRE, 2026-10-06**, with a track loaded and cued on each
   deck (both PLAY LEDs at State 2, deck 1 `+28` = 500 and deck 2 `+28` = 250).
   The FLX4's PLAY note for each deck alternated at exactly its own entry's rate —
   a half-period of **244.9 ms** on deck 1 (on 244.7 / off 245.1 over 129
   transitions) and **121.6 ms** on deck 2 over 33. **Same LedStat id (49), two
   decks, two different rates**, each matching the per-entry period: that is the
   discriminator a single global fallback cannot pass, and the fallback stayed at
   800 ms unused. Deck 2's on-runs read 136 ms against a true 125 because the
   shim resends every ~100 ms and a 125 ms half is sampled at ~1.25 points per
   half; deck 1's 250 ms half has ~2.5 and reads true. Deployed and confirmed
   loaded as `knobshim.so 1f080c96`.
3. **Structural validation in the object scans (2.2).** Not urgent, but it is
   the shape of the fix for the class of bug `scan_plinn()` had.

Nothing here is a reason to restructure the port. This tree's in-process design
buys the test suite and the unplug survival, and neither is worth trading.

## 5. Method and evidence

Read on 2026-10-02 by four parallel readers, one per axis, each reporting
file:line citations: control/MIDI architecture, USB media and storage, display
and controller LED, and install/deploy/docs. The load-bearing citations —
`doctor`/`clean` (`install.sh`), the run-time object scan, the LED decode and the
meter table — were re-checked against the source before this document was
written. The remainder are as read, and their line numbers move with their tree.

Not determined, and left open: whether their `LedStat` offsets, vtable addresses
and the vitals at `0x4cfb08`/`0x4d5e60`/`0x50170` hold on any firmware other
than 1.19 (their own code marks them as disassembly-derived); and the exact
firmware-side debounce contract for their touch reports beyond their own note
that held and release reports must repeat for about 10 ms.

