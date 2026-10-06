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
| Audio | DDJ-FLX4 over USB (class-compliant, 4 output channels: master 1-2, headphones 3-4; `S16_LE`/`S24_3LE` at 44100 or 48000, measured — see S1.4) |
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
├── install.sh       one-time (and idempotent) deploy; `install.sh doctor` checks instead
├── doctor.sh        check a deployed unit and report — changes NOTHING ([12](12-troubleshooting.md))
├── fix-dev.sh       bind mounts + device stubs; re-run after every reboot
├── start-rb.sh      the launcher
├── usb-watch.sh     media hotplug
├── display-watch.sh display hotplug: restarts the unit when the fb geometry changes
├── log/             rbp.log, edb_streamd.log, usbwatch.log, displaywatch.log
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

### Never `rm -rf` anything under the chroot's `/dev`, `/proc`, `/sys` or `/tmp`

devtmpfs is **one superblock** for the whole machine. The chroot's `/dev` is a
bind mount of the host's, so it is not a copy of it: a file created through the
bind is created in the *host's* `/dev`, and a file deleted through it is deleted
there. (Same for `/proc`, `/sys` and `/tmp`, whose binds exist for the same
reason.) The dangerous shape is an `umount` whose failure is ignored, followed by
a cleanup of the directory it was meant to clear — when the unmount fails, the
cleanup runs *through* the still-mounted bind and takes the host's tree with it.

That is not hypothetical: on **2026-09-27** `fix-dev.sh` had exactly that pair,
the `umount` failed because rbp held the chroot's `/dev` open (which is the
normal state on a live unit, since `install.sh` ends by running `fix-dev.sh`),
and the `rm -rf` emptied the host's device tree. What that looked like is worth
knowing because none of it points at `/dev`:

* **every SSH login came up non-interactive and promptless** — `/dev/ptmx` and
  the devpts instance's own `ptmx` inode were gone, so sshd could not allocate a
  pty, and bash without a tty prints no prompt, echoes nothing and keeps no
  history. It reads as "something broke my profile".
* `/dev/null` was **re-created as a regular file** by the next `>/dev/null` that
  ran (a redirect creates its target), which is a disk-filling hazard rather than
  a missing-node one.
* `/dev/fb0`, `/dev/snd/seq`, `/dev/dri/card0`, `/dev/input/event*` and
  `/dev/sda` vanished, but the *running* rbp never noticed: it holds file
  descriptors opened before the wipe. The unit looks healthy until something
  restarts the player, which then fails into `Restart=always`.

Recovery is split. The pty comes back by hand — `mknod /dev/null c 1 3`,
`chmod 666`, then a `mount -t devpts devpts /dev/pts -o …,ptmxmode=000` to make
the kernel create a fresh devpts `ptmx`, then `mknod /dev/ptmx c 5 2`. **The
kernel-created nodes come back only from a reboot:** `udevadm trigger
--action=add` recreates udev's directories (`/dev/block`, `/dev/disk`,
`/dev/input/by-id`, …) but never device nodes, because on devtmpfs the kernel
creates those, not udev. `docs/12` carries the same thing as a symptom row.

The fix in the tree is the shape to keep: the `umount` is **checked** and its
failure is survivable, `mkdir -p` re-creates the mountpoint, the bind goes on
regardless, and **nothing is ever deleted** — the directory's contents do not
need clearing, because the bind covers whatever is there and a stale mount is
shadowed rather than fought.


The budget is comfortable but not unlimited: the extracted chroot is roughly
150–250 MB, so an 8 GB card is plenty and a 4 GB card is tight once `rbp`, the
DirectFB stack and the media mount are in.

## The HDMI mode

**On Bookworm the `framebuffer_*` options in `config.txt` are ignored.** The
mode comes from `video=` on the kernel command line. Append to the single
existing line in `/boot/firmware/cmdline.txt`:

```
video=HDMI-A-1:1280x800@60 video=HDMI-A-2:1280x800@60 fbcon=map:1 console=tty3 consoleblank=0 vt.global_cursor_default=0
```

Each token earns its place:

| Token | Why |
|---|---|
| `video=HDMI-A-1:1280x800@60` | **Measured:** the sink is on `HDMI-A-1` (`connected`, `enabled`) and `HDMI-A-2` is `disconnected`, so the port name is right for the panel that is actually attached. 1280×800 is `rbp`'s logical geometry, so the present path can copy 1:1. It is **not** in the connector's mode list; a `video=` mode is programmed anyway, which is exactly what this token is testing. |
| `video=HDMI-A-2:1280x800@60` | The same mode on the *other* micro-HDMI port, so a monitor plugged into either one comes up at the logical geometry instead of at whatever the kernel picks. **It does not make moving the cable mid-session seamless**, and the token is easy to over-read: `rbp` opens `RB_FB_DEV` (`/dev/fb0`), so a port move still needs `RB_FB_DEV=/dev/fb1` — and if nothing was attached to `HDMI-A-2` at boot there is **no `fb1` at all**, so a reboot after the move is the reliable path. A `video=` mode is also re-added on every hotplug re-probe and validated against the CRTC/encoder rather than the sink's EDID — **but "re-added" is not "re-applied", and S10.5 measured the difference on 2026-09-26: on a same-mode replug the re-probe *refused* this very token** (`vc4-drm gpu: [drm] User-defined mode not supported: "1280x800": …`), **and the picture survived because the CRTC was already running that mode with the fbdev left untouched — not because the mode was re-imposed.** So the token carries a *boot*; a hotplug is carried by `drm_fbdev` not tearing the fb down. Which of the two carries a monitor whose own mode list does not contain 1280×800 is what S10.5's second half and S10.8 measure; it is not assumed here. |
| *(no depth suffix)* | The 16 bpp RGB565 default is exactly `rbp`'s native format. Forcing `-16` or `-32` hits a known `drm_fb_helper` "No compatible format found" regression. Leave it alone. |
| `fbcon=map:1` | Binds the kernel console to `fb1`, so **nothing** draws over the UI. Without it, kernel messages and a blinking cursor appear on top of rekordbox. With the `HDMI-A-2` token above there is now a *real* `fb1` when a second monitor is attached, so the console lands on that monitor instead of nowhere — the intent, not a regression; the second panel shows console text and the player's panel is untouched. |
| `console=tty3` | Moves the console to a VT nothing displays, so boot messages do not race the mode-set. |
| `consoleblank=0` | No screen blanking. `rbp` presents continuously; a blanked panel is a diagnostic red herring. |
| `vt.global_cursor_default=0` | No VT cursor at all, as belt and braces with `fbcon=map:1`. |

Then reboot and confirm with `cat /proc/cmdline` that the tokens survived.

**You no longer have to type this.** `install.sh`'s boot trim ensures the tokens
above — everything except `video=HDMI-A-2`, which is deliberately left out and
named as a gap by `sh /opt/rblive4/boot-trim.sh status bootfiles` (see
[Boot time](#boot-time) and S10.8 below). The block is kept because it is what
the trim implements, and because a target being brought up by hand still needs
it.

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
is dead on this panel** — no crop, no `scale`, no `letterbox`, no `convert`. (It
is a statement about this sink and not about the code: `scale` is what a *different*
panel needs, and a monitor swapped in for this one is the case it exists for —
S10.) See [the present path](#the-present-path) for what is left (and it is not
nothing).

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

**S2.1 answered this, and not with either reading:** the sink accepts modes it
never advertised, so the mode list above was never an EDID and the panel takes
1280×800 when it is asked. The `scale` mode that reading 2 called for was built
anyway — a *different* monitor is the case that keeps it alive — and it is S10
that runs it on glass. What reading 2 got wrong is the word "decision": with one
rule in one helper and an automatic upgrade in front of it, a mismatched panel is
now a config question about a frame budget, not a new code path.

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
> which is `rbp`'s logical surface to the byte. So on **this** panel no rung in
> the *table* below is reached at all — no `convert`, no `letterbox`, no `crop`,
> no `scale` — and `RB_DFB_PRESENT` stays `off`. That is a statement about this
> sink, not about the code: `scale` is now written, because the question the
> table was written to answer is what the *next* monitor needs, and a panel that
> is not 1280×800 is the case it exists for. **S10.0–S10.3** are the rows that
> put it on glass.
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
| `off` | No blit at all: the layer surface **is** the fb page. Fastest. | **The measured Pi, and the default.** The fb *is* the logical surface — same size, format and stride — so there is nothing to correct and nothing to copy. On a fb that *disagrees* with the logical geometry this is a broken picture rather than a slow one, so the driver now upgrades it to `scale` by itself; see [06](06-display.md#a-mismatch-selects-the-rung-by-itself). |
| `convert` | 1:1, format-driven: 565 → 8888 when the fb's format differs, a whole-frame `memcpy` when it does not. | A 32 bpp fb — **not this port's path**, since the fb measured 16 bpp RGB565 (S1.3). It is still the knob S2.3 reaches for on this target, because on a 565 fb it is the same-format `memcpy` into a **system-memory back buffer** — the double buffering `off` cannot have. Also the automatic choice when the geometry matches and only the format differs. |
| `letterbox` | 1:1 copy, centred, with filled bars. | The fb is **larger** than 1280×800 in *both* axes. **Not this port's path**, and unreachable on this sink anyway — its connector offers nothing ≥ 1280×800. Kept for a monitor that does. |
| `crop` | Copies the top N rows 1:1 — a row budget on the copy that already exists, no resampling. | The fb is shorter than 1280×800 but not narrower. **Not this port's path** — the forced mode gave 1280×800. Kept for a display that caps at 720p. It has a **column** budget too, so an explicit `crop` on a narrower panel takes what fits instead of copying nothing. |
| `scale` | Uniform nearest-neighbour resample to fit the fb — aspect-fit, centred, bars in the unused axis; an exact fit falls through to the 1:1 body. **The automatic choice when the fb differs from 1280×800 in either axis.** | The fb is a different size, whether smaller (a 720p-only sink) or larger (any panel that is not 1280×800 — the case this port will meet next). Written and artefact-verified but **unrun on a mismatched panel**: S10.0–S10.3 are the rows that settle it, and `RB_DFB_PRESENT_AUTO=0` holds the driver to what it is told while they are run. See [06](06-display.md#a-mismatch-selects-the-rung-by-itself). |
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

**The pacer is measured-dead on this configuration, and the frame boundary is
somewhere else.** `RB_PAN_PACER_MS` (default `16.666666`, `0` disables it) paces
`FBIOPAN_DISPLAY`, and `vc4`'s `drm_fbdev` does return from a pan without waiting
for vblank — the same situation that let `rbp`'s RT-priority-98 render thread spin
a core at 100% on the previous target. What this paragraph used to claim is that
with a single buffer the pan is a no-op that still goes through the shim, so the
pacer is the *only* thing bounding that thread. Measured on the unit on
2026-09-29, that is not true here: with `RB_DFB_PRESENT=off`, **`FBIOPAN_DISPLAY`
(`0x4606`, DirectFB's own spelling) is never issued at all.** Every distinct
framebuffer ioctl that reached the shim over a whole session was logged at its
first occurrence — `0x4600` `FBIOGET_VSCREENINFO`, `0x4602` `FBIOGET_FSCREENINFO`,
`0x4601` `FBIOPUT_VSCREENINFO`, `0x4604` `FBIOGETCMAP`, `0x4605` `FBIOPUTCMAP` and
`0x40044620` `FBIO_WAITFORVSYNC` — and the pan is not among them. It is not the
panstep early-out either (`xpanstep`/`ypanstep` are both 1 here, so that test
passes); the call simply does not happen. So the pacer governs the present modes
that *do* pan and nothing else, and the boundary that really paces rbp's render
thread is the **vsync wait, at a measured 57.1/s**.

Two consequences. First, the pacer is kept — it is what the frame-rate fix on the
previous target rests on — but nothing new may hang off it. Second, anything that
needs "immediately after rbp has finished a frame" hangs off the `FBIO_WAITFORVSYNC`
case instead, which is what the top menu does (see
[07 — touch](07-touch.md#the-swipe-down-top-menu)); that switch now has a second
caller, and its comment is where the reasoning lives.

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
`/tmp/pointsrc.log`, and correct with `RB_POINT_SWAP_XY` /
`RB_POINT_INVERT_X` / `RB_POINT_INVERT_Y`. Between those flags and
`root/settings/TouchCalib_*.dat` — a six-line file whose `offX`/`scaleX` can
express any affine transform — every 2D case is reachable without a rebuild.

### S3.2 pointing

The arrow `fbshim.so` composites is the answer to "the pointer is invisible", and
both halves of that were measured on the unit rather than reasoned about. The
first version drew the arrow once and then asked each tick whether its tip pixel
was still intact — a question with no trustworthy answer (the tip is a black
outline cell and much of rbp's UI is black) — and it redrew at 16 ms, matching
rbp's own frame rate, which is the rate that guarantees an arrow gets erased. The
damage measurements behind the corrected `RB_POINT_CURSOR_MS=0.5` default are in
[07 — Touch / pointing](07-touch.md#the-arrow-on-the-screen-point_cursor).

Verification, and the reason it is stated as a measurement:

```bash
# while rbp is up, with a mouse plugged in
/tmp/fbfind 20 100000        # scans /dev/fb0 for the exact 12x19 glyph
```

`arrow present in 20/20 samples (100%)`, at `(640,400)` — the pointer's initial
centre, matching the shim's `cursor: first paint at (640,400)`. With
`RB_POINT_CURSOR=0` the same scan reports `0/5 samples` and exit 2, which is what
makes the 20/20 a measurement of the arrow rather than of something in rbp's UI.

**What it costs**, measured as an A/B on an idle UI: rbp's own CPU is 48.4% of one
core with `RB_POINT_CURSOR=0` and 56.3% with the default 0.5 ms period — about
**8% of one core** for ~2000 wakeups a second. On a four-core Pi 4 with rbp's
render thread already burning half a core this is affordable, and raising
`RB_POINT_CURSOR_MS` to `1` halves it at the cost of a few percent of visibility.
The alternative that removes the cost *and* the flicker is a second DRM plane on
`card1` — **an overlay plane, not a cursor plane**, since `card0` is `v3d` and
rbp's own picture is `card1`'s primary plane (id 91, `BOUND crtc=102 fb=722`).
Being attempted with `work/drmplane.c`; the probe and the inventory are in
[07](07-touch.md#the-arrow-on-the-screen-point_cursor).

## Audio

The DDJ-FLX4 is USB class-compliant with **4 output channels: master on 1-2,
headphones on 3-4.** `rb.conf`'s defaults describe exactly that:

```
RB_AUDIO_DEV=hw:CARD=DDJFLX4,DEV=0
RB_AUDIO_CHANNELS=auto
RB_AUDIO_MAP=master=0,1;headphones=2,3;booth=-
RB_AUDIO_FMT=s24_3le
```

`rbp`'s side of the contract does not change: it believes it has three
2-channel S24_LE 44.1 kHz outputs (master, phones, booth) opened in a fixed
order, plus a dummy capture. The shim presents them on the real device.

**`hw:`, not `plughw:` — and that is a measurement, not a preference.** The map
`AUDIO_MAP` names *hardware* channel indices, so the shim has to learn the card's
real channel count, and a plug device cannot tell it:

| device | `snd_pcm_hw_params_get_channels_max()` |
|---|---|
| `hw:CARD=DDJFLX4,DEV=0` | **4** |
| `plughw:CARD=DDJFLX4,DEV=0` | **10000** |

With `plughw` the shim negotiated 8 logical channels — its own ceiling — onto a
4-channel card, and every write failed `-EINVAL`, which the log could only show as
`written=-22`. `hw:` is also why `AUDIO_FMT` is set: with no plug chain nothing
converts for us, so the shim's own `s24pack()` does the packing and the format must
be one the card accepts. The card takes **44100 natively** — no resampling is
needed either. The measured hw params are in S1.4 below.

The `hw` plugin accepts `CARD=`/`DEV=`, so the device string goes straight to real
`snd_pcm_open` on the **host's** ALSA — no card-enumeration code is needed. Verify
the card id with `aplay -L` / `cat /proc/asound/cards`; if it differs, change
`RB_AUDIO_DEV`, not the shim.

**Request exactly as many logical channels as the hardware has.** On a `hw:` device
there is no plugin in between, so the shim's frame is the card's frame and the
map's indices mean what they say; asking for more is a bare `-EINVAL` at
`hw_params`. (On the previous target, where a `route` plugin *was* in between, the
same mistake instead made it downmix and put garbage on the second pair — a failure
that sounded like a working card and a broken mix.) `AUDIO_CHANNELS=auto` reads the
real maximum and clamps to `rbp`'s 8.

The headphones stream is routed **straight through** — `rbp`'s `HeadPhone`
already mixes cue and master time-aligned, so summing the separate ALSA devices
would comb-filter. Booth is dropped with one log line (the FLX4 has no separate
booth pair). Stereo-only devices degrade to master-only with cue dropped, never
silently mixed. See [09 — audio](09-audio.md) for the stream model.

## Controls

Two independent selections, one per event source: `RB_MIDI_MAP` picks the map that
reads the ALSA sequencer — the DDJ-FLX4 map by default, `kbd` for no controller at
all, `jp21` for the previous target's — and `RB_EVDEV_MAP` picks the map that reads
`/dev/input/event*`, which defaults to `kbd`. So a keyboard and a mouse are live
**alongside** the controller with no configuration; `RB_EVDEV_MAP=none` is how to
ask for a controller with no keyboard. See
[08 — Controls](08-controls.md#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone).

**The FLX4 map is written and fixture-tested, and it is this target's default**
(`flx4` in `rb.conf`, and the fallback in source). Its tables are the one place
this port is *unverified* rather than *unrun*: the shim subscribes to the unit's
own port and an `aseqdump` capture has been taken from it (S5.1), but **no control
has yet been pressed and watched through to rbp**, so every note and CC number
still comes from Pioneer's published DDJ-FLX4 MIDI message list, and the fixture
test can only prove the map matches its own tables. Expect the first session to
correct some of them — the channel conversion and the pad base+pad encoding are
the two whose failure mode is a whole section doing nothing. The dump procedure,
the tables and the explicitly-unverified parts are in
[15 — DDJ-FLX4 MIDI](15-flx4-midi.md). The LED bridge is a further step behind:
it is **on** (`RB_LED_DISABLE=0`) but every FLX4 note row is `-1`, which sends
nothing — so the bridge runs and the panel stays dark. The notes moved out of
`rbp_led.c` into the map's `struct led_notes`, so the previous target's numbers
can no longer be driven at this one by accident; what remains is to *see* each
one light, which only the panel can settle (S9 below).

**The keyboard fallback is written, and it now runs on this target.** A map with a
key table, plus the evdev reader that feeds it, with the keycodes it produces
pinned by 365 fixture checks in `make -C scripts/shims test` and the *reader*
pinned by its own suite (`test_evdev`, 53 checks over five scenarios, the kernel
faked at the `syscall()` boundary). It is selected by `EVDEV_MAP`, which defaults
to `kbd`, so it is live whether or not a controller is — the earlier note here that
it "only starts under `RB_MIDI_MAP=kbd`" described the defect that made the
keyboard dead while the FLX4 worked, not a property of the map. It has been driven
on the unit through a virtual keyboard. Its list direction for `↑`/`↓` is still
the one thing about it that cannot be settled off hardware. The key list is in
[08 — Controls](08-controls.md#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone).

The **pointer** reader is a different story, and it has run: it is always active,
and `/tmp/pointsrc.log` records it opening `/dev/input/event7` as
`'Logitech G203 Prodigy Gaming Mouse'`. Discovery and the open are therefore
verified on hardware; what is not is the visible behaviour.

Two things about the Pi are worth knowing here:

* **The sequencer device must exist.** `/dev/snd/seq` is not autoloaded on every
  boot, and its absence is the classic silent "controls do nothing" failure:
  `rbp` starts, the UI draws, and no input ever arrives. `install.sh` checks for
  it and runs `modprobe snd-seq`; `fix-dev.sh` does the same on every launch.
  With `EVDEV_MAP=kbd` (the default) this is no longer fatal (the keyboard is an
  evdev device, not a sequencer client) — the shim logs the failure loudly and
  keeps the keyboard working, but there is still no MIDI input and no LED/meter
  output until the module is loaded, so the check is still the fix. That is true
  **with a controller selected as well**, which is the difference the two
  selections made: a missing sequencer no longer costs you the keyboard.
* **The FLX4's meter is a different kind of meter, and the VU bridge drives it.**
  `RB_LED_VU` is a manual OFF switch now rather than a statement about the
  target. Pioneer's list documents a **CH LEVEL METER on CC 2** — one host-driven
  level per deck channel (0/1), **pre-fader** like rbp's own — where `rbp`'s
  meter and the previous target's are an 11-segment **bitmask**, so which kind a
  panel wants is read from its own map's meter row (`meter_enc`), not from the
  flag. The FLX4's is live as of 2026-10-01 and **confirmed on the panel by the
  operator's own eye**, with `VU_TEST=1` stepping both channel meters — a
  successful `midi_cc()` proves only that the message left the shim, so the eye
  is the evidence, and the sweep loops so it can be read whenever the operator
  looks ([15](15-flx4-midi.md#the-leds)). What the surface has *not* got is a
  **master** meter: the list gives no separate master address, so the row stays
  absent rather than invented. A map that declares no meter row installs no hook
  and sends no traffic whatever the flag says — the earlier claim that the unit
  "has no level meters", and the conclusion drawn from it that a value ramp could
  not be fed to this bridge, were both wrong.
  The flag does *not* skip the cue: `rbp` has no PFL keycode, so the shim asserts
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
| S0.2 | stock `rbp` `4f2efcfc…` → `rbp-audio` `3706c68f…` → `rbp-nopc` `18a64bc4…`; pin all three | **measured 2026-10-06:** re-running both patchers over the local stock copy reproduces `18a64bc4…` **byte-for-byte**, and the unit's `root/pdj/rbp` is that same file — so the deployed player is provably *stock + 68 words + 2 words* and nothing else. `doctor.sh` now reports which of the three by name, and flags a deploy-root `rbp-audio` (stage 1) that `start-rb.sh` would silently copy over it | wrong stock binary (both patchers validate the stock word at every address and abort by design) |
| S0.3 | `scripts/build-chroot.sh` | tarball has `directfbrc`, `TouchCalib_*`, all shims, `rb.conf`, and its `usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` sha256 matches `work/dfb`'s — a stale module is otherwise invisible until S2.2 | — |
| S1.1 | Pi OS Lite 32-bit boots | **measured:** the userland is 32-bit — `getconf LONG_BIT` = **32**, `dpkg --print-architecture` = **`armhf`** — and the kernel can run it: `32-bit EL0 Support` in the CPU features line, then S1.5's chroot `echo ok` proves it end to end. `uname -m` is **not** the check: the measured unit's kernel is aarch64, and it reports `aarch64` *inside* the 32-bit chroot too. (`file /bin/sh` is not part of this either — `/bin/sh` is a symlink to `dash`, so it prints `symbolic link to dash` and stops; `file -L` would follow it) | a kernel with no 32-bit emulation — the chroot's own binaries then cannot run, and **that** is what the S1.5 exec test catches |
| S1.2 | device nodes | `/dev/fb0`, `/dev/snd/seq`, `/dev/input/event*` all present | missing `seq` → `modprobe snd-seq`; symptom is "sequencer setup failed" + zero controls |
| S1.3 | `tools/fbdump` | **recorded, pre-recipe: 1280×720, 16 bpp RGB565, `line_length` 2560, one page** — the unedited output is below. Superseded by S2.1's re-run at 1280×800, and the two together are what settles the present path | format is a `memcpy` (`565 == 565`); a 1280×720 fb cannot hold a 1280×800 image 1:1 |
| S1.4 | `kmsprint -m`; `aplay -L`, `/proc/asound/cards`; `aseqdump -l`; `aplay --dump-hw-params -D hw:CARD=DDJFLX4,DEV=0` | **measured.** Connector `HDMI-A-1`. Card id `DDJFLX4`; port name `DDJ-FLX4 MIDI 1`; the unit enumerates as `2b73:0045` (AlphaTheta DDJ-FLX4) and `snd-usb-audio` probes it (`usb 1-1.2: Quirk or no altset; falling back to MIDI 1.0`). The card's own hw params, which settle the audio design: `FORMAT S16_LE S24_3LE`, `SAMPLE_BITS [16 24]`, `FRAME_BITS [64 96]`, `CHANNELS 4`, `RATE [44100 48000]`, `PERIOD_SIZE [45 48000]`, `BUFFER_SIZE [90 96000]` — **no `S32_LE`**, which is why the shim's hand-rolled `SND_PCM_FORMAT_S24_3LE` being wrong (10 = `S32_LE`) could never have worked. Use `hw:` for this query: a plug device answers with what the plug layer will accept, not with what the card has. **The HDMI side, measured the same way (2026-09-27):** full KMS (`dtoverlay=vc4-kms-v3d`) gives `snd_soc_hdmi_codec` **two more cards** — card 2 `vc4hdmi0` (connector HDMI-A-1, connected) and card 3 `vc4hdmi1` (HDMI-A-2, disconnected) — and each offers exactly **one** format, `IEC958_SUBFRAME_LE`, 2 channels, 32000–48000 Hz. There is **no mixer and no volume control** on them (only `HDMI Jack`, `ELD`, `IEC958 Playback Default/Mask`, `Playback Channel Map`), and nothing in the chroot's libasound will convert to that format — which is what the HDMI mirror's `hw:` + `subframe_le` default is made of ([09](09-audio.md#the-hdmi-mirror)) | card id differs → fix `rb.conf`. Both formats it offers are accepted by `RB_AUDIO_FMT`; the default `s24_3le` is the one that needs no conversion |
| S1.5 | chroot sanity: `chroot … /bin/sh -c 'echo ok'`; run `edb_streamd` | `ok`; daemon stays alive | exec bits, missing binds |
| S2.1 | the connector query (`/proc/cmdline`, the `modes` files, `dmesg`), then the `cmdline.txt` recipe + reboot | **answered, and in the best way.** First half: the sink is on `HDMI-A-1` (`connected`/`enabled`, `HDMI-A-2` `disconnected`) and its mode list tops out at `1280x720` (four times) plus `960x600`, `960x540`, `800x600`, `480x320` — no mode ≥ 1280×800, **so `letterbox` is unreachable on this display.** Second half: the mode was forced anyway and **the panel took it** — `fbdump` after the reboot reads **1280×800, 16 bpp RGB565, `line_length` 2560, `smem_len` 2048000, one page**, i.e. `rbp`'s logical surface exactly. `letterbox`/`crop`/`scale`/`convert` are all moot here | it did not reject it, so the old "fall back to crop/scale" branch is dead on this unit; it stays in the present path's table for a sink that caps at 720p |
| S2.2 | launch with `crashcatch.so`, then verify against the **generated** `usr/etc/directfbrc` — no hand-added lines. `debug=FBDev/Mode` is only needed to see the arithmetic, and is *not* in the shipped file | **PASSED.** The `PRESENT:` line reads `mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)` — `pages=1 pan=0` is the page-count decision having been taken from the real geometry, and `mode=off` says the layer surface *is* the fb page, so no blit happens at all. The UI was then confirmed by capturing `/dev/fb0` and decoding it as RGB565: both decks, HOT CUE A–H on each, BEAT FX/DELAY at 120.0 BPM, QUANTIZE, TRACK/REMAIN/TEMPO and the waveform lanes all render. Before the page fix, the fb's one page against a forced `DLBM_TRIPLE` gave `need_mem` `2560×2400` = 6,144,000 > `smem_len` 2,048,000 and no UI. See [the present path](#the-present-path) | no UI with the `PRESENT:` line reading `pages=3 pan=1` → the page arithmetic is still reading `ypanstep`, not the fb; no UI with `pages=1` → the page decision is right and the blocker is downstream of it, and the `FBDev_Mode` line names what it is. **Two deploy-level blockers sat in front of this milestone and neither is Pi-specific** — both are now fixed in the tree, and both are worth knowing because each mimics a code bug: (1) `module-dir` missing from `directfbrc`, so *no DirectFB module loaded* and rbp segfaulted on a NULL `IDirectFB*` ([06](06-display.md#required-directfbrc), [12](12-troubleshooting.md)); (2) AppleDouble `._*` files from a macOS build host in the module directories, which DirectFB tries to dlopen ([12](12-troubleshooting.md)) |
| S2.3 | watch the running image for tearing, then re-launch with `RB_DFB_PRESENT=convert` | same image, no tearing, and the pacer holding the render thread to ~60 fps. `convert` on a 565 fb is a 1:1 whole-frame `memcpy` into a system-memory back buffer, which is the double-buffering `off` cannot have | tearing that survives `convert` → the blit is not reaching the visible page; the pacer's absence shows as a pinned core instead, at 100% on the render thread |
| S2.4 | idle 10 min, then type on the console | no text or cursor ever over the UI | `fbcon=map:1` not applied (`cat /proc/cmdline`) |
| S3.1 | `tools/evdevdump --list` | **run twice, and the second run is the one that matters.** On 2026-09-26 it found `Logitech G203 Prodigy Gaming Mouse` — a mouse, so `RB_POINT_KIND=rel` was pinned — in `rb.local.conf`, not `rb.conf`, because a value set in `rb.conf` is reverted to `auto` by the next deploy (see [scripts/device/README.md](../scripts/device/README.md)). **Then the operator attached a USB touch monitor and the pointer stopped being found at all** — the pin's `rel` filter rejecting the panel exactly as it was built to: `abs` requires `EV_ABS` + `ABS_X`/`ABS_Y` + `BTN_TOUCH` and `rel` requires `EV_REL` + `REL_X`/`REL_Y` + `BTN_LEFT`, and the CTouch panel has none of the latter. `/tmp/pointsrc.log` carried **no `pointsrc:` line at all**, which is what a silent total exclusion looks like. The pin was removed on 2026-09-27 and the shim's own default now reads `pointsrc: absolute device /dev/input/event3 name='TSTP CTouch'` with `raw range x=[0..1920] y=[0..1080]`. So the earlier advice in this cell — that pinning `rel` "stops the shim hunting for a touchscreen that is not there" — was wrong in the only way that counts: the touchscreen arrived. **Leave `RB_POINT_KIND` at its `auto` default** ([07](07-touch.md#finding-the-device-pointsrcc)), and re-measure a pin whenever the hardware changes, because a pin describes the hardware as it was. **Do not pin the `eventN` number anywhere**: this row first recorded `event7` and a later read of the same unit saw the same mouse on `event3`, because the number is assigned at enumeration and moves. Match on the name and the capabilities — which is what both readers do — and read the node number out of the log when you need it | set `RB_POINT_DEV` only to override discovery, and prefer the name over a node; if no pointer is found, `evdevdump --list` says so and only the keyboard map is available |
| S3.2 | `POINT_DEBUG=1`, touch two opposite corners and the centre — run 2026-09-27 with the USB touch panel (`TSTP CTouch`), not the mouse | **Measured by the operator's finger, and the fit is exact.** Every emitted coordinate is reproduced with **no residual** by `point_xform_abs()`'s own law, `raw × 1280/1921` and `raw × 800/1081` (the `+1` is `range = max - min + 1`, `point_xform.c:55`): top-left raw `(12,9)` → `(7,6)`, its settle to `(33,36)` → `(21,26)`; centre raw `(954,477)` → `(635,353)`; bottom-right raw `(1874,1071)` → `(1248,792)`, its lift-off to `(1875,1059)` → `(1249,783)`. All ten coordinates match the arithmetic exactly. So the orientation is right across the full diagonal — logical x rises with raw x, logical y with raw y — and there is **no mirroring, no transposition, no offset and no scale error**, so the affine `TouchCalib` hook is not needed for this panel. The centre tap's y reads 353, 47 px above the logical middle, and that is the **finger, not the fit**: the corners land at 0.8% and 99.2% of the panel, symmetric, and a linear map preserves midpoints. The panel reports its own range rather than the fb's size (`x=[0..1920] y=[0..1080]` against 1280×800), which is why every touch is scaled. `1280/1921` is `0.666320`, **not** `2/3`: the `+1` is the difference between 635 and 636 at the raw middle, and dropping it is the mistake these numbers would catch. **What this row does and does not settle, after 2026-09-27's report:** it measures `point_xform_abs()` — the panel's axes and the fit — and that reading stands, unchanged. It does **not** say where a tap lands, because the *consumer* reflects x: rbp acts at `1279 - x` of the record it is handed, so a shim writing `lx` straight through puts every tap on the mirror of the finger's pixel with y exactly right. S3.3 is the row that caught it, and [07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it) is where it is now recorded. A future pass on this row must keep the distinction: **raw vs logical tests this row, logical vs wire tests the reflection** | mirrored or transposed coordinates → `RB_POINT_INVERT_X` / `RB_POINT_SWAP_XY`; a constant shift → an affine `TouchCalib`; one end of an axis reading far inboard while the other is right → a scale error, which only the corners expose. **A tap landing on the wrong side while the log's `raw`→`logical` fit is exact ⇒ do not touch these flags**: that is the reflection, and no calibration expresses it ([07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it)) |
| S3.3 | drag-scroll the browser, tap PLAY, tap a playlist row | **Run 2026-09-27 by the operator's finger, and it FAILED — this is the row that caught the reflection.** The report, verbatim: *"touch seems off on certain screens, not sure why, when browsing, i can select certain songs but i can't access the icons on the left. also scrubbing through a song only seems to detect for deck 1 but im scrubbing through deck 2"* — and the reproduction, which is what made it a coordinate defect rather than a UI one: *"clicking a song in browse seems fine, but when i click bpm icon nothing, switched out of browse to scrub a song on deck 1 nothing, switched back to browse and bpm icon - nothing, went back to check scrubbing on deck 2 and deck 1 - nothing, back on the browse screen."* A full-width list row and a full-width deck both kept working; only the things that live on **one side** of the screen failed, and each one acted on the *other* side. **Cause: rbp acts at `POINT_LOGICAL_W - 1 - x` of the record it is given** — the shim's transform is honest (S3.2, all ten coordinates exact) and the reflection happens *inside* rbp on the way in. The sidebar occupies x 0..100, which under the reflection is the INFO/LOAD column at 1179..1279, so **no tap could reach a sidebar cell at all**; deck 2's platter is the right half, so scrubbing it acted on deck 1. Measured at five points by writing known records into the pipe rbp holds both ways (`work/tap.py`) and reading the framebuffer: x `1229` → the cell drawn at 8..50, `90` → deck 2's LOAD at 1152..1272, `42` → INFO at 1180..1275, `50` → fell *through* the sidebar column into the list, `200` → fell outside that LOAD button. Slope −1, intercept 1279. **Fixed** in `tscfake_emit()` (`tscfake.c`), which writes the reflection and is where rbp's other two consumer quirks already live; `POINT_DEBUG` now prints both ends of the transform. **Proven end to end on the unit, both binaries, same screen state, same injection instrument** (an `input_event` injected into the panel's own node, `work/poke.py`, so the whole chain runs — kernel → pointsrc → `point_xform_abs` → `tscfake_emit` → rbp): injecting the finger position raw `(75,209)` = logical `(49,154)` moved rbp to x `1230` pre-fix — PLAYLIST unchanged, the *right* pane's folder opened, 55 542 px changed — and to x `49` post-fix, selecting the sidebar's **TRACK** cell (361 384 px, a category redraw); the log says `logical=(49,154) wire=(1230,154)` against the old binary's `logical=(49,154)` with no `wire` field at all. A second post-fix injection at the **BPM** cell (raw `(74,386)`, logical `(49,285)`) lights BPM — the operator's *"when i click bpm icon nothing"*, at the finger's position. **Then the finger confirmed it, 2026-09-27, and this row passes end to end.** The operator, verbatim: *"ok, i just tested, everything seems to work correctly. sidebar works, scrubbing works, tapping songs works"* — i.e. the three things the injections could not settle, all three on a hand on the glass. **The log holds the finger's own record, not a synthetic one**, because `POINT_DEBUG` was left on: `/tmp/pointsrc.log` carries **25 distinct touch points spanning logical x 0 → 1251** — the sidebar column at x 49/53/59/67/69, the right half (deck 2's platter region) at x 885–1123, list rows at 137–463 and the right pane at 928/991 — and **every one prints `wire = 1279 − logical`**, the reflection being undone exactly, at the full width, with no residual. So the injection's proof and the finger's proof agree, on the same binary. **What is still owed:** only the mouse half of this row, which stays unrun (the G203's discovery, the `rel` accumulation and the emitted triples are exercised; no human has watched the on-screen arrow follow it, and the arrow is the mouse's alone — an absolute panel draws none, by design) | fast clicks swallowed → raise `RB_POINT_MIN_DWELL_MS`; a touch that lands where it should and does nothing → the reader is fine and the question is downstream, at the UI's own hit test; **a tap that acts on the opposite side of the screen ⇒ the reflection has been removed from `tscfake_emit()`, and no `POINT_*` flag or `TouchCalib` has anything to do with it** ([07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it)); a cursor that does not move at all → check `/tmp/pointsrc.log` and the `subscribed to` line that names the node it opened |
| S3.4 | tap the deck QUANTIZE box on the performance screen — both decks, a control outside the rectangle, then the same rectangle on the browse screen | **PASSED by injection 2026-09-28, and it is the row that found a reader bug that is not about quantize.** The deck QUANTIZE widget is the one part of the performance screen rbp draws and binds to no touch of its own (measured: injected taps at the widget left the frame pixel-identical while a control tap on INFO toggled the screen), so the shim supplies the binding: a press in the rectangle becomes `0x410b` `K_QUANTIZE` on that deck's channel through rbp's own key path, which is exactly what a hardware press sends ([07](07-touch.md#the-two-deck-quantize-boxes-touch_zonec), [08](08-controls.md#the-quantize-tap--a-keycode-rbp-already-has)). Measured with `work/qprobe.py`, which samples the deck's own flag every 8 ms *through* the gesture and so can say which phase moved it: **20 taps on deck 1 and 20 on deck 2 each flipped the flag on the press, none on the release, no misses**; a 10-tap control at the same y and raw x 600 flipped none; and the left bound is exact — logical x 26 (raw 40) is inside, logical x 19 (raw 30) is out. The screen follows, which is the half a flag cannot show: a frame diff either side of a single tap changes **34 rows inside that deck's own widget, 0 rows in the other deck's, 0 in a control region — 830 px of the frame for deck 1 and 1038 for deck 2**. **The trap this row is worth having is that it first measured 0 of N, and the fault was the reader rather than the tap.** After every `systemctl restart rblive4`, injected taps at raw x 83 wrote no log line and moved no flag for minutes; `work/poke.py move:400,900` then made the next tap land, which is what pointed at x. The cause is `pointsrc.c`'s absolute reader starting at `rx = ry = 0` while **the kernel drops an `ABS` event whose value the device already holds**: the device held x 83, so every report's `ABS_X` was filtered and the reader kept the zero it started with, judging every tap at logical x 0 — outside the zone. It is not an injection-only hole. The panel's stored x *is* the last position it reported, so the first finger touch after a restart at that same pixel arrives carrying no x at all and is delivered to rbp at logical x 0. **Fixed** by `seed_abs_position()` (one `EVIOCGABS` per axis at attach) and proven with a report containing only `MT_TRACKING_ID` and `BTN_TOUCH` — no position event in it — which now reads `logical=(55,754)` and logs `deck1 QUANTIZE tap at (55,754)` where it read `(0,0)` before. **The instruments are fixed with it**: `qtap`/`qprobe`/`qtrace`/`qwarm` prime x as well as y now, because a prime that moves only y silently measures the reader's stale x instead of the feature — and the runs that were read as "the tap is unreliable" (2/8, 7/10, 0/10) are unusable as evidence for anything, which is why the send-before-emit ordering rests on `sendKey`'s structure and not on them. **The browse screen is the known collision** and none of this changes it: the same rectangle there also hits rbp's own deck time-mode toggle (measured: `REMAIN 03:39` → `TIME 01:47` and back, 2940 px either way), the overlap is total in both axes and cannot be trimmed away, and `RB_POINT_QUANTIZE_TAP=0` is the way out. **And the operator's finger has now passed it** — *"quantize works correctly"*, 2026-09-29, given in answer to being asked directly — so the row's last open half is closed by the same kind of report that passed S8.5, and the feature has no outstanding item. | a tap that logs `deckN QUANTIZE tap` without the widget changing ⇒ the keycode or the channel, not the zone; **a tap that logs nothing at all ⇒ suspect the reader's x before the rectangle** — turn `POINT_DEBUG=1` on and read the `raw=` field, and `raw=(0,…)` means the report carried no x ([07](07-touch.md#the-position-the-reader-starts-from-seed_abs_position)); deck 1 works and deck 2 does not ⇒ the +640 px offset, which is not a mirror; **both decks toggling one tap** ⇒ the reflection has come back ([07](07-touch.md#rbp-reflects-x-and-tscfake_emit-undoes-it)) |
| S3.5 | swipe down from the top edge, tap all seven buttons (**the seventh, `USB STOP`, raises the chooser and ejects nothing** — until 2026-10-05 it unmounted the stick, see below), dismiss by tapping the panel — then repeat with `RB_POINT_MENU=0` as the baseline | **the wiring and the drawing are measured; the comfort is not.** By injection 2026-09-29 (`work/poke.py`'s `drag:` verb, into the panel's own node): a swipe in the top 55 logical px opens the bar, and the frame after it differs from the frame before it by exactly 1280×112 = 143,360 px inside the panel and by **0 px** anywhere below (the panel was 112 rows when this was taken — the operator had it halved to 56 later the same day, below, and every row range and pixel count in this row is the record of the height it was measured at) — the panel is drawn *over* rbp and rbp's picture does not move. A 2.5-minute idle pair in the same session differs by 0 px, and the one 416 px caveat is rbp's own deck-number chevrons: four 9-px glyph clusters in rows 605..625, toggling at **~2.1 Hz (a ~0.47 s period)** for the first half-minute or so after rbp starts and then settling, so a pair of frames taken a second apart right after a restart can differ by that much with nothing touched. **A restart with the panel open leaves no residue**: six frames at 1 s intervals from the service start hold the *last composite* (byte-identical to the live open capture) for ~4 s, and the first frame whose top band is rbp's own is byte-identical to the panel-closed baseline there, the whole frame differing from it by 87 px in rbp's own level-meter column. All six buttons fire with the shipped table — SOURCE / BROWSE / TAG LIST navigate, PLAYLIST (`0x204`) opens the BANK sidebar, SEARCH (`0x205`) the search pane with its QWERTY keyboard, and MENU (`0x206`) toggles MY SETTINGS **on the SOURCE screen only** (0 px from BROWSE, TAG LIST and PERFORMANCE). PLAYLIST and SEARCH shipped with a `0` sentinel until that measurement; the keyboard's `5`/`6`/`7`/`8`/`9`/`0` send the same six ([08](08-controls.md)). **The gate off is the baseline, and both halves of it are now measured** (`work/menu12.sh`, 2026-09-29 — the first run of this step whose every phase is verified by its *own* signature line before it is measured): the off session writes the gate line `menu: POINT_MENU=0 -- no panel is drawn and no touch is swallowed` and **not one other `menu:` line**, `fbpanel` reads **0.0 %** (`neither` 200 of 200 samples, **0/1280** border pixels on both rows), and the same swipe that opens the panel with the gate on produces no `menu: panel open` and not one panel pixel. The strip is rbp's again, point for point: rbp's own **INFO button** at logical `(1211,25)` — which lay entirely inside the 55-row strip as it was first built, and is now outside the entry zone's right edge at x 852 (measured off the glass: a grey `◄` square at x 1183..1206, rows 12..35, then the white `INFO` label at x 1217..1258, rows 18..30 — byte-identical on TAG LIST and BROWSE, and the label's bounding box the same on all 82 captured frames, so the menu takes rbp's INFO on *every* screen) — reads `bands_changed=[NONE]` with the gate on and changes **every** band with it off, while deck 2's QUANTIZE box at `(719,754)`, outside the strip, changes in **both** — so the ON verdict is a swallow and not a dead reader. rbp's TIME/REMAIN bar, wholly inside the band as it was first built and overlapping the entry zone only in its right end (x 426..484), reacted in *neither* phase and is recorded as a probe that cannot discriminate, not as evidence. **The strip's cost was then cut from a loss to a delay**, after the operator's fourth finding — *"touches when the bar is not displayed don't work anymore, the info and timer buttons specifically"*, which is precisely the ON verdict in that pair. `menu_zone.c` now answers a third value, `MZ_FEED_TAP`, for a strip press that never became a swipe and never opened the panel, and `pointsrc.c`'s `menu_replay_tap()` gives rbp the whole press back at the point the finger landed — down, a 45 ms dwell (`POINT_MENU_TAP_MS`, a bench knob, not in `SHIM_VARS`), up. So with the replay in place rbp's INFO should react in **both** phases, the ON one 45 ms late: **measured 2026-09-29 (`work/menu19.sh`), and it does** — 549,764 px of change with the gate on against 549,767 px with the shim out of the path, the same transition to four figures, with 4 replay log lines in the ON phase and 0 in the OFF one. **And the timer bar is the other half of that complaint, measured not in the strip's favour**: tapped at logical `(199,25)` it read 416 px (rbp's own dashed rule) with the gate on and **0 px — byte-identical — with the gate off**, so it does not answer a tap there with the shim out of the path either. The strip did not take the timer; there was nothing to give back, and that half is a separate question (the seventh/eighth panel button and the never-sent `K_INFO 0x020b` below) rather than a defect in the menu. **The strip's x extent was then narrowed to the middle third, which is the fourth finding answered at the source instead of downstream** — the operator's own suggestion after the replay shipped: *"can you make the swipe only register from the middle third? will this help get the info button working?"* `menu_zone.h` gained `MZ_ENTRY_X0..MZ_ENTRY_X1` (logical 426..852), and it is read on **one arm only**: `menu_feed()`'s down edge with the panel *closed*. Outside it nothing is swallowed at all, so rbp's `◄ INFO` control — the complaint's own control, spanning x 1183..1258 — is rbp's natively again. **Measured 2026-09-29 (`work/menu21.sh`)**: a tap at logical `(1211,25)` with the menu live changes **549,706 px** and the log carries **0** `pointsrc: strip tap` lines — answered on the frame it happened, no dwell, no replay — and a second tap closes it (549,498 px). The same run pins the zone from both sides: a drag from the **middle third** (logical 639) opens the panel at exactly **143,360 px, rows 0..111** (112 rows then), one `menu: panel open`; the identical drag from the **left third** (logical 200) changes **0 px** and opens nothing; and from the **right third** (logical 1211) it opens nothing either but **rbp** reacts to it (549,706 px) — the honest other half of giving the ends back, because a downward drag that starts on rbp's INFO is now rbp's drag. Inside the zone the replay still works: a tap at logical 639 leaves 416 px (rbp's drifting chevrons, rows 605..625) and logs **1** `strip tap`, and no panel. The panel's drawing is untouched — still full width — and **while it is open the x bounds do not apply at all**, because its leftmost and rightmost columns are SOURCE and MENU and a press on either that fell through to rbp would be a dead button (`test_menu.c` pins both ends and the zone's own edges at x 425/426 and 852/853). **And the zone does not touch the flicker, which is what the operator hoped it might**: the shimmer is rbp's repaint erasing the band in the page both share, and where a gesture may start has nothing to do with how often rbp paints. It also retires the frame diff as the swallow's instrument *for that row* — with the replay, a swallowed-and-replayed tap and an unswallowed one both end with rbp's panel open, and what separates them is the replay's own log line plus the delay ([07](07-touch.md#what-is-measured-on-the-unit)). **The labels were re-set after that run**, because the operator's first test on the glass said the font was *"too big and pixely"*: the old table is a 5×7 cell drawn at an integer scale, so scale 4 on a 96-row button meant every source pixel replicated as a 4×4 block. They are now **Decker** — the one Latin font file the device carries (`gui/fontdata/decker.ttf`; *not* the face rbp draws its own UI in, which is Pioneer's own `NS_FONT_ID_*.bin` bitmaps, corrected 2026-10-06) — rasterised once by `scripts/shims/bake_menu_font.py` into the committed `scripts/shims/menu_font.h`, so the shim still has no font engine, no freetype and no file I/O. At the **16 px** atlas the labels were first set at, a capital's ink is 11 rows in a 20-row line box and the labels measure 41..65 px in a **182** px column (the band is seven columns now, not six), off a host render of the panel; rbp's own top-bar `INFO` label is 13 rows of ink at 42 px, so 16 px sat two rows under the player's own text and `… 18` was the size that matches it exactly. **On 2026-10-06 the operator asked to *"size up the font but keep the size of the bar the same"*, and the atlas is 19 px now**: a capital's ink is **14 rows** in the **24-row** line box, the labels measure **49..76 px** in the same 182 px column, and the ink sits at rows **21..34** of the unchanged 56-row band. **19 is the ceiling and the geometry sets it, not the eye** — the bar's button band is 40 rows at 1280x800 and the line box has to fit it on the SMALLEST picture the port draws on, a 24-row band on the 480-row panels (640x480, 800x480), and the line box is 24 rows at 19 px against **25 at 20** — so one more pixel would cost those panels their band rather than making this one bigger. The size also raises the width a label needs, and the band now refuses on it: at 76 px `USB STOP` no longer fits a 480-px-wide picture's 68 px columns, so that shape draws **no band** and logs the refusal (640 wide, 91 px columns, still draws), where the fixed list of seven widths the test used to carry had been a canary 3 px from failing at 16 px. `test_menu.c` pins the ink band, the advances, the per-column fit and the width floor; `bake_menu_font.py <px> <light>` is the whole change — the size is the operator's to settle (`fbshim.so` md5 `68bae1cc0b4a6d70a75cf495760cc366`, mapped out of `/proc/<pid>/maps`, and the band read back off its own overlay plane with all seven labels at 19 px). **Asked on 2026-10-06 for a face that resembles the RX3's own** (*"is there another font that resembles the out of the box RX3 font?"*), the labels were **thinned 1/6 of a pixel a side** — Decker ships bold and only bold, so the lightening is synthesized by eroding the ×6 supersampled outlines and filtering back down. (Pioneer's own tables were the other candidate, and an earlier note called them *wide* — that was a decode error: they are **2 bits per pixel, 28 px to a row, not 1 bpp / 56 px**, and read correctly they are a *light* normal-width face, `USB STOP` spanning 98 px against this atlas's 76. See [07](07-touch.md#the-fonts-the-device-actually-has).) It changes bytes inside `menu_font_coverage[]` and **not one metric**, so every width and position above stands; the set's ink falls 598,976 → 473,826 (79%) at 16 px and 940,040 → 794,881 (85%) at the shipped 19 px. `bake_menu_font.py <px> <light>` sets both, `… 0` is the unmodified bold, and the shipped band was read back off its own overlay plane in the lighter face ([07](07-touch.md#the-fonts-the-device-actually-has)). **Every figure above was taken with the old 5×7 labels**, and the font does not touch them: the panel's rectangle, the palette and the witness's 15 sample points are unchanged by it (the points are chosen glyph-free — at the time, band rows 34 and 76 with the ink at rows 51..61, a panel *fraction* that the halving invalidated: they are now the button band's own edges one row in, rows 9 and 46 of a 56-row panel with the ink then at 23..33, and `menu_view_ok()` refuses a panel whose band cannot host the label's line box — `test_menu.c` asserts the points against the production list across ten panel sizes, since a witness that sampled a blend would read damaged on every tick). **The label pixels were then read off this glass** (`work/menu18.sh` + `work/m13/inkbox.py`, 2026-09-29): the six labels' ink is **11 rows** high (rows 51..61, every label the same), which is the 5×7 cell's 28 rows shrunk to a text height two rows under rbp's own top bar; each label's ink holds **56..69 distinct colours**, where a scaled 1-bit bitmap can only ever hold one or two — that count is the direct measurement of "pixely" and it is gone; the widest label is **59 px in a 213 px column**, i.e. **154 px of slack** where the old cell had 25; and the band holds **5552 border pixels** with the panel on against **0** with the gate off (112 rows then), which is the same capture answering the gate's promise in both directions. `work/m18_band_new.png` is that band, rendered for the eye. **The flicker was then measured before and after the change, and the number it lands on is not the one the plan expected** (`work/menu13.sh` tags `before`/`after`, two slide passes each in opposite directions, a **169 µs** fbpanel plus a sampler watching the six columns' highlight, both timestamped and joined by `work/m13/align.py`): the highlight marches 1 → 6 in all four traces, so the transitions really happened and the column-granular re-classify did not break them; the longest run *starting at a press boundary* fell from **2090 and 4234 µs** to **1226 and 1922 µs**, with both of the before's over-2 ms runs gone; and the miss fraction over the panel's open period is **unchanged at ~3.3 % in all four traces** (112 rows; 2.09 % at the 56-row band with the shipped 0.5 ms tick), because the steady-state miss is the one repair copy rbp's own repaint forces every frame and F1/F2 never touched it. So the panel no longer blanks for milliseconds on a press, and what is left — a **~550 µs shimmer, 57 times a second** at the 112-row band (~366 µs at 56 rows) — is F5's to remove, at 1.4–2.4 ms of every 17.5 ms frame on rbp's own RT thread ([07](07-touch.md#the-flicker)). **And the change's own bill came down with it** (`work/menu16.sh` run before and after, `work/m13/m16_before.txt` and `m16_before2.txt` against `m16_after2.txt`, 15 s windows, three closed/open pairs per session, nothing touching the screen in any window): the paired (open − closed) cost of rbp's process fell from **+415 and +364 ticks/60 s — ~6.5 % of one core** — to **+143 (2.4 %)**, and the two sets do not overlap, since the largest after-difference is below the smallest before-difference in both tick rows (+260 against +284 at the shipped 0.5 ms; +696 against +1384 at 0.05 ms). So the ~3× the counted repaints could not account for was `menu_intact()`'s fifteen layout rebuilds per tick, and it is **1.1×** now. **The panel was halved on the operator's own request the same day** — *"also the swipe down menu is too tall, make it half the height"* — so `MZ_PANEL_Y1` went 111 → 55 and `MZ_BTN_Y1` 103 → 47: 56 rows, 7 % of the screen, with a 40-row button band instead of 96 and the 8-row border kept at each end. Everything above that was measured at 112 rows is marked as such. Measured at the new height, on the glass: a raw dump of rows 0..119 with the panel open reads frame 0..1, fill 2..7, buttons 8..47, fill 48..53, frame 54..55 and rbp's own black from 56 — so the drawn band is exactly those 56 rows; the label ink is 11 rows at 23..33 (the 16 px atlas of the day; **14 rows at 21..34** now), unmoved *by the halving*, because the font is a fixed line box; and the flicker instrument reads **97.91 %** duty over 30,000 samples (17.4 s), 617 miss runs, longest **1223 µs**, all runs under 2 ms — against 97.05 % and a longest run of 1807 µs at 112 rows; a second 12,000-sample window read **97.64 %** (longest run 1465 µs), so read the 0.5 ms tick as a ~97.6–97.9 % band and not a constant. **The tick was then re-priced at this height, because halving the band moved the miss off the copy and onto the poll wait** (`work/mm10.sh`, one build, one variable, both phases verifiably open, the same 12,000-sample window each): `POINT_MENU_MS` **0.5 → 0.1** takes the duty **97.64 % → 99.11 %**, the miss runs **275 → 107**, the longest run **1465 → 908 µs**, the total miss **2.62 % → 0.94 %** of the window, and the samples catching *both* border pairs missing **105 → 0**; the *paired* (open − closed) cost of rbp's process goes **+62 → +88 ticks/15 s**, i.e. **4.13 % → 5.87 %** of one core. So the default is **0.1 ms** now — five times the wakes for +1.5 points of duty and halved runs, at 1.7 points of one core for the whole panel — and the revert is that one literal or `POINT_MENU_MS=0.5` in the environment. **Verified as the compiled default, with nothing in the environment at all** (`work/mm11.sh`, `POINT_MENU_MS=<unset>` printed as the run's own first line): **99.07 %** duty over 12,000 samples (112 miss runs, longest 669 µs, `neither 0`) and **98.99 %** over 30,000 (303 runs, longest 988 µs, `neither 1`), with the panel's paired cost at **+76 ticks/15 s = 5.07 %** of one core. So the default is the configuration the sweep priced, and the CPU column is a within-session pair that does not transfer across sessions — the older `menu16` figure of +2.4 % for this feature was 112 rows on a different shim, not a before/after of this one. The repair copy itself fell from 244 µs to **81.9 µs** for the now 143,360 B (`work/fbcopy`, warm page, one run per method — u32 67.9, u64 81.5, vst1q 79.9, memcpy 81.9, and a 53.5 µs floor at 2,680 MB/s), which is *why* the tick became the lever: at 112 rows the 550 µs copy was most of the miss and no period could shorten it. **And the panel gained a seventh column on 2026-10-01, at the operator's own ask — `USB STOP`, the safe eject.** *"yes add a USB STOP button to the menu"*, after they declined pulling the stick for S8.7 on the grounds that rbp's own stop button *"is not mapped currently"*. This is the first column that is **not** a convenience for something reachable elsewhere: the six are rbp browse keys this rig can also send from the keyboard's digit row, while `K_USBSTOP 0x8002` is reachable from nowhere else at all — so the column is a new binding, not a new way to press an old one. `MZ_COLS` went 6 → 7 and the columns 213 → 182 logical px (the arithmetic is `(i*1280)/7`, so the widths are 182/183/183/183/183/183/183 and they still tile x 0..1279 exactly, pinned by a `test_menu.c` assertion that now spells its width off `MZ_COLS` rather than the literal 213 that would have gone on passing). **The key needs three edges, not two** — measured live the same day through the real dispatch with the `SOURCE` column repointed at it (`systemctl set-environment POINT_MENU_KEY_EXTRA=0x8002`, held 500 ms, `UsbStorageManager` sampled at 1 kHz over `/proc/<pid>/mem`): the **press** sets `[this+0x8c] = 5` and mutes both decks, the **repeat** sets bit 7 of `[this+0x3d0]` and calls `UsbStorageManager::request_usb_stop` @ `0x324578` — *that* is the eject request — and the **release** unwinds both. `[usb1+0x8c]` reading 5 for exactly the 500 ms of the hold is the proof the handler body ran; a tap looks like nothing because the release clears it microseconds later. `pointsrc.c` therefore sends press + repeat + release for this key on both the tap and hold paths (`menu_key_needs_repeat()`, asked by **keycode** and not by column so that repointing a column through the bench knob stops sending it), and `menu_zone.c`'s label table gained `"USB STOP"` at 65 px — the widest of the seven, 117 px of margin in its column then (76 px and 106 px of margin at the shipped 19 px atlas). **Read off the glass with the panel open and nothing pressed** (2026-10-01): the six dividers between the seven columns are the only `MENU_DIV` pixels in the band — **6 runs of 39 rows = 234 px**, the column count read straight off the framebuffer — every label's ink is 11 rows at rows 23..33 as before, and the widths are 57..59 for the six browse words and **65 for `USB STOP`** (the 16 px atlas; at the shipped 19 px the ink is 14 rows at 21..34 and the widths are 66..76). So the drawing is measured; the press came the same day, and **it works**. Twice, by the operator's own finger (`fbshim.so` md5 `b151d3d128d46fe636424ad8cc6b8baa`), and both presses produced the same chain in the unit's own logs: `pointsrc: menu 'USB STOP' tapped (button 7) -> key 0x8002`, **the SOURCE screen cleared** (the operator's report — the half no log line carries, and rbp's own UI dropping the media the moment the stop is issued), the stick off the bus at `09:55:24` and again at `10:32:2x` (`usb 2-2: USB disconnect, device number 2` with `[sda] Synchronizing SCSI cache`), `usb-watch` logging `detach: notifying rbp` in the same second and `umount -l` on both mounts a second later, then remount + `chroot bind ok` and `attach: rbp opened export.pdb` **about 20 s after the press in each case**. Two things stay distinct here. **The button stops the media; it does not release the host mount** — a Pi has no port power switch, so the mounts go when the kernel sees the device leave, one second *after* the physical pull, and the operator still does the pulling. And **the release-following-repeat question is still open**, because the stick was taken out by hand rather than by rbp; settling it needs one press with nothing pulled afterwards (rbp's media state returning to present on its own means the release did not cancel the eject; staying stopped means the fix is a gap between the edges — the hold path is the shape with one, and it is a one-line change). **A press on this column is not harmless**: it really does stop the stick, which is the feature — it exists so nobody has to pull live media. **Only the operator can settle:** whether the shimmer that remains is what they were seeing — if it is, the fix is F5 and it has a real price — and it is **parked there for now** on their own instruction (*"ok thats fine for now, just document so we can revisit another time"*), so what ships is the measured state above and F5 is unstarted; its terms, its two traps that measure *worse* than today, and the question to ask first are written out in [07](07-touch.md#the-flicker) — and whether 56 px is a comfortable swipe start and 213×40 px a good target (the button band is 40 rows now, was 96), whether the labels read at the viewing distance, whether the 45 ms a strip tap now costs is perceptible — it is the same 45 ms `RB_POINT_MIN_DWELL_MS` already spends holding a fast click open, and the only way to remove it is `RB_POINT_MENU=0` — whether the replay really gives the strip back in daily use (BROWSE's USB1 cell and title band and PERFORMANCE's title band are drawn, not controls, so a replayed tap on them may do nothing for reasons that have nothing to do with the strip), whether it is worth a **seventh panel button** for rbp's INFO everywhere — whose keycode `rbp_abi.h` already names as `K_INFO 0x020b`, referenced nowhere else in the tree and never sent, so it is a wiring change rather than a measurement — and whether closing on a button press is right | the panel appears but rbp's screen moves under it → the swallow is not latching: read `/tmp/pointsrc.log` for a `menu:` line and check the press really started in the strip. A button that does nothing → the log names the keycode sent, and MENU is narrow by design — check the screen. A ghost left after the close → rbp's repaint is not complete, and the answer is `cursor_paint.c`'s conditional restore rather than a change in the menu. The duty tool reporting **0% for a panel that is plainly on the screen** → it is a stale copy of the tool, not a stale shim: `/tmp/fbpanel` built before the border colour was fixed looks for `0xC67A` while the framebuffer holds `0xCE7A` and counts zero every sample. Rebuild and re-copy it — `gcc -O2 -static -o fbpanel work/fbpanel.c` builds **on the unit itself**, which is the reliable route; `work/fbpanel` is cross-built and a copy of it left on the Pi from before the fix reads exactly this way. **And a drill on this feature must wait for the phase's own signature, never for the pointer:** `menu_draw_start()` is called unconditionally from `tscfake_open()` and logs the gate line from the *main* thread, while the reader thread's `pointsrc: absolute device` attach line comes **later** — so a drill that waits for the attach line can measure the window before the gate. Wait for `menu: POINT_MENU=0` (off) or `drawing the panel` (on), and refuse to measure a phase that never produces its own ([07](07-touch.md#what-is-measured-on-the-unit)). **But do not read such a phase as one that never reached its state** — that is what cost `work/menu19.sh` both of its phases on 2026-09-29, in a run whose log held the reader's attach line and the raw-range line and the cursor line and *no* `menu:` line of either kind. The gate had run; its line had been dropped. `pointsrc_log()`'s one-shot open published `log_tried = 1` before `log_fp = fopen(...)` under no lock and returned silently on NULL, so every call in flight while the winner was inside its `fopen()` was lost, and every line here is one-shot — a lost signature is lost for the life of the process. The tell is which lines survive: the ones written by threads that log *later*. Fixed with `pthread_once()`; `work/menu20.sh` counts missing lines per start before and after it. **And the button's second meaning shipped the same day, after the operator's next two findings on the glass** — *"the menu button does't seem to bring up the mentu. also, if i long press menu it should go to utility"*. The first is not the panel's: on BROWSE a ~300 ms press of the MENU column changes **71,680 px — our own 56 rows and nothing of rbp's** — while `/tmp/pointsrc.log` carries `menu 'MENU' tapped (button 6) -> key 0x206`, so the keycode leaves and this firmware simply binds no menu to a press of `0x0206` on the browse screens (the same 0-px result this row already records for BROWSE, TAG LIST and PERFORMANCE). The second is rbp's own: **held, the key reaches UTILITY from any screen** — 837,127 px from BROWSE and 583,886 from SOURCE, both read off the framebuffer as PNGs — on rbp's timer, whose threshold the keyboard path brackets at **between 300 and 400 ms** (200/300 ms change nothing, 400 ms and up give the whole screen). `menu_zone.h` therefore carries `MZ_HOLD_FINGER_MS` **350** (what the finger must do), `MZ_HOLD_KEY_MS` **500** (how long the panel then holds the key) and `menu_hold_fires()` — pure, host-pinned by `test_menu.c`'s 6,330 checks, with `POINT_MENU_HOLD_MS=0` at the bench turning every press back into a tap (`work/m23panel.py` drives both, and `--browse` drives the two findings on the library screen). **The measuring, not the gesture, is what nearly shipped broken**: the press's duration came from `(long)ts.tv_sec * 1000000000L + ts.tv_nsec` and `long` is four bytes here, so a nanosecond clock wraps every 4.29 s of uptime and reads negative half the time — a real 1101 ms finger measured **0 ms** and was spent as a tap. `work/m23clock.py` is the instrument that caught it, comparing the shim's number with the kernel's own delivery stamps from `work/evwatch.py`: **3 of 5 presses read 0 ms before, 5 of 5 within a millisecond after** the clock moved to a 64-bit `shim_now_ms()` (and the guard from `down_ns > 0` to an explicit `have_down` flag); the boundary on the real path reads 340.9 ms → tap, 360.3 ms → hold; and the rel loop carried the same wrap, where it had been hiding behind a fallback that happened to equal the dwell it wanted. The host suite was green through all of it and cannot be otherwise — the host's `long` is 8 bytes — so this one is a target-side drill and nothing else (`menu_draw.c`'s microsecond clock has the same shape and is safe only because every use is a difference, written down there). **Only the operator could settle whether 350 ms is the right amount of finger — and they have**: *"yes right amount of delay"*, 2026-09-29, so the constant ships as measured *and* felt, and the feature's one preference is closed. What is still unasked in that feature is whether the ~500 ms the panel holds the key for (`MZ_HOLD_KEY_MS`) feels like the player's own button. **And the hold was then made to fire while the finger is still down** (2026-10-04), on the operator's next word about the same button: *"for holding the MENU to get utility, after two seconds the menu should disappear and it should just go to utility by itself"*. The "two seconds" is a description of the old behaviour and the shim's own log is what says so — the line from their finger reads `menu 'MENU' held 3368ms (hold is 350ms) -> key 0x206 down for 500ms`, i.e. they held it for three and a third seconds and the key did not go out until they let go, because that was where the fire-on-release rule put it. rbp's timer runs on the **key**, which the shim only pressed at that moment, so the panel showed no reaction for as long as the finger stayed down. `menu_zone.c` gained `menu_hold_pending()` and `pointsrc.c`'s `read_loop_abs()` now polls on a 20 ms tick (`POINT_MENU_HOLD_TICK_MS`) instead of blocking in `read()` while such a press is outstanding — the one press in that loop that can complete with no event to wake it — so the panel closes itself and the key goes down at the threshold, with the finger still on the glass. The threshold did **not** move: `MZ_HOLD_FINGER_MS` stays **350**, as felt and approved above. Gated on `menu_pressed()`, so every other press is answered by the blocking read exactly as before, and on the `press_btn`/`cur_btn` guards, so the swipe that opens the panel and a press on the panel's border can no more become a hold than they can fire on release. **Measured on the unit by injection** (`fbshim.so` md5 `995d644884ae757e2bbedeeeccb0f873`, panel swiped open, `work/poke.py`'s `touch:1508,37,2000` = logical `(1004,27)` = the MENU column, the device silent for the whole 2 s): `pointsrc: menu button 6 held 360ms and the finger is still down -> the panel dismissed itself` / `menu 'MENU' held 360ms (hold is 350ms) -> key 0x206 down for 500ms`, then `menu: panel closed` — two lines at 360 ms of a 2000 ms hold and **nothing else for the remaining 1640 ms**, so no second fire, no re-open and no tap replayed; the same drill on the `USB STOP` column fires button 7 identically, which is the generic-hold rule still holding. Host suite 6,373 checks (was 6,330). |
| S3.6 | **the edge drawers — the operator's six requests, in one pass.** Swipe in from either edge (`python3 work/poke.py /dev/input/event0 sideswipe:left`, then `sideswipe:right`); with **both** out, **ride a fader** (`sidetap:left,lane:0`, then `sidetap:right,lane:1023`) with an eye on the glass, press each strip's SYNC (`sidetap:left,sync` / `sidetap:right,sync`) and the nudges (`sidetap:left,plus`, `sidetap:right,minus`), press the **middle of the glass** (`glasspress` — this must NOT close anything), then put each away with an **outward sweep on its own background** (`sideaway:left`, `sideaway:right`) | **All six requests are built and proven by injection, 2026-10-05; the look, the swipe's feel, the fader's smoothness and CUE/PLAY on a loaded deck are still owed to the operator.** *(1) Both panels at once:* the DRM one-master block is **solved** rather than worked around — `drmband.c` opens the card once and refcounts the fd and the master (`dev_acquire()`/`dev_release()`), each band owning only its buffer/framebuffer/plane, and `pick_plane()` skips any plane whose `crtc_id != 0`. Measured: **two distinct planes on one crtc — `plane 127` and `plane 138`, both `180x800`** — and both drawers answered their own channel with both out (`SYNC -> 0x4112 ch1`, `-> 0x4112 ch2`). *(2)* SYNC at the top of the strip is `K_SYNC 0x4112`, per-deck channel. *(3)* CUE/PLAY moved to the bottom (`0x4102`/`0x4101`); the keycode and the channel are proven, **the screen is not** — the unit sat on rbp's browse screen with **no track loaded**, so there was nothing to move, and a 401-byte `/dev/fb0` diff turned out to be rbp's own clock (a control capture pair with nothing injected differs by the same 401 bytes), so a small diff is **not** evidence a key landed. *(4) The `±` nudge is the one with no keycode:* rbp has no bend/nudge macro, so it is `K_JOG_ROT 0x4305` with **`OP_ROTATE`** and a signed rev/s, and rbp **keeps bending at the last speed until told zero** — measured `rotate speed +0.35 ch1` then `speed +0.00`, and `-0.35 ch2` then `+0.00`. *(5) The fader's flicker* — the operator's *"the volume slider flicker when i slide up or down"* — was `side_paint()` rewriting all 180×800 px of the **live, single-buffered** overlay plane **under `g_lock`** (the `MENU_FILL` bed goes down first, so mid-paint the panel reads as flat colour; **8** such repaints in one drag). Fixed by the band's own pattern: each drawer now has a **front/back image pair built off the lock** (`menu_side_build()`, from `menu_thread`) and **published by one bounded `memcpy` under it** (`menu_side_publish()`; `pitch 180 == width`, the unit's path). Host-side the paint count is unchanged and the image is now `PLANE_IS_DRAWER`-compared at both strides; the on-glass verdict is the operator's. *(6) The drawers stay up* — the *"away tap"* that used to close both is **removed** at the operator's word (*"the side bar should stay up until i swipe them away"*): a press on the centre glass now returns `MZ_FEED_NONE` and is handed to rbp untouched, and the **only** dismissal is the 56 px outward sweep on the drawer's own background. *The fader* takes `K_FADER 0x501e` via `OP_VALUE`; `ch1 -> 0` and `ch2 -> 1023` measured, and the shared array read out of the running process gives `g_fader = (1023, 0, 1023)` / `g_fader_seen = (0, 1, 1)` **minutes** after load — past the ~30 s unity seed window that would have re-asserted that `0` — and knobshim's `R_ARM_GLOB_DAT` slot for `g_fader` resolves to `0xf788e2b8`, byte-identical to fbshim's definition address, so it is one array. *A dismissal hands the plane back:* `side: left/right drawer closed -- its plane is released`, then the band's `1280x56` plane on id 127. Host suite green: test_menu 6,572 · test_side 2,422 · test_window 258 · test_keyboard 1,044 · test_menu_dev 148 × 2 strides | a **repeated plane id** on the second open ⇒ the second drawer silently took the first one's plane, which is the failure the shared device exists to prevent. A **missing `nudge stop`** ⇒ a track that runs away, since rbp holds the speed; the stop must also be reachable on the two paths with no release report (device loss, `POINT_MENU=0`). `g_fader` reading **1023** for a channel the drawer set to 0 ⇒ the two shims have private copies and the seed is overriding the operator. A drawer that stops answering after any earlier press elsewhere on the glass ⇒ step 0's `was_down` rule is gone (the declined-press silent death; `test_entry_survives_declined` pins it). **A press on the centre glass that closes a drawer ⇒ the reversal did not take** (`glasspress` is the drill). **Never inject at raw x 1884 at the strip rows** — that is USB STOP and it stops the operator's media. |
| S3.7 | **the spontaneous exit — not a drill anyone runs.** `rbp` leaves on its own: **seven times** between 2026-09-27 and 2026-10-05 (09-27 ×2, 10-01 ×2, 10-04, 10-05 at 10:52 and 12:18 — the last being the operator's *"ok it just hung and restarted"*). Read `journalctl -u rblive4` for the shape of the exit and `$RB_DEPLOY_ROOT/rbx3-run/root/pdj/crash.log` for its cause | **Diagnosed as not ours, and the witness that will name the cause is armed and proven — 2026-10-05.** *The symptom:* rbp (pid 6065) left at 12:18:41; the launcher's `kill -0` loop noticed within 2 s, ran `cleanup`, and printed `start-rb: rbp exited`; `Restart=always` with `RestartSec=10` had it back at 12:19:04 — **~25 s of frozen screen, and the Pi itself never rebooted (up 41 h).** *Why the journal is the only place it shows:* a **deliberate** restart prints `stopped` ×2 → `Deactivated successfully` → `Stopped rblive4.service` → `Started`; a **spontaneous** exit prints `start-rb: rbp exited` → `Deactivated successfully` → **`Scheduled restart job, restart counter is at N`**, with no human in it. The launcher can never report a status either — its last command on *every* path is an `echo`, so the unit always looks like a successful stop. *Every other cause is measured out:* dmesg is **empty** across the exit — no `segfault at`, no OOM kill, no under-voltage, no USB event — which rules SIGSEGV out on its own, because the kernel's `show_signal_msg` prints a line for any user-space fault; there is **no core** (`core_pattern` is the plain file `core` and rbp's cwd is the chroot root, so it would land in `$RB_DEPLOY_ROOT/rbx3-run/`); memory is **flat at ~2558 M for the ten minutes around it**; the launcher latched the *right* pid, so its `kill -0` could only have ended because 6065 died; and **none of the three loaded shims defines or calls `exit`**, so nothing of ours asked. *The witness:* `crashcatch.so` — a SIGSEGV handler only until now — gained interposers for `exit`/`_exit`/`_Exit` that log the status **and `__builtin_return_address(0)`**, the caller inside rbp that decided to quit, plus the whole fatal set (SEGV/BUS/ABRT/FPE/ILL) with pc/lr/addr/sp/fp and r0..r9. It is **first in `RB_LD_PRELOAD`**, because its constructor is what arms the handlers and resolves the real `exit` before any other preload can fault or exit. It logs to **disk, never `/tmp`** (a 1.9 GB tmpfs that loses the evidence at the next reboot), at the **chroot-relative** `/root/pdj/crash.log` — `$RB_DEPLOY_ROOT/rbx3-run/root/pdj/crash.log` host-side — because the chroot's own `/opt` is **empty**, so a shim naming `/opt/rblive4/log` silently falls back to tmpfs and defeats the whole point; `RB_CRASH_LOG` overrides it. Three glibc-2.13 facts are handled rather than assumed, since a shim calling a symbol this vendor libc lacks links *fine* and then kills rbp at startup with `symbol lookup error`: `dlsym` lives in **libdl, not libc** (hence `.symver dlsym, dlsym@GLIBC_2.4` and `libdl.so.2` on the link line), `atexit` is **absent entirely** (which is why the "did it exit cleanly?" question is answered by interposing `exit`, never by registering a handler), and `stat` is **absent** (the size cap measures with `lseek`). The cap is 256 KB because the unit runs `Restart=always` with `StartLimitIntervalSec=0`, so a crash loop would otherwise append a line to the SD card every 10 s forever. *Proven inside the chroot with `ccexit`* — which is deliberately **not** in `TESTS`, because the whole host suite is `-static` and a static binary has no dynamic loader, so `LD_PRELOAD` is ignored and there is no host-suite test for this by construction: `exit` → `status=7 caller=0x008454cc`, `_exit` → `status=9 caller=0x005734d4`, a nil dereference → `SIGSEGV sig=11 addr=0x00000000 pc=0x0045b4a8`, `abort()` → `SIGABRT sig=6 pc=0xf7cd98d4`, and the exit codes propagate through (7, 9, 1, 1, 0). Deployed and live: `armed log=/root/pdj/crash.log real_exit=resolved` is in the log with `crashcatch` mapped into the running rbp, and the default path was proven separately — 119 bytes, on disk. *One measured limit:* `ccexit ok` — **returning from `main`** — writes only the armed line and no death line, because glibc reaches `exit()` from inside itself on that path and an intra-libc call is bound internally. rbp never returns from main, so it costs nothing here; a silent log would otherwise be a puzzle. *Child noise is bounded and startup-only:* rbp spawns ~3 short-lived helpers at bring-up (`exit status=0` ×2, `_exit status=0` ×1) and the log held at **7 lines / 426 bytes across a 45 s measurement**. **A death with NO line is therefore evidence too — it was SIGKILLed**, which is uncatchable by design and is exactly what `cleanup()` sends, so the absence is what rules the launcher in or out | a log that stays at the armed line across a real freeze ⇒ rbp was **SIGKILLed**, and the first killer to look at is `cleanup()` — the other being the kernel OOM killer, the one case this witness cannot see. A **missing `armed` line** ⇒ `crashcatch.so` never loaded: check it is still first in `RB_LD_PRELOAD`, that `start-rb.sh` still re-copies it into `$RB_CHROOT/usr/lib/` (it had **never** been in that list until this change, so a stale deploy is the default failure here), and that `CRASH_LOG` resolved to a path the chroot can actually write |
| S3.8 | **touch in rbp's own UTILITY screen.** Hold the shim's MENU control to open UTILITY, then drive the new gesture through the panel's pointer node (`work/poke.py`'s `drag:` verb): a drag up and down the list should scroll it, and a tap on a row should put the highlight on that row and open it | **the code and the host tests are done; the glass run is not.** `util_zone.c` + `test_util.c` (421 checks: every row boundary, the coalescing, the cap, the tap travel, and both refusals). The gate, the window model and the geometry are measured, not inferred -- `getBrowseMode() == 7`, 33 items in a 12-row window, `abs = initialNo + cursorNo` moving exactly one per rotate, row pitch 52 px from y 50 ([07](07-touch.md) § *Touch in rbp's own UTILITY screen*) | the frame **diff** is the witness, **bracketed against a same-length drift control**: rbp marquees long titles on that screen and a 2 s drift there reads ~3,400 px, so a "reaction" under that is a scrolling title and not the gesture. The two refusals are the ones to watch for: a drag must not tap, and a drag must **not rotate while rbp is editing** -- that is what would silently rewrite a setting rather than scroll. **Never inject at raw x 1884 at the strip rows** (USB STOP) |
| S3.9 | **the waveform swipe.** Load a track into deck 1, then, on the performance screen, swipe a finger UP and then DOWN on the middle of the waveform | **the superseded pinch verified its sending half on the unit; the swipe is the operator's own replacement for it and has not been driven by a finger yet.** The first build was a two-finger pinch and the operator rejected it — *"it messes up scrolling everywhere else"* — because `pinch_zone.c` answered `MZ_FEED_TAKEN`: its latch swallowed every report, one-finger ones included, until both fingers lifted, and on every screen but that one a single-finger drag IS the scroll. `wave_zone.c` replaces it and **cannot take a report at all** — its one door returns a signed step count — so rbp is handed exactly the stream it is handed today. The gesture is one finger, vertical-dominant (`|dy| >= |dx|`, so a drag *along* the wave stays rbp's), 40 logical px per step, up = zoom IN, ratcheted from the anchor and clamped to rbp's 0..4 ladder from the scale the press began at. The rect is `x 200..1080, y 60..480` (clear of the deck panels, BEAT FX, the hot-cue row, and the shim's own band strip). `work/poke.py` writes into the touch node and the shim log answers with `pointsrc: wave zoom IN 1 step(s) at (600,300)`. **A track must be loaded** or rbp refuses the zoom itself: `IsAbleZoom` @`0x113574` wants one of the two per-deck words at `0x03267238 + 0x1610`/`+0x1670` to read 7 or 18, and an empty deck reads 1 | `work/poke.py` (logical coordinates into the panel's pointer node) and `work/zoomprobe2.py` (looping into `/tmp/zoomprobe2.log`, with a known-good `ZoomGridDirection` marker and the gate's three words). The gate is *screen*-level, as asked, so a swipe that is not vertical-dominant is still rbp's — that is the price of never consuming a report, and it is the design, not an oversight |
| S3.10 | **the USB STOP chooser — the operator's ask of 2026-10-05** (*"when you have USB stop, put up a prompt for USB1, USB2 or Cancel"*). Swipe the band open, tap the seventh column, and read the box; then tap each row in turn, and leave it alone once to watch it dismiss itself | **Pass by injection, 2026-10-05, on the live build (`fbshim.so` md5 `b7c3ddf0…`, verified the running one out of the process's own memory rather than from the deploy copy), and every step of it read off the shim's own log.** The raise is one line and it names rbp's answer for both devices — `pointsrc: menu 'USB STOP' -> the USB STOP chooser (usb 1 ready, usb 2 absent)` — and the box is then a **fourth overlay plane** of its own: `menu plane: crtc 102 plane 138 fb 725 560x271 16 bpp pitch 560 px (303520 bytes)`. **Its picture was captured rather than assumed**, and not with a screenshot: the box is a plane, so `/dev/fb0` does not contain it, and `/proc/pid/mem` at the plane's own logged address returns **`EIO`** — vc4's dumb buffers are `PFNMAP` mappings and `get_user_pages()` refuses them (measured on the band's plane too, so it is a property of the rig and not of the box). `work/boxshot2.py` fetches it the way a DRM client does, by framebuffer id from a second fd, and the 560×271 frame shows the title, `USB 1` bright, `USB 2` **visibly dimmed** and `CANCEL` bright — which is the one thing the host suites cannot say, since they compare against a reference they drew themselves. `CANCEL` answers `pointsrc: usb stop chooser -> cancel (channel 0)` and closes the box; the **dead row answers nothing at all** — tapping `USB 2` closes the box with no `usb stop chooser ->` line, which is the refusal it is meant to be; and left alone it dismisses itself with `pointsrc: usb stop chooser timed out after 10000 ms`. **The one step not run is the eject itself**, `chooser:usb1`: it is the operator's production media and their press to make, and it is the only thing that needs their eyes. Host coverage at the same binary: `test_prompt` 143 checks and `test_menu_dev` **195** (up from 148), the latter including the liveness leg, the pitch-matching memcpy fast path and the page fallback with the plane refused. See [07](07-touch.md#the-seventh-column-raises-a-chooser-prompt_zonec-prompt_paintc). |
| S3.11 | **both faders at once — the operator's own two hands.** Swipe both drawers open, then put **two fingers on the two drawn handles at the same time** and drag them together: each must move **only its own** channel (`pointsrc: side left fader ch1 -> N` / `side right fader ch2 -> N`), and neither may jump when the other moves | **Pass — run by the operator's own two hands, 2026-10-06; their verdict: *"it looks ok now".*** The ask, verbatim: *"if i try to drag both volume meters it gets confused and only one of them changes."* **The gesture itself is the proof and it is in `/tmp/pointsrc.log`:** **304** `side <l\|r> fader ch<N> -> <v>` lines, the two channels **alternating** (210 alternating runs of consecutive same-side reports), **zero non-monotonic runs on either side** — ch1 walking **144..1023** and ch2 walking **230..1023** — then both drawers closed and the band opened. That interleave is what two independent fingers look like and is impossible if the two were still folded into one state. The cause was the pointer model, not the panels: every gesture module in `pointsrc.c` was fed from a **single** contact, so two fingers were two reports into one state and whichever hand moved last owned the channel. The repair is a **pointer dimension** — `SZ_PTRS 2` with `SZ_PTR_MAIN 0` / `SZ_PTR_ALT 1` — where the primary contact is the only one **rbp, the band, the browser window, the USB STOP chooser and the UTILITY gesture** ever see (rbp has one pointer), `pointer_report_alt()` offers the second contact to the **drawers and nothing else**, and a report it does not take is **dropped, never forwarded**. Drawer gesture state is per-`(side, pointer)` (`struct side_st st[2][SZ_PTRS]`); whether a drawer is *out* stays per-panel (`sz_open[2]`). **The trap is one layer below the shim: the kernel de-duplicates an unchanged `EV_ABS` per axis** — `input_get_disposition()` drops a value equal to the last one it accepted, keeping **one per axis, not one per MT slot**, so a second contact's first position can vanish. Measured on the panel's own node: both fingers down at the same height delivered `MT_SLOT 1, MT_ID 2, SYN` and **nothing else**, leaving that contact at **(0,0) — the LEFT drawer's own corner**, so the right hand moved the left fader. The `seen` guard closes it (`int seen[POINT_SLOTS]`, cleared on every `ABS_MT_TRACKING_ID`, set by `ABS_MT_POSITION_X`/`_Y`; a contact that has never said where it is is reported as up). A **still** finger's repeats are legitimately dropped, so a drill must move both — the injection runs gave **5/5 identical**: `ch1 -> 897`, then `ch2 -> 897/724/548/375/198/375/548/724/897`, then `ch1 -> 724/…/198`. Shipped `fbshim.so bc98920795f0e6cf60abc05d59fb316a`, the running one verified out of `/proc/<pid>/maps`. Host proof: `test_side.c`'s `test_two_hands()` (2,467 checks, 0 failures). The injection drills moved rbp's channel fader values and both were returned to the shim's default 1023; the operator's own gesture then left them at **ch1 920 / ch2 1005**, which are theirs and were not touched. See [07](07-touch.md) § *Two fingers at once* | each hand moving **the same** channel, or the left fader moving when the right finger drags ⇒ the alternative contact is not being kept separate (check `pointer_report_alt` is fed `rx[1]/ry[1]` and not the primary's). A second hand that moves **nothing** ⇒ either the drill sent a still finger (the kernel dropped its repeats — move both), or the `seen` guard is rejecting a contact that never reported a position. **Never inject at raw x 1884 at the strip rows** — that is USB STOP and it stops the operator's media |
| S4.1 | `speaker-test -D hw:CARD=DDJFLX4,DEV=0 -c 4 -r 44100 --format S24_3LE -t sine` with the FLX4's MASTER level up | **still needs ears** — this is the one step no log can answer. It plays each of the 4 hardware channels in turn, which both proves the card makes sound and identifies which pair is the RCA out versus the headphone jack, validating `AUDIO_MAP=master=0,1;headphones=2,3` | silence → the card's own mixer/level, not the shim (nothing of ours is loaded here); channels not 1:1 → logical count ≠ hw count, see below |
| S4.2 | launch `rbp`, `/tmp/audioshim.log`, load and play | **verified, and the silence that was blamed on this row was never in the audio path.** The log reads `format constants verified: S16_LE=2 S24_LE=6 S24_3LE=32`, `real set_format(32) res=0`, `negotiating 4 channel(s)`, `real hw_params res=0`, `resolved 4 channel(s): master=0,1 headphones=2,3 booth=-1,-1`, and `writei frames=64 bytes=768 written=64` at ≈realtime with **zero** negative returns — 768 bytes ÷ 64 frames = 96 bits/frame is exactly the card's `FRAME_BITS` ceiling, so the packing is byte-correct. The port was nevertheless **silent with a deck playing** (2026-09-26), and the log said why once `peak_m` was read rather than skimmed: `peak_m=0` on all 242 blocks — digital silence *from rbp*, pre-gain. rbp's mixer builds its **channel faders at zero**, and nothing on this port ever set them. Seeded at unity from the shim ([09](09-audio.md#audibility-rbp-builds-the-channel-faders-at-zero)); audio now flows with no operator action | `written=-22` in a loop ⇒ the format constant or the device string, see [12](12-troubleshooting.md); **silence with a healthy log ⇒ read `peak_m` first — zero means rbp is silent and it is the mixer, not the shim** (the `0x3C665C` patch was the old answer and was not this); distortion ⇒ sign-extension. **A storm of `written=-22` with the log growing at ~1 MB/s, or a player that is visibly alive and never paints ⇒ the master's device opens and refuses every write**, which is S4.10 — the log's `open(` lines name it, and one `MASTER LOST: … never carried a frame` followed by silence is the containment working, not the failure |
| S4.3 | PFL/cue buttons, cue mix and level | cue bus changes, no combing | summing master+cue instead of routing the phone stream |
| S4.4 | leave it playing and pull the FLX4's USB, then plug it back | **run by accident, and it failed: audio did not come back.** The unit's under-voltage dropped the FLX4 and it re-enumerated 5 s later (`USB disconnect, device number 3` → `new high-speed USB device number 7 ... DDJ-FLX4`). The card is present again and `/proc/asound/cards` lists it, but rbp's PCM was opened against the old device, so every write after that returned `written=-19` — ~5.1 million of them — while `peak_m` went on showing music, because it is measured from rbp's own buffer. There was **no reopen path**; the workaround was to restart the player. **Now fixed in the shim** (2026-09-26): the write path detects `-ENODEV`, retires the dead handle and reopens with the negotiation replayed ([16](16-input-and-hotplug.md#4-hot-swap), [09](09-audio.md)). **Re-run and passed, 2026-09-28** — the operator pulled the FLX4 mid-play **twice** with the watcher on: one `MASTER LOST` per pull (`after 181536 write(s) on that handle`, then `after 222629`), the reopen attempted once a second with `res=-19` while the card was away, then `MASTER RECOVERED` with the whole negotiation replayed (`access=3`, `format(32)`, `channels(4)`, `rate=44100`, `period=64`, `periods=2`) and `writei … written=64 peak_m=159251` on the new handle — audio back, no restart. Both pulls carry S8.6's half of this row and S9.8's. **How it fails now** — see S8.6. The *different* failure reported in the field the same day — pulling the FLX4 while a track is playing makes the audio slow down — is diagnosed and fixed as well: it was the shim's own cardless block clock, not the swap (S4.10). This row's preserved log is where that shape is invisible rather than absent (`dropped=0 lost=0`, `writei … written=64` throughout) — a clock is not content, so no counter here counts it — and both post-fix pulls came back sounding right | a persistent `written=-19` **after** the shim says `MASTER RECOVERED` ⇒ the replay did not take, and the log names the step that failed; `-19` with **no** `MASTER LOST` line ⇒ the `-ENODEV` guard is not being reached, which is a different defect. Always check `dmesg -T \| grep -i usb` for a disconnect/re-enumerate rather than a bad cable, and note this is not the same `-19` as a startup `open()` failure ([09](09-audio.md)) |
| S4.5 | **HDMI mirror, log criterion.** With `RB_AUDIO_MIRROR_DEV` at its default, launch `rbp` and read `/tmp/audioshim.log` **and** `/proc/asound/card2/pcm0p/sub0/status` | **Pass, 2026-09-27**, on the deployed shim (`audioshim.so` md5 `5d845e557e7162974cbea4fb4559c0b2`). The log reads `mirror dev="hw:CARD=vc4hdmi0,DEV=0 hw:CARD=vc4hdmi1,DEV=0" fmt=subframe_le reopen=5000ms candidates=4`, then `mirror OPEN hw:CARD=vc4hdmi0,DEV=0 … period=256 periods=4 buffer=1024 prefill=512`, then `mirror UP … (first block delivered 64 of 64 frames)` — and the per-500-block line advances **1:1 with the master's** `writei #N`: `mirror #50001 blocks=50001 frames=3200064 dropped=0 short=0 retries=1 lost=0 pads=56 padframes=1728` — as it read on this run; the line gained a trailing `gain=` field on 2026-09-27 when the mirror's level was added ([09](09-audio.md#the-level-the-units-own-master-level-knob)), a `padsilent=` field on 2026-09-30 when the pad's content changed from silence to a repeat of the last delivered frame ([09](09-audio.md#two-clocks-no-resampler)), and a `clips=` field the same day when `RB_AUDIO_MIRROR_BOOST_DB` lifted the mirror's level — whose startup line carries the matching `boost=%.2fdB (x%.4f)` ([09](09-audio.md#the-lift-rb_audio_mirror_boost_db-and-the-clamp-it-needed)). So a line quoted before any of those dates is missing fields, not wrong. **The hold itself was read out of the running process the same day** (`readelf -sW` symbol offsets read through `/proc/<pid>/mem`, the same instrument the exact audio counters use): `g_mirror_last_bytes` 8, and `g_mirror_hold` left by the last pad as **64 copies of one 8-byte frame** — the replication, on the unit, not just in `test_audio`. What that run could not show is a *non-zero* held frame, because nothing was playing (`peak_m=0` throughout); the frame's content is the music, so that reading needs a track. The sink's own view agrees: card 2 `RUNNING` with an `owner_pid`, `format IEC958_SUBFRAME_LE`, `channels 2`, `rate 44100`, `period_size 256`, `buffer_size 1024`, and `delay` in a 488–640 band around the 512-frame target over three minutes — never near 0 (an XRUN) and never near 1024 (a filled ring). `retries=1` there is that run's *first* open having needed one attempt — the counter counts reopens, so it reads whatever the run's start cost and then must not move; the run deployed later the same day reads `retries=0` over 158 001 blocks because its first open succeeded outright | `mirror NONE — none of the N candidate(s) opened` ⇒ nothing is on that HDMI port, or the sink refused the format; the line above it names the step and the `res=` (`set_format` ⇒ the format, `open` ⇒ the port). `written=-22` or a `mirror LOST` loop ⇒ re-read the chroot-vs-host libasound trap in [09](09-audio.md#the-hdmi-mirror) before changing the format: the chroot's libasound offers the vc4 PCMs **only** `IEC958_SUBFRAME_LE`, and a host `aplay` will disagree. `retries` climbing ⇒ the ring is starving again, i.e. the prefill/pad path regressed; `dropped` climbing while `frames` stalls ⇒ the sink is genuinely slow, and that is the counter doing its job |
| S4.6 | **HDMI mirror, ears.** Play a track with the master audible on the FLX4, and listen to the monitor's own speakers | **Run 2026-09-27, and it failed first — which is the whole point of the row.** The operator's report was *"loud and very distorted"* against a mirror whose every counter was perfect (`dropped=0 short=0 lost=0`, `delay` in its band, 1:1 with the master). It was the subframe layout, and the row is the only thing that could have found it: `IEC958_SUBFRAME_LE` is not "a 24-bit sample left-justified in the 32-bit word" — that reading (`value << 8`) is what `s24pack.c` first shipped, and at bits 8..31 the sink reads bits 4..27, finds the sample's **low 20 bits promoted to the top**, and plays a wrapped, aliased waveform at full level. It is a genuine IEC 60958 subframe instead: bits 0–3 preamble, **4–27 the sample (LSB at bit 4)**, 28 validity, 29 user, 30 channel status, **31 even parity over 4..30** — alsa-lib's own layout, whose encoder does `data >>= 4; data &= ~0xf;`, and the vc4 DMA "does almost no repacking between the FIFO submission and the wire". The driver agrees and is why the card offers this name *alone* (`.formats = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE`); it does **not** set `VC4_HD_MAI_CTL_PAREN`, so the shim must supply bit 31 itself. Fixed in `s24pack.c`, and the operator's second verdict is **"HDMI audio is great now"** — so the layout, the parity and the framing are now measured end to end rather than reasoned ([09](09-audio.md#the-format-is-not-negotiable-and-neither-is-hw), [s24pack.h](../scripts/shims/s24pack.h)). One more thing this bought: the test's own parity assertion had been masking bit 31 off, so it could not see the parity the packer was correctly writing and reported four failures against a *correct* packer | anything but recognisable audio, with S4.5 healthy, is the byte layout — and it is worth stating how it presents, because it is not silence: a wrapped, full-level waveform. Check `/proc/asound/card2/pcm0p/sub0/hw_params` names `IEC958_SUBFRAME_LE` and rate 44100 ch 2 first, then re-read the layout in `s24pack.c`'s `AUDIO_FMT_SUBFRAME_LE` — the mask is `0x0ffffff0` with the parity bit OR'd in, i.e. a `<< 4`, and a `<< 8` there is this row's original defect. Silence with a healthy S4.5 ⇒ the sink's own input selection, or the monitor's volume — the Pi's HDMI output has **no mixer and no volume control at all** (only `HDMI Jack`, `ELD`, `IEC958 Playback Default/Mask`, `Playback Channel Map`), so there is nothing on this end to unmute |
| S4.7 | **No regression on the master.** Run S4.5 with a deck playing, and watch the FLX4's side of the log and the service | **Pass, 2026-09-27**, and this row outranks the mirror: `writei #N … written=64 peak_m=…` cadence unchanged and 1:1 with `mirror #N`, card 1 (`DDJFLX4`) `state RUNNING`, `format S24_3LE channels 4 rate 44100 period_size 64 buffer_size 128`, `NRestarts=0`, one `MainPID`, and **no** `MASTER LOST` / `MASTER RECOVERED` line anywhere in the run. The mirror's own corrections (prefill and pad) are silence written to *its* handle, so nothing in them can reach the card's stream | **any** xrun, any changed `peak_m` behaviour, any lost block, or `NRestarts≠0` ⇒ the mirror gets reverted, per [09](09-audio.md#the-hdmi-mirror)'s rule that the FLX4's audio is the one thing that must not change. A `written=-19` here is the S4.4/S8.6 subject, not this one |
| S4.8 | **The drift, held.** Leave it running for several minutes and re-read the mirror's counter line and the card's `delay` | **Pass, 2026-09-27.** Over three minutes (`mirror #66501`→`#177001`, ~160 s of audio) `state` was `RUNNING` at every sample, `delay` stayed in a 520–640 band, `retries` stayed at 1, `lost=0`, `dropped=0`, and `padframes` advanced 648→1728 — **6.75 frames a second**, which is the measured clock difference between the vc4 HDMI sink and the FLX4 (~155 ppm) appearing on the log as the correction that absorbs it. Without the correction the ring drains in ~74 s and each recovery restarts the stream, i.e. an 11.6 ms gap in the monitor's audio every minute and a quarter | `delay` trending to 0 and back ⇒ the XRUN-and-restart path is being taken, so the pad is not running (check for `pads` never moving, which is what an unresolved `snd_pcm_avail_update` looks like); `dropped` climbing instead ⇒ the sink got slower than the source and the drop side is handling it, which is the designed behaviour but worth recording as a change in the unit's clocks |
| S4.9 | **The mirror's level follows the unit's MASTER LEVEL knob.** With a track playing, turn the knob end to end and watch the `gain=` field of the mirror's counter line — then listen | **Half run, 2026-09-27 — the log half is MEASURED (both the 14-bit decode and the level law), the ears half is the operator's and is outstanding.** The knob is a **14-bit pair** — list channel 7 (0-based 6 = `CH_MIX`), **CC 8 (MSB) + CC 40 (LSB)** — and it writes `g_mirror_gain`, the HDMI mirror's level, and nothing else. **A hand sweep of the knob moved the field through 0.136 … 1.000 in small consecutive steps** (`1.000 → 0.910 → 0.871 → 0.737 → 0.717 → 0.616 → 0.501 → 0.484 → 0.479 → 0.477 → 0.202 → 0.136 →` back up, every one an `%.3f` of a different position, not a 128-step ladder). **That sweep by itself falsifies the two single-CC readings**, which no assertion in the suite can do: a map reading either CC as the whole 7-bit value can only ever print `k/127`, and **`0.501` is not one** (63/127 = 0.496, 64/127 = 0.504) and neither is **`0.616`** (78/127 = 0.614, 79/127 = 0.622) — as are 0.202, 0.477, 0.479, 0.484, 0.737, 0.871 and 0.910. So the pair is genuinely composed on hardware, exactly as `test_flx4`'s `check_mastervol` pins it in the suite (eight gains across the travel, the half-pair negatives, the one-count-either-side-of-the-knee pair, and the two safety assertions that no rbp keycode was sent and `g_master_gain` stayed at 1.0; green at 535). **Those values are the first law's**, `gain = pos/16383`, and the mapping changed the same run — see below. **The new law was then measured on the unit with no hand on the knob**, by handing the controls shim a hand-written dump through `RB_MIDI_REPLAY` (`MIDI_REPLAY` drives the real `ctrl_dispatch` with nothing plugged in; the shim logged `10 events`): ten CC 8/40 lines walking the travel, replayed at speed 1.0, moved the field **1.000 → 0.000 → 0.500 → 1.000, and then no further change** for the rest of the run. **It is the shape of that series that identifies the law**, not any single value: the same positions under `pos/16383` print six distinct values (0.000, 0.250, 0.500, 0.750, 1.000, 0.500), and the middle — where the operator's knob is parked — prints **0.500**. It printed **1.000**, which is the report fixed. The master was untouched by the replay too: 154 `mirror` lines against 154 `writei` lines, `mainvol=1.000` the only value in the log, `MASTER LOST` zero, and `card1` and `card2` both `RUNNING` under one `owner_pid`. The hand sweep's own run measured the same thing — `mainvol=1.000` the only value in the whole log, the `writei` cadence 1:1 with the mirror's through `#402501`, `peak_m` moving throughout, card 1 `RUNNING` under one `MainPID`, `NRestarts=0`, no `MASTER LOST`. **The level was then reported wrong, and the law changed the same run.** With unity at the stop the mirror was too quiet to use — the operator's 2026-09-27 report, verbatim: *"it should in theory be loud enough at the middle and no needed to be cranked to the max"*. The middle is the working point of any volume knob and it was only 0.5, i.e. **−6 dB**. So `flx4_mastervol_gain()` moved **unity to the middle of the travel**, flat above it and linear below. **Then the knee moved again, the same day and on the same ear: the operator asked for the top of the useful travel to be 1 o'clock rather than 12**, verbatim *"maybe make the 1pm the max level vs 12 oclock"*, and unity moved up with it to **0.6 of the raw range**. That number is **measured from the knob's own travel, not assumed** — and the measurement came out of this very dump. `RB_MIDI_DUMP` was already on, so the knob's whole travel could be read back: the composed MSB/LSB positions span raw **0 to 16383**, and both ends are among the most-visited positions (14 samples at 16383, 11 at 0), which is what a mechanical stop looks like — the pot pins at the ends of its electrical range rather than stopping short of them. So `pos / 16383` is a fraction of *rotation*, and **12 o'clock is raw 0.5**: the electrical midpoint, and where the hand rests in the same dump, whose most-visited positions cluster on 0.479–0.532 centred on 0.50. 1 o'clock is one hour past that — 30° of a 270–300° sweep — i.e. **0.60–0.61** of the rotation, and 0.6 is the round number (0.15 dB from the other). **An earlier reading of this same dump derived 0.65 from a bottom stop of raw 2228, and that was wrong**: 2228 came from the hand sweep's *lowest logged gain* under the first law (0.136), and a sweep that never reaches the bottom prints a value indistinguishable from a stop — the very thing this row had already flagged as unrecorded. **What it costs, asserted rather than implied**: 12 o'clock now reads **0.833** (−1.6 dB) instead of full level, and since nothing in the law can exceed 1.0 the change only ever *lowered* what a given position gives. The bottom is untouched — raw 0 is the stop and still maps to silence, which the 0.65 premise would also have got wrong. The `gain=` field a replay prints at the middle is therefore **0.833**, not the 1.000 this run measured. `RB_MIRROR_GAIN_MID=1.0` reproduces `pos/16383` exactly and 0.5 restores the middle knee, so the knob is its own rollback, and a degenerate value (≤0 would divide, >1 would ask for a boost) falls back to 0.6 ([09](09-audio.md#the-level-the-units-own-master-level-knob)). **No boost above unity, deliberately**: `s24pack()` wraps its 24-bit subframe rather than clamping it, so a gain past 1.0 folds the waveform — S4.6's "loud and very distorted" — and `clamp01()` in `audioshim` is a ceiling, not a limit to raise. The master peaks at ≈ **−8.3 dBFS** (3229776 of 8388608, its loudest 500-block window this run), so headroom does exist, but using it means a saturating clamp in `s24pack()` first; that is a separate change and is not this one. What is still the operator's is the *ears*, now with the knee at **1 o'clock**: is 12 o'clock (0.833) still loud enough, now that it is deliberately below full level, does turning on to 1 o'clock reach a level they like, and does the bottom still go silent — the dump shows the pot reaching raw 0, so full-down is silence in the law, but no run has yet been *heard* there | `gain=` never moves while the knob visibly turns ⇒ first ask whether the CC arrived at all — `grep -cE "cc=(8\|40) " /tmp/flx4.dump` after S5.1 has the dump running: an **empty** dump is "the event never arrived", a filling dump with a still `gain` is the map. **The knob drops the FLX4's own output as well as the monitor's** ⇒ the value reached `rbp`'s master level or `g_master_gain` instead of the mirror, which is the double attenuation this design exists to avoid: the knob is *downstream* of the USB audio the Pi feeds the unit, so the samples are attenuated once in `rbp` and again in the unit's own output stage ([09](09-audio.md#the-level-the-units-own-master-level-knob)). The `writei` line's `mainvol` falling with it is the same defect, named directly — and it is **measured to be absent**, so this row's failure mode now has a live control. A level that moves in 128 steps rather than ~16383 ⇒ the MSB/LSB dispatch regressed, since **only the LSB completes a position** and an MSB is held for the next one; the log is what decides it, because a 7-bit ladder prints `2k/127` — a **flat 1.0 from the knee up**, and a 128-step ladder below it, where the true decode gives ~16383 steps. **Loud only with the knob near the stop, quiet below 1 o'clock** ⇒ the level mapping has regressed to the first law: the `gain=` field must read **1.000 at MSB 76 + LSB 102** (raw 9830, the first position to reach unity, with raw 9829 one count short of it) and **0.833 at MSB 64 + LSB 0** (raw 8192, the middle of the range, which is deliberately below full level now), and `env \| grep MIRROR_GAIN_MID` on the launcher must be empty or `0.6` (a stray `1.0` restores the stop law and `0.5` the middle knee, both deliberately, neither a fault). **Distorted** rather than loud ⇒ the mirror's saturation is being bypassed. Unity is no longer a ceiling on this path — `RB_AUDIO_MIRROR_BOOST_DB` (default +4 dB) sits *under* this law on purpose ([S4.13](#bring-up-order)) — and what keeps a lifted sample from folding into S4.6's waveform is `mirror_saturate()`, which clamps to the 24-bit domain at the gain and counts what it caught as `clips=`. The packers deliberately do not clamp (they are modular by pinned contract), so distortion means a path reached `s24pack()` without passing through that clamp. `clamp01()` stays for the other reason: it is what keeps the *knob* an attenuator. And expect the first touch to be a **jump**: the unit announces **no** knob position at connect — its connect report is a lone MSB, and a restart does not re-enumerate it, so over 113 501 blocks of delivered audio after a restart the MIDI dump held zero CC 8/40 lines and `gain` read `1.000` throughout. There is no position to restore, so every run starts the mirror at unity |
| S4.10 | **Boot with no controller at all.** Stop the player, make the card absent — unplug the FLX4, or, for an ssh-only run, `echo 1-1.2 > /sys/bus/usb/drivers/usb/unbind`, which is the path a physical unplug takes — then start it again | **Pass, 2026-09-27** (run by unbinding the device). Two `open(…) res=-19` lines — `hw:CARD=DDJFLX4,DEV=0` and its `plughw:` twin, **nothing else tried**, `grep -c "open('default')"` = **0** — then one `NO OUTPUT DEVICE — rbp will run silent and every stream will sleep to pace itself`. `grep -c "writei #"` = **0** and `grep -c "written=-22"` = **0**; the log stayed at 24 KB (1% of tmpfs) where the defect produced 449 MB in eight minutes. Thread `JuceALSA` read **5.0%** of a core against the defect's ~43% — the process total is still ~46%, and that is `gui_task` drawing the UI. The screen was confirmed by reading `/dev/fb0`: identical over 3 s with no input, and a key injected with `vkeyd.py` changed the hash to a new value that then held, so input reaches the UI and the UI paints. **What this row did not claim was the cardless rate, and 2026-09-28 settled it — with two defects, and the second was invisible to the arithmetic that found the first.** That inference was right as far as it went: `pace_without_device()` was called *per stream*, rbp holds three, and a cardless block measured **216 blocks/s** where 3 × (1451.247 + 64) µs = 4545 µs predicts 220.0 — a third of real time. One paid sleep per block (counted in `stage.pace_seen` by `pace_secondary()`) recovered that third and left the operator's *"still slow, and it sounds distorted"*: **625.0 blocks/s over a 12.5 s cardless window and 629.1/s over 33.1 s**, 91 % of real time, because the clock underneath was still `usleep(period)` per block and `usleep` sleeps *at least* its argument, so every block cost period + overhead — 167 µs on a 1451.247 µs block, ~64 µs of it the sleep's own wakeup latency and ~103 µs the block's work (the same 64 µs is what makes the three-sleep arithmetic above come out exactly). The mirror is what made it legible, because its drift correction reads the block clock out as silence: over the same two windows its ring fell **7.63 and 7.53 frames per 64-frame block** (617.9 blocks/s predicts 7.38) and it answered with ~4560 frames/s of pads against the ~6.9 frames a *second* two crystals differ by — 660× the card-present rate, 10.3 % of the monitor's timeline replaced by ~0.17 ms holes about a hundred times a second. `pace_without_device()` is now a **deadline per block, not a sleep per block** ([pace_policy.h](../scripts/shims/pace_policy.h)): the deadline advances one period, the caller sleeps only the remainder, and the block's work and its wakeup come out of the block's own period. **Verified on the operator's own mid-play pulls, 2026-09-28 (S9.8):** cardless **700.9 blocks/s over a 32.1 s window** and 666.7/s over 6.0 s against real time's 689 — real time within the instrument's resolution, where it had been 9 % slow, the rate being read from a `mirror #` line the shim prints every 500 blocks (±4 % on a window that length) — and pads **0.0096 frames per block, the card-present floor** (0.0092–0.0093 over 500 000-block spans) against 7.53–7.63 before, with `drop`/`short`/`lost` at 0, `retries` at its startup value and `gain=1.000` through both. Passed by ear, which is the only instrument that can. The unit boots, stays responsive and recovers | **a storm of `written=-22`, a log growing ~1 MB/s, or `JuceALSA` pegged ⇒ a device that opens and refuses every write is back in the chain**, i.e. a third rung the configuration did not name; the `open(` lines name it. A UI that does not paint with all of the above healthy ⇒ the display, not this ([12](12-troubleshooting.md)) |
| S4.11 | **Attach a controller to a unit that booted without one** — S4.10, then plug the FLX4 in (`echo 1-1.2 > /sys/bus/usb/drivers/usb/bind` for the ssh-only run) | **Pass, 2026-09-27**, and this row's first line is the whole of it: `replay: access=3 channels=4 rate=44100 period=64 periods=2 …` — access 3 because the replay always asks for `RW_INTERLEAVED`, and **channels 4, not 2**. Then `resolved 4 channel(s): master=0,1 headphones=2,3 booth=-1,-1 monitor=-1,-1`, `the pair map was resolved again against hw:CARD=DDJFLX4,DEV=0 and is complete: … on 4 channel(s)`, `startup mute released after 79424 frames`, and `writei #N frames=64 bytes=768 written=64 … prepares=1` thereafter. The mirror's counters stop degrading at the same moment: `pads` 39876 → 39878 and `retries` frozen over the next 9000 blocks, where before the plug-in both advanced every block. The controller's other halves return by themselves as well — all seven USB interfaces re-bind (`snd-usb-audio` ×5 and `usbhid`), the MIDI node reappears as `midiC3D0`, and the controls shim logs `control surface: subscribed to 28:0 'DDJ-FLX4 MIDI 1'` then `LED/meter output route: sequencer 128:1 -> 28:0 'DDJ-FLX4 MIDI 1'`. What is **not** in this row is the ears: the cue on the FLX4's headphone jack is the operator's, as S4.1 is | the cue missing **with** the "only the master pair" note ⇒ the card really is 2-channel, and the note is the answer rather than a fault; no re-resolution at all ⇒ the cold branch needs `g_master_frames_delivered == 0`, so a shim that had already delivered a frame will keep the old map deliberately; `-22` on every write *after* a successful replay ⇒ first read the `replay:` line: it must say `access=3`. If it does, the access is not the variable and the card is — reproduce with `tools/pcmprobe … 32 nonblock 3 4` before touching the shim; if it does not, an old shim is loaded (check the `.so`'s sha256 against the build); the map right and `writei` fine but still silent ⇒ `startup mute released` must appear — if it does not, the mute was never re-armed |
| S4.12 | **The mirror went silent with every counter perfect** — not a planned drill: the operator's report on 2026-09-28 (*"i'm not sure why but it doesn't sound like HDMI audio is working anymore"*) | **Run 2026-09-28 by accident, and the finding is that nothing on this path could see the fault.** The sink was the CX101 on `HDMI-A-1` (`card1`), connected, and its EDID advertises exactly what the mirror sends — LPCM, 2 channels, 32/44.1/48 kHz, 16/20/24-bit, basic-audio flag set, speaker allocation FL/FR — and every counter was clean: `mirror #994501 … dropped=0 short=0 retries=1 lost=0 gain=0.852` 1:1 with the master's `writei` (whose `peak_m` was moving, so rbp really was playing), the PCM `state: RUNNING` with `hw_ptr` advancing **132 608 frames in 3 s = 44 200 frames/s** — the hardware was consuming the audio at exactly the right rate — `delay` 488–584 about the 512-frame target, and **not one error line of any kind**: no `LOST`, no `NONE`, no `MASTER LOST`, `NRestarts=0`. **Recovery: restarting the player**, which closes and reopens the mirror's PCM (`mirror OPEN hw:CARD=vc4hdmi0,DEV=0 …` → `mirror UP … (first block delivered 64 of 64 frames)`), and the operator's ear said the sound was back — **but which of the two candidate cures it was is not settled**: that restart, or the monitor's own replug / power cycle at 02:29:10 and 02:32:25. The operator's own reading is the monitor — *"i suspect re-plugging it or me turning it off or on fixed the issue"* — which would make this a **sink re-lock** rather than the shim's handle, and it is the reading the evidence leans to as well, since a 12 s replug was measured to leave the mirror completely undisturbed (below). So: the failure is recoverable by hands, and *which* hand is the open question. **Which event put it there is not measured.** Two things were measured that night, and both weaken the obvious suspects. **(a) An HDMI re-probe does not break it:** across a ~12 s unplug (02:29:10–02:29:22) and a one-sample one (02:32:25) the mirror's writes never failed, `dropped`/`short`/`lost`/`retries` never moved and `hw_ptr` kept advancing 24.2M → 32.8M frames *with the cable out* — the ring drains into a port with nothing on it. There were four `[drm] User-defined mode not supported: "1280x800"` re-probes that night (02:13:06, 02:29:10, 02:29:44, 02:32:25), and **three of the four are now confirmed connector drops** — see (c). **(b) The framebuffer cannot see a modeset:** `FBIOGET_VSCREENINFO` on `/dev/fb0` returns `pixclock`, `hsync_len`, `vsync_len` and all four margins as **0** (measured 2026-09-28), so the only fields that move are `xres`/`yres`/`bpp` — the geometry the display watcher already watches, and precisely what a same-size mode change does *not* change. What a detector *could* use: `status`, `enabled`, `dpms`, `modes` and `edid` under `/sys/class/drm/card*-HDMI-A-*/` all move on a sink event (a disconnect reads `status=disconnected`, `enabled=disabled`, `dpms=Off`, with both `modes` and `edid` empty). **(c) A one-second sysfs sampler caught the connector actually leaving, and it left while the PCM kept draining.** `/root/hdmiprobe.sh` (one sample a second into `/root/hdmiprobe.log`, capped at 3600) ran from 02:24:53 and across 1 855 samples saw exactly **two** states: `connected On enabled modes=8ad667fd edid=1e0113e0` (1 841 samples) and `disconnected Off disabled modes=d41d8cd9 edid=d41d8cd9` (**14**). Those 14 fall in three runs — **02:29:10–02:29:21 (eleven one-second samples, so between 11 and 12 s out), 02:29:44–02:29:45, 02:32:25** — which are precisely the three dmesg re-probes at those times, so **a `User-defined mode not supported` re-probe line *is* a connector drop**, measured rather than assumed. Through the 11 s run the PCM did not stop: `card1` stayed `state: RUNNING` with `hw_ptr` 24 182 496 → 24 645 088 and the mirror's own block counter climbing 756 → 771. **That closes the row's central question on both sides at once**: the connector really reported the sink gone, with `modes` and `edid` reading the empty-file md5 **`d41d8cd9`**, while the shim went on writing and the driver went on consuming — the two halves were previously known separately, from `hw_ptr` here and from a dmesg line there. The 02:13:06 re-probe is *before* the sampler started, so it is unmeasured; by this pattern it was a fourth drop. **And one instrument caveat, because it bit twice in one evening:** a rate derived from a sampler's own `%H:%M:%S` stamps overstates the truth, because each iteration's work adds to the real interval while the stamps still advance one second — the sampler's own log gives 46 500 frames/s and my own loop gave 45 022, while the honest figure is **44 116 frames/s** (epoch-bracketed bare `sleep 30`: elapsed 30 s, delta 1 323 496), i.e. 44 100 plus the mirror's pad, which is the same figure the 3 s sample above read slightly high as 44 200. Bracket the interval with `date +%s` and put *nothing* inside it — a `grep -c` over the shim log inside the window is enough to corrupt it. **What this does to the timeline, as an inference and not a measurement:** the operator was still pulling the cable at 02:29:44 and 02:32:25, nine and thirteen minutes *after* the 02:19:58 restart, which reads as the restart not having restored the sound and the monitor being what did — the same reading the operator gave of it. **What the row leaves is one forward test**: play a track, pull the monitor's cable, plug it back, and listen — if the sound dies, a detector is worth building and the mirror can reopen on it; if it survives, the fault is the sink's own (input selection, volume, a power cycle — none of which the Pi can see) and the honest answer is the restart above. **That forward test now has its answer, 2026-10-01 (S8.8), and it is the second branch** — nothing was audible across a real one-second HPD drop — with this row's own caveat standing: what the operator heard was the *master* (FLX4), which the mirror cannot disturb by construction, and the sink event was a monitor-side source change that no Pi-side detector could ever attribute to the sink rather than to rbp | `dropped`/`short`/`lost` climbing, or a `writei` going negative ⇒ a *different* class of failure, and the mirror's own recovery should already be handling it (S4.5, S8.8). Silence with the counters clean ⇒ **the counters are not evidence the sink is being fed** ([09](09-audio.md#the-counters-cannot-tell-you-the-sink-is-being-fed)): restart the player first, it is the one known cure, and check the sink's input and volume before touching the shim. A `gain=0` in the counter line ⇒ the knob, not this row |
| S4.13 | **HDMI mirror, level lift.** Set `RB_AUDIO_MIRROR_BOOST_DB` (default `4`), restart, play a track, and read the mirror's delivered samples back out of the running process | **Measured 2026-09-30, on material.** *The delivery costs nothing:* a **120.0003 s** exact-counter window returned mirror `+5291328` frames **= the master's `+5291328`** at `44094.290 Hz` (−129.5 ppm, the FLX4's crystal), `padsilent=0`, every fault `+0`. *The lift is applied, exactly:* reading `g_mirror_out` (`0x17d24`) beside the master's own `g_out` (`0x4fd44`) **in the same block** — the block counter brackets both reads and a pair that straddles a fill is discarded, which is the whole of the residual scatter (the window between the fill and `g_mirror_blocks++`) — gives a ratio of exactly **1.5849** on 21 570 of 26 604 sampled blocks; end to end, the loudest master sample of the window was **3 813 785** against the mirror's **6 044 442**, the expected ×1.5849 to within truncation. *And it clips on the loudest material:* `clips=3739` cumulatively (and **the trim change closes it: with the trim at its 12 o'clock detent a **160 s / 110 237-block** window on the same material gave `clips +0`, `padsilent +0`, the loudest rbp sample at −6.85 dBFS and the mirror at −2.85 dBFS — 2.85 dB of margin**) — all of it in one burst around blocks 858 501–998 501, none since — because rbp's own peaks reach **7 132 566** (−1.4 dBFS) on that track, from which ×1.5849 cannot fit inside 8 388 607. Note the earlier headroom figure (−8.3 dBFS, the loudest 500-block window of a *different* run) was not this material | `clips=` climbing ⇒ the lift is past what the material, **at that TRIM**, can carry. Two levers, and the second is the one to reach for: the setting (one `rb.conf` line and a restart, window −24…+12 dB, `0` restoring the old behaviour) — or rbp's own level, which the FLX4's **TRIM** sets and which `peak_m` reports. Measured 2026-09-30: with the trim hot, `peak_m` reached −1.41 dBFS and +4 dB clipped; with the trim at its 12 o'clock detent, `peak_m` fell to **−6.85 dBFS** and the mirror landed at **−2.85 dBFS** with `clips +0` — 2.85 dB of margin, and the room's 5.4 dB is recovered on the FLX4's own MASTER knob, which above its knee moves the room and not the mirror ([09](09-audio.md#the-lifts-headroom-is-rbps-own-level--trim-buys-boost)) — and the arithmetic is strict: with rbp's own peaks at **−1.41 dBFS** on this material, **+1 dB** lands the loudest at −0.41 dBFS (it fits) while **+2 dB** puts it at +0.59 dBFS, over again. So the largest boost that cannot touch the ceiling *on this track* is +1 dB. **A waveform that sounds torn rather than merely loud is not this** — that would be a wrap, and the wrap is closed: `mirror_saturate()` clamps to the 24-bit domain *at the gain* before packing, which is the clamp `s24pack()`'s modular contract forbids putting inside the packers ([13](#bring-up-order), S4.6 measured the wrapped version; [09](09-audio.md#the-lift-rb_audio_mirror_boost_db-and-the-clamp-it-needed) has the argument). **Whether 3 739 single-sample flat-tops in two bursts — about 0.02 % of the samples in those minutes — is audible is the operator's ears alone** |

| S5.1 | `aseqdump -p <client>:0` while pressing everything, piped into `tools/aseqdump2dump.py --stats --revs <N> -o flx4.dump` | **run — and it converted the map's biggest guess into a measurement.** The inventory is in [15](15-flx4-midi.md); the relative controls use the 0x40-centred convention (platter CC 34 vinyl-on / 35 vinyl-off / 41 `+SHIFT`, jog ring CC 33), and one counted turn gives **720 counts/revolution**. `map_flx4.c`'s `jog_ppr` is now 720: the inherited 128 was the *JP21's* platter, which made every turn 5.6× too fast and pinned it to the 8 rev/s clamp. Still `TODO: unverified`, and a smaller set than before: the pitch fader's polarity, the pad base+pad encoding and its SHIFT pairing, the FX knob targets, BEAT SYNC long-press and 4 BEAT/EXIT | nothing → `snd-seq` / USB |
| S5.2 | `MIDI_DUMP=… RB_MIDI_MAP=flx4`, press each control | **run (2026-09-26), and it is the row that distinguishes a real press from a synthetic one.** The dump records both, and they are told apart by velocity: `seqinject2` sends **100**, the FLX4 sends **127**. So `grep 'vel=127'` is the panel. Note 65 (browse push) and note 70 (LOAD) are **measured** this way; **note 66 (SHIFT + browse push) was measured the same way on 2026-09-30** — the one thing this row said was outstanding, and it is now settled rather than still published-only: the dump holds 12 `note=66` lines, all at vel 127, each falling strictly inside a SHIFT-held window, with `note=65` appearing nowhere ([15](15-flx4-midi.md#the-shift--browse-push-and-the-two-questions-it-is-two-questions-about)). **The shifted LOAD notes 104 / 122, and note 71 (LOAD deck 2), were answered the same day and by the same one-press procedure** — the operator pressed all three, both screens came up, and the dump caught all three on the wire at the panel's own velocity: `note=104 ch=6 vel=127`, `note=122 ch=6 vel=127`, `note=71 ch=6 vel=127`, with no `vel=100` anywhere in the file, so nothing injected is in it (each of the two SHIFT keycodes is bound by exactly one row, both on ch 6, so a wrong number could not have fired at all) ([15](15-flx4-midi.md#the-shift--load-bindings-and-the-edge-rbp-needs-that-a-tap-does-not-send)) | wrong channel/note assumptions — that is the dump's purpose. A silent "the button does nothing" may also be the map, not the note: check `subscribed to` in the log to tell "the event never arrived" from "the event arrived and was mis-bound". This row's old advice — that `MIDI_MAP` selects exactly one map and under `kbd` the FLX4 is ignored — was wrong twice and has been removed; the two selections are independent ([16](16-input-and-hotplug.md#two-selections-not-one)) |
| S5.3 | `make -C scripts/shims test` under `qemu-arm` | **eight suites, all green** (2026-09-26): `test_point` (55 checks), `test_audio` (50), `test_midi` (170), `test_flx4` (500), `test_kbd` (367), `test_cursor` (920), `test_evdev` (5 scenarios / 53 checks) and `test_cursor_dev` (14 scenarios / 74 checks). The last two fake the kernel at the `syscall()` boundary — `test_evdev` for the input reader, `test_cursor_dev` for `fb_cursor.c`, which had no coverage at all before it | off-by-one on a CC pair or threshold |
| S5.4 | no configuration needed: with `EVDEV_MAP=kbd` (the default) and the FLX4 on `MIDI_MAP=flx4`, press a keyboard key **and** a controller pad | **both reach rbp** — this is the operator's original "the keyboard isn't doing anything" reversed. Then `1` to load and `space` to play, `↑`/`↓`, `Esc`, right-click. (This row used to say `RB_MIDI_MAP=kbd` first, which is no longer required and no longer sufficient on its own as a description: the keyboard has its own variable.) Note `KNOB_VERBOSE=1` still costs a log line per key and should be off before measuring anything about timing | wrong keycodes; **the selector's direction is the one thing only hardware settles** (see 08); a key that works once and stops ⇒ a lost release, see [16](16-input-and-hotplug.md) |
| S5.5 | pad LEDs (`LED_VERBOSE=1`) | pads mirror | the colour table is a known TODO |
| S5.6 | **The FX CH SELECT lever**, moved CH1 → CH2 → CH1&CH2 → back to CH1, watching `/tmp/knobshim.log` **and rbp's own Beat FX panel**. This is also the row that reads back what rbp does with `K_BFXCH`, which nothing here had measured | **PASSED by injection 2026-09-28, and it settles the whole control, not just the third position.** The lever is two notes rather than a position ([15](15-flx4-midi.md#the-map)), and the third position — CH1&CH2 — had no value of its own, because rbp's Beat FX has **one** target channel. It is now sent as **MASTER** (`0x448c` value **5**, `BFX_CH_MASTER`), the output both decks reach, and what the map tracks is the **lever's position** rather than the last keycode, so leaving CH1&CH2 for the deck rbp was already told about still lands. Injected through the real dispatch (`seqinject2 note 4 16 / note 5 17`, the same route the panel takes), the log gives **four** sends for the seven injected events and in order — `target deck 1 (0)`, `deck 2 (1)`, `MASTER (5)`, `deck 2 (1)` — with the two events that name no position (mid-slide, and the always-OFF note) sending nothing. **And rbp acts on the value**: a frame capture at each position reads the Beat FX panel's target line as **`1`**, **`2`** and **`MASTER`**, so the values are read off rbp's own screen rather than assumed from the enum. The MASTER-vs-CH1 diff is **691 px confined to x 1134..1225, y 178..194** — that one line of the panel and nothing else — while CH1-before vs CH1-after is **416 px of the UI's free-running blink marks**, i.e. no change at all. Host side, `test_flx4`'s expectation table and `tests/midi_flx4.dump` carry the four sends in order (580 checks green) | a lever move that logs nothing ⇒ the legs, not rbp: `KNOB_VERBOSE=1` prints the note that arrived and from which channel. **A move *out of* CH1&CH2 that leaves the panel reading MASTER ⇒ the map is comparing the last keycode instead of the lever's position.** A log line with no panel change ⇒ `K_BFXCH`'s value, which is what this row exists to catch. **The lever was moved by hand through all three positions on 2026-09-30 and rbp followed all three** — the map logged `deck 1 (0)`, `deck 2 (1)`, `MASTER (5)` in that order and the dump holds the leg pattern beneath each (CH1 = `ch4 note16` alone, CH2 = `ch5 note17` alone, CH1&CH2 = both), so the third position reaching rbp as MASTER is now proven under a hand and not only by injection. What that run also settled is why the connect half is hard: **the panel volunteers nothing for the lever** — the FLX4's opening events are controller values (cc 33/39/7/64 in the first second), and the *first* lever event in the whole 4254-line run is the operator's own move, because a note carries no value to volunteer. So a cold start leaves rbp on its own default until the lever is touched, **and that default is measured now: deck 1.** Two cold starts (03:25:37 and 03:28:49 EDT, 2026-09-30 — the first with the lever left at CH 1 by the operator, the second with no operator at all) each read the Beat FX `CH SELECT` box as **1** off rbp's own framebuffer, with `grep -ac "FX CH SELECT" /tmp/knobshim.log` = **0** for the run. The second is the one that separates "built-in default" from "restored": leg B was injected first through the shim's own port (`seqinject2 note 5 17`, the route the panel takes) and **rbp moved to 2** — the control — then the unit was restarted with no lever traffic at all and rbp came back at **1**, in a process whose predecessor had been left at 2. `XdjSettings.dat` is not the carrier either: its mtime held at 02:07 through the target change and both restarts. **So rbp does not inherit the lever's last position across a restart, and a power-on with the lever away from CH 1 shows and acts on deck 1 until the lever is nudged** — a habit, not a defect, since the surface names nothing at connect ([15](15-flx4-midi.md#open-questions) item 7) |
| S5.7 | **The two repurposed pad-mode buttons** — press PAD FX 1, run the pads; press SAMPLER, run the pads; then press each one again. This is the operator's pad-mode request against the four modes rbp actually has ([15](15-flx4-midi.md#pads-list-ch-810--rch-79)) | **The map side is done and proven to dispatch (2026-09-28), and what rbp does with those two keycodes is settled as of 2026-09-30 — it acts on them, and the pad grid changes mode on the glass (see the correction further down this cell, which replaces an earlier wrong reading). What is still outstanding is the panel's LED feedback, which is undriven, and the finger on the pads themselves.** rbp's dispatch tree (`ui::PlayerInnards::onPhysicalKey` @ `0x306b78`, emulated for all 65536 keycodes) has **exactly four pad modes** — HOT CUE (0), AUTO BEAT LOOP (1), SLIP BEAT LOOP (2), BEAT JUMP (3) — confirmed three ways: the `onKey_Pad` jump table, `getStat`'s exported pad-mode byte, and `ui::Player`'s four `check*LedState` functions. **RELEASE FX, PAD FX 1/2, SAMPLER and KEYBOARD do not exist in rbp at all** — no keycode, no mode value, no such string in the binary — and SLIP LOOP is the same mode as SLIP BEAT LOOP, so four of the six targets in the original request have nothing on the rbp side to select. The unit's PAD FX 1 (note 30) therefore sends `0x4114` K_ALOOP and its SAMPLER (note 34) sends `0x4115` K_SLIPLOOP, **and the pad bases those two modes use (16 and 48) are bound with them** — without those the two modes would come with eight dead pads, because the unit really does move its pads to the new base when the button is pressed. `test_flx4` is green at **619 checks**, with a press/release pair for each button and one pad in each of the four bound bases in the fixture. The change also exposed a **silent ceiling**: `CTRL_NKEYS` was 96 with this map at 99, so `add_note()` had been dropping the last three rows — a mixer note and both Beat FX notes — without printing anything; raised to 192 with the hazard written at the constant. **CORRECTED 2026-09-30 — the mode does move, and the operator's eyes caught what this measurement missed.** The note below is kept as the tell, not as the state. Pressing PAD FX 1 does change rbp's screen; what stays dark is the panel's pad-mode LED. Measured the operator's way — one key at a time, rbp's framebuffer read after each — **note 27 → deck 1's pad grid reads `HOT CUE` (A–H), note 30 → `BEAT LOOP` (1/4 … 32), note 32 → `BEAT JUMP`** (1264 px and 5801 px of pad-grid change), and the dispatcher agrees from the other side (`ch0 note30 -> 0x4114 press`, all four keys, both edges). Superseded, and worth keeping because it shows how a measurement lies: **And the mode does not visibly move.** With the shim's own dispatcher confirmed — injecting to 128:0 logs `-> 0x4114 press` / `-> 0x4114 release` and the same for `0x4115`, both edges, no latch — rbp's pad-mode byte was then read out of the live process (`PlayerInnards+0x74`, located by the same vtable scan `scan_plinn()` does) and polled at ~100 kHz across each of notes 27/30/32/34: **no change at all**, over roughly 258 000 polls per key — *but that byte is not the mode.* Repolled **1.1 M times** across all four buttons while the grid verifiably switched HOT CUE → BEAT LOOP, it still never left 0, so `+0x74` is falsified as the source of truth and everything derived from it needs re-deriving. The empty screen diff that agreed with it was worse than uninformative: **both captures in that pair ended on SAMPLER**, so 0 px was guaranteed by construction rather than measured. The binary says the handler *would* write that byte (`0x3070b8`, behind a held-pad-list check at the top of the dispatch), and a **positive control in the same state** shows the route is alive end to end — injecting this map's SOURCE binding moved rbp's `browseMode` **1 → 12**. A **direct write** of the byte (exactly what `force_auto_padmode()` does) **sticks for 4 s with no revert** — which proves the byte is writable and not UI-clobbered, and nothing more, since it is not the mode byte. So the honest state of the two buttons is **corrected**: the notes arrive, the keycodes are dispatched, **rbp acts on them** and its pad grid changes mode on the glass, and the broken half is the *feedback* — the panel's four pad-mode LEDs are never driven at all, because `struct led_notes` (`ctrl_map.h`) carries the deck LEDs, the pads' RGB and the mixer/master CUE but **no field for them**. The two things the fix needs are therefore about the LED and not the route: the LED note numbers for those four buttons (never measured — the same LED-probe route `docs/15` describes) and a source of truth for rbp's *current* pad mode, which is still the missing byte, since `+0x74` is not it. **The source of truth is no longer missing — `UiGetPadMode(deck)` @ `0xfd3cc`, found 2026-09-30 and used continuously on 2026-10-01 — so the LED note numbers are the *only* thing left.** **CLOSED 2026-10-01: the binding is POSITIONAL and every button and every base is measured on real presses.** The operator's instruction was *"ignore the names on the FLX4, it should just map to the way the RX3 behaves by position for muscle memory"*, and the map now follows the RX3's row (HOT CUE, BEAT LOOP, SLIP LOOP, BEAT JUMP) by position, so this unit's **3rd button (its BEAT JUMP, note 32) sends `0x4115` K_SLIPLOOP and its 4th (SAMPLER, note 34) sends `0x4116` K_BEATJUMP** — the opposite way round from the sentence above, which is kept as the superseded reading. The pad rows did **not** move (all four bases send the same `K_PAD1..8`; what swapped is base→mode: 16→1, 32→2, 48→3). Measured on the operator's own fingers with two independent witnesses — a live MIDI dump (vel 127, the panel's own) and `UiGetPadMode` read continuously (`work/padmode.py`) — the byte went `0 → 2 (SLIP)` on the 3rd button and `2 → 3 (BEAT JUMP)` on the 4th, and deck 1's grid relabelled to **SLIP LOOP** (1/16 … 3/4) and **BEAT JUMP** (1, 2, 4, 8) while deck 2 held at HOT CUE. Note 34, the one button the 2026-09-30 run never caught on the glass, is now measured. A second press of a mode rbp is already in takes the byte to a fifth value, `4` (the size-bank path) — stable, not a corrupt read. **And the size bank is not what this cell says:** measured off rbp's own grid, the AUTO BEAT LOOP bank is **1/4, 1/2, 1, 2, 4, 8, 16, 32**, so a 4-beat loop is **pad 5, not pad 1** — `docs/08`'s table stores the reciprocal of every size. Records: `work/dumps/flx4-2026-10-01-pad-mode-positional.dump` and `work/dumps/padmode-2026-10-01.log`. **What only the finger settles now:** in PAD FX 1 the pads start auto beat loops (**pad 5 is the 4-beat size** — the bank runs 1/4 … 32, see the correction above), in SAMPLER they start beat jumps, and pressing the *same* button again switches rbp's size bank rather than nothing at all | a pad that produces nothing in PAD FX 1 or SAMPLER ⇒ **not the base — that is measured now**: on 2026-09-30 the operator pressed all eight pads in each mode and the dump caught them on **ch 7** as **16,17,…,23** (PAD FX 1) and **48,49,…,55** (SAMPLER), `base + pad` confirmed on both and the mode button really moving the unit's pads to the new base (`work/dumps/flx4-2026-09-30-pad-bases.dump`), so a silent pad is the mode route above, not the table. **A pad that produces nothing while rbp *is* in the right mode ⇒ the base rows, not the mode rows.** **A mode button that lights no LED ⇒ the shim's LED table, not the route**: both keycodes are confirmed dispatched *and* rbp's pad grid changes mode on the glass (`HOT CUE` / `BEAT LOOP` / `BEAT JUMP`), so the only broken half is the panel's feedback — `struct led_notes` has no field for the four mode buttons and nothing drives them. **A mode button that changes nothing on rbp's screen ⇒ this row's route, and note that driving `+0x74` directly is *not* the repair** — the byte is measured not to be the mode. A mode button that works exactly once ⇒ the latch, and **not** the unit: the panel's own off edge for these buttons is a **Note-On at velocity 0** (measured 2026-10-01 — the dump prints `NOTEON ch=0 note=32 vel=0`, never a real `NOTEOFF`), and `map_flx4.c:1155` reads `velocity > 0` as the press, so that is handled; a latch that really does stick is the map's own state, and the fix is a pulse (send the release with the press, as the beat-loop knob does) rather than a table edit. A pad that acts on **both** decks ⇒ `sch`, the pad rows must stay per-deck |
| S5.8 | **SHIFT + LOAD 1 / LOAD 2**, the operator's ask of 2026-09-29: inject `note 6 104` and `note 6 122` into the shim's own port (`/tmp/seqinject2`), then press them on the panel with `RB_MIDI_DUMP` armed | **The binding PASSED by injection 2026-09-30, and the note numbers were settled the same day by the operator's own presses** — both screens came up under the finger *and* the dump caught both edges on the wire (`note=104` at 8.749 s, `note=122` at 23.433 s, both ch 6, both vel 127, releases ~150-200 ms later, in a file whose velocity histogram is 86 x `vel=0` + 82 x `vel=127` and nothing else), which is what converted 104 / 122 from published to measured. `0x420f` K_TRACKFILTER opens rbp's track-filter panel (mode 8) and `0x0210` K_SHORTCUT its shortcut view (mode 10) — and the run is an **A/B**, the same navigation and the same injection against the shim deployed before the change and against the new build, so only one variable moves: the old shim logs `unmapped ch6 note104` / `note122` at **0 px** with the mode never leaving 3, the new one logs `ch6 note104 -> 0x420f press+repeat` at **631 664 px** (mode 3 → 8) and `ch6 note122 -> 0x0210 press` at **702 207 px** (mode 3 → 10), and the two frames are the two screens by name. **The finding underneath it is the reason `0x420f` had never worked**: rbp delivers the key to `UiKey_Filter` and the handler refuses it, because `UiKey_Filter` is the one browse handler that gates on its record reaching state **> 2** (`0x118af8`) while a press+release pair can only reach **2**. The third edge — op 1, `OP_REPEAT` — is what lifts it to 3, and it is a distinct op rather than a release by rbp's own decoder (`TouchPreviewProc`'s release family is exactly {2, 3}). `sendKey` sends no repeats, so the surface has to; `add_note_repeat()` in `ctrl_map.c` and one flag on the note-104 row are the whole of the map-side change, and the note-122 row deliberately does not have it. `test_flx4` is green at **643 checks** (five new expectation rows, `note_map_n` 99 → 101). The m29 live read of rbp's record table that found this is at [15](15-flx4-midi.md#the-shift--load-bindings-and-the-edge-rbp-needs-that-a-tap-does-not-send) | nothing happens on either button ⇒ **the note number, not the binding** — the map prints `unmapped ch6 note…` for it, so run `RB_KNOB_VERBOSE=1` and read the note off the panel (both numbers are measured now, so this is the shape of a future regression rather than of an open question). **LOAD 1 opens the filter but LOAD 2 does nothing** (or vice versa) ⇒ the note-104 row's `add_note_repeat()` was lost: it is the only row in this map that needs the third edge, and a row moved back to `add_note()` gets 0 px and no error at all. **Both work only from the browse screen** ⇒ that is their design and not a fault — they are browse modes; carrying the gesture to the browse screen from anywhere is unimplemented and is the operator's call |
| S6.1 | plug a stick; `usb-watch.sh status` | **run, and now with rekordbox-exported media the whole chain works end to end.** The first stick answered the mount and import questions but was not a rekordbox export: it carried only WAV files and an *empty* `PIONEER/USBANLZ`, no `PIONEER/rekordbox/export.pdb` anywhere, so `attach: rbp never opened export.pdb after retries` was **correct, not a defect** — there was no DeviceSQL database for rbp to import. With the 1 TB export on it, the SOURCE screen reads **USB1: 736 songs, 931.3 GB**, the browse tree fills, a track loads and it plays (2026-09-26). Note the medium's own pre-existing FAT damage and the under-voltage, which are the medium's problems and not the port's ([10](10-usb.md)) | a stick that *has* `export.pdb` and still fails to import ⇒ then the row's original causes apply: wrong ancestor match, `udisks2` grabbing it, or missing PM NULL guards → crash on insert. **An empty UI with the media mounted is not this row**: it is the *screen* — `K_SOURCE` unreachable, see S5.2 and [15](15-flx4-midi.md) |
| S7 | review these docs against the bring-up log | — | — |

## S8 — the hot-swap drills

Six device classes — the sixth being the HDMI sink, the one device in the block
whose loss must not even change a return value on the audio thread — and until this
block is run **not one of them has been exercised end to end on the unit except the
two that already worked**. Each row is a pull and a replug with the player running;
none needs a restart, and every one of them is expected to recover on its own. The
failures are worth reading as *pairs*: the log line that should appear, and the
line that means it did not.

`RB_KNOB_VERBOSE=1` is **not** needed for any of this and should be off — the
lines below are all printed regardless, and verbose adds a line per key which is
noise in exactly the measurement being read. The one exception is S8.5, where it
is the point.

| # | Drill | Pass | Fail |
|---|---|---|---|
| S8.1 | **Keyboard, hot add.** With rbp running and playing, plug in a USB keyboard, then press `space` | `evdev[…] … 'AT Translated Set 2 keyboard' appeared; adding it` with a timestamp, then `kbd: evdev type=1 code=57 value=1 -> 0x4101 press`; deck 1 responds. The `appeared` line must arrive **within 1000 ms of the plug**, and that number is the fix: the deadline is absolute, so it holds even if a mouse is streaming | never appears ⇒ the reader is not scanning (`EVDEV_MAP=none`, or no `EV_KEY` node); appears but late while a mouse moves ⇒ the absolute deadline regressed, and `test_evdev` scenario 1 exists to catch that |
| S8.2 | **Keyboard, hot remove with a key held.** Hold a mapped key down and pull the keyboard's USB | **Passed, and by construction rather than by luck — measured 2026-09-30 22:59:14.** The removal was made in software (`usbhid/unbind` on `1-1.4:1.0`/`:1.1`, which reaches `input_unregister_device` → `input_dev_release_keys()`, the same path as the cable), and **the instant was chosen by the log rather than by hand**: the unbind fired only after `Backspace` had been down for two seconds with no release line, so the finger was provably still on the key when the device vanished. In order: `kbd: evdev type=1 code=14 value=1 -> 0x420d press`, 46 × `value=2 -> 0x420d ignored` (the kernel's autorepeat, ~23/s while held), then **`kbd: evdev type=1 code=14 value=0 -> 0x420d release` — a release nobody typed** — then `event6/7/8 went away (read: No such device); released 0 held key(s), 6 → 5 → 4`, then all three `appeared; adding it` 5.45 s later, with no restart and `1-1.4:1.0`/`:1.1` bound again at the end. **`released 0 held key(s)` is the pass and not a failure: the kernel synthesised the release, so the reader's own held-key table was already empty** — its `(EV_KEY, code, 0)` fallback is the belt to this brace and is *unexercised* on USB HID, which is why this row's original `released 1` was written before the USB path had ever been measured (only uinput had). The latch half of the clause is proved by the *word* rather than by a later press: `release` is `act_name[ACT_RELEASE]`, printed only where `value == 0 && b->down` ran (`map_kbd.c:328`), so `b->down` was set when the device died and is now cleared. **The device numbers rotate across a re-enumeration** — before, event6 was `Dell KB216 Wired Keyboard`, event7 `Consumer Control`, event8 `System Control`; after, event6 is `Consumer Control` and event8 is `Wired Keyboard` — so nothing may treat an event number as a device identity across a replug | no `release` line for that keycode after the `went away` ⇒ the map's latch is left set and that key looks dead after the replug (the same defect from the map's side); and when it appears, **the tell is `ignored` with `value=1`** — the `value=2` lines the kernel sends *while a key is held* are autorepeat by design and count for nothing, so count the `value=1` ones; `released 1 held key(s)` is **not** a failure — it means the kernel's release was dropped and the reader's own synth path carried it, which is the case that path exists for |
| S8.3 | **Mouse.** Pull and replug the mouse, then right-click and roll the wheel | `went away` / `appeared` as above, and BACK and selector rotation still work | the *pointer* keeps working while the buttons do not, or vice versa — these are two different modules ([16](16-input-and-hotplug.md#4-hot-swap)) and a report that says "the mouse is fine" may only mean the pointer |
| S8.4 | **FLX4 MIDI.** Pull the controller's USB mid-play, wait ~5 s, replug | `control surface 'FLX4' disappeared (unplugged?); waiting for it to come back`, then `subscribed to N:0 'DDJ-FLX4 MIDI 1'` — **write N down**, because whether the client number changed is the thing this drill exists to record — then press a pad and see it work | the surface is re-found (`subscribed to`) but a pad does nothing ⇒ the recycled-client-id case the retry was written for; press nothing and check the LED/meter route line instead |
| S8.5 | **The double-press question.** With the keyboard attached *and* the controller live *and deck 1 already playing*, hold the FLX4's PLAY, tap `space` once, release PLAY | **The gesture now has its log, and what it shows is arithmetic rather than gating — measured 2026-09-30 19:11:52–19:12:02, deck 1 playing.** The KB216 keyboard went into the Pi and the reader hot-added `event6/7/8` within a second with no restart, so the gesture itself is S8.1's pass as well; the space then landed *inside* a held PLAY, exactly as this row asks — `kbd: evdev type=1 code=57 value=1 -> 0x4101 press` between `ch0 note11 -> 0x4101 press (sch1)` and its `release`. Read off one clock — block anchor 8 192 001 = epoch 1790813790.64, against the `writei` line's `peak_m` every 500 blocks (0.73 s) — **the FLX4's PLAY press at 19:11:52.96 took the audio to silence at 5 520 501, a second PLAY press at 19:11:59.57 brought it back at 5 525 501, and the space press at ~19:12:01.8 silenced it again at 5 527 001.** Three press edges, three transport toggles, one for one: **rbp acts on every press from either surface and keeps no per-(keycode, channel) aggregation at all.** So this row's criterion is not a property rbp has. `space` is `0x4101`, the *same keycode* as PLAY, so "does playback continue through the gesture" is decided by **parity**: the clean gesture — one FLX4 press, one space tap — pauses and resumes, and playback therefore continues, which is what the operator reported on 2026-09-26 and what any even number of edges gives; that evening's run carried an *odd* number (two PLAY presses then the space) and netted **stopped**, with the deck left silent for the 64 minutes afterwards (`peak_m=0` on every `writei` line from 19:12:02 to 20:16). **No aggregator was built, and none is owed — this time on a measured reason rather than an assumed one** ([16](16-input-and-hotplug.md#two-selections-not-one)). The wording this row used to carry said the second press was *redundant* and that rbp ignoring it was the pass; the measurement says it is not redundant, it is a second press, and rbp acting on it is right — suppressing it would make the keyboard's PLAY inert while the controller's PLAY is held, which is a worse surface than the one shipped. What the 2026-09-26 pass was missing is now supplied: the log the 11:37 reboot had destroyed (the surviving window held **no `KEY_SPACE` (code 57) event at all**, and every `note=11` in `/tmp/flx4.dump` was a tap of 100–330 ms and never a hold). And **PLAY restarts the deck afterwards** — refuted rather than merely unreported: the operator's next press resumed it and `peak_m` was back at 1 235 208 within the minute | **an odd number of press edges leaving the deck stopped is arithmetic, not a defect** — count the edges before reading a failure; a `space` that reaches rbp as *nothing* ⇒ the keyboard half of the gesture, and the first thing to check is whether a keyboard is on the unit at all (**on 2026-09-30 none was attached, and a 16 153-line run held no `type=1 code=57` — a PLAY hold with nobody to tap is not a pass**); `playback stops and PLAY cannot restart it afterwards` ⇒ the one outcome this row was built to catch, now refuted, so seeing it again is a regression rather than a design gap; a second press that does nothing ⇒ gating has appeared where the measurement says there is none |
| S8.6 | **Audio.** Play, pull the FLX4, replug (S4.4 re-run) | in order: `writei … written=-19` → **one** `MASTER LOST` line (not one per block) → `writei #` stops → `MASTER RECOVERED: reopened hw:CARD=DDJFLX4,DEV=0` → `replay: access=3 channels=4 rate=44100 …` (the replay asks for `RW_INTERLEAVED` whatever was recorded — [09](09-audio.md)) → `the pair map is unchanged: master=0,1 headphones=2,3 …` → `startup mute released after 79424 frames` → `written=64` again **with non-zero `peak_m`**. The operator confirms by ear with no restart | recovers but only the master pair, with the "only the master pair" note ⇒ this was the cold case, which means the shim thought no card had ever been open; `MASTER LOST` repeating ⇒ the guard is not clearing the global; no `MASTER RECOVERED` at all ⇒ watch whether the backoff is climbing (it logs only after three failures) |
| S8.7 | **USB media.** Pull the stick while rbp is browsing it, then replug | **Measured 2026-10-01, twice** (09:55 and 10:32), on the SOURCE screen with the stick's entry showing: that entry **cleared** (the operator's own eye — the half no log line can show), the stick left the bus, and `usb-watch.sh` released both mounts lazily (`detach: notifying rbp`, then `umount -l`) — then on the replug the mounts re-bound ~6 s after the pull and `attach: rbp opened export.pdb` **~20 s after that**, rbp re-importing the stick with **no restart anywhere**. The two lines this row used to name (`knobshim2: USB removed`, `USB1 detected -> registered (dev=3)`) belong to the architecture it was written against and no longer exist; the release path now belongs to [usb-watch.sh](../scripts/device/usb-watch.sh). Evidence and the two presses in [8](08-controls.md) and [10](10-usb.md) | the screen keeps showing the removed stick ⇒ the screen, not the mount. And **`/proc/<pid>/fd` cannot answer "does rbp still hold the file"**: its timestamps are entry-lookup times, not open times (measured — a shell's fd 0 and a three-second-later fd stamped identically), so the `export.pdb` check in `usb-watch.sh` cannot tell a fresh handle from a stale one |
| S8.8 | **HDMI sink.** With the mirror up and a track playing (S4.5/S4.6), unplug the monitor's HDMI, wait ~10 s, replug it | **unrun as a drill — and its central prediction has since been measured and is wrong (2026-09-28).** Expected, in order: `mirror LOST err=-19 (No such device)` **once**, with the frames and blocks it had delivered on it; then `mirror NONE — none of the 4 candidate(s) opened; next attempt in 1000ms` at most once per backoff (1000 → 2000 → 4000 → 5000 ms, never once per block); the FLX4's `writei #N … written=64` **continuing uninterrupted** with `MASTER LOST` appearing **never**; then, within ~5 s of the replug, `mirror OPEN …` → `mirror UP … (first block delivered …)`. **Measured instead, by accident (S4.12): the PCM never fails, so none of the mirror's recovery ever runs.** Across a 12 s unplug of the monitor (02:29:10–02:29:22) and a one-sample one (02:32:25), the counter read `dropped=0 short=0 retries=0 lost=0` throughout, there was **no `LOST` and no `NONE` line at all**, and `hw_ptr` advanced 24.2M → 32.8M frames *with the cable out*: the ring drains into a port with nothing on it, so a sink that goes away costs the mirror **nothing it can see**. **Both sides of that are now measured together (S4.12 (c)):** the connector did read `disconnected`/`disabled`/`dpms=Off` with `modes` and `edid` empty (`d41d8cd9`) for those 11 s — the sink genuinely left — while the PCM stayed `RUNNING` and the mirror's block counter climbed 756 → 771. So this is not "the sink never went away"; it is that **the shim has no way to notice it did**. The FLX4 half of the prediction held exactly — the master's `writei` never faltered and `MASTER LOST` never appeared. The **ears** half is still unrun, and it is now the interesting one: whether the sound survives a replug, and if it does not, whether a second replug re-locks the sink or only a player restart does ([13](13-raspberrypi4.md) S4.12's forward test). `RB_AUDIO_MIRROR_REOPEN_MS=0` is the control case: the mirror tries once at startup, the `NONE` line says `or never, if AUDIO_MIRROR_REOPEN_MS is 0`, and it stays down until the player restarts | the FLX4 going quiet, or a `writei` returning anything negative, ⇒ the mirror reached the master's path and it gets reverted (S4.7's rule). `mirror LOST` repeating per block ⇒ the handle is not being cleared; `NONE` repeating per block ⇒ the restamp in `mirror_open()` regressed, which measured ~2000 opens/s and 121k log lines in 28 s. No `mirror UP` after the replug ⇒ check which card the port came back as in `/proc/asound/cards` (`vc4hdmi0` or `vc4hdmi1`), which is why the device is a list. **And the converse, now measured: no `mirror LOST` on an unplug is the correct behaviour, not a missing guard** — this device does not go away from the shim's point of view at all, so a fix for a silent sink cannot be built on this row's original premise. **And the gesture itself turns out to be unavailable on this rig — measured 2026-10-01, four routes, all dead.** (1) There is no cable to pull: this monitor's HDMI is wired internally, so the row's premise — "unplug the monitor's HDMI" — cannot be performed at all. (2) `vcgencmd display_power 0` is a **silent no-op under the KMS driver**: it read back `display_power=1` in the same second as the write and the screen never blanked. (3) The connector's `dpms` attribute is mode `0444` — `echo off` returns `Permission denied` **even as root**. (4) The monitor's own power switch does not drop the link either: across a ten-second power-off the connector read `connected` on **every one of 95 one-second samples**, because a monitor in standby keeps hot-plug detect asserted. **The one route that does make the sink leave is the monitor's source/input change**, and it gave the sharpest form of this row's finding: the auto-scan drops HPD, so at 07:55:56 the connector read `disconnected` on exactly **one** sample with `connected` on either side — a real cable-out event from the Pi's side — and the shim logged **nothing**: `mirror LOST|NONE` still counts 1 (the startup `mirror UP`), `dropped=0 short=0 retries=2 lost=0` identical either side, the block counter unbroken through it (465 501 → 466 001 → 466 501), `peak_m` non-zero throughout, and `dmesg` carries no HPD or DRM line at that moment at all. **So it is not only that this sink never leaves — it is that when it does, the shim cannot see it**, and the recovery path (`LOST` → `NONE`/`OPEN` → `UP`) has still never run in the field, now against a genuine disconnect rather than a sink that stayed. What the Pi read across the operator's 10+ s away was **`connected` on every sample but that one** (1 015 further one-second samples, every counter unmoved) — and the operator's own account is that the monitor **auto-switches back**, so how long the *display* was actually away is their observation and not something the connector distinguishes. **The only event the Pi saw in the whole window was that single one-second `disconnected`**, and it is the only cable-out-shaped event this rig has ever produced. What a source change does to the *audible* output is the half only the operator's ears can supply, and it is the one thing this rig can still answer — though with the Pi's link never dropping, "the sound came back by itself" is not evidence of recovery, only of a link that was never lost. **The ears half was then answered by the operator, 2026-10-01, and the answer is "nothing was audible at all":** they switched the monitor's source with a track playing, *"music never stopped on the flx4"*, held it 10+ s, switched back, and *"everything recovered without interruption"* — no gap, no dropout, no resync heard at any point in the ten seconds. **Read this for exactly what it is:** the FLX4 out is the *master*, a different PCM from the mirror, and the mirror's own failure never reaches it by design (S4.7's rule) — so this is a statement about the master path, and the master was never a candidate to be disturbed here. What it does establish is the whole of what this rig can say: **the one HPD drop the Pi's connector reported produced no audible event, and the operator heard no interruption anywhere in those ten seconds.** With the link never dropping from the shim's side, there is nothing for the mirror to recover *from* — "it came back by itself" is not evidence of recovery, only of a link that was never lost. And a **source change can never test the shim** even where a monitor does drop HPD for it, because whatever goes quiet during a source change goes quiet at the *monitor's* input selector, downstream of everything the Pi can see. The **ears** half is therefore not merely unrun: it has no route on this hardware at all, and it is owed to a rig where the display can actually be detached. |

Three things to record while running these, because they are numbers no log gives
directly: the **latency** between plug and `appeared` (S8.1, read off the
timestamps), the **client number** the FLX4 comes back on (S8.4), and **the window
the log actually covers**. That last one is not a nicety: `/tmp` is tmpfs, so the
log's first line is the current boot, and a reboot destroys the record of every
drill run before it — silently, and in a way that makes a report look verified.
Measured 2026-09-26: S8.5 passed on the operator's report, and no trace of it
survives, because the 11:37 reboot had already cleared `/tmp`. The pass stands —
the operator ran the drill and is the authority on what they saw; what was missing
was only the log that would corroborate it, and **that log was taken on
2026-09-30** (S8.5's row). It corroborates the *mechanism* — every press from
either surface reaches rbp — and shows the original report was arithmetic rather
than gating, which is worth remembering before treating a corroboration as a
formality. Check
`ls -l --time-style=long-iso /tmp/knobshim.log` against `uptime -s` before
believing *or* disbelieving any on-unit report — noting that `uptime -s` itself
reads ~19 s late on this unit ([Boot time](#boot-time) says why, and the error is
immaterial at this granularity). And before a reboot, read the log,
because afterwards there is nothing left to read.

## S9 — the LED drills

The LED bridge is switched **on** for this target as of 2026-09-26
(`RB_LED_DISABLE=0`), and it transmits nothing, because every FLX4 note row is
`-1`. That is the design, not a defect: `-1` means "this surface has no such LED"
and short-circuits the send, so the bridge could be turned on before the unit's
illumination notes were known. **Every row below therefore starts dark, and S9.0
is what turns a row from `-1` into a number.**

Nothing in `make -C scripts/shims test` can see an LED light. The suite pins the
*table's shape* — every unmeasured row is `-1`, a filled row names a legal note on
a legal channel, no row carries an SC Live 4 `(channel, note)` pair (a copied
number is not a dark LED, it is a **phantom control press**), and a channel base
of `-1` may not sit next to a non-zero count. The operator's eye is the only
evidence that the lights are right, and the rows below say so rather than
implying the suite covers it.

`LED_VERBOSE=1` is what makes an *absent* row legible: a row that is `-1` logs
nothing at all, so "the pad never lit" and "the shim never sent" look identical
on the panel and are only distinguishable in the log.

**The route, first, because it is not the obvious one.** `amidi` does not work on
this unit: `hw:1,0,0` is `EBUSY` to userspace because the kernel's `snd_seq_midi`
holds the node once the sequencer attaches it. Drive the panel through the
sequencer — `seqinject2`, or `/tmp/ledprobe.sh` on the unit — which is the same
route the shim's own writes take ([15](15-flx4-midi.md#measuring-the-notes-the-route-and-the-loopback-result)).

| # | Drill | Pass | Fail |
|---|---|---|---|
| S9.0 | **Measure the notes (this is the one that gates the rest).** Run `sh /tmp/ledprobe.sh cand`, watching the panel; then `pair <ch> <n>` for any candidate that lit, then `sweep <ch>` on any channel whose candidates all stayed dark | **Run for three rows on 2026-10-01 — the first LEDs this surface has ever lit** (CH CUE 84/0, 84/1, MASTER CUE 99/6, the numbers and the rule in [15](15-flx4-midi.md)), from the host with a one-note SMF (`aplaymidi -p 28:0`); the rest of the table still gates. Each control's own note is *seen* to light and clears on velocity 0, and the number goes into `flx4_leds` as a one-line edit | a candidate lights a **different** control than the one named ⇒ the same-note hypothesis is wrong for that row and the sweep is the answer; nothing lights anywhere ⇒ the write is not reaching the panel at all, which is a different problem from a wrong note and should be reported as such |
| S9.1 | **CH CUE tracks rbp.** With a track loaded on deck 1, press the FLX4's channel-1 CUE, then press again | **Measured 2026-10-01 on both decks, both edges — and it took a fix to get there**: the LED lit on the first press and *never went dark*, which was not the state machine (the log showed a correct `off` every time) but the encoding — `midi_note()` sent a real Note-Off (`0x80`) and the FLX4's LEDs ignore one, so off is now Note-On with velocity 0 ([15](15-flx4-midi.md)). The button's LED lights on the first press and goes dark on the second, matching rbp's own channel CUE state; `LED_VERBOSE=1` logs one `led sch… note… on` and then one `off` | lit but not tracking ⇒ the strip loop is reading `me_get_cue()` wrongly; never lit ⇒ `n_strip_cue` is still `-1` (check the log before the hardware) |
| S9.2 | **BEAT SYNC.** Engage SYNC on deck 1, disengage, then sync and nudge the tempo so rbp blinks it | solid while engaged, dark when disengaged, and it **blinks in step with rbp** rather than staying solid — the state that lights it is `LedStat` id 4, where 2 means blink | solid where rbp blinks ⇒ the `== 2` case is not being honoured; dark while rbp's own screen shows SYNC engaged ⇒ the row is unmeasured or on the wrong channel |
| S9.3 | **Hot-cue pads.** Set two hot cues on deck 1, leave a third pad empty | the two pads light **in the right colours and the third stays dark**; an empty pad is not merely dim | right pads, wrong colours ⇒ the velocity encoding is not the Engine OS convention and `pad_enc`/`RB_PAD_BRIGHT` change; all pads the same colour ⇒ the stored RGB is not being read (`ledstat_rgb()`), which is a different failure from a wrong encoding |
| S9.4 | **PLAY / CUE.** With a track loaded: play, pause, then unload | PLAY solid while playing; PLAY blinks and CUE lights with a track loaded and stopped; both dark with nothing loaded | PLAY lights with nothing loaded ⇒ the `loaded` test; CUE never lights ⇒ `n_cue` still `-1` |
| S9.5 | **MASTER CUE.** Press the unit's MASTER CUE button, then press it again | **Measured 2026-10-01, both edges** — with the same encoding fix S9.1 needed. One thing this drill must not read as a fault: **the LED is lit at every boot**, because rbp enables master cue itself (`knobshim2: master cue enabled at startup`), so the boot-time `led sch6-6 note99 on` is the shim reporting the truth rather than a stray write. The LED follows the button, and rbp's own master-cue state agrees — this one is engine state rather than a mirror, because `me_set_master_cue()` is asserted at startup and the button toggles the same state | the LED and rbp's screen disagree ⇒ the button is bound to a different call than the LED reads |
| S9.6 | **Absent rows stay absent.** With `LED_VERBOSE=1`, exercise the controls this unit does not have (there are none for Colour FX, KEY LOCK, VINYL, SLIP and the loop section) | **no** send is logged for those rows; the FLX4 has no loop section and no KEY LOCK/VINYL/SLIP LED, and the four Sound Color FX rows are permanently `-1` | a send appears for a row this unit does not have ⇒ a row was filled in that should have stayed `-1`, and on this panel a wrong note is a control, not a dark LED |
| S9.7 | **No regression on the previous target.** Run the suite and read the counts | `test_midi`, `test_flx4`, `test_kbd` green with the JP21 move a pure move — the SC Live 4's behaviour byte-for-byte unchanged | a JP21 count changes ⇒ the move was not pure, and `test_midi`'s exact pins name the row |
| S9.8 | **Hot-swap the FLX4 with the bridge on.** Pull the controller mid-play, wait, replug (this is S8.4 with the LEDs live) | no crash, and the LEDs **come back on replug** — `midi_note()` retries every tick while there is no route, so the mirror re-establishes itself without a restart. Write down whether the client number changed (S8.4's number). **Reported by the operator 2026-09-28, reproduced and fixed the same day, and it was neither of the two candidates below.** What the preserved audio log (`/root/audioshim.log.before-restart`, the run before the 08:21 restart) does say about that shape is where the *absence* of evidence is the finding: the counters stay clean straight through the master's churn — `mirror … dropped=0 short=0 lost=0` and `writei #N frames=64 written=64` throughout, with the master re-selecting **four times mid-run** and re-preparing the stream (`prepares=8`), and no failed write anywhere. Every `set_rate_near req=44100` in it was accepted (`res=0 rate=44100`), so the request is not where a clock mismatch would hide. And that is the shape of the defect rather than a gap in the log: the audio *clock* is not content, so every counter in the shim counts it wrong; the operator's two pulls of 2026-09-28 (S4.10) were run with the watcher on and the mirror's pad rate is what reads the clock out — **7.53–7.63 frames per 64-frame block** with the card away against ~7 a *second* with it open. Both pulls now come back at the card-present floor (0.0096 frames/block) and sound right. S4.4 carries the reopen half of the same two pulls | the LEDs never return after replug while the controls do ⇒ the LED output route did not re-subscribe, which is a different code path from the input subscription and worth reporting separately. **For the slowdown:** resolved 2026-09-28 — the deck's tempo and the device's clock were both innocent; it was the shim's own cardless block clock (S4.10), and both candidates below were written before anything measured it. A slowdown that comes back is read the same way and without rbp's BPM display: `writei` frozen while the mirror's `padframes` advances at ~1000 a second rather than ~7 is this defect, and both lines are in the shim's own log |
| S9.9 | **CLOSED 2026-10-01 — the wiring half of this row now ships: the four pad-mode lights are driven *and* the eight pads are driven, which is the operator's original ask in full. The rest of this cell is kept as the history of how it was found, because two of its premises were wrong and the corrections are the useful part.** The four mode LEDs are lit from `struct led_notes.n_mode[4]` (`ctrl_map.h` — the field this cell used to say did not exist) with **notes 27 / 30 / 32 / 34 on ch 0**, which are the buttons' own input notes under the rule that a control's LED is that control's own input note on its own input channel; which one is lit is read live from **`UiGetPadMode(deck)` @ `0xfd3cc`**, not remembered from the last keycode, so a mode set from rbp's own screen moves the light too. The pads are lit from the LedStat entries 18..25 per deck, and the note they go out on follows rbp's mode, because the unit re-addresses its eight pads when a mode button is pressed — the four bases are **0 / 16 / 32 / 48**, all measured on real presses (`work/dumps/flx4-2026-09-30-pad-bases.dump`). The pads are dark unless rbp has given them something: a pad with `LED_ENTRY_OFF_UNASSIGNED` (`rbp_abi.h`, word `+20`) is sent off, which is what stops an empty hot-cue pad from being lit white — the operator's *"for hot cues its lit even when theres no hot cue assigned"*, fixed and confirmed by their own eyes (*"they all light up now"*). **A third input landed the same day, and it closed the operator's last pad question.** rbp's `State` — not the unassigned word, not the colour — is what says *something is happening on this pad now*: in AUTO BEAT LOOP all eight read 1 and the **engaged** one reads **3**. Measured on a paused deck with a track loaded, by moving the loop and watching the 3 move with it (pad 5 → id 22, then pad 7 → id 24 with 22 back to 1, then released → all 1) and independently by the operator's own finger. The pad path used to treat 3 as 1, so **all eight pads looked identical whether or not a loop was running** — the operator's *"aren't they supposed to flash when engaged? i can't remember"*, and their memory was right. Pads now blink on `st == 2` or `st == 3`; blink is a **choice**, because this panel has no brightness and "dimmer than its neighbours" cannot be sent. Record: `work/dumps/ledstat-2026-10-01-pad-engaged-loop.txt`. **And it is now measured at the wire and confirmed by an eye.** On the live unit the shim's own MIDI output shows the engaged pad alternating in **runs of four sends** (400 ms on, 400 ms off — `led_tick & 8` at the 50 ms tick with the 2-tick resend) while its seven siblings hold solid runs of 609 to 1124 sends with no alternation; every blink landed on the pad whose LedStat id rbp had actually set to 3, in three modes (id 20 → note 18 in AUTO BEAT LOOP, id 19 → note 33 in SLIP BEAT LOOP, id 21 → note 3 in HOT CUE), so the per-mode base moved underneath it. The operator settled the half no log can: asked whether the pad they pressed was the one that flashed, **"yes only the pad i pressed flashed"** (2026-10-01). **What this row's own history got wrong, twice:** it said `struct led_notes` had no pad-mode field (true when written, false since the field landed), and it said the four note numbers were the only thing missing (the *mode* reader was missing too — `UiGetPadMode` is what closed that, `[player+0x74]` having been falsified as the mode at 1.1 M polls). **Closed by an eye 2026-10-01 — and not by the look this cell asked for.** Pressing the four buttons and watching is the *weak* test: the FLX4 lights its own pad-mode button device-locally when the panel sees the press, so that look cannot separate our bridge from the panel's own behaviour. The isolating route walks the mode from the shim's own sequencer port instead (`seqinject2 --dest 128:0 note 0 27 on`, then 30 / 32 / 34, `off` after each), which the panel never hears: rbp's screen switched modes, the bridge's log stepped 27 → 30 → 32 → 34 for six cycles at 17–18 resends per mode, deck 2's LED held `note27` the whole time, and the operator watching the panel — **"yep i see that"**. One of the four lit at a time, on the right one, for a mode rbp changed itself. (Mind the argument shape: `note <ch> <n> <on or off>`; a missing on-or-off is reported as *"unknown command 'note'"*, which reads like a wrong verb rather than a missing argument.) | **Run, and passed on the pads; the plumbing this cell called "new" is now built.** The host suite is green at **`test_flx4` 4125 checks, 0 failures** (2026-10-01), up from the 619 that the 2026-09-28 paragraph below quotes — the four-bases × eight-pads × two-decks enumeration is what moved it, and it is worth noting that **the count moving is the check, not a nuisance**: the earlier fixture had `struct pair f[64]` with an unchecked `PAIR` macro, so the pads pushed the FLX4's rows from 21 to 85 and wrote past the end into the SC Live 4's array, reporting thirteen failures on a channel the FLX4 never uses. Capacity is now 128 with the guard inside `PAIR` and a `CHECK` that makes a future overflow loud. Pressing the four buttons **does** move rbp's pad mode on the glass (`HOT CUE` / `BEAT LOOP` / `BEAT JUMP`, measured one button per framebuffer capture) and each one now lights its own LED. **Superseded — the paragraph below is the state before the wiring landed, and it is kept because the way it was wrong is instructive.** Pressing those buttons **does** move rbp's pad mode on the glass (`HOT CUE` / `BEAT LOOP` / `BEAT JUMP`, measured one button per framebuffer capture); at the time the panel's LED stayed dark — exactly the operator's report, *"i press padfx and rbp changes the screen but the pad mode led is not lit"*. What the paragraph below then concluded was *"the missing half is **one** thing now, not two: the four note numbers"* — and that was wrong, because it assumed the notes were the whole of it. It also assumed a field that has since been added: `struct led_notes` ([ctrl_map.h](../scripts/shims/ctrl_map.h)) then had **no pad-mode field at all** — not a `-1` to fill in, no field — so driving the four lights needed a new field plus a reader in `rbp_led.c`, and the notes were unmeasured on top of that because **S9.0 gates every LED row**. It was right about the shape (a field, a reader, four notes) and wrong about it being one thing. While it was unwritten the FLX4 lit its own pad-mode button device-locally, which is right whenever the mode is chosen from these buttons and wrong in two cases: a mode set from rbp's screen moves no light, and pressing PAD FX 2 / BEAT LOOP / KEY SHIFT lights that button while rbp's mode does not change | n/a — the wiring is in, so the reading is the ordinary one: **a mode button that lights no LED while rbp's pad grid changes mode ⇒ the four mode notes** (27 / 30 / 32 / 34, ch 0), and **a mode button that changes nothing on the glass ⇒ S5.7's route**, not this row. **The four notes were never the whole of it** — the paragraph this cell used to end on said they were, and that was the second premise it got wrong: a source of truth for the mode was missing too, and the next sentence is what supplied it. **The source of truth was found 2026-09-30 and is no longer the missing half**: `UiGetPadMode(deck)` @ `0xfd3cc` is an exported C function that returns rbp's real mode (defines in `rbp_abi.h`), proven against the live process with the control in the same run — injecting ch0 notes 27/30/32/34 moved deck 1 `3 → 0, 1, 3, 2` exactly, while deck 2 held at 0. The byte this row used to name — `rbp_bridge.c`'s `scan_plinn()` logging `padmode=%d` — is **falsified**: S5.7 polled `[player+0x74]` 1.1 M times across all four mode buttons while the grid verifiably switched, and it never left 0 |
| S9.10 | **The channel meters — the row S9.0 gates too, and the operator's original report.** With `RB_LED_VU` on (it ships `1`), run `VU_TEST=1` and watch **both** channel meters; then take `VU_TEST` off, play a track, and watch the meters follow the music as the channel and master levels move | **Measured 2026-10-01 — the wiring half, which is the half only an eye can settle**: with `VU_TEST=1` sweeping the steps, the operator confirmed **both channel meters stepping**. A successful `midi_cc()` proves the message left the shim and nothing more, so nothing but the panel could close this. The row is `CC 2`, channels `0`/`1`, **pre-fader**, **no master meter**, and the startup line names it: `VU bridge up: channels CC 2 on ch 0/1 pre-fader, master CC -1/-1 on ch -1 (absent)`. `VU_DEBUG`'s `wire=` prints the value handed to `midi_cc` and the result, which is the positive proof the message went out | a meter that never lights ⇒ **the map's meter row or `RB_LED_VU=0`, not "the FLX4 has none"** — it has one, and it is host-driven; a meter that lights but is *wrong in an unrecognisable way* ⇒ the step→band table (`vu_flx4_level[]`, which anchors each step at its own band's bottom); a meter that **drops as the channel fader is pulled down** ⇒ `meter_pre_fader` is wrong for this surface, since the list puts the LEDs before the fader; a **MASTER** meter appearing ⇒ an address the list does not give has been invented |
| S9.11 | **SHIFT + HOT CUE pad deletes that pad's cue** — the one gesture on this surface rbp has no keycode for, and the operator's own ask (*"id rather you get delete hot cues working but shift pad button"*). Set cues on two pads of a loaded deck, leave a third empty; hold SHIFT and press each in turn | **PASS — measured on the glass by the operator's own gesture (2026-10-01).** They loaded a track, set a cue on pad 5, held SHIFT and pressed that pad; the log read `hotcue delete deck1 pad5 @0xcda7c670 inn=0xcda7cbf8 eng=0xe9601510 reg=1`, and their verdict is *"it seems to work"* — the cue is gone and the cell stopped drawing. **`reg=1` is the load-bearing detail**: rbp's own `isRegisteredHotCue` gate agreed a cue *was* registered for that pad, so this ran the real path, not the no-op one (the injected lines below, `reg=0`, are the no-op path on trackless decks). The same gesture is **archived** at `work/dumps/flx4-shift-pad-delete-2026-10-01.dump` — 43 lines holding the LOAD, the cue-set (`ch=7 note=4`) and the delete (`ch=8 note=4`) together, with the shim's own injected events at t=0 as an in-file vel-100-vs-127 control. The operator's own press with `RB_MIDI_DUMP` on put the shifted pad on the wire as `NOTEON ch=8 note=0 vel=127` inside a SHIFT-held window (note 63 down at 610.514, released at 611.118) — so the +SHIFT layer is **ch 8/10 as the map assumed**, the note is `base + pad`, and it **replaces** its base note (no ch 7 twin beside it, on either pad). The route there took two negatives, both measured: `0x4124` "CueDelete" is **not pad-addressable** (it deletes one fixed slot, `Player+0x234`, `add r3,r4,#564`, one of only two `#564` sites in the binary, whose only writer is the constructor's `memset`), and the selector poke is dead too — `[innards+0x38]` set to the impossible **7** on the deck's own innards, one plain `K_PAD1` sent, the word read back **0**, so rbp clears it on the key path before the handler reads it. What ships is **two direct calls**, because the delete has two halves: `rbp_bridge.c`'s `hotcue_delete(deck, pad)` resolves the deck's `ui::Player` from rbp's own array and calls `ui::Player::onHotCueDeleteEvent(pad)` @0x2fb328 (`rbp_abi.h`'s `ADDR_HOTCUE_DELETE`), a plain concrete symbol which guards itself at every step — pad range, that pad's own has-cue flag (`Player+0x479..0x47b`, **bit SET = no cue → return**), a live TrackInfo, `seacheHotCue`, a per-pad state byte — so an empty pad is a no-op, not a stray delete. **Then the operator's own gesture found the seam in it**: *"ok, i did it but the cue is still there. the light does go off though"* — and that sentence is the whole defect, because the FLX4's pad LED is driven from rbp's cue list, so a dark pad means **rbp agrees the cue is gone while the glass keeps drawing it**. `onHotCueDeleteEvent` is the **UI and DB half** of the delete and never touches the engine; the engine half has exactly **one call site in the binary** (`bl 0x48a48` at `0x307e68`, inside the *physical* `ui::PlayerInnards::pad_HotCue` @0x307918, which `IKeyManager::sendKey` never reaches), so the cue stayed registered in the engine, `ui::StatWatcher` kept serving the pad grid its IN time (deck 1 pad 5 still held **IN `0x31fa6` with colour 0**, drawn as a default-cyan pillar — the rule is *a pillar is drawn iff `IN != 0xffffffff`, and colour 0 renders as the default cyan*), and the cell kept drawing. `hotcue_delete()` now makes that second call too: `rbp_abi.h`'s `ADDR_ENGINE_CLEAR_HOTCUE` = `djengine::DjEngineIF::clearHotCue(ch, pad)` @0x48a48, whose `EnCueType` is the **pad number 1..8** (`backHotCueGate` @0x66bf8 opens `sub r2, r1, #1; cmp r2, #7`) and whose channel is the **0-based deck index** (how `pad_HotCue` computes it at 0x307e18) — plus `ADDR_ENGINE_IS_REGHOTCUE` @0x48b00, asked and logged as `reg=` but deliberately **not** used as a gate, since the pad most needing the clear is the one the UI half has already forgotten. **Measured on the unit with the new build** (`knobshim.so` 3d48e411): `hotcue delete deck1 pad1 @0x… inn=0x… eng=0xe9601510 reg=0` and the same shape for deck 2 pad 4 — both decks' innards found, the engine pointer the **same object for both decks** (a global dispatcher, exactly as the disassembly says), rbp surviving both and still rendering (framebuffer-captured). **The one honest limit is instrumentation, not behaviour**: the cell clearing was witnessed by the operator's *eye*, not by a before/after framebuffer capture, because the cue was already gone by the time a capture was possible. This row also found and fixed a **real shipped bug**: `scan_plinn()` required `chan == 2 \|\| chan == 3`, but the innards' channel byte is 1-based (deck 1 = 1, deck 2 = 2), so deck 1's innards was rejected outright, deck 2's was filed as deck 1, and `g_plinn[1]` stayed NULL for the life of the process — every write through `plinn()`, including the beat-loop path, had been landing on the wrong deck. `test_flx4` is green at **4127 checks, 0 failures** | a shifted pad that logs `hotcue delete deckN padM @0x…` and leaves the cue ⇒ **already explained and fixed**: that was the missing engine half, so check `eng=` (a plausible heap pointer) and `reg=` in the same line, and check the running shim's vintage first — the pre-fix build prints neither; a shifted pad that logs `no engine yet` ⇒ the innards scan has not found that deck, which is the beat-loop path's failure too; a shifted pad that logs `no ui::Player yet` ⇒ the object array is not populated at that moment, which is rbp's state, not the binding; a shifted pad that logs **nothing at all** ⇒ the binding, and the note numbers are measured (`base + pad` on ch 8/10) so check the layer's channel first. A cue that disappears on the **wrong deck** ⇒ the deck index, and note `UiObject::Channel` is 1-based — that is exactly the bug above. A cue still drawing **after** a delete that logged `eng=0x… reg=1` ⇒ the engine is the authority no longer: re-derive the chain before touching the map. See [15](15-flx4-midi.md#pads-list-ch-810--rch-79) |
 With `RB_LED_VU` on (it ships `1`), run `VU_TEST=1` and watch **both** channel meters; then take `VU_TEST` off, play a track, and watch the meters follow the music as the channel and master levels move | **Measured 2026-10-01 — the wiring half, which is the half only an eye can settle**: with `VU_TEST=1` sweeping the steps, the operator confirmed **both channel meters stepping**. A successful `midi_cc()` proves the message left the shim and nothing more, so nothing but the panel could close this. The row is `CC 2`, channels `0`/`1`, **pre-fader**, **no master meter**, and the startup line names it: `VU bridge up: channels CC 2 on ch 0/1 pre-fader, master CC -1/-1 on ch -1 (absent)`. `VU_DEBUG`'s `wire=` prints the value handed to `midi_cc` and the result, which is the positive proof the message went out | a meter that never lights ⇒ **the map's meter row or `RB_LED_VU=0`, not "the FLX4 has none"** — it has one, and it is host-driven; a meter that lights but is *wrong in an unrecognisable way* ⇒ the step→band table (`vu_flx4_level[]`, which anchors each step at its own band's bottom); a meter that **drops as the channel fader is pulled down** ⇒ `meter_pre_fader` is wrong for this surface, since the list puts the LEDs before the fader; a **MASTER** meter appearing ⇒ an address the list does not give has been invented |


The loopback property that makes all of this safe was measured on 2026-09-26
rather than assumed: a write to the device port reaches the panel and is
delivered neither to the shim's own input port nor to a monitor, while a real
button press demonstrably is. The FLX4 is the case that needed checking, because
the shim reads and writes the *same* port `20:0` — on the previous target the two
were different routes ([15](15-flx4-midi.md#measuring-the-notes-the-route-and-the-loopback-result)).

## S10 — the display drills

**S10.1–S10.4 have been run, S10.5's same-mode half with them, and S10.6's
decision table; the rest have not.** The four display rows were run on 2026-09-26
against the fixed fbdev module
(`libdirectfb_fbdev-rot16.so`, sha256 `a3e3741e…`, the same file at both deploy
ends), each with a `video=` edit and a reboot — so "the code is right" is now
separated from "the screen is right". S10.5's same-mode half was run later the
same day, on a boot with **no** `video=` edit and no reboot: a cable pull, which
is what makes it the one display row that tests the shipped configuration as the
operator meets it. S10.6's watcher logic was exercised earlier in
`--dry-run` against a fake `RB_SYSFS_ROOT`, which needs no restart and no cable.
**S10.5's different-monitor half was run on 2026-09-27** — a different panel swapped
in with the player running, which survived it untouched — **but it is a pass with a
caveat that must not be smoothed over, and it does not decide
`RB_DISPLAY_RESTART_ON_RECONNECT`**: the swapped-in panel advertises 1280×800
first, so the geometry never changed and the watcher's change test still had
nothing to fire on. **S10.0, S10.7–S10.10 are still unrun**, and the cells that say
*write this after running it* still say it. The `scale` rung
and the automatic upgrade that selects it
([06](06-display.md#a-mismatch-selects-the-rung-by-itself)) were built and
artefact-verified first — the built `.so` read back with `strings` rather than
trusted to a `make` exit code — with the arithmetic pinned on both copies
(`tools/fit-crosscheck.sh`, `test_point`) and the cursor's *use* of the rectangle
pinned by `test_cursor_dev` (S5.3, re-run green).

**What the rows settled, one line each:** a panel that is not 1280×800 gets
the whole UI, correctly placed, with nobody selecting anything (S10.1); the
resample is 1.1–3.4 ms against a 16.67 ms frame, so the shipped budget default is
not wrong (S10.2, S10.3); the matched 1280×800 panel is unchanged (S10.4); and a
same-mode unplug/replug fires no restart and leaves the fbdev and the CRTC
untouched — while the re-probe *refuses* the forced token, so the token carries a
boot and not a hotplug (S10.5, which is also where that correction to
[06](06-display.md#setting-the-mode) is measured from). **What they did not settle
is a downscale:** at 0.75× and 0.625× single-pixel
rules and thin glyph strokes are lost, which is S10.2's own stated escalation
trigger, and that escalation is **not implemented** — it is a decision, not a
defect.

**The lever is S2.1's measurement**: the sink accepts modes it does not advertise,
and the modes it *does* advertise include guaranteed-accepted downscales
(`1280x720`, `960x600`, `960x540`, `800x600`, `480x320`). So a mismatched
framebuffer is manufactured on demand — one line in `/boot/firmware/cmdline.txt`,
one reboot — and each row names the one it wants. **Revert the line and reboot
before moving to the next row**; two `video=` tokens for the same port is a
silent last-one-wins.

**The recipe ships two tokens; the measured unit carries one, and that is now a
named gap rather than an installed one.** [06](06-display.md#setting-the-mode)
prints both `HDMI-A-1` and `HDMI-A-2`, and the unit's cmdline was read on
2026-09-26 with only `video=HDMI-A-1:1280x800@60`. `install.sh`'s boot trim (see
[Boot time](#boot-time)) deliberately keeps it that way: `boot-trim.sh apply
bootfiles` ensures the A-1 token and only the A-1 token, and `boot-trim.sh status
bootfiles` **names the A-2 gap instead of closing it**, because forcing a mode on
a connector with nothing attached is not free and that token is S10.8's
precondition rather than a boot-time trim. That is the precondition S10.8 is
waiting on, not a defect, and it is why S10.8's row is written as two outcomes
rather than one.

**The first pass ran S10.1–S10.3 with `RB_DFB_PRESENT_AUTO=0`** — the control that
holds the driver to what it is told, so the rung is verified *before* anything
selects it. **The re-run after the driver fixes used the default**, and that is
where the automatic upgrade was first exercised: at 1280×720 the log carries a
`PRESENT: auto -- …` line ahead of the mode line, with no help from the operator.
Both are recorded in the rows below, and the difference between them is which
line is absent. S10.4 runs **only** with the default, because staying silent is
the whole thing it tests.

**Captures change with the geometry.** `tools/pi-bringup/drive-keys.sh` captures
with `dd bs=2560 count=800`, which is a 1280×800 fb read through its 2560-byte
stride — exactly the two numbers a mismatch changes. Use `bs=<line_length>
count=<yres>` from `fbdump` (or from `/sys/class/graphics/fb0/stride`, `virtual_size`),
and decode as RGB565; a capture taken at the wrong stride is a sheared image that
looks like a rendering bug.

**Read `PRESENT:` before believing anything else.** It is a one-shot line at
`/tmp/dfbdig9.log` on every launch, and the `scale` suffix it grows
(`scale=WxH dx=N dy=N fit=fit skip=N`) is the driver's own report of the
rectangle it drew into — the same rectangle the cursor maps into. If the two
disagree the picture and the arrow are the two symptoms. There are up to **three**
lines to read, and which are present is itself the reading: a `PRESENT: auto -- …`
line only when the upgrade fires, the `mode=` line always, and a second
`PRESENT: … avg … worst …` line only under `scale` — at the 300th presented frame,
so a run shorter than that never prints it. On a matched 1280×800 panel **neither**
of the first and third appears, which is what S10.4 asserts.

| # | Drill | Pass | Fail |
|---|---|---|---|
| S10.0 (E0) | `video=HDMI-A-1:1280x720@60`, `RB_DFB_PRESENT_AUTO=0`, `RB_DFB_PRESENT=off`, reboot | **the evidence [06](06-display.md#a-mismatch-selects-the-rung-by-itself)'s comment cites.** The picture is sheared, or there is no UI at all, **and** `/tmp/dfbdig9.log` shows the present path failing on a smaller fb. This row exists to *prove* the "off on a mismatched fb is broken, not slow" claim rather than assume it — a clean picture here means the claim is wrong and the auto-upgrade is unnecessary | a picture that is merely *degraded* rather than broken ⇒ re-read the claim before trusting the rest of this block; a blank screen with **no** log line ⇒ the failure is upstream of the present path, and S2.2's `pages=`/`need_mem` arithmetic is where to look |
| S10.1 (E1) | `video=HDMI-A-1:1280x720@60` — first pass with `RB_DFB_PRESENT=scale` and `AUTO=0`, so the rung is verified before anything selects it; then re-run with the **default**, which is what puts the upgrade itself on trial | **Pass, 2026-09-26**, and it is the first exercise of the user-facing feature — the auto-select fired unaided, with no `AUTO=0`: `PRESENT: auto -- real fb 1280x720 pitch 2560 does not match the logical 1280x800 pitch 2560, and 'off' cannot present that; using mode=scale`, then `mode=scale angle=0 real_fb=1280x720 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800) scale=1152x720 dx=64 dy=0 fit=fit skip=1`, then `avg 2.76 ms after 60 warmup`. `fbdump` independently computes **0.9000 → 1152×720**, agreeing with the `PRESENT:` factor; the capture shows the whole UI legibly scaled with 64-px bars each side and the cursor inside the picture. **The auto line's wording is a wart** — it names pitch even where the two pitches are equal (both are 2560 here), so it reads as a self-contradiction. S10.7's four-corner arrow sweep is still owed | the UI visible but *cropped* ⇒ the rectangle is coming from the fb rather than the fit; `fbdump` and `PRESENT:` disagreeing ⇒ the two copies have drifted, which is what `tools/fit-crosscheck.sh` exists to prevent; a frame rate that will not hold ⇒ S10.3, and the budget is the knob |
| S10.2 (E2) | `video=HDMI-A-1:960x600@60`, then `800x600` (0.75× and 0.625×) | **Run 2026-09-26 — the picture is complete and correctly placed at both factors, and the hairlines do not survive, which is this row's own escalation trigger.** 960×600 (0.75×): `scale=960x600 dx=0 dy=0 fit=fit skip=1`, `avg 1.09–1.26 ms`, worst 2.52–2.67 ms, `fbdump` 0.7500 — the UI fills the panel exactly (same 1.6 aspect, so no bars), nothing clipped. 800×600 (0.625×): `scale=800x500 dx=0 dy=50`, `avg 1.23 ms`, worst 2.60 ms — the UI sits in an 800×500 band with 50-row bars top and bottom. **But glyph strokes break: at 0.75× `Bars` reads `Uars`; at 0.625× it is heavy — `HOT CUE` reads `MOI CUP`, with `B`→`D`/`F`, `H`→`M`, `Q`→`2`.** So at these factors bitmap text is *not* legible and a 1-px rule does not hold as one line. This **contradicts this block's earlier "hairlines survive" reading** and is the measured input to the second half of the Fail cell | hairlines breaking up ⇒ **observed, at both factors** — the vertical 2-tap average on the two 2-bpp-source arms is the escalation, and it is affordable (a downscale writes strictly fewer pixels than it reads). Do not reach for a general box filter. **Not implemented: whether it is worth doing is the operator's call, and nothing else in this block depends on it.** A *cropped* picture or a wrong bar width ⇒ the fit arithmetic, and `tools/fit-crosscheck.sh` is where to look |
| S10.3 (E3) | `video=HDMI-A-1:1920x1080@60` — **not advertised**, so read `dmesg`/`fbdump` for whether it was refused — then `2560x1440` if it locks on | **Pass, 2026-09-26.** 1920×1080 locked on even though the panel does not advertise it — the `video=` token is programmed regardless, which is S2.1's finding holding on a second mode. `scale=1728x1080 dx=96 dy=0 fit=fit skip=1`, `avg 3.34 ms after 60 warmup`, **worst 7.66 ms** on a fresh boot: inside the 16.67 ms frame at `skip=1`, so no skip was needed and **`RB_DFB_PRESENT_PX_BUDGET`'s shipped default of 2,600,000 stands unchanged**. The capture is complete and correctly scaled with 96-px side bars. 2560×1440 was not attempted. **Two readings on the way here did not reproduce** — see [the intermittent high reading](#the-intermittent-high-reading) below, which is also where the line's own shape changed | the mode refused ⇒ record the refusal and move on; the line absent at the 300th present ⇒ the count is not advancing, which is its own defect; an average over budget with `skip=1` ⇒ set the budget from the measurement before drawing conclusions about the panel |
| S10.4 (E8) | revert to 1280×800, reboot, **no other change** | **Pass on the log criterion, 2026-09-26, and run last** — the order is the point of a no-regression row. `PRESENT: mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)`: no `scale=`, `fit=` or `skip=` suffix, **no `PRESENT: auto` line**, and no average line (correct — the timing exists only under `scale`). `NRestarts=0`, one rbp process, the cmdline restored to the single `video=HDMI-A-1:1280x800@60`. The capture is the crisp native UI — 165 distinct RGB565 values, 75.95% black. **The pixel-identity clause cannot be met as written**: no same-screen capture from before the deploy is on hand, so only the log criterion and the colour census are asserted here, and that is a gap rather than a pass | **any** difference ⇒ the new code is on the `off` path, and the difference names which part of it |
| S10.5 (E4) | unplug/replug **at the same mode** with `video=` set; then swap in a *different* monitor (a 1080p TV is the realistic stand-in) | **Pass on the same-mode half, 2026-09-26 — and it is the first hardware measurement of the watcher's no-fire rule**, since S10.6's version of it is a `--dry-run` argument. Pulling and replugging the same monitor at the forced mode produced **no restart of any kind**: `NRestarts=0`, the same rbp pid with its boot-time start stamp (so its fd and mapping were never disturbed), no `RESTART` line in `displaywatch.log`, no `/tmp/displaywatch.fires`. The geometry read `1280,800 2560 16` on both sides of the replug, so the watcher's change test had nothing to fire on — the operator's rule, on hardware. The driver state agrees: `crtc[102] pixelvalve-2 enable=1 active=1 mode "1280x800"` with a plane at `crtc-pos=1280x800+0+0`, i.e. the fbdev was **not** torn down and the CRTC was never re-set. **The finding is on the probe, not on the picture**: the re-probe *refused* the forced token — `vc4-drm gpu: [drm] User-defined mode not supported: "1280x800": 60 83496 1280 1344 1480 1680 800 801 804 828 0x20 0x6`, once, at uptime 1361.7 s — and the display survived because the CRTC was *already* running that mode, not because the mode was re-applied. That corrects [06](06-display.md#setting-the-mode): the token is a boot-time instruction, not a standing one. **Two things are still owed, and neither is a pass.** First, the panel's own confirmation: `fb0/blank` reads `4` (`FB_BLANK_POWERDOWN`) while the atomic state says the CRTC is live — the two disagree, no shell read or fb capture settles which state the panel is in, and `[drm]` logged no blanking event, so the picture is asserted from the driver state and wants one glance at the glass. Second, the different-monitor half, which is what actually decides `RB_DISPLAY_RESTART_ON_RECONNECT`. **That half was run on 2026-09-27, on a boot with no `video=` edit and no reboot: the operator swapped in a different monitor with the player running ("different monitor, player running") and the picture and the player survived it.** The evidence is a *no-fire* on a second kind of event: the re-probe refused the forced token again, once, at uptime 438.4 s (`vc4-drm gpu: [drm] User-defined mode not supported: "1280x800"`), **no `displaywatch.log` was created at all**, no `/tmp/displaywatch.fires`, the fb stayed `1280x800`, `NRestarts=0` and the same rbp pid with its boot-time start stamp. In the same window the unit's under-voltage took the FLX4 off the bus at 464.9 s and returned it at 467.9 s (`Undervoltage detected!` at 466.7 s) — the shim logged `MASTER LOST`, two retries and `MASTER RECOVERED` with **no restart**, which is S4.4/S8.6's fix seen on hardware a second time. **The caveat, and it is why this is not the deciding measurement:** the swapped-in panel (`CX101`, a readable 256-byte EDID) advertises **1280×800 first**, so the geometry rbp launched with never changed — the watcher's change test had nothing to fire on for exactly the same reason it had nothing on the same-mode half. The row still needs a panel whose own geometry *differs* from the boot-time fb (S10.0's 720p or a 1080p TV), and `RB_DISPLAY_RESTART_ON_RECONNECT` stays undecided until one is swapped in | a same-mode replug that leaves a black screen ⇒ the fbdev was recreated at identical geometry, which is the one hole the watcher's "only on a change" rule has; that is what `RB_DISPLAY_RESTART_ON_RECONNECT` exists for, and this row decides it. **The forced token is no fallback there** — measured: on the replug the probe refused it, so a torn-down fb would *not* have been rescued by 1280×800 being asked for again. A fire on a same-mode replug ⇒ the geometry string changed by a byte; `display-watch.sh status` prints both sides |
| S10.6 (E5) | one manual `sh /opt/rblive4/display-watch.sh run` plus a mode change (or `--dry-run` against `/sys` first) | **the decision table is measured, 2026-09-26, against a fake tree — the restart half is not.** With `RB_SYSFS_ROOT` pointed at a tree that says `1280,720`, a `--dry-run` run fires **once**, one poll interval after the two-poll debounce agreed: `RESTART 1/5: geometry is '1280,720 2560 16', not the '1280,800 2560 16' rbp was launched with` → `--dry-run: decided to restart, did not.` — and the poll after it decides **nothing**, which is the in-memory `base=now` suppression working. A tree with no `virtual_size` logs `framebuffer /dev/fb0 is gone` **once** and never fires; the leaf returning at the launch geometry fires **nothing** at the default and fires **once** with `RB_DISPLAY_RESTART_ON_RECONNECT=1`. On the real `/sys` the watcher started on every launch of this session's drills and **never fired** across three different baselines (`1280x800`, `1280x720`, `1280x800`) with no `/tmp/displaywatch.fires` — the no-false-positive half. **Still owed: a real fire**, so `journalctl -u rblive4` around it and the no-orphan question are answered | a second watcher ⇒ step 2's `pids_matching display-watch.sh` is not clearing the orphan, and a restart storm follows; the unit's `KillMode=mixed` killing the watcher by the restart it asked for is **expected and fine** |
| S10.7 (E7) | S10.1, then move the mouse to all four corners and along each edge | the arrow tip tracks the pointer and **never enters the bars**; `fb_cursor.c`'s own log line reports the same `picture WxH at X,Y` the driver's `PRESENT:` line does | the tip stopping short at an edge, or drawing into a bar ⇒ the cursor is mapping into the whole fb rather than the picture rectangle, which is the coupling `test_cursor_dev` pins and this row confirms on glass |
| S10.8 | unplug from HDMI-A-1 and replug into HDMI-A-2 | with the `HDMI-A-2` token in place: the same 1280×800 on the *second* port once `RB_FB_DEV=/dev/fb1` is set. **Record which of these happened** — and in particular whether `fb1` exists at all when nothing was attached to A-2 at boot | no `fb1` ⇒ expected, and a reboot after the move is the reliable path (S5's note); the picture alive on fb0 while the monitor is on the other port ⇒ the port move needs the `RB_FB_DEV` change and is not automatic |
| S10.9 | boot with the monitor unplugged, then plug it in | **a measurement first**: is `/dev/fb0` absent at boot, is rbp crash-looping under `Restart=always`, does the launcher fail before the watcher starts? **Write this cell after running it** — [06](06-display.md) and the unit's own file both refuse `ConditionPathExists=/dev/fb0` precisely because this is unmeasured | a crash-loop is the outcome to watch for: `RestartSec=10` makes it look like a slow start rather than a fault, so read `systemctl status` and the journal rather than the screen |
| S10.10 | pull the monitor repeatedly and watch `display-watch.log` | no restart storm: the two-poll debounce, the cooldown and the persisted per-boot cap each appear in the log, and the cap ends the sequence **by itself**. **The debounce is measured (S10.6); the cooldown and the cap are not, and cannot be from a keyboard** — `--dry-run` returns *before* the counter is written, so a dry-run sequence never reaches either (measured: no `fires` file is created), while a real sequence spends the **live** watcher's per-boot budget in the shared `/tmp/displaywatch.fires`. That is why this is a physical row and not a shortcut | restarts more than once per ~25 s ⇒ the debounce or the cooldown is not in the path; a cap that never fires ⇒ the counter is in a variable rather than in `/tmp/displaywatch.fires`, which a restart would reset |

**Unverifiable on this unit, and said so rather than implied:** the `565 → 8888`
arm of the scaler and any 32 bpp fb, because `video=…-32` hits the known
`drm_fb_helper` "No compatible format found" regression (the token table above).
That arm may simply stay unexercised here. **The `565 → 565` arm's pixel cost is
no longer a cycle-count argument** — S10.2 and S10.3 measured it at four
rectangles (1.09–3.34 ms, worst frame 7.66 ms), which is what the budget default
was set from. The other arms' costs remain estimates, which is why the budget is
a config value rather than a constant.

**On the `fbdump` cross-check in S10.1, and how far it goes.** `fbdump` computes
its factor in `double` and truncates (`(unsigned)(1280 * s)`); the driver computes
it in 16.16 with a floored step and rounds the final shift to nearest. Those agree
wherever the products are exact and differ by a pixel where they are not: measured
over every framebuffer from 64×64 to 4096×2400 (9,425,121 sizes), **42.400% differ
in at least one axis**. But they agree on **every mode this panel advertises**
(`1280x720`, `960x600`, `960x540`, `800x600`, `480x320`) and on every larger mode
the drills use (`1920x1080`, `1920x1200`, `2560x1440`, `3840x2160`, `1600x900`) —
so "`fbdump` agrees with the `PRESENT:` factor" is a valid criterion for the modes
in this block and **not** a general identity. One that does disagree, as a
concrete case: 1366×768, where the driver says 1229×768 and `fbdump` says
1228×768.

### The intermittent high reading

S10.3's number was not always 3.34 ms, and *how* it was not is worth keeping,
because the first reading of it was wrong in a way that nearly became a
conclusion. Two runs reported **67.85 ms** and **80.21 ms** per present. At
1280×720 the same code measured 2.03–2.76 ms, so this was 33× the cost for 2.25×
the pixels — 36 ns per output px at 1080p and 139 ns/px at 960×600, where the fast
path runs at 2.45 ns/px. The first "reproduced to 0.01 ms across two launches"
claim was **not a reproduction**: the second reading was the same stale
`/tmp/dfbdig9.log` line read twice, because the restart had not cleared the file.
A clean restart at the identical rectangle then read 4.30 ms.

What settled it was a standalone benchmark on the unit —
`fbdev_present_scale()`'s loop copied verbatim, timed into `/dev/fb0` through the
same mapping *and* into a heap buffer of the same size and stride, while rbp was
live and the fb was being scanned out. It read **3.06 ms** for the 1080p rectangle
and **3.11 ms** at 960×600, against **3.12 ms** for the heap copy: neither the
mapping nor the access pattern is responsible, and the loop is not slow. Spreading
the same 300 presents over 2400 flips (`skip=8`, ≈40 s) gave **1.30 ms** at
960×600, which localises the outlier to the **opening of a run** rather than to any
steady state. Scheduling is not the cause either: load 0.49, **87% idle**, rbp at
54.5% CPU.

Across twelve measurements on the finished module, ten landed in 1.09–6.23 ms and
two at ~68–80 ms; on the final module neither a restart nor a fresh boot at either
rectangle reproduced it (1.09/1.26 ms at 960×600, 3.34 ms at 1080p). So it is an
**intermittent run-opening transient — observed, not explained**, it does not
reproduce on demand, and nothing in the budget depends on it.

Three defects in this block's *own* instrumentation fell out of chasing it. All
three are fixed, and they are named here because each one made a measurement lie:

* The "one-shot" average line was **not** one-shot at `skip > 1`: `present_done`
  stalls at the trigger between presents, so an equality test on it held on every
  skipped flip — **eight identical lines in a row** at `skip=8`. It is now gated by
  a `present_reported` flag instead.
* The average included the run's own bring-up (window stack, first paints, the
  media scan) and ran up to ~2 ms high on the whole window. The line now reports
  the window **after 60 warmup presents**, and keeps the totals for anyone reading
  the skip arithmetic.
* It now carries the **worst single present**, because an average alone cannot be
  interpreted: 80 ms is a uniformly slow blit if the worst frame is 80 ms and one
  freeze the other 299 frames are averaging away if the worst frame is seconds, and
  those two need opposite responses. On every reproducible run the worst frame was
  2.5–7.7 ms.

So the line's shape changed with the fix — the format in
[06](06-display.md#a-mismatch-selects-the-rung-by-itself) is the current one, and
this is a real S10.3 reading rather than an illustration:

```
PRESENT: 300 presented / 300 flips, avg 3.34 ms per present after 60 warmup, worst 7.66 ms (60 Hz budget 16.67 ms), skip 1
```

## Boot time

**Time from power-on to the rekordbox UI is a first-class number for this
target, and it is now carried by the installer rather than by a recipe.**
[`scripts/device/boot-trim.sh`](../scripts/device/boot-trim.sh) does the work —
verbs `apply`/`revert`/`status`/`report` — and `install.sh` runs `apply` as its
eighth section, after the `/dev/fb0` and `/dev/snd/seq` warnings so an operator
sees a hardware complaint before a trim reports success. It is idempotent, it
edits `/boot/firmware` itself rather than printing lines to paste, and every item
is reversible.

### The baseline, and the two ways it is easy to get wrong

One boot, one `boot_id`, taken on the ollama-free unit before anything changed,
and kept on the unit as `/root/boot-before.txt`. Every figure below is
`ActiveEnterTimestampMonotonic` (or the launcher's own journal echo), so there is
one basis throughout:

| milestone | monotonic s | note |
|---|---|---|
| kernel done | 2.243 | `systemd-analyze`'s kernel term. The **firmware/bootloader** span before it is in none of these figures |
| `/boot/firmware` mounted | 6.928 | after `systemd-fsck@p1` (928 ms) |
| **sysinit.target** | **11.905** | reached only after cloud-init's three stages have run |
| basic.target | 11.915 | |
| network.target | 16.551 | after NetworkManager's own 4.370 s |
| `NetworkManager-wait-online` | 22.528 | it exists **only** for cloud-init |
| network-online.target | 22.532 | |
| **multi-user.target** | **22.549** | |
| **rblive4.service** | **22.558** | 9 ms after multi-user — gated by it exactly |
| launcher's first echo | 22.642 | |
| first `fix-dev` line | 32.484 | **9.84 s of nothing, measured** |
| launching rbp | 33.753 | |
| **rbp pid ready (UI up)** | **35.903** | |
| `usb-watch` / `display-watch` | `n/a` | the launcher's two *distinct* echoes for these were added by this work; both watchers previously logged the byte-identical `started pid N`, so the rows were unattributable. The next `report` populates them |

Three more rows from the same report, and they are the ones the trim moves:
`reloads 5`, `cloud-init enabled`, `apt-daily yes`.

**This baseline is a real boot, but it is not a reproducible one, and that was
only discovered at S11.6.** The journal keeps the boot before it, `54fffd40`, also
pre-change, which reached `rbp-ready` at **29.623 s** — 6.3 s earlier for an
identical configuration. So a single pre-change boot has a spread of at least
6.3 s, and the figure above is the *slower* of the two. Everything in this section
that compares before and after is therefore stated as a range across boots, and
the claims the trim actually rests on are the categorical ones (the `reloads`
count, which rows read `n/a`, whether cloud-init is in `blame`) rather than any
one number of seconds. See [the S11 drills](#the-s11-drills) for the full series.

1. **There is no RTC battery, so the clock starts ~19 s slow and
   `systemd-timesyncd` steps it forward once, early.** The step is one line in
   the journal: `Initial clock synchronization to Sun 2026-09-27 10:15:19.895445
   EDT` at **monotonic 39.117 s** — before it the wall/monotonic offset is 0,
   after it about **+19 s** (18.4 s if it is read off the journal's own
   second-resolution stamps, 19.3 s from the message's fractional one; the
   difference is the truncation, not a second step). So `uptime -s`, which
   computes *now* − uptime, reports the boot ~19 s late once the sync has
   happened: on boot `430f86ae` it says `10:14:40` where that boot's own first
   journal line puts the start at `10:14:21.5`. The rule this gives is **narrower
   than "wall clocks are wrong"**, and it was worth measuring: a delta that stays
   *inside* the pre-sync window is right — the wall delta from the first journal
   line to `rbp-ready` agrees with the monotonic one to within ~1 s on four boots
   (both pre-change ones, a trimmed one, and the one after `install.sh`) — while
   anything that **spans** monotonic ~39 s, or any absolute reading taken after
   it, is off by the step. That is still not a property to lean on, because the
   sync lands whenever the network does and the trim's whole purpose is to move
   the UI earlier. **All boot numbers here come from
   `journalctl -o short-monotonic` or `ActiveEnterTimestampMonotonic`.**
2. **`systemd-analyze` excludes kernel time and the journal includes it.**
   `multi-user.target reached after 20.306s in userspace` and the journal's
   `Reached target multi-user.target` at 22.549 are the *same moment*
   (20.306 + 2.243). Quoting the two bases in one table makes a baseline look
   self-contradictory, and `boot-trim.sh report` exists partly so that cannot
   happen twice: it prints `ActiveEnterTimestampMonotonic` for units and targets
   alike.

### What the measurement found

**The launcher's step 1 was 9.3–13.3 s of pure overhead, and systemd said so.**
Every `systemctl mask`/`disable` in `start-rb.sh` triggers a full manager
reload — 1.8–3.6 s each here, three-plus of them, and **every call behind them
was a no-op on this unit**: `lightdm`/`gdm3`/`sddm` are `not-found`, `udisks2` is
already masked on disk, and `getty@tty1` is already disabled *and* inactive. A
read-only `is-enabled`/`is-active` costs 31–40 ms and reloads nothing. Each call
is now behind a read-only guard, and `report`'s `reloads` row is the regression
test — [11](11-runtime-launcher.md) has the guard in full, including why the
getty test is `is-enabled` and not `is-active`. The window it closes is the
launcher's own log echo to `fix-dev`'s first line, which is why it can be read off
the journal for any boot rather than inferred.

**Read the `reloads` row with its callers, not on its own: it counts the whole
boot, so anything *you* did after it started is in the number.** Every
`boot-trim.sh apply|revert` ends in a `systemctl daemon-reload`, and so does
`install.sh`, so a boot during which one was run reads high for a reason that has
nothing to do with boot. Measured twice on 2026-09-27: the boot after the `/dev`
repair read **6**, and `journalctl -b | grep 'Reload requested from client'`
named them — one `NetworkManager.service` at 10:44:21 and **five** from
`session-*.scope` at 10:46 and 10:47, which was the round-trip test of the new
`revert unit` running over SSH. The boot before it read **5**, of which one was
NetworkManager's and four were that boot's 10:21 `install.sh` session. `report`
cannot tell those apart from the launcher's, so the caller lines are the evidence
and the launcher's share is the figure that has to be 0.

**cloud-init was on the critical chain, not beside it.** `sysinit.target` cannot
be reached until `cloud-init-main` (4.058 s), `-local` (0.729 s) and `-network`
(0.158 s) have run, and the imager's seed **was still on the FAT** so it redid
that work every boot. `NetworkManager-wait-online`'s 5.97 s existed only for it:
`systemctl list-dependencies --reverse network-online.target` returns exactly
`cloud-config` and `cloud-final`. The attribution risk — that something else might
pull the target in — was closed by measurement: `apt-daily.service` and
`apt-daily-upgrade.service` carry `After=network-online.target` but **`Wants=no`**,
and `After=` alone does not activate a target. `rblive4` needs no network at all.

**`rblive4.service` was gated by `multi-user.target` and nothing else** — it
started 9 ms after it. The launcher needs the framebuffer, `/dev`, `/proc`, `/sys`
and `/tmp`, all of which are up at `basic.target`, so waiting for `multi-user`
bought nothing. It is now `After=basic.target`, stated explicitly rather than left
to `DefaultDependencies`, and `WantedBy=multi-user.target` stays so
`systemctl enable` remains valid. Undoing it is a drop-in rather than an edit:
`revert unit` writes
`/etc/systemd/system/rblive4.service.d/rblive4-wait-for-multi-user.conf` with
`After=multi-user.target`, which works because drop-ins *append* to list
directives — so it is the one thing that can add an ordering back to a shipped
unit nobody wants to edit, and it survives a later `install.sh` (which replaces
that file unconditionally). `apply unit` deletes the drop-in again.

**How much that was worth is smaller than the baseline suggests, and the 12.9 s
first quoted here was an artefact of Trap 2.** It came from subtracting
`multi-user`'s *monotonic* stamp from `basic.target`'s *userspace* one — 22.549 −
9.671 — which inflates the span by exactly the kernel term. Read in one basis,
the baseline's `basic.target` → `multi-user.target` was **10.634 s** (22.549 −
11.915) and the earlier pre-change boot `54fffd40` measured **9.056 s** (19.778 −
10.722). What the reorder saves is precisely that span, because `rblive4` no
longer waits for it — and **the span is a property of the boot, not of the
change**, which is why the two cannot be added up. Every span recorded:

| configuration | `basic.target` → `multi-user.target` |
|---|---|
| pre-change `54fffd40` | 9.056 s |
| pre-change `229cfdd9` (the baseline) | 10.634 s |
| S11.2 (cloud-init off, reorder not yet in) | 5.715 s |
| S11.4 / S11.5 (trimmed) | 5.243 / 5.217 s |
| S11.6 / S11.6b (reverted, cloud-init back) | 4.946 / 3.248 s |
| after `install.sh` | 4.398 s |

So the reorder is worth the span *of the configuration it runs in* — **4.4–5.7 s**
as the unit now ships, not 12.9 — and quoting the baseline's 10.6 s would be wrong
twice over: that is the *untrimmed* boot's span, and cloud-init was what held it
open. The mechanism is unconditional in a way none of these
numbers are: `rblive4` starts at `basic` + ~0.05 s instead of `multi-user` +
~0.005 s in every boot since. The same caution applies in the other direction:
**the trimmed configuration is also far more reproducible.** Three trimmed boots
reached `rbp-ready` at 17.930, 18.006 and 18.164 s, a spread of 0.234 s, because
rbp no longer waits on the part of the boot that varies (network, `wait-online`,
`multi-user`). Before the trim the same figure ranged over 6.3 s.

**The apt timers ran at boot because `Persistent=true` catches up a missed
window**, not because anything asked them to: `apt-daily.timer` is
`OnCalendar=*-*-* 6,18:00`, `RandomizedDelaySec=12h`, with **no `OnBootSec`**.
Together 3.175 + 1.907 = 5.08 s.

### What the trim changes

| item | what it does | how it is undone |
|---|---|---|
| `services` | masks `$RB_MASK_SERVICES`, disables each *present* `$RB_STOP_SERVICES`, and disables `getty@tty1` when `RB_DISABLE_GETTY=1` — at install time, where a reload is free. **Never masks `getty@tty1`**: that breaks `systemctl start getty@tty1`, the documented console-recovery idiom | `revert services` unmasks and re-enables |
| `cloudinit` | touches `/etc/cloud/cloud-init.disabled`, **disables** (not masks) the five `cloud-*` units, and moves the imager's seed to `/root/cloud-init-seed-<date>/` (dir 700, files 600) | `revert cloudinit` re-enables and moves the seed back |
| `apt` | a `[Timer] Persistent=false` drop-in per timer. The calendar window is untouched, so the update still runs whenever the unit is up at its slot | `revert apt` removes the drop-ins |
| `unit` | installs the shipped unit when the installed one still has `After=multi-user.target`; deletes the revert's drop-in if one is present | `revert unit` writes a `rblive4.service.d/` drop-in re-adding `After=multi-user.target` (the shipped unit is never edited) |
| `bootfiles` | `/boot/firmware/config.txt` (`boot_delay=0`, `disable_splash=1`) and `cmdline.txt` (drop the imager's `ds=` hint; ensure the `video=`, `fbcon=`, `console=`, `consoleblank=`, `vt.global_cursor_default=` tokens and `quiet loglevel=3`) | `revert bootfiles` restores from the backup |

**How cloud-init actually gets removed, measured rather than assumed.**
`cloud-init-generator` has exactly **one** output: the symlink
`/run/systemd/generator.early/multi-user.target.wants/cloud-init.target`. Its only
input is `ds-identify`'s exit code, and `ds-identify`'s `is_disabled()` consults
`/etc/cloud/cloud-init.disabled` **before** it searches for a datasource — so the
marker makes it return 2, the generator deletes that symlink, `multi-user.target`
stops wanting `cloud-init.target`, and nothing pulls the five units or the
`network-online.target` behind them. Verified on the unit 2026-09-27 by running
the generator against a faked `/proc/cmdline`: with the marker, **every** variant
— including the unit's own `ds=nocloud` token — gives `ds-identify` **rc=2** and
**no symlink**.

> **A trap for anyone re-checking this by hand.** `ds-identify` caches its verdict
> in `/run/cloud-init/.ds-identify.result` and returns the cached value unless
> given `--force`. A hand run without `--force` reports the **boot's** answer, not
> the current state — which reads as "the marker does nothing at all". That wrong
> reading was reached and discarded here before the cache was found. Delete the
> file, or pass `--force`, before believing it.

So the marker is the *mechanism*, and the `systemctl disable` of the five is belt:
`cloud-init.target`'s own unit file names all five in a `Wants=`, so their on-disk
enablement symlinks are redundant with it, and clearing them means nothing re-arms
the five if something ever wants that target again. The `ds=` token removal is belt
for the marker — it is the hint `ds-identify` would answer from if the marker were
ever removed — and not what disables cloud-init. Because the marker is one file,
`revert cloudinit` is `rm` plus re-enable, and the whole item is reversible without
physical access.

**Disabling cloud-init is safe only if the network does not depend on it, and
that was checked on the unit rather than inherited from a recipe:** the WiFi
profile is a persistent NetworkManager keyfile (mode 0600, mtime = the image
build, no cloud-init marker in it), the `/etc/netplan/90-NM-*.yaml` files are
**empty stubs**, the hostname predates boot and is in `/etc/hosts`, and
`/etc/ssh/sshd_config.d/50-cloud-init.conf` persists on disk.
`apply cloudinit` re-runs that check and *refuses* (warns, does not die) if a
keyfile is missing or a netplan file has become non-empty.

**And the SSH service's own ordering was checked, because it names a cloud-init
unit.** `ssh.service` carries `After=cloud-init-network.service` and is itself
`WantedBy=cloud-init-network.service` — neither is in `ssh.service`'s unit file.
Both are injected by cloud-init's generator, and with the marker in place the
generator emits nothing, so `ssh.service` reverts to its own
`After=network.target` (NetworkManager, which is enabled and has no cloud-init in
its `WantedBy`/`RequiredBy`/`After`). Even if that ordering survived, it would be
vacuous: ordering against a unit outside the transaction is satisfied
immediately. Nothing `Requires=` any cloud-init unit (measured: `RequiredBy` is
`<none>` for all five).

**The seed may hold the WiFi PSK and an SSH key, so it is moved and never
printed** — by `apply`, `status` or `report`. `status` names the backup
directory, which is how it is findable for a re-image.

**`/boot/firmware` is FAT with no journal, on a unit with a brownout history**,
so the three rules that come from the partition rather than from taste are: one
backup per file, created once and never overwritten (`cmdline.txt.rblive4.bak`,
deliberately a different *shape* from the operator's own
`cmdline.txt.bak-1280x800` / `-1080p` so the two are never confused); write a temp
on the same partition and `mv` over; and verify before replacing — still exactly
one line, and every root-finding token still present. A refused edit leaves the
original alone and names the candidate.

**Two `video=` tokens for one connector is a silent last-one-wins**, so `apply`
**replaces** an existing `video=HDMI-A-1:…` rather than appending a second.
**`HDMI-A-2` is deliberately not added**: `status bootfiles` names that gap
instead of closing it, because the A-2 token is S10.8's precondition rather than a
boot-time trim, and forcing a mode on a connector with nothing attached is not
free. **`console=ttyS0,115200` is not touched either** — it is already inert
(`8250.nr_uarts=0`, no `/dev/ttyS*`, `/proc/consoles` lists only `tty1`), so
removing it would buy nothing and the console line is the riskiest edit available
on a remote unit. **`quiet loglevel=3` is the one item here that is expected
rather than measured**: it suppresses console `printk`, `dmesg` keeps every
message, and the console is invisible anyway under `fbcon=map:1` — so it is the
first thing to drop, and `revert bootfiles` takes it back.

### The S11 drills

**Land one item, reboot, `report`, diff against the previous `report`, and only
then the next.** That is method, not tidiness: the reload cost in step 1 was
inflated by cloud-init running at the same time, and removing cloud-init is what
takes the last gate off `multi-user.target`, so landing them together would make
it unknowable which trim bought what. All seven rows have been run, and the
`S11.6` row is the one that changed what this section can claim.

| # | Drill | Pass | Fail |
|---|---|---|---|
| **S11.0** | one clean baseline on the ollama-free unit, kept as `/root/boot-before.txt` | **Done, 2026-09-27.** Every figure from one `boot_id` (`229cfdd9`), monotonic throughout, with the inputs block filled in — the table above. `rbp-ready` **35.903**. **Not reproducible**: the earlier pre-change boot `54fffd40` measured **29.623** for the same configuration, so treat this as the slow end of a range and not as *the* baseline | a figure quoted from `systemd-analyze` next to a journal one ⇒ Trap 2, and the comparison is void |
| **S11.1** | `apply services` + `apply cloudinit`, reboot, **and SSH first** | **Done.** SSH answered in ~24 s and the network was up; `cloud-init status` = disabled; `cloud-init.target` = static; no `cloud-*` unit in `blame`; **`sysinit.target` is no longer gated by cloud-init**; the `wait-online` **and** `net-online` rows both read `n/a`; the generator re-run *for real* emits no symlink (`ds-identify` rc=2). `rbp-ready` **35.903 → 29.990**. The `reloads` count fell **5 → 4**, not to 0 — one of the baseline's five was cloud-init's own, so step 1 has not landed and the other four are still there | no SSH ⇒ the pre-flight was wrong and the seed goes back (`revert cloudinit`); cloud-init gone but `wait-online` still on the chain ⇒ something else wants `network-online.target`, and the reverse-dependency listing names it |
| **S11.2** | then `apply apt` + reboot | **Done.** Both timers `LAST = -`: nothing fired, where S11.1's boot had a catch-up at 09:48:10. The calendar windows are intact (next 19:29 and 06:20), so this is a deferral and not a disable. No NM dispatcher hooks (`[]`), and nothing apt-related in `blame`. `rbp-ready` **→ 26.540** | the timers still firing at boot ⇒ either a dispatcher hook or `Persistent` did not take; `status apt` says which |
| **S11.3** | then the step-1 guard + `systemctl restart rblive4` | **Done, no reboot.** Reloads since the restart = **0 attributable to the launcher** (the boot's total is 1, and it is NetworkManager's own). The launcher→`fix-dev` window fell from **9.309 s to 2.583 s inside that one boot** — the same launcher, one restart apart — and the journal says what went: three `Reload requested … (unit rblive4.service)` lines fill the first and the second has none. The two watcher rows are populated and distinct (123.676 / 124.709) | reloads still non-zero ⇒ a call is still unguarded; the watcher rows both showing the same line ⇒ the two echoes are not in the running launcher |
| **S11.4** | then the ordering change + reboot | **Done.** `rblive4` started at **8.982 s** — after `basic.target` (8.938) and *before* `network.target` (13.910) and `multi-user.target` (14.181); `wait-online`/`net-online` still `n/a`. **The media still binds**: both mounts up, and rbp's own fds name it — `/proc/<pid>/fd` holds the `udev_usb1` FIFO and `…/PIONEER/rekordbox/export.pdb`. `NRestarts=0`, `Result=success`. **`/dev/fb0` was there first**: `vc4drmfb frame buffer device` registered at 8.776 s, 206 ms *before* the unit started. Confirmed on two further boots (8.000 s before 8.954; 9.308 s before 9.589). `rbp-ready` **17.930** | a first-attempt failure here lands in `Restart=always`'s 10 s loop and *looks* like a slow start — read `systemctl status` and the journal, not the screen. `/dev/fb0` missing ⇒ the fallback in the unit's comment (`Wants=` **and** `After=systemd-udev-settle.service`) |
| **S11.5** | `apply` a second time with no reboot | **Done.** The second run printed **no diff at all** — `cmdline.txt unchanged`, `config.txt unchanged`, `bootfiles: keeping the existing backup` — and `/proc/cmdline` after the reboot shows each token exactly once (`video=` 1, `fbcon=map` 1, `consoleblank=` 1, `quiet` 1, `loglevel=` 1, `ds=` **0**) with one `[all]` section in `config.txt`. `quiet loglevel=3` did **not** cost `dmesg`: 552 lines, first one `[0.000000] Booting Linux`. `rbp-ready` 18.006 | a second `video=` token, or a second `[all]` append ⇒ a guard is testing the wrong thing, and the backup is what recovery starts from |
| **S11.6** | `revert` + reboot | **Done, and it passes on every item — but not on the number the plan predicted.** `revert` restored every system-state item, verified one by one: marker gone, all five cloud units re-enabled, the seed back on `/boot/firmware`, apt drop-ins removed with `Persistent=yes` back, `ds=` back on the cmdline, `config.txt` back. The reboot confirmed it *categorically*: cloud-init ran again (all five units in `blame`, generator symlink present), `wait-online`/`net-online` populated again, `apt-daily yes`. `revert services` said **"nothing recorded"** — correct, because on this unit `apply` changed nothing there (udisks2 was already masked by hand; no display manager is installed). **`report` did not match S11.0** (24.215 then 19.327 across two boots) and that is the finding above, not a failed revert: the baseline is a 6.3 s range. The `unit` item was the one the script would not revert at the time of this drill — it is a shipped-file change and it refused by name rather than pretending; **the drop-in that closes that gap was written after this drill and has not been drilled** (see the item table above) | a row that does not come back ⇒ that item's revert is incomplete, and `revert` says which ones it cannot do |

**`install.sh` carries all of it**, and it has been run end to end on the unit
twice: the 8th section ran `boot-trim.sh apply` at its default `RB_BOOT_TRIM=1`,
re-applied every item, copied `boot-trim.sh` to the deploy root, and installed the
ordered unit — and after both runs the deploy root's `boot-trim.sh` and the
installed unit hash identically to the repo's. The second run printed no diff and
`status` still says *every item checked is applied*, which is the idempotency
claim holding under the installer rather than by hand. The closing text it prints
points at `status`, `report` and `revert` instead of asking anyone to paste a
recipe, names the `HDMI-A-2` gap as a gap, and warns that the
`report > /root/boot-before.txt` line inside it would overwrite a baseline already
sitting there. The reinstall **did not touch what was already working** — the
display shim's hash, the loader's, and the chroot's own file count
(`find … -xdev -type f`, which prunes the bind-mounted `/proc`, `/sys`, `/dev` and
`/tmp`: **605** before and after) were identical, and the running player was not
restarted. The never-overwritten backups on the FAT also survived a revert
followed by a reinstall, which is the property that makes recovery from a bad
`/boot` edit possible.

**Two things no drill here can settle, and they are stated rather than implied.**
**Firmware/bootloader time** is invisible to `systemd-analyze` and to the
journal, so power-on→UI is every figure above plus an unmeasured span — which is
why `boot_delay=0`'s 1 s is labelled *expected*, not measured. And **the
no-monitor boot** is S10.9, still unrun, which is exactly the case where an
earlier start could now fail its first attempt inside `Restart=always`'s loop.

**What this can promise, and what it cannot.** Not a single number — the
pre-change configuration spread 6.3 s across two boots, so no single figure is
honest. What it can promise is the categorical effect, and the shape of the
end-to-end result:

| configuration | `rbp-ready` | note |
|---|---|---|
| pre-change, before the trim | **29.623 s** and **35.903 s** | two boots, same configuration: the spread is the point |
| reverted, guard + ordering only | **19.327 s** and **24.215 s** | still the spread; cloud-init, the apt timers and the `/boot` tokens are all back |
| trimmed (S11.4, S11.5, and after `install.sh`) | **17.930 / 18.006 / 18.164 s** | spread of 0.234 s — rbp no longer waits on the part of the boot that varies |

So the honest headline is **about 12 s off the typical boot and 18 s off the
worst**, and *the end state is far more predictable than the starting one*.

**The dominant single item is the step-1 guard rather than the reorder, and S11.3
measures it inside a single boot.** That boot ran the unguarded launcher at 13.8 s
and the guarded one on a restart a hundred seconds later, so the two
launcher→`fix-dev` windows — 9.309 s and **2.583 s** — differ by the guard and by
how busy the rest of the system is, and the journal says which: three
`Reload requested from client … (unit rblive4.service)` lines fill the first, and
the second has none. Across the boots that window was **9.3–13.3 s** before the
guard (13.292 S11.1, 9.842 baseline, 9.309 S11.2) and **4.1–5.1 s** after (4.110
after `install.sh`, 4.452 S11.6, 5.122 / 5.126 S11.4 / S11.5), with the boot's
reload count falling **5 → 1** and the launcher's share of it to zero. The two
reverted boots cannot separate the guard from the reorder, because at the time of
that drill the ordering lived only in the shipped unit and `revert` did not touch
it (the drop-in above was written afterwards) — they carry *both*, and
land 10.3 s and 11.7 s better than the pre-change pair. Subtracting one term from
the other would be exactly the arithmetic this section warns against: the terms
overlap, `basic` → `multi-user` shrinks on its own when cloud-init leaves (the
table above), and the reload cost was inflated by cloud-init contention to begin
with. That is why the wins do **not** add, and it is a measurement the drills
produced rather than an arithmetic the plan predicted.

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
   derived. A USB touch panel is now the pointer (`TSTP CTouch`), and S3.2's
   three taps settle it: `point_xform_abs()`'s law reproduces every emitted
   coordinate exactly, so `POINT_SWAP_XY`/`POINT_INVERT_X`/`POINT_INVERT_Y` are
   all correctly left off and no `TouchCalib` affine is needed. The mouse's own
   half of S3.3 is still unrun — the arrow is the `rel` device's alone, and no
   human has watched it follow. See above.
5. **Every DDJ-FLX4 MIDI detail** — see [15 — DDJ-FLX4 MIDI](15-flx4-midi.md).
   None of it can block the port: the keyboard map plus the fixture tests keep
   everything else moving.
6. **Which of the 68 patches are load-bearing on the Pi.** Each one's
   justification carries over, but only `0x3C665C` (the audio `scanForDevices`
   patch) is provably necessary in advance, because the Pi 4 fails the same
   `board_is_rev` check. The rest are re-tested per S6 in
   [12 — troubleshooting](12-troubleshooting.md).
