# scripts/device/

On-device scripts (ship to `/data/` on the SC Live 4).

| Script | Purpose |
|---|---|
| `fix-dev.sh` | bind mounts (`/dev /proc /sys /tmp`), device stubs, `/tmp/udev_*` FIFOs, `etc/mtab` symlink. Run after every reboot. |
| `start-rb.sh` | stop Engine OS + `edisksd`, prepare the chroot, start `edb_streamd` then `rbp`, start the USB watcher. |
| `usb-watch.sh` | hotplug the USB-A media port (`usb1`), mount + bind into the chroot, notify rbp via `/tmp/udev_usb1`; retries until rbp opens `export.pdb`. |

Also needed on the device:

* `/data/timeout` — build it from [`timeout.c`](timeout.c) with
  `make CROSS=arm-linux-gnueabihf-` in this directory; used by `usb-watch.sh`
  so a FIFO write to a dead rbp can never block the watcher.
* `/data/rbp-audio`, `/data/knobshim2.so`, `/data/audioshim.so`,
  `/data/fbshim-tsc.so`, `/data/libdirectfb_fbdev-rot16.so` — the runtime files
  `start-rb.sh` copies into the chroot (already baked into the chroot tarball).

## Notes

* The rbp launch sets `PATH=/bin:/sbin:/usr/bin:/usr/sbin` (the device default
  lacks `/bin`) and preloads `fbshim:knobshim:audioshim` (that order).
* Restarting? clear `/tmp/guard_LocalDBServer` + `/tmp/req_LocalDBServer` first
  (DeviceSQL locks) — see [docs/11](../../docs/11-runtime-launcher.md).
