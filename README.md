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
| Display | HDMI at 1280×800 — the mode is forced via `video=` and the measured panel takes it, so the fb matches `rbp`'s 1280×800×16 RGB565 surface exactly: no scaling, no bars, no crop | [docs/06](docs/06-display.md), [docs/13](docs/13-raspberrypi4.md) |
| Pointing | No touchscreen on a monitor: an evdev pointer (touch panel or mouse) is discovered by capability and transformed into the tsc2007 protocol `rbp` reads — including `rbp`'s own `1279 - x` reflection of that protocol's x, undone in the shim so a tap lands under the finger | [docs/07](docs/07-touch.md) |
| Controls | transport, decks, mixer, jog and pads mapped from the DDJ-FLX4 MIDI surface — the tables are written from Pioneer's published MIDI message list and are unverified until a dump from the hardware, see [docs/15](docs/15-flx4-midi.md) | [docs/08](docs/08-controls.md), [docs/15](docs/15-flx4-midi.md) |
| Panel LEDs | PLAY / CUE / SYNC / FX LEDs mirror rbp's own LED state (blink included) | [docs/08](docs/08-controls.md) |
| VU meters | No meters on the FLX4, so `RB_LED_VU=0` by default — and the `rbp` machine-code patch behind them is then not installed at all | [docs/15](docs/15-flx4-midi.md) |
| Audio | master (RCA out) and headphones/cue on the FLX4's 4-channel USB audio, **plus the same master audio out of the Pi's HDMI**; card, channel count and pair map are all configurable | [docs/09](docs/09-audio.md) |
| USB | rekordbox-exported stick detection and `export.pdb` import into DeviceSQL | [docs/10](docs/10-usb.md) |
| Launcher | `start-rb.sh` stops the desktop services that would fight for the display or the stick, then brings the player up | [docs/11](docs/11-runtime-launcher.md) |
| Boot time | `install.sh` applies a reversible boot trim — cloud-init off the critical chain, the apt timers off the boot path, the launcher's no-op `systemctl` calls behind read-only guards, and the unit ordered at `basic.target` instead of after `multi-user.target` | [docs/13](docs/13-raspberrypi4.md#boot-time) |
| Access | plain root on Pi OS — none of the previous target's overlay/SSH workarounds are needed | [docs/13](docs/13-raspberrypi4.md) |

## State of the port

**It runs on the hardware.** `rbp` has been launched on the measured Pi 4 and
paints its UI; the audio stream reaches the FLX4's USB card with every write
accepted. Read this with [docs/13](docs/13-raspberrypi4.md)'s bring-up table
(S0–S7) open, which is where each step's check and likely failure is recorded —
each row there says whether it has been run, and what it measured.

**Verified on the unit:**

* **Display.** The forced `video=HDMI-A-1:1280x800@60` mode was accepted by a
  sink whose own EDID lists nothing above 720p, and the fb it produced is
  `rbp`'s logical surface exactly — 1280×800, 16 bpp RGB565, stride 2560, **one
  page**. The driver logs `PRESENT: mode=off angle=0 … pages=1 pan=0`, i.e. the
  layer surface *is* the fb page and no blit happens at all. The UI was then
  confirmed by capturing `/dev/fb0` and decoding it. This needed one real fix:
  the stock driver forced three buffers while the fb holds one, so the buffer
  mode now comes from the page count rather than `ypanstep`
  ([docs/06](docs/06-display.md#the-present-path)).
* **Audio.** `audioshim.so` opens `hw:CARD=DDJFLX4,DEV=0`, negotiates the card's
  4 channels and `S24_3LE`, and writes 64-frame/768-byte blocks at ≈realtime with
  **zero** negative returns over 41,001 writes. Getting there fixed three
  defects, all documented in [docs/09](docs/09-audio.md) — the last of them a
  hand-rolled `SND_PCM_FORMAT_S24_3LE` that had been wrong for the life of the
  shim and whose only symptom was silence. **The same master audio also goes out
  of the Pi's HDMI**, as a second advisory output the FLX4's stream cannot be
  affected by: it is opened non-blocking, holds its own ring half full so the two
  clocks (~155 ppm apart, measured) never starve it, and a sink that is
  unplugged mid-set costs it dropped blocks and a log line, never a stall. That
  sink needed **a fourth defect found**: the vc4 HDMI PCMs take only
  `IEC958_SUBFRAME_LE`, a real IEC 60958 subframe rather than a 32-bit
  container, and the first `<< 8` reading of it produced *"loud and very
  distorted"* audio with every counter perfect — an ears-only failure the
  bring-up table's S4.6 row exists to catch. The HDMI copy's level follows the
  unit's own **MASTER LEVEL** knob and nothing else, so it tracks the room
  without attenuating the FLX4's output a second time
  ([docs/09](docs/09-audio.md#the-hdmi-mirror)).
* **Controls.** The shim subscribes to the unit's own port
  (`subscribed to 28:0 'DDJ-FLX4 MIDI 1'`), and an `aseqdump` capture taken from
  it turned the FLX4 map's largest guess — the jog's counts per revolution — into
  a measurement (720, where the inherited value was 128).
* **Pointing.** A real mouse is discovered and opened
  (`/tmp/pointsrc.log`: `relative device /dev/input/event7 name='Logitech G203
  Prodigy Gaming Mouse'`).
* **Deploy.** `install.sh` places the tree, proves the chroot can execute a
  32-bit binary, and no longer clobbers values measured on the unit — those live
  in `rb.local.conf`, which the deploy cannot reach
  ([scripts/device/README.md](scripts/device/README.md)).

**Not yet verified, and each needs something only hardware or a person can give:**

* **Whether it is *audible*.** The write path is confirmed; nobody has yet
  listened to the FLX4's RCA outs. Requires a track on a USB stick (S6) as well.
* **Whether the FLX4 map's tables are right.** Nothing has pressed a control and
  watched `rbp` react, so every note and CC number still rests on Pioneer's
  published list — the fixture test proves the map matches its own tables and
  nothing more. This is S5.2 and it is the largest remaining unknown
  ([docs/15](docs/15-flx4-midi.md)).
* **Whether the cursor follows the mouse.** The discovery and the open are
  verified; the visible behaviour needs a hand on the mouse (S3.2/S3.3). The
  **touch** half of that row is now settled — the operator attached a USB touch
  monitor, found taps acting on the mirror of the finger (rbp reflects the
  tsc2007 x, undone in the shim), and on 2026-09-27 confirmed the fix by hand:
  the sidebar's cells, deck 2's scrubbing and the list rows all act where the
  finger is.
* **USB media has run, but only against a stick it cannot use.** Detection,
  vfat mount, chroot bind and the notification into `/tmp/udev_usb1` all
  succeeded; the watcher then reported `rbp never opened export.pdb` — correctly,
  because the stick holds only WAVs and an empty `PIONEER/USBANLZ` and has no
  rekordbox database. A stick from rekordbox's *Export to USB Device* is what
  settles the rest of S6, and it is also the precondition for playing a track and
  therefore for hearing anything.
* **Tearing, and the console over the UI** (S2.3/S2.4).
* **A monitor that is not 1280×800, and a monitor swapped mid-session.** Both are
  built: the present path's `scale` rung resamples the logical 1280×800 surface
  into whatever fb is there (aspect-fit, centred, nearest-neighbour), the driver
  upgrades `off` to it by itself when the real geometry disagrees with the
  shim's, and `display-watch.sh` restarts the player when a swap changes the
  geometry. `test_point` and `test_cursor_dev` pin the arithmetic and the
  cursor's use of it, and `tools/fit-crosscheck.sh` keeps the driver's copy of
  the fit rule equal to the shim's. **The first half has now been on the panel**:
  S10.1–S10.4 ran on 2026-09-26, and a 1280×720, a 960×600, an 800×600 and a
  1920×1080 fb each came up with the whole UI correctly scaled at 1.1–3.4 ms of a
  16.67 ms frame, while the matched 1280×800 panel was unchanged. Two things are
  still owed there: a **downscale loses hairline rules and thin glyph strokes**
  at 0.75× and 0.625× (S10.2, unfixed and a decision rather than a defect), and
  **the hot-swap rows are unstarted** — S10.0, S10.5–S10.10, the ones that need a
  `video=` edit, a cable pull and a reboot
  ([docs/13](docs/13-raspberrypi4.md#s10--the-display-drills),
  [docs/06](docs/06-display.md#a-mismatch-selects-the-rung-by-itself)).
* **The LED bridge**, on at `RB_LED_DISABLE=0` and transmitting nothing yet. The
  note numbers moved out of `rbp_led.c` into the selected map's `struct
  led_notes`, so a row that reads `-1` means "this surface has no such LED" and
  sends nothing — which is what lets the bridge run before the FLX4's
  illumination notes are known. Every FLX4 row is `-1` until it is *seen* to
  light; only the panel can confirm a number, and the probe now exists
  ([docs/15](docs/15-flx4-midi.md#the-leds)).
* **The keyboard fallback's reader.** It starts under `EVDEV_MAP`, which defaults
  to `kbd` — so the keyboard and mouse are live alongside the FLX4 with no
  configuration, and the "keyboard does nothing while the controller works"
  defect is closed. Its map and keycodes are fixture-tested under `qemu-arm`
  (`test_kbd`), and the reader itself now has a suite of its own (`test_evdev`,
  the kernel faked at the `syscall()` boundary); both have also been driven on the
  Pi through a virtual keyboard. What is still open on hardware is only the
  selector's *direction*.

Where a number in these documents is an expectation rather than a measurement,
it says so; [docs/README](docs/README.md) explains the convention.

## Limitations

* The FLX4 tables are unverified until someone dumps them — the tables, the
  dump procedure and the unmeasured values are all in
  [docs/15](docs/15-flx4-midi.md), and the ones that a dump settles are marked
  `TODO: unverified` in `scripts/shims/map_flx4.c` too.
* The booth stream is dropped (one log line): the FLX4 has two output pairs, so
  master and headphones are what there is.
* No LEDs lit yet: the bridge is on (`RB_LED_DISABLE=0`) but every FLX4 LED row
  is `-1`, because the unit's transport/pad LED notes are not published and a
  guess is not a dark LED — it is a phantom control press. Each row becomes a
  one-line edit once it is seen to light. Pad illumination, and the
  velocity→colour encoding with it, land with those numbers.
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

* **Display** — `/dev/fb0` is now `vc4drmfb`, DRM fbdev emulation over HDMI, and
  `tools/fbdump` measured it as 16 bpp RGB565 at 1280×800: the same format and
  the same geometry as `rbp`'s logical surface, rather than a portrait 32 bpp
  panel. So there is no transform to do — but the fb holds **one page** while
  the stock driver asked for three buffers, and that is what the patch's
  page-count buffer mode fixes.
* **Pointing** — there is no touchscreen. The tsc2007 protocol `rbp` reads is
  kept byte-for-byte, but what feeds it is now a discovered evdev pointer.
* **Controls** — the FLX4 sends different messages than any earlier surface,
  and puts its toggles on CCs rather than notes, so the map grew a CC-as-button
  concept and the shim stopped hardcoding a client id and a device node.
* **Audio** — `hw:1,0` with 8 channels becomes `hw:CARD=DDJFLX4,DEV=0` with 4,
  negotiated rather than forced, with the pair map and the output format in
  config. `plughw:` was the plan and was wrong: the map names hardware channels,
  and a plug device reports the wrong count for that (see [09](docs/09-audio.md)).

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
