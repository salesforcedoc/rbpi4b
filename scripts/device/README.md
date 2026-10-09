# scripts/device/

Everything that runs **on the Raspberry Pi 4**, as root.

Note where these come from, because it is not the tarball: the deploy tarball
carries only `rb.conf` and the `rbx3-run/` chroot, and `install.sh` — which is
itself in this directory, run from wherever you scp'd it — copies the scripts
below into the deploy root (`/opt/rblive4` by default). So a unit needs
`scripts/device/` present at install time, not just a tarball:

```
/opt/rblive4/
├── rb.conf          every machine-specific constant, for the scripts AND the shims
├── rb.local.conf    machine-LOCAL overrides, sourced last; never overwritten
├── lib.sh           shared: rb.conf loading, the /proc process lookup
├── install.sh       one-time (and idempotent) deploy: untar, verify, fix-dev
├── uninstall.sh     the inverse, in the one safe order: stop, revert the trim,
│                    unmount and PROVE, then remove
├── doctor.sh        check a deployed unit and report; changes NOTHING
├── fix-dev.sh       bind mounts + device stubs; run again after every reboot
├── start-rb.sh      the launcher: services, shims, edb_streamd, rbp, USB watcher
├── usb-watch.sh     media hotplug -> mount -> bind into the chroot -> notify rbp
├── display-watch.sh monitor geometry change -> restart the unit
├── boot-trim.sh     the boot-time trim, and its own inverse
├── healthwatch.sh   the wedge recorder, run by healthwatch.service
├── vnc-run.sh       the VNC viewer's launcher, run by rblive4-vnc.service
├── vncserve/        the viewer itself: sources + the binary built HERE, on the unit
├── log/             rbp.log, edb_streamd.log, usbwatch.log, displaywatch.log, vncserve.log
├── media/usb1/sda1  where a stick is mounted (host side)
└── rbx3-run/        the soft-float ARM32 chroot: player, shims, DirectFB
```

| Script | Purpose |
|---|---|
| `install.sh` | Untars the deploy root, checks `rb.conf`, proves the chroot can execute a 32-bit binary, checks `/dev/fb0` and `/dev/snd/seq`, then runs `fix-dev.sh`. Run once per deploy. `install.sh doctor` runs `doctor.sh` instead. |
| `doctor.sh` | Checks a *deployed* unit and reports. **Changes nothing** — no write, no mount, no module, no service call; the one thing it executes is a chroot'd `busybox echo`. Its headline check is the one this tree has most often got wrong: every shim's deployed build against **the copy `rbp` is actually loading**, read out of the running process's `/proc/<pid>/maps`. Prints one paste-ready fix block and exits 1 if anything failed. Every path it knows comes out of `rb.conf` and `lib.sh`, so it cannot drift from them. |
| `fix-dev.sh` | Binds `/dev /proc /sys /tmp` into the chroot, creates the device stubs rbp expects, the `/tmp/udev_*` FIFOs and the `etc/mtab` symlink. Run after **every** reboot. |
| `start-rb.sh` | Stops the services that would fight for the display or the stick (each behind a read-only guard, so the steady state reloads nothing), kills stale processes, runs `fix-dev.sh`, translates `rb.conf` into the shim environment, starts `edb_streamd` then `rbp`, waits for rbp to open its USB FIFO, records the display baseline, starts both watchers, then waits for rbp to exit. |
| `usb-watch.sh` | `start`/`stop`/`status`/`run`. Watches for USB mass storage, mounts it, binds it into the chroot at the path baked into the player, and notifies rbp through `/tmp/udev_usb1`, retrying until rbp opens `export.pdb`. |
| `display-watch.sh` | `start`/`stop`/`status`/`run`/`baseline` (`--dry-run` on the first two). Compares the framebuffer's geometry against the one `rbp` was launched with and restarts the unit when it changes — the driver and the pointer each read the real geometry once and hold it, so a monitor swap otherwise leaves a sheared or blank picture until someone intervenes. Bounded by a cooldown and a per-boot cap, so a flapping monitor cannot become a restart loop. |
| `boot-trim.sh` | `apply`/`revert`/`status`/`report` (`items`: `services cloudinit apt unit bootfiles`). The boot-time trim, run by `install.sh` and reversible: it persists the service masks so the launcher's step 1 stops reloading systemd for no-ops, turns cloud-init off, takes the apt timers off the boot path, and ensures the `/boot/firmware` tokens and directives. `report` prints the boot's monotonic milestones so a before/after is a diff. |
| `healthwatch.sh` | Run by `healthwatch.service`, not by hand. Records *why* the unit wedges: a seq-numbered round every 15 s to `/opt/rblive4/log/health.log` (survives a reboot) **and** `/tmp/health.log` (survives a disk stall — read it before rebooting), probing the symptom itself with a loopback `:22` banner read. A gap in the sequence numbers is itself evidence that the recorder was not scheduled either. |
| `vnc-run.sh` | Run by `rblive4-vnc.service`, not by hand. Sources `lib.sh` + `rb.conf` and execs `vncserve/vncserve` with the `RB_VNC_*` values. A unit of its own rather than a branch in `start-rb.sh`, so a fault in the viewer cannot take the player down and `RB_VNC=0` costs nothing at all. Deliberately has **no ordering dependency** on `rblive4.service`: with the player down it serves an honest black screen with the drawers on it, which is a useful thing to look at while working out why the player is down. |
| `vncserve/` | The viewer: rbp's screen over RFB, with a runtime `raw` ↔ `hwjpeg` switch. Hand-rolled (a subset needing only libc) because `libvncserver` cannot be handed a JPEG we already encoded, and the hardware encoder is the entire point. Two ports: the session (`RB_VNC_PORT`) and the control page, which carries the mode switch, the live preview, and the full-screen view — **the state it opens in**; tap the picture for the words. **Built here on the unit by `install.sh`** — see the note below. |
| `uninstall.sh` | `--dry-run`/`--yes`/`--purge`. The inverse of `install.sh`, and the reason it is a script is the order: stop the units, `boot-trim.sh revert` **before** anything is deleted (it lives inside the tree being removed, and it is the only thing that knows how to restore `/boot/firmware` and re-enable the services the trim disabled), then unmount the six mounts under the deploy root and **re-read `/proc/mounts` and refuse while anything remains** — an `rm -rf` over a live bind mount descends through it, and one of the six is the host's own `/dev`. Preserves `rb.local.conf` to `/root` unless `--purge`, and never writes to, deletes from or repairs the USB media. |
| `lib.sh` | Sourced by eight of the others (`boot-trim`, `display-watch`, `doctor`, `fix-dev`, `install`, `start-rb`, `usb-watch`, `vnc-run`) — not run directly. `healthwatch.sh` deliberately does not: it has to keep working when the rest of the box does not. |

## Notes

* **`rb.conf` is the only place to change a machine-specific value.** It is
  sourced by all the scripts and copied into the tarball by
  `scripts/build-chroot.sh`, so a deploy cannot end up with a config that
  disagrees with its tree. Each variable's comment names the component that
  reads it, which tells you whether a change needs a rebuild.
* **A value measured on the unit goes in `rb.local.conf`, not `rb.conf`.** Every
  line of `rb.conf` is a *shipped default* and every one of them is rewritten on
  the next install — twice over, because `rb.conf` also travels inside the
  tarball, so the `tar -xzf` in `install.sh` replaces it before the script's own
  `cp` runs. `RB_POINT_KIND=rel`, set by hand after the four-corner procedure,
  reverted to `auto` on the next deploy, and the symptom was a pointer that worked
  only on the bench it was calibrated on. `rb.local.conf` is not in the tarball and
  `install.sh` creates it once and never writes it again; `rb.conf` sources it
  last, so **assign plainly in it** (`RB_POINT_KIND=rel`) rather than with `:=`,
  which would leave the default in place. It is not a second schema: a stray
  `RB_CONF_VERSION` in it is ignored on purpose.
* **`RB_<NAME>` becomes `<NAME>`** in the launched environment — that mapping is
  in one list in `start-rb.sh`, and it is the only translation point. The shims
  never parse `rb.conf`. **Empty means unset** for every one of those variables.
* **`LD_PRELOAD` is passed inside the chroot** (`chroot … env LD_PRELOAD=… ld.so
  rbp`), not exported by the launcher shell: the loader that must act on it is
  the chroot's `ld.so`, and exporting it would also hand it to the *host's*
  `chroot` binary. The order `fbshim:knobshim:audioshim` is load-bearing — see
  [`scripts/shims/shmstate.h`](../shims/shmstate.h).
* **Processes are found via `/proc/*/cmdline`**, never `ps w | awk`: busybox and
  procps `ps` truncate differently, and a pattern loose enough to survive that
  can match the `awk` doing the matching — [docs/11](../../docs/11-runtime-launcher.md)
  records that failure.
* **`/dev` inside the chroot is a bind mount of the host's `/dev`**, so the stubs
  `fix-dev.sh` creates are really created in the Pi's own `/dev`. That is
  intended (the chroot sees the same nodes), but it is why they must be recreated
  every boot: `/dev` is a tmpfs.
* **Every `systemctl` mutation in `start-rb.sh`'s step 1 sits behind a read-only
  guard, and that is a boot-time fix, not tidiness.** A *mutation* makes systemd
  reload its entire manager: measured at 1.8–3.6 s per call, **9.84 s of a 15.4 s
  launcher**, spent re-masking an already-masked `udisks2` and "stopping" three
  units that are not installed. A read-only `is-enabled`/`is-active` is 31–40 ms
  and reloads nothing. The mutations are kept — the launcher must still work on a
  target where `install.sh` has not run — so the guard is what makes them free:
  `boot-trim.sh apply services` persists those same decisions at install time.
  The `reloads` row of `boot-trim.sh report` is the regression test, and it reads
  **1**, not 0: the one that remains is NetworkManager's own, which is not the
  launcher's to remove. What matters is that **0** are attributable to the
  launcher, which is the state every boot since has been in;
  [docs/13](../../docs/13-raspberrypi4.md) has the measurement.
* **`boot-trim.sh` is the only thing in this tree that edits `/boot/firmware`**,
  and it does so under three rules: one backup per file created once and never
  overwritten (`cmdline.txt.rblive4.bak`, matching the operator's own
  `cmdline.txt.bak-1080p` idiom); write a temp on the same partition then `mv`
  over, because that partition is FAT with no journal on a unit with a brownout
  history; and verify before replacing — one line, and every root-finding token
  the original had. A refused edit leaves the original untouched and names the
  candidate. It carries the `HDMI-A-1` `video=` token only; `status bootfiles`
  names the `HDMI-A-2` gap rather than closing it, because that token is S10.8's
  precondition, not a boot-time trim.

* **`vncserve` is compiled on the unit, by `install.sh`, and that is not a
  shortcut — there is no toolchain in this tree that can produce it.** The
  `rblive4-build` image is soft-float **armel**; this unit's userland is
  hard-float **armhf**, and the image has no `arm-linux-gnueabihf-gcc` and no
  hard-float `libc.a`. It would exit 0 having built nothing, which is exactly why
  the compile is a step in `install.sh` that **aborts the install with the
  compiler's own output** on failure instead of a summary of it. The build runs in
  `$DEPLOY/.vnc-build.$$` and only the finished binary is renamed into
  `$DEPLOY/vncserve/` — `cc -o` truncates its output in place, and a running
  `rblive4-vnc` has that binary mapped, so building in place would `SIGBUS` the
  operator's live view. `make -C scripts/device` builds it on the workstation too,
  with the **host** compiler and ignoring `CROSS`, so a compile error surfaces here
  rather than at install time; `make -C scripts/device test` runs its six
  pure-module suites natively — `vnc_compose` 90, `vnc_des` 11, `vnc_diff` 24,
  `vnc_mode` 31, `vnc_rfb` 108, `vnc_zlib` 231, **495 checks** — which is a
  deliberate difference from `scripts/shims/`, whose tests are soft-float armel under
  `qemu`. **`test_vnc_zlib` cannot run on the unit** — it includes `<zlib.h>` and the
  unit carries zlib's *runtime*, not its headers — so `make test` there stops at it;
  build and run that one on the workstation, or name the other five explicitly. The
  unit's `Makefile` links `-l:libz.so.1` directly for the same reason.

* `timeout.c` / `make` here build a static `timeout` for targets without
  coreutils. **Not needed on Pi OS**, which has `/usr/bin/timeout` — that is what
  `RB_USB_TIMEOUT` points at. Kept for portability.

## See also

* [docs/13-raspberrypi4.md](../../docs/13-raspberrypi4.md) — the Pi target:
  image, `cmdline.txt`, present modes, bring-up order.
* [docs/11-runtime-launcher.md](../../docs/11-runtime-launcher.md) — what the
  launcher does and why, including the DeviceSQL lock files.
* [docs/10-usb.md](../../docs/10-usb.md) — the media mount and the rbp FIFO
  protocol.
