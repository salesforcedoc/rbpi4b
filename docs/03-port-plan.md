# 03 — Port plan (reuse map + what changes)

Moving the port from the Denon SC Live 4 to a Raspberry Pi 4B replaces the
*device* and nothing else. This doc is the map of what is reused verbatim, what
had to change, and the order to build it. The plan of record, with the bring-up
table and the risks, is [13 — Raspberry Pi 4](13-raspberrypi4.md).

## 1. Reuse map

| Piece | Where it lives | On the Pi 4 |
|---|---|---|
| `rbp` patcher (`rbp_patch.py`) | [tools/patch-rbp](../tools/patch-rbp/) | **unchanged** — every patch's justification is the absence of Pioneer hardware, which a Pi also lacks |
| glibc-2.13 soft-float chroot | [scripts/build-chroot.sh](../scripts/build-chroot.sh) | **unchanged recipe**, new root: `/opt/rblive4/rbx3-run` |
| `tsc2007` protocol + `TouchCalib` | [scripts/shims](../scripts/shims/) | **unchanged** — it is `rbp`-facing ABI, not device code |
| `crashcatch.c`, `seqinject2.c`, `udplog.c` | [scripts/shims](../scripts/shims/) | unchanged |
| `gpioshim.c`, `tscshim.c`, `fbshim16.c` | [scripts/shims](../scripts/shims/) | reference only, out of the default build |
| DirectFB patch + build | [tools/build-directfb](../tools/build-directfb/) | **patch mechanism unchanged**, present path generalized (gated on measurement) |
| `usb-watch.sh` | [scripts/device](../scripts/device/) | media discovery generalized to any USB ancestor |
| `start-rb.sh` / `fix-dev.sh` | [scripts/device](../scripts/device/) | services and paths from `rb.conf` |
| `knobshim2.so` (controls) | [scripts/shims](../scripts/shims/) | **split into `knobshim.so`**; the map becomes a separate table |
| `audioshim.so` (audio) | [scripts/shims](../scripts/shims/) | **device, channel count and pair map negotiated, not hardcoded** |
| Root access | `freelive4` method (SC Live 4) | **not needed** — plain root on Pi OS Lite |

The three rows that say "unchanged" are the reason this port is a port and not
a rewrite; they are also the expensive parts. See
[Why it works](../README.md#why-it-works) in the top-level README.

## 2. What changed

### 2.1 Display — the present path, not just rotation

The SC Live 4's panel was a fixed 800×1280 portrait 32 bpp fb, so the patch
could commit to one transform. A Pi drives whatever monitor is attached, so the
driver has to *decide* what to do from the geometry and format it finds:
`off` (no conversion), `convert` (565→8888), `letterbox` (1:1, centred, bars),
`crop` (1:1, truncated), `scale` (uniform resample, centred, bars) and the old
`rotate`, selected by `RB_DFB_PRESENT`.

**As of this tree the generalization is installed**, not just written up. The
measurements it was gated on have been taken, and they close the geometry
question outright. The format needs no work (16 bpp RGB565 is `rbp`'s native
format), and after the `cmdline.txt` recipe the fb is **1280×800 with a stride
of 2560** — the same width, height and stride as `rbp`'s surface. The connector's
mode list offered nothing above 1280×720, but a `video=` mode is programmed
whether or not the list names it, and the panel took it: so `crop`, `scale`,
`letterbox` and `convert` are all dead **on this panel**, and `RB_DFB_PRESENT`
never needs to leave `off` here.

That last sentence is about the sink, not the code, and the distinction is the
one this port now turns on. A monitor that is not 1280×800 is a different
monitor, not a different build: `scale` is written, the driver **upgrades `off`
to it by itself** when the real fb disagrees with the shim's logical geometry
(under `off` the layer surface *is* the fb page, so a mismatch is a sheared image
on a larger fb and no UI at all on a smaller one — deterministic, not cosmetic),
and a software scaler needs a frame budget because it runs on rbp's `SCHED_FIFO
98` render thread. `display-watch.sh` handles the other half of the operator's
request — a monitor swapped for one of a different size mid-session — by
comparing the framebuffer's geometry against the one rbp was launched with and
restarting the unit when it changes. **The display half of that is no longer only
design: S10.1–S10.4 ran on 2026-09-26**, and a 1280×720, a 960×600, an 800×600 and
a 1920×1080 panel each came up with the whole UI, correctly placed, the last
without anyone selecting anything — at 1.1–3.4 ms of a 16.67 ms frame, which is
why the shipped budget stands. What those rows also found is a **downscale**
losing single-pixel rules and thin glyph strokes at 0.75× and 0.625× (S10.2), which
is not fixed and is a decision rather than a defect. **The hot-swap half and the
two remaining modes have not been run**: S10.0, S10.5–S10.10 in
[13](13-raspberrypi4.md#s10--the-display-drills) are still the rows that settle
those, and
[06](06-display.md#a-mismatch-selects-the-rung-by-itself) is the design.

What the target did need was **not** a transform but a page decision. The fb has
a **single page** (`yres_virtual == yres`, `smem_len == 2560 × 800`) while the
driver forced `DLBM_TRIPLE`, and the guard that would normally catch that keys on
`ypanstep == 0` — which this fb does not report. So `yres_virtual` tripled to
2400, `dfb_fbdev_test_mode()`'s `need_mem` came out at 6,144,000 against a
`smem_len` of 2,048,000, `DFB_LIMITEXCEEDED` was returned, and the primary region
test failed: no UI, with a framebuffer-memory shortfall in the log. The patch now
decides the buffer mode from the **page count**, giving a single `FRONTONLY`
buffer whose `need_mem` equals `smem_len` exactly.
[13 — Raspberry Pi 4](13-raspberrypi4.md) carries the diagnosis and the bring-up
step (**S2.2**) that confirms it on the unit.

What is *not* optional: the per-flip `fopen`/`fwrite` of the whole triple buffer
to `/tmp/rot_surface.dump`, and the per-pointer-event `/tmp/dfbdig*.log`
fopens, are **removed**. On the previous target they were a throughput bug; as a
general rule they are a build the user should never ship.

### 2.2 Pointing — there is no touchscreen

The SC Live 4 port read `/dev/input/event0` by name, because it always was the
ILI2117 panel. A monitor has no panel, so `fbshim-tsc.c` is split into
`fb_shim.c` (the fb ioctl lies, `/dev/mem` → `EACCES`, the pan pacer, the
`gpiodrv` stubs), `tscfake.c` (the fake `/dev/tsc2007_2-0048`, **byte-for-byte
unchanged**), `pointsrc.c` (evdev discovery by capability, re-scan on `ENODEV`)
and `point_xform.c` (per-axis `EVIOCGABS` scaling plus swap/invert), all built
into one `fbshim.so`.

Because a mouse is relative and `rbp` wants absolute, the shim also accumulates
`REL_*` into a virtual pointer and holds a short tap open for `rbp`'s debounce.
Details: [07 — Touch / pointing](07-touch.md).

### 2.3 Controls — the map becomes a table

`knobshim2.c` was one 2500-line file that contained the rbp ABI, the LED and VU
paths, and the SC Live 4's note/CC tables. On the Pi the tables are the part that
changes, so the file is now six: `ctrlshim.c` (dispatch), `rbp_bridge.c` (every
hardcoded `rbp` address, the key manager, the mixer-engine cue helpers,
`install_meter_hook()`), `rbp_led.c`, `rbp_vu.c`, `midi_io.c`, `shimutil.c` —
plus `ctrl_map.h` and a map per surface. A map asks for *meaning* through the
named functions in `rbp_bridge.h` (`send_rx_key()`, `send_rx_key_f()`, the
`me_*()` mixer-engine helpers, …), so the bridge never learns a note number and
a map never learns an `rbp` address.

The plan assumed DDJ-class controllers put toggles on **CCs with a ≥64
threshold** rather than on notes, which is why the map struct was expected to
grow explicit CC-as-button entries. Pioneer's published list for this unit says
otherwise: every CC on an FLX4 is continuous (a 14-bit position pair or a
relative encoder) and every toggle is a note, so `struct ctrl_map` never grew
that entry kind and `ctrl_map.h` is unchanged. The other device fact did hold,
and it removed the shim's hardcoded sequencer client id and `/dev/snd/midiC0D0`:
the controller is found by name — `QUERY_NEXT_CLIENT` / `QUERY_NEXT_PORT` with
`MIDI_IN_MATCH` / `MIDI_OUT_MATCH` — with rawmidi kept only as a runtime
fallback. **That discovery now runs against the unit**: the shim logs
`subscribed to 28:0 'DDJ-FLX4 MIDI 1' (match 'FLX4')`, and an `aseqdump` capture
taken alongside it (S5.1) is what turned the map's largest guess — the jog's
counts per revolution — into a measurement. What is still not verified against
the hardware is the map's *tables*: nothing has yet pressed a control and watched
rbp react, so every keycode in `map_flx4.c` rests on Pioneer's published list.
Those parts are marked in the map and listed in [15](15-flx4-midi.md). Details:
[08 — Controls](08-controls.md) and the FLX4 runbook
[15 — DDJ-FLX4 MIDI](15-flx4-midi.md).

### 2.4 Audio — negotiated instead of hardcoded

`hw:1,0` with 8 JP21 channels becomes `hw:CARD=DDJFLX4,DEV=0`, with the channel
count negotiated from the real card and the stream→pair assignment in
`RB_AUDIO_MAP`. `rbp`'s side of the contract is untouched: three 2-channel
S24_LE 44.1 kHz playback streams plus a dummy capture, opened in a fixed order.

The plan was `plughw:` here, on the reasoning that ALSA's plug chain would do the
S24_LE ↔ S24_3LE packing and the 44.1 → 48 kHz conversion for free. **That was
wrong, and it is worth keeping the reason:** the map names *hardware* channel
indices, so the shim has to learn the card's real channel count, and a plug device
reports what the plug layer will accept instead — 10000 channels against the card's
4. The shim therefore negotiated 8 logical channels onto a 4-channel card and every
write failed `-EINVAL`. Measured afterwards: the FLX4 takes **44100 natively** and
accepts `S24_3LE` directly, so the plug chain was not needed for either. The shim's
own `s24pack()` does the packing, `RB_AUDIO_FMT` selects it, and `plughw:` survives
only as the fallback candidate for a card that refuses our format.
Details: [09 — Audio](09-audio.md).

### 2.5 Launcher — no vendor OS to get out of the way

There is no `engine.service` on a Pi, so the launcher's job shrinks to: stop
whatever desktop session might hold the display (`RB_STOP_SERVICES`), mask
`udisks2` so it cannot grab the media stick, disable the console getty, prepare
the chroot, start `edb_streamd`, then `rbp`, then the USB watcher. It also
gained a loud failure if `/dev/snd/seq` is missing, because the symptom of that
otherwise is "the UI drew and no control ever worked".

Details: [11 — Runtime launcher](11-runtime-launcher.md). The service lists,
paths, audio device, MIDI match strings and display mode are all in
`scripts/device/rb.conf` — the single place a machine-specific value lives.

### 2.6 `rbp` — the `getPcController()` no-op patch

`rbp-audio` has a timing-dependent crash in `IUiObjManager::getPcController()`:
a `NetworkMonitor` timer derefs a NULL PC-controller pointer at `[NULL+0x9c]`
about a second after start, before fb0 opens. One extra patch makes the getter
return NULL:

```
0x31DF64  e30636b0 -> e3a00000   mov r0, #0
0x31DF68  e3403268 -> e12fff1e   bx  lr
```

Applied by [`scripts/patch-rbp-nopc.py`](../scripts/patch-rbp-nopc.py) — renamed
from `patch-rbp-sclive4.py`, because the name implied a Denon-specificity it
never had. The PC controller it stubs out is a Pioneer one, absent on every
target this project has run on.

## 3. Build order

1. **Firmware assets** — obtain the extracted XDJ-RX3 tree and `rbp-audio`
   ([04](04-firmware-assets.md)).
2. **Chroot** — `rbx3-run` assembled by `scripts/build-chroot.sh`.
   ([05](05-chroot.md))
3. **Display** — patched DirectFB fbdev; `fbdump` first, then the present mode
   you actually need. ([06](06-display.md), [13](13-raspberrypi4.md))
4. **Pointing** — evdev discovery + the tsc2007 protocol. ([07](07-touch.md))
5. **Audio** — device, channels and pair map. ([09](09-audio.md))
6. **Controls** — the keyboard fallback first (`EVDEV_MAP=kbd`, no hardware
   needed — and it is the default, so it needs no configuration either), then the
   FLX4 map: written from Pioneer's published MIDI list, and
   corrected from a dump off the hardware.
   ([08](08-controls.md), [15](15-flx4-midi.md))
7. **USB** — `usb-watch.sh` + DeviceSQL import of `export.pdb`. ([10](10-usb.md))
8. **Launcher** — `start-rb.sh`. ([11](11-runtime-launcher.md))

Steps 5 and 6 can be worked with nothing but a keyboard attached, which is
deliberate: it keeps a missing controller from blocking the rest of the port.
That is now the *architecture* rather than a workaround — `EVDEV_MAP` and
`MIDI_MAP` are independent selections, so the keyboard is live whether or not a
controller is, and `EVDEV_MAP=kbd` needs no controller, no MIDI and no
`/dev/snd/seq`.

## 4. Working commands

```sh
# Pi
ssh pi@<host>

# produce rbp-audio from a stock rbp (shared patch table), then the
# getPcController() no-op stage
#   python3 tools/patch-rbp/rbp_patch.py extracted/XDJRX3/pdj/rbp -o extracted/rbp-audio
#   python3 scripts/patch-rbp-nopc.py extracted/rbp-audio
#   python3 scripts/patch-rbp-depth.py <that output>   # 32 bpp, matching rb.conf

# build shims (host, or the repo's Docker image)
#   make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs"

# build DirectFB (host)
#   see tools/build-directfb/README.md

# assemble the chroot, deploy, launch
#   scripts/build-chroot.sh            # -> work/rbpi4b-pi4.tgz
#   ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rbpi4b-pi4.tgz'
#   ssh pi@<host> 'sudo sh /opt/rblive4/start-rb.sh'
```

The per-subsystem docs (05–12) hold the detailed steps; [13](13-raspberrypi4.md)
holds the order that makes each one verifiable.
