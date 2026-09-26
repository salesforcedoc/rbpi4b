# scripts/shims/

`LD_PRELOAD` libraries (and two static helpers) that adapt the Raspberry Pi 4's
hardware to what the XDJ-RX3 `rbp` binary expects. All original code (MIT).
None of them contain Pioneer code; they call into `rbp`'s exported/singleton
entry points and translate hardware.

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
| `fb_shim.c` + `tscfake.c` + `pointsrc.c` + `point_xform.c` → `fbshim.so` | fb ioctl shim (1280×800 RGB565 logical fb, 60 fps pacing) + the fake `/dev/tsc2007_2-0048` rbp reads. `fb_shim` owns the interposed libc symbols; `tscfake` is rbp ABI; `pointsrc` finds an evdev pointer and `point_xform` maps its coordinates | discovery and geometry, via `POINT_*` |
| `ctrlshim.c` + `ctrl_map.c` + `map_flx4.c` + `map_jp21.c` + `map_kbd.c` + `evdev_io.c` + `rbp_bridge.c` + `rbp_led.c` + `rbp_vu.c` + `midi_io.c` + `mididump.c` + `shimutil.c` → `knobshim.so` | control surface → rbp keycodes, plus panel LED and VU output. `midi_io` is the sequencer, `evdev_io` the non-MIDI source (`/dev/input/event*`), `rbp_bridge` everything that resolves into `rbp`, `ctrlshim` the front end, `map_*.c` the surfaces | `flx4` (the Pi's own surface, the default), `jp21` (SC Live 4) and `kbd` (keyboard, no controller needed). **`flx4`'s note/CC tables are written from Pioneer's published MIDI list and are unverified** — see [`map_flx4.c`](map_flx4.c)'s provenance block and [15 — DDJ-FLX4 MIDI](../../docs/15-flx4-midi.md) |
| `audioshim.c` + `s24pack.c` → `audioshim.so` | presents rbp's three 2-channel S24_LE 44.1 kHz streams as one real ALSA stream on a configurable card, channel map and format | card/map/format, via `AUDIO_*` |
| `crashcatch.c` → `crashcatch.so` | SIGSEGV `pc`/`lr` → `/tmp/crash.log` | diagnostic |
| `seqinject2.c` → `seqinject2` | static helper: inject MIDI into the shim's sequencer port | diagnostic |
| `udplog.c` → `udplog` | static UDP listener for rbp's DebugLog | diagnostic |
| `test_point.c` → `test_point` | unit test for the record stream and the coordinate transform | not deployed |
| `test_audio.c` → `test_audio` | unit test for the output byte layout: `s24pack()` per format, the extremes, the refusals | not deployed |
| `test_midi.c` → `test_midi` | unit test for the MIDI path: a recorded dump, replayed through the real JP21 map | not deployed |
| `test_flx4.c` → `test_flx4` | the same discipline against the real FLX4 map, over a **hand-written** fixture — so its assertions pin the map's own tables rather than the unit's behaviour | not deployed |
| `test_kbd.c` → `test_kbd` | unit test for the keyboard fallback: synthetic evdev triples through the real keyboard map | not deployed |
| `gpioshim.c`, `tscshim.c`, `fbshim16.c` | earlier standalone implementations, kept for reference | not deployed |

`s24pack.c` is a separate object from `audioshim.c` for one reason: it is pure —
no ALSA, no allocation, no globals — so the part of the audio path that is
checkable without a card can be checked without a card. On this target it is
always in play: `AUDIO_DEV` is a `hw:` device, so nothing between the shim and the
card converts, and `AUDIO_FMT` (default `s24_3le`, which is what the FLX4 accepts)
is the format `s24pack()` produces. It is only bypassed when `AUDIO_DEV` names a
plug device, where the plug chain does the packing instead.

See [08 — Controls](../../docs/08-controls.md) and
[09 — Audio](../../docs/09-audio.md).

## Preload order (matters)

```
LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so
```

`knobshim.so` before `audioshim.so`, because the controls shim *defines* the
globals the audio shim reads (`g_master_gain`) and the audio shim defines the ones
the controls shim reads back (`g_vu_peak`). These are data symbols, so a wrong
order is not a crash — it is a silent second copy that stays zero — which is why
each consumer verifies the contract in its constructor and aborts with the correct
line if it fails. See [`shmstate.h`](shmstate.h); `SHMSTATE_STRICT=0` downgrades
that to a warning for single-shim fixture runs.

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
