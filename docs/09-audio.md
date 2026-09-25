# 09 — Audio (JP21 8-channel codec)

XLR main, built-in monitors, headphones (cue + master cue + split cue), booth,
and all panel level knobs/switches are driven by `audioshim.so`.

## JP21 channel map

`aplay -D hw:1,0 -c 8 -f S32_LE`, one pair at a time:

| Channels | Output |
|---|---|
| **0/1** | **XLR/RCA main out** (Main Vol) |
| **2/3** | booth out |
| **4/5** | **headphones** |
| **6/7** | **built-in monitors** (speaker/booth knob + on/off switch) |

The codec exposes **no ALSA mixer controls** (`amixer -c 1 scontrols` is empty)
and the hardware format is **S24_LE** (`hw_params` confirms), so all routing and
level is done in software in `audioshim.so`.

## audioshim routing

`scripts/shims/audioshim.c` opens `hw:1,0` with 8 channels (S24_LE, 44.1 kHz):

| rbp stream | ch | gain |
|---|---|---|
| master (device 0) | 0/1 | `g_master_gain` (Main Vol, CC20) |
| master | 6/7 | `g_speaker_gain` (CC15) × `g_speaker_on` (note 41) |
| phones (device 1) | 4/5 | routed **straight through** (rbp mixes it) |
| booth (device 2) | 2/3 | — |

rbp's **phone stream is the headphone bus**. With rbp's **master cue enabled**
it contains cue + master, already mixed and time-aligned by rbp's `HeadPhone`
object (which applies the cue mix, cue level and stereo/split type). The shim
therefore routes ch4/5 directly rather than summing rbp's separate master and
cue ALSA devices (which are not sample-aligned and would comb-filter).

## S24 sign extension

rbp's S24_LE samples are right-justified in 32-bit words but **not
sign-extended** (the top byte is 0), so a sample of −500000 is the int32 word
`0x00F85EE0`. Before any gain is applied the shim sign-extends with
`int32_t sl = (int32_t)(l << 8) >> 8;`, then scales and stores back as S24. The
same helper is used for peak/VU measurement.

## Level / switch controls

| SC Live 4 control | MIDI | Effect |
|---|---|---|
| Main Vol | CC20 ch15 | `g_master_gain`, applied to **ch0/1 only** |
| Speaker/booth | CC15 ch15 | `g_speaker_gain`, applied to **ch6/7 only** |
| Speaker on/off switch | **note 41 ch15** | `g_speaker_on` gates ch6/7 |
| Cue mix | CC18 ch15 | `0x4405` → rbp `HeadPhone` mix rate |
| Cue level | CC19 ch15 | `0x4406` → rbp headphone level |
| Split cue | **note 11 ch15** | `HeadPhone::setStereoType` (0 = split, 1 = stereo) |

Because rbp initialises its mixer *after* the shims load, the VU thread
re-asserts rbp's master level to unity for the first ~30 s (alongside the
fader/EQ absolute-control re-assert).

## Cue / PFL (rbp has no PFL keycode)

rbp's XDJ-RX3 panel only exposes deck CUE (`0x4102`) and MASTER CUE (`0x4407`);
there is **no per-channel PFL keycode**, so the SC Live 4 strips drive rbp's
mixer engine directly:

```
mixerengine::MixerEngine singleton      @0x011493c0
setMixerChHeadphoneCue(EnMixerInput,b)  @0x000575a0
getMixerChHeadphoneCue(EnMixerInput)    @0x000575e8
setMasterOutHeadphoneCue(bool)          @0x0005766c
getMasterOutHeadphoneCue()              @0x0005767c
setHeadphoneStereoType(type)            @0x0005768c
```

* strips **1/2** PFL (ch 0/1, note 13) → toggle that channel's headphone cue;
  the strip's PFL LED mirrors the state.
* strips **3/4** PFL (ch 2/3, note 13) → toggle rbp's **master cue** (the SC
  Live 4 has no MASTER CUE button). Their LEDs mirror it.
* rbp's **master cue is enabled at startup** so the headphone bus contains the
  master out of the box.

## VU meters

Master and channel meters both come from **rbp's own meters**
(`MonoLvMeter::getLedValue` hook, 11 segments → panel's 6), with the Main Vol
applied to the master in dB. Using one source keeps them consistent. See
[08 — Controls](08-controls.md) for the MIDI details.

## Startup transient

A loud ~100 ms burst from the built-in monitors occurs right after the stream
starts (rbp's first buffers contain a full-scale transient). `audioshim.so`
holds every output channel at zero for `STARTUP_MUTE_MS` (default 1500 ms)
after the first write, then fades in over `STARTUP_FADE_MS` (default 300 ms).

```sh
STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300   # defaults
STARTUP_MUTE_MS=0                          # disable
```

## Notes

* The speaker/booth and master-out gains are linear (`val/127`); a log curve
  would feel more natural.
* rbp's MASTER CUE is forced on at startup (no physical button); the strip 3/4
  PFL buttons toggle it afterwards.
