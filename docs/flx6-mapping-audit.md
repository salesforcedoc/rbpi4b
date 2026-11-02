# DDJ-FLX6 mapping inventory — 2026-11-02

This is the final two-deck layout and audit result. Hardware decks 3/4 provide
auxiliary controls for RX3 decks 1/2. FLX4 remains an unused compatibility map.

Sources: Pioneer's [official MIDI list](https://downloads.support.alphatheta.com/software_info/dj-controllers/DDJ-FLX6/DDJ-FLX6_MIDI_message_List_E1.pdf),
[Mixxx Mapping](https://github.com/dolchebeats/DDJ-FLX6-Mixxx-mapping)
[current map](../scripts/shims/map_flx6.c). Native keys/APIs and disassembly
provenance are in [rbp_abi.h](../scripts/shims/rbp_abi.h).

All MIDI bytes below are hexadecimal. Binding counts are decimal: 190 unique
note-table addresses and 14 unique absolute-control addresses, with additional
specialized handlers. No duplicate addresses were found in the binding tables.

## Inputs

| Control | MIDI | RX3 action |
| --- | --- | --- |
| PLAY / CUE | 90/91 0b/0c | Native transport and cue |
| SHIFT+PLAY / SHIFT+CUE | 90/91 47/48 | Return to main cue and play / pause at track beginning |
| SYNC / MASTER | 90/91 58/5c | Sync / master |
| SHIFT+SYNC / SHIFT+MASTER | 90/91 5d/60 | Quantize / tempo range |
| Jog touch and shifted touch | 90/91 36/67 | Same native touch binding, including cross-SHIFT release |
| Jog rim/platter and shifted movement | B0/B1 21/22/23/26/29 | Native jog; confirmed FLX6 sensitivity, no extra shifted speed multiplier |
| Tempo | B0/B1 00/20 | 14-bit pitch |
| LOOP IN/OUT and shifted aliases | 90/91 10/11 and 4c/77 | Native loop setting/boundary adjustment |
| Hold LOOP IN | 90/91 10, additional14 ignored | Native held-IN four-beat shortcut; avoids duplicate firmware action |
| RELOOP / SHIFT+RELOOP | 90/91 4d/50 | Reloop/exit / toggle called saved memory loop's Active flag |
| CALL left/right | 90/91 51/53 | Previous/next memory cue or loop |
| SHIFT+CALL | 90/91 61/62 | Halve/double an active loop |
| MEMORY / SHIFT+MEMORY | 90/91 3d/3e | Save/delete memory cue |
| Auxiliary SYNC / MASTER | 92/93 58/5c | RX3 deck1/2 Master Tempo / Slip |
| Auxiliary SHIFT+JOG CUTTER | 92/93 17, SHIFT held | Reverse; unsolicited startup ON ignored |
| Auxiliary CALL / SHIFT+CALL | 92/93 51/53 and 61/62 | Held Search backward/forward / previous/next Track Search |
| SHIFT | 90..93 3f | Tracks physical-side SHIFT across deck-layer switches |
| Trim / EQ / fader1/2 | B0/B1 04/07/0b/0f/13 plus corresponding CC+20 LSB | 14-bit assembly to native 10-bit mixer positions |
| Color amount1/2 | B6 17/37 and18/38 | Individual channel Color FX amount |
| Channel3 FILTER | B6 19/39 | Color FX amount on both RX3 channels |
| Channel CUE1/2 | 90/91 54 | PFL toggle, duplicate ON suppressed |
| SHIFT+PFL physical3/1/2/4 | 92/90/91/93 68 | Dub Echo / Sweep / Noise / Filter on both RX3 channels |
| Crossfader | B6 1f/3f | 14-bit position; decks assigned A/B, sharp curve at startup |
| Browse normal/shifted rotation | B6 40/64 | Native list navigation or waveform zoom, context-dependent |
| Browse push / shifted push | 96 41/42 | Selector / BACK |
| BACK / SHIFT+BACK | 96 65/66 | BACK / MENU |
| VIEW / SHIFT+VIEW | 96 7a/68 | BROWSE-waveform toggle / SOURCE |
| LOAD1/2 | 96 46/47 | Native load; double-click Instant Doubles |
| SHIFT+LOAD1/2 | 96 58/59 | Native double-LOAD gesture for Instant Doubles |
| FX SELECT | 94/95 70..72 | Echo / Spiral / Reverb / Trans / Flanger / Phaser |
| FX target | 94/95 1e/1c/1d/1f/14 | A / 1 / 2 / B / MASTER |
| BEAT left/right | 94/95 06/07 | Previous/next Beat FX beat |
| SHIFT+BEAT left/right | 94/95 66/6b | AUTO / TAP |
| LEVEL/DEPTH | B4/B5 02/22,04/24,06/26 | Six 14-bit packet sources feed global native FX depth |
| FX ON/OFF | 94/95 47..49 | Global Beat FX toggle |
| Right Merge preset / shifted preset | 95 2f/30 | Next/previous utility preset |
| Right Merge push, presets1..4 | 95 2e | USB1 eject / USB2 eject / Track Mark / master recording toggle |
| SHIFT+right Merge push | 95 31 | Align software utility preset1; select physical preset1 first |
| Hold left Merge push five seconds | 94 2e | Host shutdown -h now; release/SHIFT/connection change cancels hold |

The eight pad selectors 90/91 1b/1e/20/22/69/6b/6d/6f request Hot Cue,
Beat Loop, Slip Loop, Beat Jump, Gate Cue, Beat Loop2, Release FX and Beat
Jump2. The 128 regular bindings use channels97/99, bank bases00..70 with
pad offsets0..7. The 112 shifted non-Hot-Cue addresses on98/9a alias those
actions. SHIFT+Hot Cue pads delete cues. Primary mode lamps remain firmware-owned:
host writes previously changed the hardware bank and broke shifted selection.

## Outputs and hardware-owned controls

| Output/control | Implementation |
| --- | --- |
| Transport | Native PLAY/CUE/SYNC/MASTER, including shifted PLAY/CUE lamp aliases47/48 |
| Quantize / loops | Native quantize and loop state; normal and shifted IN/OUT addresses |
| Auxiliary lamps | SYNC/MASTER normal/shifted aliases indicate RX3 Master Tempo/Slip |
| Channel CUE / shifted CUE | Native PFL / actual selected Color FX |
| Pads | Native occupancy/activity and blink state, binary brightness, mirrored to SHIFT channels |
| Jog rings / load animations | BB 00..03 / 9F 00..03; actual native position and load state, auxiliary mirror |
| FX ON/OFF | Clears five inactive slots then writes selected slot last; requires active native type AND enable flag |
| BEAT display | B4/B5 64; all twelve RX3 rungs represented, including measured3/4 and agreed blink fallbacks |
| Meters | B0/B1 02; five cumulative hardware bands from native pre-fader channel state |
| Right Merge ring | B5 10; USB readiness or recording/Track Mark feedback |
| Left Merge ring | B4 10; shutdown hold blinks1Hz for0–2s,2Hz for2–4s,5Hz for4–5s; steady after hold, OFF on cancel/release |
| Merge preset numbers | Controller-owned; no undocumented reset/output commands |
| Headphone MIX / MASTER CUE | Hardware mixes USB master0/1 and cue2/3. Native headphone stream stays cue-only; no host MASTER CUE lamp writes |

The shutdown ring acknowledges the completed hold, not successful poweroff.
Its reader never starts shutdown; left/right ring caches and retries are separate.
The shutdown helper enters the host root through /proc/1/root and execs the host
command with a clean environment, allowing systemd to stop services/unmount.

## Audit corrections and remaining limits

The audit's FX-depth A→B→A defect is fixed by one global dedupe cache with
separate per-slot packet assembly. Channel CUE now deduplicates ON reports.
Mixer controls use 14-bit pairs. Shifted jog/pad coverage and held-input cleanup
were added. Connection cleanup handles disappearance and fresh subscriptions
even when ALSA reuses a client address; it cancels held keys, jog motion, queued
pad/transport operations and partial position packets.

SHIFT+PLAY waits for native cue standby before playing, because BackCueACB
can pause after buffering. Loading, normal PLAY/CUE, disconnect, track change
or timeout cancels pending playback. SHIFT+CUE retains the stored main cue.

Normal hot-plug is user-confirmed, but automatic current Merge preset discovery
is unsupported by the supplied protocol. Explicit calibration avoids a restart:
select physical preset1, then SHIFT+right Merge push. Very fast same-track load
animations can still fall between LED polls. Firmware Jog Cutter remains active;
the shim does not implement it and has no documented disable command.

Intentionally unassigned: left Merge preset controls and both rotations,
SHIFT+tempo, long VIEW, SHIFT+FX ON/OFF, fader-start messages, LOAD3/4 and
remaining auxiliary controls/pads/mixer strips. SHIFT+PFL already supplies
Color selection; the rejected left Merge selection proposal was not implemented.
Analog booth/mic routing is outside this MIDI layout. Additional native RX3
functions without shortcuts include vinyl speed adjustment, TIME MODE/AUTO CUE,
needle search, remaining FX selections, fine FX TIME, Beat FX Quantize and
dedicated TAG LIST/SEARCH/TRACK FILTER. PLAYLIST's button was deliberately freed.

## Verification

User-confirmed: core controls, eight pad modes/lights, meters, FX lamp/beat
display, right Merge utility, headphone correction, startup Reverse and ordinary
hot-plug. The latest audit fixes, new shortcuts, 14-bit mixer, held-input cleanup,
calibration, shutdown and countdown are locally built/checked, with no subsequent
Pi confirmation reported. Compilation does not establish hardware behavior.

ARM build/ABI checks and controller registry tests (243 checks) pass. Temporary
host harnesses cover pad transitions/bindings, thirty cross-slot FX-depth cases,
shortcut/CUE edges, native cue buffering/cancellation, 14-bit mixer/global Color,
MIDI subscription changes, startup query, LED routing/retry and shutdown timing.
Shutdown tests mock fork/filesystem/child syscalls and verify exact command/root/
environment behavior; they never shut down this machine. No repository
test_flx6.c was added, per the user's preference.
