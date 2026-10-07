# 15 — DDJ-FLX4 MIDI

The DDJ-FLX4 is the control surface for the Pi target, and
`scripts/shims/map_flx4.c` is its map. This document is two things: the **record
of what that map says the unit sends** (with the source of every number and an
explicit mark on the ones nobody has confirmed), and the **runbook for turning a
dump from the hardware into a correction of it**.

The distinction it is built around is between *measured* and *published*.
Nothing in this repository has ever been run with an FLX4 attached. Every FLX4
number in `map_flx4.c` comes from Pioneer DJ / AlphaTheta's published **DDJ-FLX4
"List of MIDI messages", Ver 1.0** (the PDF on the unit's support page), and is
therefore an expectation about the unit rather than an observation of it:

> **State.** The tables are written, cross-compiled and fixture-tested
> (`scripts/shims/test_flx4.c`, run by `make test`), and the tests pass — which
> says the map dispatches what it says it dispatches, not that the unit sends
> what the map expects. Every value below is `TODO: unverified` in this sense
> until a dump from the hardware replaces it. A dump is the only thing that
> makes any number here evidence.

For the control surface in general — the bridge into `rbp`, the LED and meter
paths, the map abstraction — see [08 — controls](08-controls.md). For how to
launch with it, [13 — Raspberry Pi 4](13-raspberrypi4.md).

## How the shim finds it

Nothing is hardcoded to a device node. The controls shim walks the ALSA
sequencer's client and port lists with `SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT` /
`SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT`, matches the port name against
`RB_MIDI_IN_MATCH` (a case-insensitive substring; empty — the shipped default —
means the FLX4's own hint from the controller table, `controllers.c`), requires
the port's capabilities to include `CAP_READ | SUBS_READ`, excludes its own
client, and subscribes. It retries in a loop, because the shim starts before the
controller is necessarily enumerated — plugging the FLX4 in late has to work.

Output (LED notes and meter CCs) goes through a **second local port** subscribed
to the matched port's receive side, matched independently by
`RB_MIDI_OUT_MATCH` (also default `FLX4`) — so the surface's receive port is
found by name too, and no device node is hardcoded anywhere. Rawmidi is what it
was always meant to be: a **runtime fallback**, opened only when no matching
sequencer destination exists (one attempt every 5 s, `MIDI_LED_DEV` tried first,
then `/dev/snd/midiC*D0`). Which route is in use is always one line in
`/tmp/knobshim.log`:

```
knobshim2: LED/meter output route: sequencer 128:1 -> 20:0 'DDJ-FLX4 MIDI 1'
knobshim2: LED/meter output route: rawmidi /dev/snd/midiC0D0
knobshim2: LED/meter output route: none (no sequencer surface, no rawmidi node)
```

because "no `/dev/snd/seq`" is otherwise a silent failure — the UI draws, and no
control ever reaches it.

This half has never run either: it is implemented (`midi_io.c`) and awaits the
first FLX4. The route line above is how you tell that events are arriving at
all, which is the first thing to check before blaming the map — with
`KNOB_VERBOSE=1`, an unmapped note logs the channel and note number it saw, so a
surface whose numbers differ from these tables is visible without editing
anything.

Confirm what the kernel reports before touching the map:

```bash
cat /proc/asound/cards        # expect a card whose id is DDJFLX4
aseqdump -l                   # expect a port named "DDJ-FLX4 MIDI 1"
aplay -L | grep -i flx4       # the PCM name RB_AUDIO_DEV must match
```

If the card id is not `DDJFLX4`, change `RB_AUDIO_DEV` in `rb.conf` rather than
the shim — and note that an empty `RB_AUDIO_DEV`, which is what ships, already
means "the card of whichever controller `MIDI_MAP` selects", read from that
surface's row in `controllers.c`. Same for `RB_MIDI_IN_MATCH`: empty means the
row's hint, so a surface that reports a differently-spelled name is corrected in
one place, the table, rather than in `rb.conf` and the shim separately.
`controllers_cli match flx4` prints what the shim will look for.

### The keepalive, and the SysEx route it needed

The FLX4's row in the controller table carries a 12-byte vendor SysEx —

```
F0 00 40 05 00 00 04 05 00 50 02 F7
```

— sent every **200 ms** while the surface is attached, and that claim is **not
this port's measurement**. It is the sibling public port's
(`../rx3-flx4`, `controller-bridge.py`), which records that the FLX4 "only
reports controls (and keeps its audio path alive) while the host polls it every
200 ms", sourced in turn from Mixxx's reverse-engineering of the device.

What this port measures is only that the message *can now be sent at all*, which
it could not before: the panel output carries Note On, Note Off and CC, and
`out_write()` drops everything else, so a SysEx had no route to a surface on the
**sequencer** — the FLX4's route. `midi_sysex_out()` adds one
(`SNDRV_SEQ_EVENT_SYSEX` with `SNDRV_SEQ_EVENT_LENGTH_VARIABLE` and the payload
inline immediately after the event header, which is the shape the sequencer ABI
wants; the `VARUSR` length form is direct-dispatch only and is for bulk
transfers, and a wrong pointer there is a kernel-side dereference). On the
rawmidi route it is a plain `write()`.

**First run, 2026-10-06 — it goes out, and nothing observable changes.** Shipped
with the table (`knobshim.so c41d5e92`) and restarted; the shim logged

```
knobshim2: keepalive: 'DDJ-FLX4' 12 byte(s) every 200 ms to 'DDJ-FLX4 MIDI 1'
```

— the row's hint resolved against the *live* port name rather than a constant,
so the route is named and not assumed — and then 320 sends in the first 70 s,
which is the 200 ms period and not merely a first send. With it going out, the
operator's reading of the unit was *"ok playing correctly"*, the same verdict
they gave the build **without** it an hour earlier.

**That narrows the question rather than closing it, and the shape of the claim
says why:** "keeps its audio path alive" is a statement about a **long idle**, so
a few minutes of active use is the case least likely to exhibit it. What the run
does settle is the other half — the message is not required for LEDs, meters,
faders, pads or the touch band, all of which behave identically with it.

So the knob keeps its default of on. Twelve bytes every 200 ms is not worth
optimising away, the sibling documents the device expecting it, and the only real
price is that `RB_KNOB_VERBOSE=1` turns the per-send line into 5 log lines a
second (~9.5 MB/day, well under the `RB_VERBOSE` hazard). The one experiment still
worth running, if it is ever wanted, is a soak: controller attached, nothing
playing, left alone long enough for an idle timeout, then check whether audio
still comes out.

The row's other message is `init`, sent once. The FLX4 has none (`NULL`); the
JP21's is its absolute-control query, and it goes out from
`led_query_absolute()`, not at startup — a startup sender would put that query on
whatever route happened to be up, which with an FLX4 attached and `MIDI_MAP=jp21`
is the FLX4's own port.

## The map

`scripts/shims/map_flx4.c` holds the tables: note/CC numbers per control, the
jog and pitch CC pairs with their conventions, `jog_ppr`, and the LED note
numbers it knows (which it does not drive — see [the LEDs](#the-leds)). The
dispatcher in `ctrlshim.c` is map-agnostic; a map asks the `rbp` bridge for
*meaning* through the named hooks in `rbp_bridge.h` (`send_rx_key()`,
`send_rx_key_f()`, `send_rx_key_fl()`, `cc_to_10bit()`, the `me_*()` mixer-engine
helpers), so a bridge never learns a note number and a map never learns an `rbp`
address.

Two things about the unit shape the whole map, and both are surprising enough to
state before the tables:

* **It is a 2-channel surface.** There are no decks 3/4 and no deck switch.
  Every deck control is on list channel 1 or 2, and nothing mirrors onto
  anything else. The only channel-selection control on the unit is the Beat FX
  **FX CH SELECT** lever, which chooses the *effect's* target, not a deck.
* **Its toggles are notes, not CCs.** Every CC on the unit is continuous: a
  14-bit MSB/LSB position pair, or a relative encoder. There is no CC used as a
  switch and no ≥64 threshold anywhere, so the map needs no CC-as-button entry
  kind — `ctrl_map.h` never grew one, and an earlier draft of this document
  ("DDJ-class controllers put switches on CCs with a ≥64 threshold") is simply
  not true of this unit. The one place a 63/64 boundary means anything is the
  browse encoder, where it is the direction split of a two's-complement delta.

**The channel conversion is the single most load-bearing thing here.** The list
prints MIDI channels 1-based; `struct snd_seq_event` carries them 0-based, so
every channel in the map is the list's minus one. A deck on the wrong channel
makes every control on it do nothing at all, and no fixture test can catch that,
because the fixture was written from the same reading of the list the map was.

### Deck 1 and deck 2 (list ch 1/2 → rch 0/1 → rbp players 1/2)

Source: the list's channel-1/2 pages, plus a capture from the unit (S5.1), which
**settled the conversion above**: deck 2's PLAY arrived on list channel 1 and
MASTER CUE on list channel 6, exactly as the minus-one rule predicts.

The same capture **confirmed** these rows as the map already had them: PLAY note
11 (both decks), CUE note 12, platter touch note 54, channel CUE note 84 (the
mixer engine, no keycode), SHIFT note 63, BEAT SYNC note 88, LOAD notes 70/71 on
list channel 6, MASTER CUE note 99 on channel 6, and pad 1 as note 0 on list
channel 8 (HOT CUE base 0, rch 7). The rows still marked `TODO: unverified` are
the ones the capture did not exercise.

| Control | Message | rbp keycode | op |
|---|---|---|---|
| PLAY/PAUSE | note 11 | `0x4101` K_PLAY | press / release |
| CUE | note 12 | `0x4102` K_CUE | press / release |
| platter touch | note 54 | `0x4306` K_JOG_TOUCH | press / release |
| loop IN | note 16 | `0x410c` K_LOOPIN | press / release |
| loop OUT | note 17 | `0x410d` K_LOOPOUT | press / release |
| 4 BEAT / EXIT | note 77 | `0x410e` K_RELOOP | press / release — `TODO: unverified`, a *fresh* 4-beat loop is a **pad** in rbp's AUTO BEAT LOOP mode, which the PAD FX 1 button below enters, and it is **pad 5** (the bank is 1/4 … 32, measured 2026-10-01 — **not** pad 1, which is a quarter beat) |
| BEAT SYNC | note 88 | `0x4112` K_SYNC | press **and** release on the first edge that arrives — see footnote *3 below |
| BEAT SYNC long press | note 92 | `0x4111` K_MASTER | a pulse on press — `TODO: unverified`, the list gives this note no name beyond "Long press" |
| pad mode HOT CUE | note 27 | `0x4113` K_HOTCUE | press / release — rbp's pad mode 0 |
| pad mode PAD FX 1 | note 30 | `0x4114` K_ALOOP | press / release — rbp's pad mode 1, its **AUTO BEAT LOOP**. The unit's PAD FX pads are a rekordbox feature rbp has no equivalent for and the button stands in for the RX3's **2nd** position, which is BEAT LOOP. The binding is **positional** — see the pad-mode table's caveat below |
| pad mode BEAT JUMP | note 32 | `0x4115` K_SLIPLOOP | press / release — rbp's pad mode 2, its **SLIP BEAT LOOP**. The unit's 3rd button occupies the RX3's 3rd position, which is SLIP LOOP, so the name on the button is deliberately ignored (**positional** since 2026-10-01, measured on the glass) |
| pad mode SAMPLER | note 34 | `0x4116` K_BEATJUMP | press / release — rbp's pad mode 3, its **BEAT JUMP**. Same reasoning, other way round: the unit's 4th button occupies the RX3's 4th position, which is BEAT JUMP. rbp has no sampler pads, so nothing is lost |
| CUE/LOOP CALL ◁ | note 81 | `0x420d` K_BACK, global | **not** log-only: this is the port's only BACK. The FLX4 has no BACK and no SOURCE button at all, and without a BACK the browse screen is a one-way door — every keycode that leaves it is one this surface does not have. Nothing is taken from the button by this: 81's real rbp keycode is unidentified, so it was dead, and "<" reads as "back one level", which is what it now does |
| CUE/LOOP CALL ▷ | note 83 | *(none)* | log-only — rbp's keycode for it is not identified, see [open questions](#open-questions) |

Footnote *3 of the list says the BEAT SYNC button sends its message **when the
finger is released**, not when it is pressed. The map therefore treats whichever
edge arrives first as the whole gesture and swallows the other, so one press can
never send SYNC twice. The S5.1 capture shows one press producing **both** a
note-on and a note-off on note 88 (the inventory counts it `1/1`), so there is no
edge to wait for and the first-edge rule is what keeps that single press from
being sent to rbp twice. Which edge came first is recorded in the dump itself —
`grep 'note=88' flx4.dump` prints them in order — and does not change what the map
does with them.

Every one of these controls has a **+SHIFT variant on the same channel** (a
different note: PLAY 14, CUE 72, platter touch 103, loop IN/OUT 76/78, 4 BEAT
80, CALL 62/61, channel CUE 104, MASTER CUE 120, …). **Three of them are bound
and the rest are not**, and the boundary between the two groups is the whole of
what this map claims about SHIFT:

| SHIFT + … | note | rbp | why this one |
|---|---|---|---|
| browse push | 66 | `0x0201` K_SOURCE, global | the only row that reaches the mounted USB, so the port cannot work without it — **measured** off the panel ([below](#the-shift--browse-push-and-the-two-questions-it-is-two-questions-about)) |
| LOAD, deck 1 | 104 | `0x420f` K_TRACKFILTER, global | the operator asked for it by name, 2026-09-29 — **and pressed it 2026-09-30**, bringing up the TRACK FILTER screen (`note=104 ch=6 vel=127` in the dump) |
| LOAD, deck 2 | 122 | `0x0210` K_SHORTCUT, global | ditto, bringing up the SHORTCUT view (`note=122 ch=6 vel=127`) |

The rest are unbound on purpose and log as unmapped, which is the honest outcome:
SHIFT itself (note 63) is **log-only**, so the map tracks no SHIFT *state* at all
— a shifted note **replaces** its base note on the wire rather than arriving
beside it, which is what makes binding the shifted note the right shape rather
than a shortcut. Note 104 has to be read with its channel beside it: on the deck
channels (rch 0/1) it is SHIFT + channel CUE, and on the mixer's list channel
(rch 6) it is SHIFT + LOAD 1 — the `CH_MIX` guard on that row is the whole of the
separation, and `122` is used nowhere else in the map.

### Deck channel CCs (list ch 1/2 → rch 0/1)

| Control | Message | rbp | note |
|---|---|---|---|
| TEMPO (pitch fader) | CC 0 (MSB) + CC 32 (LSB), 14-bit | `0x4109` K_TEMPO_SLIDER, op `OP_VALUE`, float −1.0…+1.0 | sent once per pair, on the LSB |
| platter | CC 34 (vinyl on) / 35 (vinyl off) / 41 (+SHIFT) | `0x4305` K_JOG_ROT, op `OP_ROTATE` | relative, **0x40-centred**, measured (S5.1): values of 63/65/66 only, and under the two's-complement reading a steady turn decodes as alternating ±63 steps |
| jog ring | CC 33 | ditto | the outer ring, same convention — measured the same way, largest step 1 |
| TRIM | CC 4 | `0x5019` K_TRIM | one 128-step row |
| EQ HI | CC 7 | `0x501a` K_EQH | ditto |
| EQ MID | CC 11 | `0x501b` K_EQM | ditto |
| EQ LOW | CC 15 | `0x501c` K_EQL | ditto |
| CH FADER | CC 19 | `0x501e` K_FADER | also feeds `g_fader[]`, the meter taper's input |

Only the MSB of each 14-bit pair is bound, giving one 128-step row per knob —
the same resolution the JP21 map had. The LSB then arrives as an unmapped CC and
is **dropped**, which is deliberate: it is the resolution the previous target's
knobs had too. The pitch fader is the exception, because rbp's
`K_TEMPO_SLIDER` genuinely wants all 14 bits.

The S5.1 capture **measures the pairing rule** this paragraph assumed. Each LSB
arrived with exactly as many events as its MSB: CC 51 with CC 19 (171 events
each), CC 63 with CC 31 (310 each), CC 32 with CC 0 (44 each) — the `n + 0x20`
rule, counted rather than quoted. So the LSB is not stray traffic that happens to
arrive alongside; it is the low 7 bits of the same control, and it is available
if a 128-step row ever proves too coarse for a fader.

The pitch fader's *polarity* `TODO: unverified`: the list gives the two ends
(min "−" side = `0x00/0x00`, max "+" side = `0x7F/0x7F`) but not which way the
physical fader moves to reach them, and "−"/"+" describe the tempo change rather
than the position. The map uses the value **without inversion** (−1.0 = slower,
+1.0 = faster, which is what the two ends line up as) and `RB_TEMPO_REV=1` is
the one-word fix if a dump says otherwise.

### Pads (list ch 8/10 → rch 7/9)

A pad's note encodes **both** its number and the unit's current pad mode:
`base + pad`, where base is 0 HOT CUE, 16 PAD FX 1, 32 BEAT JUMP, 48 SAMPLER,
64 KEYBOARD, 80 PAD FX 2, 96 BEAT LOOP, 112 KEY SHIFT, and pad is 0…7.

rbp's pad keycodes `K_PAD1`…`K_PAD8` (`0x4117`…) mean whatever **rbp's own** pad
mode says they mean, and that mode is set by the pad-mode buttons above.

**rbp has exactly four pad modes, which is why the unit's buttons are repurposed
rather than matched one for one.** rbp's dispatch tree
(`ui::PlayerInnards::onPhysicalKey` @ `0x306b78`) carries four mode setters and
nothing else — each a `str` of a small immediate to `[this+0x74]`, with the
`onKey_Pad` jump table, `getStat`'s exported pad-mode byte and `ui::Player`'s
four `check*LedState` functions all agreeing that the enum is 0…3:

| mode | keycode | rbp's own name |
|---|---|---|
| 0 | `0x4113` | HOT CUE (`onKey_HotCue`) |
| 1 | `0x4114` | AUTO BEAT LOOP (`onKey_AutoBeatLoop`) |
| 2 | `0x4115` | SLIP BEAT LOOP (`onKey_SlipBeatLoop`) |
| 3 | `0x4116` | BEAT JUMP (`onKey_BeatJumpLoopMove`) |

(The column used to be headed `+0x74`, which is the byte the four setters `str`
into. `+0x74` is **not** the mode the UI displays — see the falsification further
down — so the enum is named by value here and nothing should read that byte as
the mode.)

So **RELEASE FX, PAD FX 1/2, SAMPLER and KEYBOARD do not exist in rbp at all** —
no keycode, no mode value, no such string in the binary — and SLIP LOOP is the
same mode as SLIP BEAT LOOP rather than a second one. The unit's pad-mode buttons
therefore map onto rbp's four modes **by position, not by name** (2026-10-01): the
RX3's row is HOT CUE, BEAT LOOP, SLIP LOOP, BEAT JUMP, and each of this unit's
four buttons takes the mode that occupies the same position, whatever its own
label says. Pressing a mode button
rbp is *already* in switches that mode's size bank instead of re-entering it,
which is rbp's own doing (`onKey_AutoBeatLoop` toggles `[+0x7a]`, and each of the
other three has the like) and is the RX3's behaviour too — the map sends the key
and lets rbp read its own state rather than compute the new mode itself. It is
also what produces the fifth mode value, `4`.

**The caveat the two rows above point at — corrected 2026-09-30, and completed
2026-10-01, because the earlier reading of it was wrong.** The map sends those
keycodes, the shim dispatches them, and **rbp acts on them: rbp's own pad grid
changes mode on the glass.** Measured the operator's way, one key at a time with
rbp's framebuffer read after each: note 27 → the deck-1 pad grid reads **HOT CUE**
(A–H), note 30 → **BEAT LOOP** (1/4 … 32), note 32 → **BEAT JUMP**. Those three
were the 2026-09-30 run, **before the binding was made positional** — the two
cells that changed on 2026-10-01 are restated below. The frame diff is 1264 px
between HOT CUE and BEAT LOOP and 5801 px between BEAT LOOP and BEAT JUMP, all
inside the pad-grid rows. So the route between `sendKey` and `onPhysicalKey` is
**fine** — the plumbing the old note suspected does not need fixing, and the
operator's own eyes caught it (they reported the screen changing under PAD FX 1
while the panel's LED stayed dark).

**All four buttons are now measured on real presses, and the binding is
POSITIONAL (2026-10-01).** The operator's instruction was *"ignore the names on
the FLX4, it should just map to the way the RX3 behaves by position for muscle
memory"*: rbp's UI is an XDJ-RX3's, whose pad-mode row runs HOT CUE, BEAT LOOP,
SLIP LOOP, BEAT JUMP — rbp's modes 0,1,2,3 — which is also the order
[`map_jp21.c`](../scripts/shims/map_jp21.c) binds the SC Live 4's four in. So on
this unit, whose row is HOT CUE, PAD FX 1, BEAT JUMP, SAMPLER, the **3rd button
(its BEAT JUMP, note 32) selects SLIP BEAT LOOP** and the **4th (its SAMPLER, note
34) selects BEAT JUMP**. Measured with a live MIDI dump as one witness (the panel
sent `ch0 note=32 vel=127` then `ch0 note=34 vel=127`, vel 127 being the panel's
own and not an injection) and `UiGetPadMode` read continuously as the other: the
byte went `0 → 2 (SLIP BEAT LOOP)` on the 3rd button and `2 → 3 (BEAT JUMP)` on
the 4th, and deck 1's grid relabelled to **SLIP LOOP** (cells 1/16 … 3/4) and
**BEAT JUMP** (cells 1, 2, 4, 8) while deck 2 held at HOT CUE. That closes note
34, the one button the 2026-09-30 run never captured on the glass.

One note number to keep straight: **32 is both** the 3rd mode *button* on ch 0 and
pad 1 of base 32 on ch 7, and only the channel separates them.

A fifth value, `4`, is real and is not a corrupt read: pressing the button for the
mode rbp is **already in** takes the byte to `4` by way of the size-bank path.

The old note was wrong twice over, and both mistakes are worth keeping in view
because each is a way a measurement lies. The experiment that "showed no change"
captured the screen twice with **both** captures ending on the same button
(SAMPLER), so the two frames necessarily looked alike and the diff was empty *by
construction* — a broken experiment, not a null result. And the live-process probe
read `PlayerInnards+0x74`, which **does not track the mode the UI displays**:
polled 1.1 million times across all four mode buttons while the grid verifiably
switched HOT CUE → BEAT LOOP, that byte never left 0. So `+0x74` is not rbp's
displayed pad mode, and the static derivations that pointed at it are at least one
too many.

**The real mode was found the same evening, and it needs no scan.**
`UiGetPadMode(ENUM_DECK)` @ `0xfd3cc` is an exported C function — deck 0 is deck 1
— that walks `holder = *(0x02685f2c)`, `statwatcher = *(holder + 0x38)`,
`perdeck = *(statwatcher + (deck ? 0x10 : 0xc))`, and returns the byte at
`perdeck + 0x281`. Proven against the live process, control in the same run:
injecting ch0 notes 27/30/32/34 moved deck 1's value **3 → 0, 1, 3, 2** exactly
(HOT CUE / AUTO BEAT LOOP / BEAT JUMP / SLIP BEAT LOOP — the same four modes, in
the same order, as the `onKey_*` table above), while **deck 2 held at 0
throughout**. So note 34 is measured too, by this route; all four mode buttons are
confirmed. The defines are in `rbp_abi.h`.

The **feedback** for all four is now driven, and it was the panel that supplied
the notes. `struct led_notes` gained an `n_mode[4]` field indexed by **rbp's own
mode value**, and the four numbers were measured on the glass on 2026-10-01: each
of the four was lit alone from the host and the operator read back the button it
lit — **27 the 1st, 30 the 2nd, 32 the 3rd, 34 the 4th**, in order, on the deck
channel, with deck 1's CUE (84/0) flashing as the positive control in the same
run. They are the four buttons' own **input** notes, so the LED rule this map had
already measured twice now holds 4 for 4 here.

There is no LedStat entry to read them from, which is why they need a field of
their own: the whole 45-entry table was dumped either side of each of the four
modes (deck 1's mode read back 0/1/2/3) and **nothing in it changed but the pad
grid's RGB**. So the shim reads rbp's mode byte instead (the same walk
`UiGetPadMode` does), exactly as the FX CH SELECT lever is read rather than
remembered — a mode changed from rbp's own screen lights the right button too.
S5.7 and S9.9 in [13](13-raspberrypi4.md) carry the measurements.

A base is therefore bound iff the map puts rbp into a mode for it, which is why
**four** of the eight ranges are bound — and, the mode buttons being positional,
which rbp mode each base carries follows **that** order rather than the unit's
labels: HOT CUE (base 0 → mode 0), base 16 → AUTO BEAT LOOP, **base 32 → SLIP BEAT
LOOP** (the unit's BEAT JUMP button) and **base 48 → BEAT JUMP** (its SAMPLER).
All four bases send the *same* `K_PAD1..8`, because rbp's pad keycodes mean
whatever rbp's current mode says they mean — so the positional rebind rewrote the
base→mode correspondence and left the pad rows alone. The bases are **not
optional**: the unit really does move its pads to the new base when the button is
pressed, so without them pressing PAD FX 1 would put rbp into a mode whose eight
pads sent notes nothing was bound to.

The other four stay unbound because their **buttons** reach no rbp mode: KEYBOARD
and PAD FX 2 have no rbp equivalent at all, and BEAT LOOP and KEY SHIFT arrive
from a SHIFT + mode-button combination **whose pairing the list never states** —
it gives four extra modes and four shift notes without saying which is which, so
binding them would be a guess about which button puts rbp into which mode. An
unbound pad produces nothing and logs as unmapped.

The pads' +SHIFT layer is a **second channel per deck** (list ch 9/11 → rch
8/10) carrying the same eight ranges. **Its HOT CUE base is bound** (2026-10-01,
on the operator's own instruction) and carries the FLX4's own gesture, SHIFT +
HOT CUE pad → delete that pad's cue; the other three bases stay unbound
deliberately, because a delete is not what they would mean.

**The layer's note numbers are measured, not published.** One press by the
operator on 2026-10-01, with `RB_MIDI_DUMP` on, put the shifted pad on the wire
as

```
610.514000 NOTEON ch=0 note=63 vel=127    <- SHIFT (note 63, mixer ch 0)
610.810000 NOTEON ch=8 note=0 vel=127     <- +SHIFT pad 1, rch 8 = CH_PADS1S
610.910000 NOTEON ch=8 note=0 vel=0
611.118000 NOTEON ch=0 note=63 vel=0
```

so the layer is where the list says, the note is `base + pad`, and the shifted
note **replaces** its base note — there is no ch 7 note 0 beside it, the same
behaviour as the other three SHIFT bindings. vel 127 is the panel and the shim's
own `seqinject2` sends at 100, which is what keeps the two apart in a dump.

**A later press the same day is archived, so the evidence is now a file and not a
transcription** — `work/dumps/flx4-shift-pad-delete-2026-10-01.dump` (43 lines),
and it is a better record than the four lines above because it holds the whole
gesture rather than one note number:

```
1184.308000 NOTEON ch=6 note=70 vel=127      <- LOAD deck 1
1190.667000 NOTEON ch=7 note=4 vel=127       <- K_PAD1+pad5 = SET the cue on pad 5
1191.397000 NOTEON ch=0 note=63 vel=127      <- SHIFT down
1191.609000 NOTEON ch=8 note=4 vel=127       <- +SHIFT pad 5 = the DELETE
1191.877000 NOTEON ch=8 note=4 vel=0
1191.921000 NOTEON ch=0 note=63 vel=0
```

Two things that file settles beyond the note numbers: the shifted pad arrives with
**no** base-note twin at 1191 (the replacement again, on a second pad), and the
shim's own injected events sit in it at t=0 as an in-file **control** — vel 100 is
the injector, vel 127 the panel, so which is which never has to be argued. It
confirms note 66 (the SHIFT + browse push) at vel 127 inside a SHIFT-held window
(1176.830–1177.314) as well.

Getting the *action* there took a negative worth recording in full, because
**both** halves of the obvious wiring are wrong:

* **`0x4124` is named CueDelete, and it deletes nothing you can choose.** Its
  arm in `ui::Player::onPostHandleEvent` (@0x2f5e10, reached through
  `Player::asEventCode` @0x2f16d4 → EC 34 and the jump table at 0x2f5b10) calls
  `DbProxy::deleteCue(channel, music_id, &player[0x234])` — a pointer built by
  `add r3, r4, #564`, a **compile-time constant**. That instruction shape occurs
  exactly twice in the whole binary: there, and in `ui::Player`'s constructor
  `memset`, **which is also the only writer that slot ever has**. The real
  hot-cue slots are the nine-block array at `0x268`..`0x3d4`, which
  `Player::isLoadableHotCue` @0x2f2e50 indexes as `this + 616 + 52*(cueId-1)`.
  So the keycode takes no pad number and reads no selection field, and
  pre-selecting pad N cannot change what it deletes.
* **The real per-pad delete is index-addressed and takes the keycode this map
  already sends — and no key the shim can send reaches it.**
  `ui::Player::onHotCueDeleteEvent(pad)` @0x2fb328 *is* index-addressed — it
  checks that pad's own flag in `Player+0x479..0x47b` (**bit SET = that pad holds
  no cue → return**), then `Player+0x13c` TrackInfo, then
  `TrackInfo::seacheHotCue(pad)`, then a per-pad state byte, and only past all
  four does it reach `PlayerInnards::onEv_SetHotCueRecDisable` and
  `DbProxy::deleteCue` with that pad's own `CueInfo_t`. `Player::onHotCueEvent`
  @0x2f5720 calls it on **`K_PAD1+p`** (`pad = keycode − 0x4116`) when
  **`[innards+0x38] == 3`** — so the keycode is right and only the selector byte
  is missing. **Writing that byte from the shim does not work, and this is
  measured rather than inferred** (2026-10-01): with `[innards+0x38]` set to the
  impossible value **7** on the deck's *own* innards, one plain `K_PAD1` sent, and
  the word read back — it is **0**. rbp clears it on the key path before the
  handler reads it, so nothing written from outside survives the dispatch. (The
  delete arm carries a second gate in any case: `cmp r2, #1` on `onHotCueEvent`'s
  `IInnards3RES` argument, before `popne`, which only rbp's own caller supplies.)
  The earlier form of this bullet — that the selector's one writer is the
  physical path, and writing it is therefore the way in — was the wrong half of a
  right observation.

**So the gesture is two direct calls, which is what this map now does**, and the
second one is the whole of the operator's own verdict on the first —
*"ok, i did it but the cue is still there. the light does go off though"*
(2026-10-01). That sentence is a complete description of the defect, because the
FLX4's pad LED is driven from rbp's cue list, so a dark pad means **rbp agrees the
cue is gone while the glass keeps drawing it**.

`rbp_bridge.c`'s `hotcue_delete(deck, pad)` resolves the deck's `ui::Player` from
rbp's own object array — the walk `IUiObjManager::getPlayer` makes,
`*(0x026867c0)` → `+64` → array, `UiObject::Channel` being 1-based — and calls
`onHotCueDeleteEvent(pad)` through `rbp_abi.h`'s `ADDR_HOTCUE_DELETE`;
`map_flx4.c` catches the +SHIFT layer's HOT CUE base above its table loop, which
is the one shape a table row cannot express. That function's dispatch is virtual;
the function itself is a plain concrete symbol, which is what makes calling it
legal, and what makes it **safe** is how thoroughly it guards itself (the four
checks above): an empty pad is a no-op, not a stray delete.

**But that is the UI and DB half of the delete, and rbp's delete has two halves.**
`onHotCueDeleteEvent` rewrites TrackInfo's cue list, clears the pad's own flag and
state byte, calls `onEv_SetHotCueRecDisable`, deletes from the DbProxy and stops
the pad's blink timer — and it **never touches the engine**. The engine half exists
at exactly **one call site in the whole binary**: `bl 0x48a48` at `0x307e68`,
inside `ui::PlayerInnards::pad_HotCue` @0x307918. `pad_HotCue` is the *physical*
pad handler — reached from `onPhysicalKey` → `onKey_Pad`, the path
`IKeyManager::sendKey` never takes — which is why no keycode this shim can send
reaches it, and why the engine kept the cue after a delete that had otherwise
fully succeeded. The cue stayed registered, `ui::StatWatcher` kept serving the pad
grid its IN time, and the cell kept drawing a pillar. So `hotcue_delete()` now
makes that second call too, in the same order rbp does: `rbp_abi.h`'s
`ADDR_ENGINE_IS_REGHOTCUE` (asked, and its answer logged as `reg=`, but **not**
used as a gate — see that constant's comment) and `ADDR_ENGINE_CLEAR_HOTCUE`,
`djengine::DjEngineIF::clearHotCue(ch, pad)` @0x48a48, whose body tail-branches to
`playengine::PlayEngine::clearHotCue` @0x5ed58 → `Player::clearCuePosition`
@0x65f6c. Its `EnCueType` is the **pad number 1..8**, not an index —
`playengine::Player::backHotCueGate` @0x66bf8 opens `sub r2, r1, #1; cmp r2, #7;
bhi <return 0>` — and its channel is the **0-based deck index**, which is how
`pad_HotCue` computes it at 0x307e18 (`ldrb r6,[r4,#0x26]; sub r1,r6,#2; rsbs
r6,r1,#0; adc r6,r6,r1`). Both entry points ignore their `this` (each reloads r0
from its own global singleton, lazily constructing the PlayEngine if NULL), which
is visible on the unit as the **same** engine pointer for both decks.

**What the ghost looked like, and the rule that explains every pad cell.** After a
delete whose UI and DB halves had both succeeded, deck 1 pad 5's StatWatcher record
(`deckobj + 0x78 + 52*pad`, +0x04 = IN) still held **IN `0x31fa6` with colour 0**,
and the grid drew it as a **default-cyan pillar**. The same stale IN sat in rbp's
own mirror at `Player+0x268+52*(pad-1)`, and poking *that* does not stick — rbp
restores `0x31fa6` on a later scan, so the mirror is re-synced and the **engine is
the authority**. The rendering rule that covers every cell observed: **draw a
pillar iff `IN != 0xffffffff`**, colour it from the colour field, and **colour 0
renders as the default cyan (0,226,255)**. That one rule accounts for deck 2's
eight cyan pillars (all-zero records → IN 0), pads 6/7 blank (IN = −1), pad 5's
cyan ghost (stale IN, colour 0), and pads 1–4/8's genuine coloured pillars.

**It works, and the operator's own gesture is what says so** (2026-10-01). They
loaded a track, set a cue on pad 5, held SHIFT and pressed that pad, and the log
read

```
knobshim2: hotcue delete deck1 pad5 @0xcda7c670 inn=0xcda7cbf8 eng=0xe9601510 reg=1
```

— verdict, in their words: *"it seems to work"*. Note **`reg=1`**: rbp's own
`isRegisteredHotCue` gate agreed a cue *was* registered for that pad, so this ran
the real path and not the no-op one (the two injected lines above, `reg=0`, are the
no-op path on trackless decks). The dump of that same gesture is archived at
`work/dumps/flx4-shift-pad-delete-2026-10-01.dump`, so the cue-set and the delete
are evidence together rather than a report.

**What is measured, and what is not.** The plumbing is: both decks' players and
innards resolve, the engine pointer is the same object for both, `rbp` survives the
calls and still renders normally (checked by framebuffer capture), and the delete
itself now works on the glass. The one honest limit left is instrumentation rather
than behaviour — the cell clearing was witnessed by the operator's **eye**, not by
a before/after framebuffer capture, because the cue was already gone by the time a
capture was possible. `work/padwatch.py`'s `aux` word (`LED_ENTRY_OFF_UNASSIGNED`,
1 = nothing assigned) stays the panel-side instrument if a later session wants the
picture as well as the verdict. And note, for anyone re-testing: setting a test cue
writes cue data to the operator's own export DB on their USB stick.

**Measured 2026-09-30 — the two repurposed bases are off the vendor's list.** The
operator pressed all eight pads in each mode with the dump running, and the notes
land exactly as the encoding predicts: **PAD FX 1** (note 30, ch 0) then pads 1–8
as **notes 16,17,18,19,20,21,22,23** on **ch 7**, and **SAMPLER** (note 34, ch 0)
then pads 1–8 as **48,49,50,51,52,53,54,55** on ch 7, every press with its release
~125–220 ms later. So base 16 and base 48 are what the unit really sends, `base +
pad` is confirmed on both, and — the part that made these two bases non-optional
above — **the mode button really does move the unit's pads to the new base**, and
(contrary to what this file said until the same evening) **rbp acts on the mode
keycode itself**: its pad grid changes mode on the glass, and only the byte this
file had been watching stayed still. The correction is earlier in this section;
the bases are unaffected by it either way. Record:
`work/dumps/flx4-2026-09-30-pad-bases.dump`, 92 lines, md5
`9c86c0bc05990073c68b368971d383d3`.

**Measured 2026-10-01 — all four bound bases have now been pressed, and the two
remaining ones behave the same way.** Base 48 (the unit's SAMPLER) was pressed
eight at a time with rbp in BEAT JUMP: notes **48…55 on ch 7**, vel 127, each with
its release. Base 32 (the unit's BEAT JUMP) was pressed eight at a time with rbp in
SLIP BEAT LOOP: notes **32…39 on ch 7**, vel 127, after the mode button had put
deck 1's grid on **SLIP LOOP**. So the encoding is `base + pad` in all four bases
this map binds, and the positional rebind does not touch it. Two honest limits on
that run: with **no track loaded** on either deck, a beat-jump or slip-loop pad has
nothing to act on, so base 32 and base 48 are **dispatch-measured and
action-unwitnessed** — the action evidence is base 0 and base 16, where the
playhead moved and a window check ruled out every other control. And the pads in
the *unit's* PAD FX 1 base still act in whatever rbp mode is current, so a press
made in the wrong mode is a wrong action rather than nothing. Records:
`work/dumps/flx4-2026-10-01-pad-mode-positional.dump` (39 lines, md5
`7acd4f55ff576b448be3d311b931480d`) and the continuous mode-byte log beside it,
`work/dumps/padmode-2026-10-01.log`.

What is still not measured in this table is the **SHIFT pairing of the four extra
modes** — none of which this map binds, and binding them without the pairing
would be the guess the paragraph above refuses. **The +SHIFT pad layer itself is
measured now** (2026-10-01, above): its HOT CUE base is bound, its note numbers
came off the operator's own press, and the gesture's plumbing is on the unit — so
nothing about the layer is published-only any more. The
[+SHIFT paragraph](#pads-list-ch-810--rch-79) carries all of it.

### Mixer, browse and LOAD (list ch 7 → rch 6)

| Control | Message | rbp | note |
|---|---|---|---|
| Sound Color FX, deck 1 | CC 23 | `0x509d` K_COLOR to rbp ch 1 | the **mixer's** channel, not the deck's |
| Sound Color FX, deck 2 | CC 24 | `0x509d` K_COLOR to rbp ch 2 | ditto |
| crossfader | CC 31 | `0x6017` K_XFADER, global | |
| headphone MIX | CC 12 | `0x4405` K_HPMIX, global | also feeds `g_cue_mix` for the audio shim |
| headphone LEVEL | CC 13 | `0x4406` K_HPLEVEL, global | also feeds `g_cue_gain` |
| browse push | note 65 | `0x420c` K_SELECTOR, global | press / release |
| SHIFT + browse push | note 66 | `0x0201` K_SOURCE, global | rbp's SOURCE screen, and the only row that reaches the mounted USB at all. **Measured off the panel 2026-09-30** — see below |
| browse rotate | CC 64 (+SHIFT CC 100) | `0x420c` K_SELECTOR, global | relative, **0x01/0x7F** two's complement — *not* the jog's convention. Measured (S5.1): values 1 and 127 only, and the two conventions are what tell it apart from the platter, so this is the row that could most easily have been mapped backwards |
| LOAD, deck 1 / 2 | notes 70 / 71 | `0x4311` K_LOAD, global key with rbp ch 1 / 2 | both note numbers measured off the panel — 70 on 2026-09-26, **71 on 2026-09-30**. Note 71 is also the FX ON/OFF note (`N_FX_ONOFF`, one leg per target channel), and the channel is what separates them: `ch=6` is LOAD deck 2, `ch=4` is the FX button |
| SHIFT + LOAD, deck 1 / 2 | notes 104 / 122 | `0x420f` K_TRACKFILTER / `0x0210` K_SHORTCUT, global | rbp's track-filter panel and its shortcut view. **Both halves measured 2026-09-30** — the binding by injection, the note numbers by the operator's own presses, which the dump caught on the wire (`note=104` / `note=122`, both ch 6, both vel 127) *and* which brought both screens up; 104 needs a third edge a tap does not send ([below](#the-shift--load-bindings-and-the-edge-rbp-needs-that-a-tap-does-not-send)) |
| MASTER CUE | note 99 | *(none — the mixer engine)* | toggles `me_set_master_cue()` from `me_get_master_cue()` |
| SMART CFX / SMART FADER | notes 0 / 1 | *(none)* | log-only — rekordbox features |
| MONO/STEREO | note 109 | *(none)* | log-only — it is the mono sum of both channels, not the L=cue/R=master split `rbp`'s stereo type means, so it is not `me_set_stereo()` |

Two mixer controls are **deliberately not in the table at all**, and the reason
is not that they are unknown:

* **MASTER LEVEL** (CC 8 MSB + CC 40 LSB — a 14-bit pair, measured 2026-09-27)
  is the unit's own output volume, and it sits *after* the USB audio it feeds.
  Driving rbp's master level or the audio shim's `g_master_gain` from it would
  attenuate the master twice, so it is deliberately **not** bound to
  `K_MASTERLVL`; `flx4_startup()` pins rbp's master level at unity instead. It
  is not dropped either: `flx4_mastervol()` writes **`g_mirror_gain`**, the HDMI
  mirror's level — the one place the value can go without attenuating anything
  twice, so the HDMI copy tracks the room while not a sample of the FLX4's
  stream changes ([09](09-audio.md#the-level-the-units-own-master-level-knob)).
  Shaped like `flx4_pitch()` and for the same reason: a pair of messages is one
  physical change, so it dispatches on the **LSB** and holds the MSB, and the
  unit reports no position at connect — which is why the mirror starts at unity
  and re-syncs on the knob's first touch. The position is then mapped, not used
  raw: `flx4_mastervol_gain()` puts **unity at 1 o'clock** — 0.6 of the raw
  range, measured from the knob's own stops rather than assumed — and holds it flat
  above, so 12 o'clock reads 0.833 rather than full level. It sat at the *middle*
  first, because with unity at the stop the mirror was too quiet to use (the
  operator's 2026-09-27 report); the move to 1 o'clock is that operator's follow-up
  the same day. `RB_MIRROR_GAIN_MID=1.0` restores the linear law
  ([09](09-audio.md#the-level-the-units-own-master-level-knob)). The level
  *above* unity is not this knob's and never was: since 2026-09-30
  `RB_AUDIO_MIRROR_BOOST_DB` (default **+4 dB**) multiplies in *underneath*
  this law, so the knob attenuates a lifted level rather than defining it, and
  the lift is clamped to the 24-bit domain at the gain rather than in the
  packers ([09](09-audio.md#the-lift-rb_audio_mirror_boost_db-and-the-clamp-it-needed)).
* **MIC LEVEL** (CC 5) has no reader here, because rbp's mic input is not part
  of this port.

The browse knob's two relative conventions are worth keeping apart, since they
are the kind of thing that "works" while being backwards: the **jog** reports
counts from a **0x40** centre ("Turn clockwise: Increases from 0x41 / Turn
counterclockwise: Decreases from 0x3F"), while the **browse** knob reports a
**two's-complement delta** ("0x01 / 0x7F"). They are handled by two different
functions with two different scales for that reason.

### The SHIFT + browse push, and the two questions it is two questions about

The row above is the only way this surface reaches the SOURCE screen, and it is
the row with the weakest evidence behind it — so it is worth being exact about
what has and has not been shown.

There are **two independent things** that have to be right, and they fail
differently:

1. **What the binding does.** A `K_SOURCE` press is what opens the screen. This
   is confirmed on the unit, by injection through the real code path: one
   `ch6 note66` press and release took rbp from `mode 1` to `mode 12` (SOURCE)
   and `dev 0` to `dev 3` (USB1), and a framebuffer capture showed the USB with
   its 736 songs. The map's *disposition* of note 66 is therefore known good.
2. **That note 66 is what the panel actually sends.** **Measured 2026-09-30**, from
   the operator's own presses. `/tmp/flx4.dump` holds 12 lines of `note=66`, every
   one at **vel 127** — the panel's velocity, not `seqinject2`'s 100, which appears
   nowhere in the file (the whole dump is 261 lines at `vel=127` and 277 at
   `vel=0`, and nothing else). The six presses are not merely present: each falls
   strictly inside a SHIFT-held window. Note 63 (SHIFT) goes down at 2864.364,
   2865.940, 2949.356 and 3419.741 and up again at 2865.404, 2866.692, 2951.236
   and 3421.060, and every one of the six `note=66` presses lies between a down
   and an up. `note=65` — the *unshifted* browse push — appears **nowhere** in this
   dump, and the only notes on ch 6 are 66 and 70. So the shifted note **replaces**
   the base note rather than accompanying it, and that is what makes binding the
   shifted note the right shape rather than a shortcut: SHIFT itself arrives as
   note 63 on the **deck** channel (0 or 1), never on ch 6, so a map that tried to
   track a SHIFT state would be watching a note on a different channel from the one
   it qualifies.

   The measurement is one press with `RB_MIDI_DUMP` on, and it answers (2) and only
   (2): an injection tests the binding, a press tests the number, and neither
   implies the other. Note 70 (LOAD 1) is measured the same way in the same dump.
   Note 71 on ch 6 — LOAD 2 — is **not**: the six `note=71` events in the file are
   all on **ch 4**, where 71 is `N_FX_ONOFF` (see the Beat FX table), so LOAD 2's
   own row has still never been seen on a real press.

   One claim in this paragraph used to be stronger than the evidence: it said note
   65 and note 70 *are* measured at the panel's velocity. Note 70 is, in this dump.
   Note 65 is not in this file, nor in `work/sample.dump` or
   `work/rb-replay-gain.dump`, and `/tmp/flx4.dump` is overwritten by each dump
   run — so if an earlier session measured it, that dump is gone and the claim
   cannot be checked against anything in the tree. The browse-push row is
   unembarrassed either way (its *binding* is measured and it shares a mechanism
   with the note-66 row above); it is the sentence that overreached, and it is
   recorded here rather than quietly dropped.

### The SHIFT + LOAD bindings, and the edge rbp needs that a tap does not send

These are the operator's ask of 2026-09-29, verbatim: *"shift browse push gets
the source menu yes!  please also make shift load 1 the track filter and shift
load 2 the shortcut menu"*. Both are bound to rbp's **own** keycodes — `0x420f`
K_TRACKFILTER and `0x0210` K_SHORTCUT — derived from a complete 229-entry
id → `UiKey_*` table recovered from `InitUiBrowseKey`, and named by rbp's own
`ui::KeyInput::keyCodeAsText()` as `"TrackFilter"` and `"Shortcut"`. Both also
appear in the RX3's own panel protocol
(`ui::panel_protocol::EupRxDataCheck::checkOtherData`) and in
`ui::KeyTestMode::onKey()`, so they are codes a real panel sends rather than
touch-only widgets, and `0x420f` is additionally in
`ui::PcControlKeyData::getMidiDataFromKeyCode()` — rbp's own PC-control table —
i.e. reachable from outside by design.

On the glass they are browse **modes**: `0x420f` opens the track-filter panel
(mode **8**) and `0x0210` the shortcut view (mode **10**).

**They are not the same kind of binding, and the difference is the whole of this
section.** `0x420f` does nothing at all when sent as a press and a release — and
not because rbp rejects the keycode. rbp delivers it to the handler and the
handler refuses it, and the mechanism is in rbp's own bytes:

* `KeyManager::sendKey` writes the **op** into the low nibble of the key record's
  `byte[11]`, and every `onKey` reads it as `ands rN, rN, #15`.
* `BrowseUiIf::InputKey` turns that op into two booleans and passes them through
  unswapped — `b1 = (op == 0)`, `b2 = (op <= 1)`. So **op 0 is the only press**,
  **op 1 is the pair (0,1)**, and **op ≥ 2 is (0,0)**.
* `UiKey_KeyPush` reads those two and writes the record's **state**: op 0 takes it
  to **2**, op 1 takes a non-zero state to **3**, and op ≥ 2 writes **no state at
  all** and clears the flags. Every call also zeroes the flags word.
* `BrowseKeyProcessing` then fires `table[kind].handler` for every record whose
  state is non-zero. It does **not** clear the state; `KeyComplete` sweeps all 230
  records to 0 (and a handler may call it itself).

Every browse handler acts on **any** non-zero state, which is why a press and a
release have always been enough — for all of them but one.
**`UiKey_Filter` is the exception**: at `0x118af8` it runs
`ldr r3, [r4, #8]; cmp r3, #2; bhi`, and only above 2 does it reach
`ChangeBrowseMode(8)`. A press+release pair can reach state **2** and no further,
so rbp's track filter is a key that **cannot be opened by a tap**. The third edge
— op **1**, what this tree calls `OP_REPEAT` — is what lifts the record to 3.

That op 1 is a *third* thing and not a release is not an inference: rbp's own
decoder says so. `BrowseUiIf::TouchPreviewProc` branches `op == 0` to start a
preview, `op == 6` to a seek, and `op - 2 <= 1` (unsigned) to *stop* one — so the
release family is exactly **{2, 3}**, and 1 is deliberately outside it. `sendKey`
generates no repeats of its own: one op is one dispatch, and its other two
messages are notifications, not ops. So a surface has to **synthesise** the
middle edge, which is what this map now does.

**Measured, live, on the unit.** With the swipe-down menu's bench knob carrying
`0x420f` on a column, a spin-sampler reading rbp's own record table out of
`/proc/<pid>/mem` caught the whole refusal (m29, 2026-09-30):

```
probe -> key 0x420f           pxdiff=190
  sample 0       mode=3  record26.state=0
  sample 297072  mode=3  record26.state=2   <- the handler IS reached: press -> 2
  sample 297090  mode=3  record26.state=0   <- release writes no state; KeyComplete sweeps
probe -> key 0x0210           pxdiff=177335
  MAX: mode=10                             <- UiKey_Shortcut acts on any non-zero state
```

`190 px` is the UI's own free-running drift, not a reaction, and the mode never
left **3**: the key arrived and was refused, in ~84 µs, exactly as the bytes say.

**The map's half of the fix is one function.** `ctrl_map.c`'s `add_note_repeat()`
is `add_note()` plus a flag, and `map_flx4.c` sends `OP_REPEAT` between the press
and the release on the rows that set it — today the note-104 row alone, because
`K_TRACKFILTER` is the only key in this map whose handler gates on state 3. It is
a **separate function rather than a fifth argument to `add_note()`** on purpose: a
row added for a handler that does not need the edge is a change in rbp's
behaviour, not a no-op, so it has to be asked for. It also goes out on the *press*
rather than the release, because rbp reads the record's state and not which edge
carried it — by the release the state has already been swept, and sending it there
would also leave a key stuck half-pressed if a note-off never arrived.

**Proven by injection, and as an A/B.** The two notes were injected into the
shim's own sequencer port (`/tmp/menu30.sh`, 2026-09-30) with the same navigation
and the same injection run twice: once against the shim deployed before the change
and once against the new build. Only the shim differs between the arms.

| inject | old shim | new shim |
|---|---|---|
| `note 6 104` (SHIFT + LOAD 1) | `unmapped ch6 note104`, **0 px**, mode stays **3** | `ch6 note104 -> 0x420f press+repeat`, **631 664 px**, mode **3 → 8** |
| `note 6 122` (SHIFT + LOAD 2) | `unmapped ch6 note122`, **0 px**, mode stays **3** | `ch6 note122 -> 0x0210 press`, **702 207 px**, mode **3 → 10** |

and the frames are the two screens by name — the 104 capture is headed
**TRACK FILTER** (BPM/KEY/RATING/COLOR, `-RESET`, `BPM/KEY` / `MY TAG`) and the
122 capture **SHORTCUT** (DECK SETTING, LCD BRIGHTNESS, VINYL SPEED ADJUST, MIXER
SETTING, MY SETTINGS, WAVEFORM COLOR). The old shim's arm is the control: the
notes are *unmapped* there, so the run's own instrument is shown to be capable of
reporting nothing.

**What this does and does not settle.** Injection proves the **binding** — the row,
its channel, its keycode and its edge sequence. The operator's own presses, on
2026-09-30, proved the **note numbers** 104 and 122, and they proved them twice
over: both screens came up under the finger, and `RB_MIDI_DUMP` caught the wire
itself — `NOTEON ch=6 note=104 vel=127` at 8.749 s and `NOTEON ch=6 note=122
vel=127` at 23.433 s, each with its release ~150-200 ms later, in a file whose
whole velocity histogram is 86 x `vel=0` and 82 x `vel=127` with **no `vel=100`
anywhere** — so nothing injected is in it and every one of those events is a hand
on the panel. The screen is the corroboration, not the proof: had either note
number been wrong, nothing would have fired at all (`K_TRACKFILTER` and
`K_SHORTCUT` are each bound by exactly one row in the whole map, both on ch 6),
and a wrong one shows up as an `unmapped ch6 note…` line rather than as a mystery,
which is exactly how the old shim's arm reads above. Both screens are browse
modes, so both are reached from
the browse screen; whether they should carry the gesture to the browse screen from
anywhere is the operator's call and is not implemented.

### Beat FX (list ch 5 → rch 4, with a second leg on ch 6 → rch 5)

| Control | Message | rbp | note |
|---|---|---|---|
| FX ON/OFF | note 71, one leg per target channel | `0x448d` K_BFX, global | footnote *4: its LED blinks on NOTE ON and lights on NOTE OFF |
| FX SELECT | ch 5 note 99 (+SHIFT note 100) | `0x448b` K_BFXTYPE, global | steps rbp's 14-position effect switch: 99 forward, 100 backward |
| BEAT ◁ / ▷ | ch 5 notes 74 / 75 | `0x4490` K_BEATPREV / `0x4491` K_BEATNEXT, global | op must be `OP_PRESS` — rbp gates these on `(op & 0xf) == 0` |
| LEVEL/DEPTH | CC 2, on **both** ch 5 and ch 6 | `0x448f` K_DEPTH, global | see below |
| FX CH SELECT | ch 5 note 16 (leg A) / ch 6 note 17 (leg B) | `0x448c` K_BFXCH, global, `OP_VALUE` 0 / 1 / **5** | the lever's third position is sent as **MASTER** — see below |

Three of these need their reason stated rather than just their value:

* **The FX CH SELECT lever is two notes, not a position.** CH1 = leg A lit, CH2 =
  leg B lit, CH1&CH2 = both. The other two notes of that group (ch 5 note 17,
  ch 6 note 16) are OFF in every position the list gives, and a mid-slide with
  both legs momentarily off is ignored rather than read as a position. The whole
  group is re-sent on every move, so only a change is a move — and what is
  compared is the **lever's position**, not the last keycode rbp was sent,
  because the three positions do not map one-to-one onto what rbp can be told.
  rbp's Beat FX has a *single* target channel: CH1 and CH2 are its two players
  and **CH1&CH2 has no value of its own**, so that position is sent as
  **MASTER** (`0x448c`, value **5**, `BFX_CH_MASTER`) — the output both decks
  reach. That is the operator's choice for the position, and it is the value the
  previous unit's map sends for its Main position
  ([08](08-controls.md#channel-assign)). Tracking the position rather than the
  keycode is what makes the move *out of* CH1&CH2 land: stepping back down to
  the deck rbp was already told about is a change of position even though the
  value it ends on is one rbp has seen, and a map that compared only the last
  keycode would treat it as no move and leave MASTER engaged. **Measured
  2026-09-30: the surface volunteers nothing here at connect, and rbp's own
  default is deck 1** — a cold start reads `CH SELECT` **1** with zero lever
  traffic in the run, and a restart that followed an injected `deck 2` came back
  at **1**, so the previous session's target is not carried over and
  `XdjSettings.dat` is not the carrier. Leave the lever at CH 2, power on, and
  rbp shows and acts on **deck 1** until the lever is nudged. The map acts on the
  first message that names a position, whether that is a move or the device
  volunteering its own state — and the surface never volunteers this one (item 7
  below).
* **The Level/Depth knob's channel is a disagreement in the vendor's own
  table**: its Channel column says 6 (list ch 6), while its own MIDI-IN status
  byte says `B4` (list ch 5 / `0x40` below). Both candidates are bound to the
  same key so the knob works either way, and the dump settles which one the unit
  sends. `TODO: unverified`.
* **FX SELECT steps — both ways — from the effect that is already playing.**
  rbp's effect switch has 14 positions and the unit's FX SELECT is a *button*,
  so the map keeps the position and steps it: note 99 forward, and its +SHIFT
  twin (note 100) backward, which is the RX3's FX-select rotary's two directions
  on one control. The position is seeded on the first press from what rbp is
  already on — `rbp_bridge.h`'s `rbp_beatfx_type()` (which calls
  `ADDR_GET_BFX_TYPE`) plus `rbp_abi.h`'s `bfx_type_to_pos[]`, the same table
  `map_jp21.c` seeds its rotary from — so the button carries on from the current
  effect rather than jumping to position 0, which on a live deck is an audible
  change of effect. `map_flx4.c` still holds no rbp address, and that is what
  makes the seeding something `test_flx4.c` can *stub* and pin; if rbp has no
  effect engine to ask, the cursor starts at 0, which is what every first press
  did before this was seeded. Both note numbers are unverified — see row 13 of
  [the open questions](#open-questions).

`flx4_startup()` also deliberately does **not** force the Beat FX target to
MASTER, which `map_jp21.c` does, because there the assign knob is a position
encoder with no state of its own. Here the lever has state, and forcing it would
fight the operator. `TODO: unverified` — what rbp's default target is.

### What `flx4_startup()` does tell rbp

Three things, before the first event, because anything sent earlier is dropped
by rbp's own mixer init:

1. **The mixer routing** — `Mixer Ch1 → Deck1, Ch2 → Deck2`, two absolute words
   named in `rbp_abi.h` (`ADDR_MIXER_ROUTE_PLAYER0/1`). The RX3 does this from
   its physical DECK/LINE switches and no control surface has any, so the
   engine's default (channel 2 on player 0) is what playback would otherwise
   inherit. This is the same work `map_jp21.c`'s startup does, and it is the one
   place this map touches a raw address.
2. **The Sound Color FX type** — Filter, on both channels, with both knobs
   centred. The unit has no CFX type buttons, so a knob would otherwise move a
   control with no effect behind it. Filter is the choice because it is the one
   effect a single knob can express without a parameter. `TODO: unverified` —
   there is no way to change it from the unit, so it is a fixed choice rather
   than something the operator can correct.
3. **rbp's master level at unity** — for the MASTER LEVEL reason above.

Deliberately **not** there: `me_set_master_cue()` and `me_set_stereo()`. Those
are asserted by `rbp_vu.c`'s `vu_thread()` on every target, meters or not, on
its own wait for rbp's mixer — a second assertion from a map would be two
writers for one piece of engine state. It matters more on this unit than on the
previous one, because the FLX4 **has** a MASTER CUE button: a startup assertion
would fight the operator for it.

## The LEDs

**The machinery is in place, and the notes are filled in one row at a time** —
this paragraph used to read "and the notes are not measured yet", which was true
when it was written and is not now: the cue LEDs, the four pad-mode lights and the
eight pads are all measured and driven, and the rows that are still unknown are
the ones that still read `-1`. The four pad-mode lights are confirmed **on the
panel** too, and by the only route that can tell our bridge from the panel's own
doing: a press on the button lights it device-locally, so the mode was instead
walked from the shim's sequencer port — the panel heard nothing — and the operator
watched the lit one step across the four and back while deck 2's row stayed put
(2026-10-01; the run is in `docs/13` S9.9). Two of those measured rows do not behave like the
rest and are worth knowing before reading the table: the **pads' outgoing note
follows rbp's mode**, because this unit re-addresses its eight pads when a
pad-mode button is pressed, and their **colour cannot be sent at all** — this
panel's pads are on/off (see [the pads](#pads-list-ch-810--rch-79)). The panel's
numbers now live in the selected map's `struct led_notes` (`map_flx4.c`) rather
than in `rbp_led.c`, so the bridge holds no note number at all, and a row that
reads `-1` means "this surface has no such LED" and transmits **nothing**. That
is what makes `RB_LED_DISABLE=0` safe to ship before the numbers are known: the
bridge runs, and every FLX4 row is `-1` until it is *seen to light*. Filling a
row in is then a one-line edit. See [08 — controls](08-controls.md#led-output)
for the machinery, and `ctrl_map.h`'s `struct led_notes` for the table's shape.

What the vendor list documents:

| Illumination | Message | list ch |
|---|---|---|
| LOADED (track-load illumination) | note 0 / 1 | 16 |
| VINYL MODE (footnote *2: only the *application* can set it) | note 23 | 1 / 2 |
| CH LEVEL METER | CC 2 — **a value ramp, not a bitmask** | 1 / 2 |

The meter is the one that does not fit the **SC Live 4's** bridge, and it needed a
second encoding rather than a new bridge: its value is a level,
ramped `0x26-0x40` green1, `0x41-0x56` green2, `0x57-0x64` orange1, `0x65-0x76`
orange2, `0x77-0x7F` red, where rbp's own meter is an 11-segment **bitmask** and
the shim's rescale assumes a segment count. The list's type column calls that row
a NOTE while its own status byte is `B0`, a control change; the status byte is
what the wire carries. **This row is now wired and live** — the kind is read from
the map's own meter row (`meter_enc = METER_ENC_FLX4_LEVEL`, pre-fader, no master
meter), so `rbp_vu.c` no longer hard-codes the Prime's numbers, and the ramp was
confirmed on the panel by the operator's own eye with `VU_TEST=1` on 2026-10-01.
The band *boundaries* are the list's published values. (The link in the sentence
below used to name a file that does not exist — `09-raspberrypi4-audio.md`; the
page is [09](09-audio.md).)

The pad **colours** are not unknown, they are **absent**, and the difference
matters: this unit's pads are not RGB at all. Pioneer's list gives all 350 of its
LED rows as a plain `OFF=0x00, ON=0x7F`, and the only row in the document with a
value *range* is the CH LEVEL METER — which this map already models as a meter.
So the FLX4 table's `pad_enc` is `LED_ENC_NONE` as a **final** value: the RGB rbp
hands over for a pad is read, discarded, and any non-zero velocity lights it.
`RB_PAD_BRIGHT`, the Engine OS Prime 6-bit layout, is inert for this target
permanently rather than pending, and `test_flx4.c` fails the suite if a row ever
claims that encoding — borrowing the SC Live 4's colour layout for a panel that
has no colour to show.

**What decides lit-or-dark, then, is not the colour.** rbp keeps every pad's
`State` at 1 in HOT CUE, so a State-only rule lights all eight pads and an
unassigned one is indistinguishable from a loaded one — which is what the
operator's panel showed. The distinction lives in the word four bytes past
`State`: 1 for a pad with nothing assigned, 0 for one that has been given a
colour. Measured 2026-10-01 (rbp_abi.h's `LED_ENTRY_OFF_UNASSIGNED` has it in
full), and it is about the pad and not the mode — in AUTO BEAT LOOP all eight
carry 0. An unassigned pad is sent dark.

**White is not the mark of an empty pad, and this is where that got corrected.**
The rule is the flag, not the colour: in HOT CUE the empty pads read white *with*
`unassigned=1`, but in SLIP BEAT LOOP two pads read white with `unassigned=0` —
a white rbp coloured deliberately — and the HOT CUE pads the operator loaded read
`1aff00` / `ff8c00` with 0. So a colour can never stand in for the flag, in either
direction.

**And the third input is `State`, which is what says *something is happening on
this pad now*.** In AUTO BEAT LOOP all eight pads read `State=1` and the
**engaged** one reads **`State=3`** — measured 2026-10-01 on a paused deck with a
track loaded, by moving the loop and watching the 3 move with it (pad 5 the "4"
size → id 22, then pad 7 the "16" → id 24 with id 22 back to 1, then released →
all eight back to 1), and independently by the operator's own finger (pad 3 →
`3` at 13:47:07, caught in the same watcher). rbp's own screen marks the same cell
with an orange outline.

That made a real defect, and it was the operator's own report: *"what's the
behavior for beat loop pads? aren't they supposed to flash when engaged? i can't
remember."* The pad path treated `State=3` the same as 1, so **all eight pads
looked identical whether or not a loop was running** — the one place the active
loop was invisible was the panel. The pads now blink on `st == 2 || st == 3`.
Blink is a rendering **choice** and worth naming as one: this panel has no
brightness (every LED row is `0x00`/`0x7F`), so "dimmer than its neighbours"
cannot be sent and blinking is the only way to make one of eight differ.
**Measured on the wire and confirmed on the glass, 2026-10-01.** The engaged
pad's velocity alternates in **runs of four sends** — 400 ms on, 400 ms off,
which is `led_tick & 8` at the 50 ms tick with the 2-tick resend — while its
seven siblings hold solid runs of 609 to 1124 sends with no alternation at all,
and every blink landed on the pad whose LedStat id rbp had actually set to 3, in
three different modes (id 20 → note 18 in AUTO BEAT LOOP, id 19 → note 33 in SLIP
BEAT LOOP, id 21 → note 3 in HOT CUE — the per-mode base moving underneath it,
16 to 32 to 0). Asked whether the pad they pressed was the one that flashed, the
operator: *"yes only the pad i pressed flashed."* Note
also that **"dim" is the shim's name for 3, not a measurement** — it stands for
the deck LEDs, where 3 still counts as on and is deliberately left alone.

**The cadence moved on 2026-10-06, for State 2 only.** The three LED entries rbp
marks State 2 carry a blink period of their own at `LedStat` entry `+28` (deck 1
PLAY 500 ms, deck 2 PLAY 250 ms, CfxFilter 250 ms), and the shim now renders the
blink at rbp's period rather than at its own counter — [08](08-controls.md) has
the reading and [17](17-rx3-flx4-comparison.md) §4 item 2 the port. **State 3 is
unchanged**: rbp gives the engaged loop pad a state and a colour and *no* period,
because its intent is dimmer and not faster, so the pad keeps the old 800 ms
fallback (400 on / 400 off) — the four-send runs measured above. That is why the
fallback was deliberately left at 800 rather than re-chosen: the before/after of
this change is legible only if the pads that were not asked to blink do not move.

### Measuring the notes: the route, and the loopback result

Two things had to be settled before any note could be probed, and both were
measured on the unit on 2026-09-26.

**`amidi` cannot be used on this unit.** The obvious tool —

```bash
amidi -p hw:1,0,0 -S "90 54 7F"        # does NOT work here
```

— fails with `Device or resource busy`. The kernel's `snd_seq_midi` holds the
rawmidi node once the sequencer attaches it, so a userspace open of `hw:1,0,0`
is refused even though `amidi -l` lists the port. **The probe must go through the
sequencer**, which is also the route the shim's own LED writes take, so a probe
through it measures what the shim can actually do:

```bash
# seqinject2 is built by scripts/shims/Makefile (`make` builds it; it is not
# deployed with the shims -- copy it to /tmp on the unit)
scp scripts/shims/seqinject2 root@<unit>:/tmp/ && ssh root@<unit> chmod +x /tmp/seqinject2

# /tmp/ledprobe.sh names the candidates and sweeps a channel:
#   sh /tmp/ledprobe.sh cand          # the named candidates, 3 s each
#   sh /tmp/ledprobe.sh sweep <ch>    # every note 0..127 on one channel
#   sh /tmp/ledprobe.sh pair <ch> <n> # one note on/off, for a single check
```

**A write to the device port does not loop back into the shim's input.** This is
the property that decides whether the whole feature is safe, because the shim
reads and writes the *same* port (`20:0`): if the shim's own LED writes were
delivered to its input port, every LED message would arrive as a phantom button
press. Measured, three ways:

| Test | Result |
|---|---|
| the device echoes what is sent to it | **no** — a write to `20:0` never comes back out |
| a write to `20:0` reaches the shim's input port | **no** — 0 new lines in `/tmp/knobshim.log` |
| a *real* button press does reach it | **yes** — the log is full of them |

The third row is what makes the second meaningful: the subscription
(`20:0` -> `rbp-knob2-in`) demonstrably delivers, so a write that never appears
there is genuinely not being delivered rather than being lost on a dead route.
The sequencer's own port listing is the explanation — `seqinject2`'s attempt to
subscribe to the shim's out port fails with `EPERM`, because `rbp-knob2-out`
advertises `R-e-` (not writable) while the device port `20:0` advertises `RWeX`:

```
Client  20 : "DDJ-FLX4" [Kernel Legacy]
  Port   0 : "DDJ-FLX4 MIDI 1" (RWeX) [In/Out]
    Connecting To: 128:0
    Connected From: 128:1
Client 128 : "rbp-knob2" [User Legacy]
  Port   0 : "rbp-knob2-in" (-We-) [Out]     Connected From: 20:0
  Port   1 : "rbp-knob2-out" (R-e-) [In]     Connecting To: 20:0
```

**Three of these are measured as of 2026-10-01, and they are the first LEDs this
surface has ever lit.** The route was a one-note SMF played into the panel from
the host — `aplaymidi -p 28:0` — which is the sequencer path the shim's own writes
take, and the same reason the rawmidi one-liners above do not work here:

| Control | Note | Channel | Velocity |
|---|---|---|---|
| CH CUE deck 1 | 84 | 0 | `0x7F` on, `0x00` off |
| CH CUE deck 2 | 84 | 1 | same |
| MASTER CUE | 99 | 6 | same |

So the hypothesis holds, and more precisely than "the same note as its input":
**a control's LED uses that control's own input note on that control's own input
channel.** Deck 1's CH CUE arrives as note 84 ch 0 and its LED is note 84 ch 0;
deck 2's is 84 on ch 1; MASTER CUE arrives as note 99 on ch 6 and its LED is 99
on ch 6. All three were then wired into `flx4_leds` and confirmed on the glass by
the operator the same day, both edges, each LED.

**And the off is a Note On with velocity 0, never a Note Off.** This is the one
thing about LED addressing that the previous target does not teach: the FLX4's LED
hardware **ignores a real Note Off**. Measured directly — deck 1's CH CUE LED lit,
`90 54 00` put it out, and with that LED lit again `80 54 00` left it on: same
channel, same note, the status byte the only difference. `midi_io.c`'s
`midi_note()` had always built off as `0x80 | ch`, which is the whole of "the
LEDs stay lit when I turn the cue off" — the off went out on the wire and the
panel did nothing with it. It now sends a Note On for both edges, so velocity 0 is
the off. That is also the MIDI spec's own definition of a Note Off, which is why
there is no per-surface switch: a conforming surface must accept it, and this one
requires it.

**What is still unmeasured is the rest of the panel**, and a note number that has
not been *seen* to light must stay `-1`. Nothing in the test suite can see an LED,
so the glass is the only instrument that can move any row out of this table:

| Control | Candidate | Probe |
|---|---|---|
| BEAT SYNC deck 1 | note 88 on ch 0 | `sh /tmp/ledprobe.sh pair 0 88` |
| hot-cue pad 1 deck 1 | note 0 on ch 7 (list ch 8) | `sh /tmp/ledprobe.sh pair 7 0` |
| PLAY / CUE deck 1 | notes 11 / 12 on ch 0 | `sh /tmp/ledprobe.sh pair 0 11` |

**Do not guess a note that has not lit.** A wrong guess here is not a dark LED —
the SC Live 4's pad notes are `15 + pad` on channels 4/5, and this unit's Beat FX
section has notes 16/17 as the FX CH SELECT legs — so a copied number is a
**phantom control press**. `test_flx4.c` refuses any FLX4 row carrying a
`(channel, note)` pair the SC Live 4's table already uses, and refuses a row
whose channel base is `-1` while its count is non-zero (which would put strip 1
on channel 0).

While the notes are unmeasured, `RB_LED_DISABLE=0` is nevertheless the right
setting: the bridge is on, and it transmits nothing.


## Step 1 — dump the surface

```bash
# aseqdump needs alsa-utils; it reads the same port the shim does
aseqdump -p <client>:0 | tee /tmp/flx4-aseqdump.txt
```

Press every control once, in order, and note what each produces. `aseqdump`
gives you the message type, channel, and controller/note number, which is most
of the map — and it is the thing that settles the channel conversion and every
`TODO: unverified` above.

Pipe it through [`tools/aseqdump2dump.py`](../tools/README.md#aseqdump2dump--turning-a-capture-into-a-fixture-and-a-table)
to get the inventory and the arithmetic instead of reading the stream by eye —
and, with `-o`, a replayable dump without the shim or `rbp` running at all:

```bash
aseqdump -p <client>:0 | python3 tools/aseqdump2dump.py --stats --revs <N> -o flx4.dump
```

Press in a known order, because the report lists the controls **in the order
they first appeared**, which is what makes it readable against the press order.
For a relative control the report decodes the values under both conventions —
the **0x40-centred** one this unit's jog uses and the **`0x01`/`0x7F`** one its
browse knob uses — and says which the data supports, which is a measurement of
the convention rather than a quotation of it. A verdict is only printed for a
control that actually behaves relatively — a fader decodes to large steps under
*both* conventions and gets no arithmetic, because "counts per revolution" for a
crossfader is a number about nothing.

For `JOG_PPR`, turn the platter a counted number of revolutions in one direction
and pass that count as `--revs`; the counts-per-revolution arithmetic is printed
for the platter's own CC (`--jog-cc`, default 34). Turn **ten** revolutions
rather than one: a single hand turn is too coarse to divide, and the answer this
procedure gives is only as good as the count you turn. S5.1's answer was 720
(7183 counts over ten turns — see [Step 3](#step-3--calibrate-the-continuous-controls)).

Then dump through the shim itself, which is what the map actually has to
consume — it sees the same events, after the shim's own filtering. This is the
route to a dump with **real timestamps**; `aseqdump` prints no clock, so a
converted dump's times are synthetic (see the tool's README section for why that
is enough for the fixture test):

```bash
# in rb.conf, or as a one-off:
RB_MIDI_DUMP=/opt/rblive4/log/midi_dump.log sh /opt/rblive4/start-rb.sh
```

The dump is written **above** the "is `rbp`'s KeyManager up yet" early-out
deliberately: during bring-up the KeyManager is often not up, and a dump that
depended on it would be empty exactly when it is most needed.

A dump is replayable:

```bash
MIDI_REPLAY=/opt/rblive4/log/midi_dump.log sh /opt/rblive4/start-rb.sh
```

`MIDI_REPLAY_SPEED` scales the inter-event delays (default 1.0; `0` ignores
them). This is what makes the map regression-testable on a bench with no
controller attached — and it is how the fixture test in `make test` works.

## Step 2 — turn the dump into a table

For each control record: message type, channel, number, and — for anything
continuous — whether it arrives as a 7-bit CC, a 14-bit CC pair (MSB/LSB), or a
relative encoder. The tables above are the map's answer to that; a dump either
confirms a row or corrects it, in which case both `map_flx4.c` and this document
change together.

Two rows in the tables are the ones to check *first*, because they are the ones
whose failure mode is silence rather than a wrong button: the **channel base**
(every deck control at once) and the **pad base+pad encoding** (every pad at
once).

## Step 3 — calibrate the continuous controls

Jog and pitch reuse the existing unwind into `sendKey(0x4305, OP_ROTATE)` and
`0x4109`. What must be calibrated, not assumed:

| Value | Meaning | How |
|---|---|---|
| `RB_JOG_PPR` | jog pulses per revolution | **measured: 720** (S5.1). `tools/aseqdump2dump.py --stats --revs 10` over a counted ten-revolution turn gives counts/revolution directly; one hand-turned revolution is too coarse to divide |
| `RB_JOG_REV` | which way the platter's counts run | turn it clockwise; the tempo must go up |
| `RB_JOG_IDLE_MS` | how long a pause means "stopped" | lower it if the deck bends on after your hand leaves, raise it if it stutters |
| pitch polarity | whether increasing CC means faster or slower | push the fader down and watch `TEMPO_VERBOSE=1` output: the tempo must *decrease*; if it rises, `RB_TEMPO_REV=1` |
| pitch resolution | steps across the fader's travel | move it end to end and count |

`RB_JOG_PPR` used to ship as **128** — the *previous unit's* value. Get a rate
like that wrong and everything still "works": the deck just responds at the wrong
rate, which is much harder to diagnose than a control that does nothing. The measured
value is 720: a ten-revolution turn produced 7183 forward counts (6509 of +1, 337
of +2, 17 back), or 718.3 per revolution. 718 would be false precision — 720 is
the plausible design value and the 0.24% shortfall is under 9° of arc at the end
of a hand turn. With 128 in place every turn was 5.6× too fast and pinned to the
8 rev/s clamp in `flx4_jog()`, which is what "the platter only ever spins at one
speed" would have looked like on the device.

The remaining calibration unknowns are the ones the S5.1 capture could not settle
by itself: `TEMPO_VERBOSE=1` and `JOG_VERBOSE=1` (`RB_VERBOSE=1` sets both) log
the raw value and the resulting normalised one side by side.

## What the FLX4 does not have

Two absences drive design decisions rather than just config:

* **No *master* meter — and the channel meters it does have are a different
  kind.** The bullet this replaces said "no level meters", which was wrong:
  Pioneer's list documents a **CH LEVEL METER on CC 2**, one host-driven level
  per deck channel (0/1), **pre-fader** like rbp's own. The bridge's shape was
  the second half of the mistake — the conclusion that a value ramp cannot be fed
  to a bridge built on rbp's 11-segment bitmask. It does not need to be: only the
  *value sent* differs, and that is now the surface's own row (`meter_enc`, here
  `METER_ENC_FLX4_LEVEL` against the Prime's segment mask), so the FLX4's meter
  is **live as of 2026-10-01**, `install_meter_hook()` patches
  `rbp`'s `MonoLvMeter::getLedValue` prologue in machine code for it, and it was
  **confirmed on the panel by the operator's own eye** (`VU_TEST=1` stepping both
  channel meters). What is absent is a **master** meter: the list gives no
  separate master address, so `meter_master_ch` is `-1` rather than an invented
  one. `RB_LED_VU` is only a manual OFF switch now — it ships `1`, and a surface
  whose table declares no meter row gets no hook installed and no traffic
  whatever the flag says. `RB_LED_VU_SEGMENTS` is *rbp's* meter height, not the
  panel's, so it is not the knob for a panel that meters differently. It does
  *not* switch off the master cue below — that is engine setup rather than meter
  work, and `vu_thread()` asserts it on a surface with no meter row too, on its
  own wait for `rbp`'s mixer.
* **No master-cue *keycode*.** `rbp` has no PFL keycode either — a channel CUE
  goes through `me_set_cue(ch)` on the mixer engine — so `vu_thread` asserts
  `me_set_master_cue(1)` and stereo cue mode at startup on **every** target, and
  the unit's own MASTER CUE button toggles the same engine state from the map.

Pad illumination stays on (`RB_LED_PADS=1`) and `RB_LED_DISABLE=0` is now the
setting — the switch no longer waits on anything, because an unmeasured row
transmits nothing. What did wait on the notes was the *lighting*: see
[the LEDs](#the-leds).

### What the unit cannot reach on rbp

The reverse absences matter too, and they are why the pointer path
(`fbshim.so`, [07 — touch](07-touch.md)) exists at all. The FLX4 has no control
for: BACK, SOURCE, MENU, VIEW, INFO, TAG LIST, USB1, REKORDBOX and every other
menu key; MASTER TEMPO (key lock), tempo range, VINYL mode and SLIP (on an FLX4
these are *application* settings — footnote *2 says so about vinyl mode, and the
VINYL MODE LED is documented as something the application sets); the Sound Color
FX type; and a SAMPLER, a KEYBOARD and a PAD FX that rbp has no pads for at all
(see [Pads](#pads-list-ch-810--rch-79) for what the pad-mode buttons whose names
have no rbp counterpart are used for instead — and note that since 2026-10-01 the
row is chosen by **position**, so no button's name decides what it selects).

## The keyboard fallback

The keyboard map (`EVDEV_MAP=kbd`, the default; `MIDI_MAP=kbd` alone for its old
meaning of "no controller at all") needs nothing plugged in: space/z/x are
PLAY/CUE/SYNC on deck 1, 1/2 LOAD, arrows plus Enter drive the selector,
Backspace is `K_BACK`, Esc is `K_SOURCE`, and the rest of the digit row is the
browse screen — 5/6/7/0 are SOURCE/BROWSE/TAG LIST/MENU. Mouse right-button is
`K_BACK` and the wheel rotates the selector. Deck 2 is n/m/, and the full table —
with the status of each binding, including the two keys declared but with no
keycode behind them — is in
[08 — Controls](08-controls.md#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone).

It exists so that display and audio can be brought up and tested with an empty
USB bus, and so the port is *playable* while the FLX4 tables are still
unverified — which is the state they are in. It is not a degraded mode — it is
the fastest path to "the display works, now play something".

Unlike every other map it does not go through the sequencer at all: its events
come from `/dev/input/event*` (`evdev_io.c`), which is also why a missing
`/dev/snd/seq` does not stop it. It has been written, cross-compiled and
fixture-tested, and **it now runs on the Pi**: the reader is selected by
`EVDEV_MAP`, which defaults to `kbd`, so it is live alongside the FLX4 rather than
instead of it, and it has been driven on the unit through a virtual keyboard
([16](16-input-and-hotplug.md)). (The same reader's *pointer* half is a different
module in `fbshim.so` and has always been active — see
[07](07-touch.md).)

## Open questions

None of these can block the port — the keyboard map and the fixture tests keep
everything else moving — but each one changes the map, so they are collected
here. The first two are the ones to settle before anything else, because their
failure mode is a whole section doing nothing, and each row now says whether it
is still open or has been answered (with S5.1, the `aseqdump` capture).

| # | Question | How it gets answered |
|---|---|---|
| 1 | **The MIDI channel of each section** — the single conversion the whole map rests on. The list is 1-based, the map is 0-based. | **Answered in S5.1.** Deck-2 PLAY arrived on ch 1, and MASTER CUE — list ch 7 — on ch 6: the list is 1-based and the map's conversion is right. The same capture confirmed PLAY note 11 on both decks, CH CUE note 84, MASTER CUE note 99, the jog touch note 54, and that CC 34 is 0x40-centred rather than `0x01`/`0x7F`. |
| 2 | **The pad base+pad encoding** — whether `base + pad` and the eight bases are what the unit sends, and whether the four +SHIFT modes pair with the four shift notes the way the list's ordering implies. | **Partly answered in S5.1**: pad 1 in HOT CUE arrived as note 0 on list ch 8 (rch 7), so HOT CUE's base is 0 and pad 1 is +0. Four bases are now bound (0, 16, 32, 48 — [Pads](#pads-list-ch-810--rch-79)), which makes the encoding load-bearing rather than dormant, and the two the map newly uses are the two nobody has looked at: **16 and 48 are still published values.** Press one pad in each of the four bound modes with `aseqdump` running and read the four notes off; `LED_VERBOSE=1` shows what rbp then does with each. The other four bases stay unbound, so nothing rests on them. **The +SHIFT half of this row is now answered, 2026-10-01**: the operator's own press put the shifted pad on the wire as `NOTEON ch=8 note=0 vel=127` inside a SHIFT-held window (note 63 down at 610.514), so the layer is ch 8/10 and the note is `base + pad` — the map's assumption, confirmed. Its HOT CUE base is bound and the delete's plumbing is measured on the unit. What is left is only the action: **a cue has to actually disappear.** The wiring is not a keycode at all — `0x4124` "CueDelete" deletes one fixed slot (`Player+0x234`, whose only writer in the binary is the constructor's `memset`), and the per-pad delete that *does* take `K_PAD1+p` needs the selector word `[innards+0x38] == 3`, which rbp clears on the key path before the handler reads it (sentinel 7 → 0, measured), so `hotcue_delete()` calls `onHotCueDeleteEvent(pad)` directly — **and, since 2026-10-01, `DjEngineIF::clearHotCue(ch, pad)` as well**, because that first call is only the UI and DB half: the engine half lives at exactly one call site in the binary, inside the *physical* `pad_HotCue`, and without it the cue is gone from rbp's data while the pad cell keeps drawing it (the operator's *"the cue is still there. the light does go off though"*). See the [+SHIFT paragraph](#pads-list-ch-810--rch-79). **The remaining question is the action, not the layer** |
| 3 | The pad-LED **velocity → colour** table. The encoder assumes the Engine OS Prime convention (bits 4-5 red, 2-3 green, 0-1 blue, 2 bits each); `PAD_BRIGHT=1` sets bit 6 for the bright range. | **Answered 2026-10-01, and the question does not apply to this surface: the FLX4's pads have no colour.** Pioneer's own *List of MIDI messages* (Ver 1.0, `DDJ-FLX4_MIDI_message_List_E1.pdf`) gives **all 350** of its LED rows as a plain `OFF=0x00, ON=0x7F` — there is not one value range among them — and the single row in the whole document that does carry a range is **3-15 CH LEVEL METER** (`0x26`..`0x7F`, banded), which this map models as a meter instead (`meter_enc`). So `pad_enc` is `LED_ENC_NONE` as a final value rather than a placeholder, the RGB is read and discarded, and `RB_PAD_BRIGHT` is inert for this target **permanently**. The RX3 behind rbp is RGB and this unit is not: that is a difference between the two panels, not a gap in the wiring. What the on/off then depends on is rbp's own *nothing assigned here* word beside the pad's State — every pad keeps States=1 in HOT CUE, so State alone lights all eight — see [the pads](#pads-list-ch-810--rch-79) |
| 4 | The **transport/pad/loop LED note numbers**, which the list does not give at all beyond LOADED and VINYL MODE. | **The tooling and the safety answer are both settled; the numbers are not.** `amidi` does not work on this unit (rawmidi is EBUSY — the sequencer holds the node), so the probe goes through the sequencer via `seqinject2`/`/tmp/ledprobe.sh`, which is the route the shim's own writes take. The loopback question is **answered: no** — a write to `20:0` is not delivered to the shim's input port, and the device does not echo (measured 2026-09-26; see [the LEDs section](#measuring-the-notes-the-route-and-the-loopback-result)). What remains is the measurement itself: send each candidate and watch. `RB_LED_DISABLE=0` is now correct **because** every unmeasured row is `-1`, so the bridge runs and transmits nothing |
| 5 | Jog `RB_JOG_PPR` (shipped as the previous unit's 128), `RB_JOG_REV` and pitch polarity/resolution. | **`RB_JOG_PPR` answered in S5.1: 720** (ten revolutions → 7183 forward counts → 718.3/rev; see [Step 3](#step-3--calibrate-the-continuous-controls)). `RB_JOG_REV`, polarity and pitch resolution are still open: the capture's jog turn direction and pitch push direction were not recorded, so neither sign can be read out of it. `TEMPO_VERBOSE=1`/`JOG_VERBOSE=1` on the device settle all three. |
| 6 | The **Level/Depth knob's channel**, where the list's Channel column (6) disagrees with its own status byte (`B4`). Both are bound, so the knob works either way — but one of the two rows is dead weight and should go. | the dump: the CC appears on one channel only |
| 7 | **Which position the FX CH SELECT lever rests in**, and rbp's default Beat FX target. The map forces nothing: it acts on the first message that names a position, which is either a move or the device volunteering its own state. | **The lever half is now answered by a hand: the panel reports nothing for it at connect.** Moved through all three positions by the operator on 2026-09-30, the map logged the three sends in order — `deck 1 (0)`, `deck 2 (1)`, `MASTER (5)` — with the dump's leg pattern underneath each (CH1 = `ch4 note16` alone, CH2 = `ch5 note17` alone, CH1&CH2 = both legs in the same millisecond), so CH1&CH2 → MASTER is proven under a hand and not only by injection. And the run answers the connect half the only way it can be answered: **the FLX4's opening events are controller values** — cc 33/39/7/64 inside the first second, the panel pushing the positions of its knobs and faders — **and no lever note at all**: the first lever event in the whole 4254-line run is the operator's own move. A note carries no value, so no panel can volunteer a three-position lever built from two of them, and a cold start therefore leaves rbp on whatever target it built itself with until the lever is touched. **The number that was still unknown — rbp's own default target — is now measured, and it is deck 1.** Two cold starts on 2026-09-30 (03:25:37 and 03:28:49 EDT) each put the Beat FX `CH SELECT` box at **1** with **zero** lever traffic in the run (`grep -ac "FX CH SELECT" /tmp/knobshim.log` = 0), read off rbp's own framebuffer rather than inferred. The first was with the lever left physically at CH 1 by the operator, so it alone cannot separate "built-in default" from "restored"; the second separates them, and it needed no operator. Leg B was injected through the shim's own port (`seqinject2 note 5 17`, the route the panel takes) and **rbp moved to 2** on the same screen — the positive control, so rbp really follows that note — and then the unit was restarted with no lever traffic at all. rbp came back at **1**. So **rbp does not carry the previous session's FX target across a restart**, and `XdjSettings.dat` is not the carrier: its mtime held at 02:07 through a target change to deck 2 and two restarts, so the target is not in its save-on-change set, and the restart that replaced the deck-2 process left the file untouched as well. **The consequence is an operator-visible mismatch at power-on**: leave the lever at CH 2 (or MASTER), bring the unit up, and rbp shows and acts on **deck 1** until the lever is nudged — because the panel volunteers nothing at connect and rbp builds its own default. That is one habit and not a defect: the shim cannot know a position the surface never names, and the FLX4 offers no lever state to ask for, a note carrying no value. In the injected run the lever itself was physically at 1 while rbp was told "2", so what is proven is the behaviour *given that last-named position is 2*, which is exactly the wire traffic a real lever-at-2 power-on produces; the literal version — lever at 2, cold boot, screen reads 1 — is one restart away and would only re-confirm it |
| 8 | Whether the BEAT SYNC release (footnote *3) carries the ON edge, the OFF edge or both. | **Answered in S5.1**: one press produced both edges (`on/off x1/1` on note 88), so the map's first-edge rule is load-bearing rather than merely defensive. The map would handle either order without double-sending, so which came first is for the record — the dump has it in order (`grep 'note=88' flx4.dump`). |
| 9 | rbp's **keycode for CUE/LOOP CALL** ◁/▷. This is **half-answered and half-bypassed**: the keycodes are still unknown, and ◁ no longer waits on them — it is `K_BACK` now, because the port had no BACK at all and a dead button was worth more as one. ▷ is still log-only, and finding either real keycode would let them be what they say they are. | rbp's own key table, or a keyboard/pointer session against the RX3 UI |
| 10 | ~~Whether the FLX4's USB audio is `S24_3LE`, `S16_LE` or `S32_LE`, and its native rate.~~ | **Answered in S1.4**, and it did change the shim: `aplay --dump-hw-params -D hw:CARD=DDJFLX4,DEV=0` reads `FORMAT S16_LE S24_3LE`, `SAMPLE_BITS [16 24]`, `FRAME_BITS [64 96]`, `CHANNELS 4`, `RATE [44100 48000]` — **no `S32_LE`**. The plan's `plughw:` default assumed the question did not matter, because the plug chain would convert whatever the answer was; the card's real count is what `AUDIO_MAP`'s hardware indices need, and a plug device reports 10000 for it (see [13](13-raspberrypi4.md) S1.4 and [09](09-audio.md)) |
| 11 | The **units of the jog's `l` field** (the 16-bit `pos` in the `OP_ROTATE` message). The map fills it with raw platter counts, so the measured 720 counts/revolution now goes into a field rbp may *compare* rather than difference — and the map inherited 128 from the JP21. `RB_JOG_SCALE` cannot express that conversion: it scales `vpos` but cancels out of the speed, and it is clamped to ≥ 1, so it can only make the units larger. | if the platter misbehaves in a way the speed cannot explain (a deck that jumps when touched), log `JOG_VERBOSE=1`'s `pos=` against a known turn and compare it with what rbp does |
| 12 | ~~**The note number the SHIFT + browse push actually sends**~~ | **Answered 2026-09-30, and the derivation was right.** `grep -c 'note=66 vel=127' /tmp/flx4.dump` is **12**, and each of the six presses falls strictly inside a SHIFT-held window (note 63 down/up at 2864.364/2865.404, 2865.940/2866.692, 2949.356/2951.236, 3419.741/3421.060) — and `note=65`, the unshifted push, appears nowhere, so the shifted note replaces the base note. See [the section above](#the-shift--browse-push-and-the-two-questions-it-is-two-questions-about). **The same one-press procedure settled the shifted LOAD notes 104/122 the same day, and the dump caught both at the panel's own velocity** — `NOTEON ch=6 note=104 vel=127` at 8.749 s (released 8.949) and `NOTEON ch=6 note=122 vel=127` at 23.433 s (released 23.572), in a file whose velocity histogram holds only 0 and 127, so no injected event is in it. The operator's presses also brought up both screens, which is the corroboration and not the proof |
| 13 | **The FX SELECT pair's note numbers**, 99 and its SHIFT twin 100 — the same kind of question as row 12, on the control that now steps the effect switch both ways. Both come from the vendor list; the SHIFT leg is the weaker of the two, because a wrong number there does not break the button the operator has been pressing, it just leaves the backward direction dead. | press FX SELECT three or four times and once with SHIFT held, then `grep -E 'ch=4 note=(99\|100) vel=127' /tmp/flx4.dump`. If the SHIFT press sends something else, the fix is one line in `map_flx4.c`'s `N_FX_SELECT_SHIFT` and one line in the fixture (`test_flx4.c` and `tests/midi_flx4.dump` name each other's numbers). |
| 14 | **Whether the FLX4 needs a keepalive for a LONG IDLE** — the 200 ms vendor SysEx is the sibling port's finding, not this port's. **Partly answered 2026-10-06:** it is deployed and going out at the measured 200 ms period, and a run with it sending played identically to the build without it, so it is not needed for LEDs, meters, faders, pads or touch. What is still untested is the half the claim is actually about — an audio path surviving a long idle, which a few minutes of use cannot show ([above](#the-keepalive-and-the-sysex-route-it-needed)). | A soak: controller attached, nothing playing, left alone past whatever idle timeout the device has, then check whether audio still comes out. The shim logs the first send, and every send under `RB_KNOB_VERBOSE=1`. **Read the log before any reboot — `/tmp` is tmpfs.** The knob defaults on: 12 bytes every 200 ms is not worth trading for an unmeasured failure mode |

## Testing without the hardware

```bash
make -C scripts/shims test      # needs qemu-arm in PATH
```

Two fixture tests matter here, and they are the same discipline applied to two
maps:

* `test_flx4` (with `tests/midi_flx4.dump`, a **hand-written** fixture) drives
  the FLX4 map through the real dispatcher via `MIDI_REPLAY` and asserts the
  resulting keycodes and mixer calls. Its assertions are therefore pins on the
  tables above — they prove the map matches itself, not the unit. A mis-read
  channel, for instance, would make a whole deck do nothing and *no* fixture
  could catch it, because the fixture was written from the same reading of the
  list the map was. **This is now replaceable**: `aseqdump2dump.py -o` records a
  fixture from the unit itself in the same format (S5.1), so the next session's
  capture can become the fixture and the assertions start pinning the unit
  instead of the reading.
* `test_midi` does the same against the JP21 map, which is measured, so it can
  catch a real regression there.

Run both after every map edit: they are the only check that catches an
off-by-one on a CC pair, a dropped table row or a broken relative encoder with
nothing plugged in.

For the record, and because it is the same discipline the rest of the port uses:
`grep 'knobshim' /tmp/knobshim.log` after a session shows what the shim saw and
what it sent, one line per event with `KNOB_VERBOSE=1`. On the first FLX4
session that log — not this document — is the primary evidence for every table
above.
