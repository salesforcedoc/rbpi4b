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
├── install.sh       one-time (and idempotent) deploy
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
The alternative that removes the cost *and* the flicker is a DRM cursor plane,
which is not attempted — see [07](07-touch.md#the-arrow-on-the-screen-point_cursor).

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
* **The FLX4's meters are a different kind of meter, so the VU bridge is off.**
  `RB_LED_VU=0` is the setting, and it does more than silence output: it skips
  `install_meter_hook()` entirely, so `rbp`'s machine code is never patched for a
  meter this bridge cannot read. The reason is not that the unit lacks meters —
  Pioneer's list documents a **CH LEVEL METER on CC 2**, one continuous value per
  channel — but that the bridge drives an 11-segment **bitmask** and this is a
  value ramp; feeding one to the other needs a different bridge, and the ramp has
  not been measured here ([15](15-flx4-midi.md#the-leds)). The earlier claim that
  the unit "has no level meters" was wrong.
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
| S1.4 | `kmsprint -m`; `aplay -L`, `/proc/asound/cards`; `aseqdump -l`; `aplay --dump-hw-params -D hw:CARD=DDJFLX4,DEV=0` | **measured.** Connector `HDMI-A-1`. Card id `DDJFLX4`; port name `DDJ-FLX4 MIDI 1`; the unit enumerates as `2b73:0045` (AlphaTheta DDJ-FLX4) and `snd-usb-audio` probes it (`usb 1-1.2: Quirk or no altset; falling back to MIDI 1.0`). The card's own hw params, which settle the audio design: `FORMAT S16_LE S24_3LE`, `SAMPLE_BITS [16 24]`, `FRAME_BITS [64 96]`, `CHANNELS 4`, `RATE [44100 48000]`, `PERIOD_SIZE [45 48000]`, `BUFFER_SIZE [90 96000]` — **no `S32_LE`**, which is why the shim's hand-rolled `SND_PCM_FORMAT_S24_3LE` being wrong (10 = `S32_LE`) could never have worked. Use `hw:` for this query: a plug device answers with what the plug layer will accept, not with what the card has | card id differs → fix `rb.conf`. Both formats it offers are accepted by `RB_AUDIO_FMT`; the default `s24_3le` is the one that needs no conversion |
| S1.5 | chroot sanity: `chroot … /bin/sh -c 'echo ok'`; run `edb_streamd` | `ok`; daemon stays alive | exec bits, missing binds |
| S2.1 | the connector query (`/proc/cmdline`, the `modes` files, `dmesg`), then the `cmdline.txt` recipe + reboot | **answered, and in the best way.** First half: the sink is on `HDMI-A-1` (`connected`/`enabled`, `HDMI-A-2` `disconnected`) and its mode list tops out at `1280x720` (four times) plus `960x600`, `960x540`, `800x600`, `480x320` — no mode ≥ 1280×800, **so `letterbox` is unreachable on this display.** Second half: the mode was forced anyway and **the panel took it** — `fbdump` after the reboot reads **1280×800, 16 bpp RGB565, `line_length` 2560, `smem_len` 2048000, one page**, i.e. `rbp`'s logical surface exactly. `letterbox`/`crop`/`scale`/`convert` are all moot here | it did not reject it, so the old "fall back to crop/scale" branch is dead on this unit; it stays in the present path's table for a sink that caps at 720p |
| S2.2 | launch with `crashcatch.so`, then verify against the **generated** `usr/etc/directfbrc` — no hand-added lines. `debug=FBDev/Mode` is only needed to see the arithmetic, and is *not* in the shipped file | **PASSED.** The `PRESENT:` line reads `mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)` — `pages=1 pan=0` is the page-count decision having been taken from the real geometry, and `mode=off` says the layer surface *is* the fb page, so no blit happens at all. The UI was then confirmed by capturing `/dev/fb0` and decoding it as RGB565: both decks, HOT CUE A–H on each, BEAT FX/DELAY at 120.0 BPM, QUANTIZE, TRACK/REMAIN/TEMPO and the waveform lanes all render. Before the page fix, the fb's one page against a forced `DLBM_TRIPLE` gave `need_mem` `2560×2400` = 6,144,000 > `smem_len` 2,048,000 and no UI. See [the present path](#the-present-path) | no UI with the `PRESENT:` line reading `pages=3 pan=1` → the page arithmetic is still reading `ypanstep`, not the fb; no UI with `pages=1` → the page decision is right and the blocker is downstream of it, and the `FBDev_Mode` line names what it is. **Two deploy-level blockers sat in front of this milestone and neither is Pi-specific** — both are now fixed in the tree, and both are worth knowing because each mimics a code bug: (1) `module-dir` missing from `directfbrc`, so *no DirectFB module loaded* and rbp segfaulted on a NULL `IDirectFB*` ([06](06-display.md#required-directfbrc), [12](12-troubleshooting.md)); (2) AppleDouble `._*` files from a macOS build host in the module directories, which DirectFB tries to dlopen ([12](12-troubleshooting.md)) |
| S2.3 | watch the running image for tearing, then re-launch with `RB_DFB_PRESENT=convert` | same image, no tearing, and the pacer holding the render thread to ~60 fps. `convert` on a 565 fb is a 1:1 whole-frame `memcpy` into a system-memory back buffer, which is the double-buffering `off` cannot have | tearing that survives `convert` → the blit is not reaching the visible page; the pacer's absence shows as a pinned core instead, at 100% on the render thread |
| S2.4 | idle 10 min, then type on the console | no text or cursor ever over the UI | `fbcon=map:1` not applied (`cat /proc/cmdline`) |
| S3.1 | `tools/evdevdump --list` | **run, and a pointer is present.** `Logitech G203 Prodigy Gaming Mouse` — a mouse, so `RB_POINT_KIND=rel`, which lives in `rb.local.conf` because a value measured here would be reverted to `auto` by the next deploy (see [scripts/device/README.md](../scripts/device/README.md)). `auto` would also find it; pinning it is what stops the shim hunting for a touchscreen that is not there. **Do not pin the `eventN` number anywhere**: this row first recorded `event7` and a later read of the same unit saw the same mouse on `event3`, because the number is assigned at enumeration and moves. Match on the name and the capabilities — which is what both readers do — and read the node number out of the log when you need it | set `RB_POINT_DEV` only to override discovery, and prefer the name over a node; if no pointer is found, `evdevdump --list` says so and only the keyboard map is available |
| S3.2 | `POINT_DEBUG=1`, click the four corners | emitted logical coords match the UI's reaction | mirrored/transposed → `RB_POINT_INVERT_X` / `RB_POINT_SWAP_XY`, else the `TouchCalib` affine |
| S3.3 | drag-scroll the browser, tap PLAY, tap a playlist row | **the code path is verified; the physical mouse has never been moved.** Discovery, the `rel` accumulation and the emitted `(down, raw, logical)` triples are all exercised, but no human has watched the on-screen cursor follow the G203, so "clicks land where you aimed" is still unmeasured | fast clicks swallowed → raise `RB_POINT_MIN_DWELL_MS`; a cursor that does not move at all → check `/tmp/pointsrc.log` and the `subscribed to` line that names the node it opened |
| S4.1 | `speaker-test -D hw:CARD=DDJFLX4,DEV=0 -c 4 -r 44100 --format S24_3LE -t sine` with the FLX4's MASTER level up | **still needs ears** — this is the one step no log can answer. It plays each of the 4 hardware channels in turn, which both proves the card makes sound and identifies which pair is the RCA out versus the headphone jack, validating `AUDIO_MAP=master=0,1;headphones=2,3` | silence → the card's own mixer/level, not the shim (nothing of ours is loaded here); channels not 1:1 → logical count ≠ hw count, see below |
| S4.2 | launch `rbp`, `/tmp/audioshim.log`, load and play | **verified, and the silence that was blamed on this row was never in the audio path.** The log reads `format constants verified: S16_LE=2 S24_LE=6 S24_3LE=32`, `real set_format(32) res=0`, `negotiating 4 channel(s)`, `real hw_params res=0`, `resolved 4 channel(s): master=0,1 headphones=2,3 booth=-1,-1`, and `writei frames=64 bytes=768 written=64` at ≈realtime with **zero** negative returns — 768 bytes ÷ 64 frames = 96 bits/frame is exactly the card's `FRAME_BITS` ceiling, so the packing is byte-correct. The port was nevertheless **silent with a deck playing** (2026-09-26), and the log said why once `peak_m` was read rather than skimmed: `peak_m=0` on all 242 blocks — digital silence *from rbp*, pre-gain. rbp's mixer builds its **channel faders at zero**, and nothing on this port ever set them. Seeded at unity from the shim ([09](09-audio.md#audibility-rbp-builds-the-channel-faders-at-zero)); audio now flows with no operator action | `written=-22` in a loop ⇒ the format constant or the device string, see [12](12-troubleshooting.md); **silence with a healthy log ⇒ read `peak_m` first — zero means rbp is silent and it is the mixer, not the shim** (the `0x3C665C` patch was the old answer and was not this); distortion ⇒ sign-extension |
| S4.3 | PFL/cue buttons, cue mix and level | cue bus changes, no combing | summing master+cue instead of routing the phone stream |
| S4.4 | leave it playing and pull the FLX4's USB, then plug it back | **run by accident, and it failed: audio did not come back.** The unit's under-voltage dropped the FLX4 and it re-enumerated 5 s later (`USB disconnect, device number 3` → `new high-speed USB device number 7 ... DDJ-FLX4`). The card is present again and `/proc/asound/cards` lists it, but rbp's PCM was opened against the old device, so every write after that returned `written=-19` — ~5.1 million of them — while `peak_m` went on showing music, because it is measured from rbp's own buffer. There was **no reopen path**; the workaround was to restart the player. **Now fixed in the shim** (2026-09-26): the write path detects `-ENODEV`, retires the dead handle and reopens with the negotiation replayed ([16](16-input-and-hotplug.md#4-hot-swap), [09](09-audio.md)). **Re-run this row to verify it** — see S8.6 | a persistent `written=-19` **after** the shim says `MASTER RECOVERED` ⇒ the replay did not take, and the log names the step that failed; `-19` with **no** `MASTER LOST` line ⇒ the `-ENODEV` guard is not being reached, which is a different defect. Always check `dmesg -T \| grep -i usb` for a disconnect/re-enumerate rather than a bad cable, and note this is not the same `-19` as a startup `open()` failure ([09](09-audio.md)) |
| S5.1 | `aseqdump -p <client>:0` while pressing everything, piped into `tools/aseqdump2dump.py --stats --revs <N> -o flx4.dump` | **run — and it converted the map's biggest guess into a measurement.** The inventory is in [15](15-flx4-midi.md); the relative controls use the 0x40-centred convention (platter CC 34 vinyl-on / 35 vinyl-off / 41 `+SHIFT`, jog ring CC 33), and one counted turn gives **720 counts/revolution**. `map_flx4.c`'s `jog_ppr` is now 720: the inherited 128 was the *JP21's* platter, which made every turn 5.6× too fast and pinned it to the 8 rev/s clamp. Still `TODO: unverified`, and a smaller set than before: the pitch fader's polarity, the pad base+pad encoding and its SHIFT pairing, the FX knob targets, BEAT SYNC long-press and 4 BEAT/EXIT | nothing → `snd-seq` / USB |
| S5.2 | `MIDI_DUMP=… RB_MIDI_MAP=flx4`, press each control | **run (2026-09-26), and it is the row that distinguishes a real press from a synthetic one.** The dump records both, and they are told apart by velocity: `seqinject2` sends **100**, the FLX4 sends **127**. So `grep 'vel=127'` is the panel. Note 65 (browse push) and note 70 (LOAD) are **measured** this way; note 66 (SHIFT + browse push) is not yet — the only note-66 lines in the dump are the harness's, so that row's number is still published-only ([15](15-flx4-midi.md)) | wrong channel/note assumptions — that is the dump's purpose. A silent "the button does nothing" may also be the map, not the note: check `subscribed to` in the log to tell "the event never arrived" from "the event arrived and was mis-bound". This row's old advice — that `MIDI_MAP` selects exactly one map and under `kbd` the FLX4 is ignored — was wrong twice and has been removed; the two selections are independent ([16](16-input-and-hotplug.md#two-selections-not-one)) |
| S5.3 | `make -C scripts/shims test` under `qemu-arm` | **eight suites, all green** (2026-09-26): `test_point` (55 checks), `test_audio` (50), `test_midi` (170), `test_flx4` (500), `test_kbd` (367), `test_cursor` (920), `test_evdev` (5 scenarios / 53 checks) and `test_cursor_dev` (14 scenarios / 74 checks). The last two fake the kernel at the `syscall()` boundary — `test_evdev` for the input reader, `test_cursor_dev` for `fb_cursor.c`, which had no coverage at all before it | off-by-one on a CC pair or threshold |
| S5.4 | no configuration needed: with `EVDEV_MAP=kbd` (the default) and the FLX4 on `MIDI_MAP=flx4`, press a keyboard key **and** a controller pad | **both reach rbp** — this is the operator's original "the keyboard isn't doing anything" reversed. Then `1` to load and `space` to play, `↑`/`↓`, `Esc`, right-click. (This row used to say `RB_MIDI_MAP=kbd` first, which is no longer required and no longer sufficient on its own as a description: the keyboard has its own variable.) Note `KNOB_VERBOSE=1` still costs a log line per key and should be off before measuring anything about timing | wrong keycodes; **the selector's direction is the one thing only hardware settles** (see 08); a key that works once and stops ⇒ a lost release, see [16](16-input-and-hotplug.md) |
| S5.5 | pad LEDs (`LED_VERBOSE=1`) | pads mirror | the colour table is a known TODO |
| S6.1 | plug a stick; `usb-watch.sh status` | **run, and now with rekordbox-exported media the whole chain works end to end.** The first stick answered the mount and import questions but was not a rekordbox export: it carried only WAV files and an *empty* `PIONEER/USBANLZ`, no `PIONEER/rekordbox/export.pdb` anywhere, so `attach: rbp never opened export.pdb after retries` was **correct, not a defect** — there was no DeviceSQL database for rbp to import. With the 1 TB export on it, the SOURCE screen reads **USB1: 736 songs, 931.3 GB**, the browse tree fills, a track loads and it plays (2026-09-26). Note the medium's own pre-existing FAT damage and the under-voltage, which are the medium's problems and not the port's ([10](10-usb.md)) | a stick that *has* `export.pdb` and still fails to import ⇒ then the row's original causes apply: wrong ancestor match, `udisks2` grabbing it, or missing PM NULL guards → crash on insert. **An empty UI with the media mounted is not this row**: it is the *screen* — `K_SOURCE` unreachable, see S5.2 and [15](15-flx4-midi.md) |
| S7 | review these docs against the bring-up log | — | — |

## S8 — the hot-swap drills

Five device classes, and until this block is run **not one of them has been
exercised end to end on the unit except the two that already worked**. Each row is
a pull and a replug with the player running; none needs a restart, and every one of
them is expected to recover on its own. The failures are worth reading as *pairs*:
the log line that should appear, and the line that means it did not.

`RB_KNOB_VERBOSE=1` is **not** needed for any of this and should be off — the
lines below are all printed regardless, and verbose adds a line per key which is
noise in exactly the measurement being read. The one exception is S8.5, where it
is the point.

| # | Drill | Pass | Fail |
|---|---|---|---|
| S8.1 | **Keyboard, hot add.** With rbp running and playing, plug in a USB keyboard, then press `space` | `evdev[…] … 'AT Translated Set 2 keyboard' appeared; adding it` with a timestamp, then `kbd: evdev type=1 code=57 value=1 -> 0x4101 press`; deck 1 responds. The `appeared` line must arrive **within 1000 ms of the plug**, and that number is the fix: the deadline is absolute, so it holds even if a mouse is streaming | never appears ⇒ the reader is not scanning (`EVDEV_MAP=none`, or no `EV_KEY` node); appears but late while a mouse moves ⇒ the absolute deadline regressed, and `test_evdev` scenario 1 exists to catch that |
| S8.2 | **Keyboard, hot remove with a key held.** Hold a mapped key down and pull the keyboard's USB | `evdev[…] … went away (…); released 1 held key(s), N device(s) left`, then a `release` line for that keycode, and **the next press is not `ignored`** | no `released N held key(s)` line ⇒ held-key tracking regressed; the next press logging `ignored` is the same defect seen from the map's side |
| S8.3 | **Mouse.** Pull and replug the mouse, then right-click and roll the wheel | `went away` / `appeared` as above, and BACK and selector rotation still work | the *pointer* keeps working while the buttons do not, or vice versa — these are two different modules ([16](16-input-and-hotplug.md#4-hot-swap)) and a report that says "the mouse is fine" may only mean the pointer |
| S8.4 | **FLX4 MIDI.** Pull the controller's USB mid-play, wait ~5 s, replug | `control surface 'FLX4' disappeared (unplugged?); waiting for it to come back`, then `subscribed to N:0 'DDJ-FLX4 MIDI 1'` — **write N down**, because whether the client number changed is the thing this drill exists to record — then press a pad and see it work | the surface is re-found (`subscribed to`) but a pad does nothing ⇒ the recycled-client-id case the retry was written for; press nothing and check the LED/meter route line instead |
| S8.5 | **The double-press question.** With the keyboard attached *and* the controller live, hold the FLX4's PLAY, tap `space` once, release PLAY | **playback continues through the gesture** — **operator-reported pass, 2026-09-26** ("worked as expected, playback did not stop"), and confirmed by the operator as the outcome this row predicts. The second press lands on a key rbp is already holding, and PLAY acts on the press edge, so the only outcomes that would matter are rbp acting on the redundant press or on the early release; neither happened. **No aggregator was built, and none is owed** ([16](16-input-and-hotplug.md#two-selections-not-one)). What *is* still owed is to the record rather than to the result: `/tmp` is tmpfs, the 11:37 reboot destroyed the pre-reboot log, and the surviving window (11:37–11:51) contains **no `KEY_SPACE` (code 57) event at all** while every `note=11` in `/tmp/flx4.dump` is a tap of 100–330 ms and never a hold — so unlike S8.1 and S8.2 this pass has no log behind it. **The two-second corroboration (not a re-run):** with the log live, press `space` once and look for `kbd: evdev type=1 code=57 value=1 -> 0x4101` | playback stops, or stops and PLAY cannot restart it afterwards ⇒ rbp *does* act on the redundant press or the early release, and the per-(keycode, channel) aggregator described in [16](16-input-and-hotplug.md#two-selections-not-one) becomes real work — built on that observation and on nothing else. As run, rbp does neither |
| S8.6 | **Audio.** Play, pull the FLX4, replug (S4.4 re-run) | in order: `writei … written=-19` → **one** `MASTER LOST` line (not one per block) → `writei #` stops → `MASTER RECOVERED: reopened hw:CARD=DDJFLX4,DEV=0` → `replay: access=… channels=4 rate=44100 …` → `the pair map is unchanged: master=0,1 headphones=2,3 …` → `startup mute released after 79424 frames` → `written=64` again **with non-zero `peak_m`**. The operator confirms by ear with no restart | recovers but only the master pair, with the "only the master pair" note ⇒ this was the cold case, which means the shim thought no card had ever been open; `MASTER LOST` repeating ⇒ the guard is not clearing the global; no `MASTER RECOVERED` at all ⇒ watch whether the backoff is climbing (it logs only after three failures) |
| S8.7 | **USB media.** Pull the stick while rbp is browsing it, then replug | `knobshim2: USB removed`, the Source screen clears, then `USB1 detected -> registered (dev=3)` and the label is readable again | the screen keeps showing the removed stick ⇒ the screen, not the mount; **this row is the one nobody has tested** ([10](10-usb.md)) |

Three things to record while running these, because they are numbers no log gives
directly: the **latency** between plug and `appeared` (S8.1, read off the
timestamps), the **client number** the FLX4 comes back on (S8.4), and **the window
the log actually covers**. That last one is not a nicety: `/tmp` is tmpfs, so the
log's first line is the current boot, and a reboot destroys the record of every
drill run before it — silently, and in a way that makes a report look verified.
Measured 2026-09-26: S8.5 passed on the operator's report, and no trace of it
survives, because the 11:37 reboot had already cleared `/tmp`. The pass stands —
the operator ran the drill and is the authority on what they saw; what is missing
is only the log that would corroborate it. Check
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
| S9.0 | **Measure the notes (this is the one that gates the rest).** Run `sh /tmp/ledprobe.sh cand`, watching the panel; then `pair <ch> <n>` for any candidate that lit, then `sweep <ch>` on any channel whose candidates all stayed dark | each control's own note is *seen* to light and clears on velocity 0, and the number goes into `flx4_leds` as a one-line edit | a candidate lights a **different** control than the one named ⇒ the same-note hypothesis is wrong for that row and the sweep is the answer; nothing lights anywhere ⇒ the write is not reaching the panel at all, which is a different problem from a wrong note and should be reported as such |
| S9.1 | **CH CUE tracks rbp.** With a track loaded on deck 1, press the FLX4's channel-1 CUE, then press again | the button's LED lights on the first press and goes dark on the second, matching rbp's own channel CUE state; `LED_VERBOSE=1` logs one `led sch… note… on` and then one `off` | lit but not tracking ⇒ the strip loop is reading `me_get_cue()` wrongly; never lit ⇒ `n_strip_cue` is still `-1` (check the log before the hardware) |
| S9.2 | **BEAT SYNC.** Engage SYNC on deck 1, disengage, then sync and nudge the tempo so rbp blinks it | solid while engaged, dark when disengaged, and it **blinks in step with rbp** rather than staying solid — the state that lights it is `LedStat` id 4, where 2 means blink | solid where rbp blinks ⇒ the `== 2` case is not being honoured; dark while rbp's own screen shows SYNC engaged ⇒ the row is unmeasured or on the wrong channel |
| S9.3 | **Hot-cue pads.** Set two hot cues on deck 1, leave a third pad empty | the two pads light **in the right colours and the third stays dark**; an empty pad is not merely dim | right pads, wrong colours ⇒ the velocity encoding is not the Engine OS convention and `pad_enc`/`RB_PAD_BRIGHT` change; all pads the same colour ⇒ the stored RGB is not being read (`ledstat_rgb()`), which is a different failure from a wrong encoding |
| S9.4 | **PLAY / CUE.** With a track loaded: play, pause, then unload | PLAY solid while playing; PLAY blinks and CUE lights with a track loaded and stopped; both dark with nothing loaded | PLAY lights with nothing loaded ⇒ the `loaded` test; CUE never lights ⇒ `n_cue` still `-1` |
| S9.5 | **MASTER CUE.** Press the unit's MASTER CUE button, then press it again | the LED follows the button, and rbp's own master-cue state agrees — this one is engine state rather than a mirror, because `me_set_master_cue()` is asserted at startup and the button toggles the same state | the LED and rbp's screen disagree ⇒ the button is bound to a different call than the LED reads |
| S9.6 | **Absent rows stay absent.** With `LED_VERBOSE=1`, exercise the controls this unit does not have (there are none for Colour FX, KEY LOCK, VINYL, SLIP and the loop section) | **no** send is logged for those rows; the FLX4 has no loop section and no KEY LOCK/VINYL/SLIP LED, and the four Sound Color FX rows are permanently `-1` | a send appears for a row this unit does not have ⇒ a row was filled in that should have stayed `-1`, and on this panel a wrong note is a control, not a dark LED |
| S9.7 | **No regression on the previous target.** Run the suite and read the counts | `test_midi`, `test_flx4`, `test_kbd` green with the JP21 move a pure move — the SC Live 4's behaviour byte-for-byte unchanged | a JP21 count changes ⇒ the move was not pure, and `test_midi`'s exact pins name the row |
| S9.8 | **Hot-swap the FLX4 with the bridge on.** Pull the controller mid-play, wait, replug (this is S8.4 with the LEDs live) | no crash, and the LEDs **come back on replug** — `midi_note()` retries every tick while there is no route, so the mirror re-establishes itself without a restart. Write down whether the client number changed (S8.4's number) | the LEDs never return after replug while the controls do ⇒ the LED output route did not re-subscribe, which is a different code path from the input subscription and worth reporting separately |

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
**S10.0, S10.5's different-monitor half, and S10.7–S10.10 are still unrun**, and
the cells that say *write this after running it* still say it. The `scale` rung
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
| S10.5 (E4) | unplug/replug **at the same mode** with `video=` set; then swap in a *different* monitor (a 1080p TV is the realistic stand-in) | **Pass on the same-mode half, 2026-09-26 — and it is the first hardware measurement of the watcher's no-fire rule**, since S10.6's version of it is a `--dry-run` argument. Pulling and replugging the same monitor at the forced mode produced **no restart of any kind**: `NRestarts=0`, the same rbp pid with its boot-time start stamp (so its fd and mapping were never disturbed), no `RESTART` line in `displaywatch.log`, no `/tmp/displaywatch.fires`. The geometry read `1280,800 2560 16` on both sides of the replug, so the watcher's change test had nothing to fire on — the operator's rule, on hardware. The driver state agrees: `crtc[102] pixelvalve-2 enable=1 active=1 mode "1280x800"` with a plane at `crtc-pos=1280x800+0+0`, i.e. the fbdev was **not** torn down and the CRTC was never re-set. **The finding is on the probe, not on the picture**: the re-probe *refused* the forced token — `vc4-drm gpu: [drm] User-defined mode not supported: "1280x800": 60 83496 1280 1344 1480 1680 800 801 804 828 0x20 0x6`, once, at uptime 1361.7 s — and the display survived because the CRTC was *already* running that mode, not because the mode was re-applied. That corrects [06](06-display.md#setting-the-mode): the token is a boot-time instruction, not a standing one. **Two things are still owed, and neither is a pass.** First, the panel's own confirmation: `fb0/blank` reads `4` (`FB_BLANK_POWERDOWN`) while the atomic state says the CRTC is live — the two disagree, no shell read or fb capture settles which state the panel is in, and `[drm]` logged no blanking event, so the picture is asserted from the driver state and wants one glance at the glass. Second, the different-monitor half, which is what actually decides `RB_DISPLAY_RESTART_ON_RECONNECT` | a same-mode replug that leaves a black screen ⇒ the fbdev was recreated at identical geometry, which is the one hole the watcher's "only on a change" rule has; that is what `RB_DISPLAY_RESTART_ON_RECONNECT` exists for, and this row decides it. **The forced token is no fallback there** — measured: on the replug the probe refused it, so a torn-down fb would *not* have been rescued by 1280×800 being asked for again. A fire on a same-mode replug ⇒ the geometry string changed by a byte; `display-watch.sh status` prints both sides |
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
   derived. A mouse is now attached (`Logitech G203 Prodigy`, `event7`) and
   discovered, so the one measurement left is a human click: S3.2's four corners
   decide whether any of `POINT_SWAP_XY`/`POINT_INVERT_X`/`POINT_INVERT_Y` is
   needed. See above.
5. **Every DDJ-FLX4 MIDI detail** — see [15 — DDJ-FLX4 MIDI](15-flx4-midi.md).
   None of it can block the port: the keyboard map plus the fixture tests keep
   everything else moving.
6. **Which of the 68 patches are load-bearing on the Pi.** Each one's
   justification carries over, but only `0x3C665C` (the audio `scanForDevices`
   patch) is provably necessary in advance, because the Pi 4 fails the same
   `board_is_rev` check. The rest are re-tested per S6 in
   [12 — troubleshooting](12-troubleshooting.md).
