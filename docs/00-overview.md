# 00 — Overview

rbpi4b runs the **Pioneer DJ XDJ-RX3 standalone rekordbox player** (`rbp`,
called `rb` internally) on a **Raspberry Pi 4B**. The XDJ-RX3 firmware builds
its player as **soft-float ARM32**; a Pi executes soft-float EABI ELF natively —
the same arrangement the Prime GO and SC Live 4 ports relied on. What that needs
is 32-bit *emulation* in the kernel (`32-bit EL0`/`CONFIG_COMPAT`), not a 32-bit
kernel, and Pi OS provides it: the measured unit runs an arm64 kernel on a 32-bit
image and reports `32-bit EL0 Support` ([13](13-raspberrypi4.md)).

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
│  │      ▲  ▲  ▲  ▲                                               ││
│  │      │  │  │  └── netshim.so    eth0-name introspection → wlan0││
│  │      │  │  └───── knobshim.so   DDJ-FLX4 MIDI → RX3 keycodes  ││
│  │      │  └──────── audioshim.so  JUCE/ALSA → FLX4 USB audio    ││
│  │      └─────────── fbshim.so     fb ioctl + evdev → tsc2007    ││
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
| CPU float ABI | soft-float ARM32 | hard-float kernel, armhf userland | soft-float chroot; the kernel runs soft-float ELF natively, and only needs 32-bit emulation to do it (its own bitness does not matter) |
| Display | 1280×800 landscape, RGB565 | `vc4drmfb` over HDMI, measured 1280×800 at 16 bpp RGB565 — `rbp`'s own surface | rebuilt DirectFB fbdev driver: the present path bridges any geometry/format difference, and on the measured panel the difference is **none** — what needed fixing there was the buffer mode, decided from the fb's page count. A monitor of *any other size* is the case the `scale` rung exists for, and the driver selects it by itself when the real fb disagrees with the logical one — **run on glass at 1280×720, 960×600, 800×600 and 1920×1080 (S10.1–S10.3, 2026-09-26), whole UI, 1.1–3.4 ms of a 16.67 ms frame, with the 1280×800 panel unchanged (S10.4)**; `display-watch.sh` restarts the player when a swap changes the geometry mid-session, and **that half is still unrun** ([06](06-display.md#a-mismatch-selects-the-rung-by-itself)) |
| Pointing | tsc2007 resistive via `/dev/tsc2007_2-0048` | **no touchscreen** — an evdev pointer | `fbshim.so` synthesises the tsc2007 protocol from a discovered evdev device |
| Controls | Pioneer front-panel MCUs (EUP/SUB) | DDJ-FLX4 over USB MIDI | `knobshim.so` maps MIDI → `sendKey()` |
| Audio | 3× discrete CS4344 DACs | the FLX4's 4-channel USB audio | `audioshim.so` maps rbp's streams onto the FLX4's output pairs |
| USB | 2 host ports + sub-MCU | USB-A host ports | `usb-watch.sh` + native DeviceSQL import |
| Music DB | internal EDB daemon | — | RX3 `edb_streamd` runs in the chroot |
| Networking | `eth0` with a Pro DJ Link peer on the LAN | `eth0` **down**, WiFi up | `netshim.so` rewrites the interface *name* in rbp's whitelisted `eth0` introspection — **built and host-verified, off by default, not deployed** ([18](18-prodjlink.md)) |

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

A **second** stick takes the same route a slot over: the first candidate in
`/sys/block` order that actually carries an export is USB 1, the next is USB 2,
onto `/opt/rblive4/media/usb2/sda1` → `/media/usb2/sda1` → `/tmp/udev_usb2`. rbp
holds one `ui::UsbStorageManager` per channel, so both are separate devices and
the band's USB STOP chooser can eject either one on its own channel.

**Loading + playing a track**

```
LOAD  → FLX4 MIDI → knobshim → sendKey(0x4311)
      → rbp loads track + ANLZ analysis → waveform
PLAY  → knobshim → sendKey(0x4101)
      → DjEngineIF::play → PlayEngine clocked by the ALSA callback
      → audioshim packs S24_LE periods into S24_3LE for hw:CARD=DDJFLX4,DEV=0
      → master (pair 1) + headphones (pair 2) on the FLX4
```

The note numbers above are what the FLX4 map sends. The map is written and
fixture-tested, and the unit's own messages have been inventoried with
`aseqdump`, but the tables themselves still come from Pioneer's published MIDI
list: no control has yet been pressed and watched through to rbp. See
[15 — DDJ-FLX4 MIDI](15-flx4-midi.md).

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
* **Use the 32-bit image — and do not be alarmed if the kernel is 64-bit.** The
  shims' 32-bit-ARM-only constraints (32-bit `smem_start`/`time_t`, `SYS_mmap2`,
  `uc_mcontext.arm_*`, ARM32 machine-code patching) are properties of the
  *chroot's* binaries, which are 32-bit whatever the host runs — there is no
  64-bit `rbp`. So what the kernel must provide is 32-bit emulation, not a 32-bit
  kernel, and current Pi OS 32-bit images already ship the arm64 kernel.
  `install.sh` prints both and then tests it for real by executing a 32-bit
  binary in the chroot.
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
