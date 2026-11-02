# rbpi4b

Credit to: [![Instagram: @i.erhan.es](https://img.shields.io/badge/Instagram-%40i.erhan.es-E4405F?logo=instagram&logoColor=white)](https://instagram.com/i.erhan.es) for the original base of this code.

**Run the Pioneer DJ XDJ-RX3 *rekordbox* standalone player on a Raspberry Pi 4B.**

rbpi4b runs the ARM32 `rekordbox` player application (`rbp`, called `rb`
internally) extracted from **XDJ-RX3 firmware v1.20** on a **Raspberry Pi 4B**
running **Pi OS Lite 32-bit**, with an **HDMI monitor** and a **Pioneer
DDJ-FLX6** over USB.

The port started from a working Denon SC Live 4 port, which in turn derived from
the Prime GO work; the differences between them are the whole reason this was
tractable, and they are set out in [docs/13](docs/13-raspberrypi4.md).

> rbpi4b is an **interoperability / preservation** project. It contains **no
> Pioneer/AlphaTheta firmware, no `rbp` binary, no Denon software and no
> rekordbox content.** You supply your own extracted assets. See
> [NOTICE.md](NOTICE.md).

---

## What it does

| Subsystem | Current behavior | Documentation |
| --- | --- | --- |
| Display and pointing | Native RX3 UI through DirectFB; evdev touchscreen/mouse translation | [display](docs/06-display.md), [touch](docs/07-touch.md) |
| Controls | DDJ-FLX6 two-deck mapping, using physical decks3/4 for auxiliary functions; keyboard remains available | [FLX6 inventory](docs/flx6-mapping-audit.md), [controls](docs/08-controls.md) |
| Feedback | Native transport/pad/loop/FX state, channel meters, jog position/load animations and Merge rings | [shim guide](scripts/shims/README.md) |
| Audio | FLX6 USB master0/1 and headphones2/3; hardware headphone MIX/MASTER CUE, optional HDMI mirror | [audio](docs/09-audio.md) |
| USB and recording | Rekordbox-exported media, safe-eject requests, native Track Mark and master recording | [USB](docs/10-usb.md), [FLX6 utilities](scripts/shims/README.md#merge-fx-utilities-and-shutdown) |
| Runtime | systemd launcher, configuration/status page, VNC and hot-plug recovery | [launcher](docs/11-runtime-launcher.md), [configuration](docs/20-config-page.md), [VNC](docs/19-vnc.md) |

## State of the port

The user has confirmed FLX6 audio/USB playback, core controls, eight pad modes
and lights, meters, Beat FX ON/OFF and beat indicators, right Merge utilities,
the headphone-mixing correction, startup Reverse correction and normal hot-plug.

The latest audit fixes, shifted transport/Instant Doubles, 14-bit mixer,
held-input cleanup, utility-preset calibration, shutdown hold and countdown are
built and locally checked. Subsequent Pi hardware verification has not been
reported. ARM build/ABI checks, 243 controller-registry checks and focused host
harnesses passed; mocked shutdown tests never shut down the workstation.

The [mapping inventory](docs/flx6-mapping-audit.md) separates implementation
from hardware evidence. Older dated FLX4 and SC Live 4 measurements remain in
the subsystem docs as historical references; they do not describe the default
controller or its current verification status.

## Limitations

- FLX4 is an unused compatibility option; FLX6 is the compiled default.
- Right Merge utility preset alignment is explicit: select physical preset1,
  then SHIFT+press its knob. The supplied protocol cannot read the preset.
- Firmware Jog Cutter cannot currently be disabled by the shim.
- Only two playback decks are implemented; FLX6 decks3/4 are extra button layers.
- The USB audio map carries master/headphones; the separate RX3 booth stream
  is not assigned. Analog booth routing is outside the MIDI map.
- Some native RX3 functions still lack controller shortcuts; see the inventory.
- Very fast same-track load animations can fall between LED polls.

Hold the left Merge knob five seconds for a graceful host shutdown; release
early to cancel. Its ring blinks faster as the threshold approaches. Deploy
the new shim before relying on this feature, and let the Pi finish shutting
down before removing power.

## The idea

```
        XDJ-RX3 v1.20 assets (extracted externally)
                              │
       ┌──────────────────────┴───────────────────────┐
       │ pdj/rbp  (ARM32, soft-float)                 │
       │ + interoperability patches                   │  tools/patch-rbp
       └──────────────────────┬───────────────────────┘
                              │  rbp-audio
                              ▼
   Raspberry Pi 4B  ──  soft-float glibc-2.13 chroot  ──  rbp
        │                    ( /opt/rblive4/rbx3-run )
        │
        ├── display  : rebuilt DirectFB fbdev module (the present path)
        ├── pointing : fbshim.so    (evdev pointer → RX3 tsc2007 protocol;
        │                            also sends rbp's QUANTIZE keycode for the
        │                            deck boxes rbp draws but does not bind)
        ├── controls : knobshim.so  (DDJ-FLX6 MIDI → rbp keycodes + LEDs)
        ├── audio    : audioshim.so (JUCE/ALSA → the FLX6's USB audio)
        ├── usb      : usb-watch.sh + native DeviceSQL import
        └── daemons  : edb_streamd
```

There is no emulation: the real `rbp` binary from the XDJ-RX3 firmware runs
directly, with a set of thin shims translating the Pi's hardware into what `rbp`
expects.

## Why it works

Pi OS 32-bit is `armhf` on a **hard-float kernel**, so the soft-float chroot runs
untouched — the same arrangement as the previous target. (The measured Pi's
kernel is `aarch64`; what the chroot needs is 32-bit emulation in the kernel, not
a 32-bit kernel.)

Three things carry over nearly intact, and they are the expensive parts:

* **The chroot recipe.** Every 32-bit-ARM-only constraint in the shims is
  *satisfied*, not violated: 32-bit `smem_start`/`time_t` in the fb structs,
  `SYS_mmap2`, `uc_mcontext.arm_*`, ARM32 machine-code patching. They are
  constraints on the chroot's own 32-bit binaries — there is no 64-bit `rbp` — so
  the kernel only has to execute 32-bit ARM ELF, which current Pi OS kernels do
  even though their kernel is 64-bit.
* **The `rbp` patch table.** All 66 words plus the 12-word `getPcController()`
  fix. Every patch's justification is a property of the *binary and the absent
  Pioneer hardware*, not of the machine underneath: there is no Pioneer panel
  MCU, no power-manager MCU and no i.MX6 board here either.
* **The shims' `rbp` ABI.** `IUiObjManager` @`0x02685f2c`, `MixerEngine`
  @`0x011493c0`, `PlayEngine` @`0x011497d0`, the `LedManager` walk, the
  `MonoLvMeter::getLedValue` prologue hook. These are addresses *in the
  executable*, so they stay valid verbatim. This is the single biggest reason
  the port is feasible.

What does have to change is everything that named a device:

* **Display** — `/dev/fb0` is now `vc4drmfb`, DRM fbdev emulation over HDMI, and
  `tools/fbdump` measured it as 16 bpp RGB565 at 1280×800: the same format and
  the same geometry as `rbp`'s logical surface, rather than a portrait 32 bpp
  panel. So there is no transform to do — but the fb holds **one page** while
  the stock driver asked for three buffers, and that is what the patch's
  page-count buffer mode fixes.
* **Pointing** — there is no touchscreen. The tsc2007 protocol `rbp` reads is
  kept byte-for-byte, but what feeds it is now a discovered evdev pointer.
* **Controls** — the FLX6 has its own note/CC map, including alternate messages
  for shifted controls and selected FX slots. Controller discovery uses its
  registered port-name hint rather than a hardcoded ALSA client or device node.
* **Audio** — `hw:1,0` with 8 channels becomes `hw:CARD=DDJFLX6,DEV=0` with 4,
  negotiated rather than forced, with the pair map and the output format in
  config. `plughw:` was the plan and was wrong: the map names hardware channels,
  and a plug device reports the wrong count for that (see [09](docs/09-audio.md)).

---

## Repository layout

```
rbpi4b/
├── README.md                 you are here
├── TUTORIAL.md               build → deploy → run, end to end
├── NOTICE.md                 copyright / legal notes
├── LICENSE                   MIT (our code)
├── docs/                     findings & subsystem documentation (see docs/README.md)
│   ├── 00-overview … 12-troubleshooting
│   ├── 13-raspberrypi4.md    the target: image, cmdline, bring-up order
│   └── flx6-mapping-audit.md the current map, verification and limits
│       15-flx4-midi.md       FLX4 compatibility and historical measurements
├── scripts/
│   ├── build-chroot.sh       host: assemble the soft-float chroot tarball
│   ├── patch-rbp-nopc.py     the getPcController() no-op patch
│   ├── patch-rbp-depth.py    the layer pixel format (the depth pair)
│   ├── device/               scripts that run on the Pi
│   └── shims/                LD_PRELOAD shims (soft-float) + Makefile
├── tools/
│   ├── patch-rbp/            shared rbp patch table (stock -> rbp-audio)
│   ├── build-directfb/       DirectFB 1.4.16 patch + build notes
│   ├── build-toolchain/      the soft-float armel build image (Dockerfile)
│   ├── fbdump.c              framebuffer geometry/format diagnostic
│   └── evdevdump.c           input device names, caps and absinfo
└── work/                     local scratch (gitignored)
```

## Quick start

Full instructions live in **[TUTORIAL.md](TUTORIAL.md)**. The short version:

```bash
# 0. prerequisites
#    host   : arm-linux-gnueabi-gcc, libc6-dev-armel-cross, python3, tar, and the
#             DirectFB build tools -- flux included (TUTORIAL.md step 0). Docker
#             only for the shim build image, tools/build-toolchain/Dockerfile
#    assets : the extracted XDJ-RX3 tree (see docs/04-firmware-assets.md)
#    Pi     : Pi OS Lite 32-bit, booted, on the network, with root, AND gcc +
#             make + python3 -- install.sh compiles vncserve on the unit, and
#             the boot screen and config page are python3; without either it
#             warns and carries on, so the viewer or the page is simply absent.
#             Apply docs/13's cmdline.txt recipe and reboot before deploying.

# 1. build the patched DirectFB stack (tools/build-directfb/README.md)
#    -> work/dfb/lib/...   (build-chroot.sh looks there by default; DFB= overrides)

# 2. build the soft-float chroot (rootfs + rbp-audio + shims + DirectFB
#    + directfbrc + touch calibration)
RX3=/path/to/extracted scripts/build-chroot.sh   # -> work/rbpi4b-pi4.tgz

# 3. deploy (the tarball carries rb.conf; install.sh puts it in place)
scp work/rbpi4b-pi4.tgz pi@<host>:/tmp/
scp -r scripts/device    pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rbpi4b-pi4.tgz'

# 4. launch -- usually already up: install.sh enables rblive4.service
#    (RB_AUTOSTART=1), so this is a RESTART, or the manual path at RB_AUTOSTART=0
ssh pi@<host> 'sudo sh /opt/rblive4/start-rb.sh'
```

## What is *not* in this repo

To stay clean, rbpi4b deliberately excludes:

* any `.UPD`, `.iso`, firmware image or `rbp` executable,
* Denon / Engine OS files,
* rekordbox music databases or media,
* any firmware key,
* downloaded firmware or decryption tooling (handled by the related projects),
* built binaries of the shims (build them from source).

## Related projects

rbpi4b is one of several XDJ-RX3 `rb` porting projects. Firmware acquisition,
decryption and key handling are **out of scope** here; the separate **PrimeBox**
project (the Prime GO port) covers that pipeline, and **rb2go** / **chromebit**
explore other targets. Root-shell access to a Denon unit is covered by
**freelive4** — that is where the SC Live 4 half of this repo's history comes
from, and none of it is needed on a Pi. Obtain those projects separately.

## Credits

* Pioneer DJ / AlphaTheta — XDJ-RX3 and its GPL source distribution.
* Denon DJ / inMusic — the SC Live 4, this port's starting point.
* Raspberry Pi Ltd — the Pi 4 and Raspberry Pi OS.
* DirectFB, JUCE, ALSA and glibc maintainers.

See [NOTICE.md](NOTICE.md) for licensing details.
