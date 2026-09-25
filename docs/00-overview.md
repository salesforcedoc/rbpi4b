# 00 — Overview

rblive4 runs the **Pioneer DJ XDJ-RX3 standalone rekordbox player** (`rbp`,
called `rb` internally) on a **Raspberry Pi 4B**. The XDJ-RX3 firmware builds
its player as **soft-float ARM32**; Pi OS 32-bit is armv7l with a hard-float
kernel, which executes soft-float EABI ELF natively — the same arrangement the
Prime GO and SC Live 4 ports relied on, and the reason a 64-bit image is not an
option.

There is no emulation. The real `rbp` binary from the XDJ-RX3 firmware runs
directly, with a set of thin shims translating the Pi's hardware — HDMI
framebuffer, USB MIDI controller, USB audio — into what `rbp` expects.

The target document is [13 — Raspberry Pi 4](13-raspberrypi4.md); it carries the
image, the `cmdline.txt` recipe and the bring-up order. This chapter is the map
of the whole thing.

## The pieces

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Raspberry Pi 4B                              │
│  BCM2711 · HDMI (vc4drmfb) · USB-A host · no touchscreen            │
│  DDJ-FLX4 on USB: MIDI control surface + 4-channel USB audio        │
│                                                                     │
│  ┌────────────── /opt/rblive4/rbx3-run (chroot) ────────────────────┐│
│  │  soft-float glibc 2.13 + RX3 libs + DirectFB 1.4              ││
│  │                                                               ││
│  │   rbp-audio  ──  the XDJ-RX3 rekordbox player                 ││
│  │      ▲  ▲  ▲                                                  ││
│  │      │  │  └── knobshim.so    DDJ-FLX4 MIDI → RX3 keycodes    ││
│  │      │  └───── audioshim.so   JUCE/ALSA → FLX4 USB audio      ││
│  │      └──────── fbshim.so      fb ioctl + evdev → tsc2007      ││
│  │                                                               ││
│  │   libdirectfb_fbdev.so (rebuilt) ── the present path          ││
│  └────────────────────────────────────────────────────────────────┘│
│        ▲              ▲                ▲               ▲           │
│     /dev/fb0    /dev/input/eventN   MIDI "DDJ-FLX4"  /tmp/udev_usb1│
│   (HDMI, 16bpp)   (pointer evdev)     seq port        (hotplug)    │
└──────────────────────────────────────────────────────────────────────┘
```

## Why each piece is needed

| Mismatch | XDJ-RX3 has | Pi 4 has | Solution |
|---|---|---|---|
| CPU float ABI | soft-float ARM32 | hard-float armv7l kernel | soft-float chroot; the kernel runs soft-float ELF natively |
| Display | 1280×800 landscape, RGB565 | `vc4drmfb` over HDMI, 16 bpp RGB565 expected | rebuilt DirectFB fbdev driver; the present path bridges any geometry/format difference |
| Pointing | tsc2007 resistive via `/dev/tsc2007_2-0048` | **no touchscreen** — an evdev pointer | `fbshim.so` synthesises the tsc2007 protocol from a discovered evdev device |
| Controls | Pioneer front-panel MCUs (EUP/SUB) | DDJ-FLX4 over USB MIDI | `knobshim.so` maps MIDI → `sendKey()` |
| Audio | 3× discrete CS4344 DACs | the FLX4's 4-channel USB audio | `audioshim.so` maps rbp's streams onto the FLX4's output pairs |
| USB | 2 host ports + sub-MCU | USB-A host ports | `usb-watch.sh` + native DeviceSQL import |
| Music DB | internal EDB daemon | — | RX3 `edb_streamd` runs in the chroot |

Every row above is a *device* difference. Nothing in the list touches the
binary, which is why the `rbp` patch table and the shims' hardcoded `rbp`
addresses survive the move unchanged — see
[03 — Port plan](03-port-plan.md).

## Data flow for a typical action

**Browsing a USB stick**

```
stick → kernel usb-storage → usb-watch.sh mounts /opt/rblive4/media/usb1/sda1
      → bind-mount into chroot at /media/usb1/sda1
      → write "mount /media/usb1/sda1" to /tmp/udev_usb1
      → rbp UsbMountManager → DbProxy → DbIF::mount('C')
      → DeviceSQL scans export.pdb → detect flag = 2
      → source list shows the drive, categories populate natively
```

**Loading + playing a track**

```
LOAD  → FLX4 MIDI → knobshim → sendKey(0x4311)
      → rbp loads track + ANLZ analysis → waveform
PLAY  → knobshim → sendKey(0x4101)
      → DjEngineIF::play → PlayEngine clocked by the ALSA callback
      → audioshim feeds S24_LE periods to plughw:CARD=DDJFLX4,DEV=0
      → master (pair 1) + headphones (pair 2) on the FLX4
```

The note numbers above are what the FLX4 map sends, and the FLX4 side of the
first hop is the one part of this diagram nobody has measured yet: the map is
written and fixture-tested, but its tables come from Pioneer's published MIDI
list rather than from the unit — see [15 — DDJ-FLX4 MIDI](15-flx4-midi.md).

## Repository map

See the top-level [README](../README.md). The device facts for the previous
target are in [01 — Device survey](01-device-survey.md), the comparison of all
four machines in [02 — Hardware](02-hardware.md), and the reuse/change plan in
[03 — Port plan](03-port-plan.md).

## Prerequisites

* A Raspberry Pi 4B running **Pi OS Lite 32-bit**, with root and network.
* A DDJ-FLX4 (or, for bring-up, nothing at all — display, pointing and audio can
  all be tested before a controller is plugged in).
* A workstation with `arm-linux-gnueabi-gcc` (soft-float) and, for the DirectFB
  build, Docker or an equivalent ARM toolchain.
* Extracted XDJ-RX3 v1.20 assets (rootfs, gui, `rbp-audio`) — see
  [04 — Firmware assets](04-firmware-assets.md).
* An SD card with room for the chroot — a few hundred MB extracted, which is
  nothing on an 8 GB card but is not nothing on a 4 GB one.

## Caveats

* **The tree is mid-port.** Where a number is an expectation rather than a
  measurement, the document says so; [docs/README](README.md) explains the
  convention, and [13](13-raspberrypi4.md)'s bring-up table is the honest
  current state.
* **Use the 32-bit image.** 64-bit Pi OS breaks every constraint the shims
  depend on (32-bit `smem_start`/`time_t`, `SYS_mmap2`, `uc_mcontext.arm_*`,
  ARM32 machine-code patching). The chroot may still run under a 64-bit kernel
  with 32-bit emulation, but that is untested and not supported.
* **The HDMI mode comes from the kernel command line**, not `config.txt`, and
  the kernel console must be moved off the framebuffer
  ([13](13-raspberrypi4.md#the-hdmi-mode)). Getting this wrong looks like a
  display bug and is not one.
* **`vc4` does not block on vblank.** Its `drm_fbdev` returns from
  `FBIOPAN_DISPLAY` immediately, so the pacer in the fb shim is load-bearing,
  not a nicety — without it `rbp`'s RT-priority render thread spins a core.
* This is a DJ player on a general-purpose computer. It has no vendor OS to fall
  back to, and no panel MCU: the shims own the device nodes they fake, and
  `fix-dev.sh` has to recreate them after every reboot.
