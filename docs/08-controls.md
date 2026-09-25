# 08 — Controls (SC Live 4 MIDI → rbp keycodes)

Transport, deck, mixer, jog, pads and DJ / Sound Color FX are mapped from the
SC Live 4's MIDI control surface onto rbp's key space, and rbp's LED and VU
state is mirrored back onto the panel.

## Source of truth

Engine OS defines every hardware control's MIDI identity in QML:

```
/usr/Engine/AssignmentFiles/PresetAssignmentFiles/JP21/JP21_Controller_Assignments.qml
/usr/Engine/AssignmentFiles/PresetAssignmentFiles/JP21/JP21_Controller_Device.qml
```

`JP21` is the SC Live 4 (internal name `SCX-4`). `knobshim2.c` is built from
these files. The SC Live 4 uses **channels 4/5 for the decks** and **15 for the
global/FX group**.

## MIDI layout (all channels 0-based)

| Surface | seq channel |
|---|---|
| Global (transport/FX/mix) | 15 |
| Deck Left / Right | **4 / 5** |
| Mixer strips **1 / 2** | 0 / 1 |

The SC Live 4 has **4 mixer strips**, but rbp is a **2-channel** mixer, so only
strips **1/2** are mapped (→ decks 1/2). Strips 3/4 are used for the
master-cue function instead (see below).

## Mapping table (as built in `scripts/shims/knobshim2.c`)

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
| Main Vol | CC 20 | `g_master_gain` — audioshim, **ch0/1 (XLR) only** |
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
| **PFL / cue (strips 1/2)** | note 13 | drives `MixerEngine::setMixerChHeadphoneCue` **directly** (rbp has no PFL keycode); LED mirrored on note 13 |
| **Master cue (strips 3/4)** | note 13 | drives `MixerEngine::setMasterOutHeadphoneCue` (SC Live 4 has no MASTER CUE button); LEDs mirrored |

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

### Not mapped

* Beat-loop knob — experimental, `BEATLOOP=1` (above).
* Parameter (23/24), Layer (31), StopTime (CC 37), Thru (note 15) — no
  direct rbp keycode, or needs a distinct param.
* SHIFT (note 28 per deck) is tracked and used for the BEAT < / > combo; other
  shift-actions are not wired.
* Pad-mode LEDs (11–13) and pad colours.

## LED output (SC Live 4 panel)

Transport LEDs mirror the engine: PLAY / CUE / SYNC / KEY LOCK / VINYL / SLIP,
both decks.

### How the SC Live 4 drives its LEDs

Every front-panel LED is **MIDI**: Engine OS sends `Note On`/`Note Off` to
the *Control Surface* — rawmidi `hw:0,0` (`/dev/snd/midiC0D0`), or seq
`16:0` from the sequencer side. Velocity encodes colour/brightness
(`(r<<4)|(g<<2)|b`, 2 bits per channel; `0x7F` = bright for the simple
transport LEDs, `Note Off` = dark).

```sh
aplaymidi -p 16:0 ledtest.mid      # note8/9/10 ch4 -> SYNC/CUE/PLAY light up
amidi -p hw:0,0 -S "94 0A 3F"      # PLAY deck1 bright
```

Writing LED MIDI does **not** loop back into the Control Surface input path
(verified with `aseqdump -p 16:0`), so it is safe to do from inside `rbp`.

### The bridge (`knobshim2.so`)

rbp computes its LED state into `uif::LedStat` and encodes it for the
XDJ-RX3's EUP/SUB micons, sent to `/dev/subucom_spi1.0` — a dead FIFO in this
port. `led_thread()` therefore polls rbp's own engine state at 20 Hz through the
`playengine::PlayEngine` singleton and mirrors it onto the JP21 notes:

| LED | note (ch 4/5) | source |
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
`EnPlayerChannel`; the panel MIDI channels are 4/5).

Env:

* `LED_VERBOSE=1` — log every LED change (`led deckN noteM on/off`)
* `LED_DISABLE=1` — do not touch the panel LEDs

Not yet driven: pad colours (RGB), pad-mode LEDs (11–13), LOAD / browse
LEDs, CUE id, and the loop LEDs.

The bridge opens `/dev/snd/midiC0D0` (rawmidi) exclusively, so while rbp
runs nothing else can open the Control Surface output (`amidi`/`aplaymidi` will
report "Device or resource busy"). Set `LED_DISABLE=1` to release it.

### Global LEDs driven straight from rbp

The LedStat id for the FX group is **`LedDef::ID + 8`**:

| control | panel note (ch 15) | rbp LED id |
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

Discovery switches in `knobshim2.so`: `LED_DUMP=1` (log every
`(id,ch,state)` change), `LED_SWEEP=1` (drive rbp's keycodes and snapshot the
table around each one).

## VU meters

The panel meters are MIDI CCs, and **bitwise**: the value is a bitmask of the
segments lit, from `ledCCValues: [0, 1, 3, 7, 15, 31, 63]` with
`vuLedCCIndexing: Bitwise` (so `0x3F` = all six).

Each meter on the SC Live 4 is **6 LEDs, bottom → top: 4 white, 1 blue,
1 orange**.

| meter | MIDI |
|---|---|
| Master L / R | **CC 32 / CC 33**, channel **15** |
| Channel 1 / 2 | **CC 10**, channels **0 / 1** |

Thresholds: master `[-45.7, -25.7, -12.2, -7.2, -4.2, -0.2, 20]`, channels
`[-45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20]`.

**Master** — `audioshim.so` sees the master mix in `snd_pcm_writei()`; it
computes a per-channel peak (S24_LE full scale `0xFFFFFF`) with a ~300 ms
release and publishes it to the shared `g_vu_peak[2]`. `knobshim2.so`'s
`vu_thread` converts peak → true dB → segment bitmask and sends CC 32/33 on
ch15 at 40 Hz. L and R are independent.

**Channels 1/2** — rbp's channel meter is **11 segments**
(`LED_TABLE = (1<<n)-1` for n = 0..11) and is **pre-fader**, while the panel
has 6. `ui::Mixer::MonoLvMeter::getLedValue()` is the single function that
turns a level into the meter bitmask, so `knobshim2.so` patches its prologue
and hooks it (`install_meter_hook()`; the caller LR identifies master / ch1 /
ch2, and the original runs through an RWX trampoline). `vu_thread` then:

1. takes rbp's lit-segment count (0..11 → uint32, *not* a byte),
2. rescales 11 → 6 (`(n*6+5)/11`),
3. applies the **channel-fader taper** so the meter drops with the fader like
   Engine OS,
4. sends `CC 10` on channel 0 (meter 1) and channel 1 (meter 2).

Env: `VU_TEST=1` (sweep 0→6 segments), `VU_DEBUG=1` (log raw hooked bitmasks +
fader positions), `VU_DISABLE=1`.

At startup the shim sends Engine OS's **absolute-control query** sysex
(`F0 00 02 0B 7F 12 04 00 00 F7`, from `JP21_Controller_Device.qml`
`queryAbsoluteControls()`) so the panel reports every fader/knob position
immediately. It first waits until rbp's `KeyManager` exists, then re-asserts the
positions every 2 s for the first **30 s** (deliberately short, so it does not
fight the user).

Two implementation notes:

1. rbp finishes initialising its mixer *after* the shim is loaded, so a value
   sent too early is overwritten and the fader reads "down" until moved once.
2. `handle_cc_abs()` only sends on change, so a repeat query returns the *same*
   values and would be dropped — `led_query_absolute()` therefore invalidates
   the CC cache (`abs_map[].last = -1`) before asking.

## Key gestures added by the port

Controls the SC Live 4 has that the RX3 does not (or vice versa):

| Gesture | Sends | Why |
|---|---|---|
| **Hold SYNC** (≥ 600 ms) | `0x4111` K_MASTER | SC Live 4 has no MASTER button. Fires at the threshold, not on release. Tap = normal SYNC. |
| **CENSOR** | `0x410e` K_RELOOP | reverse/censor is unused here; it makes a useful loop exit |
| **TIME push + turn** | `0x4490` / `0x4491` | RX3's BEAT `<` / `>` buttons, absent on the SC Live 4 |

The SYNC hold sends nothing until the threshold is reached; sending the press
early and suppressing the release would leave rbp with a stuck SYNC press and
trigger its long-press action (**instant double**).

## Build

```sh
# in WSL (soft-float ARM cross-compile, GLIBC_2.4 only)
cd scripts/shims
make knobshim2.so RX3=/path/to/extracted/XDJRX3-rootfs
```

Deploy: `scp knobshim2.so root@sclive4:/data/knobshim2.so`, then restart rbp
(or run `start-rb.sh`).

## Verify

```sh
# on device: KNOB_VERBOSE=1 logs every event + keycode
cat /tmp/knobshim.log
#   knobshim2: ch4 note10 -> 0x4101 press (sch1)   <-- PLAY on deck 1
```
