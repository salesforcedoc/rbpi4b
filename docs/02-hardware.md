# 02 — Hardware & environment

Three machines matter here: the **XDJ-RX3** (source of `rbp`), the **Prime GO**
(the already-working port), and the **SC Live 4** (this port's target).

| | Pioneer XDJ-RX3 (source) | Denon Prime GO (worked) | Denon SC Live 4 (target) |
|---|---|---|---|
| SoC | NXP i.MX6 Quad, ARMv7 **soft-float** | Rockchip RK3288, ARMv7 hard-float | Rockchip **RK3288**, ARMv7 hard-float |
| Kernel | Linux 3.0.101 | 6.1.111-inmusic PREEMPT_RT | 6.1.111-inmusic PREEMPT_RT |
| OS | BusyBox / in-house init | Buildroot 2023.02.11, systemd | **Buildroot 2023.02.11, systemd** |
| Display | 1280×800 landscape, RGB565 | 800×1280 portrait, RGB32 DRM fb | 800×1280 portrait, RGB32 DRM fb |
| Touch | tsc2007 resistive | ILI2117 capacitive (event0) | **ILI2117** capacitive (event0) |
| Audio | 3× CS4344 DACs | JP11 codec, 4 ch (`hw:1,0`) | **JP21 codec, 8 ch** (`hw:1,0`) |
| Controls | EUP / SUB MCUs over SPI | ALSA MIDI "Control Surface" (seq 16) | ALSA MIDI **"Control Surface"** (seq 16) |
| USB | 2 host ports + sub-MCU | 1× USB-A | USB-A host (EHCI/OHCI) |
| Storage | — | root 466 MB ro, `/data` ~50 MB free | root 466 MB ro, **`/data` 5.6 GB free** |
| Root SSH | — | manual setup | **already done** (freelive4) |

The SC Live 4 is a **drop-in target** for the PrimeBox port: every subsystem
`rbp` touches is either identical or a superset (more audio channels, more
space). Only two things need real adaptation:

1. **Audio** — the JP21 codec exposes **8 playback channels** instead of the
   Prime GO's 4. See [09 — Audio](09-audio.md).
2. **Controls** — the SC Live 4's MIDI map is different (4-channel deck, more
   pads/controls). See [08 — Controls](08-controls.md).

## 1. The soft-float chroot

`rbp` and its libraries are soft-float glibc 2.13. To run them we assemble an
RX3 userland at `/data/rbx3-run` and `chroot` into it — exactly as PrimeBox.
The RK3288 kernel provides the hard-float host tooling; the chroot is pure RX3
soft-float.

### Contents of `/data/rbx3-run` (planned)

```
/data/rbx3-run/
├── lib/ld-linux.so.3 -> ld-2.13.so     soft-float loader
├── lib/libc.so.6, libpthread.so.0, ... RX3 glibc 2.13
├── usr/lib/                            libstdc++, DirectFB 1.4, freetype, ...
├── usr/lib/directfb-1.4-6/
│   ├── systems/libdirectfb_fbdev.so    ← rebuilt, patched module
│   ├── inputdrivers/…                  linux_input (VT gate removed)
│   └── wm/libdirectfbwm_default.so
├── root/pdj/rbp                        ← rbp-audio
├── root/gui/                           fonts + pset + imagedata
├── usr/bin/edb_streamd, kill_daemon    DeviceSQL
├── bin/sh -> busybox
├── usr/share/alsa/                     ALSA config
├── media/usb1/sda1                     bind-mount point for the stick
├── dev/ proc/ sys/ tmp/                bind-mounted from host
└── etc/mtab -> /proc/mounts
```

### Bind mounts (run after every reboot)

```sh
mkdir -p /data/rbx3-run/dev /data/rbx3-run/proc /data/rbx3-run/sys /data/rbx3-run/tmp
mount --bind /dev  /data/rbx3-run/dev
mount --bind /proc /data/rbx3-run/proc
mount --bind /sys  /data/rbx3-run/sys
mount --bind /tmp  /data/rbx3-run/tmp
```

`/tmp` is shared, so FIFOs created on the host (e.g. `/tmp/udev_usb1`) are the
same objects `rbp` sees inside the chroot.

### Device stubs

`rbp` talks to i.MX6 devices that do not exist on the Rockchip. PrimeBox
emulates them so `open()` succeeds and threads don't spin/crash — all of this
carries over verbatim:

| Device | Type | Why |
|---|---|---|
| `/dev/gpiodrv` | regular file + `read()` shim | `GpioManager` blocks/polls on it |
| `/dev/subucom_spi{1,2}.0`, `/dev/subucom_spi_rdy{3,4}.0` | FIFOs | polled SPI to the (absent) sub-MCU |
| `/dev/hidg0` | FIFO | USB HID gadget for rekordbox HID mode |
| `/dev/printkdrv0`, `/dev/tsc2007_2-0048` | regular files | ioctl-only; touch one replaced by shim |
| `/dev/paudiog0` | **absent** | presence makes JUCE try gadget-audio ioctls |
| `/dev/mem` | chmod 000 | `rbp` maps i.MX6 phys regs; must be blocked |

## 2. Cross toolchain

Same as PrimeBox — the shims must be **soft-float EABI5, GLIBC_2.4-only** to
load under the RX3 glibc 2.13. Toolchain: Ubuntu's `arm-linux-gnueabi-gcc`.

```bash
sudo apt-get install gcc-arm-linux-gnueabi libc6-dev-armel-cross
```

Link against the **RX3 rootfs libraries** so symbol versioning is correct:

```bash
RX3=extracted/XDJRX3-rootfs
arm-linux-gnueabi-gcc -O2 -march=armv5t -mfloat-abi=soft \
    -fno-stack-protector -fPIC -shared \
    -o knobshim2.so knobshim2.c \
    -I"$RX3/usr/include" -L"$RX3/lib" -L"$RX3/usr/lib" \
    -lpthread -lc -Wl,-rpath-link,"$RX3/lib:$RX3/usr/lib"
```

Verify:

```bash
arm-linux-gnueabi-objdump -T knobshim2.so | grep GLIBC | sort -u
# must only reference GLIBC_2.4 / GLIBC_2.7 (no 2.17!)
```

## 3. DirectFB

`rbp` renders through DirectFB 1.4. The stock RX3 `libdirectfb_fbdev.so`
assumes an i.MX6 fbdev (16 bpp, 1280×800) and crashes on the Rockchip DRM fb.
The patched fbdev module is built from
[`tools/build-directfb/`](../tools/build-directfb/) — reused as-is (same
800×1280 geometry).
See [06 — Display](06-display.md).

## 4. Disk budget

The SC Live 4's `/data` is ~5.9 GB with ~5.6 GB free, vs the Prime GO's ~50 MB.
The full chroot is ~60 MB, so there is **no** trimming pressure:

| Item | Size |
|---|---|
| glibc + libstdc++ + DirectFB + freetype | ~30 MB |
| `rbp` | 7.6 MB |
| `gui/` fonts + imagedata | ~15 MB |
| EDB daemons | <1 MB |
| shims + scripts | <1 MB |
| **Total** | **~60 MB** |
