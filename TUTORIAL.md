# Tutorial — build, deploy and run `rb` on a Raspberry Pi 4B

This is the complete deployment path: the baseline XDJ-RX3 player, the custom
touch menus, the optional persistent full touch UI, and the pinned RX3 Toolkit
integration (including Samples and prepared Stems).

The target is a Raspberry Pi 4B running Pi OS Lite 32-bit (the current unit uses
Debian 13 trixie), an HDMI display and a DDJ-FLX6.

> rbpi4b does not cover firmware acquisition, decryption or keys. Start from
> the extracted tree described in [docs/04](docs/04-firmware-assets.md). This
> repository and its generated Toolkit bundle contain no firmware player.

## What gets built

There are two outputs:

1. `work/rbpi4b-pi4.tgz` — the main chroot, patched player, DirectFB stack,
   shims and configuration. The separately transferred device directory
   supplies the installer and services. `fbshim.so` includes the swipe-down
   menus, side drawers, settings panel and optional
   [full touch UI](docs/23-full-touch-ui.md).
2. `work/rx3-toolkit-pi/` — the optional pinned
   [RX3 Toolkit](docs/21-rx3-toolkit.md) runtime, Pi adapters, assets, manifest,
   adapted source and license notices. Its modules all default off.

Deploy the main tarball first and the Toolkit bundle second. The full touch UI
is part of the main tarball; enabling it does not require the Toolkit bundle.

## 0. Prerequisites

- **Workstation (Linux or WSL):** `arm-linux-gnueabi-gcc`,
  `libc6-dev-armel-cross`, `python3`, `tar`, a native compiler and the DirectFB
  build tools, including the separate `flux` IDL compiler. See
  [tools/build-directfb](tools/build-directfb/README.md). The shim toolchain can
  alternatively run in the repository's Docker image; see
  [tools/README.md](tools/README.md#build-toolchain).
- **Extracted assets:** `XDJRX3-rootfs/`, the GUI assets and `rbp-audio` (stock
  `rbp` plus the shared interoperability patches). See
  [docs/04](docs/04-firmware-assets.md).
- **Pi:** Pi OS Lite 32-bit, network access and `gcc`, `make`, and `python3`.
  `install.sh` compiles `vncserve` on the Pi; Python runs the boot screen,
  configuration page and Toolkit helper.

On the Pi:

```sh
sudo apt-get install -y gcc make python3
getconf LONG_BIT                     # expect 32
dpkg --print-architecture            # expect armhf
dmesg | grep -c '32-bit EL0 Support' # expect 1
```

The kernel may identify as `aarch64`; what matters is its ability to execute
32-bit ARM ELF. Apply the deterministic HDMI `cmdline.txt` recipe in
[docs/13](docs/13-raspberrypi4.md) and reboot before deploying.

## 1. Build patched DirectFB (workstation)

Follow [tools/build-directfb/README.md](tools/build-directfb/README.md). It
fetches the pinned DirectFB 1.4.16 source, applies `directfb-full.diff`, and
installs the result under `work/dfb/`.

## 2. Build the main system (workstation)

From the repository root:

```sh
RX3=/path/to/extracted DFB="$PWD/work/dfb" scripts/build-chroot.sh
# -> work/rbpi4b-pi4.tgz
```

`build-chroot.sh` assembles the rootfs, GUI, patched player, current shims,
DirectFB, `directfbrc`, touch calibration and `rb.conf`. It applies the
`getPcController()` and display-depth patches and checks the depth/config pair.

This is enough for the baseline player and all `fbshim.so` touch additions. The
persistent navigation/deck-switch UI is compiled in but defaults off.

## 3. Build the optional RX3 Toolkit (workstation)

On a fresh clone, obtain the pinned upstream source:

```sh
git clone https://github.com/Tratosca/rx3-toolkit.git work/rx3-toolkit
git -C work/rx3-toolkit checkout --detach cc17a1fde3016a44440948e50bde72f430da34d9
```

Then build the Pi bundle and the controller shim that bridges FLX6 pad,
pad-mode and SHIFT events into it:

```sh
python3 scripts/build-rx3-toolkit.py
make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs" knobshim.so
# -> work/rx3-toolkit-pi/
```

The builder requires firmware 1.20 assets and a clean checkout at the pinned
revision. It verifies the stock player hash, guarded patches, hook sites, both
display-depth baselines and ARM imports. Skip this step if you do not want any
Toolkit modules.

## 4. Deploy the main system (Pi)

```sh
scp work/rbpi4b-pi4.tgz pi@<host>:/tmp/
scp -r scripts/device pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rbpi4b-pi4.tgz'
```

`install.sh` is the only main on-device entry point. It upgrades
`/opt/rblive4` in place, installs `rb.conf`, checks 32-bit execution and required
devices, builds optional on-device helpers, and installs/enables the systemd
units. `RB_DEPLOY_ROOT` can override `/opt/rblive4`.

## 5. Deploy the Toolkit and configure integrations (Pi)

If you built the optional Toolkit:

```sh
scp -r work/rx3-toolkit-pi pi@<host>:/tmp/
scp scripts/device/start-rb.sh scripts/device/rx3-toolkit.py \
    scripts/shims/knobshim.so pi@<host>:/tmp/

ssh pi@<host> '
  sudo systemctl stop rblive4 &&
  sudo install -m 755 /tmp/start-rb.sh /tmp/rx3-toolkit.py \
    /tmp/knobshim.so /opt/rblive4/ &&
  sudo mkdir -p /opt/rblive4/rx3-toolkit &&
  sudo cp -a /tmp/rx3-toolkit-pi/. /opt/rblive4/rx3-toolkit/
'
```

Store local choices in `/opt/rblive4/rb.local.conf`. Do not replace the shipped
`rb.conf` merely to enable features:

```sh
sudo nano /opt/rblive4/rb.local.conf
```

Example configuration:

```sh
# Persistent SOURCE/BROWSE/TAG/PLAYLIST/SEARCH navigation plus VINYL/SLIP/MT.
RB_POINT_FULL_UI=1

# Enable only the Toolkit modules you want.
RB_TOOLKIT_BEATJUMP32=1
RB_TOOLKIT_IMMEDIATE=1
RB_TOOLKIT_KEYSHIFT=1
RB_TOOLKIT_KEY_SYNC=1
RB_TOOLKIT_KEY_MATCH=1
RB_TOOLKIT_SAMPLES=1
RB_TOOLKIT_BROWSE_COLUMNS=1
RB_TOOLKIT_SEARCH_LATIN=1
RB_TOOLKIT_THEME=switch

# Prepared .rx3stem packages under RX3_STEMS/ on each track's export USB.
RB_TOOLKIT_STEMS=1
RB_TOOLKIT_STEMS_DIR=auto
```

Every Toolkit flag and `RB_POINT_FULL_UI` default off. Key Sync enables its Key
Shift/Match dependencies. Samples requires a prepared bank; see
[docs/21](docs/21-rx3-toolkit.md#prepare-samples-and-a-logo). Stems requires
prepared packages; see [docs/22](docs/22-stems.md). Use the pinned Toolkit's
desktop stem maker or the repository importer, but do not run the upstream
stock-RX3 `autoexec.bin` installer for this Pi deployment.

## 6. Start and inspect

```sh
ssh pi@<host> '
  sudo systemctl start rblive4 &&
  sudo tail -n 80 /tmp/rx3-toolkit.log &&
  sudo journalctl -u rblive4 -n 40 --no-pager &&
  sudo tail -n 80 /tmp/pointsrc.log
'
```

The Toolkit log should list `features=...` for the selected modules. The full
touch UI should log `full ui: primary Flip hooked` followed by its 16- or 32-bit
presentation line. A selected Toolkit module that fails installation refuses
the Toolkit-ready marker instead of silently changing the baseline player.

Without the Toolkit, `install.sh` normally has the baseline player running
already because `RB_AUTOSTART=1` is the shipped default. The clean manual
restart path is always:

```sh
sudo sh /opt/rblive4/start-rb.sh
```

The launcher prepares the chroot, stops conflicting desktop services, runs
`fix-dev.sh`, starts `edb_streamd` and `rbp`, and starts the USB watcher. See
[docs/11](docs/11-runtime-launcher.md).

## 7. Verify in layers

Follow the bring-up table in [docs/13](docs/13-raspberrypi4.md). Build the two
single-file diagnostics on the Pi before using their rows:

```sh
gcc -O2 -static -o fbdump tools/fbdump.c
gcc -O2 -static -o evdevdump tools/evdevdump.c
```

| Check | Expected result |
| --- | --- |
| `fbdump` | Measured target: 1280×800 RGB565, 16 bpp, line length 2560, one 2,048,000-byte page, `geometry MATCHES` |
| Native display/input | Full-screen UI; pointer reaches all four corners; USB and keyboard navigation work |
| Full touch UI | Persistent navigation, gear tap/hold, SEARCH dropdown dismissal and both decks' VINYL/SLIP/MT state; existing swipe panels still work |
| FLX6 audio | Master on USB0/1 and cue on2/3; hardware MIX and MASTER CUE perform the headphone blend |
| FLX6 controls | PLAY/CUE, faders, jog, pads, LEDs and utilities agree with the [mapping inventory](docs/flx6-mapping-audit.md) |
| Rekordbox USB | SOURCE shows the device label and track count; load and play a track |
| Toolkit | `/tmp/rx3-toolkit.log` names every selected module and publishes the current-PID ready marker; exercise enabled panels on both decks |
| Samples | Open SAMPLES, trigger pads1–8, test once/hold/loop/latch and return to native pad routing |
| Stems | A prepared track logs `stems package: package verified` and `stems ready`; test touchscreen roles and primary Slip Loop pads5/6/7 |

The expanded Toolkit deployment is user-confirmed. Stems and the full touch UI
have build/host checks but still need the complete Pi acceptance passes listed
in [docs/22](docs/22-stems.md) and [docs/23](docs/23-full-touch-ui.md).

## 8. Iterate without rebuilding everything

The main tarball already carries built shims. To test a shim change, copy it to
the deploy root; the launcher installs it into the chroot on the next start:

```sh
make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs" fbshim.so knobshim.so
scp scripts/shims/fbshim.so scripts/shims/knobshim.so pi@<host>:/tmp/
ssh pi@<host> '
  sudo systemctl stop rblive4 &&
  sudo install -m 755 /tmp/fbshim.so /tmp/knobshim.so /opt/rblive4/ &&
  sudo systemctl start rblive4
'
```

Deploy the matching `start-rb.sh` whenever a new configuration variable must be
forwarded. Rebuild/redeploy the Toolkit bundle when Toolkit runtime code or Pi
adapters change. Keep `start-rb.sh`, `rx3-toolkit.py`, `knobshim.so` and the
bundle from the same checkout.

## 9. Restart, disable or restore

```sh
# Clean player restart; also clears DeviceSQL locks.
sudo sh /opt/rblive4/start-rb.sh

# Return to a normal desktop session.
sudo systemctl stop rblive4
sudo systemctl start lightdm
```

Set `RB_POINT_FULL_UI=0` to disable persistent touch navigation while retaining
the original custom swipe UI. Disable individual `RB_TOOLKIT_*` switches to
remove their modules. To return to the baseline player, set every enabled
Toolkit feature to `0`, set `RB_TOOLKIT_THEME=off`, and restart; the verified
baseline player is never overwritten.

FLX6 remains the default controller. Leave MIDI/audio identity overrides empty
to use its registered identity, or explicitly set `RB_MIDI_MAP=flx6`. FLX4 is
available only as the compatibility selection `RB_MIDI_MAP=flx4`.

Hold the unshifted left Merge knob for five seconds to request a graceful host
shutdown; release early to cancel and wait for shutdown to finish before
removing power. Right Merge presets 1–4 provide USB1 eject, USB2 eject, Track
Mark and master record toggle; recording requires writable USB2. See
[scripts/shims/README.md](scripts/shims/README.md) for the complete operator
shortcuts and safety notes.
