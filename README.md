# rblive4

[![Instagram: @i.erhan.es](https://img.shields.io/badge/Instagram-%40i.erhan.es-E4405F?logo=instagram&logoColor=white)](https://instagram.com/i.erhan.es)

**Run the Pioneer DJ XDJ-RX3 *rekordbox* standalone player on a Raspberry Pi 4B.**

rblive4 runs the ARM32 `rekordbox` player application (`rbp`, called `rb`
internally) extracted from **XDJ-RX3 firmware v1.20** on a **Raspberry Pi 4B**
running **Pi OS Lite 32-bit**, with an **HDMI monitor** and a **Pioneer
DDJ-FLX4** over USB.

The port started from a working Denon SC Live 4 port, which in turn derived from
the Prime GO work; the differences between them are the whole reason this was
tractable, and they are set out in [docs/13](docs/13-raspberrypi4.md).

> rblive4 is an **interoperability / preservation** project. It contains **no
> Pioneer/AlphaTheta firmware, no `rbp` binary, no Denon software and no
> rekordbox content.** You supply your own extracted assets. See
> [NOTICE.md](NOTICE.md).

---

## What it does

| Subsystem | Detail | Doc |
|---|---|---|
| Display | HDMI, forced to 1280×800 (the monitor's native mode with letterboxed bars is the fallback), rekordbox UI full-screen | [docs/06](docs/06-display.md), [docs/13](docs/13-raspberrypi4.md) |
| Pointing | No touchscreen on a monitor: an evdev pointer (touch panel or mouse) is discovered by capability and transformed into the tsc2007 protocol `rbp` reads | [docs/07](docs/07-touch.md) |
| Controls | transport, decks, mixer, jog and pads mapped from the DDJ-FLX4 MIDI surface — the tables are written from Pioneer's published MIDI message list and are unverified until a dump from the hardware, see [docs/15](docs/15-flx4-midi.md) | [docs/08](docs/08-controls.md), [docs/15](docs/15-flx4-midi.md) |
| Panel LEDs | PLAY / CUE / SYNC / FX LEDs mirror rbp's own LED state (blink included) | [docs/08](docs/08-controls.md) |
| VU meters | No meters on the FLX4, so `RB_LED_VU=0` by default — and the `rbp` machine-code patch behind them is then not installed at all | [docs/15](docs/15-flx4-midi.md) |
| Audio | master (RCA out) and headphones/cue on the FLX4's 4-channel USB audio; card, channel count and pair map are all configurable | [docs/09](docs/09-audio.md) |
| USB | rekordbox-exported stick detection and `export.pdb` import into DeviceSQL | [docs/10](docs/10-usb.md) |
| Launcher | `start-rb.sh` stops the desktop services that would fight for the display or the stick, then brings the player up | [docs/11](docs/11-runtime-launcher.md) |
| Access | plain root on Pi OS — none of the previous target's overlay/SSH workarounds are needed | [docs/13](docs/13-raspberrypi4.md) |

## State of the port

**The tree is mid-port, and this README describes the target, not a finished
unit.** Read it with [docs/13](docs/13-raspberrypi4.md)'s bring-up table
(S0–S7) open, which is where each step's check and likely failure is recorded.

* **Landed and verified off-hardware:** the chroot recipe, the `rbp` patch
  table, the shim ABI addresses, the audio stream model, the pointing split, the
  controls-shim decomposition, and the DirectFB build with its debug
  instrumentation removed.
* **Written but not yet run on a Pi:** the `cmdline.txt` recipe, the pointer
  discovery and transform, the audio device/map/format negotiation, the MIDI
  device discovery, the keyboard fallback (its map and its evdev reader, fixture-
  tested but never fed a real keyboard or mouse), and every device script.
* **Written and fixture-tested, but unverified against the unit:** the FLX4 map
  (`map_flx4.c`), now `RB_MIDI_MAP`'s default. Nothing in this tree has been run
  with an FLX4 attached, so its note and CC numbers come from Pioneer's
  published MIDI list; the fixture test proves the map matches its own tables and
  nothing more. The LED bridge is a step further behind — `RB_LED_DISABLE=1` is
  this target's interim setting, because `rbp_led.c` still carries the previous
  target's notes.
* **Not yet written:** the generalized DirectFB present path, gated on two
  measurements nobody has taken yet.

Where a number in these documents is an expectation rather than a measurement,
it says so; [docs/README](docs/README.md) explains the convention.

## Limitations

* The FLX4 tables are unverified until someone dumps them — the tables, the
  dump procedure and the unmeasured values are all in
  [docs/15](docs/15-flx4-midi.md), and the ones that a dump settles are marked
  `TODO: unverified` in `scripts/shims/map_flx4.c` too.
* The booth stream is dropped (one log line): the FLX4 has two output pairs, so
  master and headphones are what there is.
* No LEDs yet: `RB_LED_DISABLE=1` for this target, because the LED bridge is
  still shaped for the previous surface and the FLX4's transport/pad LED notes
  are not published. Pad illumination, and the velocity→colour encoding with it,
  land with that bridge.
* DJ FX parameter / layer encoders, StopTime and some shift actions are not
  mapped.
* Autostart is not provided; the launcher is run manually.

---

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
        ├── pointing : fbshim.so    (evdev pointer → RX3 tsc2007 protocol)
        ├── controls : knobshim.so  (DDJ-FLX4 MIDI → rbp keycodes + LEDs)
        ├── audio    : audioshim.so (JUCE/ALSA → the FLX4's USB audio)
        ├── usb      : usb-watch.sh + native DeviceSQL import
        └── daemons  : edb_streamd
```

There is no emulation: the real `rbp` binary from the XDJ-RX3 firmware runs
directly, with a set of thin shims translating the Pi's hardware into what `rbp`
expects.

## Why it works

Pi OS 32-bit is `armv7l`/armhf — a **hard-float kernel**, exactly the same
arrangement as the previous target — so the soft-float chroot runs untouched.
Three things carry over nearly intact, and they are the expensive parts:

* **The chroot recipe.** Every 32-bit-ARM-only constraint in the shims is
  *satisfied*, not violated: 32-bit `smem_start`/`time_t` in the fb structs,
  `SYS_mmap2`, `uc_mcontext.arm_*`, ARM32 machine-code patching. A 64-bit image
  would break all of them.
* **The `rbp` patch table.** All 68 words plus the 2-word `getPcController()`
  fix. Every patch's justification is a property of the *binary and the absent
  Pioneer hardware*, not of the machine underneath: there is no Pioneer panel
  MCU, no power-manager MCU and no i.MX6 board here either.
* **The shims' `rbp` ABI.** `IUiObjManager` @`0x02685f2c`, `MixerEngine`
  @`0x011493c0`, `PlayEngine` @`0x011497d0`, the `LedManager` walk, the
  `MonoLvMeter::getLedValue` prologue hook. These are addresses *in the
  executable*, so they stay valid verbatim. This is the single biggest reason
  the port is feasible.

What does have to change is everything that named a device:

* **Display** — `/dev/fb0` is now `vc4drmfb`, DRM fbdev emulation over HDMI,
  expected to be 16 bpp RGB565 at 1280×800 rather than a portrait 32 bpp panel
  (the expectation is `drm_fb_helper`'s, and `tools/fbdump` settles it on the
  day). The present path has to cope with a monitor that may not accept the mode
  we ask for.
* **Pointing** — there is no touchscreen. The tsc2007 protocol `rbp` reads is
  kept byte-for-byte, but what feeds it is now a discovered evdev pointer.
* **Controls** — the FLX4 sends different messages than any earlier surface,
  and puts its toggles on CCs rather than notes, so the map grew a CC-as-button
  concept and the shim stopped hardcoding a client id and a device node.
* **Audio** — `hw:1,0` with 8 channels becomes `plughw:CARD=DDJFLX4,DEV=0` with
  4, negotiated rather than forced, with the pair map in config.

---

## Repository layout

```
rblive4/
├── README.md                 you are here
├── TUTORIAL.md               build → deploy → run, end to end
├── NOTICE.md                 copyright / legal notes
├── LICENSE                   MIT (our code)
├── docs/                     findings & subsystem documentation (see docs/README.md)
│   ├── 00-overview … 12-troubleshooting
│   ├── 13-raspberrypi4.md    the target: image, cmdline, bring-up order
│   └── 15-flx4-midi.md       the DDJ-FLX4 map: what is verified, what is not
├── scripts/
│   ├── build-chroot.sh       host: assemble the soft-float chroot tarball
│   ├── patch-rbp-nopc.py     the getPcController() no-op patch
│   ├── device/               scripts that run on the Pi
│   └── shims/                LD_PRELOAD shims (soft-float) + Makefile
├── tools/
│   ├── patch-rbp/            shared rbp patch table (stock -> rbp-audio)
│   ├── build-directfb/       DirectFB 1.4.16 patch + build notes
│   ├── fbdump.c              framebuffer geometry/format diagnostic
│   └── evdevdump.c           input device names, caps and absinfo
└── work/                     local scratch (gitignored)
```

## Quick start

Full instructions live in **[TUTORIAL.md](TUTORIAL.md)**. The short version:

```bash
# 0. prerequisites (host): arm-linux-gnueabi-gcc, python3, tar, docker
#    extracted XDJ-RX3 assets (see docs/04-firmware-assets.md)
#    a Pi 4 running Pi OS Lite 32-bit, with root

# 1. build the patched DirectFB stack (tools/build-directfb/README.md)
#    -> work/dfb/lib/...

# 2. build the soft-float chroot (rootfs + rbp-audio + shims + DirectFB
#    + directfbrc + touch calibration)
RX3=/path/to/extracted scripts/build-chroot.sh   # -> work/rblive4-pi4.tgz

# 3. deploy (the tarball carries rb.conf; install.sh puts it in place)
scp work/rblive4-pi4.tgz pi@<host>:/tmp/
scp -r scripts/device    pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rblive4-pi4.tgz'

# 4. launch
ssh pi@<host> 'sudo sh /opt/rblive4/start-rb.sh'
```

## What is *not* in this repo

To stay clean, rblive4 deliberately excludes:

* any `.UPD`, `.iso`, firmware image or `rbp` executable,
* Denon / Engine OS files,
* rekordbox music databases or media,
* any firmware key,
* downloaded firmware or decryption tooling (handled by the related projects),
* built binaries of the shims (build them from source).

## Related projects

rblive4 is one of several XDJ-RX3 `rb` porting projects. Firmware acquisition,
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
