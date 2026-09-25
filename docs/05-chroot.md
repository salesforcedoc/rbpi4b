# 05 — Soft-float chroot on the Pi

`rbp` runs from `/opt/rblive4/rbx3-run`, a soft-float glibc-2.13 RX3 userland.
The Pi's kernel is `armv7l`/armhf (hard-float) and executes soft-float EABI ELF
natively, which is the whole reason this works — **a 64-bit image would break
every shim** in the port. An 8 GB card holds the tree with room to spare.

The chroot is **~150–250 MB** extracted — an estimate from the tree, not a
measurement, and not a typo for the ~60 MB the previous target's copy of this
document quoted: that figure counted only `rbp` + glibc + DirectFB, not the full
RX3 rootfs staged beside them. An 8 GB card is the recommended minimum and is
comfortable; the deploy is not the thing that fills it.

## Build (host, WSL/Linux)

[`scripts/build-chroot.sh`](../scripts/build-chroot.sh) assembles the whole
deploy root and tars it to `work/rblive4-pi4.tgz`:

1. **RX3 rootfs** (`extracted/XDJRX3-rootfs`) — soft-float glibc 2.13,
   libstdc++, DirectFB, freetype, ALSA, `edb_streamd`, busybox.
2. **GUI assets** → `rbx3-run/root/gui/{fontdata,imagedata,pset,system}` (rbp
   reads `/root/gui/pset/...` and `/root/gui/system/...`).
3. **Patched player** → `rbx3-run/root/pdj/rbp` (shared rbp patches + the
   `getPcController` fix — see
   [`scripts/patch-rbp-nopc.py`](../scripts/patch-rbp-nopc.py)). The patch is
   **not** Denon-specific: it turns a NULL `getPcController()` result into a
   no-op, and the Pi fails the same board check the previous target did.
4. **Shims** → `rbx3-run/usr/lib/{fbshim,knobshim,audioshim,crashcatch}.so`.
5. **DirectFB 1.4.16 stack** — core libs + the patched fbdev module +
   inputdrivers/wm, in `rbx3-run/usr/lib/directfb-1.4-6/`.
6. **`rbx3-run/usr/etc/directfbrc`** (`no-hardware`/`no-cursor`/`system=fbdev`/
   `fbdev=/dev/fb0`) and **`rbx3-run/root/settings/TouchCalib_{User,Factory}.dat`**.
7. **`rb.conf`** at the deploy root — the single place machine-specific values
   live, so the device needs one `scp`, not two.
8. Fixes exec bits and the `etc/mtab → /proc/mounts` symlink.

Asset locations come from `RX3` / `DFB` — defaulting to `extracted/` and
`work/dfb` under the repo root, whatever the current directory is. The output
goes to `$OUT` (default `work/`).

```sh
RX3=/path/to/extracted DFB=$PWD/work/dfb scripts/build-chroot.sh
# -> work/rblive4-pi4.tgz
```

## Deploy

Pi OS's `tar` is GNU tar, so the busybox `-z` problem the previous target had
does not apply here. [`install.sh`](../scripts/device/install.sh) is the
one-time (and idempotent) entry point — it untars the deploy root to
`/opt/rblive4`, puts the device scripts beside it, and runs `fix-dev.sh`:

```sh
scp work/rblive4-pi4.tgz scripts/device pi@<host>:/tmp/
ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rblive4-pi4.tgz'
```

`RB_DEPLOY_ROOT` overrides the destination. Re-running upgrades in place.

`fix-dev.sh` creates the bind mounts (`/dev /proc /sys /tmp`), the device
stubs (gpiodrv, subucom FIFOs, hidg0, printkdrv0, `chmod 000 /dev/mem`),
the `/tmp/udev_*` FIFOs and the `etc/mtab` symlink, and ensures
`/dev/snd/seq` exists. It must run after every reboot — `/dev` is a tmpfs, and
the stubs inside the chroot are the *host's* nodes because `/dev` is bind-
mounted, so nothing survives a restart.

Because the chroot has no `/data` of its own, the paths differ from the
previous target at every level: deploy root `/opt/rblive4`, chroot
`/opt/rblive4/rbx3-run`, logs `/opt/rblive4/log`, host-side media mount
`/opt/rblive4/media/usb1/sda1`. The one path that did **not** change is the
chroot-internal media mount `/media/usb1/sda1` — it is baked into the patched
binary.

## Notes

* the build script restores exec bits on `/bin`, `/sbin`, `/usr/bin`,
  `/usr/sbin` and the loader.
* `var/log/wtmp` may have an ACL that denies even root on a Windows/WSL mount;
  the script uses `tar --ignore-failed-read` and recreates it empty.
* Symlinks survive (git-bash/WSL store real reparse points).
* Verify with `chroot /opt/rblive4/rbx3-run /bin/sh -c 'echo ok'`.
