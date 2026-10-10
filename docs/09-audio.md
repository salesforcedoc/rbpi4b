# 09 — Audio

On the Pi the audio path is the **DDJ-FLX4's USB audio**: master out to its
RCA/XLR outputs, headphones and cue out of its jack. There is one real device
and one real stream; everything the shim does is about putting rbp's three
streams onto the right pairs of it.

The device, the channel count and the stream→pair assignment are all
configuration (`AUDIO_DEV`, `AUDIO_CHANNELS`, `AUDIO_MAP`), so a different
controller is an `rb.conf` edit rather than a rebuild. The previous target's
8-channel codec map is kept at the bottom as the worked example that shaped the
model.

## rbp's side of the contract — unchanged

`rbp` opens ALSA in a fixed order and believes each device is a **2-channel
S24_LE 44.1 kHz** one. It never checks:

| Order | Stream | rbp uses it for |
|---|---|---|
| 0 | master | the main output |
| 1 | phone | the headphone bus — cue + master, mixed by rbp |
| 2 | booth | a second output |
| 3+ | (any) | discarded |
| capture | dummy | read and ignored, forever |

That belief is the shim's whole job to preserve. Three consequences shape the
code:

* Only the **master** stream is backed by a real device handle. The phone and
  booth streams stage their last block, and the master's write flushes the whole
  interleaved frame — one `snd_pcm_writei()` per frame for every pair at once.
* That is also why **the master is the audio clock**: rbp asks for a
  non-blocking device and then relies on it to pace the render thread, so the
  shim masks `SND_PCM_NONBLOCK` off and lets the card do the pacing.
* When no device could be opened at all, stream 0 still gets a non-NULL handle
  with a recognisable identity, because rbp is not written to survive a NULL
  `pcm`. Every stream then paces itself with a sleep — silent, but alive, and
  with a log line that says so rather than a mystery hang.

## Choosing the device: `AUDIO_DEV`

```
RB_AUDIO_DEV=                 # empty -> hw:CARD=<the selected controller's card>,DEV=0
```

The card is **not** written down in `rb.conf` any more. An empty value means the
ALSA card of whichever controller `MIDI_MAP` selects, read from that surface's
row in `controllers.c` (`DDJFLX4` for the FLX4), so a bench that switches surface
switches its audio card with it — it used to be independent, and selecting `jp21`
left audio pointed at an FLX4. Setting it here still wins, which is how a card
the table does not name is used; `doctor.sh` reports when the two disagree.

That resolved value is `hw:CARD=DDJFLX4,DEV=0`, and it is `hw:`, **not**
`plughw:` — and this is a measurement, not a preference. The reason
is the section after next: `AUDIO_MAP` names *hardware* channel indices, so the
shim has to learn the card's real channel count, and a `plughw` device cannot tell
it. The plug layer's whole job is to make the logical channel count arbitrary, and
it reports what it can accept, not what the card has. Measured on this unit:

| device | `snd_pcm_hw_params_get_channels_max()` |
|---|---|
| `hw:CARD=DDJFLX4,DEV=0` | **4** |
| `plughw:CARD=DDJFLX4,DEV=0` | **10000** |

With `plughw` the shim negotiated 8 logical channels — its own ceiling — onto a
4-channel card, and the map then indexed channels of a stream the plug layer was
free to remix. Every write failed with `-EINVAL` for reasons the log could only
show as `written=-22`.

The FLX4 accepts **44100 natively**, so nothing needs resampling either:

```
$ aplay -D hw:CARD=DDJFLX4,DEV=0 --dump-hw-params /dev/zero
FORMAT:      S16_LE S24_3LE
SUBFORMAT:   STD MSBITS_MAX
SAMPLE_BITS: [16 24]
FRAME_BITS:  [64 96]
CHANNELS:    4
RATE:        [44100 48000]
PERIOD_SIZE: [45 48000]
BUFFER_SIZE: [90 96000]
```

That set is the whole story: two formats, both 16- or 24-bit; 44.1 or 48 kHz; four
channels. `FRAME_BITS`'s maximum of 96 is worth a second look — 4 channels × 24
bits — because it is an independent check on what the shim writes: 64 frames in
768 bytes is 96 bits per frame, exactly the card's ceiling.

The candidate list, tried in order, is built once at startup rather than
assembled on the failure path — a fallback chain whose last step only runs when
something is already wrong is a step nobody has tested:

1. `AUDIO_DEV`;
2. if it starts with `hw:`, the same string as `plughw:` — the plug layer's
   conversion is a genuine fallback for a card that will not take our format
   directly;
3. nothing — silent, sleep-paced, one loud log line.

There were four rungs until 2026-09-27 and the third was the bare name `default`.
It is gone, and the measurement that removed it is the reason: with no FLX4 on the
bus `default` resolves to card 0 `bcm2835 Headphones`, which **opens**
(`res=0`) and takes `hw_params` — so it was not a rung that fails, it was one that
*succeeds* at opening and then refuses every write with `-EINVAL`. The shim
therefore believed the master was up and never took rung 4, and rbp's recovery
spun prepare → write(-22) at ~29k times a second: 13M failures, 449 MB into
`/tmp` in eight minutes, and — the symptom the operator reported — a player that
read touch and keyboard and painted nothing. The rule that now stands in its
place, and the reason the chain is built only from names the configuration
supplies, is in `scripts/shims/master_policy.h`: *the chain may only name the card
`AUDIO_DEV` names*, because anything else is a device whose channel count gets
latched into the pair map, which the map's indices were never resolved against.

`AUDIO_DEV=default` is still honoured if you ask for it by name — that is the
escape hatch the drill uses to stage the failure deliberately (`RB_AUDIO_DEV`,
`docs/13-raspberrypi4.md`), and it puts `default` in the chain because the
configuration named it, not because the code did.

`RB_AUDIO_FMT` names the format the **shim** packs into, and on this target it is
the normal path rather than an escape hatch: with a `hw:` device nothing converts
for us, so it must name a format the card actually accepts. The shipped default is
`s24_3le` — what the FLX4 wants, and what `s24pack()` produces; `s24_le` (rbp's
own 4-byte container) and `s16_le` are also accepted. An *unset* value is not the
same as the shipped default: it parses to `S24_LE` and hands rbp's words over
untouched, which is correct in exactly one case — the `plughw` fallback above,
where the plug chain is doing the packing.

### The format constants are hand-rolled, and one of them was wrong

`audioshim.so` deliberately does not link the target's ALSA headers — it links only
`libdl` and `libc`, so it can be built from the armel toolchain without a sysroot
for the chroot's libasound — so it carries its own copy of `snd_pcm_format_t`.
That copy had a wrong value in it for a long time:

| constant | was written as | actually is |
|---|---|---|
| `SND_PCM_FORMAT_S16_LE` | 2 | 2 |
| `SND_PCM_FORMAT_S24_LE` | 6 | 6 |
| `SND_PCM_FORMAT_S24_3LE` | **10** | **32** |

10 is `S32_LE`. This is the trap in the enum: the *packed* 24-bit formats are not
next to the padded ones, they start at 32 — so the guess does not merely miss, it
lands on a format that exists, and the card answers with a bare `-EINVAL` that
names nothing.

Nothing had caught it because the value only ever reaches libasound on a `hw:`
device, and `AUDIO_DEV` was `plughw:` until this port — the old `set_format()`
answered for itself and the number was never used. Two things keep it fixed now:

* `tools/pcmprobe.c` prints the whole mapping, measured from libasound rather than
  transcribed from it;
* `check_format_constants()` in the shim has libasound *name* each value at
  startup and logs what it resolved — including when it resolves them correctly,
  since a check that is silent both when it passes and when it has been quietly
  disabled is not a check. A wrong number is now one loud line instead of silent
  no-audio.

## Channels: `AUDIO_CHANNELS=auto`, and the 1:1 rule

`auto` reads `snd_pcm_hw_params_get_channels_max()` from the real handle and
clamps it to the 8 the output buffer holds. A number forces that count.

**The rule: ask for exactly as many logical channels as the card has** — on the
FLX4, 4. On a `hw:` device there is no plugin between the shim and the card, so the
shim's frame *is* the card's frame and the map's indices mean what they say. The
rule is inherited from the previous target, where it had different teeth: there a
`route` plugin stayed 1:1 only while logical ≤ hw, and asking for more made it
*downmix*, folding front-left/front-right into the surround pair and putting
garbage on the second output. That failure sounds like a working card and a broken
mix. With `hw:` the equivalent mistake is a bare `-EINVAL` at `hw_params` instead —
which is at least honest. Either way the count is negotiated rather than forced to
8 the way the previous target's code did, and `AUDIO_CHANNELS` should only ever be
set to a number the card actually reports.

## The stream→pair map: `AUDIO_MAP`

```
RB_AUDIO_MAP=master=0,1;headphones=2,3;booth=-
```

A list of `stream=pair` entries, starting from the defaults above and
overriding only what it names, so `AUDIO_MAP=booth=0,1` moves one stream and
leaves the rest alone. `-` means **dropped on purpose**, with one log line —
not a silent skip. A malformed entry, an unknown stream name, or a map longer
than the buffer is ignored as a whole, with a log line, rather than half-applied.

`RB_AUDIO_MONITOR_PAIR` is separate: an extra pair that always carries a **copy
of the master**, for a card that has a fixed monitor or booth output of its own.
It is not a stream — `rbp` never writes to it — which is why it is not an
`AUDIO_MAP` entry.

A **stereo-only** card degrades to master-only with the cue dropped, and says
so. It never silently sums the cue into the master: that produces a mix that
looks right until someone cues a track, and it is the kind of bug that gets
diagnosed as "the headphones are broken".

### Why the phone stream is routed straight through

rbp's **phone stream is already the headphone bus**: with rbp's master cue
enabled it contains cue + master, mixed and time-aligned by rbp's own
`HeadPhone` object, which is where the cue mix, cue level and stereo/split type
live. The shim therefore routes it directly rather than summing rbp's separate
master and cue ALSA devices — those are not sample-aligned, and summing them
comb-filtered on the previous target.

Corollary: the cue mix/level knobs are **not** applied in the audio shim. They
belong on rbp's mixer engine, through the controls bridge — see
[08 — Controls](08-controls.md).

## The HDMI mirror

The master audio can also go out of the Pi's HDMI, **at the same time** as the
FLX4, so a screen on the desk carries what the room hears. It is the *master pair
only* — `ol`/`or_`, post Main Vol and post startup mute, the same samples the card
is handed — so it is a second copy of one output, not a second mix. The monitor
pair above is the same idea for a card that has a monitor output of its own; this
one is for a sink that is not a sound card at all.

It is **advisory**, and that is the constraint everything about it follows from.
A monitor is hot-swapped on this unit with the player running, so an absent or
slower HDMI sink must cost the mirror blocks and never the master:

* Opened and written **non-blocking**. A full ring or a missing sink costs one
  dropped block and a counter; nothing on this path waits.
* **No thread and no new dependency** — the shim links only `libc`/`libdl`, and
  the retry is a frame-counted backoff on the audio thread, not a timer. Its
  recovery deliberately does **not** re-arm the startup mute the way the master's
  reopen does: the FLX4 thumps on open, so copying that here would silence the
  FLX4 for 1.8 s every time the HDMI side hiccuped.
* **UP is earned by a write**, never by an open. Opening succeeds against a port
  with nothing on it, so the `mirror UP` line only appears after the first block
  is actually delivered.
* An ALSA **`multi`** PCM would have been the config-only answer and is the one
  thing that cannot work: `multi` is all-or-nothing, so an HDMI slave that will
  not open takes the FLX4 down with it.

### The knobs

| knob | default | note |
|---|---|---|
| `RB_AUDIO_MIRROR_DEV` | `hw:CARD=vc4hdmi0,DEV=0 hw:CARD=vc4hdmi1,DEV=0` | space-separated, tried in order, because which micro-HDMI port the monitor is on is not fixed; each `hw:` entry also yields its `plughw:` twin. **Empty turns the mirror off** |
| `RB_AUDIO_MIRROR_FMT` | `subframe_le` | `s24_le`, `s24_3le`, `s16_le`, `subframe_le` — see the trap below |
| `RB_AUDIO_MIRROR_REOPEN_MS` | `5000` | the ceiling on the retry wait after the sink goes away; `0` means try once at startup and never again |
| `RB_AUDIO_MIRROR_BOOST_DB` | `4` | the mirror's fixed level lift, in dB — the only thing on this port that puts the HDMI out above the master's own level. `0` is the old behaviour; negative sits the mirror below the master. Accepted window −24…+12 dB; watch `clips=` after changing it |

These need a **shim rebuild** when they change, unlike the rest of the audio
block: the device, the format and the retry interval are read inside
`audioshim.so`.

### The level: the unit's own MASTER LEVEL knob

By default the mirror follows rbp's Main Vol, because it is handed the same
`ol`/`or_` — and on this map Main Vol is pinned at unity, so the mirror is at full
level and the only volume the room hears is the FLX4's own. The HDMI copy can be
tied to that same knob: the **MASTER LEVEL** knob (list channel 7, i.e. 0-based 6,
**CC 8 MSB + CC 40 LSB — a 14-bit pair**, measured on the unit 2026-09-27) writes
`map_flx4.c`'s `g_mirror_gain`, and that is the only thing it writes.

**Why not rbp's master level.** The knob is the unit's *analogue output* volume,
sitting downstream of the USB audio the Pi feeds it. Driving rbp's Main Vol — or
`audioshim`'s `g_master_gain` — from it would attenuate the master **twice**: once
in the samples the Pi sends, again in the unit's output stage. So the two are
deliberately separate symbols, and `g_mirror_gain` scales the advisory mirror
alone: the HDMI tracks the room while not a sample of the FLX4's stream changes.
The handler dispatches on the **LSB** and holds the MSB, like the pitch fader and
the jog — an MSB alone is half an update, not a position.

**Where unity sits: 1 o'clock, not the stop and no longer the middle.** The raw
position is not the gain. `flx4_mastervol_gain()` in `map_flx4.c` maps it — unity
at **1 o'clock**, **flat above it**, and a linear ramp below:

```
gain = min(1, (pos / 16383) / MID)          MID = MIRROR_GAIN_MID, default 0.6
```

The knee has moved twice, both times on the operator's ear and both times on
**2026-09-27**. First: with unity at the stop the mirror was too quiet to use —
the operator had to crank the knob to the end — because a volume knob's working
point is not its stop, and the middle was only 0.5, i.e. **−6 dB**. So the middle
became full level. Then the operator asked for the **top of the useful travel to
be 1 o'clock rather than 12**, and unity moved up with it.

**Where 1 o'clock is: measured, not assumed** — and the measurement is the unit's
own MIDI dump. `RB_MIDI_DUMP` was already on, so the knob's whole travel could be
read back rather than inferred. The composed MSB/LSB positions span raw **0 to
16383**, and both ends are among the most-visited positions (14 samples at 16383,
11 at 0), which is what a mechanical stop looks like: the pot pins at the ends of
its electrical range instead of stopping short of them. So `pos / 16383` is a
fraction of *rotation*, and **12 o'clock is raw 0.5** — the electrical midpoint
and, in the same dump, where the hand rests: the most-visited positions cluster on
0.479–0.532, centred on 0.50. 1 o'clock is one hour past that, 30° of a 270–300°
sweep, i.e. **0.60–0.61** of the rotation. 0.6 is the round number; the two differ
by 0.15 dB at 12 o'clock.

An earlier reading of this dump put the bottom stop at raw 2228 and derived 0.65
from it. **That was wrong, and wrong in a way worth keeping**: it came from the
hand sweep's *lowest logged gain* under the old `pos / 16383` law, which was 0.136
— and a sweep that never reaches the bottom prints a low value that is
indistinguishable from a stop. This document had already recorded that full-down
had not been observed; the inference was drawn anyway. The dump settles it because
those are the pot's own raw numbers rather than a law applied to them.

**What that costs, stated plainly:** 12 o'clock is no longer full level. It reads
**0.833**, about **−1.6 dB**, and everything below the knee scaled down with it.
Nothing anywhere in the knob's law can exceed 1.0, so this change only ever
*lowers* what a given position gives — it moves the knee, it does not add level. If
what is wanted is more level available rather than a different knee, that is
`RB_AUDIO_MIRROR_BOOST_DB`, which is the next subsection and is **no longer** a
thing that needs a change to `s24pack()` first.

The one mercy of the travel being the whole range is that the **bottom is
unaffected**: raw 0 is the stop and it still maps to silence, exactly as under
every earlier version of this law. Under the 0.65 reading it would not have been,
which is the second thing that bad premise would have got wrong.

The **travel above the knee is flat, not boosted, and that is still true of the
knob** — turning it past 1 o'clock buys nothing. The level above the master comes
from `RB_AUDIO_MIRROR_BOOST_DB` instead, because the knob is an attenuator the
operator sets by ear and a fixed, logged, one-line-revertible number is the easier
thing to change and the easier thing to take back. The two multiply.

**How that is checked with no hand on the knob:** point `RB_MIDI_REPLAY` at a
hand-written dump of CC 8/40 lines — `mididump_replay()` feeds the real
`ctrl_dispatch`, so a walk of the travel replays with nothing plugged in. That is
how the middle was shown to be full level on the unit: the `gain=` field read
`1.000 → 0.000 → 0.500 → 1.000` and then no further change, where the old law
would have printed six distinct values and **0.500** at the middle
([13 — Raspberry Pi 4](13-raspberrypi4.md#bring-up-order), S4.9). Under the 0.6
knee the same replay should read `1.000 → 0.000 → 0.833 → 1.000`: the shape is the
same and only the middle's number moves, because that is the knee moving and
nothing else changing. (Predicted from the positions that run used, not yet
re-run.)

The knob is its own rollback: **`RB_MIRROR_GAIN_MID=1.0`** reproduces the old
`pos / 16383` law exactly, with unity back at the stop. Values of 0 or below, and
above 1, are ignored in favour of the default (0 would divide, and >1 would ask
for the boost above). Unlike the rest of this section the setting is read by
`map_flx4.c` — the **controls** shim — because the level is the knob's, not the
audio path's; `audioshim` only consumes the result.

**It starts at unity, every run.** The unit's connect report is a **lone MSB** —
the FLX4 map's recorded fixture (`scripts/shims/tests/midi_flx4.dump`) has
`0.098000 CONTROLLER ch=6 cc=8 val=64` with no `CC 40` behind it, and half a pair
is not a position — and restarting the player does not re-enumerate the unit, so
nothing arrives at all: measured over **113 501 blocks / 7 264 064 frames (164 s
of delivered audio)** after a restart, the MIDI dump held **zero** CC 8/40 lines
and the log's `gain` read `1.000` throughout.

Nor could it be asked. The tree's one absolute-value query is
`midi_io.c`'s `led_query_absolute()`, and it is a **JP21** protocol message — the
sequencer route cannot carry it, and a surface matched by name never sends it at
all (`knobshim2: absolute-value query not sent: the rawmidi route is what carries
it`). The FLX4 has no such query to answer and reports a control only as it
moves, so an untouched knob is silent by protocol, not merely by observation. So
there is nothing to restore, and
`g_mirror_gain` stays at its initial `1.0` until the knob is first moved. Turning
it once re-syncs the mirror; until then the HDMI copy is at full level wherever
the knob is sitting, so the first touch can be a jump. `audioshim` samples it once per flush and clamps it (`clamp01`), because it
arrives over the shared-state contract rather than from the shim's own arithmetic;
what it is clamped *to* — unity — is then multiplied by the lift below.

That contract is why **both shims ship together**: `g_mirror_gain` is in
`SHMSTATE_SYMBOL_LIST`, and `SHMSTATE_ABI_VERSION` is 2. A new `audioshim`
against an old `knobshim` fails loudly at load (`undefined symbol`); the reverse
mix is the quiet one — an old `audioshim`'s list never mentions the symbol, so it
resolves what it knows and the knob is simply dead — which is what the version
bump exists to make loud. See [08 — Controls](08-controls.md).

### The lift: `RB_AUDIO_MIRROR_BOOST_DB`, and the clamp it needed

**The question was "any way to get audio on HDMI louder?" (2026-09-30), and before
this the honest answer was no.** The knob reaches unity at 1 o'clock and is flat
above it, so the HDMI out sat at the master's own level with its top travel spent;
and the Pi's vc4 HDMI card has **no ALSA mixer control at all** — `amixer -c 1
scontrols` is empty, as are cards 2 and 3, and only card 0 (the headphone jack) has
a `PCM` — so there is no fader in that path to raise. The only volume on the HDMI
output was the monitor's own OSD.

So the mirror takes a **single multiplier of its own**, applied in `flush_master()`
where the mirror's copy is scaled and **nowhere else**: the master's `ol`/`or_`
writes are untouched, so not a sample of the FLX4's stream changes and the unit's
own output stage is unaffected. Default **+4 dB** (×1.5849), the operator's own
choice — it was proposed at +6 and answered *"how about instead of +6 how about
testing +4?"*.

**The number comes from the measured headroom.** rbp's master peaks below full
scale: the loudest 500-block window of the 2026-09-27 run read **3229776 of
8388608**, ≈ **−8.3 dBFS**. +4 dB lands that peak near **−4.3 dBFS**; +6 would
land near −2.3. That is a *peak* figure, not an RMS one, so material with a smaller
crest factor than that run will run out of headroom before the arithmetic says so.

**Running out of headroom now CLIPS rather than wraps, and that is the whole
reason this is safe.** The mirror's samples are saturated to the 24-bit domain
(`S24PACK_SAMPLE_MAX/MIN` in
[s24pack.h](../scripts/shims/s24pack.h)) on the way into the mirror's buffer, by
`mirror_saturate()`, which counts what it pulled back as **`clips=`** on the
mirror log line. Without it the same gain would wrap: `((uint32_t)src[i] << 4) &
0x0ffffff0U` folds a sample pushed past 24 bits, which is the *loud and very
distorted* defect S4.6 measured. `clips=` is the field to watch after changing
this setting or after playing material with more dynamic range than the run above —
it is cumulative, in **samples**, so one loud transient on both channels counts 2.

**The clamp is at the gain, not inside `s24pack()`** — and the first draft of this
change put it in the packer, which was wrong. The packers are *modular on purpose*:
they read bits 23..0 and encode whatever they are given, which is what makes them
insensitive to the missing sign extension in rbp's raw word (rbp's raw `-1` is the
positive `0x00ffffff`). A clamp there turns that raw word into **+8388607** — a
full-scale blast where a quiet *−1* was meant — because on a 24-in-32 card those
low three bytes *are* the sample. It would have replaced a correct reading with the
"aliased waveform at full level" class from the other side. So the domain is
defined in `s24pack.h`, enforced at the one point that can leave it, and
`test_audio.c` pins **both** halves: that the helper saturates, and that the
packers did not start doing it.

The window (**−24…+12 dB**) exists for a mis-keyed order of magnitude: `+40` for
`+4.0` is a factor of 100, i.e. every sample pinned to a rail. A value outside it
is clamped, and the log prints **what was asked for** beside the gain that reached
the samples, so the two numbers differing is the tell. A non-number
(`RB_AUDIO_MIRROR_BOOST_DB=4dB`) answers the default rather than a silent partial
parse.

The setting is read inside `audioshim.so`, so — like the three knobs above — it
needs a shim rebuild and it appears in the startup line:

```
audioshim: mirror dev="hw:CARD=vc4hdmi0,DEV=0 hw:CARD=vc4hdmi1,DEV=0" fmt=subframe_le boost=4.00dB (x1.5849) reopen=5000ms candidates=4 dropped=0
```

### The lift's headroom is rbp's own level — `TRIM` buys boost

The lift has a fixed ceiling to fit under, and it is absolute: `0 dBFS`. What makes
room for it is not on the mirror at all — it is **rbp's own output level**, which
the FLX4's **TRIM** knob sets (`add_abs(rch, CC_TRIM, K_TRIM, sch)`, a channel
strip control, so it moves *both* outputs) and which the periodic `writei` line
reports as **`peak_m`**. The mirror's peak is `peak_m x 1.5849`, so the setting is
a budget, measured on the unit on 2026-09-30:

| TRIM | rbp's loudest sample (`peak_m`) | the mirror at +4 dB | verdict |
|---|---|---|---|
| hot (as found) | 7 132 566 = **−1.41 dBFS** | +2.59 dBFS | `clips=3739` — the ceiling is touched |
| 12 o'clock (detent) | 3 813 818 = **−6.85 dBFS** | **−2.85 dBFS** | clean, `clips +0`, 2.85 dB of margin |

So for +4 dB to be clean, `peak_m` must stay at or below **−4.0 dBFS**; the trim
is the control that decides whether it does. Two consequences worth carrying:

* **The MASTER LEVEL knob cannot fix clipping.** It is a 0..1 attenuator on the
  mirror *and* the unit's own analogue output, so turning it down takes the room
  down with it — it is not a balance control. Turning it *up* past its knee moves
  the room only, because `clamp01()` pins the mirror's gain at 1.0. So the room can
  be restored after a trim change without disturbing the lift.
* **The trim/boost trade changes the balance, not the ceiling.** With a hot trim
  and +1 dB, or a cool trim and +4 dB, the mirror can sit at the same level — but
  the ratio of HDMI to room is the boost alone (1.12x against 1.58x), which is the
  whole point of the lift. Lowering the trim is what buys a bigger lift, and the
  room's loss is recovered on the FLX4's own knob.

### The format is not negotiable, and neither is `hw:`

The vc4 HDMI PCMs offer exactly **one** format — `IEC958_SUBFRAME_LE`, a genuine
IEC 60958 subframe whose 24-bit sample sits at bits 4..27 with its even-parity bit
at 31 (`s24pack.h` has the layout and the driver evidence) — and nothing converts
to it for us. Measured inside the chroot against the libasound the shim dlopens
(2026-09-27, `tools/pcmprobe` sequence mode):

```
plughw:CARD=vc4hdmi0,DEV=0   mask: IEC958_SUBFRAME_LE
                             set_format(32=S24_3LE) FAILED -22   # and S16_LE, S24_LE too
hw:CARD=vc4hdmi0,DEV=0       format 18 runs the whole sequence: rate=44100 channels=2
                             period=256, write ok
```

**A host `aplay` says the opposite** — `aplay -D plughw:CARD=vc4hdmi0,DEV=0 -f
S24_3LE` plays happily and dumps the full 32-format list — because it links the
Pi OS libasound, which is newer than the chroot's. That is the trap: the
instrument that looks authoritative is answering a question about a different
library. Hence `subframe_le` on a `hw:` device, and hence `pcmprobe`'s sequence
mode ([tools/README](../tools/README.md#pcmprobe--asking-libasound-what-its-constants-mean)).

### Two clocks, no resampler

The FLX4 and the HDMI sink run on independent clocks and there is no resampler
between them — the master's clock is the one that must not move. Measured on this
unit, the vc4 HDMI sink consumes **~6.9 frames a second faster** than the FLX4
feeds the mirror (~155 ppm), which drains the mirror's 1024-frame ring.

So the mirror holds its ring near **half full** against both directions:
a **prefill** of 512 frames of silence at each stream start (without it the ring
holds only the 64 frames of one block, the DMA drains that in 1.4 ms, and the
stream is in XRUN before the next block arrives — measured as a `prepare` on 3500
of 3501 blocks), and a **pad** when the level falls below the target, which writes
a **repeat of the last frame the mirror delivered** — a zero-order hold, not
silence. The drop side handles the other direction.

**The pad was silence until 2026-09-30, and the measurement is what changed it.**
The design predicted "about one frame every 0.2 s, 23 µs of silence"; the unit
instead pads **32–41 frames at a time, one every 5.2–6.5 s** — 0.76–0.9 ms, ~145 ppm
of the stream, about eleven a minute. The total is the two clocks' difference and is
right; the *granularity* was the wrong prediction, because `snd_pcm_avail_update()`
on a hw PCM reports against `hw_ptr` and the DMA advances that a whole 256-frame
period at a time, so the level the shim samples is a sawtooth and nearly every
correction is clamped at `MIRROR_PAD_MAX`. At 23 µs silence is inaudible; at 0.8 ms
it is the envelope dropping to zero and coming back — a tick. Holding the last
delivered frame costs one step, from that frame to the one that follows the pad, and
no energy at all. The prefill stays silence deliberately: it is 11.6 ms, long enough
that a DC hold would be its own artefact, and it lands at a stream start under the
startup mute. `padsilent` counts the pads that had no delivered frame to hold — the
silence fallback — so it must stay **0** after the first block; a non-zero reading is
the old behaviour back. `scripts/shims/mirror_policy.c` carries both halves as pure
functions (`mirror_pad_frames` and `mirror_hold_fill`) and `test_audio.c` pins them
on the host; which frame counts as "last" is the shim's own ordering and is a drill
(S4.5).

`/tmp/audioshim.log` carries the record, one line every 500 blocks:

```
audioshim: mirror OPEN hw:CARD=vc4hdmi0,DEV=0 fmt=subframe_le rate=44100 period=256 periods=4 buffer=1024 prefill=512
audioshim: mirror UP hw:CARD=vc4hdmi0,DEV=0 (first block delivered 64 of 64 frames)
audioshim: mirror #50001 blocks=50001 frames=3200064 dropped=0 short=0 retries=1 lost=0 pads=56 padframes=1728 padsilent=0 clips=0 gain=1.000 boost=4.00dB dev=hw:CARD=vc4hdmi0,DEV=0
```

`retries` staying at its startup value, `lost=0` and `dropped=0` are the pass;
`padframes` advancing at ~7 a second is the correction working, not a fault; and
`gain` is the knob's level, printed **raw** rather than clamped so the log shows
what the controls shim actually wrote (`1.000` until the MASTER LEVEL knob is
touched), `boost` is the fixed lift in dB, and `clips` counts the samples that lift
pushed past full scale — see the lift above, where that field is the thing to
watch. The sink's own view is `/proc/asound/card2/pcm0p/sub0/status` —
`RUNNING`, with a `delay` near half the buffer. Both are in the S4.5–S4.8 rows of
the bring-up order ([13](13-raspberrypi4.md#bring-up-order)), and the sink going
away and coming back is S8.8.

**`padframes` carries a second reading, and on 2026-09-28 it was the instrument
that found a defect no other counter could see.** The mirror is fed one block per
master block, so the ring's drain rate *is* the master's block clock: the pad rate
is the feed deficit, and with the card away it read **7.53–7.63 frames per
64-frame block** — ~4560 frames/s of silence against the ~6.9 frames a *second*
two crystals differ by, 660×, i.e. 10.3 % of the monitor's timeline replaced by
~0.17 ms holes about a hundred times a second. That is the *"slow and distorted"*
the operator reported, and it read out here first because every other counter in
the shim counts *content*, and the clock is not content. The cause was the block
clock itself — with no card, the sleep `pace_without_device()` takes on rbp's
behalf — now a **deadline per block**: the deadline advances one period and the
caller sleeps only the remainder, so the block's work and its wakeup come out of
the block's own period ([pace_policy.h](../scripts/shims/pace_policy.h)). After
that fix the cardless mirror pads **0.0096 frames per block**, the card-present
floor (0.0092–0.0093 measured over 500 000-block spans). The drill is S4.10 and
the operator's pulls are S9.8, both in [13](13-raspberrypi4.md).

### The counters cannot tell you the sink is being fed

Every number in that block can be perfect while the monitor is silent, and that is
now **measured** rather than warned about: on 2026-09-28 the operator reported no
HDMI sound, and the state behind the report was `mirror #994501 … dropped=0
short=0 retries=1 lost=0 gain=0.852` 1:1 with a master whose `peak_m` was moving,
the PCM `state: RUNNING` with `hw_ptr` advancing **44 200 frames/s** (132 608
frames in 3 s — the hardware really was consuming the audio), `delay` in its 488–584
band, the sink connected, its EDID advertising exactly this stream (LPCM, 2
channels, 32/44.1/48 kHz, basic-audio flag, FL/FR speaker allocation), and **not
one error line of any kind** in the log.

So the mirror's counters are evidence about *the shim's* side of the wire — that it
opened, that it is being fed, and that the driver is taking the frames. They say
nothing about whether the sink is being fed in a way it can play. The two ways this
port has already seen a mirror "work" and be wrong are the ones to reach for first:
the subframe layout (S4.6 — loud and distorted, never silence) and this one, which
is silence with the counters clean.

What to do about it, in order: **restart the player** — it closes and reopens the
mirror's PCM, which is the one cure measured to work — then check the sink's own
input selection and volume, which the Pi cannot see. Do **not** go looking for a
failed write: an HDMI unplug does not produce one. Measured (S4.12): across a 12 s
replug the mirror's `dropped`/`short`/`lost`/`retries` never moved and `hw_ptr`
went on advancing, i.e. **the PCM drains into a port with nothing on it**, so a sink
that goes away costs the mirror nothing it can see and S8.8's original
`mirror LOST err=-19` prediction is wrong. And that is now measured on **both**
sides at once: a one-sample-a-second sysfs sampler caught the connector reading
`disconnected`/`disabled`/`dpms=Off` with `modes` and `edid` empty (the empty-file
md5 `d41d8cd9`) for the whole of that same unplug — **11 consecutive samples,
02:29:10–02:29:21**, with the connector back on the next second — while `card1`
stayed `state: RUNNING` and the mirror's block counter climbed 756 → 771. So the
sink genuinely leaves; the shim simply has no way to notice. That also means a fix
for a silent sink cannot hang off this path's error handling; the only signals
measured to move on a sink event are `status`, `enabled`, `dpms`, `modes` and
`edid` under `/sys/class/drm/card*-HDMI-A-*/` — and the framebuffer is *not* one of
them (`FBIOGET_VSCREENINFO` returns `pixclock` and every timing length as **0**, so
it carries geometry only). A `[drm] User-defined mode not supported` re-probe line
in `dmesg` is a connector drop too — measured, three for three, at 02:29:10,
02:29:44 and 02:32:25.

**Measuring the rate, if you ever need it:** `hw_ptr` here advances at **44 116
frames/s** against a nominal 44 100 (the difference is the mirror's own pad), and
the honest way to get that number is a `date +%s` pair bracketing a bare `sleep 30`
with *nothing* inside the bracket — 1 323 496 frames over 30 s. Two cheaper ways
overstate it: reading the stamps off a one-second sampler whose iterations cost
more than a second (its own log gives 46 500 frames/s), and a per-second loop that
also greps the shim log (45 022). Both were written down as findings for a while,
and neither is the device.

What is still open is one forward test, and it is the operator's: play a track, pull
the monitor's cable, plug it back, and listen. If the sound dies, a detector is
worth building; if it survives — or if a second replug re-locks it while a player
restart is not needed — then this is the sink's own audio lock, recovered by hand,
and the honest record is that sentence rather than a fix. The 2026-09-28 incident
could not settle it: the sound came back after *both* a player restart and a
monitor power-cycle/replug, and the operator's own reading is the monitor
([13](13-raspberrypi4.md#bring-up-order), S4.12). What the sampler later added is
an argument from timing rather than from audibility — the operator was still
pulling the cable at 02:29:44 and 02:32:25, nine and thirteen minutes *after* the
02:19:58 restart, which reads as the restart not having been the cure.

## S24 sign extension — load-bearing, must not be "fixed"

rbp's S24_LE samples are right-justified in 32-bit words but **not
sign-extended** (the top byte is 0), so a sample of −500000 is the int32 word
`0x00F85EE0`. Before any gain is applied the shim sign-extends with

```c
int32_t sl = (int32_t)(l << 8) >> 8;
```

then scales and stores back. The same helper is used for peak/VU measurement.
Without it, **every gain and every peak reading is wrong** — and it is worth
knowing the other half of the rule: the packing in `s24pack.c` does *not* need
the extension, because it reads only bits 23..0. So a wrong level is never the
packer's doing; look in the arithmetic.

## What must not change

Each of these is a lie rbp needs, verified on the previous target and equally
true here:

* the `mmap` interposition that rewrites rbp's broken `user_space_rtc_init()`
  `/dev/mem` mapping to anonymous memory;
* the sw-params setters always succeeding, and the **2/2 channel min/max lie**;
* `test_rate` answering yes for 44100 and nothing else;
* the `snd_pcm_prepare()` retry after a failed `writei`;
* `readi` answering silence forever, for the capture;
* `snd_ctl_open` fakery (matching the configured card now, not `hw:1,0`);
* `g_vu_peak` still being published even where the bridge drives no meter —
  the controls shim decides what to do with it.

## `SCHED_RT`

`RB_SCHED_RT=1` (default) keeps the interpositions that force `SCHED_FIFO 98`
and core-0 affinity **in force** — i.e. rbp's own RT requests are suppressed.
Setting it to `0` passes the calls through via `dlsym(RTLD_NEXT, …)`. A
preloaded library unconditionally pinning the render thread to a core at RT
priority is a hang risk on any target, which is why this is a switch and not a
constant.

## Cue / PFL (rbp has no PFL keycode)

rbp's XDJ-RX3 panel exposed deck CUE (`0x4102`) and MASTER CUE (`0x4407`) only;
there is **no per-channel PFL keycode**, so a channel CUE drives rbp's mixer
engine directly:

```
mixerengine::MixerEngine singleton      @0x011493c0
setMixerChHeadphoneCue(EnMixerInput,b)  @0x000575a0
getMixerChHeadphoneCue(EnMixerInput)    @0x000575e8
setMasterOutHeadphoneCue(bool)          @0x0005766c
getMasterOutHeadphoneCue()              @0x0005767c
setHeadphoneStereoType(type)            @0x0005768c
```

* a channel CUE button toggles that channel's headphone cue.
* rbp's **master cue is enabled at startup**, because the headphone bus should
  contain the master out of the box — and because rbp has no PFL keycode to
  enable it with, so the shim does it rather than the surface
  ([15](15-flx4-midi.md)). The FLX4's own MASTER CUE button toggles the same
  engine state afterwards.
* a split-cue switch, where the target has one, is
  `HeadPhone::setStereoType` (0 = split, 1 = stereo).

## Audibility: rbp builds the channel faders at ZERO

The port was silent from the day it first played a track, and the reason is not
in this file's chain at all — it is a value rbp needs and nothing was setting.

rbp's mixer engine initialises its two channel faders to **zero**, which is the
safe default when the panel is expected to report where its faders physically
are. A panel that **cannot** report them therefore leaves rbp's master stream as
digital silence while a deck plays: the fader, not the transport, is what is
shut. Nothing in the audio path is broken, so it presents as "the audio shim
does not work" while every log line looks healthy.

`rbp_vu.c`'s `mixer_defaults_tick()` seeds both channel faders at **unity** from
the shim side, under three conditions that make it a default rather than an
override:

* only while `led_query_absolute()` reports that the query **went out** — i.e.
  only on a surface with no way to answer (the FLX4 is found by name on the
  sequencer route, which carries no SysEx; see [15](15-flx4-midi.md));
* only for the first ~30 s after the shim loads, the same window the master
  level is re-asserted in;
* only for a channel whose own control has not reported yet (`g_fader_seen[m]`).
  The moment the physical fader moves, the map's value takes over and the seed
  stops being re-sent, so a fader that is physically down is *not* fought. A
  **side drawer's fader** (`07-touch.md`) counts as that control reporting: it
  writes `g_fader[ch]` and `g_fader_seen[ch]`, so on a machine with no FLX4 the
  drawer is the writer and the seed stops for that channel at the first touch.

Those two arrays are defined **once**, in `fader_state.c`, with default visibility,
and that object is linked into both `fbshim.so` and `knobshim.so` — because the seed
lives in knobshim and the drawer's send lives in fbshim. A second copy would be
invisible to the other shim and the seed would silently overwrite the operator's fader
for the first 30 s of every run. `07-touch.md` shows the `R_ARM_GLOB_DAT` relocations
and the live GOT entries that prove the two bind to one array. Measured in the running
process after a drawer fader drag: `g_fader = (1023, 0, 1023)`, `g_fader_seen =
(0, 1, 0)` — channel 1 claimed by the drawer, seed disarmed for it, and unchanged 35 s
later.

The consequence to know about: **on a first start, both channels come up at
unity until their faders are touched once.** A channel fader parked at the
bottom will therefore play at full level until it is moved. The seed is
deliberately on both `vu_thread` paths (`LED_VU=0` included) because the target
with no absolute controls to report is exactly the target that needs it, and
`LED_VU=0` is a manual OFF switch on the meter bridge rather than a statement
about the surface — a map that declares no meter row gets no hook and no traffic
either way, and the FLX4's map declares one (a host-driven level on CC 2, live
as of 2026-10-01). Putting it inside the meter loop made it dead code on the only
unit that needed it.

The measurement that pins it (unit, 2026-09-26), and the shape of the trap:

```
audioshim: writei #N frames=64 bytes=768 written=64 peak_m=0 mainvol=1.000   <- 242 lines,
                                                                               a deck playing
```

`peak_m` is the master stream's peak **before** the shim's own gain, so zeros
there are proof of digital silence *from rbp* — not of a muted shim, and not of a
missing device. One `cc 19` (the deck-1 channel fader) reaching the map turned
the very next blocks into `peak_m=731447 2260018 1560110 580118`, which is what
identified the fader rather than the transport as the thing that was shut.

## Startup transient

A loud burst can occur right after the stream starts, because rbp's first
buffers contain a full-scale transient — on the previous target it was the
codec's power-up; here it would be USB audio settling. `audioshim.so` can hold
every output channel at zero for `STARTUP_MUTE_MS` after the first write and
then fade in over `STARTUP_FADE_MS`.

**Both default to 1500 ms and 300 ms on the Pi**, and `rb.conf` ships them that
way. They were `0` on the theory that USB audio has no codec power-up transient
to mask — but the FLX4 was found to thump on open, a loud pop/buzz the moment
rbp first touches the card (2026-09-26), so the theory was wrong for the card
this port is aimed at. The cost when a card does not need it is 1.5 s of
silence at startup.

**And the pop is now gone, by ear** — the operator, on 2026-09-30, asked nothing
and reported it in passing: *"that audio popping thing is gone btw"*. That is the
half of this section nothing in the log can supply. The arithmetic above proves
the mute is **armed**; `startup mute released after N frames` appears on a run
with no card just as it does on a working one, so the line can never show that
the thing the mute masks stopped being audible. Ears are the only instrument for
that, and they were the instrument that first found the thump (2026-09-26), so
they close it. What is *not* separated by this is which change did it: the mute
was set to 1500/300 in response to the thump and is the standing explanation, but
the same window holds the pair-map and `access=3` replay work, so the attribution
is "the pop is gone with the mute in force" rather than "the mute alone was
measured to be the cure".

A card that is *late* rather than noisy wants the opposite: set them back to 0
in `rb.local.conf`, or lower `STARTUP_MUTE_MS`, so the port does not sit silent
for longer than the audio takes to arrive. The log line to watch is the shim's
own:

```
audioshim: config ... mute=1500ms fade=300ms
audioshim: startup mute released after 79424 frames
```

79424 frames at 44100 Hz is 1.8 s — the 1500 ms mute plus the 300 ms fade, so
the arithmetic in that line is also a check that the values in force are the
ones intended.

**But that line does not prove a card is open, and it is a trap for exactly that
reason.** `flush_master()` is the master write path with *and* without a device —
with none it ends in `pace_without_device()` — and the mute counter is released
inside it either way. So `startup mute released after N frames` appears on a run
that is producing no sound at all. On 2026-09-26 that is what it was: the FLX4
was not on the USB bus, and the run that printed it had zero `writei #` lines.

**The line that means "a real device is open" is `writei #N frames=... written=...`**
— it is only printed from the branch that actually writes to the card
([audioshim.c:1290-1305](../scripts/shims/audioshim.c)). To tell a silent run
from a working one, in this order:

```sh
grep -c "writei #" /tmp/audioshim.log     # 0 = silent; >0 = the card is being fed
grep -a "NO OUTPUT DEVICE" /tmp/audioshim.log
grep -a "open('hw:CARD=DDJFLX4,DEV=0')" /tmp/audioshim.log   # res=-19 is -ENODEV
cat /proc/asound/cards                                        # no DDJFLX4 entry?
lsusb | grep 2b73:0045                                        # the FLX4 itself
```

`-ENODEV` on the configured device means the card is absent, not misconfigured —
and with `default` out of the chain, both candidates report it and the no-device
path takes over, which is how a missing controller turns into `NO OUTPUT DEVICE`.
This paragraph said something else until 2026-09-27: that `plughw:` *and* `default`
fail too. On this unit `default` did not fail. It opened onto card 0 and refused
every write, and that is the whole of the defect described in
[13](13-raspberrypi4.md#bring-up-order) S4.2.

**Two different `-19`s, and only one of them is a startup problem.** The `open()`
above is the card being absent when the shim starts. The other is `written=-19`
on the *write*, which means the PCM handle rbp holds is stale because the card
re-enumerated underneath it — the USB device dropped and came back with a new
device number, so `/proc/asound/cards` lists it and `lsusb` sees it while the
open fd is dead. The tell is the write count, not the peak: `peak_m` keeps showing
music from rbp's own buffer while the card hears nothing. The measured case is in
[16](16-input-and-hotplug.md#4-hot-swap).

**The reopen, and the retired-handle model it needs** (2026-09-26, **fixed in code
and not yet on the unit**). `snd_pcm_open` hands rbp the real `snd_pcm_t *` itself
and the shim's `is_master`/`is_real` are pointer *equality* against its global, so
there is no shim-owned wrapper to swap underneath rbp — and a stable token would
have to be unwrapped again at the ~13 sites that deliberately forward the
*caller's own pointer*. The smaller fix is a **retired-handle set** and three
predicate edits:

* `is_master()` also accepts a retired handle, so a write to a dead pointer still
  routes into the one place that writes through the *global*;
* `is_real()` stays "the live master", because it gates the calls (`sw_params`,
  `prepare`, `reset_stream_state`, the channel negotiation) that must act on a
  live handle only;
* `is_forwardable()` **excludes** a retired handle — the one that matters, since
  otherwise a dead pointer reaches those forwarding sites and is used as live.

In the write path `-ENODEV` calls `master_lost()`, which clears the global before
closing (so a re-entrant inner call cannot mistake itself for the master), retires
the pointer, closes it and logs **one** `MASTER LOST` line for the whole outage
rather than one per block.

`-ENODEV` is not the only reason to retire a handle, and the other one is the
defect that produced this section. A device that **opens and refuses every block**
reports nothing so specific: measured on the unit, `default` → card 0 opened with
`res=0` and then every write returned `-EINVAL`. So the classifier at the write
site counts the frames that never landed — the verdicts are pure functions in
`scripts/shims/master_policy.h`, with their own tests — and a handle that has
failed to consume `MASTER_DEAD_FRAMES` = 44100 (a second of audio) is retired
exactly as `-ENODEV` retires one, with `MASTER LOST: the handle opened but never
carried a frame …` rather than the card-gone wording. Only *delivered* frames reset
that counter, so a merely slow card cannot trip it: the FLX4's ring is 64 frames ×
2 periods (2.9 ms) and a healthy write is absorbed in 1.45 ms, so it is reachable
only by both attempts on a block failing over and over. A full ring answers
`-EAGAIN` and is **not** a fault — rbp's `SND_PCM_NONBLOCK` survives onto the
handle, so that state really occurs.

Two things about that classifier are worth naming, because both were latent until
it existed. The handle **rbp holds** is pinned and excluded from the retired ring:
the ring's own justification — that a handle retired long enough ago to have been
overwritten cannot still be in rbp's hands — is false for the one handle rbp keeps
for the process's life, so on the 9th distinct loss the pointer left the ring,
`is_master()` stopped recognising it, and rbp's write forwarded a stale pointer to
libasound. And a recovery is complete only once a frame lands: the backoff and
failure counters are cleared by the first *delivered* frame rather than by a
successful open, so a device that opens and refuses everything backs off 500 ms →
5 s instead of re-opening at the floor forever. `snd_pcm_prepare()`'s own log line
is bounded with it — the first three per handle, plus every change of the result,
with the total on the periodic line as `prepares=%lu`; unbounded it was the 1 MB/s
that filled `/tmp` while this was happening.

Recovery runs from the write path itself under a
500 ms → 5 s backoff, and the new handle has the negotiation **replayed** through
the shim's own interposed setters — format, channel count and rate, i.e. exactly
what the pair indices were resolved against, logged as a `replay:` line followed by
`the pair map is unchanged: …`. Two consequences worth knowing:

* **The stream map is deliberately not reset** *when frames have been delivered*.
  `g_cfg.*` has already been clamped against the real channel count, and clearing
  the channel count would silently change what the pair indices mean — the cue pair
  would come back on top of the master. Losing the card costs the audio, not the
  map. The one case that re-resolves is the cold one, where no frame has ever been
  delivered and no index has ever meant anything; see the cold-case paragraph
  below.
* **A reopen re-arms the startup mute**, because the card thumps on a reopen just
  as it does on an open — so `startup mute released` after a `MASTER RECOVERED`
  line is expected and costs 1.8 s of silence, not a new defect. (Before this,
  that line was a trap: it is printed by the no-device path too, which is why it
  never proved a card was open. After a recovery it is preceded by the recovery
  line, which is what makes it unambiguous.)

**The cold case: the pair map is re-resolved against the card that appears.** A unit
booted with no FLX4 already ran the no-device path (`g_real_playback == NULL`), so
being absent at startup and being lost mid-session are the same state. What differs
is what the *map* can mean: in the cold case `resolve_pairs()` had latched the
2-channel fallback and `clamp_pair()` had destructively written `PAIR_NONE` into the
headphones and booth pairs, so for a long time a controller plugged in after boot
recovered **master-pair audio only** — the pair's own `2,3` was gone, so
"re-resolve" was not even possible — and the recovery line said so and said to
restart.

That is fixed (2026-09-27, measured on the unit). `load_config()` keeps a pristine
copy of the map (`g_cfg_map0`), and a recovery that has never delivered a master
frame (`g_master_frames_delivered == 0`) restores it, clears the
resolved/assumed flags, and re-resolves against the new card *before* arming the
startup mute — so the cue pair comes back inside the same 1.8 s window rather than
at the next restart. The guard is on delivered frames rather than on
`g_channels_assumed` because the pair map exists only to route frames that have
actually been written: if none ever has, no index has ever meant anything, and
re-resolving cannot put the cue pair "on top of" a stream that does not exist. The
three outcomes are three distinct sentences in the log — re-resolved and complete,
re-resolved against a card that really is 2-channel (that case keeps the note and
the "restart" advice), or unchanged. The measured sequence, from a boot with the
card absent and the card attached afterwards:

```
replay: access=3 channels=4 rate=44100 period=64 periods=2 …
resolved 4 channel(s): master=0,1 headphones=2,3 booth=-1,-1 monitor=-1,-1
the pair map was resolved again against hw:CARD=DDJFLX4,DEV=0 and is complete …
startup mute released after 79424 frames
writei #47501 frames=64 bytes=768 written=64 peak_m=0 mainvol=1.000 prepares=1
```

**The replay asks for RW_INTERLEAVED whatever the negotiation recorded.** The
recorded access is rbp's *last* request, and on a boot with no card there is nothing
to refuse it: the shim's fake answers every `set_access` with success, so rbp never
takes the fallback it takes against a real card (`set_access req=4` then `req=3` —
visible in any working log). The record therefore ends at **4 =
`SND_PCM_ACCESS_RW_NONINTERLEAVED`**, and a replay that put 4 on the real card had
its failure ignored: this card refuses that access, so the params stayed on the
`hw_params_any` default (`0`, `MMAP_INTERLEAVED`), `hw_params` and `prepare` both
returned 0, and then every `snd_pcm_writei()` returned `-EINVAL`. Nothing in that
sequence reports a fault at configure time; the writes were the only evidence. The
replay now asks for `SND_PCM_ACCESS_RW_INTERLEAVED` unconditionally, and says so when
the recorded value differed. Measured against the card itself with `tools/pcmprobe`
(`… 32 nonblock 4 4`): `set_access` → `-EINVAL`, the object left on
`0 (MMAP_INTERLEAVED)`, `hw_params` ok, `prepare` ok, then **every write `-EINVAL`**
— the same shape, from an instrument that shares no code with the shim. The same
probe at `… 32 nonblock 3 4` writes 4631 frames. **Which layer answers `-EINVAL` to
the write is not yet isolated**: the refused `set_access` never reaches the card, so
the probe's run is really an access-`MMAP_INTERLEAVED` one and it fails exactly as
the shim's replay did; reading `snd_pcm_hw_params_current()` from the handle is what
would name the committed access authoritatively. The rule the shim follows is the
empirical one — `RW_INTERLEAVED` is the only access this card has been observed to
carry frames on.

**One hole, deliberately not fixed:** `snd_ctl_open` hands rbp a real `snd_ctl_t`
for the card, which is just as stale after a re-enumeration, and nothing recovers
it. Since `snd_ctl_pcm_info` never forwards, rbp may simply never ask again — a
`grep -c snd_ctl_open` before and after a replug settles whether it is inert.

## VU meters

The master level comes from `audioshim.so`, which sees the master mix in
`snd_pcm_writei()`, computes a per-channel true-S24 peak with a ~300 ms release,
and publishes it to the shared `g_vu_peak[2]`. The controls shim turns that into
whatever the target can display — and it **does** display on the FLX4, whose
meter is a CC 2 **value ramp** rather than the 11-segment bitmask the SC Live 4
takes: the kind is read from the selected map's meter row (`meter_enc`), and the
FLX4's row is live as of 2026-10-01, confirmed on the panel by the operator's own
eye. `RB_LED_VU` ships `1` and is only a manual OFF switch — a surface whose map
declares **no meter row** gets no hook installed and no traffic whatever the flag
says. (This paragraph used to say the unit "has no meters", and then that the
bridge could not drive the one it has; both are wrong.) See
[08 — Controls](08-controls.md) and [15](15-flx4-midi.md#the-leds).

## For reference: the previous target's 8-channel codec

The SC Live 4's **JP21** codec on `hw:1,0` exposed 8 playback channels
(`aplay -D hw:1,0 -c 8 -f S32_LE`, one pair at a time):

| Channels | Output |
|---|---|
| **0/1** | XLR/RCA main out (Main Vol knob) |
| **2/3** | booth out |
| **4/5** | headphones |
| **6/7** | built-in monitors (speaker/booth knob + on/off switch) |

It had **no ALSA mixer controls** (`amixer -c 1 scontrols` was empty) and a
hardware format of **S24_LE**, so all routing and level was done in software,
and the shim was built around a fixed 8-channel frame with index arithmetic.
That code is gone; the channel *count* is what the Pi target's negotiation
replaced, and the built-in-monitor pair has no equivalent — a Pi has no internal
speakers, so `g_speaker_gain`/`g_speaker_on` have no consumer here.

## Notes

* The master and monitor gains are linear (`val/127`); a log curve would feel
  more natural.
* Check what the card actually offers before changing anything:
  `aplay -L | grep -i flx4`, `cat /proc/asound/cards`, and
  `aplay --dump-hw-params -D hw:CARD=DDJFLX4,DEV=0`. Use `hw:` for this, not
  `plughw:` — the question is what the *card* offers, and a plug device answers
  with what the plug layer will accept on its behalf, which is not the same list.
  `tools/pcmprobe` prints the same thing plus ALSA's own names for the format
  constants.
* `/tmp/audioshim.log` records the device candidates tried, the result of each,
  the negotiated parameters and the stream→pair map. It is the first place to
  look when there is no sound.

## External mixer mode: sending each deck raw

`RB_MIXER_MODE=external` hands the mixing to whatever is on the other end of USB
and takes rbp's mixer out of the path. Each deck goes out **on its own pair**
(`RB_AUDIO_MAP`'s `deck1`/`deck2`, default **2,3** and **4,5**), and **rbp's mix
is not sent at all** — sending both would play every deck twice.

| | `internal` (default) | `external` |
|---|---|---|
| what the card gets | rbp's mix, post Main Vol, on `master` | the two decks, raw, on `deck1`/`deck2` |
| cue/booth pairs | folded from rbp's own streams | not sent |
| rbp's trim, EQ, isolator, fader, crossfader | in the path | **out of the path** |
| rbp's Sound Color FX and Beat FX | in the path | **not on the deck feeds** (see below) |
| HDMI mirror | rbp's mix | unchanged — still rbp's mix |

**The tap is the deck before its channel strip**, which is the only place that
makes "external" mean what it says. rbp's own `mixerengine::MixerChannel::update`
(`0x9e890`) fetches the deck's audio with
`djengine::MixerRouteMngr::getPlayerDataPointer(EnMixerInput)` (`0x85898`) and
*then* applies TRIM → EQ → isolator → fader. That function's whole body is two
loads:

```
r3 = 0x01149f08 + input*4;   r0 = *(u32 *)(r3 + 0x48);   return r0 ? *(u32 *)(r0+4) : 0;
```

so the shim reads the same two words rather than calling in. **There is no hook
and no patched code** — no trampoline, no `mprotect`, no prologue to match — and
the address is re-read every flush instead of cached, so a rebuilt engine or a
moved buffer cannot be missed. Inputs 0 and 1 are the two decks; 2 is preview, 3
mic, 4 aux, and 5/6 the USB/PC pair (read off the live objects' vtables).

**Reading it at flush time is safe, and that is a measurement.** The buffer is a
fixed address written in place each block: measured on `.239` (2026-10-10) deck 1's
pointer held `0xc8b28ea8` across five seconds while its contents moved on every
sample, and deck 2's held `0xc8b474d8`. So the deck taps ride the **same
`snd_pcm_writei`** as the master always did — same block, same instant, no queue
and no second thread — which is what makes the latency identical to internal
mode's rather than merely small. The length needs no bound either: the master
block being written *is* that engine block, so the per-input buffers are at least
that long, exactly as the master's own source is.

**It applies only while an external digital mixer is on the USB bus.** That is the whole
point of the mode — the other device's channel strips — so with none of them plugged in the
decks would go out on pairs nothing is listening to, and the mix (which is what a
controller's own output carries) would have been dropped. The recognised devices are the
`mixer` rows of `scripts/shims/controllers.c`: **euphonia** (id and product string measured on
`.239`), **DJM-V10**, **DJM-A9** and **DJM-900NXS2** (ids from the kernel's own
`sound/usb/quirks-table.h`), **DJM-V5** and **DJM-900NXS** (no id recorded anywhere this tree
could check, so they are reached by model name alone — a guessed id is the drift that table
exists to remove).

The question is asked of **sysfs**, once, by `scripts/shims/usb_devices.c` — the same single
walk behind `controllers_cli detect`, so the shim and the diagnostic cannot answer
differently. Matching is by USB id first and by model name second, both sides normalised
(lowercased, punctuation dropped) so that `DJM-900NXS2`, `DJM900NXS2` and `djm-900nxs2` all
reach one token. Ask it by hand:

```
controllers_cli mixer        # 'id name', or 'none' with exit 1
controllers_cli mixer /tmp   # the same question about a path that is not the bus
controllers_cli detect       # every recognised device, mixers included
```

When the bus has no recognised mixer and `RB_MIXER_MODE` still says `external`, the shim
**says so and stays internal** rather than doing nothing quietly, and the config page hides
the setting instead of offering one that cannot apply — except to say the saved value is
inert, which is the one case where hiding it would leave a setting doing nothing with nothing
on the page to explain why.

Two consequences worth knowing before choosing the mode:

* **The card must be wide enough.** The pairs are hardware channel indices, so
  external mode wants **6 channels** (2 + 2 + 2). On a card opened narrower than a
  pair's indices those decks are skipped and `/tmp/audioshim.log` says so once
  rather than writing past the frame.
* **rbp's FX cannot ride the deck feeds.** Beat FX and Sound Color FX are applied
  at the master, after the channel sum — and the master is exactly what external
  mode does not send. A deck that needs one of rbp's effects wants `internal`.

The shim's constructor logs the mode and the pairs it chose, so
`/tmp/audioshim.log` distinguishes the two modes without inference:

```
audioshim: MIXER_MODE=external -- the deck taps go out on 2,3 and 4,5 and rbp's own mix is NOT sent. ...
```

`RB_MIXER_MODE` is read **once**, in the constructor, like every other value in
this file: it is not a live switch, and changing it takes a player restart.
`start-rb.sh` passes it through as `MIXER_MODE` (`SHIM_VARS`), so a value that is
missing there reaches the shim as empty — which is internal mode, the shipped
behaviour, and also what a hand-run shim with no environment at all gets.
