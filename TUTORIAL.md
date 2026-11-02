# Tutorial — run `rb` on a Raspberry Pi 4B

End-to-end: from the extracted XDJ-RX3 assets to `rbp` running on the Pi.
Everything targets a **Pi 4B on Pi OS Lite 32-bit (the current unit uses Debian 13 trixie)** with an HDMI
monitor and a DDJ-FLX6.

> rbpi4b does not cover firmware acquisition, decryption or keys — start from
> the extracted tree described in [docs/04](docs/04-firmware-assets.md).

## 0. Prerequisites

* **Workstation (Linux, or WSL):** `arm-linux-gnueabi-gcc`,
  `libc6-dev-armel-cross`, `python3`, `tar`, plus the DirectFB build tools —
  which include the **`flux` IDL compiler**, a separate package because
  DirectFB fetched from git does not ship the source files it generates (see
  [tools/build-directfb](tools/build-directfb/README.md)). The shim build can
  be run in the repo's Docker image if the float-ABI toolchain is awkward to
  install — that image is
  [`tools/build-toolchain/Dockerfile`](tools/build-toolchain/Dockerfile), and
  building it needs nothing on the host but Docker (see
  [tools/README.md](tools/README.md#build-toolchain)); the DirectFB build needs
  a native compiler *and* a cross one.
* **Extracted assets:** an `XDJRX3-rootfs/`, the GUI assets and `rbp-audio`
  (stock `rbp` + the shared patches). See
  [docs/04](docs/04-firmware-assets.md).
* **The Pi:** Pi OS Lite 32-bit, booted, on the network, with root, **plus
  `gcc`, `make` and `python3`**. Pi OS Lite ships no compiler, and `install.sh`
  compiles `vncserve` *on the unit* — that compile is what gives you the VNC
  viewer and its unit. Without `make`/`cc` it warns and carries on, and the
  result is no VNC binary and no `rblive4-vnc` unit rather than a failed install.
  `python3` is what the boot screen and the configuration page run under, and
  that page's unit carries `ConditionPathExists=/usr/bin/python3`, so without it
  the page is simply absent. The diagnostic tools in step 6 are single-file C and
  want the compiler too.
  ```sh
  ssh pi@<host>
  sudo apt-get install -y gcc make python3    # if they are not already there
  ```
  Confirm the image before anything else. What matters is that the kernel can
  execute 32-bit ARM ELF — **not** that its name says `armv7l`, because current
  Pi OS 32-bit images ship a 64-bit `-v8` kernel:
  ```sh
  getconf LONG_BIT                     # 32   (userland)
  dpkg --print-architecture            # armhf
  dmesg | grep -c '32-bit EL0 Support' # 1    (the kernel can run it)
  ```
  `install.sh` prints the same two facts and then tests the chroot for real, so
  if this looks different from the above, read what it says rather than the
  `uname` line.
  Then follow [docs/13](docs/13-raspberrypi4.md) for the `cmdline.txt` recipe
  (it is what makes the HDMI mode, and therefore the display, deterministic) and
  reboot before continuing.

## 1. Build the patched DirectFB stack (host)

Follow [tools/build-directfb/README.md](tools/build-directfb/README.md). It
fetches DirectFB 1.4.16 (pinned by commit — there is no 1.4.16 tag upstream),
applies `directfb-full.diff` and installs into `work/dfb/lib/`.

## 2. Build the chroot (host)

[`scripts/build-chroot.sh`](scripts/build-chroot.sh) assembles the whole
`rbx3-run` tree (rootfs + gui + patched rbp + shims + DirectFB + `directfbrc` +
`rb.conf`) and tars it:

```sh
RX3=/path/to/extracted DFB="$PWD/work/dfb" scripts/build-chroot.sh
# -> work/rbpi4b-pi4.tgz
```

It builds the shims from [`scripts/shims/`](scripts/shims/) if needed, applies
the `getPcController()` no-op patch via
[`scripts/patch-rbp-nopc.py`](scripts/patch-rbp-nopc.py), the layer pixel format
via [`scripts/patch-rbp-depth.py`](scripts/patch-rbp-depth.py) at the depth
`rb.conf` asks for, and installs the DirectFB stack.

## 3. Deploy (Pi)

[`scripts/device/install.sh`](scripts/device/install.sh) is the only on-device
entry point. It untars to `/opt/rblive4`, puts `rb.conf` in place, proves the
chroot can execute a 32-bit binary, checks `/dev/fb0` and `/dev/snd/seq`, and
runs `fix-dev.sh`. It is idempotent — re-running upgrades in place.

```sh
scp work/rbpi4b-pi4.tgz pi@<host>:/tmp/
scp -r scripts/device    pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rbpi4b-pi4.tgz'
```

`RB_DEPLOY_ROOT` overrides the destination if you would rather not use
`/opt/rblive4`. Everything machine-specific — display mode, audio card, MIDI
match strings, services to stop — is in `/opt/rblive4/rb.conf`, and nothing else
should need editing.

## 4. (Optional) iterate on a shim

The tarball already contains built shims. To change one, build it and drop it at
the deploy root; the launcher installs it into the chroot on the next start, so
you never rebuild the tarball to test a shim:

```sh
cd scripts/shims
make knobshim.so RX3=/path/to/extracted/XDJRX3-rootfs
scp knobshim.so pi@<host>:/opt/rblive4/knobshim.so
```

The same applies to `fbshim.so`, `audioshim.so` and `rbp-audio`.

## 5. Launch

**The player is normally already up by the time you get here.** `install.sh`
installs *and enables* `rblive4.service` (`RB_AUTOSTART=1`, the shipped default),
so after the reboot that follows a deploy `rbp` starts on its own. This command
is then a **restart**, and the manual path only if you set `RB_AUTOSTART=0`
([docs/11](docs/11-runtime-launcher.md)):

```sh
ssh pi@<host> 'sudo sh /opt/rblive4/start-rb.sh'
```

That stops the desktop services that would fight for the display or the media
stick, prepares the chroot, starts `edb_streamd` then `rbp`, and starts the USB
watcher. The exact sequence is in
[docs/11](docs/11-runtime-launcher.md).

`fix-dev.sh` runs as part of it, and must be re-run after **every** reboot — the
device nodes it creates live in `/dev`, which is a tmpfs.

## 6. Verify

Work down [docs/13](docs/13-raspberrypi4.md)'s bring-up table rather than
jumping to the last row: each step exists so that a failure cannot be masked by
the next subsystem, and the display one in particular is the gate for everything
else.

Two of the rows below read tools that are not built for you — both are
single-file C, so build them on the Pi first, with the `gcc` from step 0:

```sh
gcc -O2 -static -o fbdump    tools/fbdump.c
gcc -O2 -static -o evdevdump tools/evdevdump.c
```

| Check | Expectation |
|---|---|
| `tools/fbdump` (after the `cmdline.txt` recipe + reboot) | **measured: 1280×800, 16 bpp RGB565, `line_length` 2560, `smem_len` 2048000 (one page)** — the panel takes a `video=` mode its EDID never advertised, so the fb matches `rbp`'s surface exactly and the verdict line reads `geometry MATCHES`. `110 x 60 mm` means the EDID is still unread, i.e. the mode is forced, not negotiated. If a sink does refuse the timings you get 1280×720 back instead — then read [docs/06](docs/06-display.md) before assuming the display path is broken |
| UI on the monitor | rekordbox UI, full-screen ([docs/06](docs/06-display.md)) |
| `tools/evdevdump --list`, then point at the screen | rb reacts; see the four-corner procedure in [docs/07](docs/07-touch.md) |
| USB playback on the FLX6, then load + play | Master on USB0/1 and headphones on2/3. Hardware MIX and MASTER CUE add master to the cue-only headphone stream ([docs/09](docs/09-audio.md)) |
| `aseqdump -l`, then PLAY / CUE / faders / jog | Messages arrive and the mapped deck responds; compare the [FLX6 inventory](docs/flx6-mapping-audit.md), which distinguishes confirmed controls from recent locally checked additions |
| load a track with `1` and press `space` | deck 1 plays; `↑`/`↓` scroll the list, `Esc`/`Backspace`/right-click leave a screen. The keyboard and mouse need no configuration — they run alongside the controller (`EVDEV_MAP=kbd`, the default) and need no MIDI, no controller and no `/dev/snd/seq` ([docs/08](docs/08-controls.md#the-keyboard-map-rb_evdev_mapkbd-or-rb_midi_mapkbd-alone)) |
| Insert a rekordbox USB stick | shows as **USB 1** with the label/track count ([docs/10](docs/10-usb.md)) |

## 7. Restart / restore

```sh
# clean restart (clears the DeviceSQL locks — important for the USB library)
sudo sh /opt/rblive4/start-rb.sh

# back to a normal desktop session
sudo systemctl start lightdm
```

Unlike the previous target there is no vendor OS to hand the hardware back to —
nothing is disabled at boot, and the services the launcher stops are started
again by the line above.

## Current controls and limitations

FLX6 is the default. Leave RB_MIDI_MAP, RB_AUDIO_DEV and MIDI match overrides
empty to use its registered identity, or explicitly select RB_MIDI_MAP=flx6.
Old local overrides naming FLX4/DDJFLX4 must be cleared when moving to FLX6.
FLX4 remains available only by selecting RB_MIDI_MAP=flx4.

See the [full layout](docs/flx6-mapping-audit.md) and
[operator shortcuts](scripts/shims/README.md). The keyboard/mouse fallback
remains enabled through RB_EVDEV_MAP=kbd independently of the controller.

After deploying the latest shim, verify SHIFT+CUE (pause at track zero),
SHIFT+PLAY (main cue and play), SHIFT+LOAD (Instant Doubles), channel3 FILTER
(global Color amount), held-input reconnect cleanup, and shutdown/countdown.
These latest additions have local build/host checks but no reported Pi result.

Right Merge presets1..4 perform USB1 eject, USB2 eject, Track Mark and master
record toggle. Recording requires writable USB2. Select physical preset1 and
SHIFT+press its knob to align the utility counter after a mismatch.

Hold unshifted LEFT Merge knob five seconds to request host shutdown -h now;
release early to cancel. The ring accelerates from1Hz to2Hz to5Hz during the
hold, then stays lit when the hold completes. Let shutdown finish before
removing power. The latest shim must be deployed before relying on the shortcut.

Automatic preset discovery, disabling firmware Jog Cutter, four-deck audio,
and separate RX3 USB booth output are outside the current implementation.
Autostart remains enabled by RB_AUTOSTART=1; RB_AUTOSTART=0 leaves the installed
service disabled.
