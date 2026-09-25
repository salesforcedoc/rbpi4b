# 10 — USB stick + rekordbox database

A rekordbox-exported stick in the SC Live 4's rear USB-A port is detected
natively: it mounts, `export.pdb` is opened and analysed by DeviceSQL, and the
drive shows as **USB 1** in rb.

The flow uses `usb-watch.sh` with two SC Live 4 specifics:

1. **The media port is on `usb1` (DWC OTG)**, not `usb3/usb4` as on the Prime
   GO. A stick in the back enumerates as:
   ```
   /sys/block/sda -> ../platform/ff540000.usb/usb1/1-1/1-1:1.0/host0/.../block/sda
   ```
   The control surface is a MIDI UART (card 0), *not* USB, so `usb1` is free
   for media. `USBWATCH_BUSES="1 2 3"` (the shipped default) watches all ports.

2. **`PATH` must include `/bin`** when launching rbp. The SC Live 4's default
   `PATH` is `/usr/bin:/usr/sbin`, but the chroot's busybox tools (`ls`, …) live
   in `/bin`. Without it rbp logs `sh: ls: not found` and the mount-handler
   helpers fail. `start-rb.sh` sets
   `PATH=/bin:/sbin:/usr/bin:/usr/sbin`.

## Working chain

```
stick (usb1, 1-1) → sda/sda1
  usb-watch.sh:  mount /dev/sda1 → /media/usb1/sda1 (vfat)
                 mount --bind → /data/rbx3-run/media/usb1/sda1
                 FIFO /tmp/udev_usb1: "umount …" then "mount /media/usb1/sda1"
  rbp UsbMountManager → DbProxy → DbIF::mount(type=3)
  DeviceSQL (edb_streamd) opens export.pdb + exportExt.pdb and analyses the
  library → USB 1 appears in Source / Browse
```

## Files

* `scripts/device/usb-watch.sh` — hotplug watcher (SC Live 4 buses), shipped to
  `/data/usb-watch.sh`.
* `/data/timeout` — a small ARM `timeout(1)` helper required by
  `usb-watch.sh notify()`.
* `fix-dev.sh` creates the `/tmp/udev_*` FIFOs and the `/etc/mtab` symlink.

## Run

```sh
sh /data/usb-watch.sh start    # one-shot; start-rb.sh does this automatically
sh /data/usb-watch.sh status   # mounted? chroot bind? media sd?
sh /data/usb-watch.sh stop
```

## Notes

* **`edisksd.service`** must stay stopped — it will bus-reset and unmount a
  storage device it doesn't manage (~30 s later).
* The stick is re-notified automatically when rbp restarts (the watcher tracks
  the rbp pid).
* A stick without a rekordbox export (no `PIONEER/rekordbox/export.pdb`) still
  mounts, but shows empty — folder browsing needs the native DB import.
