# 01 — Device survey (live)

Raw, reproducible survey of the **Denon DJ SC Live 4** this project targets.
Everything below was captured over root SSH on the running device.

* Hostname: `sclive4` (example; use your device's hostname)
* Access: `ssh root@sclive4` (root password required; persistent root SSH from
  the `freelive4` `/data` overlay + `/etc/ld.so.preload` method).

## 1. OS & CPU

```
NAME=Buildroot
VERSION=2023.02.11-5-g140647e9b3
ID=buildroot
VERSION_ID=2023.02.11
PRETTY_NAME="Buildroot 2023.02.11"
```

```
Linux sclive4 6.1.111-inmusic-2024-09-19-rt41 #1 SMP PREEMPT_RT Tue Mar 25 ... armv7l GNU/Linux
```

* Kernel: `6.1.111-inmusic-2024-09-19-rt41`, **PREEMPT_RT**, `armv7l`.
* CPU: Rockchip **RK3288** (ARMv7, Cortex-A17). `model name: ARMv7 Processor rev 1 (v7l)`.
* Device tree:
  * model: `DENON DJ SCX-4` (internal name; the retail product is the SC Live 4)
  * compatible: `inmusic,jp21` `inmusic,az05` `rockchip,rk3288`

> This is **Engine OS 4.3.x** (Buildroot) — the *same* base OS and kernel
> family as the Prime GO, not the Yocto "scarthgap" 5.0.4 rootfs documented in
> the `freelive4` notes. All of PrimeBox's assumptions hold.

## 2. Storage & partitions

```
# cat /proc/partitions
mmcblk0          7634944
 mmcblk0p1    480   mmcblk0p2    480   mmcblk0p3    512   mmcblk0p4    512
 mmcblk0p5   4096   mmcblk0p6   4096   mmcblk0p7   4096   mmcblk0p8   4096
 mmcblk0p9  16384
 mmcblk0p10 1048576          # rootfs (dm-verity)
 mmcblk0p11 6551023          # /data (~6.2 GB, plain ext4)
```

```
# mount (relevant lines)
/dev/dm-0 on / type ext4 (ro,relatime)                        # rootfs, dm-verity
/dev/dm-0 on /usr/lib/libmali.so.14.0 type ext4 (ro,relatime) # Mali GPU lib bind
/dev/mmcblk0p11 on /data type ext4 (rw,relatime)
/dev/mmcblk0p11 on /media/az01-internal type ext4 (rw,relatime)
overlay on /etc ... upperdir=/data/system/etc/overlay ...
overlay on /var ... upperdir=/data/system/var/overlay ...
```

```
# df -h
/dev/root            466.3M  386.4M   51.0M  88% /
/dev/mmcblk0p11        5.9G    2.3M    5.6G   0% /data
```

* Root is **read-only** through dm-verity (466 MB, ~51 MB free). Don't modify it.
* `/etc` and `/var` are **overlays** whose upper layers live on `/data`.
* `/data` is a plain ext4 partition with **5.6 GB free** — no disk budget
  problem (the Prime GO had ~50 MB; the rbp chroot is ~60 MB).

## 3. Display

```
# /dev/fb0
rockchipdrmfb
virtual_size: 800,3840      # 3 pages of 800×1280 (triple buffered)
bits_per_pixel: 32
stride: 3200
rotate: 0

# /sys/class/drm/card0-DSI-1/
status: connected
modes: 800x1280
```

* Panel: DSI, **800×1280 portrait**, RGB32, triple-buffered DRM framebuffer.
* Identical geometry to the Prime GO (which PrimeBox's DirectFB patch already
  handles: rotate + RGB565→RGB32).
* GPU: Mali (`/usr/lib/libmali.so.14.0` is bind-mounted from the rootfs).

## 4. Touch

```
/dev/input/event0   ILI2117 Touchscreen   (Bus=0018, ABS)
/dev/input/event1   gpio-keys (kbd)        (power / hardware buttons)
```

* **ILI2117** capacitive touch, exactly the Prime GO controller. PrimeBox's
  `fbshim-tsc.so` (evdev → tsc2007 protocol) applies unchanged.

## 5. Controls (MIDI control surface)

```
card 0: "Surface"  = MIDI UART "Control Surface"
  /dev/snd/midiC0D0            (raw MIDI)
  seq client 16: "Control Surface" [Kernel]
    Connecting To: 129:0
    Connected From: 130:1[r:0]
```

* The buttons / pads / knobs / faders / jog wheels arrive as **ALSA MIDI**
  on the "Control Surface" (raw MIDI `midiC0D0`, seq client 16) — the same
  transport PrimeBox's `knobshim2.so` consumes on the Prime GO.
* Only the **MIDI → keycode mapping** differs (the SC Live 4 is a 4-channel
  controller with a different deck layout). See
  [08 — Controls](08-controls.md).
* `gpio-keys` (event1) is the power/encoder level, not the performance surface.

## 6. Audio

```
card 1: "JP21"  = JP21 PCM (inmusic,jp21-audio-codec-0)

# /proc/asound/card1/pcm0p/sub0/hw_params (when open)
access: RW_INTERLEAVED, format: S32_LE, channels: 8, rate: 44100,
period_size: 512, buffer_size: 1024

# capture
access: RW_INTERLEAVED, format: S32_LE, channels: 2, rate: 44100
```

* Codec: **JP21** (`hw:1,0`), **8 playback channels** (S32_LE, 44.1 kHz) +
  2 capture channels.
* This is the one real delta vs the Prime GO (JP11, 4 channels). PrimeBox's
  `audioshim.so` maps rbp's 4 channels (master L/R + headphones L/R) onto a
  4-ch codec; on the SC Live 4 it must drive an **8-ch** device instead. The
  likely layout is master L/R + booth L/R + headphones L/R (+ spare) — to be
  confirmed in [09 — Audio](09-audio.md).
* ALSA tools present: `amixer`, `aplay`, `arecord`, `alsamixer`, `alsactl`.

## 7. USB

```
usb1: DWC OTG Controller
usb2: EHCI Host Controller
usb3: Generic Platform OHCI controller
```

* USB-A host port(s) via EHCI/OHCI (no devices were plugged during the survey).
* A `az01-usbsata-fixer.service` (JMicron USB-SATA bridge) exists — the SC
  Live 4 firmware also talks to a USB-SATA bridge; relevant when a drive is
  attached.

## 8. Services (running)

```
az01-data-mkfs.service        exited   Create /data filesystem if necessary
az01-libmali-setup.service    exited   Mali GPU driver setup
az01-machine-id.service       exited   Fix transient machine-id
az01-power-button.service     running  AZ01 power button emergency shutdown
az01-script-runner.service    running  AZ01 script runner (UDP 127.0.0.1:8080 root exec)
az01-setup-hostname.service   exited   Hostname setup
az01-usbsata-fixer.service    running  AZ01 JMicron USB-SATA bridge fixer
engine.service                running  Engine OS (the stock DJ app)
```

* `engine.service` is the stock Engine OS. The rbp launcher must **not**
  fight it: either stop it before starting rbp, or run rbp alongside (they
  both want the same display/audio/MIDI, so the practical approach is to stop
  Engine and take over — same trade-off as PrimeBox). See
  [11 — Runtime launcher](11-runtime-launcher.md).

## 9. Root access (already in place)

```
# ps: sshd
sshd: /usr/sbin/sshd -f /data/ssh/sshd_config [listener]

# /etc/ld.so.preload
/data/ssh/freelive4.so
```

* Persistent root SSH is provided by the `freelive4` payload:
  `/data/ssh/freelive4.so` in `/etc/ld.so.preload` starts
  `sshd -f /data/ssh/sshd_config` as root at boot (password + key auth).

## 10. Tools on the device

Present: `amixer`, `aplay`, `arecord`, `alsamixer`, `alsactl`, `ssh`, `scp`,
`busybox`.

Not present: `gcc`, `make`, `python3`, `strace`, `gdbserver`. **Cross-build on
the workstation** (as PrimeBox does) and `scp` the payload over.

## 11. Bottom line

| What rbp needs | Prime GO | SC Live 4 (live) | Delta |
|---|---|---|---|
| SoC / kernel | RK3288, 6.1.111-inmusic | RK3288, 6.1.111-inmusic | none |
| Base OS | Buildroot 2023.02.11, systemd | Buildroot 2023.02.11, systemd | none |
| Display | 800×1280 portrait, RGB32 DRM fb | 800×1280 portrait, RGB32 DRM fb | none |
| Touch | ILI2117 evdev | ILI2117 evdev | none |
| Controls | MIDI "Control Surface" | MIDI "Control Surface" | mapping only |
| Audio | JP11 codec, 4 ch | JP21 codec, **8 ch** | **audioshim channel count** |
| USB | 1× USB-A | USB-A host | none |
| `/data` | ~50 MB free | **5.6 GB free** | easier |
| Root SSH | needed setup | already done | none |

Port plan and reuse map: [03 — Port plan](03-port-plan.md).
