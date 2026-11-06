# scripts/shims/

LD_PRELOAD libraries and static helpers adapting Raspberry Pi hardware to
the XDJ-RX3 player. Original shim code is MIT-licensed; shims call native
player entry points and translate hardware.

The current two-deck FLX6 layout is recorded in the
[full mapping inventory](../../docs/flx6-mapping-audit.md).
FLX6 is the default; FLX4 remains a compatibility map. Physical decks3/4 provide
extra buttons for RX3 decks1/2, rather than two additional players.

Optional [persistent touch controls](../../docs/23-full-touch-ui.md) add native
navigation, MENU/UTILITY, SHORTCUT and per-deck VINYL/SLIP/MT through `fbshim.so`.
Set `RB_POINT_FULL_UI=1` after deploying the updated shim and launcher. The
feature defaults off and still needs Pi acceptance.

Optional [RX3 toolkit features](../../docs/21-rx3-toolkit.md) use a separate
pinned toolkit bundle. All default off. The FLX6 map offers an optional
pad/mode/SHIFT callback so Samples can receive shim events; existing MIDI
assignments stay intact. See that guide for the build, enable/rollback and Pi-verification limits.

User-confirmed features include the core controls, eight pad modes/lights,
meters, FX ON/OFF and beat lights, headphone mixing, right Merge utilities,
startup Reverse correction and normal hot-plug. The latest audit fixes,
shifted transport/LOAD, 14-bit mixer, disconnect cleanup, preset calibration,
shutdown and countdown are built/checked locally; no subsequent Pi verification
has been reported. The mapping inventory records the current verification state.

## FLX6 controls

| Gesture | RX3 action |
| --- | --- |
| Double-click LOAD or SHIFT+LOAD1/2 | Native Instant Doubles on the destination deck |
| SHIFT+CUE | Pause at track beginning, retaining the main cue |
| SHIFT+PLAY | Return to main cue and play once native cue buffering completes |
| SHIFT+PFL, physical3/1/2/4 | Dub Echo / Sweep / Noise / Filter on both RX3 channels |
| Channel3 FILTER | Shared Sound Color FX parameter (e.g. resonance); individual1/2 amount knobs remain mapped |
| MEMORY / SHIFT+MEMORY | Native memory save/delete |
| CALL left/right / SHIFT+CALL | Previous/next memory cue or loop / halve/double an active loop |
| SHIFT+LOOP IN/OUT | Native loop boundary adjustment |
| SHIFT+RELOOP | Toggle the called saved memory loop's Active flag |
| JOG CUTTER, primary decks1/2 | RX3 deck1/2 Slip; Jog Cutter LED outputs disabled |
| Auxiliary deck3/4 MASTER (SHIFT+SYNC) | RX3 deck1/2 Master Tempo; auxiliary SYNC is unassigned |
| Auxiliary deck3/4 JOG CUTTER | Reverse without SHIFT; Jog Cutter LED outputs disabled |
| Auxiliary CALL / SHIFT+CALL | Held Search backward/forward / Track Search previous/next |
| SHIFT+Browse/jog/non-Hot-Cue pads | Same existing selector/jog/pad action; Hot Cue SHIFT+pad remains delete |

SHIFT+PLAY is refused without a loaded track/main cue and cancels on normal
PLAY/CUE, load, disconnect, track change or a two-second timeout. For Active
Loop, call a saved memory loop first; ordinary memory cues and hot cues are
refused. The copied cue goes through native DbProxy save/notifications; the
shim does not rewrite the database itself. Native API addresses/provenance
are recorded in rbp_abi.h.

FLX6 jog pulse position and speed are divided by3 by default, following the
operator's report of three ring rotations per hand rotation. Fractional pulses
are accumulated. `RB_JOG_DIVISOR=1` restores the old response; the ring continues
to follow actual sample position. These latest changes need Pi retesting.

The four physical pad families plus SHIFT select all eight RX3 modes.
Mode-button output is deliberately disabled: writes can select the hardware
bank and previously cancelled shifted selections. Pad lights follow the
physical bank and actual native occupancy/activity. Jog rings follow native
sample position (0.6075 revolutions/s); load animations observe loading and
music-ID changes, mirrored to auxiliary layers. A very fast same-track reload
can fall between LED polls.

The FX selector exposes Echo, Spiral, Reverb, Trans, Flanger and Phaser.
The target selector maps A/1/2/B/MASTER. All six ON/OFF and depth aliases are
handled. Depth assembles each hardware slot separately and deduplicates against
one global value. ON/OFF light output clears inactive slots and writes the
selected slot last; native active type and enable flag both gate it.
BEAT left/right map previous/next beat; SHIFT+BEAT left/right map AUTO/TAP.

## Merge FX utilities and shutdown

| Right Merge FX preset | Knob press | Ring |
| --- | --- | --- |
| 1 | Eject USB1 | On when ready, blinking while present/not ready, off when absent |
| 2 | Eject USB2 | Same for USB2 |
| 3 | Track Mark | On while recording, brief dark pulse for a request |
| 4 | Toggle master recording | Blinking while native recorder reports recording |

Right preset95/2f cycles forward; SHIFT+preset95/30 cycles backward; push is
95/2e. Duplicate ONs and cold releases are suppressed. Eject uses native
K_USBSTOP press/repeat/release on slot channel1/2; Track Mark0402 and USB REC0401
use native recorder channel0. Recording requires writable USB2; Track Mark
splits an active recording. Its feedback acknowledges a request, not file creation.

**Preset alignment:** select physical right preset1, then SHIFT+press the right
Merge knob (95/31) to set the software counter to1 without a restart or media
action. Alternatively start the player with physical preset1 selected.
Automatic current-preset discovery is unsupported by the supplied protocol;
preset numbers are controlled locally by firmware. Right ring output is B5/10.

**Shutdown:** hold unshifted LEFT Merge knob push (94/2e) for five seconds.
A short press does nothing. Early/shifted release or connection change cancels;
duplicate ONs do not restart the timer. Only one request is sent per hold.
Left ring B4/10 lights immediately and blinks1Hz for0–2s,2Hz for2–4s,5Hz
for4–5s. It is OFF on cancellation/release and steady after the hold completes.
Steady light acknowledges the hold, not successful poweroff.

The root-run player is inside the firmware chroot. A child enters the host root
through /proc/1/root, then executes host shutdown -h now with a clean environment
and no LD_PRELOAD. Systemd performs the normal stop/unmount/poweroff sequence;
no direct kernel halt is used. Errors go to /tmp/knobshim.log. Let the Pi finish
shutting down before removing power. Left preset controls and both rotations
remain unassigned.

## Beat display, audio and reconnect

The display reads the native Beat FX/X-PAD rung, including while FX is off, and
sends B4/B5 CC64. All MIDI values below are hexadecimal.

| RX3 beat value | MIDI value | FLX6 display |
| --- | --- | --- |
| 1/16 | 2a | Quarter blinking regularly |
| 1/8 | 2d | Quarter blinking slowly |
| 1/4 | 03 | Quarter |
| 1/2 | 04 | Half |
| 3/4 | 21 | Three quarters |
| 1 | 05 | 1 |
| 2 | 06 | Native2-beat pattern (documented as1+4) |
| 4 | 07 | 4 |
| 8 | 08 | 8 |
| 16 | 09 | 16 |
| 32 | 0a | 32 |
| 64 | 37 | 32 blinking slowly |

The official standard codes and measured21/2a/2d/37 come from the user's
hardware probes. Paired unit outputs did not combine; the blinking fallbacks
are the user's choice. Unchanged commands refresh every3s to preserve native
blink cadence; failed writes retry. See flx6-beat-led-probe.py under scripts/device.

Standalone USB tests confirmed that hardware headphone MIX blends master0/1 and
cue2/3, with MASTER CUE gating master. Native headphones stay stereo cue-only:
rbp master cue OFF and MIX fully CUE. Do not mix master into the USB cue pair a
second time. Channel PFL still selects cue sources. This also applies with VU
disabled. flx6-headphone-probe.py reproduces the routing test.

Mixer controls assemble 14-bit pairs to native10-bit positions; PFL suppresses
duplicate ONs. Crossfader startup assigns decks A/B with sharp curve.
The startup position request F0 00 20 7F 03 01 F7 is sent only to an identified
FLX6 route with ready engine/KeyManager, at most three times per connection.
After six seconds unreported faders may use unity fallback; reported positions
are preserved. Logs distinguish a sent query from received absolute reports.
Connection changes release held keys, stop jog motion, cancel pending mode/
transport requests and invalidate packet/button state. The event/tick/connection
paths share a mutex. Other maps retain their own audio/control policies.

## Build

Soft-float ARM, glibc-2.4-only, linked against the RX3 rootfs:

```sh
make RX3=/path/to/extracted/XDJRX3-rootfs
make RX3=/path/to/extracted/XDJRX3-rootfs check
make RX3=/path/to/extracted/XDJRX3-rootfs test     # needs qemu-arm in PATH
```

`check` fails the build if a shim references `GLIBC_2.17+` or is hard-float.
The `compat/` dir (empty `libc_nonshared.a` / `libpthread_nonshared.a`) is
created automatically so `-lpthread` links.

`test` builds the unit tests static and runs them under `qemu-arm` — no device,
no Pi, no rootfs to load. It fails on the first non-zero exit, so `make test`
in CI is a single answer rather than a log to read.

Build a single target during development, e.g.
`make RX3=… knobshim.so audioshim.so`.

`knobshim.so` is a set of objects — the front end, the rbp bridge, the LED and
meter modules, the sequencer, the maps — plus `shmstate.o`, which is the one
file that must not be linked into a consumer of those globals. `CTRL_OBJS` in
the Makefile is the list the build actually uses and documents what each entry
is for. The split is so a new controller map can be added without touching the
rbp-facing half — see [08 — Controls](../../docs/08-controls.md).

## The shims

| File | Role | Target-specific? |
|---|---|---|
| `fb_shim.c` + `tscfake.c` + `pointsrc.c` + `point_xform.c` + `touch_zone.c` + `fb_cursor.c` + `cursor_paint.c` + `menu_zone.c` + `menu_paint.c` + `menu_font.h` + `menu_draw.c` → `fbshim.so` | fb ioctl shim (1280×800 RGB565 logical fb, 60 fps pacing) + the fake `/dev/tsc2007_2-0048` rbp reads. `fb_shim` owns the interposed libc symbols; `tscfake` is rbp ABI; `pointsrc` finds an evdev pointer and `point_xform` maps its coordinates; `touch_zone` is the one part of rbp's screen the pointer path acts on by itself (the deck QUANTIZE boxes) and `rbp_key` sends the keycode for it. The two compositors are pure image work plus a driver each: `cursor_paint`/`fb_cursor` draw the mouse arrow (relative pointers only), `menu_paint`/`menu_font`/`menu_draw` the swipe-down top menu and its witness thread | discovery and geometry, via `POINT_*`; the menu's strip, panel and buttons are literals in `menu_zone.c` |
| `ctrlshim.c` + `ctrl_map.c` + `map_flx6.c` + `map_flx4.c` + `map_jp21.c` + `map_kbd.c` + `evdev_io.c` + `rbp_bridge.c` + `rbp_led.c` + `rbp_vu.c` + `midi_io.c` + `mididump.c` + `shimutil.c` → `knobshim.so` | control surface → rbp keycodes, plus panel LED and VU output. `midi_io` is the sequencer, `evdev_io` the non-MIDI source (`/dev/input/event*`), `rbp_bridge` everything that resolves into `rbp`, `ctrlshim` the front end, `map_*.c` the surfaces | `flx6` (the Pi's surface and default), `flx4` (compatibility), `jp21` (SC Live 4), and `kbd` (keyboard, no controller needed). FLX6 transport, pads, FX, jog/load displays, meters and Merge rings use the official list, working Mixxx references and measured codes; see the mapping inventory for hardware verification status. |
| `rbp_key.c` → linked into **both** shims | rbp's key path on its own — are we rbp, where is its `KeyManager`, `send_rx_key*()`. Both shims need it (`fbshim` for the pointer path's keycodes) and it is a leaf, so neither pulls in `rbp_bridge.o` and its meter hook twice — see [`rbp_key.h`](rbp_key.h) |
| `audioshim.c` + `s24pack.c` + `mirror_policy.c` + `master_policy.c` + `pace_policy.c` → `audioshim.so` | presents rbp's three 2-channel S24_LE 44.1 kHz streams as one real ALSA stream on a configurable card, channel map and format; the three policy modules are its master device chain, its HDMI mirror's decisions, and (with no card open) the playback deadline that keeps rbp at real time rather than a third of it | card/map/format, via `AUDIO_*` |
| `crashcatch.c` → `crashcatch.so` | SIGSEGV `pc`/`lr` → `/tmp/crash.log` | diagnostic |
| `netshim.c` + `netalias.c` → `netshim.so` | rbp's Pro DJ Link stack over an interface that is up: interposes `ioctl`/`if_nametoindex`/`system` and rewrites the interface *name* in whitelisted `eth0` requests to the alias, delegating everything else. `netalias.c` is the pure half (the whitelist and the selection policy) and is built `-fvisibility=hidden` so its `na_*` symbols never reach rbp's global scope; `test_netalias` (in `make test`) pins its rules, including that a setter is never retargeted. **Second in `RB_LD_PRELOAD`, before `fbshim.so`** — both define `ioctl` and the first one wins; see [18 — Pro DJ Link](../../docs/18-prodjlink.md) | interface selection via `NETALIAS_IFACE`, and off by default (`NETALIAS=0`). `NETALIAS_CONNECT` additionally arms a 250 ms poll for a request file (`NETALIAS_CONNECT_FILE`, default `/tmp/rb_link.req`) that makes it call `NetworkManager::operateConnectNetwork` once — the **out-of-band** bring-up. The in-band alternative is rbp's own path: `NetworkMonitor::timerCallback` (vtable slot +12, once a second) is gated on `ui::PcController::isUsbBConnected()` (`PcController+0x72`), and the word `connect` on its own `/tmp/udev_usb1` FIFO does raise that gate — **measured live, and it is not sufficient.** That timer reads the gate *through* `IUiObjManager::getPcController()`, which the superseded `rbp-nopc` build stubbed to `mov r0,#0`; the gate went to 1 and Link stayed down on both routes. Fixed 2026-10-07 (the getters now return NULL instead of faulting), but the in-band drill has not been re-run, so **neither route is proven yet.** Either way the substitution alone is not enough |
| `seqinject2.c` → `seqinject2` | static helper: inject MIDI into the shim's sequencer port | diagnostic |
| `netcheck.c` → `netcheck` | **must not be `-static`** — it is `netshim.so`'s `LD_PRELOAD` subject, so it exercises the constructor, the `RTLD_NEXT` chain and the name restore that `test_netalias` cannot reach. Run it twice, with and without the shim: the `eth0` line moving between the two runs is the whole result. `NETALIAS=1 NETALIAS_IFACE=lo LD_PRELOAD=…/netshim.so ./netcheck` in the chroot — see [18](../../docs/18-prodjlink.md) | diagnostic |
| `udplog.c` → `udplog` | static UDP listener for rbp's DebugLog | diagnostic |
| `test_point.c` → `test_point` | unit test for the record stream and the coordinate transform, including the stream a swallowed strip tap is replayed as — the burst, the reflection, and that the pair leaves the wire released | not deployed |
| `test_audio.c` → `test_audio` | unit test for the output byte layout: `s24pack()` per format, the extremes, the refusals | not deployed |
| `test_midi.c` → `test_midi` | unit test for the MIDI path: a recorded dump, replayed through the real JP21 map | not deployed |
| `test_flx4.c` → `test_flx4` | the same discipline against the real FLX4 map, over a **hand-written** fixture — so its assertions pin the map's own tables rather than the unit's behaviour | not deployed |
| `test_kbd.c` → `test_kbd` | unit test for the keyboard fallback: synthetic evdev triples through the real keyboard map | not deployed |
| `test_cursor.c` → `test_cursor` | unit test for the arrow's glyph and its compositing rule: idempotent paint, conditional restore, a third party's pixels surviving | not deployed |
| `test_cursor_dev.c` → `test_cursor_dev` | the driver seam for the arrow: the shim's geometry lie, the real ioctls, the thread's tick and its witness | not deployed |
| `test_evdev.c` → `test_evdev` | the non-MIDI reader: discovery, re-enumeration, and a hot-unplug that must not eat held keys | not deployed |
| `test_menu.c` → `test_menu` | the swipe-down menu: the gesture's clauses, the button geometry including the mirror, the baked font table and its ink band, the panel's paint/restore rules, and the damage witness's sample points being glyph-free | not deployed |
| `bake_menu_font.py` | **build-time only**, not compiled: rasterises the device's own `decker.ttf` (Decker Bold, in `extracted/`, gitignored) into the committed `menu_font.h` — the 8-bit coverage atlas and the per-glyph metrics the panel's labels are drawn from. `python3 scripts/shims/bake_menu_font.py [px]`; needs Python, Pillow and the ttf, none of which a **build** needs — a build reads the committed header and never opens a font file | not deployed |
| `gpioshim.c`, `tscshim.c`, `fbshim16.c` | earlier standalone implementations, kept for reference | not deployed |

`s24pack.c` is a separate object from `audioshim.c` for one reason: it is pure —
no ALSA, no allocation, no globals — so the part of the audio path that is
checkable without a card can be checked without a card. On this target it is
always in play: `AUDIO_DEV` is a `hw:` device, so nothing between the shim and the
card converts, and `AUDIO_FMT` (default `s24_3le`, which the FLX6 probe accepts)
is the format `s24pack()` produces. It is only bypassed when `AUDIO_DEV` names a
plug device, where the plug chain does the packing instead.

`mirror_policy.c` and `master_policy.c` are the same split applied to the two
decisions the shim cannot be trusted to get right by reading it: which device
candidates a configured device name yields, what one `snd_pcm_writei()` return
means, and when a device that has stopped carrying the stream may be retired —
`mirror_policy.c` for the HDMI mirror, `master_policy.c` for the master.
`audioshim.c` cannot be linked into a test at all, so anything left inside it is
only checkable by a drill on a Pi; both modules are pure in the same sense
`s24pack.c` is, which is what lets `test_audio` pin the verdicts on the host. The
master's chain is the one that matters most: it may name the card `AUDIO_DEV`
names and nothing else, because a fallback device is one whose channel count gets
latched into the stream→pair map without any index having been resolved against
it — see [09 — Audio](../../docs/09-audio.md).

See [08 — Controls](../../docs/08-controls.md) and
[09 — Audio](../../docs/09-audio.md).

## Preload order (matters)

```
LD_PRELOAD=/usr/lib/crashcatch.so:/usr/lib/netshim.so:/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so
```

`knobshim.so` before `audioshim.so`, because the controls shim *defines* the
globals the audio shim reads (`g_master_gain`) and the audio shim defines the ones
the controls shim reads back (`g_vu_peak`). These are data symbols, so a wrong
order is not a crash — it is a silent second copy that stays zero — which is why
each consumer verifies the contract in its constructor and aborts with the correct
line if it fails. See [`shmstate.h`](shmstate.h); `SHMSTATE_STRICT=0` downgrades
that to a warning for single-shim fixture runs.

Two positions are fixed for reasons that are *not* that contract, and both are
checked by `doctor.sh`:

* `crashcatch.so` is **first**, because its constructor is what arms the exit and
  fatal-signal handlers and resolves the real `exit` before any other preload can
  fault or exit.
* `netshim.so` is **second — before `fbshim.so` — and that is not a preference.**
  `fb_shim.c` also defines `ioctl`, and a process resolves a symbol to the *first*
  preloaded object that defines it. Put `netshim` after `fbshim` and the network
  rewrite silently does nothing while its other two hooks keep working; put it
  before `fbshim` *without* chaining and the framebuffer emulation never runs.
  `netshim` therefore resolves `dlsym(RTLD_NEXT, "ioctl")` and delegates, giving
  the chain **netshim → fbshim → `syscall(SYS_ioctl)`**. See
  [18 — Pro DJ Link](../../docs/18-prodjlink.md).

## Diagnostics

`KNOB_VERBOSE=1` logs every MIDI event + resulting keycode to
`/tmp/knobshim.log`, and likewise every evdev triple with the keycode it
produced. `LED_VERBOSE=1` / `LED_DUMP=1` log panel LED traffic, and
`VU_DEBUG=1` logs the meter values.

The audio shim always logs its resolved configuration — the device string it
opened, the negotiated channel count, the format, and the stream→pair map it
built — plus every fallback it took, to `/tmp/audioshim.log`. That file is the
first thing to read when there is no sound: it distinguishes "opened the wrong
card" from "opened the right card and got the pairs wrong" from "wrote
successfully and the device ignored it".
