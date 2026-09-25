# Tutorial — run `rb` on a Raspberry Pi 4B

End-to-end: from the extracted XDJ-RX3 assets to `rbp` running on the Pi.
Everything targets a **Pi 4B on Pi OS Lite 32-bit (Bookworm)** with an HDMI
monitor and a DDJ-FLX4.

> rblive4 does not cover firmware acquisition, decryption or keys — start from
> the extracted tree described in [docs/04](docs/04-firmware-assets.md).

## 0. Prerequisites

* **Workstation (Linux, or WSL):** `arm-linux-gnueabi-gcc`,
  `libc6-dev-armel-cross`, `python3`, `tar`, plus the DirectFB build tools (see
  [tools/build-directfb](tools/build-directfb/README.md)). The shim build can
  be run in the repo's Docker image if the float-ABI toolchain is awkward to
  install; the DirectFB build needs a native or emulated ARM toolchain.
* **Extracted assets:** an `XDJRX3-rootfs/`, the GUI assets and `rbp-audio`
  (stock `rbp` + the shared patches). See
  [docs/04](docs/04-firmware-assets.md).
* **The Pi:** Pi OS Lite 32-bit, booted, on the network, with root.
  ```sh
  ssh pi@<host>
  ```
  Confirm the image before anything else — a 64-bit install breaks every shim:
  ```sh
  uname -m                     # armv7l
  dpkg --print-architecture    # armhf
  ```
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
# -> work/rblive4-pi4.tgz
```

It builds the shims from [`scripts/shims/`](scripts/shims/) if needed, applies
the `getPcController()` no-op patch via
[`scripts/patch-rbp-nopc.py`](scripts/patch-rbp-nopc.py), and installs the
DirectFB stack.

## 3. Deploy (Pi)

[`scripts/device/install.sh`](scripts/device/install.sh) is the only on-device
entry point. It untars to `/opt/rblive4`, puts `rb.conf` in place, proves the
chroot can execute a 32-bit binary, checks `/dev/fb0` and `/dev/snd/seq`, and
runs `fix-dev.sh`. It is idempotent — re-running upgrades in place.

```sh
scp work/rblive4-pi4.tgz pi@<host>:/tmp/
scp -r scripts/device    pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rblive4-pi4.tgz'
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

| Check | Expectation |
|---|---|
| `tools/fbdump` (after the `cmdline.txt` recipe + reboot) | the geometry you asked for, or the monitor's native mode — which is a letterbox trigger, not a failure |
| UI on the monitor | rekordbox UI, full-screen ([docs/06](docs/06-display.md)) |
| `tools/evdevdump --list`, then point at the screen | rb reacts; see the four-corner procedure in [docs/07](docs/07-touch.md) |
| `speaker-test` on the FLX4, then load + play | master out on the RCA, cue on the headphone jack ([docs/09](docs/09-audio.md)) |
| `aseqdump -l`, then PLAY / CUE / faders / jog | messages arrive, and the deck responds — the map is written and fixture-tested, but its note/CC tables are unverified until a dump confirms them ([docs/15](docs/15-flx4-midi.md)) |
| `RB_MIDI_MAP=kbd` in `rb.conf` (or the environment), restart, then load a track with `1` and press `space` | deck 1 plays; `↑`/`↓` scroll the list, `Esc`/`Backspace`/right-click leave a screen. This is the keyboard fallback: no controller, no MIDI, no `/dev/snd/seq` needed ([docs/08](docs/08-controls.md#the-keyboard-map-rb_midi_mapkbd)) |
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

## Current limitations

* The FLX4 map is written and fixture-tested, but **nothing has been run with an
  FLX4 attached**, so its note/CC tables come from Pioneer's published MIDI list
  rather than from the unit. Expect to correct some of them, starting with the
  channel conversion and the pad encoding; the dump procedure and the open
  questions are in [docs/15](docs/15-flx4-midi.md). The first sign of a wrong
  table is silence from a whole section rather than a wrong button — run with
  `RB_KNOB_VERBOSE=1` and read the unmapped lines in `/tmp/knobshim.log`.
* The way to drive `rbp` without one is the **keyboard fallback**,
  `RB_MIDI_MAP=kbd` ([docs/08](docs/08-controls.md#the-keyboard-map-rb_midi_mapkbd)):
  keyboard and mouse, no MIDI and no controller. It is written, cross-compiled
  and fixture-tested, but it has **never been run with real input devices** on
  the Pi, and the direction its selector turns for `↑`/`↓` is the one thing
  about it that only hardware can settle.
* Booth output is dropped: the FLX4 has two output pairs ([docs/09](docs/09-audio.md)).
* DJ FX parameter/layer encoders, TrackSkip, BeatJump and some SHIFT-actions are
  not mapped ([docs/08](docs/08-controls.md)).
* No autostart unit; launch `start-rb.sh` manually ([docs/11](docs/11-runtime-launcher.md)).
