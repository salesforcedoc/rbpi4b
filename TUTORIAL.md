# Tutorial — run `rb` on a Denon SC Live 4

End-to-end: from the extracted XDJ-RX3 assets to `rbp` running on the device.
Everything targets the SC Live 4 (`JP21`, "SCX-4"), Engine OS 4.3.x.

> rblive4 does not cover firmware acquisition, decryption or keys — start from
> the extracted tree described in [docs/04](docs/04-firmware-assets.md).

## 0. Prerequisites

* **Host (WSL/Linux):** `arm-linux-gnueabi-gcc`, `libc6-dev-armel-cross`,
  `python3`, `tar`, plus the DirectFB build tools (see
  [tools/build-directfb](tools/build-directfb/README.md)).
* **Extracted assets:** an `XDJRX3-rootfs/`, the GUI assets and `rbp-audio`
  (stock `rbp` + the shared patches). See
  [docs/04](docs/04-firmware-assets.md).
* **Device:** SC Live 4 with root SSH. In the examples `<device>` is your
  device's hostname or IP.
  ```
  ssh root@<device>
  ```

## 1. Build the patched DirectFB stack (host)

Follow [tools/build-directfb/README.md](tools/build-directfb/README.md). It
fetches DirectFB 1.4.16, applies `directfb-full.diff` and installs into
`work/dfb/lib/`.

## 2. Build the chroot (host)

[`scripts/build-chroot.sh`](scripts/build-chroot.sh) assembles the whole
`/data/rbx3-run` tree (rootfs + gui + patched rbp + shims + DirectFB +
`directfbrc` + touch calibration) and tars it:

```sh
RX3=/path/to/extracted DFB="$PWD/work/dfb" scripts/build-chroot.sh
# -> work/rbx3-run.tgz  (~33 MB)
```

It builds the shims from [`scripts/shims/`](scripts/shims/) if needed, applies
the SC Live 4 `getPcController` patch via
[`scripts/patch-rbp-sclive4.py`](scripts/patch-rbp-sclive4.py), and installs the
DirectFB stack.

## 3. Deploy (device)

The device busybox `tar` has no `-z`, so use `zcat`:

```sh
scp work/rbx3-run.tgz root@<device>:/data/
ssh root@<device> '
  rm -rf /data/rbx3-run; mkdir -p /data/rbx3-run
  zcat /data/rbx3-run.tgz | tar x -C /data/rbx3-run
  chown -R root:root /data/rbx3-run
  sh /data/fix-dev.sh'
```

Also ship the on-device helpers once:

```sh
make -C scripts/device CROSS=arm-linux-gnueabihf-   # builds ./timeout
scp scripts/device/fix-dev.sh scripts/device/start-rb.sh \
    scripts/device/usb-watch.sh scripts/device/timeout root@<device>:/data/
```

## 4. (Optional) rebuild a shim

The chroot tarball already contains built shims. To change one:

```sh
cd scripts/shims
make knobshim2.so RX3=/path/to/extracted/XDJRX3-rootfs
scp knobshim2.so root@<device>:/data/knobshim2.so
```

## 5. Launch

```sh
ssh root@<device> 'sh /data/start-rb.sh'
```

That stops Engine OS, prepares the chroot, starts `edb_streamd` then `rbp`, and
starts the USB watcher. The exact sequence is in
[docs/11](docs/11-runtime-launcher.md).

## 6. Verify

| Check | Expectation |
|---|---|
| UI on the panel | rekordbox UI, full-screen, upright |
| Tap the screen | rb reacts ([docs/07](docs/07-touch.md)) |
| PLAY / CUE / faders / jog | work ([docs/08](docs/08-controls.md)) |
| Insert a rekordbox USB stick | shows as **USB 1** with the label/track count ([docs/10](docs/10-usb.md)) |
| Load + PLAY a track | audio from the built-in speakers ([docs/09](docs/09-audio.md)) |

## 7. Restart / restore

```sh
# clean restart (clears DeviceSQL locks — important for the USB library)
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
sh /data/start-rb.sh

# back to stock Engine OS
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
systemctl start engine.service
```

## Current limitations

* Speaker volume knob control is not used — a fixed level is set with
  `SPEAKER_GAIN` ([docs/09](docs/09-audio.md)).
* DJ FX parameter/layer encoders, TrackSkip, BeatJump and some SHIFT-actions
  are not mapped ([docs/08](docs/08-controls.md)).
* No autostart unit; launch `start-rb.sh` manually ([docs/11](docs/11-runtime-launcher.md)).
