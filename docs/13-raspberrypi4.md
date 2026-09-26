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

1. **The soft-float chroot recipe.** What this needs is not a 32-bit kernel but
   32-bit *emulation* in the kernel (`CONFIG_COMPAT`): `rbp` and the shims are
   32-bit ARM ELF and bring their own 32-bit `ld.so`, so the host's own bitness
   never enters into it — the kernel only has to be able to execute 32-bit ELF.
   Pi OS 32-bit guarantees that, and on current images it does so with a
   **64-bit kernel**: the measured unit runs `6.18.50+rpt-rpi-v8` (aarch64) and
   reports `32-bit EL0 Support` in its CPU features line. The kernel does not
   care about the userspace float ABI, so a soft-float EABI5 ARM32 binary runs
   fine on it. Every 32-bit-ARM-only assumption in the shims (32-bit
   `smem_start`/`time_t` in the fb structs, `SYS_mmap2`, `uc_mcontext.arm_*`,
   ARM32 machine-code patching) is *satisfied*, not violated. **There is no
   64-bit `rbp`**, so the chroot's binaries are 32-bit whatever the host runs —
   which is why the check that matters is executing one, not reading `uname -m`.
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
| Board | Raspberry Pi 4B (any RAM size; 1 GB is enough). The measured unit is a **Rev 1.4, 4 GB** |
| OS | **Raspberry Pi OS Lite 32-bit** — `armhf` userland, no desktop. The measured unit is the Debian 13 (trixie) generation |
| Kernel | the stock Pi kernel — **64-bit** `6.18.50+rpt-rpi-v8` on the measured unit, with `32-bit EL0 Support`; `vc4` / `vc4-kms-v3d` |
| Display | HDMI monitor or TV |
| Audio | DDJ-FLX4 over USB (class-compliant, 4 output channels) |
| Controls | DDJ-FLX4 over USB MIDI; keyboard fallback |
| Pointer | USB mouse, trackball or touchscreen (see [pointing](#pointing)) |

**Use the 32-bit image, and don't be alarmed by `uname -m` saying `aarch64`.**
`install.sh` prints both (`kernel: aarch64 6.18.50+rpt-rpi-v8; userland: 32-bit`)
and then *tests* the thing that actually matters by executing a 32-bit binary in
the chroot, rather than judging from the kernel's name. It does not warn on an
`aarch64` kernel at all, because on current Pi OS images that is the normal
arrangement rather than a fault — the 32-bit image ships the arm64 kernel. If the
chroot test fails, the kernel is missing 32-bit emulation, and reinstalling with
a different image is the fix, not a workaround.

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
| `video=HDMI-A-1:1280x800@60` | **Measured:** the sink is on `HDMI-A-1` (`connected`, `enabled`) and `HDMI-A-2` is `disconnected`, so the port name is right and there is nothing to switch. 1280×800 is `rbp`'s logical geometry, so the present path can copy 1:1. It is **not** in the connector's mode list; a `video=` mode is programmed anyway, which is exactly what this token is testing. |
| *(no depth suffix)* | The 16 bpp RGB565 default is exactly `rbp`'s native format. Forcing `-16` or `-32` hits a known `drm_fb_helper` "No compatible format found" regression. Leave it alone. |
| `fbcon=map:1` | Binds the kernel console to `fb1`, which does not exist, so **nothing** draws over the UI. Without it, kernel messages and a blinking cursor appear on top of rekordbox. |
| `console=tty3` | Moves the console to a VT nothing displays, so boot messages do not race the mode-set. |
| `consoleblank=0` | No screen blanking. `rbp` presents continuously; a blanked panel is a diagnostic red herring. |
| `vt.global_cursor_default=0` | No VT cursor at all, as belt and braces with `fbcon=map:1`. |

Then reboot and confirm with `cat /proc/cmdline` that the tokens survived.

**This has now been applied, and it worked — including on a panel whose EDID
never offered 1280×800.** The stock line gave 1280×720 (see
[which reading](#which-reading-and-what-the-modes-list-adds)): nothing had asked
for anything else. With the line above in place and a reboot, `fbdump` reports:

```
--- var (FBIOGET_VSCREENINFO) ---
  xres yres           1280 800
  xres_virtual        1280
  bits_per_pixel      16
  red     offset=11 length=5  msb_right=0
  green   offset=5  length=6  msb_right=0
  blue    offset=0  length=5  msb_right=0
--- fix (FBIOGET_FSCREENINFO) ---
  id                  vc4drmfb
  smem_len            2048000  (1.95 MiB)
  line_length         2560
--- sysfs ---
  virtual_size       1280,800
```

So the framebuffer is now **exactly `rbp`'s logical surface**: same width, same
height, same pixel format, same stride (`2560`), and `smem_len` of exactly one
frame. **The entire geometry and format half of the present-path generalization
is dead on this target** — no crop, no `scale`, no `letterbox`, no `convert`. See
[the present path](#the-present-path) for what is left (and it is not nothing).

The way it worked is worth recording, because it contradicts the obvious reading
of the modes list: **a mode the connector never advertised was accepted by the
panel.** The list tops out at 1280×720, yet 1280×800 is what is now running — a
`video=` mode is parsed and programmed whether or not it is in the list, and a
panel that is natively 1280×800 locks onto it happily. The list constrains the
kernel's *automatic* pick; it is not a statement about what the display can do.
Note also that `physical w x h` is still `110 x 60 mm`, so **the EDID is still
not being read** — the mode is forced, not negotiated.

**If a sink does reject it**, the kernel falls back to the mode it had, and
`fbdump` says so. That is not a failure, but it is not the `letterbox` trigger it
was once written up as: with no mode ≥ 1280×800 on offer there is nothing to
letterbox *into*. The rungs for that case are `crop` and a Y-only `scale` — kept
in [the present path](#the-present-path)'s table for a display that needs them,
and not needed here.

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

**This is the pre-recipe reading** — the stock kernel command line, which asked
for no mode at all. S2.1 superseded its *geometry* half (1280×720 → 1280×800) by
forcing `video=` on the command line; the format and page facts below carry over
unchanged, since neither depends on the mode that was programmed.

* **The format is a `memcpy`.** `bpp 16` with `red` 5@11, `green` 6@5, `blue`
  5@0 is RGB565 — `rbp`'s native format exactly. No pixel conversion is needed,
  so `DFB_PRESENT=convert` is **not** what fixes the display; the `565 → 8888`
  loop stays available for a 32 bpp fb that this sink does not present.
* **No stride padding.** `line_length 2560` is exactly `1280 × 2`, so a
  present-path row copy can work at full rows with no edge handling.
* **One page, and `ypanstep` lies about it.** `yres_virtual == yres` and
  `smem_len` is exactly one frame (`2560 × 720` here, `2560 × 800` after S2.1).
  `FBIOPAN_DISPLAY` cannot reach a second page, so a multi-buffered layer has
  nowhere to flips between — the driver must settle for `FRONTONLY`, and that is
  what the patch now does. `ypanstep` reads 1/1, which is the generic
  `drm_fb_helper` value rather than evidence of a second page; **`smem_len` is
  the thing that decides it**, and `ypanstep` is the thing that used to mislead
  the buffer-mode decision into three buffers. See
  [the present path](#the-present-path).
* **The geometry did not fit — and that turned out to be fixable.** This reading
  is **1280×720** against `rbp`'s logical **1280×800**, so the image was *taller*
  than the destination and a 1:1 centred letterbox was geometrically impossible:
  only a crop or a scale could reach the screen. S2.1 then forced the mode and
  the panel took 1280×800, so the fb is now `rbp`'s surface to the byte and none
  of `crop`/`scale`/`letterbox` is needed. See the present-mode table below.

### Which reading, and what the modes list adds

`physical w x h 110 x 60 mm` is the giveaway that no EDID was read — 110 mm at
1280 px is ~300 dpi, which is not a monitor. Two readings are consistent with
it, and they lead to different work:

1. The sink refused (or was never asked for) 1280×800 and the kernel fell back
   to a default mode. Then a connector that is asked for 1280×800, or for any
   mode at least 1280×800, restores the 1:1 path and **no scaler is needed**.
   *(The "never asked" half is confirmed — the cmdline is stock. The "ask for
   something bigger" remedy turns out to have nothing to ask for.)*
2. The sink really is 720p-only. Then this port needs a **scale** mode that
   [06](06-display.md)'s ladder does not have, and that is a decision, not a
   config change. *(What the mode list says — with the caveat that a list this
   generic is more likely an unread EDID than a 720p panel. A `crop` is
   cheaper than the scaler and is tried first.)*

The three commands that separate them, none of which needs the chroot:

```bash
cat /proc/cmdline                                    # did the video= token land?
for f in /sys/class/drm/card*-HDMI-A-*/{status,enabled,modes}; do
    echo "== $f"; cat "$f"; done                     # which connector, and its mode list
dmesg | grep -iE 'vc4|hdmi|edid|drm'                 # was 1280x800 tried and refused?
```

**All three have now been run.** The first two show that 1280×800 was never
*asked* for; the third shows there is nothing at least that big to ask for, which
is the outcome reading 2 predicted — with the caveat, argued at the end of this
section, that the mode list itself looks unread rather than limited.

`/proc/cmdline` is the *stock* line — `console=ttyS0,115200 console=tty1
root=PARTUUID=… rootwait` and the Pi's usual `numa=fake=2`/`vc_mem.*` tokens,
plus what the Imager appended (`cfg80211.ieee80211_regdom=US`,
`ds=nocloud;i=rpi-imager-…`). There is **no `video=` token of any kind**, so the
kernel was never asked for 1280×800, and there is no `fbcon=map:1` either: the
console is bound to `fb0` and will paint over the UI until the recipe above is
applied (the dmesg shows it happening — `Console: switching to colour frame
buffer device 160x45`, twice, as `simplefb` and then `vc4drmfb` take ownership).

`dmesg` says which mode the *firmware* chose on a connector it could not read an
EDID from: `simple-framebuffer … mode=1280x720x32, linelength=5120`, then
`[drm] Initialized vc4 0.0.0 for gpu on minor 1` and `vc4-drm gpu: [drm] fb0:
vc4drmfb frame buffer device`. Two things follow. The framebuffer that DirectFB
will open is the **`vc4drmfb` one at 16 bpp** that `fbdump` measured — not the
firmware's 32 bpp `simplefb` it replaced, which is a trap for anyone who greps
`dmesg` for the format instead of measuring the live `/dev/fb0`. And there is no
`No compatible format found` and no rejected-mode line anywhere, because nothing
was asked for. Both `vc4-hdmi-0` and `vc4-hdmi-1` are bound, which left *which*
of `HDMI-A-1`/`HDMI-A-2` the sink was on to the modes loop — that answer is
below. (For the record, since a `drmkms` fallback is the last rung of the ladder:
**vc4 is `/dev/dri/card1`** here; `card0` is `v3d`, the render node.)

**The modes loop has now been run too, and it is the answer.** Verbatim:

```
== /sys/class/drm/card1-HDMI-A-1
connected
enabled
1280x720
1280x720
1280x720
1280x720
960x600
960x540
800x600
480x320
== /sys/class/drm/card1-HDMI-A-2
disconnected
disabled
```

Three things came out of it, and the third is the one that turned out to be the
whole story.

**The connector is `HDMI-A-1`, so the recipe above names the right one.** That was
a guess (`kmsprint -m` was going to settle it); the `connected`/`enabled` pair
settled it instead, and `HDMI-A-2` is empty, so there is nothing to switch.

**There is no mode at least 1280×800 on this sink** — the largest is 1280×720,
four times over. Taken at face value that kills reading 1's remedy ("ask for
something at least as big") and makes **`letterbox` unreachable on this display**:
it was the rung for a fb *larger* than the image in both axes, and this sink's
list offers none.

**Every mode is 1280 wide or narrower, and `1280×720` is exactly 1280 wide.** So
the width never needed touching: the 1280×800 surface is the same width as the
destination, leaving the Y axis as the only axis in play. No scaler is needed for
X, ever, on this sink — and the measured fb's stride of 2560 confirms it after
the fact.

The caveat that kept the `video=` test alive was that **that list is what a
not-fully-read EDID looks like.** The four `1280x720` lines are one resolution
repeated (a mode appears once per refresh-rate/flags variant, and the file prints
the name only), and the rest — `960x600`, `960x540`, `800x600`, `480x320` — is a
generic fallback set rather than a monitor's own mode table. It agrees with the
`110 x 60 mm` physical size that gave the missing EDID away in the first place. So
the sink could still be a genuine 1280×800 panel that had never been *asked*
correctly, and there were two cheap ways to find out:

1. **Force the mode anyway.** A `video=` mode absent from the connector's list is
   still parsed and programmed — the list constrains the automatic pick, not the
   forcing. Whether the panel then locks onto 1280×800 is only knowable by
   looking, and it costs one reboot.
2. **Re-seat or swap the cable, and try `HDMI-A-2`**, then re-run the modes loop.
   A Pi 4 with a marginal HDMI cable or a passive adapter is a common cause of an
   EDID that reads as nothing while the HPD line still reports `connected` — which
   is exactly the state this connector is in. If a real mode table appeared and it
   contained 1280×800, the problem was the physical link all along.

**Test 1 was run, and it worked** — the recipe's `video=HDMI-A-1:1280x800@60` is
now the running mode, the panel locked onto timings its own mode list never
offered, and `fbdump` reports 1280×800 (the output is recorded in
[the HDMI mode](#the-hdmi-mode) section above). So the mode list was never a
statement about what the display can do; it was a statement about what the kernel
would pick on its own, from an EDID it could not read. Test 2 is now moot: the
panel takes 1280×800, so there is nothing left for a different cable to buy.

Two consequences. `letterbox`, `crop` and `scale` are all moot on this unit, and
the present path has no geometry to do — the fb is `rbp`'s surface. And the
`110 x 60 mm` reading means the **EDID is still not being read**, so the mode
remains forced rather than negotiated: if the sink is ever swapped for one that
refuses these timings, the kernel falls back and `fbdump` says so.

Also worth knowing before S2.2: sysfs reports `blank 4` (`FB_BLANK_POWERDOWN`).
That is bookkeeping rather than a state — the console *is* bound to `fb0` today
(it paints, twice, in the dmesg) and it cannot be painting into a powered-down
framebuffer. But if S2.2 draws and the screen still goes black, this is the first
thing to check: issue `FBIOBLANK` with `FB_BLANK_UNBLANK` (`0`) before blaming
the present path. `FBIOBLANK` (`0x4611`) is inside the shim's interposed fb range
and reaches the kernel through the `default:` arm, so the shim is not swallowing
it.

## The present path

`rbp` renders a logical **1280×800 RGB565** surface through DirectFB. The real
framebuffer may be a different size, a different pixel format, or both, and the
present path is what bridges that gap.

> **State of this section: written, built, and the diagnosis is in the patch.**
> The two measurements the design was gated on have both been taken, and they
> agree with `rbp` exactly — the fb is **1280×800, 16 bpp RGB565, stride 2560**,
> which is `rbp`'s logical surface to the byte. So no `convert`, no `letterbox`,
> no `crop`, no `scale`: nothing in the *table* below has to be written for this
> target, and `RB_DFB_PRESENT` can stay `off`.
>
> The other axis of the same question turned out to be the real one: the fb has
> **one page** of memory while the driver was built around three, which blocked
> the first launch outright. That is the paragraph under the table, and it is
> **fixed** — the buffer mode now comes from the page count. **S2.2 has since
> passed on the unit**: the `PRESENT:` line reads `pages=1 pan=0`, and the
> rekordbox UI renders. The full record, including the two deploy-level problems
> that had to be cleared first, is in the [bring-up table](#bring-up-order).

| `RB_DFB_PRESENT` | What it does | When |
|---|---|---|
| `off` | No blit at all: the layer surface **is** the fb page. Fastest. | **The measured Pi, and the default.** The fb *is* the logical surface — same size, format and stride — so there is nothing to correct and nothing to copy. |
| `convert` | 1:1, format-driven: 565 → 8888 when the fb's format differs, a whole-frame `memcpy` when it does not. | A 32 bpp fb — **not this port's path**, since the fb measured 16 bpp RGB565 (S1.3). It is still the knob S2.3 reaches for on this target, because on a 565 fb it is the same-format `memcpy` into a **system-memory back buffer** — the double buffering `off` cannot have. |
| `letterbox` | 1:1 copy, centred, with filled bars. | The fb is **larger** than 1280×800 in *both* axes. **Not this port's path**, and unreachable on this sink anyway — its connector offers nothing ≥ 1280×800. Kept for a monitor that does. |
| `crop` | Copies the top N rows 1:1 — a row budget on the copy that already exists, no resampling. | The fb is shorter than 1280×800 but not narrower. **Not this port's path** — the forced mode gave 1280×800. Kept for a display that caps at 720p. |
| `scale` | **Not implemented**, and it logs that and presents without a blit. | The fb is smaller than 1280×800 in either axis. **Not this port's path.** Kept for a 720p-only sink. |
| `rotate` | The previous target's portrait path (`RB_DFB_ROTATE=left` maps to it). | Not used on a Pi; refused on a 16 bpp fb, with a log line, because its loops store 4-byte pixels. |

**The blocker was pages, not pixels, and it came from a guard that tested the
wrong thing.** `fbdump` reports `ypanstep 1/1` together with
`yres_virtual == yres` and `smem_len` of exactly one frame (`2048000 == 2560 ×
800`). So there is exactly one page: `FBIOPAN_DISPLAY` can be called all day and
will never reach a second one. But `dfb_fbdev_mode_to_var()`'s protection
against a non-pannable fb tests

```c
if (shared->fix.ypanstep == 0 && shared->fix.ywrapstep == 0)
```

and **this fb reports `ypanstep == 1`** — a value `drm_fb_helper` sets
generically, meaning "panning granularity is one row", not "there is somewhere
to pan to". So the guard did not fire, and because `primaryInitLayer()` forced
`config->buffermode = DLBM_TRIPLE`, the patched `dfb_fbdev_mode_to_var()`
multiplied `yres_virtual` by 3 → 2400 rows. `dfb_fbdev_test_mode()` then asked

```c
need_mem = 2560 × 2400 = 6,144,000      vs      smem_len = 2,048,000
```

and returned `DFB_LIMITEXCEEDED`, which fails `primaryTestRegion()` and takes the
primary surface with it — a first launch that fails with a framebuffer-memory
shortfall rather than the torn-but-working UI the `off` rung was written up to
give.

**That is now fixed in the patch, and S2.2 has since confirmed it on the unit.**
The buffer
mode is decided from the *page count* — the number of whole frames the fb holds
— instead of from `ypanstep`, and the same test now gates the four places that
ask the question (`primaryInitLayer()`, `dfb_fbdev_mode_to_var()`,
`dfb_fbdev_test_mode()`, and the pan/blit index). On this fb that yields a single
`FRONTONLY` buffer, `yres_virtual` stays 800, `need_mem` equals `smem_len`
exactly, the test passes, and because a single-buffered primary surface **is**
the visible page, `rbp` draws straight into the framebuffer and every flip
becomes a no-op pan. No scratch surface and no per-frame copy. Full account in
[06 — Display](06-display.md#the-present-path).

**What to run it with, and what to read first.** Launch as the tree stands, with
`crashcatch.so`, and `debug=FBDev/Mode` in `usr/etc/directfbrc` — spelled
exactly like that, `=` and slash included: the config parser only splits an
option at `=`, and the domain's name is `FBDev/Mode`, so the obvious
`debug FBDev_Mode` is rejected as an invalid option and then silently ignored
([06](06-display.md#the-present-path) has the two halves). Two lines are the
evidence:

```
PRESENT: mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)
```

from `/tmp/dfbdig9.log` — `pages=1 pan=0` is the page-count fix arriving at the
right answer, and it is the line to check before anything else. Then the UI
appearing is the outcome. If it does not, `FBDev/Mode`'s `not enough framebuffer
memory` line names both numbers, and that is the whole diagnosis.

**Off is not a rung here, and it is not a downgrade either.** The plan called a
system-memory surface plus a blit **mandatory**, reasoning that forced triple
buffering has no pages to pan between — which is true, but the cheaper answer is
not to triple-buffer at all. The price is tearing, because the render thread
draws onto a visible surface; if that turns out to be unacceptable, the next step
is `RB_DFB_PRESENT=convert` — which is a system-memory back buffer with a
per-frame copy of the whole frame in one `memcpy` (~2 MB at 60 fps), and it is
the `sbpp == dbpp` fast path earning its place. Bring the tearing decision back
after seeing S2.2, not before.

**The pacer stays load-bearing either way.** `RB_PAN_PACER_MS` (default
`16.666666`, `0` disables it) is what paces `FBIOPAN_DISPLAY`. `vc4`'s
`drm_fbdev` returns from a pan **without waiting for vblank** — the same
situation that let `rbp`'s RT-priority-98 render thread spin a core at 100% on
the previous target — and with a single buffer the pan is a no-op that still
goes through the shim, so the pacer is the *only* thing bounding that thread.
It is not a nicety here; without it the render thread spins a core.

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
| S0.3 | `scripts/build-chroot.sh` | tarball has `directfbrc`, `TouchCalib_*`, all shims, `rb.conf`, and its `usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` sha256 matches `work/dfb`'s — a stale module is otherwise invisible until S2.2 | — |
| S1.1 | Pi OS Lite 32-bit boots | **measured:** the userland is 32-bit — `getconf LONG_BIT` = **32**, `dpkg --print-architecture` = **`armhf`** — and the kernel can run it: `32-bit EL0 Support` in the CPU features line, then S1.5's chroot `echo ok` proves it end to end. `uname -m` is **not** the check: the measured unit's kernel is aarch64, and it reports `aarch64` *inside* the 32-bit chroot too. (`file /bin/sh` is not part of this either — `/bin/sh` is a symlink to `dash`, so it prints `symbolic link to dash` and stops; `file -L` would follow it) | a kernel with no 32-bit emulation — the chroot's own binaries then cannot run, and **that** is what the S1.5 exec test catches |
| S1.2 | device nodes | `/dev/fb0`, `/dev/snd/seq`, `/dev/input/event*` all present | missing `seq` → `modprobe snd-seq`; symptom is "sequencer setup failed" + zero controls |
| S1.3 | `tools/fbdump` | **recorded, pre-recipe: 1280×720, 16 bpp RGB565, `line_length` 2560, one page** — the unedited output is below. Superseded by S2.1's re-run at 1280×800, and the two together are what settles the present path | format is a `memcpy` (`565 == 565`); a 1280×720 fb cannot hold a 1280×800 image 1:1 |
| S1.4 | `kmsprint -m`; `aplay -L`, `/proc/asound/cards`; `aseqdump -l` | active connector; card id `DDJFLX4`; port name `DDJ-FLX4 MIDI 1`; playback formats/rates/channels. The MIDI half is already known: the unit enumerates as `2b73:0045` (AlphaTheta DDJ-FLX4) and `snd-usb-audio` probes it — `usb 1-1.2: Quirk or no altset; falling back to MIDI 1.0` | card id differs → fix `rb.conf` |
| S1.5 | chroot sanity: `chroot … /bin/sh -c 'echo ok'`; run `edb_streamd` | `ok`; daemon stays alive | exec bits, missing binds |
| S2.1 | the connector query (`/proc/cmdline`, the `modes` files, `dmesg`), then the `cmdline.txt` recipe + reboot | **answered, and in the best way.** First half: the sink is on `HDMI-A-1` (`connected`/`enabled`, `HDMI-A-2` `disconnected`) and its mode list tops out at `1280x720` (four times) plus `960x600`, `960x540`, `800x600`, `480x320` — no mode ≥ 1280×800, **so `letterbox` is unreachable on this display.** Second half: the mode was forced anyway and **the panel took it** — `fbdump` after the reboot reads **1280×800, 16 bpp RGB565, `line_length` 2560, `smem_len` 2048000, one page**, i.e. `rbp`'s logical surface exactly. `letterbox`/`crop`/`scale`/`convert` are all moot here | it did not reject it, so the old "fall back to crop/scale" branch is dead on this unit; it stays in the present path's table for a sink that caps at 720p |
| S2.2 | launch with `crashcatch.so`, then verify against the **generated** `usr/etc/directfbrc` — no hand-added lines. `debug=FBDev/Mode` is only needed to see the arithmetic, and is *not* in the shipped file | **PASSED.** The `PRESENT:` line reads `mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)` — `pages=1 pan=0` is the page-count decision having been taken from the real geometry, and `mode=off` says the layer surface *is* the fb page, so no blit happens at all. The UI was then confirmed by capturing `/dev/fb0` and decoding it as RGB565: both decks, HOT CUE A–H on each, BEAT FX/DELAY at 120.0 BPM, QUANTIZE, TRACK/REMAIN/TEMPO and the waveform lanes all render. Before the page fix, the fb's one page against a forced `DLBM_TRIPLE` gave `need_mem` `2560×2400` = 6,144,000 > `smem_len` 2,048,000 and no UI. See [the present path](#the-present-path) | no UI with the `PRESENT:` line reading `pages=3 pan=1` → the page arithmetic is still reading `ypanstep`, not the fb; no UI with `pages=1` → the page decision is right and the blocker is downstream of it, and the `FBDev_Mode` line names what it is. **Two deploy-level blockers sat in front of this milestone and neither is Pi-specific** — both are now fixed in the tree, and both are worth knowing because each mimics a code bug: (1) `module-dir` missing from `directfbrc`, so *no DirectFB module loaded* and rbp segfaulted on a NULL `IDirectFB*` ([06](06-display.md#required-directfbrc), [12](12-troubleshooting.md)); (2) AppleDouble `._*` files from a macOS build host in the module directories, which DirectFB tries to dlopen ([12](12-troubleshooting.md)) |
| S2.3 | watch the running image for tearing, then re-launch with `RB_DFB_PRESENT=convert` | same image, no tearing, and the pacer holding the render thread to ~60 fps. `convert` on a 565 fb is a 1:1 whole-frame `memcpy` into a system-memory back buffer, which is the double-buffering `off` cannot have | tearing that survives `convert` → the blit is not reaching the visible page; the pacer's absence shows as a pinned core instead, at 100% on the render thread |
| S2.4 | idle 10 min, then type on the console | no text or cursor ever over the UI | `fbcon=map:1` not applied (`cat /proc/cmdline`) |
| S3.1 | `tools/evdevdump --list` | names, caps and absinfo for the mouse and keyboard. The measured unit has a **keyboard only** (`Dell KB216 Wired Keyboard`, with its Consumer/System Control interfaces) — no mouse has been attached, so the relative-pointer path is unmeasured *and* unexercised, and a mouse (or trackball/touchscreen) has to be plugged in before this step means anything | set `RB_POINT_DEV`; if no pointer is found, `evdevdump --list` says so and only the keyboard map is available |
| S3.2 | `POINT_DEBUG=1`, click the four corners | emitted logical coords match the UI's reaction | mirrored/transposed → `RB_POINT_INVERT_X` / `RB_POINT_SWAP_XY`, else the `TouchCalib` affine |
| S3.3 | drag-scroll the browser, tap PLAY, tap a playlist row | all react | fast clicks swallowed → raise `RB_POINT_MIN_DWELL_MS` |
| S4.1 | `speaker-test -D plughw:CARD=DDJFLX4,DEV=0 -c 4 -r 48000` | sound, and channel identification pair by pair | channels not 1:1 → logical count ≠ hw count |
| S4.2 | launch `rbp`, `/tmp/audioshim.log`, load and play | negotiated channels/format and the pair map logged; master audible on the FLX4's RCA out, cue on its headphone jack | **silence with a healthy log ⇒ the `0x3C665C` audio patch is missing**; distortion ⇒ sign-extension |
| S4.3 | PFL/cue buttons, cue mix and level | cue bus changes, no combing | summing master+cue instead of routing the phone stream |
| S5.1 | `aseqdump -p <client>:0` while pressing everything, piped into `tools/aseqdump2dump.py --stats --revs <N> -o flx4.dump` | **run — and it converted the map's biggest guess into a measurement.** The inventory is in [15](15-flx4-midi.md); the relative controls use the 0x40-centred convention (platter CC 34 vinyl-on / 35 vinyl-off / 41 `+SHIFT`, jog ring CC 33), and one counted turn gives **720 counts/revolution**. `map_flx4.c`'s `jog_ppr` is now 720: the inherited 128 was the *JP21's* platter, which made every turn 5.6× too fast and pinned it to the 8 rev/s clamp. Still `TODO: unverified`, and a smaller set than before: the pitch fader's polarity, the pad base+pad encoding and its SHIFT pairing, the FX knob targets, BEAT SYNC long-press and 4 BEAT/EXIT | nothing → `snd-seq` / USB |
| S5.2 | `MIDI_DUMP=… RB_MIDI_MAP=flx4`, press each control | dump and `/tmp/knobshim.log` show the expected keycodes | wrong channel/note assumptions — that is the dump's purpose |
| S5.3 | `make -C scripts/shims test` under `qemu-arm` | both fixtures → expected keycodes (`test_midi`, `test_kbd`) | off-by-one on a CC pair or threshold |
| S5.4 | `RB_MIDI_MAP=kbd`, then `1` to load and `space` to play; `↑`/`↓`, `Esc`, right-click | keys drive rbp, and the wheel/right-button do too | wrong keycodes; **the selector's direction is the one thing only hardware settles** (see 08) |
| S5.5 | pad LEDs (`LED_VERBOSE=1`) | pads mirror | the colour table is a known TODO |
| S6.1 | plug a stick; `usb-watch.sh status` | sd found, mounted, chroot-bound, rbp shows the drive | wrong ancestor match, or `udisks2` grabbed it; or missing PM NULL guards → crash on insert |
| S7 | review these docs against the bring-up log | — | — |

## Risks specific to this target

1. ~~**Does `vc4`'s `drm_fbdev` accept DirectFB's mode-set and present sequence
   at all?**~~ **Answered, and it does** — `fbdump` after the recipe reads
   1280×800×16 exactly as asked, so the kernel accepted both the forced
   `video=` mode and DirectFB's subsequent mode-set. The multi-day
   `drmkms`/hand-written-DRM fallback is off the table. What this risk turned
   into was the **page** question — the fb holds one frame while the driver
   asked for three buffers — which is risk 3, and which the patch answers;
   **S2.2** has since confirmed it on the unit.
2. **The real fb bpp (16 vs 32).** **Answered: 16**, RGB565 with
   `red@11/5, green@5/6, blue@0/5` — which is `rbp`'s own format, so the
   conversion half of the present path is unnecessary on this target. The
   conflicting reports were about `simplefb` (32 bpp, visible in `dmesg`) versus
   `vc4drmfb` (16 bpp, what `/dev/fb0` actually is): measuring the live node
   rather than grepping the log is what separates them.
3. **Does the driver present at all on a fb with one page?** This was the top
   risk, and it was the one real blocker the measurements turned up — but as a
   *deterministic* failure, not an unknown: `primaryInitLayer()` forced
   `DLBM_TRIPLE` while the no-pan guard keyed on `ypanstep`, which this fb
   reports as 1, so `yres_virtual` tripled and the primary region test failed on
   memory (`need_mem` `2560×2400` = 6,144,000 > `smem_len` 2,048,000) — no UI,
   for reasons that had nothing to do with the display's geometry or format.
   **Fixed in the patch**: the buffer mode is now decided from the page count,
   read at open from the real geometry, which yields one `FRONTONLY` buffer on
   this fb and needs no transform of any kind. S2.2 has confirmed the fix on the
   unit; the first evidence to read is the `PRESENT:` line. See
   [the present path](#the-present-path).
4. **The pointer's axis algebra on a non-rotated display** — measured, not
   derived, and currently blocked on there being a pointer to measure: the unit
   has a keyboard and no mouse. See above.
5. **Every DDJ-FLX4 MIDI detail** — see [15 — DDJ-FLX4 MIDI](15-flx4-midi.md).
   None of it can block the port: the keyboard map plus the fixture tests keep
   everything else moving.
6. **Which of the 68 patches are load-bearing on the Pi.** Each one's
   justification carries over, but only `0x3C665C` (the audio `scanForDevices`
   patch) is provably necessary in advance, because the Pi 4 fails the same
   `board_is_rev` check. The rest are re-tested per S6 in
   [12 — troubleshooting](12-troubleshooting.md).
