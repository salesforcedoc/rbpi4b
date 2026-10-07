# 10 — USB stick + rekordbox database

A rekordbox-exported stick in one of the Pi's USB ports is detected natively: it
mounts, `export.pdb` is opened and analysed by DeviceSQL, and the drive shows as
**USB 1** in rb. A second stick appears as **USB 2** — rbp has two media devices
and this port feeds both.

The chain is fixed by the player, not by this port — the chroot path rbp looks
at, and the FIFO it listens on, are both compiled into the binary:

```
stick A (first in /sys/block order)      stick B (the second one)
  usb-watch.sh:  mount /dev/sdX1 → $RB_MEDIA_MOUNT/sda1
                 mount --bind   → $RB_CHROOT/media/usb1/sda1
                 FIFO /tmp/udev_usb1: "umount …" then "mount /media/usb1/sda1"
  usb-watch.sh:  mount /dev/sdY1 → $RB_MEDIA_MOUNT2/sda1
                 mount --bind   → $RB_CHROOT/media/usb2/sda1
                 FIFO /tmp/udev_usb2: "umount …" then "mount /media/usb2/sda1"
  rbp UsbMountManager (one per channel) → DbProxy → DbIF::mount(type=3)
  DeviceSQL (edb_streamd) opens each stick's export.pdb + exportExt.pdb and
  analyses it → USB 1 and USB 2 appear in Source / Browse
```

Which stick is which is **by position**, not by preference: the candidates in
`/sys/block` order, and the first one carrying a rekordbox export takes slot 1.
That is the operator's rule ("the first stick as USB1 and the second as USB2",
2026-10-07). A device keeps its slot while it is present, so pulling the first
stick leaves the second on USB 2 rather than renumbering it — renumbering would
detach and re-attach the same stick and make rbp re-import the library for
nothing. To choose which stick is USB 1, plug that one in first.

Slot 1's chroot path is baked into the patched binary (patch 1 of
[`tools/patch-rbp/PATCHES.md`](../tools/patch-rbp/PATCHES.md)). Slot 2's is not:
`ui::UsbMountManager::getMountPath(int)` returns the string the FIFO line
carried, which is why `/media/usb2/sda1` works without a patch — measured
2026-10-07.

## Finding the device

The previous target hardcoded bus numbers (`"usb1 usb2 usb3"`) because its media
port was a known controller on a known SoC. A Pi has no such mapping — every
Type-A port hangs off one PCIe-attached xHCI controller plus the USB-C/OTG port
on another, and the bus numbers vary by model and firmware revision. So the test
is **structural**: a block device is media if its sysfs path passes through a
USB bus (`/sys/block/sd*` → resolve → must contain `/usbN`).

A candidate then earns a slot only if it carries `PIONEER/rekordbox/export.pdb`,
which the watcher establishes by mounting it **read-only** (`probe_export()`).
That replaced the `removable` bit as the selection rule, and it fixes what the
bit got wrong here: the operator's library is an SSD enclosure that reports
`removable=0`, so any cheap thumb drive out-ranked it. The damage was not only a
wrong pick — a stick with no export mounted into the slot made the re-notify loop
below run *forever*, because rbp never opens a database that is not there, and
the source then disappeared from the UI entirely. "A second stick" read as "USB 1
vanished" (measured 2026-10-06). An export-less device now takes no slot at all.

Because `probe_export()` mounts, its verdict is memoised per device and re-asked
about once a minute, so a device that was still enumerating is not written off
for the life of the plug.

`usb-watch.sh status` prints the device serving each slot and the full candidate
list, which is the thing to read when a stick seems invisible.

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
re-notifies every ~8 s, up to 12 times, until `/proc/<rbp>/fd` shows **that
slot's** `export.pdb` open, which is the proof that analysis has actually begun.
The witness has to name the slot: rbp holds both slots' databases open at once,
so a bare `export.pdb` match would let a live USB 1 make USB 2 look ready. From
outside the chroot those fd targets read as `$RB_CHROOT` + the slot's chroot
path — measured 2026-10-07. The watcher also tracks the rbp pid, so a restart
re-notifies every live slot without an unplug.

## The device name

rbp's SOURCE screen names each device itself, with the literals `USB1` and
`USB2`, and there is no setting that changes that. Since 2026-10-07 it shows the
stick's own volume label instead.

The label comes from the host, in two pieces:

* `usb-watch.sh` reads it with `blkid -s LABEL -o value /dev/sdX1` at attach time
  and writes it to `/tmp/udev_usbN.label`, beside that slot's FIFO; `detach()`
  removes it again.
* the controls shim's USB thread reads that file and puts it in rbp's own
  device-name field, which is the 64-byte UTF-16 name at the head of the device's
  property record (`CmnFunc_CmnInfo_GetMountInfo_DevicePropertyInfo`, called by
  `ConvertBrowseUi2Gui`). rbp copies the whole field into the row when its first
  UTF-16 unit is non-zero, and falls back to `"USB1"`/`"USB2"` when it is zero —
  which is every stock run, since rbp never fills that field on this build.

Four things about it are deliberate:

* **`blkid` and not a parser of our own.** The volume label's offset in the boot
  sector differs between FAT12/16 (`0x2b`), FAT32 (`0x47`) and exFAT (`0x53`), and
  both of the operator's sticks are FAT32 — where reading `0x2b` returns GPT-like
  garbage. `blkid` is what `lsblk -f`'s LABEL column and udev read, and this port
  has no reason to disagree with them.
* **The write is re-asserted, not one-shot.** rbp repopulates and clears these
  records on mount events (nine `Clr` callers), so the shim compares the field
  every 100 ms and rewrites it only when it differs.
* **A field the shim has never written is left completely alone** — not even
  cleared — so a unit still running an older `usb-watch.sh`, which writes no
  label files, gets byte-for-byte the shim it had before.
* **No label means rbp's own name.** A stick with no volume label, an empty file
  and an absent file are all the same answer: leave `USB1`/`USB2` alone. Bytes
  above printable ASCII become `?` rather than being decoded — udev's LABEL is
  UTF-8 and rbp's baked label font is a Latin atlas, so a decoded name would draw
  as boxes.

The small caption under the drive icon, at the left of each row, is a **different**
string, still rbp's own slot number. Only the DEVICE NAME column carries the label.

**The USB STOP chooser's buttons carry it too.** Since 2026-10-07 the box the band's
seventh column raises reads `HOLD PHASIM_USB2 | HOLD RBOX USB` rather than `HOLD USB 1 |
HOLD USB 2`. Same file, one hop further: `fbshim.so` reads `/tmp/udev_usbN.label` itself
(`pointsrc.c`'s `usb_name_read()`, in the shim that already owns `real_open`),
`prompt_zone.c` composes `"HOLD " + <label>` and falls back to the shipped `HOLD USB n`
on an absent, empty or whitespace-only file, and `prompt_paint.c` clips the result to the
cell. **The names travel in the state**, so the drawn module still opens nothing of its
own, and the two guards do not share a path: `pointsrc_usb_state()` fills the names
**before** it tries to walk rbp, so a caller that gets `0` still gets buttons that name
the devices they are refusing.

**Measured on the unit 2026-10-07**, both sticks in, through the production composition
code: `HOLD PHASIM_USB2` is **166 px** and `HOLD RBOX USB` **135 px** in a 265 px cell, so
neither is clipped and `prompt_paint_ok()` still answers 1. A name **longer** than the cell
is cut rather than allowed to blank the box: the width refusal measures the cell's
**default** label, which is the label every cell is guaranteed to have, so a unit whose
`usb-watch.sh` writes no label files draws byte-for-byte the picture it drew before.
Characters the atlas cannot draw become `?` for the same reason they do in the SOURCE
row — a gap in the middle of a button reads as a dead panel rather than as a missing
glyph.

## Files and paths

| Thing | Where |
|---|---|
| watcher | `$RB_DEPLOY_ROOT/usb-watch.sh` |
| host mount points | `$RB_MEDIA_MOUNT/sda1` (default `$RB_DEPLOY_ROOT/media/usb1/sda1`) and `$RB_MEDIA_MOUNT2/sda1` (default `…/media/usb2/sda1`) |
| rbp's view | `$RB_CHROOT` + `$RB_CHROOT_MEDIA` = `/opt/rblive4/rbx3-run/media/usb1/sda1`, and `$RB_CHROOT` + `$RB_CHROOT_MEDIA2` = `…/media/usb2/sda1` |
| FIFOs | `/tmp/udev_usb1` and `/tmp/udev_usb2` (created by `fix-dev.sh`) |
| device-name files | `/tmp/udev_usb1.label` and `/tmp/udev_usb2.label`, written by the watcher at attach and removed at detach |
| log | `$RB_LOG_DIR/usbwatch.log` |

`RB_CHROOT_MEDIA=/media/usb1/sda1` is **not really configurable** — it is baked
into the patched binary (patch 1 of
[`tools/patch-rbp/PATCHES.md`](../tools/patch-rbp/PATCHES.md)). The *host-side*
mount points are, and the two must differ. `RB_CHROOT_MEDIA2` is settable
because nothing bakes slot 2's path — it is whatever the FIFO line said — but it
is written in the same shape on purpose. On the previous target the host path and
the chroot path were the same string; here they are deliberately different,
because the FIFO messages are read by rbp inside the chroot while the `mount`
commands act on the host.

`RB_USB_TIMEOUT` points at `/usr/bin/timeout` (Pi OS has coreutils). The
`timeout.c` kept in `scripts/device/` is for targets that do not, and is not
used here.

## Run

```sh
sh /opt/rblive4/usb-watch.sh start    # one-shot; start-rb.sh does this automatically
sh /opt/rblive4/usb-watch.sh status   # each slot's device and label, candidates, both mounts, rbp pid, log tail
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
* A stick with no rekordbox export (no `PIONEER/rekordbox/export.pdb`) takes no
  slot at all: `probe_export()` refuses it, so it never displaces a device that
  is already mounted and playing. It is left alone entirely — not mounted, not
  announced.
* **Two slots, and rbp was built for both.** rbp holds a separate
  `ui::UsbStorageManager` **per channel** and answers for each one independently,
  so its own USB STOP screen has a USB 1 and a USB 2. Since 2026-10-07
  `usb-watch.sh` feeds both paths, so both rows are live when both sticks are in
  — measured as both managers reading `media=2` and rbp holding all four
  databases (`export.pdb` *and* `exportExt.pdb` for `/media/usb1/sda1` and
  `/media/usb2/sda1`) open at once. The shim's USB STOP chooser
  ([07](07-touch.md#the-seventh-column-raises-a-chooser-prompt_zonec-prompt_paintc))
  lights a row from rbp's own answer, so an empty slot draws dimmed and answers
  nothing while a populated one ejects **on a three-second hold** — the release
  before the third second does nothing at all. Note that the *channel* is what a
  stop is addressed on — the old `CH_GLOBAL` spelling is USB 1's number (1) by
  coincidence, not by design, which is why it could never have reached a second
  device.
* The RX3's own `12-usb-memory-auto-mount.rules` maps `1-1*`→usb1, `2-1.1*`→usb2
  and `2-1.2*`→usb1, i.e. the vendor keys the slot off the **port**, and writes
  the slot name into the FIFO text. The Pi has no such stable port naming, which
  is why the slot rule here is position among export-bearing candidates instead.
