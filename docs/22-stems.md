# 22 — Stems on the FLX6/Pi

Stems is an optional module in the pinned toolkit bundle. The preceding toolkit
features are user-confirmed working on the Pi; Stems audio, controls, LEDs and
waveforms still need Pi acceptance. The Pi plays **prepared** packages. Separation
runs on your computer, not live on the Pi.

## Controls

| Control | Function for a prepared track |
| --- | --- |
| STEMS touchscreen tab | Open per-deck stem controls. Tap a role to mute/restore it. |
| Hold a role350ms, then drag horizontally | Change that role's amount without toggling it on release. |
| Primary SLIP LOOP, pad5 | Instrumental, including bass. |
| Primary SLIP LOOP, pad6 | Vocals. |
| Primary SLIP LOOP, pad7 | Drums when the package contains them. |
| Primary SLIP LOOP, pad8 | Its native loop. |

The FLX6 map already assigns eight logical RX3 pad modes; use its **primary
Slip Loop** bank, not shifted Release FX. The mode-aware toolkit bridge delivers
that exact bank with each pad gesture. Other banks keep their existing functions.
Samples has priority while its panel owns the pads. Changing pad mode restores
native routing. Held/captured releases are delivered on bank changes and
controller disconnect too.

Prepared roles blink while loading and use native LED state when available;
the FLX6's fixed-color pads show their supported brightness/blink behavior.
There is no new physical stem button or reassignment of transport/FX controls.
A track without a matching valid package keeps normal audio and native pads.

New preparation stores VOCAL and DRUMS. INST is reconstructed from the original
mix minus those roles; it includes the bass. Vocal-only and older compatible
packages remain readable. A new track resets stem levels. Selection changes
use upstream's256-frame gain transition. Key Shift and the other toolkit modules
remain selectable alongside Stems.

## Build, deploy and enable

On your computer, in rbpi4b. Replace `user@pi-host` with your Pi's SSH username
and hostname or IP address:

```sh
python3 scripts/build-rx3-toolkit.py
make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs" knobshim.so
scp -r work/rx3-toolkit-pi user@pi-host:/tmp/
scp scripts/device/start-rb.sh scripts/device/rx3-toolkit.py scripts/shims/knobshim.so user@pi-host:/tmp/
```

On the Pi:

```sh
sudo systemctl stop rblive4
sudo install -m 755 /tmp/start-rb.sh /tmp/rx3-toolkit.py /tmp/knobshim.so /opt/rblive4/
sudo cp -a /tmp/rx3-toolkit-pi/. /opt/rblive4/rx3-toolkit/
sudo nano /opt/rblive4/rb.local.conf
```

Add or edit these settings, leaving your other toolkit settings as they are:

```sh
RB_TOOLKIT_STEMS=1
RB_TOOLKIT_STEMS_DIR=auto
```

Then:

```sh
sudo systemctl start rblive4
sudo tail -n 100 /tmp/rx3-toolkit.log
sudo journalctl -u rblive4 -n 50 --no-pager
```

The helper checks the bundle contains both `stems` and `stemwave`, and refuses
an older non-Stems bundle. Startup should list both modules active. The core
still requires the current-PID readiness marker. Set RB_TOOLKIT_STEMS=0 and
restart to remove both modules while keeping the others enabled. Stems defaults
to0; replacing the bundle alone does not turn it on.

## Where packages go

Put the prepared RX3_STEMS directory at the root of **each** rekordbox USB:

```text
USB drive/
  Contents/...
  PIONEER/rekordbox/export.pdb
  RX3_STEMS/
    Exported track filename.rx3stem
```

With auto, the Pi adapter uses the loaded track's own mount namespace:

```text
/media/usb1/sda1/Contents/Artist/Track.mp3
  -> /media/usb1/sda1/RX3_STEMS/Track.rx3stem
/media/usb2/sda1/Contents/Artist/Track.mp3
  -> /media/usb2/sda1/RX3_STEMS/Track.rx3stem
```

It never searches the other USB for a same-named track. It follows the mount
path from each new track load, including reconnects. The existing USB watcher
binds each slot into the chroot; no new watcher or player restart per insertion
is needed. Do not edit USB watcher mounts to point at the preparation computer.

For packages stored on the Pi instead, set RB_TOOLKIT_STEMS_DIR to an absolute
**chroot** directory, for example /root/pdj/RX3_STEMS. That deliberately gives
both decks one shared library. Changes to this setting require a restart;
new packages in the same directory are found on the next track load.

Package names must match the filename actually loaded from the export, without
its last extension. Rekordbox can shorten exported names to44characters; use the
upstream exporter or the exported audio filename rather than a browser title.
Same-named files on one USB share a package namespace, an upstream limitation;
use distinct exported names for distinct tracks. The Pi adapter supports UTF-8
basenames and refuses paths too long for its bounded output buffer.

## Automatic separation on your computer

Use the **pinned checkout's desktop Stems screen**, so its package format matches
the runtime. The app can prepare stems without building/installing autoexec.bin.
There is no firmware key requirement for separation itself.

If you do not already have its app environment, from rbpi4b:

```sh
cd work/rx3-toolkit
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt pywebview==6.2.1
make app PYTHON="$PWD/.venv/bin/python"
```

On Linux, pywebview also needs its system WebKit/GTK backend. Follow the pinned
checkout's CONTRIBUTING.md and pywebview setup for your desktop distribution.
This setup is for the preparation computer, not the Pi's firmware chroot.

In the app, choose the rekordbox export/drive, open Stems, install its separation
tools when prompted, select tracks and prepare VOCAL+DRUMS. This downloads its
model/runtime separately and can take time. The app writes RX3_STEMS packages;
verify the preview and put that folder on the matching export USB. Skip the
stock RX3 “Install the mod”/autoexec.bin flow; this port uses its own bundle.

The large separator runtime and models are not downloaded by our bundle build.
The ignored source checkout and user audio are not committed to rbpi4b.

## Import stems prepared elsewhere

A small workstation wrapper uses the pinned upstream importer's alignment,
source, duration, gain and package checks. Supply lossless stems from the same
original, without normalization, clipping, effects or timeline edits. Use the
original audio from the rekordbox export as --track.

FFmpeg and FFprobe must be available on that computer. From rbpi4b:

```sh
python3 scripts/prepare-rx3-stems.py import \
    --track '/path/to/export/Contents/Track.wav' \
    --vocals '/path/to/separated/vocals.wav' \
    --drums '/path/to/separated/drums.wav'
```

The default output is ignored work/rx3-stems-export/RX3_STEMS/. Copy that folder
onto the export USB. --output changes the export root to write; no USB is
modified by default. Omit --drums for vocal-only preparation. --no-waveforms
creates an audio-only package that keeps the player's native waveform.
This wrapper imports already separated audio; it does not run a separation model.

Verify a package and show its internal manifest:

```sh
python3 scripts/prepare-rx3-stems.py inspect \
    'work/rx3-stems-export/RX3_STEMS/Track.rx3stem'
```

The wrapper checks the source revision and reuses upstream code rather than
inventing a new package format. The native loader also validates the complete
package, CRCs, role lengths and memory budget before publishing it to audio.
Unavailable/corrupt packages leave the native track playing; a selected runtime
module that fails installation still refuses startup readiness.

## First Pi check

Start with one short prepared track. Load it and wait for `stems package: package
verified` and `stems ready` in /tmp/rx3-toolkit.log. Check INST, VOCAL and DRUMS
from the touchscreen, then pads5/6/7 in primary Slip Loop; pad8 and other modes
should remain native. Load a track without a package and check normal behavior.

Then check both decks together, Samples priority, tempo/master-tempo, Key Shift,
cue/headphones, seeking/loops, track changes, USB1/USB2 and reconnect. Embedded
waveforms follow selection when supported; audio-only packages keep the native
waveform. Upstream limits are512MiB resident across both decks and300MiB reserved
for the player; a two-role package can exceed the budget for very long tracks.
The upstream preparation tools check those limits before publishing.

Host/package/native-DSP tests verify the integration and data paths. They cannot
establish audible cancellation, timing or CPU performance on the Pi; those remain
the hardware acceptance checks for this addition.
