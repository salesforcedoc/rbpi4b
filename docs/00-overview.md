# 00 — Overview

rblive4 runs the **Pioneer DJ XDJ-RX3 standalone rekordbox player** (`rbp`,
called `rb` internally) on a **Denon DJ SC Live 4**. Both machines are ARMv7
Linux devices, and the XDJ-RX3 firmware builds its player as **soft-float
ARM32** — which the SC Live 4's hard-float RK3288 kernel executes natively.

There is no emulation. The real `rbp` binary from the XDJ-RX3 firmware runs
directly, with a set of thin shims translating the SC Live 4's hardware into
what `rbp` expects. The SC Live 4 is the Prime GO's sibling: same SoC, same
touch controller, same panel geometry, same control-surface transport, same
Buildroot 2023.02.11 base.

## The pieces

```
┌──────────────────────────────────────────────────────────────────────┐
│                         Denon SC Live 4                             │
│  Rockchip RK3288 · 800×1280 portrait panel · ILI2117 touch          │
│  JP21 8-ch audio codec · USB-A host · MIDI control surface          │
│                                                                     │
│  ┌──────────────────────── /data/rbx3-run (chroot) ────────────────┐│
│  │  soft-float glibc 2.13 + RX3 libs + DirectFB 1.4              ││
│  │                                                               ││
│  │   rbp-audio  ──  the XDJ-RX3 rekordbox player                 ││
│  │      ▲  ▲  ▲                                                  ││
│  │      │  │  └── knobshim2.so   SC Live 4 MIDI → RX3 keycodes   ││
│  │      │  └───── audioshim.so   JUCE/ALSA → hw:1,0 (8ch)        ││
│  │      └──────── fbshim-tsc.so  fb ioctl + touch translation    ││
│  │                                                               ││
│  │   libdirectfb_fbdev.so (rebuilt) ── rotation + RGB565→RGB32   ││
│  └────────────────────────────────────────────────────────────────┘│
│        ▲              ▲                ▲               ▲           │
│     /dev/fb0     /dev/input/event0   MIDI 16:0     /tmp/udev_usb1  │
│   (800x1280x32)   (ILI2117 evdev)   control surface   (hotplug)    │
└──────────────────────────────────────────────────────────────────────┘
```

## Why each piece is needed

| Mismatch | XDJ-RX3 has | SC Live 4 has | Solution |
|---|---|---|---|
| CPU float ABI | soft-float ARM32 | hard-float ARMv7 kernel | soft-float chroot; kernel runs soft-float ELF fine |
| Display | 1280×800 landscape, RGB565 | 800×1280 portrait, RGB32, triple-buffered DRM fb | rebuilt DirectFB fbdev driver rotates + converts |
| Touchscreen | tsc2007 resistive via `/dev/tsc2007_2-0048` | ILI2117 capacitive evdev | `fbshim-tsc.so` synthesises the tsc2007 protocol |
| Controls | Pioneer front-panel MCUs (EUP/SUB) | ALSA MIDI "Control Surface" | `knobshim2.so` maps MIDI → `sendKey()` |
| Audio | 3× discrete CS4344 DACs | single JP21 **8-channel** codec | `audioshim.so` maps rbp's channels onto `hw:1,0` |
| USB | 2 host ports + sub-MCU | USB-A host port(s) | `usb-watch.sh` + native DeviceSQL import |
| Music DB | internal EDB daemon | — | RX3 `edb_streamd` runs in the chroot |

## Data flow for a typical action

**Browsing a USB stick**

```
stick → kernel usb-storage → usb-watch.sh mounts /media/usb1/sda1
      → bind-mount into chroot
      → write "mount /media/usb1/sda1" to /tmp/udev_usb1
      → rbp UsbMountManager → DbProxy → DbIF::mount('C')
      → DeviceSQL scans export.pdb → detect flag = 2
      → source list shows the drive, categories populate natively
```

**Loading + playing a track**

```
LOAD button → SC Live 4 MIDI note → knobshim2 → sendKey(0x4311)
      → rbp loads track + ANLZ analysis → waveform
PLAY button → knobshim2 → sendKey(0x4101)
      → DjEngineIF::play → PlayEngine clocked by the ALSA callback
      → audioshim feeds S24_LE periods to hw:1,0 @ 44.1 kHz
      → master (ch 0/1) + headphones (ch 4/5) + monitors (ch 6/7) on the JP21 codec
```

## Repository map

See the top-level [README](../README.md). The device facts are in
[01 — Device survey](01-device-survey.md), the comparison in
[02 — Hardware](02-hardware.md), and the reuse/change plan in
[03 — Port plan](03-port-plan.md).

## Prerequisites

* A Denon SC Live 4 with **root SSH** enabled (`freelive4` method), see
  [01 — Device survey](01-device-survey.md).
* A Linux workstation with `arm-linux-gnueabi-gcc` (soft-float) and Docker.
* Extracted XDJ-RX3 v1.20 assets (rootfs, gui, `rbp-audio`) — see
  [04 — Firmware assets](04-firmware-assets.md).
* ~100 MB free on the SC Live 4's `/data` partition (5.6 GB available).

## Caveats

* The SC Live 4 boots with `panic_on_oops=1`. Use the tuned, stable DirectFB
  stack described in [06 — Display](06-display.md); a wrong display path can
  panic and reboot the unit. See [12 — Troubleshooting](12-troubleshooting.md).
* The SC Live 4 is a live DJ unit. **Back up `/data`** (the library + any
  Engine OS state) before experimenting, and don't leave the rbp chroot
  autostarting if you need the stock Engine OS for a gig.
