# 05 — Soft-float chroot on the SC Live 4

`rbp` runs from `/data/rbx3-run`, a soft-float glibc-2.13 RX3 userland.
`/data` has ~5.6 GB free, so no trimming is needed.

## Build (host, WSL/Linux)

[`scripts/build-chroot.sh`](../scripts/build-chroot.sh) assembles the whole
tree and tars it to `work/rbx3-run.tgz`:

1. **RX3 rootfs** (`extracted/XDJRX3-rootfs`) — soft-float glibc 2.13,
   libstdc++, DirectFB, freetype, ALSA, `edb_streamd`, busybox.
2. **GUI assets** → `root/gui/{fontdata,imagedata,pset,system}` (rbp reads
   `/root/gui/pset/...` and `/root/gui/system/...`).
3. **Patched player** → `root/pdj/rbp` (shared rbp patches + the SC Live 4
   `getPcController` fix — see [`scripts/patch-rbp-sclive4.py`](../scripts/patch-rbp-sclive4.py)).
4. **Shims** → `usr/lib/{fbshim,knobshim,audioshim,crashcatch}.so`.
5. **DirectFB 1.4.16 stack** — core libs + the patched rot16 fbdev module +
   inputdrivers/wm, in `usr/lib/directfb-1.4-6/`.
6. **`usr/etc/directfbrc`** (`no-hardware`/`no-cursor`/`system=fbdev`/
   `fbdev=/dev/fb0`) and **`root/settings/TouchCalib_{User,Factory}.dat`**.
7. Fixes exec bits and the `etc/mtab → /proc/mounts` symlink.

Asset locations are taken from `RX3` / `CHROMEBIT` (defaults `$HOME/xdjrx3-findings`
and `$HOME/chromebit`); the output goes to `work/`.

```sh
RX3=/path/to/xdjrx3-findings scripts/build-chroot.sh   # -> work/rbx3-run.tgz
```

## Deploy

The device's busybox `tar` has **no `-z`**, so decompress with `zcat`:

```sh
scp work/rbx3-run.tgz root@sclive4:/data/
ssh root@sclive4 'rm -rf /data/rbx3-run; mkdir -p /data/rbx3-run
  zcat /data/rbx3-run.tgz | tar x -C /data/rbx3-run
  chown -R root:root /data/rbx3-run
  sh /data/fix-dev.sh'
```

`fix-dev.sh` creates the bind mounts (`/dev /proc /sys /tmp`), the device
stubs (gpiodrv, subucom FIFOs, hidg0, printkdrv0, `chmod 000 /dev/mem`),
the `/tmp/udev_*` FIFOs and the `etc/mtab` symlink. It must run after every
reboot.

## Notes

* the build script restores exec bits on `/bin`, `/sbin`, `/usr/bin`,
  `/usr/sbin` and the loader.
* `var/log/wtmp` may have an ACL that denies even root on a Windows/WSL mount;
  the script uses `tar --ignore-failed-read` and recreates it empty.
* Symlinks survive (git-bash/WSL store real reparse points).
* Verify with `chroot /data/rbx3-run /bin/sh -c 'echo ok'`.
