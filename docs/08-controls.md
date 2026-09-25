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
| `rbp_vu.c` / `.h` | the meter bit maths and the segment rescale |
| `midi_io.c` / `.h` | the sequencer in, and the surface's LED/meter port out |
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
triple. So `RB_MIDI_MAP=kbd` makes `ctrlshim.c` start the evdev reader
(`evdev_io.c`) and hand every triple to the map, and every other map — `jp21`
included — starts nothing and behaves exactly as it did before the pair
existed. `input()` gets the kernel's numbers, not a keyboard-specific hook,
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

`RB_MIDI_MAP` selects one (`flx4`, `jp21`, `kbd`). It defaults to `flx4`, both
in `rb.conf` and in the source, and an unknown name falls back to `flx4` loudly
rather than silently — the fallback is a log line and not a mystery, and the
previous target's map is one word away.

## The keyboard map (`RB_MIDI_MAP=kbd`)

The surface that needs nothing plugged in (`map_kbd.c`). It exists so display,
audio and the USB import can be brought up and played with on a Pi whose USB bus
is empty, and so a controller that has not arrived cannot block the rest of the
port.

| Key | rbp keycode | Channel |
|---|---|---|
| `space` / `z` / `x` | `0x4101` K_PLAY / `0x4102` K_CUE / `0x4112` K_SYNC | deck 1 |
| `n` / `m` / `,` | the same three | deck 2 |
| `1` / `2` | `0x4311` K_LOAD | deck 1 / 2 |
| `↑` / `↓` | `0x420c` K_SELECTOR, rotate ±1 (repeats while held) | global |
| `Enter` | `0x420c` K_SELECTOR, press/release | global |
| `Backspace`, mouse right button | `0x420d` K_BACK | global |
| `Esc` | `0x0201` K_SOURCE | global |
| mouse wheel | `0x420c` K_SELECTOR, rotate ±1 per notch | global |

The second deck mirrors the first on the same keycodes with the deck in the
*channel*, which is how the JP21 map drives both decks from one table. rbp has no
per-deck variants of the selector, BACK or SOURCE, so those three are global on
both — `CH_GLOBAL`, which is the channel rbp's own browse/source keys are sent on.

It is not a degraded version of the FLX4 map:

* **No MIDI at all.** `devices()` returns 1, so `ctrlshim.c` starts the evdev
  reader and this map gets no `event()` and no `tick()`. `/dev/snd/seq` may be
  missing entirely and the keyboard still works — the log says so loudly rather
  than the shim giving up, which is the one behaviour that differs from a
  MIDI-only map.
* **No panel.** There is no keyboard LED and no mouse meter, so there is nothing
  for `rbp_led.c`/`rbp_vu.c` to drive. (`RB_LED_VU=0` is the setting for a
  meterless target; the master-cue assertion `vu_thread` makes is unrelated and
  still runs.)
* **No rbp-side startup of its own.** `startup()` logs and returns: the JP21
  map's routing and Sound Color FX defaults exist for a surface with DECK/LINE
  switches and a Sweep knob, and the master level at unity is re-asserted for
  every map by `vu_thread()` in `rbp_vu.c`.

`KBD_DEV=/dev/input/eventN` pins the reader to one node instead of scanning;
empty (the default) discovers every node that can report key events, which is
the keyboard *and* the mouse — that is where `BTN_RIGHT` and the wheel come
from. **With `KBD_DEV` set, the mouse is not read**, so both of its bindings go
away; pin a node only to separate two devices that are being confused.

The event source is `evdev_io.c`, and it deliberately knows no keycode: it hands
the map the raw triple and nothing else. Two readers of one evdev node both get
every event, so this coexists with the pointer path in `fbshim.so`
([07 — Touch](07-touch.md)) without either noticing the other.

Start-up, and the per-event trace under `KNOB_VERBOSE=1`, are both one place to
read:

```
knobshim2: map 'kbd': 1 non-MIDI source(s) wanted; evdev reader started
knobshim2: evdev: /dev/input/event0 'AT Translated Set 2 keyboard'
knobshim2: evdev: /dev/input/event2 'Logitech USB Optical Mouse'
knobshim2: kbd: evdev type=1 code=57 value=1 -> 0x4101 press
knobshim2: kbd: evdev type=1 code=57 value=2 -> 0x4101 ignored   <-- autorepeat
```

The node list is logged whether or not `KNOB_VERBOSE` is on, because "which
device did it open" is the question that matters when nothing works.

**Status: written, cross-compiled and fixture-tested — never run with real input
devices.** The bindings above are asserted against synthetic evdev triples in
`make -C scripts/shims test` (`test_kbd`, ~300 checks), which pins the deck
channels, the wheel's direction, the right-button, the press/autorepeat/release
edges and the rotation clamp. What that cannot check is the two things only a
device can: that `evdev_io.c` opens and reads the right nodes, and whether rbp's
list scrolls *up* for `↑` or for `↓`. The wheel and the arrows agree with each
other and with the direction the JP21 map's knob sends for a clockwise turn; if
the list turns out to be inverted on hardware, it is three sign flips in
`map_kbd.c`'s table (`↑`, `↓` and the wheel) and nothing else.

## Finding the controller

Input is never hardcoded to a device node. The shim walks the ALSA sequencer's
client and port lists with `SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT` /
`SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT`, matches the port name against
`RB_MIDI_IN_MATCH` (a case-insensitive substring, default `FLX4`), requires the
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

Not every surface goes through the sequencer, though: `RB_MIDI_MAP=kbd` gets its
events from `/dev/input/event*` instead (see
[above](#the-keyboard-map-rb_midi_mapkbd)), and for that map a missing
`/dev/snd/seq` is not fatal — the failure is logged loudly and the keyboard keeps
working, with no MIDI input and no LED/meter output until the module is loaded.
`fix-dev.sh` is still the answer; there is no retry of the sequencer inside rbp.

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
| `KNOB_VERBOSE=1` | every MIDI event + resulting keycode in `/tmp/knobshim.log`, and every evdev triple with the keycode it produced (or that it was unmapped) |
| `JOG_VERBOSE=1` / `TEMPO_VERBOSE=1` | raw and normalised jog / pitch values side by side |
| `LED_VERBOSE=1` | log every LED change (`led deckN noteM on/off`) |
| `LED_DISABLE=1` | do not drive the panel LEDs at all |
| `LED_VU=0` | skip the meter hook and its sends entirely (the default; the master-cue assertion still runs — see the VU section) |
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
| MENU | note 13 | `0x0206` K_MENU |
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

### Beat-loop knob — experimental

The RX3 has no beat-loop knob (its pads trigger loops). The SC Live 4 knob is
wired behind `BEATLOOP=1` because the safe path depends on rbp's pad mode (see
below). rbp exposes the underlying machinery:

```
PlayerInnards::execAutoBeatLoop(short padIndex, bool)        @0x300c64
  pad index -> size code via a per-mode table, then
  DjEngineIF::setAutoBeatLoop(ch, StAutoBeatLoop{numer,denom}, int, bool, bool)
```

Size tables (base `0x4d3850`, codes resolved to beats):

| selector | 8 sizes |
|---|---|
| pad mode 1, `[this+0x7a] == 0` | **4, 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32** |
| pad mode 1, `[this+0x7a] != 0` | 4/3, 1, 2/3, 1/3, 1/5, 1/6, 1/7, 1/9 |
| pad mode 2 (SLIP) | 16, 8, 4, 2, 1, 1/2, 3, 4/3 |

`ui::PlayerInnards` is **not** reachable via `IUiObjManager::getPlayer()` (that
returns `ui::Player`, channel byte 1/2). It is found by scanning writable
mappings for its vptr, which is **`vtable+8`** (`0x4d1960`), then validating
`+0x26` (channel 2/3), `+0x74` (pad mode) and `+0x30` (engine ptr).

The pad keycode (`K_PAD1..8`) is only a beat loop **while rbp is in its
AUTO/LOOPS pad mode**; `execAutoBeatLoop()` called directly from the shim's MIDI
thread does not run the loop. The whole path is therefore gated behind
`BEATLOOP=1`; the reliable workflow is to select rbp's LOOPS pad mode and drive
the pad keycodes from the knob.

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

**None of that is in force on the Pi yet.** The notes below are the previous
target's, and the FLX4's own LED notes are largely not published, so
`rb.conf` ships `RB_LED_DISABLE=1` for this target as an interim setting — pad
illumination included, since `LED_DISABLE` is the switch that covers them. The
FLX4's LED table, and the reasons the bridge cannot be guessed, are in
[15 — DDJ-FLX4 MIDI](15-flx4-midi.md#the-leds).

### How a controller's LEDs are addressed

MIDI: Note On/Note Off with the velocity encoding colour/brightness
(`(r<<4)|(g<<2)|b`, 2 bits per channel; `0x7F` = bright for the simple transport
LEDs, Note Off = dark). On the previous target the surface was rawmidi
`hw:0,0` / seq `16:0`, and the two were interchangeable:

```sh
aplaymidi -p 16:0 ledtest.mid      # note8/9/10 ch4 -> SYNC/CUE/PLAY light up
amidi -p hw:0,0 -S "94 0A 3F"      # PLAY deck1 bright
```

Writing LED MIDI does **not** loop back into the control-surface input path on
those units (verified with `aseqdump`), so it is safe to do from inside `rbp`.

### The bridge

rbp computes its LED state into `uif::LedStat` and encodes it for the XDJ-RX3's
EUP/SUB micons, sent to `/dev/subucom_spi1.0` — a dead FIFO in this port. The
LED thread therefore polls rbp's own engine state at 20 Hz through the
`playengine::PlayEngine` singleton and mirrors it onto the panel's notes:

| LED | note (ch 4/5, JP21) | source |
|---|---|---|
| SYNC | 8 | **rbp LedStat id 4** (off / solid / blink) |
| CUE | 9 | loaded && !playing |
| PLAY | 10 | `isPlaying` → solid; loaded && !playing → blink; else off |
| KEY LOCK | 34 | `PlayEngine::isMasterTempo(ch)` |
| VINYL | 35 | `PlayEngine::isVinylMode(ch)` |
| SLIP | 36 | `PlayEngine::isSlipModeOn(ch)` |
| LOOP IN | 37 | derived (rbp's loop ids arrive on channel 0) |
| LOOP OUT | 38 | derived (`isLooping`) |
| AUTO LOOP | 39 | `isAutoBeatLoop(ch)` |

Deck 1 = PlayEngine channel 0, deck 2 = channel 1 (mirrors the RX3
`EnPlayerChannel`; the JP21 panel MIDI channels are 4/5 — the FLX4's differ).

The output route is the second local sequencer port described above. If the
shim has fallen back to rawmidi it holds that device exclusively instead, and
`amidi`/`aplaymidi` will report "Device or resource busy"; `LED_DISABLE=1`
releases it.

Not yet driven anywhere: pad colours (RGB) on the FLX4, pad-mode LEDs (11–13),
LOAD / browse LEDs, CUE id, and the loop LEDs.

### Global LEDs driven straight from rbp

The LedStat id for the FX group is **`LedDef::ID + 8`**:

| control | panel note (ch 15, JP21) | rbp LED id |
|---|---|---|
| Sound Color FX: Dual Filter | 21 | 41 (`CfxFilter` 33+8) |
| Sound Color FX: Dub Echo | 22 | 43 (`CfxDubEcho` 35+8) |
| Sound Color FX: Noise | 23 | 44 (`CfxNoise` 36+8) |
| Sound Color FX: Wash (Sweep) | 24 | 42 (`CfxSweep` 34+8) |
| Beat FX ON/OFF | 26 | **48** (`EffectOnOff` 40+8) |

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
  `+16` u32 State**.

State values: `0` = off, `1` = solid, `2` = blink, `3` = slip-mode dimming
applied to whole groups.

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

### On the FLX4, which has no meters

`RB_LED_VU=0` (the shim's own default is on, because the SC Live 4 has meters),
and that does more than silence output: `install_meter_hook()` patches rbp's
**machine code**, so with no meters on the target the patch is **not installed
at all** — no `MonoLvMeter` prologue hook, no trampoline, no meter CCs, and
therefore no `led_query_absolute()` either. Nothing polls rbp's meters when
there is nothing to display them on. `VU_TEST`/`VU_DEBUG` are diagnostic
overrides of the *output* only; they do not bring the hook back.

What `LED_VU=0` deliberately does **not** skip is engine setup that has nothing
to do with meters: `vu_thread` still waits for rbp's mixer (`mixer_engine()`,
which is how it knows the engine exists at all) and asserts the **master cue**
and **stereo cue mode**, then returns. That is engine setup rather than meter
work, so the meterless target must still get it, and it belongs to the thread
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
