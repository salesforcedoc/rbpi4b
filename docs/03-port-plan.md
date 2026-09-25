# 03 — Port plan (reuse map + what changes)

The SC Live 4 port applies the Prime GO approach to a sibling device. This doc
is the map of what is reused, what had to change, and the order to build it.

## 1. Reuse map

| Piece | Where it lives | On SC Live 4 |
|---|---|---|
| `rbp` patcher (`rbp_patch.py`) | [tools/patch-rbp](../tools/patch-rbp/) | unchanged |
| DirectFB patch + build | [tools/build-directfb](../tools/build-directfb/) | unchanged (same 800×1280 portrait) |
| glibc-2.13 soft-float chroot | [scripts/build-chroot.sh](../scripts/build-chroot.sh) | `/data/rbx3-run` |
| `fbshim-tsc.so` (fb + touch) | [scripts/shims](../scripts/shims/) | unchanged (same ILI2117/rockchipdrmfb) |
| `tscshim.c`, `gpioshim.c`, `crashcatch.c`, `seqinject2.c`, `udplog.c` | [scripts/shims](../scripts/shims/) | unchanged |
| `usb-watch.sh` | [scripts/device](../scripts/device/) | adapted paths |
| `start-rb.sh` / `fix-dev.sh` | [scripts/device](../scripts/device/) | adapted |
| `knobshim2.so` (controls) | [scripts/shims](../scripts/shims/) | **new MIDI map + LED/VU output for the SC Live 4** |
| `audioshim.so` (audio) | [scripts/shims](../scripts/shims/) | **8-channel JP21 codec** |
| Root SSH | `freelive4` method | already on device |

## 2. What changed

### 2.1 Audio — JP21 8-channel codec (`audioshim.c`)

The Prime GO's JP11 codec is 4-channel; the SC Live 4's JP21 codec exposes
**8 playback channels** (S32_LE @ 44.1 kHz). The shim opens `hw:1,0` with
8 channels and places rbp's streams into the JP21 slots (master ch 0/1 +
monitors ch 6/7, headphones ch 4/5, booth ch 2/3). Details:
[09 — Audio](09-audio.md).

### 2.2 Controls — SC Live 4 MIDI map (`knobshim2.c`)

Same transport (ALSA MIDI "Control Surface", seq client 16, raw
`/dev/snd/midiC0D0`), but the SC Live 4 is a 4-channel controller: the MIDI
note/CC numbers for PLAY/CUE/LOAD, the pads, the jog wheels, and the channel
faders/knobs differ from the Prime GO. `knobshim2.c` maps each control to the
right rbp keycode and also mirrors rbp's LED and VU state back to the panel.
Details: [08 — Controls](08-controls.md).

### 2.3 Launcher — Engine OS coexistence (`start-rb.sh`)

`engine.service` owns the display/audio/MIDI on the SC Live 4, so the launcher:

1. Stops `engine.service` (and `edisksd.service`).
2. Runs `fix-dev.sh` (bind mounts + device stubs) → chroot → `rbp`.
3. Starts the USB watcher once rbp is ready.

Details: [11 — Runtime launcher](11-runtime-launcher.md).

### 2.4 `rbp` — SC Live 4 crash fix (`patch-rbp-sclive4.py`)

`rbp-audio` hits a timing-dependent crash on the SC Live 4 in
`IUiObjManager::getPcController()`: a `NetworkMonitor` timer derefs a NULL
PC-controller pointer at `[NULL+0x9c]` ~1 s after start, before fb0 opens.
One extra patch makes the getter return NULL:

```
0x31DF64  e30636b0 -> e3a00000   mov r0, #0
0x31DF68  e3403268 -> e12fff1e   bx  lr
```

Applied by [`scripts/patch-rbp-sclive4.py`](../scripts/patch-rbp-sclive4.py).

## 3. Build order

1. **Firmware assets** — obtain the extracted XDJ-RX3 tree and `rbp-audio`
   ([04](04-firmware-assets.md)).
2. **Chroot** — `/data/rbx3-run` assembled by `scripts/build-chroot.sh`.
   ([05](05-chroot.md))
3. **Display** — patched DirectFB fbdev, rotated 800×1280. ([06](06-display.md))
4. **Touch** — ILI2117 → tsc2007 + identity `TouchCalib`. ([07](07-touch.md))
5. **Controls** — JP21 MIDI map + panel LEDs/VU. ([08](08-controls.md))
6. **Audio** — 8-ch JP21 routing. ([09](09-audio.md))
7. **USB** — `usb-watch.sh` + DeviceSQL import of `export.pdb`. ([10](10-usb.md))
8. **Launcher** — `start-rb.sh`. ([11](11-runtime-launcher.md))

## 4. Working commands

```sh
# device
ssh root@sclive4            # your device's root password

# produce rbp-audio from a stock rbp (shared patch table)
#   python3 tools/patch-rbp/rbp_patch.py extracted/XDJRX3/pdj/rbp -o extracted/rbp-audio

# build shims (host)
#   make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs"

# build DirectFB (host)
#   see tools/build-directfb/README.md

# assemble + deploy the chroot
#   scripts/build-chroot.sh
```

The per-subsystem docs (05–12) hold the detailed, device-specific steps.
