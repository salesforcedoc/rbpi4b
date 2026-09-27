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
RB_AUDIO_DEV=hw:CARD=DDJFLX4,DEV=0
```

`hw:`, **not** `plughw:` — and this is a measurement, not a preference. The reason
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
3. `default`;
4. nothing — silent, sleep-paced, one loud log line.

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
Nothing anywhere in the law can exceed 1.0, so this change only ever *lowers* what
a given position gives — it moves the knee, it does not add level. If what is
wanted is more level available rather than a different knee, that is the boost
below, and it needs the `s24pack()` clamp first.

The one mercy of the travel being the whole range is that the **bottom is
unaffected**: raw 0 is the stop and it still maps to silence, exactly as under
every earlier version of this law. Under the 0.65 reading it would not have been,
which is the second thing that bad premise would have got wrong.

The **travel above the knee being flat rather than boosted** is deliberate.
`s24pack()` does not clamp — `((uint32_t)src[i] << 4) & 0x0ffffff0U`
**wraps** a sample pushed past 24 bits, folding the waveform into the "loud and
very distorted" defect S4.6 measured — so `clamp01(g_mirror_gain)` in `audioshim`
is a ceiling, not a limit to be raised. rbp's master does peak below full scale
(loudest 500-block window of the 2026-09-27 run: 3229776 of 8388608, ≈ **−8.3
dBFS**), so a boost is possible in principle — but it needs a saturating clamp in
`s24pack()` first. That is a separate change and is deliberately not this one.

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
arrives over the shared-state contract rather than from the shim's own arithmetic.

That contract is why **both shims ship together**: `g_mirror_gain` is in
`SHMSTATE_SYMBOL_LIST`, and `SHMSTATE_ABI_VERSION` is 2. A new `audioshim`
against an old `knobshim` fails loudly at load (`undefined symbol`); the reverse
mix is the quiet one — an old `audioshim`'s list never mentions the symbol, so it
resolves what it knows and the knob is simply dead — which is what the version
bump exists to make loud. See [08 — Controls](08-controls.md).

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
of 3501 blocks), and a **pad** of a few frames of silence when the level falls
below the target. At the measured drift that is about one frame every 0.2 s — 23 µs
of silence — and the drop side handles the other direction. Both are silence, so
neither touches what the master hears.

`/tmp/audioshim.log` carries the record, one line every 500 blocks:

```
audioshim: mirror OPEN hw:CARD=vc4hdmi0,DEV=0 fmt=subframe_le rate=44100 period=256 periods=4 buffer=1024 prefill=512
audioshim: mirror UP hw:CARD=vc4hdmi0,DEV=0 (first block delivered 64 of 64 frames)
audioshim: mirror #50001 blocks=50001 frames=3200064 dropped=0 short=0 retries=1 lost=0 pads=56 padframes=1728 gain=1.000 dev=hw:CARD=vc4hdmi0,DEV=0
```

`retries` staying at its startup value, `lost=0` and `dropped=0` are the pass;
`padframes` advancing at ~7 a second is the correction working, not a fault; and
`gain` is the mirror's level, printed **raw** rather than clamped so the log shows
what the controls shim actually wrote (`1.000` until the MASTER LEVEL knob is
touched). The sink's own view is `/proc/asound/card2/pcm0p/sub0/status` —
`RUNNING`, with a `delay` near half the buffer. Both are in the S4.5–S4.8 rows of
the bring-up order ([13](13-raspberrypi4.md#bring-up-order)), and the sink going
away and coming back is S8.8.

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
  stops being re-sent, so a fader that is physically down is *not* fought.

The consequence to know about: **on a first start, both channels come up at
unity until their faders are touched once.** A channel fader parked at the
bottom will therefore play at full level until it is moved. The seed is
deliberately on both `vu_thread` paths (`LED_VU=0` included) because the target
whose meter this bridge cannot drive is exactly the target with no absolute
controls — putting it inside the meter loop made it dead code on the only unit
that needed it.

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
and the fallbacks below it (`plughw:`, `default`) then fail too, which is how a
missing controller turns into `NO OUTPUT DEVICE`.

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
rather than one per block. Recovery runs from the write path itself under a
500 ms → 5 s backoff, and the new handle has the negotiation **replayed** through
the shim's own interposed setters — format, channel count and rate, i.e. exactly
what the pair indices were resolved against, logged as a `replay:` line followed by
`the pair map is unchanged: …`. Two consequences worth knowing:

* **The stream map is deliberately not reset.** `g_cfg.*` has already been clamped
  against the real channel count, and clearing the channel count would silently
  change what the pair indices mean — the cue pair would come back on top of the
  master. Losing the card costs the audio, not the map.
* **A reopen re-arms the startup mute**, because the card thumps on a reopen just
  as it does on an open — so `startup mute released` after a `MASTER RECOVERED`
  line is expected and costs 1.8 s of silence, not a new defect. (Before this,
  that line was a trap: it is printed by the no-device path too, which is why it
  never proved a card was open. After a recovery it is preceded by the recovery
  line, which is what makes it unambiguous.)

**The cold case is the same mechanism, with one honest limit.** A unit booted with
no FLX4 already ran the no-device path (`g_real_playback == NULL`), so being absent
at startup and being lost mid-session are the same state and only the *return* was
missing. But in the cold case `resolve_pairs()` had already latched the 2-channel
fallback and dropped the headphones and booth pairs, so plugging a controller in
after boot recovers **master-pair audio only**, and the recovery line says so and
says to restart.

**One hole, deliberately not fixed:** `snd_ctl_open` hands rbp a real `snd_ctl_t`
for the card, which is just as stale after a re-enumeration, and nothing recovers
it. Since `snd_ctl_pcm_info` never forwards, rbp may simply never ask again — a
`grep -c snd_ctl_open` before and after a replug settles whether it is inert.

## VU meters

The master level comes from `audioshim.so`, which sees the master mix in
`snd_pcm_writei()`, computes a per-channel true-S24 peak with a ~300 ms release,
and publishes it to the shared `g_vu_peak[2]`. The controls shim turns that into
whatever the target can display — and on the FLX4, whose meter is a CC 2 **value
ramp** rather than the 11-segment bitmask this bridge drives, `RB_LED_VU=0` means
the meter hook is not installed and nothing reads it. (This used to say the unit
"has no meters", which is wrong — it has one of a different kind; see
[15](15-flx4-midi.md#the-leds).) See [08 — Controls](08-controls.md).

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
