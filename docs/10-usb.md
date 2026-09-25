# 10 — USB stick + rekordbox database

A rekordbox-exported stick in one of the Pi's USB ports is detected natively: it
mounts, `export.pdb` is opened and analysed by DeviceSQL, and the drive shows as
**USB 1** in rb.

The chain is fixed by the player, not by this port — the chroot path rbp looks
at, and the FIFO it listens on, are both compiled into the binary:

```
stick (any usb bus) → sda/sda1
  usb-watch.sh:  mount /dev/sda1 → $RB_MEDIA_MOUNT/sda1        (host side)
                 mount --bind   → $RB_CHROOT/media/usb1/sda1   (rbp's view)
                 FIFO /tmp/udev_usb1: "umount …" then "mount /media/usb1/sda1"
  rbp UsbMountManager → DbProxy → DbIF::mount(type=3)
  DeviceSQL (edb_streamd) opens export.pdb + exportExt.pdb and analyses the
  library → USB 1 appears in Source / Browse
```

## Finding the device

The previous target hardcoded bus numbers (`"usb1 usb2 usb3"`) because its media
port was a known controller on a known SoC. A Pi has no such mapping — every
Type-A port hangs off one PCIe-attached xHCI controller plus the USB-C/OTG port
on another, and the bus numbers vary by model and firmware revision. So the test
is **structural**: a block device is media if its sysfs path passes through a
USB bus (`/sys/block/sd*` → resolve → must contain `/usbN`).

`removable` then decides *between* candidates rather than gating them. That is
deliberate: a USB SSD or 2.5″ enclosure commonly reports `removable=0` while
being perfectly good rekordbox media, and requiring the flag would refuse the
one device the operator actually plugged in. If no candidate reports
`removable=1`, the first one found is used.

`usb-watch.sh status` prints both the chosen device and the full candidate list,
which is the thing to read when a stick seems invisible.

## The mount

Format comes from `blkid`; `vfat` is the fallback assumption. The vfat option
string is the RX3's (`flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,
usefree`) plus `codepage=437,iocharset=iso8859-1,utf8`.

The `nls` charset modules are **built in** on the RX3 kernel but are modules on
Pi OS, and a missing one fails the mount with a bare `EINVAL` that says nothing
about charsets. `fix-dev.sh` modprobes them, and `mount_media()` retries once
without `codepage`/`iocharset` if the first attempt fails — losing long-name
handling beats losing the stick entirely. `exfat` and `hfsplus` have their own
option strings.

## Telling rbp

Two rules, both of them learned the hard way:

* **Only the mount event is sent.** Never write to `/tmp/udev_usbctn*`: a
  "connect" there makes rbp raise *"USB Error. Remove the device."* This mirrors
  the XDJ-RX3's own `12-usb-memory-auto-mount.rules`.
* **`umount` first, then `mount`.** The umount resets rbp's `PathDecider` state,
  so the following mount event is treated as a fresh attach rather than a
  no-op — which matters most on the re-notify path, where rbp has seen the path
  before.

`notify()` writes to the FIFO under a 3-second `timeout`, because the FIFO has
no reader when rbp is down and a blocking open would wedge the watcher.

The bind mount must exist **before** rbp looks at the stick's files, and rbp's
DeviceSQL channel takes several seconds to come up after a (re)start — a mount
event sent before that is silently lost. So the watcher does not fire once: it
re-notifies every ~8 s, up to 12 times, until `/proc/<rbp>/fd` shows
`export.pdb` open, which is the proof that analysis has actually begun. The
watcher also tracks the rbp pid, so a restart re-notifies without an unplug.

## Files and paths

| Thing | Where |
|---|---|
| watcher | `$RB_DEPLOY_ROOT/usb-watch.sh` |
| host mount point | `$RB_MEDIA_MOUNT/sda1` (default `$RB_DEPLOY_ROOT/media/usb1/sda1`) |
| rbp's view | `$RB_CHROOT` + `$RB_CHROOT_MEDIA` = `/opt/rblive4/rbx3-run/media/usb1/sda1` |
| FIFO | `/tmp/udev_usb1` (created by `fix-dev.sh`) |
| log | `$RB_LOG_DIR/usbwatch.log` |

`RB_CHROOT_MEDIA=/media/usb1/sda1` is **not really configurable** — it is baked
into the patched binary (patch 1 of
[`tools/patch-rbp/PATCHES.md`](../tools/patch-rbp/PATCHES.md)). The *host-side*
mount point is; on the previous target the two were the same string, and here
they are deliberately different, because the FIFO messages are read by rbp
inside the chroot while the `mount` commands act on the host.

`RB_USB_TIMEOUT` points at `/usr/bin/timeout` (Pi OS has coreutils). The
`timeout.c` kept in `scripts/device/` is for targets that do not, and is not
used here.

## Run

```sh
sh /opt/rblive4/usb-watch.sh start    # one-shot; start-rb.sh does this automatically
sh /opt/rblive4/usb-watch.sh status   # device, candidates, both mounts, rbp pid, log tail
sh /opt/rblive4/usb-watch.sh stop
```

`status` is read-only and safe to run unprivileged; the other three require
root.

## Notes

* **`udisks2` must be masked.** `RB_MASK_SERVICES="udisks2 udisks2.service"` in
  `rb.conf` — the desktop storage stack will otherwise claim a storage device it
  did not mount and unmount it ~30 s later. On the previous target this was
  Engine OS's `edisksd.service`, doing exactly the same thing.
* On detach, rbp is told `umount` and then both mounts are released lazily
  (`umount -l`), so nothing blocks while rbp still holds files open. The mount
  directories are removed afterwards, if empty.
* A stick with no rekordbox export (no `PIONEER/rekordbox/export.pdb`) still
  mounts and still shows as a drive, but with no tracks — folder browsing needs
  the native DB import, which only runs on an export database.
