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
`RB_MIDI_IN_MATCH` (a case-insensitive substring, default `FLX4`), requires the
port's capabilities to include `CAP_READ | SUBS_READ`, excludes its own client,
and subscribes. It retries in a loop, because the shim starts before the
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
the shim. If the port name does not contain `FLX4`, change `RB_MIDI_IN_MATCH`.

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
| 4 BEAT / EXIT | note 77 | `0x410e` K_RELOOP | press / release — `TODO: unverified`, a *fresh* 4-beat loop needs rbp's AUTO LOOP pad mode, which this map does not enter |
| BEAT SYNC | note 88 | `0x4112` K_SYNC | press **and** release on the first edge that arrives — see footnote *3 below |
| BEAT SYNC long press | note 92 | `0x4111` K_MASTER | a pulse on press — `TODO: unverified`, the list gives this note no name beyond "Long press" |
| pad mode HOT CUE | note 27 | `0x4113` K_HOTCUE | press / release |
| pad mode BEAT JUMP | note 32 | `0x4116` K_BEATJUMP | press / release |
| channel CUE | note 84 | *(none — the mixer engine)* | on the press edge, toggles `me_set_cue(ch)` from `me_get_cue(ch)` |
| SHIFT | note 63 | *(none)* | log-only |
| pad mode PAD FX 1 | note 30 | *(none)* | log-only — rekordbox-only mode |
| pad mode SAMPLER | note 34 | *(none)* | log-only — ditto |
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
80, CALL 62/61, channel CUE 104, MASTER CUE 120, …). None is bound: the +SHIFT
note is not sent while SHIFT is up, so a bound note here can never arrive with
SHIFT held, and the list gives the shift actions no names. With SHIFT held each
control logs as unmapped, which is the honest outcome.

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
mode says they mean, and that mode is set by the pad-mode buttons above. So only
the two ranges rbp has a mode for are bound — HOT CUE (base 0) and BEAT JUMP
(base 32), the two modes bound above. The other six are deliberately unbound:
PAD FX 1/2 and KEYBOARD have no rbp equivalent, and BEAT LOOP and KEY SHIFT
arrive from a SHIFT + mode-button combination **whose pairing the list never
states** — it gives four extra modes and four shift notes without saying which
is which, so binding them would be a guess about which button puts rbp into
which mode. An unbound pad produces nothing and logs as unmapped.

The pads' +SHIFT layer is a **second channel per deck** (list ch 9/11 → rch
8/10) carrying the same eight ranges; it is unbound for the same reason.

`TODO: unverified` — the whole base+pad encoding, and the SHIFT pairing. It is a
large assumption: if it is wrong, the pads are the controls that fail most
quietly, because a wrong base lights nothing and sends nothing.

### Mixer, browse and LOAD (list ch 7 → rch 6)

| Control | Message | rbp | note |
|---|---|---|---|
| Sound Color FX, deck 1 | CC 23 | `0x509d` K_COLOR to rbp ch 1 | the **mixer's** channel, not the deck's |
| Sound Color FX, deck 2 | CC 24 | `0x509d` K_COLOR to rbp ch 2 | ditto |
| crossfader | CC 31 | `0x6017` K_XFADER, global | |
| headphone MIX | CC 12 | `0x4405` K_HPMIX, global | also feeds `g_cue_mix` for the audio shim |
| headphone LEVEL | CC 13 | `0x4406` K_HPLEVEL, global | also feeds `g_cue_gain` |
| browse push | note 65 | `0x420c` K_SELECTOR, global | press / release |
| SHIFT + browse push | note 66 | `0x0201` K_SOURCE, global | rbp's SOURCE screen, and the only row that reaches the mounted USB at all. **The note number is still an assumption** — see below |
| browse rotate | CC 64 (+SHIFT CC 100) | `0x420c` K_SELECTOR, global | relative, **0x01/0x7F** two's complement — *not* the jog's convention. Measured (S5.1): values 1 and 127 only, and the two conventions are what tell it apart from the platter, so this is the row that could most easily have been mapped backwards |
| LOAD, deck 1 / 2 | notes 70 / 71 | `0x4311` K_LOAD, global key with rbp ch 1 / 2 | |
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
  ([09](09-audio.md#the-level-the-units-own-master-level-knob)).
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
2. **That note 66 is what the panel actually sends.** This is **not** measured.
   The number comes from Pioneer's published MIDI list, which is also where
   every other number in this file comes from — but this is the one row where
   being wrong is silent: an injection tests (1) and says nothing about (2), and
   a real SHIFT + browse press sends whatever it sends, mapped or not.

(2) is settled by measurement, not by argument, and the measurement is one press
away: `RB_MIDI_DUMP` records every sequencer event the shim receives, so a real
SHIFT + browse push with the dump on answers it permanently. As of 2026-09-26
the dump holds no note 66 at the panel's own velocity (100 is `seqinject2`;
127 is the panel) — so **the operator's press has not been captured yet, and the
number is unmeasured**. If the press turns out to send something else, the fix is
one line in `map_flx4.c` and one line in the fixture; nothing else depends on it.
The FLX4 dump's other use is the mirror image: note 65 and note 70 *are* measured,
at the panel's velocity, which is why the browse push and LOAD are not in doubt.

### Beat FX (list ch 5 → rch 4, with a second leg on ch 6 → rch 5)

| Control | Message | rbp | note |
|---|---|---|---|
| FX ON/OFF | note 71, one leg per target channel | `0x448d` K_BFX, global | footnote *4: its LED blinks on NOTE ON and lights on NOTE OFF |
| FX SELECT | ch 5 note 99 | `0x448b` K_BFXTYPE, global | steps rbp's 14-position effect switch |
| BEAT ◁ / ▷ | ch 5 notes 74 / 75 | `0x4490` K_BEATPREV / `0x4491` K_BEATNEXT, global | op must be `OP_PRESS` — rbp gates these on `(op & 0xf) == 0` |
| LEVEL/DEPTH | CC 2, on **both** ch 5 and ch 6 | `0x448f` K_DEPTH, global | see below |
| FX CH SELECT | ch 5 note 16 (leg A) / ch 6 note 17 (leg B) | `0x448c` K_BFXCH, global, `OP_VALUE` 0 / 1 | the lever's third position has no rbp equivalent |

Three of these need their reason stated rather than just their value:

* **The FX CH SELECT lever is two notes, not a position.** CH1 = leg A lit, CH2 =
  leg B lit, CH1&CH2 = both. rbp's Beat FX has a *single* target channel, so
  "both decks" has nothing to send; the map leaves rbp where it was and logs
  that. The other two notes of that group (ch 5 note 17, ch 6 note 16) are OFF
  in every position the list gives, and a mid-slide with both legs momentarily
  off is ignored rather than read as a position. `TODO: unverified` — nothing
  says which position the lever is in at connect, so the map only acts on a
  *move*, and rbp keeps whatever target it built itself with until then. The
  whole group is re-sent on every move, so only a change is a move.
* **The Level/Depth knob's channel is a disagreement in the vendor's own
  table**: its Channel column says 6 (list ch 6), while its own MIDI-IN status
  byte says `B4` (list ch 5 / `0x40` below). Both candidates are bound to the
  same key so the knob works either way, and the dump settles which one the unit
  sends. `TODO: unverified`.
* **FX SELECT steps rather than reads.** rbp's effect switch has 14 positions
  and the unit's FX SELECT is a *button*, so the map keeps the position and
  steps it. It is deliberately **not** seeded from rbp's current effect
  (`map_jp21.c` reads that through `ADDR_GET_BFX_TYPE`; this map keeps rbp
  addresses out of itself), so the first press selects position 0 — whatever
  effect that turns out to be — rather than stepping on from what is playing.

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

**The machinery is in place and the notes are not measured yet.** The panel's
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

The meter is the one that does not fit the existing bridge: its value is a level,
ramped `0x26-0x40` green1, `0x41-0x56` green2, `0x57-0x64` orange1, `0x65-0x76`
orange2, `0x77-0x7F` red, where rbp's own meter is an 11-segment **bitmask** and
the shim's rescale assumes a segment count. The list's type column calls that row
a NOTE while its own status byte is `B0`, a control change; the status byte is
what the wire carries. Meters are out of scope for the LED pass — and note that
this row contradicts the "the FLX4 has no level meters" claim that used to sit in
`rb.conf` and [09](09-raspberrypi4-audio.md); the ramp is documented even though
it has not been measured here.

The pad **colours** are still unknown: the existing encoder assumes the Engine OS
Prime convention (bits 4-5 red, 2-3 green, 0-1 blue, two bits each;
`RB_PAD_BRIGHT=1` sets bit 6 for the bright range), and nothing confirms the FLX4
uses it. The FLX4 table's `pad_enc` is therefore `LED_ENC_NONE`, which means a
plain on with no colour — and `RB_PAD_BRIGHT` is inert until that changes.
`test_flx4.c` fails the suite if a row claims the Engine OS encoding before it has
been measured, because that is a tempting guess rather than a hypothetical one.

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

**What is still unmeasured is the only thing the panel can answer:** whether a
write to `20:0` lights anything at all, and which note lights which control. The
kernel accepts the write (`20:0` is writable, and `seqinject2` calls
`die("write event")` on a failed `write()`, which never fired), but a note number
that has not been *seen* to light must stay `-1`. Nothing in the test suite can
see an LED.

The hypothesis to test first is **a button's LED uses the same note as its
input**, which was true of the previous target (`rbp_led.c`'s old header) and is
Pioneer's convention. The input notes are known from the S5.1 capture:

| Control | Candidate | Probe |
|---|---|---|
| CH CUE deck 1 | note 84 on ch 0 | `sh /tmp/ledprobe.sh pair 0 84` |
| BEAT SYNC deck 1 | note 88 on ch 0 | `sh /tmp/ledprobe.sh pair 0 88` |
| hot-cue pad 1 deck 1 | note 0 on ch 7 (list ch 8) | `sh /tmp/ledprobe.sh pair 7 0` |
| PLAY / CUE deck 1 | notes 11 / 12 on ch 0 | `sh /tmp/ledprobe.sh pair 0 11` |
| MASTER CUE | note 99 on ch 6 (list ch 7) | `sh /tmp/ledprobe.sh pair 6 99` |

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

* **No level meters.** `RB_LED_VU=0`. This does more than silence output:
  `install_meter_hook()` patches `rbp`'s `MonoLvMeter::getLedValue` prologue in
  machine code, and with no meters on the target that patch is **not installed
  at all**. `RB_LED_VU_SEGMENTS` parameterises the meter segment count for a
  target that does have them. It does *not* switch off the master cue below —
  that is engine setup rather than meter work, and `vu_thread()` asserts it on a
  meterless target too, on its own wait for `rbp`'s mixer.
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
FX type; and rbp's ALOOP/SLIPLOOP pad modes.

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
| 2 | **The pad base+pad encoding** — whether `base + pad` and the eight bases are what the unit sends, and whether the four +SHIFT modes pair with the four shift notes the way the list's ordering implies. | **Partly answered in S5.1**: pad 1 in HOT CUE arrived as note 0 on list ch 8 (rch 7), so HOT CUE's base is 0 and pad 1 is +0. The other seven bases are unmoved — press one pad in each pad mode with `aseqdump` running; `LED_VERBOSE=1` shows what rbp then does with it |
| 3 | The pad-LED **velocity → colour** table. The encoder assumes the Engine OS Prime convention (bits 4-5 red, 2-3 green, 0-1 blue, 2 bits each); `PAD_BRIGHT=1` sets bit 6 for the bright range. | **Narrowed, still open.** The FLX4 table's `pad_enc` is now `LED_ENC_NONE`, so no colour is claimed and `RB_PAD_BRIGHT` is inert for this target until the encoding is measured; `test_flx4.c` fails a row that claims the Engine OS encoding first. Measure with `sh /tmp/ledprobe.sh pair <pad_ch> <pad_note>` and watch whether any colour at all appears, then ramp the velocity (`0x10 0x04 0x01 0x40 0x3F 0x7F 0x2A`) and read the pads |
| 4 | The **transport/pad/loop LED note numbers**, which the list does not give at all beyond LOADED and VINYL MODE. | **The tooling and the safety answer are both settled; the numbers are not.** `amidi` does not work on this unit (rawmidi is EBUSY — the sequencer holds the node), so the probe goes through the sequencer via `seqinject2`/`/tmp/ledprobe.sh`, which is the route the shim's own writes take. The loopback question is **answered: no** — a write to `20:0` is not delivered to the shim's input port, and the device does not echo (measured 2026-09-26; see [the LEDs section](#measuring-the-notes-the-route-and-the-loopback-result)). What remains is the measurement itself: send each candidate and watch. `RB_LED_DISABLE=0` is now correct **because** every unmeasured row is `-1`, so the bridge runs and transmits nothing |
| 5 | Jog `RB_JOG_PPR` (shipped as the previous unit's 128), `RB_JOG_REV` and pitch polarity/resolution. | **`RB_JOG_PPR` answered in S5.1: 720** (ten revolutions → 7183 forward counts → 718.3/rev; see [Step 3](#step-3--calibrate-the-continuous-controls)). `RB_JOG_REV`, polarity and pitch resolution are still open: the capture's jog turn direction and pitch push direction were not recorded, so neither sign can be read out of it. `TEMPO_VERBOSE=1`/`JOG_VERBOSE=1` on the device settle all three. |
| 6 | The **Level/Depth knob's channel**, where the list's Channel column (6) disagrees with its own status byte (`B4`). Both are bound, so the knob works either way — but one of the two rows is dead weight and should go. | the dump: the CC appears on one channel only |
| 7 | **Which position the FX CH SELECT lever rests in**, and rbp's default Beat FX target. The map deliberately does not force a target, so it does nothing until the lever moves. | move the lever through all three positions and watch `KNOB_VERBOSE=1`; then note what rbp had selected before the first move |
| 8 | Whether the BEAT SYNC release (footnote *3) carries the ON edge, the OFF edge or both. | **Answered in S5.1**: one press produced both edges (`on/off x1/1` on note 88), so the map's first-edge rule is load-bearing rather than merely defensive. The map would handle either order without double-sending, so which came first is for the record — the dump has it in order (`grep 'note=88' flx4.dump`). |
| 9 | rbp's **keycode for CUE/LOOP CALL** ◁/▷. This is **half-answered and half-bypassed**: the keycodes are still unknown, and ◁ no longer waits on them — it is `K_BACK` now, because the port had no BACK at all and a dead button was worth more as one. ▷ is still log-only, and finding either real keycode would let them be what they say they are. | rbp's own key table, or a keyboard/pointer session against the RX3 UI |
| 10 | ~~Whether the FLX4's USB audio is `S24_3LE`, `S16_LE` or `S32_LE`, and its native rate.~~ | **Answered in S1.4**, and it did change the shim: `aplay --dump-hw-params -D hw:CARD=DDJFLX4,DEV=0` reads `FORMAT S16_LE S24_3LE`, `SAMPLE_BITS [16 24]`, `FRAME_BITS [64 96]`, `CHANNELS 4`, `RATE [44100 48000]` — **no `S32_LE`**. The plan's `plughw:` default assumed the question did not matter, because the plug chain would convert whatever the answer was; the card's real count is what `AUDIO_MAP`'s hardware indices need, and a plug device reports 10000 for it (see [13](13-raspberrypi4.md) S1.4 and [09](09-audio.md)) |
| 11 | The **units of the jog's `l` field** (the 16-bit `pos` in the `OP_ROTATE` message). The map fills it with raw platter counts, so the measured 720 counts/revolution now goes into a field rbp may *compare* rather than difference — and the map inherited 128 from the JP21. `RB_JOG_SCALE` cannot express that conversion: it scales `vpos` but cancels out of the speed, and it is clamped to ≥ 1, so it can only make the units larger. | if the platter misbehaves in a way the speed cannot explain (a deck that jumps when touched), log `JOG_VERBOSE=1`'s `pos=` against a known turn and compare it with what rbp does |
| 12 | **The note number the SHIFT + browse push actually sends** — the one number in this file whose being wrong is silent, and the only thing standing between the port and "the SOURCE screen is reachable from the panel". Note 66 is Pioneer's published value; that the *binding* works is proven, that the *number* is right is not. | one press with `RB_MIDI_DUMP` on: the dump records the panel's velocity (127) separately from the harness's (100), so `grep 'note=66 vel=127' /tmp/flx4.dump` answers it outright. As of 2026-09-26 that grep is empty and this is open. See [the section above](#the-shift--browse-push-and-the-two-questions-it-is-two-questions-about) |

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
