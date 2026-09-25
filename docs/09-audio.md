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
RB_AUDIO_DEV=plughw:CARD=DDJFLX4,DEV=0
```

`plughw` on purpose. `rbp` speaks only S24_LE at 44.1 kHz, while the FLX4's USB
audio is whatever it is — `S24_3LE`, `S16_LE`, `S32_LE`, 44.1 or 48 kHz, all
`TODO: unverified` ([15](15-flx4-midi.md) records the question, and the answer
does not change the shim). ALSA's plug chain does that packing and rate
conversion for free, so none of it has to be written here.

The candidate list, tried in order, is built once at startup rather than
assembled on the failure path — a fallback chain whose last step only runs when
something is already wrong is a step nobody has tested:

1. `AUDIO_DEV`;
2. if it starts with `hw:`, the same string as `plughw:` (a `hw:` device that
   will not take rbp's format almost always will through `plug`);
3. `default`;
4. nothing — silent, sleep-paced, one loud log line.

`RB_AUDIO_FMT` is the escape hatch in the other direction: `s24_le` (default),
`s24_3le`, or `s16_le` makes the **shim** do the packing, which is what you need
for a `hw:` device that refuses the plug chain.

## Channels: `AUDIO_CHANNELS=auto`, and the 1:1 rule

`auto` reads `snd_pcm_hw_params_get_channels_max()` from the real handle and
clamps it to the 8 the output buffer holds. A number forces that count.

**The rule: ask for exactly as many logical channels as the card has.** The
route plugin is 1:1 only while logical ≤ hw; ask for more and it *downmixes*,
which folds front-left/front-right into the surround pair and puts garbage on
the second output. This is the failure that sounds like a working card and a
broken mix, and it is why the count is negotiated instead of forced to 8 the way
the previous target's code did.

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
* `g_vu_peak` still being published even on a target with no meters to light —
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

## Startup transient

A loud burst can occur right after the stream starts, because rbp's first
buffers contain a full-scale transient — on the previous target it was the
codec's power-up; here it would be USB audio settling. `audioshim.so` can hold
every output channel at zero for `STARTUP_MUTE_MS` after the first write and
then fade in over `STARTUP_FADE_MS`.

**Both default to `0` on the Pi**, and `rb.conf` ships them that way. USB audio
has no codec power-up transient to mask, so muting by default would trade a real
silence for a hypothetical click. Turn them on if a particular interface turns
out to need it:

```sh
RB_STARTUP_MUTE_MS=1500 RB_STARTUP_FADE_MS=300
```

## VU meters

The master level comes from `audioshim.so`, which sees the master mix in
`snd_pcm_writei()`, computes a per-channel true-S24 peak with a ~300 ms release,
and publishes it to the shared `g_vu_peak[2]`. The controls shim turns that into
whatever the target can display — and on the FLX4, which has no meters,
`RB_LED_VU=0` means the meter hook is not installed and nothing reads it. See
[08 — Controls](08-controls.md).

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
  `aplay --dump-hw-params -D plughw:CARD=DDJFLX4,DEV=0` for the negotiated
  format, rate and channel count.
* `/tmp/audioshim.log` records the device candidates tried, the result of each,
  the negotiated parameters and the stream→pair map. It is the first place to
  look when there is no sound.
