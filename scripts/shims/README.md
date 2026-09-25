# scripts/shims/

`LD_PRELOAD` libraries (and two static helpers) that adapt the SC Live 4's
hardware to what the XDJ-RX3 `rbp` binary expects. All original code (MIT).
None of them contain Pioneer code; they call into `rbp`'s exported/singleton
entry points and translate hardware.

## Build

Soft-float ARM, glibc-2.4-only, linked against the RX3 rootfs:

```sh
make RX3=/path/to/extracted/XDJRX3-rootfs
make RX3=/path/to/extracted/XDJRX3-rootfs check
```

`check` fails the build if a shim references `GLIBC_2.17+` or is hard-float.
The `compat/` dir (empty `libc_nonshared.a` / `libpthread_nonshared.a`) is
created automatically so `-lpthread` links.

Build a single target during development, e.g.
`make RX3=… knobshim2.so audioshim.so`.

## The shims

| File | Role | SC Live 4 |
|---|---|---|
| `fbshim-tsc.c` → `fbshim.so` | fb ioctl shim (1280×800 RGB565 logical fb, 60 fps pacing) + tsc2007 touch emulation from `/dev/input/event0` | used as-is |
| `knobshim2.c` → `knobshim.so` | SC Live 4 MIDI control surface → rbp keycodes, plus panel LED and VU output | **SC Live 4-specific** |
| `audioshim.c` → `audioshim.so` | presents the RX3 ALSA devices over the JP21 `hw:1,0` codec | **SC Live 4-specific** |
| `crashcatch.c` → `crashcatch.so` | SIGSEGV `pc`/`lr` → `/tmp/crash.log` | diagnostic |
| `seqinject2.c` → `seqinject2` | static helper: inject MIDI into the shim's sequencer port | diagnostic |
| `udplog.c` → `udplog` | static UDP listener for rbp's DebugLog → `/data/rbp-debug.log` | diagnostic |
| `gpioshim.c`, `tscshim.c`, `fbshim16.c` | earlier standalone implementations, kept for reference | not deployed |

See [08 — Controls](../../docs/08-controls.md) and
[09 — Audio](../../docs/09-audio.md).

## Preload order (matters)

```
LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so
```

`knobshim.so` before `audioshim.so` so the shared `g_speaker_gain` data symbol
resolves to the same address in both.

## Diagnostics

`KNOB_VERBOSE=1` logs every MIDI event + resulting keycode to
`/tmp/knobshim.log`. `LED_VERBOSE=1` / `LED_DUMP=1` log panel LED traffic, and
`VU_DEBUG=1` logs the meter values.
