# 13 — Raspberry Pi 4B target

This is the current target: `rbp` (the XDJ-RX3 standalone rekordbox player) on a
**Raspberry Pi 4B**, rendering to an **HDMI monitor**, playing through a
**DDJ-FLX4**, driven by a mouse or a touchscreen. The Denon SC Live 4 is gone;
the scripts, the shim defaults and this document treat the Pi as the only
target.

If you have not built anything yet, read [05 — chroot](05-chroot.md) for the
soft-float chroot and [03 — port plan](03-port-plan.md) for what changed and
what did not. This document is the Pi-specific half.

## Why the port works at all

Three things carry over from the previous target nearly untouched, and they are
the expensive parts:

1. **The soft-float chroot recipe.** Pi OS 32-bit is `armv7l`/armhf with a
   hard-float kernel — the same arrangement the SC Live 4 had. The kernel does
   not care about the userspace float ABI, so a soft-float EABI5 ARM32 binary
   runs fine on it. Every 32-bit-ARM-only assumption in the shims (32-bit
   `smem_start`/`time_t` in the fb structs, `SYS_mmap2`, `uc_mcontext.arm_*`,
   ARM32 machine-code patching) is *satisfied*, not violated.
   **A 64-bit userland would break all of them, and there is no 64-bit `rbp`.**
2. **The `rbp` patch table.** `tools/patch-rbp/rbp_patch.py`'s 68 words plus the
   two-word `getPcController()` fix. Each patch's justification — "there is no
   Pioneer panel MCU, no power-manager MCU, no i.MX6 board check here" — is
   equally true on a Pi.
3. **The shims' `rbp` ABI.** `IUiObjManager` @`0x02685f2c`, `MixerEngine`
   @`0x011493c0`, `PlayEngine` @`0x011497d0`, the `LedManager+0x30` walk, the
   `MonoLvMeter::getLedValue` prologue at `0x2d07a8`, the direct UI globals —
   these are properties of the *binary*, not of the device, so they stay valid
   verbatim. This is the single biggest reason the port is feasible.

## Hardware and image

| | |
|---|---|
| Board | Raspberry Pi 4B (any RAM size; 1 GB is enough) |
| OS | **Raspberry Pi OS Lite 32-bit (Bookworm)** — `armhf`, no desktop |
| Kernel | the stock Pi kernel, 32-bit; `vc4` / `vc4-kms-v3d` |
| Display | HDMI monitor or TV |
| Audio | DDJ-FLX4 over USB (class-compliant, 4 output channels) |
| Controls | DDJ-FLX4 over USB MIDI; keyboard fallback |
| Pointer | USB mouse, trackball or touchscreen (see [pointing](#pointing)) |

**Use the 32-bit image.** `install.sh` warns rather than refuses on an
`aarch64` kernel, because a 64-bit kernel with 32-bit emulation enabled can
still run the chroot — the installer tests that directly by executing a 32-bit
binary in it. But Pi OS Lite 32-bit is the tested configuration, and if the
chroot test fails, reinstalling with the 32-bit image is the fix, not a
workaround.

On Bookworm the display driver is enabled by default (`dtoverlay=vc4-kms-v3d`).
`/dev/fb0` is `vc4drmfb` — DRM fbdev emulation — and appears once the driver
binds. `install.sh` checks for it and says what to look for in `dmesg`
(`fb0: vc4drmfb frame buffer device`) if it is missing.

## Deploy layout

`scripts/build-chroot.sh` produces `work/rblive4-pi4.tgz`, which unpacks to the
deploy root (`/opt/rblive4` by default):

```
/opt/rblive4/
├── rb.conf          every machine-specific constant — for the scripts AND the shims
├── lib.sh           shared: rb.conf loading, the /proc process lookup
├── install.sh       one-time (and idempotent) deploy
├── fix-dev.sh       bind mounts + device stubs; re-run after every reboot
├── start-rb.sh      the launcher
├── usb-watch.sh     media hotplug
├── log/             rbp.log, edb_streamd.log, usbwatch.log
├── media/usb1/sda1  where a stick is mounted (host side)
└── rbx3-run/        the soft-float ARM32 chroot: player, shims, DirectFB
```

Deploy:

```bash
# on the workstation
scripts/build-chroot.sh                 # -> work/rblive4-pi4.tgz
scp work/rblive4-pi4.tgz scripts/device/* pi@<host>:/tmp/

# on the Pi
sudo sh /tmp/install.sh /tmp/rblive4-pi4.tgz
sh /opt/rblive4/start-rb.sh
```

`rb.conf` travels inside the tarball, so the device never has a config that
disagrees with its tree. **It is the only place to change a machine-specific
value.** Each variable's comment names the component that reads it, which tells
you whether a change needs a rebuild: `RB_AUDIO_DEV` is read by a shim at
runtime, `RB_AUDIO_MAP` likewise, but `RB_CHROOT_MEDIA` is baked into the
patched binary and is not configurable.

The budget is comfortable but not unlimited: the extracted chroot is roughly
150–250 MB, so an 8 GB card is plenty and a 4 GB card is tight once `rbp`, the
DirectFB stack and the media mount are in.

## The HDMI mode

**On Bookworm the `framebuffer_*` options in `config.txt` are ignored.** The
mode comes from `video=` on the kernel command line. Append to the single
existing line in `/boot/firmware/cmdline.txt`:

```
video=HDMI-A-1:1280x800@60 fbcon=map:1 console=tty3 consoleblank=0 vt.global_cursor_default=0
```

Each token earns its place:

| Token | Why |
|---|---|
| `video=HDMI-A-1:1280x800@60` | `HDMI-A-1` is the Pi 4's first HDMI port (`HDMI-A-2` is the second — check with `kmsprint -m`). 1280×800 is `rbp`'s logical geometry, so the present path can copy 1:1. |
| *(no depth suffix)* | The 16 bpp RGB565 default is exactly `rbp`'s native format. Forcing `-16` or `-32` hits a known `drm_fb_helper` "No compatible format found" regression. Leave it alone. |
| `fbcon=map:1` | Binds the kernel console to `fb1`, which does not exist, so **nothing** draws over the UI. Without it, kernel messages and a blinking cursor appear on top of rekordbox. |
| `console=tty3` | Moves the console to a VT nothing displays, so boot messages do not race the mode-set. |
| `consoleblank=0` | No screen blanking. `rbp` presents continuously; a blanked panel is a diagnostic red herring. |
| `vt.global_cursor_default=0` | No VT cursor at all, as belt and braces with `fbcon=map:1`. |

Then reboot and confirm with `cat /proc/cmdline` that the tokens survived.

**If the sink rejects 1280×800** the kernel falls back to the monitor's native
mode. That is not a failure — it is the trigger for `letterbox`, and `fbdump`
will tell you the geometry you got. It is why the present path is configurable
rather than hardcoded.

## Measuring the framebuffer: `tools/fbdump`

The patched fbdev driver decides what to do from fields that differ per SoC and
per kernel. Guessing wastes a build cycle; measuring takes one command:

```bash
sudo apt-get install -y gcc make        # Pi OS Lite has no compiler by default
gcc -O2 -static -o fbdump tools/fbdump.c
./fbdump
```

It is read-only — it never issues a mode-set — and it prints the raw
`FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` fields, the sysfs mirror, and a
plain-language verdict: whether the format is a straight `memcpy` or needs a
565→8888 convert, whether the fb is pannable, and whether the geometry matches
`rbp`'s logical 1280×800.

<!-- S1.3: the real output, unedited. Monitor model: not recorded yet. -->

Recorded on a remote Pi 4 running Pi OS Lite, `tools/fbdump` on `/dev/fb0`,
verbatim:

```
--- var (FBIOGET_VSCREENINFO) ---
  xres yres           1280 720
  xres_virtual        1280
  xoffset yoffset     0 0
  bits_per_pixel      16
  grayscale           0
  red     offset=11 length=5  msb_right=0
  green   offset=5  length=6  msb_right=0
  blue    offset=0  length=5  msb_right=0
  transp  offset=0  length=0  msb_right=0
  physical w x h      110 x 60 mm
  pixclock            0
  margins l/r u/l     0/0 0/0
  hsync vsync len     0 0
  sync                0
  vmode               0
  rotate              0
  nonstd activate     0 0

--- fix (FBIOGET_FSCREENINFO) ---
  id                  vc4drmfb
  smem_start          0x00000000
  smem_len            1843200  (1.76 MiB)
  type type_aux       0 0
  visual              2
  line_length         2560
  mmio_len accel      0 0

--- sysfs (/sys/class/graphics/fb0) ---
  name               vc4drmfb
  virtual_size       1280,720
  stride             2560
  bits_per_pixel     16
  rotate             0
  blank              4
```

### What it settles

* **The format is a `memcpy`.** `bpp 16` with `red` 5@11, `green` 6@5, `blue`
  5@0 is RGB565 — `rbp`'s native format exactly. No pixel conversion is needed,
  so `DFB_PRESENT=convert` is **not** this port's path; the `565 → 8888` loop
  stays available for a 32 bpp fb that this sink does not present.
* **No stride padding.** `line_length 2560` is exactly `1280 × 2`, so a
  present-path row copy can work at full rows with no edge handling.
* **One page, so no panning.** `yres_virtual == yres` (720/720) and
  `smem_len` is exactly one frame (`2560 × 720`). `FBIOPAN_DISPLAY` cannot reach
  a second page here, which makes the "draw in `rot_surface`, then blit"
  arrangement **mandatory** on this target rather than an optimisation: the
  forced `DLBM_TRIPLE` triple buffering has no pages to pan between. `ypanstep`
  reads 1/1, which is the generic `drm_fb_helper` value and not evidence of a
  second page — `smem_len` is the thing that decides it.
* **The geometry does not fit, and this is the new problem.** The fb is
  **1280×720**; the surface `rbp` renders is a logical **1280×800**. The image
  is *taller* than the destination (800 > 720), so the 1:1 centred letterbox
  that [06](06-display.md)'s table assumed is **geometrically impossible**: only
  a crop or a scale reaches the screen. See the present-mode table below.

### What it does not settle

`physical w x h 110 x 60 mm` is the giveaway that no EDID was read — 110 mm at
1280 px is ~300 dpi, which is not a monitor. Two readings are consistent with
it, and they lead to different work:

1. The sink refused (or was never asked for) 1280×800 and the kernel fell back
   to a default mode. Then a connector that is asked for 1280×800, or for any
   mode at least 1280×800, restores the 1:1 path and **no scaler is needed**.
2. The sink really is 720p-only. Then this port needs a **scale** mode that
   [06](06-display.md)'s ladder does not have, and that is a decision, not a
   config change.

The three commands that separate them, neither of which needs the chroot:

```bash
cat /proc/cmdline                                    # did the video= token land?
for f in /sys/class/drm/card*-HDMI-A-*/{status,enabled,modes}; do
    echo "== $f"; cat "$f"; done                     # which connector, and its mode list
dmesg | grep -iE 'vc4|hdmi|edid|drm'                 # was 1280x800 tried and refused?
```

Also worth knowing before S2.2: sysfs reports `blank 4`
(`FB_BLANK_POWERDOWN`). That may only be bookkeeping, because with `fbcon=map:1`
no console is bound to `fb0` and nothing has issued an unblank — but if S2.2
draws and the screen stays black, this is the first thing to check. `FBIOBLANK`
(`0x4611`) is inside the shim's interposed fb range and reaches the kernel
through the `default:` arm, so the shim is not swallowing it.

## The present path

`rbp` renders a logical **1280×800 RGB565** surface through DirectFB. The real
framebuffer may be a different size, a different pixel format, or both, and the
present path is what bridges that gap.

> **State of this section.** The *table below is the design*, and `RB_DFB_PRESENT`
> is already the config knob for it — but **the driver does not read it yet.** What
> `tools/build-directfb/directfb-full.diff` installs today is the previous
> target's rotation path (`rot_deg`), generalized to four present modes only once
> S1.3 and S2.1 have recorded what the Pi's framebuffer actually is. Until then
> treat `RB_DFB_PRESENT` as reserved, and expect the first `fbdump` to be the
> thing that decides whether the Pi needs `convert` at all.

| `RB_DFB_PRESENT` | What it will do | When |
|---|---|---|
| `off` | Writes straight into the fb, no conversion. Fastest, tears. | **First bring-up.** It proves the mode-set works with nothing else in the way. |
| `convert` | Pixel-format conversion only (565 → 8888). | The fb is 32 bpp. **Not this port's path**: the fb measured 16 bpp RGB565 (S1.3). Kept for a sink that presents 32 bpp. |
| `letterbox` | 1:1 copy, centred, with filled bars. | The fb is **larger** than 1280×800 in *both* axes, so the image fits inside it. Any mode ≥ 1280×800 works. |
| `scale` | **Not implemented.** Uniform downscale of the 1280×800 surface to fit, with bars in the other axis. | The fb is **smaller** than 1280×800 in either axis — **the measured case: the sink gave 1280×720**. A 1:1 copy cannot fit there and `letterbox` cannot be satisfied. Needs a decision (see below). |
| `rotate` | The previous target's portrait path (`RB_DFB_ROTATE=left` maps to it). | Not used on a Pi. Kept so the driver has one code path, not two. |
| *(no present mode)* | `rot_deg = 0`, i.e. what the driver does today: a plain write into the fb at the logical geometry. | Where the Pi starts. It is also where it can stay **if the mode is set to ≥ 1280×800**, which the format result says needs no conversion and no bars. |

So the honest position after S1.3 is *not* "the Pi may need no present work".
It is: **the format needs none, the geometry does, unless the mode changes.**
The fb is 16 bpp RGB565, which is `rbp`'s native format — so the whole
conversion half of the generalization is unnecessary on this sink. What is
missing is a mode that fits, and the cheap way to get one is to ask the
connector for it rather than to write a scaler.

**The scale mode is a decision, not a config change.** A 0.9× uniform
downscale of 1280×800 is 1152×720: no distortion, bars at the sides, and a real
per-frame scaler in software (`~0.9 Mpx`, ~830k lookups per frame at 60 fps).
That is a new code path in the same driver, and the alternative — the `drmkms`
system, where the `vc4` plane scaler does it in hardware — is the multi-day
fallback the port plan named in advance. Both wait on the connector question
below: if the sink offers any mode at least 1280×800, neither is needed.

The ladder is `off` → `convert` → `letterbox`, and it exists because the first
question — "does `vc4`'s `drm_fbdev` accept DirectFB's mode-set and present
sequence at all?" — is the one that can block everything downstream. No display
means no way to test pointing, audio or controls. `off` answers it in minutes.

`RB_PAN_PACER_MS` (default `16.666666`) is the frame pacer. `vc4`'s
`drm_fbdev` returns from `FBIOPAN_DISPLAY` **without waiting for vblank** — the
same situation that let `rbp`'s RT-priority-98 render thread spin a core at 100%
on the previous target. Without a pacer, that happens here too. `0` disables it.

The shim's "lie" is load-bearing and must not be "cleaned up": it reports a
logical 1280×800×16 RGB565 fb to DirectFB, so DirectFB's `current_var` is the
logical geometry. The *real* geometry lives only in `shared->real_var`, read
with a raw `syscall(SYS_ioctl)` that bypasses the shim, and only the present
path uses it. See [06 — display](06-display.md).

`directfbrc` stays as it is, including `no-hardware`:

```
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
```

The build is `--with-gfxdrivers=none --disable-devmem`, so `no-hardware` is the
verified configuration. It also keeps DirectFB away from the DRM/GPU path
entirely — the display is the fbdev driver's business, not DirectFB's.

## Pointing

There is no touchscreen on an HDMI monitor unless you plug one in, so this is a
general **pointing** path, not a touch path. The shim discovers the device by
capability rather than by node name, because on a Pi the pointer is whatever is
plugged into USB:

```bash
sudo apt-get install -y gcc make
gcc -O2 -static -o evdevdump tools/evdevdump.c
./evdevdump --list
```

`--list` prints every input device's name, capabilities and absolute-axis
ranges, and ends with a ready-to-paste configuration line —
`POINT_KIND=abs|rel POINT_DEV=/dev/input/eventN` — for each pointer it finds,
preferring an absolute device (touchscreen/tablet) over a relative one (mouse).

Set those in `rb.conf` as `RB_POINT_KIND` / `RB_POINT_DEV`, or leave
`RB_POINT_KIND=auto` and let the shim choose. A relative device is accumulated
into an absolute pointer (cursor starts centred, `RB_POINT_MOUSE_SPEED` scales
the accumulation, left button is down/up), and `RB_POINT_MIN_DWELL_MS` (default
45) holds a very short tap open long enough for `rbp`'s debounce and UI thread
to see it — without it, fast clicks get swallowed.

**The axis algebra is measured, not derived.** The repo's two older shim
generations disagree about whether logical x is `py` or `1279-py`, so the
procedure is fixed: set `RB_POINT_DEBUG=1`, click the four corners of the UI,
read the emitted `(down, raw_x, raw_y, lx, ly)` lines in
`/tmp/knobshim.log`, and correct with `RB_POINT_SWAP_XY` /
`RB_POINT_INVERT_X` / `RB_POINT_INVERT_Y`. Between those flags and
`root/settings/TouchCalib_*.dat` — a six-line file whose `offX`/`scaleX` can
express any affine transform — every 2D case is reachable without a rebuild.

## Audio

The DDJ-FLX4 is USB class-compliant with **4 output channels: master on 1-2,
headphones on 3-4.** `rb.conf`'s defaults describe exactly that:

```
RB_AUDIO_DEV=plughw:CARD=DDJFLX4,DEV=0
RB_AUDIO_CHANNELS=auto
RB_AUDIO_MAP=master=0,1;headphones=2,3;booth=-
```

`rbp`'s side of the contract does not change: it believes it has three
2-channel S24_LE 44.1 kHz outputs (master, phones, booth) opened in a fixed
order, plus a dummy capture. The shim presents them on the real device.

**`plughw:`, not `hw:`.** ALSA's `plug` chain does S24_LE ↔ S24_3LE packing and
44.1 → 48 kHz conversion for free, and `rbp` only ever speaks S24_LE at 44.1.
The `hw` plugin accepts `CARD=`/`DEV=`, so the device string goes straight to
real `snd_pcm_open` on the **host's** ALSA — no card-enumeration code is needed.
Verify the card id with `aplay -L` / `cat /proc/asound/cards`; if it differs,
change `RB_AUDIO_DEV`, not the shim.

**Request exactly as many logical channels as the hardware has.** The route
plugin is 1:1 only while logical ≤ hw. An 8→4 downmix would fold FL/FR into the
second pair and put garbage on the headphone output. `AUDIO_CHANNELS=auto`
reads the real maximum and clamps to `rbp`'s 8.

The headphones stream is routed **straight through** — `rbp`'s `HeadPhone`
already mixes cue and master time-aligned, so summing the separate ALSA devices
would comb-filter. Booth is dropped with one log line (the FLX4 has no separate
booth pair). Stereo-only devices degrade to master-only with cue dropped, never
silently mixed. See [09 — audio](09-audio.md) for the stream model.

## Controls

`RB_MIDI_MAP` selects the DDJ-FLX4 map by default; `RB_MIDI_MAP=kbd` is the
keyboard fallback, which needs nothing plugged in and exists so the display and
audio can be tested on their own; `RB_MIDI_MAP=jp21` selects the previous
target's map.

**The FLX4 map is written and fixture-tested, and it is this target's default**
(`flx4` in `rb.conf`, and the fallback in source). Its tables are the one place
this port is still *unverified* rather than *unrun*: every number in them comes
from Pioneer's published DDJ-FLX4 MIDI message list, because **nothing in this
repository has ever been run with an FLX4 attached**, and the fixture test can
only prove the map matches its own tables. Expect the first session to correct
some of them — the channel conversion and the pad base+pad encoding are the two
whose failure mode is a whole section doing nothing. The dump procedure, the
tables and the explicitly-unverified parts are in
[15 — DDJ-FLX4 MIDI](15-flx4-midi.md). The LED bridge is a further step behind:
`RB_LED_DISABLE=1` is set for this target because `rbp_led.c` still carries the
previous target's notes.

**The keyboard fallback is written** — a map with a key table, plus the evdev
reader that feeds it, with the keycodes it produces pinned by ~300 fixture
checks in `make -C scripts/shims test`. It has been cross-compiled and run
against synthetic events only; **it has never been run with a real keyboard or
mouse on the Pi**, and its list direction for `↑`/`↓` is the one thing about it
that cannot be settled off hardware. Select it with `RB_MIDI_MAP=kbd` (the
`kbd` entry in `rb.conf`); the key list is in
[08 — Controls](08-controls.md#the-keyboard-map-rb_midi_mapkbd).

Two things about the Pi are worth knowing here:

* **The sequencer device must exist.** `/dev/snd/seq` is not autoloaded on every
  boot, and its absence is the classic silent "controls do nothing" failure:
  `rbp` starts, the UI draws, and no input ever arrives. `install.sh` checks for
  it and runs `modprobe snd-seq`; `fix-dev.sh` does the same on every launch.
  With `RB_MIDI_MAP=kbd` this is no longer fatal (the keyboard is an evdev
  device, not a sequencer client) — the shim logs the failure loudly and keeps
  the keyboard working, but there is still no MIDI input and no LED/meter output
  until the module is loaded, so the check is still the fix.
* **The FLX4 has no level meters.** `RB_LED_VU=0` is the setting for that, and it
  does more than silence output: it skips `install_meter_hook()` entirely, so
  `rbp`'s machine code is never patched for a meter that does not exist — which
  is why the hook is skipped rather than left installed with nowhere to write.
  It does *not* skip the cue: `rbp` has no PFL keycode, so the shim asserts
  `me_set_master_cue(1)` and stereo cue mode at startup on every target, waiting
  for `rbp`'s mixer on its own — and the unit's own MASTER CUE button then
  toggles the same engine state from the map. Channel CUE goes through
  `me_set_cue(ch)`.

## USB media

Unchanged from the previous target except for discovery: the watcher iterates
`/sys/block/sd*` and matches *any* ancestor `*/usb[0-9]/*` rather than the
SC Live 4's fixed `/usb1/`. A removable device is *preferred* over a fixed one
rather than required — a USB SSD or a 2.5" enclosure reports `removable=0` while
being perfectly good rekordbox media. The bind path inside the chroot is still
`/media/usb1/sda1` — **that exact string is baked into the patched binary** — so
it is not configurable.

`udisks2` is masked by `start-rb.sh`: if it grabs the stick first, the kernel
hands the mount to it and `usb-watch.sh` ends up reading a path that has already
been given away. See [10 — USB](10-usb.md).

## Launcher

`start-rb.sh` is the entry point and is **manual** — autostart as a systemd unit
is out of scope for this pass. In order it: stops the services in
`RB_STOP_SERVICES` (a display manager drawing over `fb0` is the Pi's version of
the problem the old target's `engine.service` posed) and masks
`RB_MASK_SERVICES`; disables `getty@tty1`; kills stale processes found by
scanning `/proc/*/cmdline`; runs `fix-dev.sh`; re-copies `rbp` and the shims;
clears the DeviceSQL lock files; starts `edb_streamd` through the explicit
loader; starts `rbp`; waits for rbp to open its USB FIFO; starts the watcher;
then sleeps while rbp lives. See
[11 — runtime launcher](11-runtime-launcher.md).

## Bring-up order

Ordered so a failure cannot be masked by the next subsystem. **S1–S3 come
before most code**, because they convert the biggest unknowns into facts.

| # | Step | Check | Likely failure |
|---|---|---|---|
| S0.1 | `make -C scripts/shims RX3=… check` | only `GLIBC_2.4/2.7`, no `!! HARD-FLOAT` | a new file pulls `GLIBC_2.17` |
| S0.2 | stock `rbp` → `rbp-audio` (md5 `3706c68f…`) → +2-word stage; pin both md5s | md5 matches | wrong stock binary (the patcher aborts by design) |
| S0.3 | `scripts/build-chroot.sh` | tarball has `directfbrc`, `TouchCalib_*`, all shims, `rb.conf` | — |
| S1.1 | Pi OS Lite 32-bit boots | `uname -m` = `armv7l`, `dpkg --print-architecture` = `armhf` | a 64-bit image — breaks every shim |
| S1.2 | device nodes | `/dev/fb0`, `/dev/snd/seq`, `/dev/input/event*` all present | missing `seq` → `modprobe snd-seq`; symptom is "sequencer setup failed" + zero controls |
| S1.3 | `tools/fbdump` | **recorded: 1280×720, 16 bpp RGB565, one page** — the unedited output and what it settles are below | format is a `memcpy`; **1280×720 cannot hold a 1280×800 image 1:1** |
| S1.4 | `kmsprint -m`; `aplay -L`, `/proc/asound/cards`; `aseqdump -l` | active connector; card id `DDJFLX4`; port name `DDJ-FLX4 MIDI 1`; playback formats/rates/channels | card id differs → fix `rb.conf` |
| S1.5 | chroot sanity: `chroot … /bin/sh -c 'echo ok'`; run `edb_streamd` | `ok`; daemon stays alive | exec bits, missing binds |
| S2.1 | the `cmdline.txt` recipe + reboot, then the connector query (`/proc/cmdline`, the `modes` files, `dmesg`) | **a mode ≥ 1280×800 on the active connector**, or an explicit decision to write the `scale` mode | a 720p-only sink means the 1:1 path is impossible — that is the 0.9× scale decision, not a failure |
| S2.2 | launch as the tree stands, with `crashcatch.so` | **the rekordbox UI appears** | **the highest-risk step** — see risks |
| S2.3 | only after the present generalization lands: `DFB_PRESENT=off`, then `convert`, then `letterbox` if the mode was rejected | same image, less tearing | convert too slow at 60 fps |
| S2.4 | idle 10 min, then type on the console | no text or cursor ever over the UI | `fbcon=map:1` not applied (`cat /proc/cmdline`) |
| S3.1 | `tools/evdevdump --list` | names, caps and absinfo for the mouse and keyboard | set `RB_POINT_DEV` |
| S3.2 | `POINT_DEBUG=1`, click the four corners | emitted logical coords match the UI's reaction | mirrored/transposed → `RB_POINT_INVERT_X` / `RB_POINT_SWAP_XY`, else the `TouchCalib` affine |
| S3.3 | drag-scroll the browser, tap PLAY, tap a playlist row | all react | fast clicks swallowed → raise `RB_POINT_MIN_DWELL_MS` |
| S4.1 | `speaker-test -D plughw:CARD=DDJFLX4,DEV=0 -c 4 -r 48000` | sound, and channel identification pair by pair | channels not 1:1 → logical count ≠ hw count |
| S4.2 | launch `rbp`, `/tmp/audioshim.log`, load and play | negotiated channels/format and the pair map logged; master audible on the FLX4's RCA out, cue on its headphone jack | **silence with a healthy log ⇒ the `0x3C665C` audio patch is missing**; distortion ⇒ sign-extension |
| S4.3 | PFL/cue buttons, cue mix and level | cue bus changes, no combing | summing master+cue instead of routing the phone stream |
| S5.1 | `aseqdump -p <client>:0` while pressing everything, piped into `tools/aseqdump2dump.py --stats --revs <N> -o flx4.dump` | the inventory in the order the controls were pressed, which convention each relative control uses, counts/revolution for the platter, and a replayable fixture | nothing → `snd-seq` / USB |
| S5.2 | `MIDI_DUMP=… RB_MIDI_MAP=flx4`, press each control | dump and `/tmp/knobshim.log` show the expected keycodes | wrong channel/note assumptions — that is the dump's purpose |
| S5.3 | `make -C scripts/shims test` under `qemu-arm` | both fixtures → expected keycodes (`test_midi`, `test_kbd`) | off-by-one on a CC pair or threshold |
| S5.4 | `RB_MIDI_MAP=kbd`, then `1` to load and `space` to play; `↑`/`↓`, `Esc`, right-click | keys drive rbp, and the wheel/right-button do too | wrong keycodes; **the selector's direction is the one thing only hardware settles** (see 08) |
| S5.5 | pad LEDs (`LED_VERBOSE=1`) | pads mirror | the colour table is a known TODO |
| S6.1 | plug a stick; `usb-watch.sh status` | sd found, mounted, chroot-bound, rbp shows the drive | wrong ancestor match, or `udisks2` grabbed it; or missing PM NULL guards → crash on insert |
| S7 | review these docs against the bring-up log | — | — |

## Risks specific to this target

1. **Does `vc4`'s `drm_fbdev` accept DirectFB's mode-set and present sequence at
   all?** This is the one step that can block everything downstream, and the
   fallback — DirectFB's `drmkms` system, or a hand-written DRM present path —
   is a multi-day project, not a config change. The Rockchip driver *panicked*
   here; `vc4` is a much more standard `drm_fb_helper` client, so the
   expectation is "yes" — but that is an expectation, not a measurement.
   **S1.3 + S2.1 + one launch (S2.2) cost under an hour, need no new code, and
   settle it before any display code is written.**
2. **The real fb bpp (16 vs 32).** High confidence it is 16, but
   `drm_fb_helper`'s single-probe rework and `vc4`'s `preferred_depth` have
   produced conflicting reports across kernels. `fbdump` settles it; the design
   works either way.
3. **The pointer's axis algebra on a non-rotated display** — measured, not
   derived. See above.
4. **Every DDJ-FLX4 MIDI detail** — see [15 — DDJ-FLX4 MIDI](15-flx4-midi.md).
   None of it can block the port: the keyboard map plus the fixture tests keep
   everything else moving.
5. **Which of the 68 patches are load-bearing on the Pi.** Each one's
   justification carries over, but only `0x3C665C` (the audio `scanForDevices`
   patch) is provably necessary in advance, because the Pi 4 fails the same
   `board_is_rev` check. The rest are re-tested per S6 in
   [12 — troubleshooting](12-troubleshooting.md).
