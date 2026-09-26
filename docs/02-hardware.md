# 02 — Hardware & environment

Four machines matter here: the **XDJ-RX3** (source of `rbp`), the **Prime GO**
(the first working port), the **SC Live 4** (the previous target, and the
reference the current tree was built on), and the **Raspberry Pi 4B** (the
target).

| | Pioneer XDJ-RX3 (source) | Denon Prime GO (worked) | Denon SC Live 4 (previous) | Raspberry Pi 4B (**target**) |
|---|---|---|---|---|
| SoC | NXP i.MX6 Quad, ARMv7 **soft-float** | Rockchip RK3288, ARMv7 hard-float | Rockchip RK3288, ARMv7 hard-float | BCM2711 Cortex-A72 (4 cores), **hard-float**, 32-bit userland — the kernel is arm64 on current images |
| Kernel | Linux 3.0.101 | 6.1.111-inmusic PREEMPT_RT | 6.1.111-inmusic PREEMPT_RT | Raspberry Pi kernel **`6.18.50+rpt-rpi-v8` measured** (aarch64, `32-bit EL0 Support`) |
| OS | BusyBox / in-house init | Buildroot 2023.02.11, systemd | Buildroot 2023.02.11, systemd | **Pi OS Lite 32-bit**, Debian 13 (trixie) as measured, systemd |
| Display | 1280×800 landscape, RGB565 | 800×1280 portrait, RGB32 DRM fb | 800×1280 portrait, RGB32 DRM fb | **HDMI** `vc4drmfb`, **1280×720 at 16 bpp RGB565 as measured** — the mode has never been asked for; 1280×800 is `rbp`'s logical geometry ([13](13-raspberrypi4.md)) |
| Touch | tsc2007 resistive | ILI2117 capacitive (event0) | ILI2117 capacitive (event0) | **none** — an evdev pointer (mouse or USB touch panel) |
| Audio | 3× CS4344 DACs | JP11 codec, 4 ch (`hw:1,0`) | JP21 codec, 8 ch (`hw:1,0`) | DDJ-FLX4 USB audio, 4 ch (`plughw:CARD=DDJFLX4,DEV=0`) |
| Controls | EUP / SUB MCUs over SPI | ALSA MIDI "Control Surface" (seq 16) | ALSA MIDI "Control Surface" (seq 16) | ALSA MIDI **"DDJ-FLX4 MIDI 1"** over USB |
| USB | 2 host ports + sub-MCU | 1× USB-A | USB-A host (EHCI/OHCI) | 2× USB 3.0 + 2× USB 2.0 |
| Storage | — | root 466 MB ro, `/data` ~50 MB free | root 466 MB ro, `/data` 5.6 GB free | microSD, root rw, GBs free |
| Root access | — | manual setup | persistent (freelive4) | **plain root** — no workaround needed |

The first three columns are history: they explain why the tree looks the way it
does, and [01](01-device-survey.md) is the raw survey of the SC Live 4. **The Pi
column is the target**, and the two things it shares with the Prime GO and SC
Live 4 are the ones that matter:

1. **A hard-float kernel running a soft-float userland.** Pi OS 32-bit is
   `armhf`, and its kernel executes soft-float EABI ELF natively — the same trick
   the Buildroot targets used, with none of the vendor baggage. Note that what
   enables it is 32-bit *emulation* in the kernel (`32-bit EL0`), not the kernel
   being 32-bit: the measured unit's kernel is arm64 and runs the soft-float
   chroot all the same.
2. **No Pioneer hardware anywhere.** Every `rbp` patch exists because a Pioneer
   panel MCU, power-manager MCU or i.MX6 board check has nothing to talk to on
   the target. That is as true of a Pi as it was of a Denon.

Everything else is a device difference, and the two that need real work are the
usual two — see [08 — Controls](08-controls.md) and
[09 — Audio](09-audio.md), plus the display's present path in
[06 — Display](06-display.md).

> **Use Pi OS Lite 32-bit — and expect a 64-bit kernel.** The 32-bit-ARM
> constraints the shims depend on (32-bit `smem_start`/`time_t` layout in the fb
> structs, `SYS_mmap2`, `uc_mcontext.arm_*`, ARM32 machine-code patching of `rbp`
> itself) are constraints on the *chroot's* binaries, which are 32-bit whatever
> the host runs. The kernel therefore only needs to execute 32-bit ARM ELF
> (`32-bit EL0`/`CONFIG_COMPAT`), which Pi OS kernels do — the measured Pi runs
> the arm64 kernel `6.18.50+rpt-rpi-v8` on its 32-bit image and reports exactly
> that. 32-bit Lite is the tested configuration because that is what every
> measurement in [13](13-raspberrypi4.md) was taken on, not because a 64-bit
> kernel is fatal.

## 1. The soft-float chroot

`rbp` and its libraries are soft-float glibc 2.13. To run them we assemble an
RX3 userland at `/opt/rblive4/rbx3-run` and `chroot` into it — exactly as
PrimeBox did on Buildroot. The Pi kernel provides the hard-float host tooling;
the chroot is pure RX3 soft-float.

### Contents of `/opt/rblive4/rbx3-run`

```
/opt/rblive4/rbx3-run/
├── lib/ld-linux.so.3 -> ld-2.13.so     soft-float loader
├── lib/libc.so.6, libpthread.so.0, ... RX3 glibc 2.13
├── usr/lib/                            libstdc++, DirectFB 1.4, freetype, ...
├── usr/lib/directfb-1.4-6/
│   ├── systems/libdirectfb_fbdev.so    ← rebuilt from the patched tree
│   ├── inputdrivers/…                  linux_input (VT gate removed)
│   └── wm/libdirectfbwm_default.so
├── root/pdj/rbp                        ← rbp-audio
├── root/gui/                           fonts + pset + imagedata
├── usr/bin/edb_streamd, kill_daemon    DeviceSQL
├── bin/sh -> busybox
├── usr/share/alsa/                     ALSA config
├── media/usb1/sda1                     bind-mount point for the stick
├── dev/ proc/ sys/ tmp/                bind-mounted from the host
└── etc/mtab -> /proc/mounts
```

### Bind mounts (run after every reboot)

```sh
mkdir -p /opt/rblive4/rbx3-run/dev /opt/rblive4/rbx3-run/proc \
         /opt/rblive4/rbx3-run/sys /opt/rblive4/rbx3-run/tmp
mount --bind /dev  /opt/rblive4/rbx3-run/dev
mount --bind /proc /opt/rblive4/rbx3-run/proc
mount --bind /sys  /opt/rblive4/rbx3-run/sys
mount --bind /tmp  /opt/rblive4/rbx3-run/tmp
```

`fix-dev.sh` does this, and the launcher runs it. `/tmp` is shared with the
host, so FIFOs created outside the chroot (e.g. `/tmp/udev_usb1`) are the same
objects `rbp` sees inside it.

### Device stubs

`rbp` talks to i.MX6 devices that do not exist on any of these targets. The
stubs emulate them so `open()` succeeds and threads don't spin or crash — all of
this carries over verbatim:

| Device | Type | Why |
|---|---|---|
| `/dev/gpiodrv` | regular file + `read()` shim | `GpioManager` blocks/polls on it |
| `/dev/subucom_spi{1,2}.0`, `/dev/subucom_spi_rdy{3,4}.0` | FIFOs | polled SPI to the (absent) sub-MCU |
| `/dev/hidg0` | FIFO | USB HID gadget for rekordbox HID mode |
| `/dev/printkdrv0`, `/dev/tsc2007_2-0048` | regular files | ioctl-only; the touch one is replaced by the shim |
| `/dev/paudiog0` | **absent** | presence makes JUCE try gadget-audio ioctls |
| `/dev/mem` | chmod 000 | `rbp` maps i.MX6 phys regs; must be blocked |

The FIFOs are FIFOs and not regular files on purpose: a regular file makes
`rbp`'s `poll()` return immediately, and under `SCHED_FIFO` that thread spins a
core at 100%.

Because the chroot's `/dev` is a **bind mount of the host's `/dev`**, creating
these really creates them in the Pi's own `/dev` — which is a tmpfs, and why
they have to be recreated every boot.

## 2. Cross toolchain

The shims must be **soft-float EABI5, GLIBC_2.4-only** to load under the RX3
glibc 2.13. Toolchain: Ubuntu's `arm-linux-gnueabi-gcc`.

```bash
sudo apt-get install gcc-arm-linux-gnueabi libc6-dev-armel-cross
```

Link against the **RX3 rootfs libraries** so symbol versioning is correct. The
shims are built by their own Makefile, which already carries the flags, the
rootfs paths and the empty `libc_nonshared.a` stubs the RX3 runtime rootfs
lacks:

```bash
make -C scripts/shims RX3=extracted/XDJRX3-rootfs
```

Verify (this is what `make check` does):

```bash
arm-linux-gnueabi-objdump -T scripts/shims/knobshim.so | grep GLIBC | sort -u
# must only reference GLIBC_2.4 / GLIBC_2.7 (no 2.17!)
```

The two host-side diagnostics are different: `tools/fbdump.c` and
`tools/evdevdump.c` are built **on the Pi**, natively, because they talk to
kernel interfaces rather than to `rbp`:

```bash
sudo apt-get install -y gcc make
gcc -O2 -static -o fbdump    tools/fbdump.c
gcc -O2 -static -o evdevdump tools/evdevdump.c
```

## 3. DirectFB

`rbp` renders through DirectFB 1.4. The stock RX3 `libdirectfb_fbdev.so` assumes
an i.MX6 fbdev (16 bpp, 1280×800) and crashes on a DRM fb. The patched fbdev
module is built from [`tools/build-directfb/`](../tools/build-directfb/).

The Pi's `/dev/fb0` is **`vc4drmfb`** — DRM fbdev emulation, like the Rockchip
target's, but on a much more standard `drm_fb_helper` client. `tools/fbdump`
recorded it, and two measured facts drive the design:

* `xres=1280 yres=800 bpp=16 line_length=2560 yres_virtual=800 ypanstep=1/1` —
  16 bpp RGB565 is `rbp`'s own format, so the present path needs no conversion;
  see [13](13-raspberrypi4.md#measuring-the-framebuffer-toolsfbdump) for the raw
  output.
* **`ypanstep` is `1/1` with one page** — the fb claims it can pan and has
  nowhere to pan to, which is the value that misled the buffer-mode decision
  into three buffers where one fits. That, not the geometry, was the display
  blocker; see [06 — Display](06-display.md#the-present-path). Separately,
  `vc4`'s `drm_fbdev` returns from `FBIOPAN_DISPLAY` without waiting for vblank,
  which is why the fb shim has a frame pacer.

The mode itself comes from the kernel command line, not from `config.txt`; see
[13 — the HDMI mode](13-raspberrypi4.md#the-hdmi-mode). The present path is
general — `off`/`convert`/`letterbox`/`crop`/`rotate` — and is described in
[06 — Display](06-display.md).

## 4. Disk budget

There is no trimming pressure on a Pi — an 8 GB card is ample — but the numbers
are worth having, because the chroot is much bigger once extracted than the
tarball that carries it:

| Item | Size |
|---|---|
| glibc + libstdc++ + DirectFB + freetype | ~30 MB |
| `rbp` | 7.6 MB |
| `gui/` fonts + imagedata | ~15 MB |
| EDB daemons | <1 MB |
| shims + scripts | <1 MB |
| **Total** | **~60 MB** |

The extracted tree is the larger figure — around 150–250 MB on disk depending on
the card's block size — plus `work/rblive4-pi4.tgz` on the workstation. Budget a
few hundred MB, and keep the tarball anywhere convenient.
