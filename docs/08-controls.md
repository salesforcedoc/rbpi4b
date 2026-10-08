# 08 — Controls (MIDI control surface → rbp keycodes)

Transport, deck, mixer, jog, pads and FX are driven from a MIDI control
surface, and rbp's own LED and VU state is mirrored back onto the panel. On the
Pi the surface is a **DDJ-FLX4** (`map_flx4.c`, the compiled-in default, see
[15 — FLX4 MIDI](15-flx4-midi.md)), whose tables are written from Pioneer's
published MIDI list and **not yet confirmed against the unit**: nothing in this
repository has been run with an FLX4 attached. A **keyboard map** needs nothing
plugged in, and the previous target's JP21 map is kept in the build as a worked,
measured example.

## Structure: the front end, the bridge, and a map

The controls shim was one 2500-line file (`knobshim2.c`), which mixed three
concerns: knowing rbp, knowing a controller, and being a thread. It is now a
small set of modules built into `knobshim.so`:

| File | Owns |
|---|---|
| `ctrlshim.c` | the constructor, the threads, map selection, and map-agnostic dispatch |
| `ctrl_map.c` / `.h` | `struct ctrl_map`; the note/CC binding tables every map fills, and their invalidation |
| `map_jp21.c` | the previous target's tables |
| `map_kbd.c` | the keyboard fallback: evdev keycodes → rbp keycodes, with no MIDI and no controller involved |
| `evdev_io.c` / `.h` | the non-MIDI event source (`/dev/input/event*`): discovery, the poll loop, and a handler that receives raw evdev triples. It knows no keycode. |
| `mididump.c` / `.h` | the dump format: record a surface, replay it with no hardware |
| `rbp_bridge.c` / `.h` | **everything about rbp**: `is_rbp_process()`, the key manager and `send_rx_key()`, the mixer-engine `me_*()` cue helpers, the PlayEngine probes, the `/proc/self/maps` PlayerInnards scan, the LedManager walk, the direct global writes, the `IPowerManager` stubs, `install_meter_hook()`. Every address and offset lives in `rbp_abi.h`. |
| `rbp_led.c` / `.h` | the LED mirror: rbp's LedStat table and engine state → panel notes, blink, the LED logging |
| `led_table.c` / `.h` | the **pure** half of the mirror: the settled-table merge (the torn-read filter), the (id, channel) lookup, and the blink phase. No address of rbp's appears in it, so it is tested without a Pi (`test_led_table`). |
| `rbp_vu.c` / `.h` | the meter bit maths and the segment rescale |
| `midi_io.c` / `.h` | the sequencer in, and the surface's LED/meter port out (Note On/Off, CC, and one whole SysEx) |
| `controllers.c` / `.h` | **the controller table**: one row per surface — its map, port-name hint, card id, USB id, init and keepalive SysEx. Pure: no I/O, no globals, no clock, so `test_controllers` links it and runs it on the host. `controllers_cli.c` is the same table with a `main()` for shell callers |
| `shimutil.c` / `.h` | `klog()`, the clock, and the environment helpers |
| `shmstate.c` / `.h` | the globals the audio shim reads, and the contract that guards them |
| `map_flx4.c` | the DDJ-FLX4's tables — written and fixture-tested, tables **unverified** until a dump from the unit, see [15](15-flx4-midi.md) |

The split exists so that a map can exist without duplicating the rbp bridge, and
so the bridge never learns a controller's note numbers. `CTRL_OBJS` in
`scripts/shims/Makefile` is the list the build actually uses; this table says
what each entry is for.

### The map interface

`struct ctrl_map` is small on purpose — seven members and no data: a `name` (the
value `MIDI_MAP` selects it by, which must match the string in `rb.conf`), five
functions, and one question about where the surface's events come from.
`build()` fills the shared binding tables and reads whatever environment the
surface needs (jog resolution, polarity); `startup()` is rbp-side work that only
makes sense with this surface attached; `event()` takes one sequencer event;
`tick()` is the ~20 ms heartbeat the two time-based features need (button-hold
timeouts and idle state).

The last two members are the non-MIDI pair, and they are the whole of the
special case in the front end: **a surface is not always a MIDI surface**. A
keyboard and a mouse are `/dev/input/event*` devices carrying `EV_KEY`/`EV_REL`,
and no amount of sequencer configuration will make one appear as a client.
`devices()` answers "how many non-MIDI event sources does this map want?", `0`
for a MIDI-only surface; `input()` receives one raw evdev `(type, code, value)`
triple. So `RB_EVDEV_MAP=kbd` makes `ctrlshim.c` start the evdev reader
(`evdev_io.c`) and hand every triple to that map, and a map that leaves the pair
`NULL` — `flx4`, `jp21` — starts nothing on the evdev side. The two selections
are independent: `MIDI_MAP` picks the map for the sequencer and `EVDEV_MAP` the
map for `/dev/input/event*`, and they may name the same map (then it is built
once). `input()` gets the kernel's numbers, not a keyboard-specific hook,
which is what keeps "which key is PLAY" out of `evdev_io.c` and in a map's
table.

All four of the optional members may be `NULL`, and a map that leaves
`devices`/`input` `NULL` is simply MIDI-only. The one hard consequence of
`devices() > 0` is in the sequencer's failure path: with no `/dev/snd/seq` a
MIDI-only map gives up as it always did, while a map with a non-MIDI source logs
the failure loudly and keeps its thread alive, because its keyboard does not
need the sequencer.

The bindings themselves are **shared storage, not per-map state**, because every
surface is "some buttons and some absolute controls" and the dispatcher has to
be able to invalidate the absolute cache: `note_map[]`/`note_map_n` for buttons
(a note on a receive channel → an rbp keycode, with `key` 0 marking a control rbp
has no code for) and `abs_map[]`/`abs_map_n` for CCs whose 7-bit value is a
position. A map fills them with `add_note()`/`add_abs()`, `ctrl_bindings_reset()`
empties them for a rebuild, and `ctrl_abs_invalidate()` forgets every "last value
sent" so the panel's reply to the absolute-control query re-applies rather than
being dropped as a repeat. All of that lives in `ctrl_map.c`.

A map never touches rbp and a bridge never touches a note number. What a map may
ask rbp for is exactly the set of named functions in `rbp_bridge.h` —
`send_rx_key()`/`send_rx_key_f()`/`send_rx_key_fl()`, `cc_to_10bit()`, the
`me_*()` mixer-engine helpers, `plinn()`/`aloop_*()` — instead of reaching an
address itself. Semantics that are peculiar to one surface (shift, FX select,
whether a CC is a switch) stay local to that map.

`RB_MIDI_MAP` selects one of `flx4`, `jp21`, `kbd`, `none`, and `RB_EVDEV_MAP`
selects one of `kbd`, `none`; they are two independent choices, one per event
source, because a controller and a keyboard are different devices. Both have a
default in `rb.conf` and in the source (`flx4` and `kbd`), so a unit with a
controller, a keyboard and a mouse has all three live with no configuration. An
unknown name falls back loudly rather than silently, and the warning names *which*
variable was wrong — with two selections the bad value alone no longer says.
`kbd` is in both tables on purpose: `MIDI_MAP=kbd` keeps meaning "no controller,
keyboard only", which is what operators wrote before the second selection existed,
and `EVDEV_MAP=none` is how to ask for a controller with no keyboard at all. The
previous target's map is one word away (`MIDI_MAP=jp21`).

### One table per controller

A surface's four names are written down **once**, in `controllers.c`: the
`MIDI_MAP` value that selects its map, the ALSA sequencer port-name substring it
is detected by, its ALSA card id, and its USB id. `rb.conf` ships all four
*empty*, and empty means "the table's row for the selected controller" — so
selecting `jp21` moves the map, the port hint and the audio card together. They
did not move together before: `RB_MIDI_IN_MATCH` said `FLX4` and `RB_AUDIO_DEV`
named `DDJFLX4` while `RB_MIDI_MAP` chose the surface, in three files that never
referenced each other, and the audio card had nothing to do with the surface at
all. Setting any of the four in `rb.conf` still wins, and is how a device the
table does not name is used; `doctor.sh` reports when a setting and the table
disagree.

`controllers_cli` is that table asked from a shell — `detect` (what is *plugged
in*, by USB id; the shim itself matches the sequencer port name, not USB),
`names`, `usbids`, `card-id <id>`, `match <id>` — and it is what `doctor.sh` and
`install.sh` read, so no shell script keeps its own copy of a surface's name.
`kbd` and `none` are **maps**, not controllers: they have no device behind them
and are deliberately not rows.

The table also carries two messages rather than names, because a message belongs
beside the surface it is for: an **init** SysEx sent once (the JP21's
absolute-control query, and `NULL` for the FLX4) and a repeating **keepalive**
(the FLX4's 12-byte vendor SysEx every 200 ms — see `CTRL_KEEPALIVE` below and
[15](15-flx4-midi.md)).

## The keyboard map (`RB_EVDEV_MAP=kbd`, or `RB_MIDI_MAP=kbd` alone)

The surface that needs nothing plugged in (`map_kbd.c`). It exists so display,
audio and the USB import can be brought up and played with on a Pi whose USB bus
is empty, and so a controller that has not arrived cannot block the rest of the
port.

| Key | rbp keycode | Channel |
|---|---|---|
| `space` / `z` / `x` | `0x4101` K_PLAY / `0x4102` K_CUE / `0x4112` K_SYNC | deck 1 |
| `n` / `m` / `,` | the same three | deck 2 |
| `w` / `s` | `0x4101` K_PLAY | deck 1 / 2 |
| `1` / `2` | `0x4311` K_LOAD | deck 1 / 2 |
| `5` / `6` / `7` / `8` / `9` | `0x0201` K_SOURCE / `0x0202` K_BROWSE / `0x0203` K_TAGLIST / `0x0204` K_PLAYLIST / `0x0205` K_SEARCH | global |
| `0` | `0x0206` K_MENU | global |
| `↑` / `↓` | `0x420c` K_SELECTOR, rotate ±1 (repeats while held) | global |
| `Enter` | `0x420c` K_SELECTOR, press/release | global |
| `Backspace`, mouse right button | `0x420d` K_BACK | global |
| `Esc` | `0x0201` K_SOURCE | global |
| mouse wheel | `0x420c` K_SELECTOR, rotate ±1 per notch | global |

The browse keys sit on the rest of the digit row on purpose: `1`/`2` are LOAD, so
the whole browse surface is one hand's worth of keys that need no mnemonic.
All six browse keys now exist in `rbp_abi.h`.

**`8`/`9` were the last two to be filled in, and only by measurement.** rbp has
all six controls — the labels are in the binary as `Source`, `BROWSE`, `TAGLIST`,
`PlayList`/`PLAYLIST`, `Search` and `menu` — but for one release `rbp_abi.h` had a
keycode for only four of them, so the two rows existed with key `0` (`ctrl_map.h`'s
"a control rbp has no code for"), which made a press *visibly* pending: `kbd_build()`
said so at startup whether or not `KNOB_VERBOSE` was on, and under it the trace
named the control. The two unclaimed slots in that block (`0x0204`, `0x0205`) were
left as "a plausible-looking trap, not an answer" until the top menu's bench knob
(`POINT_MENU_KEY_EXTRA`, below) put them on the wire on 2026-09-29:

* **`0x0204` K_PLAYLIST** switches the browse view to the PLAYLIST view — the left
  sidebar becomes BANK 1..4 + DELETE.
* **`0x0205` K_SEARCH** opens the SEARCH view: the list area is replaced by a
  search pane with an on-screen QWERTY keyboard.

Both were sent from the blank browse screen *and* from the SOURCE screen and landed
on the same screen each time — `0x0205` pixel-identically, `0x0204` differing only
in the bottom 16-row hint band. The negative control is in the same runs: with the
two buttons on their shipped `0` sentinel, a tap changed nothing at all, twice.
`rbp_abi.h`'s `K_PLAYLIST`/`K_SEARCH` block carries the runs in full.

**`0` K_MENU is real but narrow, and that is worth knowing before doubting it.**
Measured the same day: from the SOURCE screen it toggles the MY SETTINGS panel
(263,596 px each way, and still open 3 s later, so a toggle and not a timeout); from
the BROWSE, TAG LIST and PERFORMANCE screens it changes nothing at all — 0 px, full
frame. A MENU press that appears to do nothing is the screen it was pressed on.

**The same keycode carries a second meaning, and rbp's own timer decides which.**
Measured 2026-09-29 through the keyboard path, which sends `0x0206` on `CH_GLOBAL`
exactly as the panel does: a press of 200 or 300 ms changes nothing (587 px of drift),
**400 ms and up reach UTILITY** — ~804,000 px, the whole screen, from the SOURCE
screen and from BROWSE — and a 16-band md5 sequence at ~200 ms resolution shows one
whole-screen change at t ≈ 455 ms with no menu before it. So the threshold is
**between 300 and 400 ms**, it lives in rbp, and nothing in the shim should try to
reproduce it: `menu_zone.h`'s `MZ_HOLD_FINGER_MS` (350) is what the *finger* must do
and `MZ_HOLD_KEY_MS` (500) is how long the panel then holds the key, so rbp's timer
sees a hold whichever side of its threshold the finger landed on
([07](07-touch.md#the-press-and-hold-and-the-clock-that-decides-it) has the unit
evidence, including the clock that measures the finger). **Only MENU has a second
meaning**: the other five were held 500 ms and compared with their own taps at a 2×2
block mean — SOURCE 318, BROWSE 357, TAG LIST 243, PLAYLIST 195, SEARCH 0 differing
blocks, all inside rbp's drifting deck rows — so a hold of them lands exactly where a
tap lands, and the panel holds them too rather than carrying a per-button table.
**The finger that has to do the 350 ms is the operator's, and they have now done it**:
asked whether the delay was right, they said *"yes right amount of delay"*
(2026-09-29), so the constant ships as measured and felt rather than as a guess.

One kernel-ABI collision is worth knowing before editing the table: **`REL_WHEEL`
is 8 and `KEY_7` is 8** — the same number in two namespaces. The binding loop
matches on the *pair* `(type, code)`, so `EV_KEY` code 8 is the digit 7 (TAG
LIST) and `EV_REL` code 8 is a wheel notch. Neither row can be folded into the
other, and both directions are pinned in `test_kbd.c`.

The second deck mirrors the first on the same keycodes with the deck in the
*channel*, which is how the JP21 map drives both decks from one table. rbp has no
per-deck variants of the selector, BACK, SOURCE or the browse keys, so all of
those are global on both — `CH_GLOBAL`, which is the channel rbp's own
browse/source keys are sent on.

It is not a degraded version of the FLX4 map:

* **No MIDI at all.** `devices()` returns 1 and `event()` is `NULL`, so on the
  evdev selection `ctrlshim.c` starts the reader and this map consumes no
  sequencer event, whichever map holds the MIDI side. `/dev/snd/seq` may be
  missing entirely and the keyboard still works — the log says so loudly rather
  than the shim giving up, which is the one behaviour that differs from a
  MIDI-only map. (Selecting this map on *both* sides — `MIDI_MAP=kbd` — is the
  same thing again: it is built once, and the front end logs that the MIDI map has
  no event handler rather than leaving the silence unexplained.)
* **No panel.** There is no keyboard LED and no mouse meter, so there is nothing
  for `rbp_led.c`/`rbp_vu.c` to drive. That is the *map's* answer rather than the
  flag's — a surface whose table declares no meter row gets no hook installed and
  no traffic whatever `RB_LED_VU` says, while the FLX4's table *does* declare one
  (a host-driven level on CC 2), which is why its meter is live as of 2026-10-01
  and `RB_LED_VU=0` is only a manual OFF switch now. The master-cue assertion
  `vu_thread` makes is unrelated and still runs on every target.
* **No rbp-side startup of its own.** `startup()` logs and returns: the JP21
  map's routing and Sound Color FX defaults exist for a surface with DECK/LINE
  switches and a Sweep knob, and the master level at unity is re-asserted for
  every map by `vu_thread()` in `rbp_vu.c`.

`KBD_DEV=/dev/input/eventN` pins the reader to one node instead of scanning;
empty (the default) discovers every node that can report key events, which is
the keyboard *and* the mouse — that is where `BTN_RIGHT` and the wheel come
from. **With `KBD_DEV` set, the mouse is not read**, so both of its bindings go
away; pin a node only to separate two devices that are being confused, and treat
the pin as a bench tool rather than a unit setting: an `eventN` number is not
stable across a re-enumeration, so a pin that is correct when written can name a
different device after a replug. Left empty — every node with `EV_KEY`, re-scanned
— the reader follows the devices wherever they land.

The event source is `evdev_io.c`, and it deliberately knows no keycode: it hands
the map the raw triple and nothing else. Two readers of one evdev node both get
every event, so this coexists with the pointer path in `fbshim.so`
([07 — Touch](07-touch.md)) without either noticing the other.

Start-up, and the per-event trace under `KNOB_VERBOSE=1`, are both one place to
read:

```
knobshim2: maps: MIDI_MAP='flx4' EVDEV_MAP='kbd'; <n> notes, <m> abs knobs; ...
knobshim2: evdev map 'kbd': 1 non-MIDI source(s) wanted; evdev reader started
knobshim2: evdev[913] /dev/input/event0 'AT Translated Set 2 keyboard' appeared; adding it
knobshim2: evdev[914] /dev/input/event2 'Logitech USB Optical Mouse' appeared; adding it
knobshim2: kbd: evdev type=1 code=57 value=1 -> 0x4101 press
knobshim2: kbd: evdev type=1 code=57 value=2 -> 0x4101 ignored   <-- autorepeat
```

The `appeared` lines carry the millisecond they were measured at, and are logged
whether or not `KNOB_VERBOSE` is on, because "which device did it open, and when"
is the question that matters both when nothing works and when a plug takes too
long to be noticed.

**Status: run on hardware** (2026-09-26). The bindings are asserted against
synthetic evdev triples in `make -C scripts/shims test` (`test_kbd`, 365 checks),
which pins the deck channels, the wheel's direction, the right-button, the
press/autorepeat/release edges and the rotation clamp; the keys have since been
driven on the unit through a virtual keyboard
(`tools/pi-bringup/`), which is what measured the release defect below and the
arrow sign. The *reader* — the loop, the rescan, the hot-add latency and the
release-on-unplug — has its own suite, `test_evdev`, which fakes the kernel at the
`syscall()` boundary and links the shipping `evdev_io.o` unchanged
(`test_evdev.c`'s header has the mechanism; it is 53 checks over five scenarios
under a virtual clock).

**The arrows are inverted relative to the wheel, on purpose.** With `↑` at `+1`
and `↓` at `-1` the operator reported the pair working backwards, so `↑` is now
`-1` and `↓` is `+1`, and the wheel stays at `+1` — "away from the user" and
"down the list" are the same motion. That sign is the operator's observation and
not a measurement: it cannot be read off the SOURCE panel, which has nothing to
move between. Full account in [16](16-input-and-hotplug.md).

**The reader used to lose a release, and it has been rewritten.** `evdev_io.c` ran
its device discovery at the top of every loop iteration, so every poll round
closed and reopened *every* node; a key release landing in that window was
discarded, the map's held-key latch stayed set, and the next press of that key was
ignored — every press/release key worked once per run. Now a rescan closes
nothing that is still alive, so a queued release on a surviving device is
delivered; a 120 ms press delivers its release.

**How long a hot-plug takes to be noticed, stated precisely**, because it is easy
to check this and wrongly "disprove" it: the 1000 ms deadline is an **absolute**
wall-clock instant and not a timeout re-armed per poll round, so a device plugged
in is picked up within ≤1 s **regardless of traffic on the other devices**. The
old code only achieved that when the bus was quiet — with a 1000 Hz mouse on the
desk its per-round re-arm postponed the rescan without bound, and a plug could go
unnoticed indefinitely. That was the operator's "it needs a restart to see a new
device" behind a moving mouse, and it is why "within a second" is now true
unconditionally rather than true-when-idle. The reader's own `appeared` line
carries the millisecond it happened at, so this is a number to read rather than a
claim to trust ([16](16-input-and-hotplug.md)).

## Finding the controller

Input is never hardcoded to a device node. The shim walks the ALSA sequencer's
client and port lists with `SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT` /
`SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT`, matches the port name against
`RB_MIDI_IN_MATCH` (a case-insensitive substring; empty — the shipped default —
means the selected controller's own hint from the table, which is `FLX4` for the
FLX4), requires the
port's capabilities to include `CAP_READ | SUBS_READ`, excludes its own client,
and subscribes — **in a retry loop**, because the shim starts before the
controller is necessarily enumerated and plugging it in late has to work. A
surface that goes away is noticed and waited for, not treated as fatal.

Two ports are created for us in that same walk: an input port the surface sends
to, and a **second local port** for LED and meter output, subscribed to the
surface's receive side. The directions are matched **independently** — the
kernel gives a rawmidi device one port per direction, and a bidirectional port
answers both tests — with `RB_MIDI_IN_MATCH` and `RB_MIDI_OUT_MATCH` naming them
separately, so a surface whose output port is named differently from its input
port still works.

The rawmidi device is a **runtime fallback**, not the design: it is opened only
when the sequencer route cannot be established, it is retried on a 5 s throttle
while there is no route, and its node search tries `MIDI_LED_DEV` first and then
`/dev/snd/midiC0D0` upward. Which route is in use is **always** one line in
`/tmp/knobshim.log` —

```
knobshim2: LED/meter output route: sequencer 128:1 -> 20:0 'DDJ-FLX4 MIDI 1'
knobshim2: LED/meter output route: rawmidi /dev/snd/midiC0D0
knobshim2: LED/meter output route: none (no sequencer surface, no rawmidi node)
```

— because with no route the UI still draws and no control ever reaches it, which
looks exactly like a broken map. The log, not this page, is the authority on a
given build. Note that the match name is the *only* thing selecting a surface:
with the default `FLX4`, a JP21/SC Live 4 falls through to the rawmidi node,
which is why the old target's LEDs still light on a bench.

There is one deliberate asymmetry in the output path: the JP21's
absolute-control query ([below](#keycode--rbp-and-the-absolute-control-query)) is
a JP21 protocol message, and the sequencer route carries only the three-byte
note/CC messages, so that query goes out on rawmidi or not at all — and says so
once in the log when it cannot be sent.

See [13](13-raspberrypi4.md) for the bring-up checks that confirm `/dev/snd/seq`
exists and the port is visible.

`/dev/snd/seq` itself is the classic silent failure on a Pi OS Lite image: if
the module is not loaded there is no sequencer at all. `fix-dev.sh` runs
`modprobe snd-seq` and fails loudly if the node is still missing.

Not every surface goes through the sequencer, though: the map on the evdev
selection gets its events from `/dev/input/event*` instead (see
[above](#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone)), and for that
map a missing `/dev/snd/seq` is not fatal — the failure is logged loudly and the
keyboard keeps working, with no MIDI input and no LED/meter output until the
module is loaded. Note that this is now true **with the FLX4 selected on the MIDI
side too**, which is the point of the two selections: the keyboard does not need
the sequencer and does not stop working when the sequencer does. `fix-dev.sh` is
still the answer; there is no retry of the sequencer inside rbp.

## The MIDI dump (and why it comes first)

The note and CC numbers for a given controller are **not derivable on paper** —
they differ between controllers of the same family and between firmware
revisions. So the procedure is fixed: dump first, map second.

The FLX4 is the exception that proves the rule, and it is worth being precise
about. Its numbers *are* published, in Pioneer's own MIDI message list, which is
where `map_flx4.c`'s tables come from — so the map exists without a unit in
front of it. That makes it written and testable, not verified: a published list
is still a document about the unit, and the dump below is what turns its rows
into measurements.

* `RB_MIDI_DUMP=<file>` writes every event the shim sees, with timing. The dump
  is written **above** the "is rbp's KeyManager up yet" early-out deliberately:
  during bring-up the KeyManager is often not up, and a dump that depended on it
  would be empty exactly when it is most needed.
* `MIDI_REPLAY=<file>` feeds a dump back through the real dispatcher, and
  `MIDI_REPLAY_SPEED` scales the inter-event delays (default 1.0; `0` ignores
  them). This is what makes a map regression-testable on a bench with no
  controller attached, and it is how the fixture test in `make test` works.

[15](15-flx4-midi.md) is both the record of the FLX4's tables — what each row is,
where it came from and what is still unverified — and the runbook for turning a
dump into a correction of them.

## Env, and the one rule for it

`RB_<NAME>` in `rb.conf` becomes `<NAME>` in the launched environment; that
translation happens in one list in `start-rb.sh`, and **empty means unset**.

The controls shim reads its flags with the `shimutil.h` family — `env_on()`,
`env_num()`, `env_dnum()`, `env_text()` — which answer that question. Use them
for anything new. A `getenv(name) != NULL` presence test is **always true** for
any name in `start-rb.sh`'s `SHIM_VARS` list, because that loop exports every
name whether or not anyone set one, so a presence test cannot be turned off from
`rb.conf`. The other two shims use `envutil.h`'s `env_flag()` instead, which
answers the looser question "is this something other than 0?".

The distinction is **value semantics**, and it is what lets a flag like
`LED_VU` or `KNOB_VERBOSE` appear in `SHIM_VARS` at all: exporting
`KNOB_VERBOSE=0` *turns logging off* rather than "creating" the variable, so
there is no need for a hand-written export block in `start-rb.sh` to say so.
`env_on()` is on for `1`, `true` or `yes` (any case) and for an *empty* value it
takes the default it was passed; `env_num()`/`env_dnum()` fall back on a value
that is absent, empty or not a number — which is what stops an empty
`RB_JOG_PPR=` from becoming `atoi("")` = 0.

The flags:

| Flag | Effect |
|---|---|
| `KNOB_VERBOSE=1` | every MIDI event + resulting keycode in `/tmp/knobshim.log`, and every evdev triple with the keycode it produced — an *unmapped* evdev event is rate-limited to one line per `(type, code)` per second, because a mouse's motion is unmapped by design and arrives thousands of times a second |
| `JOG_VERBOSE=1` / `TEMPO_VERBOSE=1` | raw and normalised jog / pitch values side by side |
| `LED_VERBOSE=1` | log every LED change (`led deckN noteM on/off`) |
| `LED_DISABLE=1` | do not drive the panel LEDs at all |
| `LED_VU=0` | manual OFF switch for the meter bridge (`RB_LED_VU` ships `1`). It is **not** a statement about the target: a surface whose map declares no meter row gets no hook installed and no traffic whatever this says, and the FLX4's map declares one — see the VU section |
| `LED_VU_SEGMENTS=N` | rbp's meter height as the rescale assumes it (default 11) |
| `LED_PADS=0` | do not illuminate the pads |
| `PAD_BRIGHT=1` | bright range for pad illumination |
| `JOG_PPR=N` | jog pulses per revolution — the conversion from a relative encoder's counts to rbp's revolutions-per-second float. Empty means the built-in default; the value is per-surface and unverified until counted on the unit ([15](15-flx4-midi.md)) |
| `JOG_SCALE=X` / `JOG_REV=1` | multiply / invert the jog's derived speed |
| `JOG_IDLE_MS=N` | silence on the jog before the shim tells rbp the wheel stopped (default 120) |
| `KNOB_SCALE=X` | multiply every absolute knob's value (default 1.0) |
| `TEMPO_REV=1` | invert the pitch fader's polarity |
| `LED_DUMP=1` | log every `(id, ch, state)` change from rbp's own LED table |
| `LED_SWEEP=1` | drive rbp's keycodes and snapshot the LED table around each |
| `LED_DEBUG_LOOP=1` | the LED thread's own trace |
| `VU_TEST=1` / `VU_DEBUG=1` | meter sweep / raw hooked bitmask logging |
| `BEATLOOP=1` | enable the experimental beat-loop knob (below) |
| `KBD_DEV=/dev/input/eventN` | the keyboard map's reader reads this one node instead of scanning (a path, not a flag: empty discovers; with it set the mouse is not read) |
| `CTRL_KEEPALIVE=0` | stop sending the attached surface's keepalive SysEx. It ships `1`, and it is the only thing in this port that sends one — the FLX4's row carries a 12-byte vendor message every 200 ms ([15](15-flx4-midi.md)). Set it to `0` to run the control, and read the log rather than the glass: the send is logged |

`RB_VERBOSE=1` sets the verbose family together: `RB_KNOB_VERBOSE`,
`RB_JOG_VERBOSE`, `RB_TEMPO_VERBOSE` and `RB_LED_VERBOSE` each default to it in
`rb.conf` and can be overridden one at a time (`RB_VERBOSE=1 RB_LED_VERBOSE=0`
is the LED mirror quiet and everything else loud).

## The previous target's map (JP21) — reference

Kept because it is a complete, measured example of every construct
`struct ctrl_map` supports: a 14-bit CC pair, a relative encoder, a channel-assign
note whose *velocity* carries the value, a hold-plus-turn combo, and a
continuous control whose polarity had to be inverted. Read it as the template,
not as the Pi's mapping.

Engine OS defined every one of the SC Live 4's controls in QML:

```
/usr/Engine/AssignmentFiles/PresetAssignmentFiles/JP21/JP21_Controller_Assignments.qml
/usr/Engine/AssignmentFiles/PresetAssignmentFiles/JP21/JP21_Controller_Device.qml
```

`JP21` is the SC Live 4 (internal name `SCX-4`). It uses **channels 4/5 for the
decks** and **15 for the global/FX group**.

### MIDI layout (all channels 0-based)

| Surface | seq channel |
|---|---|
| Global (transport/FX/mix) | 15 |
| Deck Left / Right | **4 / 5** |
| Mixer strips **1 / 2** | 0 / 1 |

The SC Live 4 has **4 mixer strips**, but rbp is a **2-channel** mixer, so only
strips **1/2** are mapped (→ decks 1/2). Strips 3/4 are used for the
master-cue function instead (see below).

### Global (ch 15)

| Control | MIDI | rbp keycode |
|---|---|---|
| LOAD deck 1 / 2 | note 1 / 2 | `0x4311` K_LOAD (deck 1 / 2) |
| BACK | note 3 | `0x420d` K_BACK |
| FWD | note 4 | `0x0201` K_SOURCE |
| Browse knob push | note 6 | `0x420c` K_SELECTOR |
| Browse knob turn | CC 5 | `0x420c` rotate |
| MENU | note 13 | `0x0206` K_MENU — narrow by measurement: it toggles MY SETTINGS on the SOURCE screen and changes nothing (0 px, full frame) on BROWSE, TAG LIST and PERFORMANCE, so a MENU button that "does nothing" is the screen it was pressed on; **held** 400 ms or more it reaches UTILITY from any screen, on rbp's own timer (between 300 and 400 ms, measured). **UTILITY itself is the selector's screen, not the touch screen's** — a finger does nothing there (0 px on three targets, 2026-09-29) while the browse knob and its push drive it ([07](07-touch.md#utility-is-not-a-touch-screen)) |
| VIEW | note 14 | `0x0202` K_BROWSE |
| Crossfader | CC 14 | `0x6017` K_XFADER |
| Main Vol | CC 20 | `g_master_gain` — audioshim, **ch0/1 only** |
| Speaker/booth level | CC 15 | `g_speaker_gain` — audioshim, **ch6/7 (built-in monitors)** |
| Speaker on/off switch | note 41 | `g_speaker_on` — gates ch6/7 |
| Split cue switch | note 11 | `HeadPhone::setStereoType` (0 = split, 1 = stereo) |
| Cue mix | CC 18 | `0x4405` K_HPMIX → rbp `HeadPhone` mix rate |
| Cue level | CC 19 | `0x4406` K_HPLEVEL → rbp headphone level |
| Sound Color FX: DualFilter / DubEcho / Noise / Wash | notes 21–24 | `0x50a6`/`0x50a2`/`0x50a4`/`0x50a3` (both channels) |

### Deck (ch 4 = left → deck 1, ch 5 = right → deck 2)

| Control | MIDI | rbp keycode |
|---|---|---|
| CENSOR → **loop exit** | note 1 | `0x410e` K_RELOOP (exits a running loop; re-enters a stored one) |
| SYNC | note 8 | `0x4112` K_SYNC (LED mirrored back on note 8) |
| CUE | note 9 | `0x4102` K_CUE |
| PLAY / PAUSE | note 10 | `0x4101` K_PLAY |
| Pad mode CUES/STEMS | note 11 | `0x4113` K_HOTCUE |
| Pad mode LOOPS/AUTO | note 12 | `0x4114` K_ALOOP |
| Pad mode ROLL/SAMPLER | note 13 | `0x4115` K_SLIPLOOP |
| Pads 1–8 | notes 15–22 | `0x4117`–`0x411e` |
| Pitch bend − / + | notes 29 / 30 | `0x4107` TEMPO RANGE / `0x4108` MT |
| Jog touch | note 33 | `0x4306` |
| Jog rotate | CC `0x11` hi + `0x31` lo (14-bit) | `0x4305` |
| Key lock | note 34 | `0x4108` K_MT |
| VINYL | note 35 | `0x4104` |
| SLIP | note 36 | `0x4110` |
| Loop in / out | notes 37 / 38 | `0x410c` / `0x410d` |
| Auto loop push / turn | note 39 / CC 32 | `0x4114` |
| Pitch fader | CC `0x1F` hi + `0x4B` lo (14-bit, invert) | `0x4109` K_TEMPO_SLIDER |

### Mixer (strips 1/2 → decks 1/2)

| Control | MIDI | rbp keycode |
|---|---|---|
| TRIM | CC 3 | `0x5019` |
| HI | CC 4 | `0x501a` |
| MID | CC 6 | `0x501b` |
| LOW | CC 8 | `0x501c` |
| Channel fader | CC 14 | `0x501e` |
| Sweep FX knob | CC 11 | `0x509d` K_COLOR |
| **PFL / cue (strips 1/2)** | note 13 | drives `me_set_cue()` (rbp has no PFL keycode); LED mirrored on note 13 |
| **Master cue (strips 3/4)** | note 13 | drives `me_set_master_cue()`; LEDs mirrored |

### DJ FX (global ch 15)

| Control | MIDI | rbp |
|---|---|---|
| FX activate | note 26 | `0x448d` K_BFX (LED = LedStat 48, blinks while active) |
| Wet/dry knob | CC 4 | `0x448f` K_DEPTH |
| **Channel assign** | **note 40**, velocity = position | `0x448c` K_BFXCH |
| **Effect select** | **CC 35**, relative (1 = +1, 127 = −1) | `0x448b` K_BFXTYPE |
| **Time / parameter** | **CC 36**, relative (1 = +1, 127 = −1) | `0x448e` K_TIME |
| **BEAT < / >** | hold **note 25** (TIME-knob push) + turn CC 36 | `0x4490` / `0x4491` |

#### Channel assign

The panel sends the position as the **note-on velocity of note 40**
(`DJFxAssign { turnCC: 40; velocities: [0,1,2,3,127] }` =
`['Channel3','Channel1','Channel2','Channel4','Main']`). rbp's
`djengine::c_str(EnBeatEffectSelectChannel)` gives the target values:

```
0 = PLAYER_0   1 = PLAYER_1   2 = MIC_0   3 = ASSIGN_A
4 = ASSIGN_B   5 = MASTER     6 = AUX
```

So: **Ch1 → 0, Ch2 → 1, Main → 5**. Ch3/Ch4 are inert — rbp has only two
players, so there is nothing to route them to.

**The FLX4's own lever has no Main position** — it is CH1 / CH2 / CH1&CH2 — and
its third position is sent as **5** for the same reason Main is: 5 is the only
value that means both decks at once, and an FLX4 operator reaching for "both" is
asking for what the RX3's Main gives them. See
[15](15-flx4-midi.md#the-map) for the lever's two-note encoding.

#### Effect select

`onEv_BeatEffectType(SW_BFX_TYPE)` is a **14-position switch** whose positions
map to internal effect types:

```
pos:  0  1  2  3  4  5  6  7  8  9 10 11 12 13
type: 6  5 13  7 14  1  4  9 10  2  3 12  8 11
```

rbp's panel encoder is endless, so the shim keeps a cursor over the 14
positions, seeds it from the current effect (`getBeatEffectType()` inverted
through the table above), and sends the position as `K_BFXTYPE`.

#### Time / parameter and BEAT < / >

`onEv_BeatFxTime` → `BeatFxTimeKnob(value, absolute)` is called with
**absolute = false**, so the argument is a **rotation delta**, not a position —
rbp itself steps and clamps it (it adds `value * 13` to the current percent).

`onEv_BeatFxBeat` (the BEAT `<` / `>` buttons) behaves the same way, and the
SC Live 4 has no such buttons — holding the TIME knob's push and turning it
sends them (-1 = halve, +1 = double). SHIFT + TIME works too.

**Op codes matter here.** `ui::Mixer::asEventCode` maps
`0x448e → 0x2014` (any op), but gates the BEAT buttons:

```
0x4490 -> 0x2015   and   0x4491 -> 0x2016   only when (op & 0xf) == 0
```

so they are sent with **OP_PRESS**.

#### The ladder and the level, read back off the unit (2026-10-07)

The four wire forms above were **sent on the unit and read back** out of
`/proc/<pid>/mem`, through rbp's `BeatEffect` object
([07](07-touch.md#rbps-own-values-read-not-inferred)):

```
K_DEPTH  OP_VALUE  CH_GLOBAL  v  v/1023.0f
  cc 100 -> BeatEffect+0x20 = 0.7879   (806/1023)      <- ABSOLUTE
  cc  20 -> BeatEffect+0x20 = 0.1574   (161/1023)          one send lands it,
                                                           one send restores it

K_BEATNEXT (0x4491) / K_BEATPREV (0x4490)  OP_PRESS  d = +1 / -1
  rung +0x44:  5 -> 6 -> 7   and   7 -> 6              <- direction MEASURED,
                                                          one rung per press
```

**`K_DEPTH` is an absolute 10-bit value, not a delta** — the whole `0..1023`
comes down the one arg, so no cursor and no feedback loop. **`K_BEAT*` is
`OP_PRESS`-only and a step** — see the gate above; it is a single rung, so a
held button needs one send per rung.

The fourth wire form, `K_BFX`, is the odd one out and is deliberately **not** in
that table: it is a **toggle**, not a value and not a step, so there is no
value to send and no way to ask for a state. A caller that needs the effect to
be *on* -- the momentary pad does, because a level moved on a switched-off
effect is inaudible -- must therefore **read the effect's state back and ask
again while it disagrees**, bounded, rather than latch what it asked for. That
distinction is the whole of the fix in
[07](07-touch.md#the-press-that-did-not-trigger-and-the-loop-it-bought); a
one-shot toggle and no toggle are the same thing from the caller's side, and
the operator's *"it doesn't seem to remember to trigger when i press until i
turn it on off"* is what told the two apart.

**And the state to read back is TWO words, because one of them lies.** Measured
on the unit 2026-10-07, after the operator's second report that the pad *"doesn't
engage"* on a freshly switched effect:

```
BeatEffectManager +0x50            +0x08 -> BeatEffect*      +0x3c
  getBeatEffectType() @0x89acc       the current object       isBeatEffectOn() @0x89964
  ldr r0,[r0,#80]                    (null-checked)           ldrb r0,[r3,#60]
     0   = the Off state                 BeatEffectPingPong        0   <- a real class, off
     0   = the Off state                 BeatEffectOff            1   <- NOT RUNNING
  after ONE FX SELECT press while the effect is off:
     0                                   BeatEffectOff            1
```

`BeatEffectOff` has every control the pad drives compiled out --
`changeEffectStatusToOn` @0x8b0b8, `changeEffectStatusToOff` @0x8b0b4,
`changeLevelDepthValue` @0x8b0ac and `changeTimeValue` @0x8b0b0 are each a bare
`bx lr` -- so nothing maintains its `+0x3c` and it reads **1** on an effect that
is doing nothing. The pad believed the flag, agreed with itself and sent
nothing at all, which is *"it doesn't engage"*. The fix is the **conjunction**
`+0x50 != 0 && +0x3c != 0` (`fxpad_zone.c`'s `live_on()`); the wire is unchanged.
Note it is a conjunction and not an equivalence: a **cold-started rbp** also
reads type 0, but with a stale *real* object at `+0x08` whose own class maintains
the flag, and that row correctly reads 0. `rbp_abi.h` carries the table.

`+0x44` is an **index into a halving/doubling ladder**, not a count:
`5 = "1 BEAT"`, `6 = "2 BEAT"`, `7 = "4 BEAT"`, with `+0x48`/`+0x4c` = 9/0 the
ends. **rbp recomputes the millisecond figure itself** — the same cell went
`480 msec / 1 BEAT → 960 / 2 BEAT → 1920 / 4 BEAT` at 125.0 BPM — so `+0x24`
is rbp's own derived value and anything drawn over that cell must copy it
rather than compute a second copy. The index is also **not** the position the
hardware button shows: a real BEAT button and an injected key move the same
rung, and the panel repaints from the struct either way.

### Beat-loop knob — experimental

The RX3 has no beat-loop knob (its pads trigger loops). The SC Live 4 knob is
wired behind `BEATLOOP=1` because the safe path depends on rbp's pad mode (see
below). rbp exposes the underlying machinery:

```
PlayerInnards::execAutoBeatLoop(short padIndex, bool)        @0x300c64
  pad index -> size code via a per-mode table, then
  DjEngineIF::setAutoBeatLoop(ch, StAutoBeatLoop{numer,denom}, int, bool, bool)
```

Size tables (base `0x4d3850`, codes resolved to beats). **The numbers below are
as the table stores them, and they are the RECIPROCAL of the size the pad
actually produces** — measured on the unit on 2026-10-01 by reading rbp's own pad
grid off the framebuffer, all eight cells of two independent rows:

| selector | as the table stores it | what the pads actually give (measured) |
|---|---|---|
| pad mode 1, `[this+0x7a] == 0` | 4, 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32 | **1/4, 1/2, 1, 2, 4, 8, 16, 32** |
| pad mode 1, `[this+0x7a] != 0` | 4/3, 1, 2/3, 1/3, 1/5, 1/6, 1/7, 1/9 | (not measured) |
| pad mode 2 (SLIP) | 16, 8, 4, 2, 1, 1/2, 3, 4/3 | **1/16, 1/8, 1/4, 1/2, 1, 2, 1/3, 3/4** |

Both measured rows are the stored row reversed term for term, eight out of eight,
so the pair is almost certainly `StAutoBeatLoop`'s `{numer, denom}` read the other
way round when this table was transcribed — but the *measurement* is what the
right-hand column rests on, not that explanation. **The consequence is the one
that matters:** rbp's fresh AUTO BEAT LOOP bank starts at a **quarter** beat, so a
4-beat loop is **pad 5**, not pad 1 — pad 8 engaged a loop whose on-screen badge
read `32`, which is what ties the labels to the applied size. The SLIP row's
measurement is the same grid the mode button draws (SLIP LOOP cells 1/16 … 3/4),
read the same way.

`ui::PlayerInnards` is **not** reachable via `IUiObjManager::getPlayer()` (that
returns `ui::Player`, channel byte 1/2). It is found by scanning writable
mappings for its vptr, which is **`vtable+8`** (`0x4d1960`), then validating
`+0x26` (channel 2/3), `+0x74` and `+0x30` (engine ptr). (`+0x74` was believed
to be the pad-mode byte; the 2026-09-30 measurement below falsifies that label —
the validation still discriminates the struct, but nothing should read that byte
as the mode.)

The pad keycode (`K_PAD1..8`) is only a beat loop **while rbp is in its
AUTO/LOOPS pad mode**; `execAutoBeatLoop()` called directly from the shim's MIDI
thread does not run the loop. The whole path is therefore gated behind
`BEATLOOP=1`; the reliable workflow is to select rbp's LOOPS pad mode and drive
the pad keycodes from the knob.

On the FLX4 that mode is **one button press away, and the press works** — the
operator's own eyes settled it on 2026-09-30, and their own fingers on 2026-10-01.
**The binding is POSITIONAL and the unit's printed labels are deliberately
ignored.** rbp's UI is an XDJ-RX3's, whose four pad-mode buttons run HOT CUE, BEAT
LOOP, SLIP LOOP, BEAT JUMP — rbp's modes 0, 1, 2, 3 in that order, which is also
how `map_jp21.c` binds the SC Live 4's four. This unit's row is HOT CUE, PAD FX 1,
BEAT JUMP, SAMPLER, so each button selects the mode that occupies **its position**
on the RX3: PAD FX 1 (note 30) stands in for BEAT LOOP, and **the 3rd button (its
BEAT JUMP, note 32) gives SLIP BEAT LOOP while the 4th (its SAMPLER, note 34)
gives BEAT JUMP** — the opposite way round from what those two labels suggest. The
operator asked for exactly this on 2026-10-01: *"ignore the names on the FLX4, it
should just map to the way the RX3 behaves by position for muscle memory"*. The
**pad bases do not move** when the keycodes do: they follow the unit's own labels,
and all four bases this map binds send the same `K_PAD1..8`, because rbp's pad
keycodes mean whatever rbp's current mode says they mean — so what is rewritten is
the base→mode correspondence (base 16 → mode 1, base 32 → mode 2, base 48 → mode
3), not the pad rows.

**rbp does act on every one of those keycodes.** One button per capture, read off
rbp's framebuffer: note 27 puts deck 1's pad grid on `HOT CUE` (A–H), note 30 on
`BEAT LOOP` (1/4 … 32), note 32 on `SLIP LOOP` (1/16 … 3/4) and note 34 on
`BEAT JUMP` — the last two measured 2026-10-01 on the operator's own presses, with
a live MIDI dump as the second witness (vel 127 is the panel's own) and rbp's
pad-mode byte read continuously alongside (`work/padmode.py`). Note **32** is
worth keeping in mind: it is the 3rd mode *button* on ch 0 and pad 1 of base 32 on
ch 7, and only the channel tells the two apart.

What does *not* move is `[this+0x74]`: polled 1.1 M times across all four mode
buttons while the grid verifiably switched, it never left 0, so the earlier
reading of this file — "the mode byte stays put" — was true of a byte that is not
the mode. **The source of truth is `UiGetPadMode(ENUM_DECK)` @ `0xfd3cc`** (see
[15](15-flx4-midi.md)), which needs no scan and no injection. The half still
broken is the **feedback**: the unit's pad-mode LEDs are never driven, because
`struct led_notes` has no field for them, so the mode changes silently, and the
only thing the repair still needs is those four LED note numbers. The measurement
is in S5.7 of [13](13-raspberrypi4.md). What a second press of the *same* button
does is not a no-op: it takes rbp's pad-mode byte to a fifth value, `4`, through
the size-bank path — a stable value and not a corrupt read, but not one of the four
modes either ([15](15-flx4-midi.md#pads-list-ch-810--rch-79)).

### Not mapped (JP21)

* Beat-loop knob — experimental, `BEATLOOP=1` (above).
* Parameter (23/24), Layer (31), StopTime (CC 37), Thru (note 15) — no
  direct rbp keycode, or needs a distinct param.
* SHIFT (note 28 per deck) is tracked and used for the BEAT < / > combo; other
  shift-actions are not wired.
* Pad-mode LEDs (11–13) and pad colours.

## LED output

Transport LEDs mirror the engine: PLAY / CUE / SYNC / KEY LOCK / VINYL / SLIP on
both decks, plus the FX group LEDs. Pad illumination is on where the target has
pads (`RB_LED_PADS=1`); pad **colours** depend on a velocity→colour table that
has not been measured for the FLX4 ([15](15-flx4-midi.md)).

**Which surface a note belongs to is now part of the code, not of this file.**
This section used to present the SC Live 4's note numbers as if they were the
port's, which is exactly the mistake the split removes: the numbers live in the
selected map's `struct led_notes` (`jp21_leds` in `map_jp21.c`, `flx4_leds` in
`map_flx4.c`), and `rbp_led.c` walks whichever table the front end selected. It
holds **no note number at all** — see `ctrl_map.h` ("a map never contains an rbp
address, a bridge never learns a note number") and `ctrl_sel_leds()` in
`ctrlshim.c`, which is the one place that knows the selection.

Every table row is `-1` by default, and `-1` means **this surface has no such
LED** and transmits nothing. That is what makes it safe for `rb.conf` to ship
`RB_LED_DISABLE=0` while the FLX4's notes are still unmeasured: the bridge runs
and sends nothing. A keyboard selection has `leds = NULL`, which is a declared
answer rather than an omission — the panel belongs to the controller, so a
keyboard lights nothing. `test_kbd.c` pins that NULL; `test_flx4.c` pins that no
FLX4 row carries an SC Live 4 `(channel, note)` pair, because a copied number is
not a dark LED but a phantom control press.

### How a controller's LEDs are addressed

MIDI: Note On with the velocity encoding colour/brightness
(`(r<<4)|(g<<2)|b`, 2 bits per channel; `0x7F` = bright for the simple transport
LEDs, **Note On with velocity 0 = dark**). Dark is never sent as a Note Off
message: the FLX4's LED hardware ignores a real Note Off outright (measured
2026-10-01, see [15](15-flx4-midi.md)), so `midi_io.c`'s `midi_note()` builds a
Note On for both edges — which is also the MIDI spec's own spelling of a Note Off,
so it stays correct for the previous target. On the previous target the surface
was rawmidi
`hw:0,0` / seq `16:0`, and the two were interchangeable:

```sh
aplaymidi -p 16:0 ledtest.mid      # note8/9/10 ch4 -> SYNC/CUE/PLAY light up
amidi -p hw:0,0 -S "94 0A 3F"      # PLAY deck1 bright
```

On the Pi's DDJ-FLX4 those rawmidi one-liners **do not work**: `hw:1,0,0` is
EBUSY to userspace because the kernel's `snd_seq_midi` holds the node once the
sequencer attaches it. Drive the panel through the sequencer instead
(`seqinject2`, or `/tmp/ledprobe.sh` on the unit) — which is the same route the
shim's own writes take. See
[15](15-flx4-midi.md#measuring-the-notes-the-route-and-the-loopback-result).

Writing LED MIDI does **not** loop back into the control-surface input path.
That was verified on the previous target with `aseqdump`, and it has now been
re-verified on the FLX4 specifically, because the FLX4 case is the one where the
shim reads and writes the *same* port (`20:0`) — a write there reaches the device
and is delivered neither to the shim's own input port nor to a monitor, while a
real button press demonstrably is. It is safe to do from inside `rbp`.

### The bridge

rbp computes its LED state into `uif::LedStat` and encodes it for the XDJ-RX3's
EUP/SUB micons, sent to `/dev/subucom_spi1.0` — a dead FIFO in this port. The
LED thread therefore polls rbp's own engine state at 20 Hz through the
`playengine::PlayEngine` singleton and mirrors it onto the panel's notes:

| LED | note | source |
|---|---|---|
| SYNC | `n_sync` | **rbp LedStat id 4** (off / solid / blink) |
| CUE | `n_cue` | loaded && !playing |
| PLAY | `n_play` | `isPlaying` → solid; loaded && !playing → blink (at rbp's own period when its LedStat entry carries one, else the 800 ms fallback); else off |
| KEY LOCK | `n_keylock` | `PlayEngine::isMasterTempo(ch)` |
| VINYL | `n_vinyl` | `PlayEngine::isVinylMode(ch)` |
| SLIP | `n_slip` | `PlayEngine::isSlipModeOn(ch)` |
| LOOP IN | `n_loopin` | derived (rbp's loop ids arrive on channel 0) |
| LOOP OUT | `n_loopout` | derived (`isLooping`) |
| AUTO LOOP | `n_autoloop` | `isAutoBeatLoop(ch)` |

The note column names a **field**, not a number, and that is the point: this
table's notes are the SC Live 4's (`8/9/10/34/35/36/37/38/39` on channels 4/5),
the FLX4's are not measured, and a bridge that spelled either of them out would
be wrong for the other surface. Read the number from `map_jp21.c`'s `jp21_leds`
or `map_flx4.c`'s `flx4_leds`.

Deck 1 = PlayEngine channel 0, deck 2 = channel 1 (mirrors the RX3
`EnPlayerChannel`; the JP21 panel MIDI channels are 4/5 — the FLX4's differ).

The output route is the second local sequencer port described above. If the
shim has fallen back to rawmidi it holds that device exclusively instead, and
`amidi`/`aplaymidi` will report "Device or resource busy"; `LED_DISABLE=1`
releases it.

Not yet driven anywhere, because no FLX4 row has been *seen* to light: pad
colours (RGB) on the FLX4, its transport/pad/loop notes, LOAD / browse LEDs, and
the CUE id. Every one of them is `-1` in `flx4_leds` and therefore silent; the
pad-mode LEDs (11–13) and the LOAD/browse LEDs are the same open question. The
loop LEDs are *not* in that list for the JP21 — they are driven there today, and
the FLX4 has no loop section at all, which is the case the `-1` row exists for.

### Global LEDs driven straight from rbp

The LedStat id for the FX group is **`LedDef::ID + 8`**:

| control | field | rbp LED id |
|---|---|---|
| Sound Color FX: Dual Filter | `n_fx[LED_FX_CFX_FILTER]` | 41 (`CfxFilter` 33+8) |
| Sound Color FX: Dub Echo | `n_fx[LED_FX_CFX_DUBECHO]` | 43 (`CfxDubEcho` 35+8) |
| Sound Color FX: Noise | `n_fx[LED_FX_CFX_NOISE]` | 44 (`CfxNoise` 36+8) |
| Sound Color FX: Wash (Sweep) | `n_fx[LED_FX_CFX_SWEEP]` | 42 (`CfxSweep` 34+8) |
| Beat FX ON/OFF | `n_fx[LED_FX_BFX_ONOFF]` | **48** (`EffectOnOff` 40+8) |

The note numbers are `21/22/23/24/26` on channel 15 for the SC Live 4 —
`jp21_leds.fx_ch` and its `n_fx[]`. **The FLX4 has only one of these**: it has a
Beat FX ON/OFF button (`flx4_leds.n_fx[LED_FX_BFX_ONOFF]`, input note 71) and a
single Sound Color FX knob rather than four buttons, so the four Colour FX rows
are permanently `-1` there — the absent case, not a pending one.

These use the same `0` off / `1` solid / `2` blink mapping, so the beat-FX
button blinks exactly while rbp does.

### rbp's own LED state — the translation path

rbp keeps all LEDs in one table, so a shim can mirror it directly:

* Every LED write goes through **two** functions:
  `uif::LedStat::setLedState` and `setLedState_Color`.
* `IUiObjManager::getLedManager()` (0x31deb0) is
  `r3 = *(0x026867c0); r3 = *(r3+104)`, and `LedManager::refStatesNoUpdate()`
  (0x33e3ec) is `add r0,r0,#0x30` — so the `LedStat` is at `LedManager+0x30`.
  Read it live with
  `dd if=/proc/<rbp>/mem bs=1 skip=$((ledstat)) count=16 | od -An -tx4`.
* `LedStat`: `+4` u16 = entry count, `+8` = `Led*` array, `+14` u16 = stride.
  Each `Led` entry is `0x2c` bytes: **`+0` u32 id, `+4` u32 channel,
  `+16` u32 State, `+20` u32 dim/unassigned, `+28` u32 blink period in ms,
  `+40..42` RGB**.

State values: `0` = off, `1` = solid, `2` = blink, `3` = slip-mode dimming
applied to whole groups.

**`+28` is rbp's own blink period, and it is the cadence the shim uses — measured
2026-10-06.** Reading the whole table live out of `/proc/<pid>/mem` (51 entries),
`+28` came back `0` for the 48 entries whose State is not 2, and non-zero for
exactly the three that are 2: **deck 1 PLAY 500 ms, deck 2 PLAY 250 ms, CfxFilter
(id 41) 250 ms**. So the state word says *that* an LED blinks and `+28` says *how
fast*, and a shim that renders a blink needs both. Three things the reading
settled: it is a **full cycle** (half duty is `(now % p) < p / 2`, and the
boundary belongs to the off half); rbp does **not** toggle State to produce the
blink (a 6 s watch showed State-2 entries never change, and an entry caught
starting to blink went `0` → `300` in the same rebuild that set State 2); and the
sibling port decodes the same offset with the same unit, independently
(`Rx3-flx4`, `rx3-handoff/control-shim.c:29` — whose `+20 brightness (0 full,
1 dim)` is the other end of the same fact as this tree's "dim/unassigned", not a
disagreement). The shim's fallback for a blink rbp did *not* ask for is 800 ms
(400 on / 400 off), which is the cadence the hand-rolled `led_tick & 8` used to
give — so only the LEDs rbp actually asks to blink changed speed. Record:
`work/blinkprobe.py`; see also the torn-read filter below. **Confirmed at the wire
2026-10-06**: with a track cued on each deck, the FLX4's deck-1 PLAY note
alternated at a 244.9 ms half-period and deck 2's at 121.6 ms — the same LedStat
id on two decks at two different rates, each matching its own `+28`, which no
single global fallback could produce.

**The table is read twice, 1.5 ms apart, and only entries that agree are used.**
rbp rebuilds `LedStat` every 20 ms, so a single read can straddle a rebuild and
show a State from after with a colour from before. An entry that disagrees with
itself carries the value it had in the previous settled table (matched by
(id, channel), not by position); an entry in flux with no history is absent for
that tick. The read sits behind the `led_disabled || !midi_out_ready()` guard, so
it costs its 1.5 ms only when there is a panel to light, and it logs under
`LED_VERBOSE` when the disagreement count changes.

**`3` is not only that, though — measured 2026-10-01, and it is worth knowing
before reading `3` as slip anything.** On the **pads**, `3` marks the *engaged*
beat-loop pad: in AUTO BEAT LOOP all eight pads of a deck sit at `1` and the one
whose loop is running goes to `3`, moving when the loop moves (pad 5 → id 22,
then pad 7 → id 24 with id 22 back to `1`) and clearing when it is let go. That
is one pad, not a whole group, and there is no slip loop in the picture. So `3`
is best read as **"this control is the active one"** and its exact rendering
decided per surface: the FLX4's deck LEDs count it as on, and its **pads blink**
it (the panel has no brightness — all 350 LED rows are `0x00`/`0x7F` — so
dimmer-than-its-neighbours cannot be sent). Record:
`work/dumps/ledstat-2026-10-01-pad-engaged-loop.txt`, and
[15](15-flx4-midi.md) for the pad semantics.

LedStat id → control map (the `uif::LedDef::ID` numbering used by
`PcControlLedData::LedDefID2Text` does not line up):

| rbp LED id | channel | control |
|---|---|---|
| 3 | 1 | VINYL |
| 4 | 1 | SYNC |
| 5 | 1 | MASTER |
| 6 | 1 | KEY LOCK (master tempo) |
| 10 | 1 | REV |
| 11 | 1 | SLIP |
| 34 | 0 | LOOP IN |
| 53 | 0 | CUE |
| 55 | 0 | PLAY |
| 18–25 | 1/2 | pads / performance row |

Discovery switches: `LED_DUMP=1` (log every `(id,ch,state)` change),
`LED_SWEEP=1` (drive rbp's keycodes and snapshot the table around each one).

## VU meters

### On a target that has meters

The meters are MIDI CCs, and **bitwise**: the value is a bitmask of the segments
lit, from `ledCCValues: [0, 1, 3, 7, 15, 31, 63]` with
`vuLedCCIndexing: Bitwise` (so `0x3F` = all six).

Each meter on the SC Live 4 is **6 LEDs, bottom → top: 4 white, 1 blue,
1 orange**.

| meter | MIDI |
|---|---|
| Master L / R | **CC 32 / CC 33**, channel **15** |
| Channel 1 / 2 | **CC 10**, channels **0 / 1** |

Thresholds: master `[-45.7, -25.7, -12.2, -7.2, -4.2, -0.2, 20]`, channels
`[-45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20]`.

**Master** — when the hook is up this is rbp's **own** master meter, the same
source as the channel meters, with the Main Vol applied in dB; `vu_thread`
converts it to a segment bitmask and sends the master CCs at 40 Hz, L and R
independent. The audio shim's peak is the fallback for when rbp's master meter
is not captured: `audioshim.so` sees the master mix in `snd_pcm_writei()`,
computes a per-channel peak (S24_LE full scale `0xFFFFFF`) with a ~300 ms
release, and publishes it to the shared `g_vu_peak[2]`.

**Channels** — rbp's channel meter is **11 segments**
(`LED_TABLE = (1<<n)-1` for n = 0..11) and is **pre-fader**, while the panel
has 6. `ui::Mixer::MonoLvMeter::getLedValue()` is the single function that
turns a level into the meter bitmask, so the shim patches its prologue
(`install_meter_hook()`; the caller LR identifies master / ch1 / ch2, and the
original runs through an RWX trampoline). `vu_thread` then:

1. takes rbp's lit-segment count (0..11 → uint32, *not* a byte),
2. rescales it to the panel's count — `n_sc = (n_rbp * 6 + 5) / N`, i.e. round
   rbp's 0..N segments onto the 6 the panel has. **`N` is rbp's meter height**
   (`RBP_METER_SEGMENTS`, 11), overridable with **`RB_LED_VU_SEGMENTS`**; the
   `6` is the panel's and is not configurable,
3. applies the **channel-fader taper** so the meter drops with the fader like
   Engine OS (rbp's channel meters are pre-fader; the taper is 60 dB across the
   fader's travel),
4. sends the channel CCs.

The master meter comes from rbp's own master meter rather than the audio shim's
peak whenever the hook is capturing it, with the Main Vol applied in dB, so
master and channels read consistently.

### On the FLX4, whose meter is a value rather than a bitmask

The heading here used to read "which has no meters", and then "whose meter is a
different kind"; **both are wrong, and the second one only described the state
before the bridge was finished.** Pioneer's list documents a **CH LEVEL METER**,
and it is **host-driven**: the host sends **CC 2**, one per deck channel (0 and
1), with a **level** in `0x26`..`0x7F` — below `0x26` is dark and it lights
bottom-up — banded Green1 `0x26`-`0x40`, Green2 `0x41`-`0x56`, Orange1
`0x57`-`0x64`, Orange2 `0x65`-`0x76`, Red `0x77`-`0x7F`. The unit does not send
it. It is **pre-fader**, like rbp's, and it has **no master meter**: the list
gives no second address for one, so `meter_master_ch` stays `-1` and the master
CCs are never written on this target.

**That is a real difference from the SC Live 4, and it is a difference in the
*encoding*, not in whether the bridge works.** The Prime wants a **segment
bitmask** (`(1 << n) - 1`); the FLX4 wants a **level**. The *kind* is read from
the map's own meter row (`meter_enc`), which is exactly the asymmetry this
section used to describe as a gap: the numbers are no longer hard-coded in
`rbp_vu.c` but come from the selected surface's `struct led_notes`
(`meter_ch_first`, `n_meter_cc`, `meter_enc`, `meter_pre_fader`,
`meter_master_ch`), and `METER_ENC_FLX4_LEVEL` anchors each of the six steps at
its own band's bottom so the colour changes land where the document puts them.
The **pre-fader** half matters too: the fader taper the Prime's meters get is
applied only where the surface's own meter is post-fader, which the FLX4's is
not. The band *boundaries* above are still the list's published values — what is
measured is that the wiring carries them.

**It is live, and the operator's own eye settled it (2026-10-01).** `RB_LED_VU`
ships `1`; with `VU_TEST=1` sweeping the steps, the operator confirmed both
channel meters stepping on the panel — which is the only thing that settles a
meter, since a successful `midi_cc()` proves the message left the shim and
nothing more. The startup line names the whole row: `VU bridge up: channels CC 2
on ch 0/1 pre-fader, master CC -1/-1 on ch -1 (absent)`. `VU_TEST`/`VU_DEBUG` are
diagnostic overrides of the *output* only.

**What `LED_VU=0` actually means now.** It is a **manual OFF switch** on the meter
bridge, not a statement about the target: a surface whose map declares **no meter
row** gets no hook installed and no traffic whatever the flag says, and the FLX4's
map declares one. So where the hook *is* installed the patch is up, and where it
is not the reason is the map and not this unit. What it deliberately does **not**
skip is engine setup that has nothing
to do with meters: `vu_thread` still waits for rbp's mixer (`mixer_engine()`,
which is how it knows the engine exists at all) and asserts the **master cue**
and **stereo cue mode**, then returns. That is engine setup rather than meter
work, so a surface with no meter row must still get it, and it belongs to the
thread
that waits for the mixer rather than to a map: rbp has no PFL keycode, and on
the FLX4 — which *does* have a MASTER CUE button, bound in the map — a startup
assertion from the map would be a second writer fighting the operator for the
same engine state.

## Key gestures added by the port

Gestures a target has that the RX3 does not (or vice versa). They live in the
map — there is no `struct ctrl_map` member for a gesture, because a gesture is
just an `event()` handler that sends a keycode rbp already understands:

| Gesture | Sends | Why |
|---|---|---|
| **Hold SYNC** (≥ 600 ms) | `0x4111` K_MASTER | the SC Live 4 has no MASTER button. Fires at the threshold, not on release. Tap = normal SYNC. |
| **CENSOR** | `0x410e` K_RELOOP | reverse/censor is unused here; it makes a useful loop exit |
| **TIME push + turn** | `0x4490` / `0x4491` | RX3's BEAT `<` / `>` buttons, absent on the SC Live 4 |

The SYNC hold sends nothing until the threshold is reached; sending the press
early and suppressing the release would leave rbp with a stuck SYNC press and
trigger its long-press action (**instant double**).

## The QUANTIZE tap — a keycode rbp already has

One gesture in this tree is not a map's. A press on either deck's on-screen
QUANTIZE box is turned into `0x410b` `K_QUANTIZE` by the **pointer** path
(`pointsrc.c` → `touch_zone.c` → `rbp_key.c`), because that widget is the one
part of the performance screen rbp draws and does not bind to a touch of its own.
The keycode, the binary reading it comes from, and the rectangle — with the one
screen where that rectangle is already rbp's — are in
[07 — Touch / pointing](07-touch.md#the-two-deck-quantize-boxes-touch_zonec).
What belongs here is why it is a keycode and not an engine call.

`PlayerInnards::onKey_Quantize` @ `0x3028bc` never looks at the keycode: it takes
the deck from its own channel, flips
`DjEngineIF::setDeckQuantizing(ch-1, !isDeckQuantizing(ch-1))`, and then calls
into `IPlayerSetting` — and that last call is what repaints the widget. The
engine-only route (the first one considered) does the audio half and leaves the
screen showing the old state, so the operator's next look at the box would lie.
Sending the key runs rbp's own path on the same channel numbering a hardware
press uses — 1-based, as `map_flx4.c:401`'s `ch + 1` — so the two are the same
gesture. A second consequence of that: it is a *toggle* rbp computes from its own
state, so nothing in the shim tries to track the value.

**The key goes out before the touch does**, and that ordering is not cosmetic.
`IKeyManager::sendKey` @ `0x37ad64` fills **one** `IKeyInput` — the pointer at
`KeyManager+132` — and only then walks its listener list, handing every listener
that same mutable struct; rbp's own `TouchPanel` is a key source too, and it is
the thread that will run for the report we are about to emit. So a key sent after
the emit is a key whose keycode a concurrent writer can overwrite before the
deck's `PlayerInnards` reads it. Sending first means rbp has not yet been told
about this gesture when the send completes. The structural reading is what the
ordering rests on: the two runs that motivated it (2/8 and 7/10 taps) were taken
with an instrument that never delivered an x, so they are not evidence — see
`pointsrc.c`'s comment on `quantize_tap` and
[07](07-touch.md#the-position-the-reader-starts-from-seed_abs_position). What is
measured with that fixed: **20 taps on deck 1 and 20 on deck 2 all flipping the
deck's own quantize flag on the press, none on the release, a 10-tap control
outside the rectangle flipping none, and a frame diff either side of a single tap
changing 34 rows inside that deck's widget and nothing anywhere else** — 830 px
of a 1280×800 frame on deck 1, 1038 px on deck 2.

This is the **second** place in the tree where `fbshim.so` reaches into rbp, and
it is why the key path is its own object: `rbp_key.{h,c}` is a leaf (`rbp_abi.h`
and libc, nothing else) that both shims link, and `rbp_key.o` is compiled with
**hidden** visibility so that fbshim does not export `send_rx_key` — fbshim is
first in `LD_PRELOAD`, so a default-visibility copy there is the one knobshim's
maps would bind to. See the `FBSHIM_OBJS` comment in `scripts/shims/Makefile` and
the header of `rbp_key.h`.

## The safe-eject button — a keycode rbp already has, on three edges

`USB STOP` is the seventh column of the swipe-down top menu
([07](07-touch.md#the-swipe-down-top-menu-menu_zonec-menu_drawc)), and the reason it
is there is that **this rig has no other way to reach rbp's own safe eject**. The
FLX4 has no such button; the keycode `0x8002` appears nowhere in the MIDI maps, the
keyboard map's whitelist cannot carry it, and rbp's name for it — `UsbStop`, from
`ui::KeyInput::keyCodeAsText()` — is the last entry of a 173-slot name table. The
operator asked for it in exactly those terms: *"rekordbox has a stop button just for
that purpose to safe eject, but its not mapped currently, so i really don't want to
pull the USB as it might continue to corrupt the USB stick"*.

| | |
|---|---|
| **Keycode** | `0x8002` `K_USBSTOP` — `CH_GLOBAL` is USB 1's number (1) by coincidence; the chooser sends the **chosen device's own channel**, because the handler drops a key whose channel is not its own |
| **Reached by** | `UsbStorageManager::onKey` (entry **[2]** of the live handler table) → `ui::UsbStorageManager::onUsbStopKey` @ `0x325788` |
| **Sent as** | **press + repeat + release**, back to back |

**The three edges do three different things, and that is the whole finding.** The
handler branches on the operation nibble of the key record, and only the middle one
asks for the eject:

| op | branch | what it does |
|---|---|---|
| 0 PRESS | `0x325828` (needs `[this+0x88] == 2`, i.e. media present) | `[this+0x8c] = 5`, `PlayerSkeleton::mute(true)` on both decks, `notifyMediaDisconnect(true)` ×2, `DbProxy::notifyMediaDisconnect` |
| **1 REPEAT** | **`0x32588c`** | sets bit 7 of `[this+0x3d0]`; if `[0x88] ∈ {1,2}` builds an 800-byte `IDataSet` and calls **`UsbStorageManager::request_usb_stop` @ `0x324578`** — *this is the eject request* |
| 2 RELEASE | `0x3257d0` | clears bit 7, unmutes both decks, resets `[0x8c]` to 0, 2 or 4 |

So a plain press+release **tap is a silent no-op**, which is what the first attempt
at this binding shipped and what the measurement below corrected. It is the same
shape as SHIFT + LOAD 1's `K_TRACKFILTER` ([15](15-flx4-midi.md)) — rbp's handler
reads the record's *state*, not which edge carried it — but it is not the same
degree: TRACK FILTER needs *a* second edge, USB STOP needs **both** the press and
the repeat, because the press does the muting half and the repeat does the eject
half.

**Measured live, 2026-10-01, through the real dispatch** (bench route: the panel's
`SOURCE` column repointed at `0x8002` with `systemctl set-environment
POINT_MENU_KEY_EXTRA=0x8002`, then held for 500 ms, sampling `UsbStorageManager` at
1 kHz over `/proc/<pid>/mem`):

```
t=7.072  (op=0, [usb1+0x8c]=5, [usb1+0x88]=2)   <- PRESS: the body runs
t=7.572  (op=2, [usb1+0x8c]=0, [usb1+0x88]=2)   <- RELEASE: unwinds it
```

`[usb1+0x8c]` reading **5 for exactly the 500 ms the key was held** is the proof the
handler body ran; the release clearing it microseconds later is why a *tap* looks
like nothing at all. Every other gate was checked and passes: `[km+0xa4] == 0`, the
handler entry's mask is `0x8000`, `isOtherKey()` is `isCategory(0x8000)` and true for
`0x8002`, and `[this+0x84]` is the input channel. (An earlier session concluded from
a coarser sampler that "the press body never runs" — that was a sampling artifact,
and the note is written where it was made.) `POINT_MENU_KEY_EXTRA` cannot send an
OP_REPEAT at all, which is why *this* binding needed a build to test and why the
hold in that trace went out as press → release with no repeat.

**Pressed on the glass, twice, by the operator — 2026-10-01, and it works.** Both
presses produced the same chain, read off the unit's own logs:

| | press 1 | press 2 |
|---|---|---|
| `pointsrc` | `menu 'USB STOP' tapped (button 7) -> key 0x8002` | the same |
| **the screen** | **the SOURCE screen cleared** — the operator's report, and the half no log line carries | — |
| stick off the bus | `09:55:24` | `10:32:2x` |
| `usb-watch` | `detach: notifying rbp` the same second, `umount -l` on both mounts a second later | the same |
| stick back, remounted, re-bound | `09:55:30` | `10:32:31` |
| rbp re-imported | `attach: rbp opened export.pdb` at `09:55:48` | `10:32:49` |

So rbp reacts to the stop — the media goes off its screen — and it comes back on its
own after a replug: **about 20 s end to end, no restart, no residue**. That is the
feature doing what it was built for.

**Two things stay distinct, and it is worth being exact about which is which.** The
button *stops* the media in rbp; it does not release the host mount. A Pi has no port
power switch, so on this rig the mounts are dropped by `usb-watch.sh` only once the
kernel sees the device leave — one second *after* the physical pull, both lazily. The
operator still does the pulling; the button is what makes that pull safe.

**And the release-following-repeat question is still open** — these two cycles cannot
answer it, because the stick was taken out by hand rather than by rbp. The release at
`0x3257d0` always ends in the unmute path (`0x325970`), and whether it *cancels* the
eject or the db unmount proceeds asynchronously once `request_usb_stop` has been
issued needs one press with nothing pulled afterwards: if rbp's media state returns to
present on its own, the release did not cancel it; if it stays stopped until a replug,
the fix is a gap between the edges — `pointsrc.c`'s hold path is the shape with one,
and it is a one-line change. The binding sends the release either way rather than
abandoning the key, because the press path arms a `UiTimer` on the key's own record
(`IKeyManager::onKey` @ `0x37b6cc`) and a key left down would leave that record live.

**Do not try to answer that from `/proc/<pid>/fd`.** The symlink timestamps there are
when the entry was first *looked at*, not when the fd was opened — measured 2026-10-01
with a shell whose fd 0 and a fd opened three seconds later both stamped identically —
so `usb-watch`'s `rbp opened export.pdb` check passes on a stale handle exactly as
happily as on a fresh one, and rbp's own media fds read `09:40` across a `09:55`
eject/remount. The grip question belongs to rbp's state (`[usb1+0x88]`, `[+0x8c]`,
bit 7 of `[+0x3d0]`), not to the fd table.

**A press on this column stops the media, so it is not a harmless button.** That is
not a caveat about the implementation, it is the feature — it exists so the operator
never has to pull live media — but it is worth saying plainly in the docs because
every other column of that panel is safe to press.

**And since 2026-10-05 the column does not send the stop at all.** It *raises a
chooser* ([07](07-touch.md#the-seventh-column-raises-a-chooser-prompt_zonec-prompt_paintc)),
and the eject leaves on the **chosen device's own channel** — `usb_stop_send(ch)` in
`pointsrc.c`, channel 1 for USB 1 and 2 for USB 2. That channel is not decoration:
`UsbStorageManager::onKey` @ `0x3259f4` drops a key whose channel is not its own, so the
old `CH_GLOBAL` spelling could only ever have reached the first device.
**Since 2026-10-07 it also takes a three-second hold** — the box puts up `HOLD USB 1` and
`HOLD USB 2` (or the stick's own volume label in place of the number, since that day —
[10](10-usb.md)), lit for a device rbp reports present; the press flashes the button, a hold
of 3000 ms or more stops that device, and a release before the third second does **nothing
at all**. So the two `menu 'USB STOP' tapped (button 7) -> key 0x8002` lines above are the
pre-2026-10-05 build; the log reads `pointsrc: menu 'USB STOP' -> the USB STOP chooser
(usb 1 ready 'HOLD RBOX USB', …)` when the box comes up, and then on a completed hold
`pointsrc: usb stop chooser -> usb 1 held 3120 ms (hold is 3000 ms) -> eject (channel 1)`.
Everything else in this section — the three edges, the press-mutes/repeat-ejects split,
the `20 s` replug cycle, and that the button stops rbp's media without releasing the host
mount — is unchanged and still the reason the column exists
([10](10-usb.md#notes)).

## The edge drawers' transports, sync, nudge and fader — keycodes rbp already has

The side drawers (`07-touch.md`) send five keycodes, and none of them needs a map, a
controller or a byte of MIDI:

| Drawer control | Sends | Notes |
|---|---|---|
| **SYNC** | `K_SYNC 0x4112`, press + release | on the side's channel: 1 = left/deck 1, 2 = right/deck 2 |
| **−/+ nudge** | `K_JOG_ROT 0x4305` with **`OP_ROTATE`** and a signed rev/s | see below — the *only* bend mechanism rbp has |
| **CUE** | `K_CUE 0x4102`, press + release | same channel argument |
| **PLAY** | `K_PLAY 0x4101`, press + release | same channel argument |
| **fader** | `K_FADER 0x501e` via `send_rx_key_f(..., OP_VALUE, ch, v, v/1023.0f)` | **absolute**, `v` 0..1023, top = full |

SYNC, CUE and PLAY are plain press/release with **no `usleep`** — unlike the band's own
buttons, they never enter the synthesized-hold path, because rbp wants an edge and not
a dwell.

### SYNC, CUE and PLAY are drawn from rbp's own state, lit and flashing

The three are not just buttons. Each also carries the **deck's real transport state**,
so a machine with no controller attached — the machine these drawers exist for — still
shows what both decks are doing. They are drawn from `rbp_led_transport()`
(`rbp_led.h`), which is the *same reading* the DDJ-FLX4's own SYNC/CUE/PLAY notes are
sent: one decoder, two surfaces, so the drawer and the hardware cannot drift apart.

| Drawer button | Where the state comes from | Lit when |
|---|---|---|
| **SYNC** | `rbp LedStat` **id 4** on the deck's channel, the same three states as the FLX4 (table above) | rbp says solid (locked); **flashes** when rbp says blink — synced but nudged off beat — at *rbp's* period |
| **CUE** | `loaded && !playing` | a track is loaded and the deck is paused on its cue |
| **PLAY** | `isPlaying` → solid; `loaded && !playing` → **flashes**; else dark | the deck is playing; the flash is the paused-on-a-loaded-track state, at rbp's own period (500 ms deck 1 / 250 ms deck 2) |

**A flash is not a third appearance, and nothing here keeps a clock.** rbp does not
toggle a blinking LED's `State` — it sets `State = 2` and leaves it, with the period in
the same entry — so `rbp_led.c` resolves the phase against rbp's own period *at the
instant of the read*, once per 20 Hz LED tick, and publishes one packed int. The drawer
therefore flashes at rbp's cadence and **on the very same value the controller is sent**;
the note goes out and the drawer's repaint picks it up within a tick, at most 50 ms
behind. Handing two painters a "blinking" flag instead would flash them at the same rate
and at whatever phase each one's own clock happened to be in — the two would agree about
the rate and disagree about the moment.

That packed int is part of the drawer's repaint gate beside the pressed control and the
fader value, so **a blink repaints and a steady state does not** — an idle drawer still
costs nothing, which is what keeps the single-buffered plane from tearing.

A **finger on a lit button still wins**: the press's own accent is drawn over the state,
because a touch surface's feedback is the one thing a state must never swallow.

The three travel from `rbp_led.c` whether or not a controller is attached, so a unit
with no FLX4 is not dark just where it matters — and `LED_DISABLE`, which names the
*panel*, no longer stops that read (`rbp_led.c`).

**The nudge is not a keycode and there is no macro that would make it one.** `0x4305` is
the jog's rotation, reached through `send_rx_key_fl(K_JOG_ROT, OP_ROTATE, ch, 0,
speed /*rev/s, clamped to ±8*/, (long)vpos)`, and it is the same call the FLX4's own jog
makes (CC `0x11`/`0x31`, above). Two properties decide the whole design:

* **rbp keeps bending at the last speed until it is told zero.** So the drawer sends the
  speed once on the down edge — the hold time *is* the bend — and a **stop** (speed
  `0.00`) on every path where the finger leaves, including the two where no release
  report arrives at all: the device going away, and `POINT_MENU=0` at startup. A missing
  stop is a track that runs away, which is why `side_nudging()` derives the truth from
  the press state rather than trusting an edge to arrive.
* **The speed is a rate, not a step**, so a tap nudges by however long it was held.
  Default `0.35` rev/s, exposed as `SIDE_NUDGE_SPEED` for calibration on the glass.

The fader is the same call the FLX4's own fader makes, so a two-deck machine with no
controller attached has a working channel fader again — and, since rbp builds its
channel faders at **zero**, a way to open a channel at all. It is ten bits rather than
the FLX4's 128 steps.

`g_fader[ch]` / `g_fader_seen[ch]` (defined once in `fader_state.c`, shared by both
shims — see `07-touch.md` for why that matters) are written on every send, so the
absolute-control query below stops seeding a channel the moment a drawer touches it.

## Keycode → rbp, and the absolute-control query

At startup `vu_thread` calls `led_query_absolute()` to ask the surface to report
every fader/knob position, instead of waiting for the operator to move each one.
The message is Engine OS's **absolute-control query** sysex
(`F0 00 02 0B 7F 12 04 00 00 F7`, from `JP21_Controller_Device.qml`
`queryAbsoluteControls()`), so this is a **JP21 protocol feature, not a
`struct ctrl_map` member** — there is no query hook for a map to implement. It
first waits until rbp's `KeyManager` exists (up to 30 s), then re-asserts the
positions every 2 s for the first **30 s** (deliberately short, so it does not
fight the user), and after that only while a channel fader has still never been
seen. On a surface that does not speak JP21 the query is simply not sent — the
sequencer route cannot carry it, and the log says so once — and its controls are
simply not absolute until moved.

Two implementation notes that carry over:

1. rbp finishes initialising its mixer *after* the shim is loaded, so a value
   sent too early is overwritten and the fader reads "down" until moved once.
2. `handle_cc_abs()` only sends on change, so a repeat query returns the *same*
   values and would be dropped — `led_query_absolute()` therefore invalidates
   the CC cache (`abs_map[].last = -1`) before asking.

## Build

```sh
# on a Linux host (soft-float ARM cross-compile, GLIBC_2.4-only)
make -C scripts/shims RX3=/path/to/extracted/XDJRX3-rootfs knobshim.so
```

Deploy: `scp knobshim.so root@<device>:/opt/rblive4/knobshim.so`, then restart
rbp (or run `start-rb.sh`).

## Verify

```sh
# on device: KNOB_VERBOSE=1 logs every event + keycode
cat /tmp/knobshim.log
#   knobshim2: ch4 note10 -> 0x4101 press (sch1)   <-- PLAY on deck 1
```

The `knobshim2:` log tag is the one artefact of the old single-file shim that is
deliberately still unchanged: the split into modules has to be verified by
diffing a `KNOB_VERBOSE` log against the pre-split build, and renaming the tag
would remove the anchor that comparison rests on. It is renamed once that
verification has passed, not before.

For behaviour that has nothing to do with which controller is attached, the
map's own fixture test is the check to run:

```sh
make -C scripts/shims test      # needs qemu-arm in PATH
```
