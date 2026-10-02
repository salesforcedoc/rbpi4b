# scripts/device/

Everything that runs **on the Raspberry Pi 4**, as root. These ship inside the
deploy tarball and land at the deploy root (`/opt/rblive4` by default) beside the
`rbx3-run/` chroot:

```
/opt/rblive4/
├── rb.conf          every machine-specific constant, for the scripts AND the shims
├── rb.local.conf    machine-LOCAL overrides, sourced last; never overwritten
├── lib.sh           shared: rb.conf loading, the /proc process lookup
├── install.sh       one-time (and idempotent) deploy: untar, verify, fix-dev
├── fix-dev.sh       bind mounts + device stubs; run again after every reboot
├── start-rb.sh      the launcher: services, shims, edb_streamd, rbp, USB watcher
├── usb-watch.sh     media hotplug -> mount -> bind into the chroot -> notify rbp
├── display-watch.sh monitor geometry change -> restart the unit
├── log/             rbp.log, edb_streamd.log, usbwatch.log, displaywatch.log
├── media/usb1/sda1  where a stick is mounted (host side)
└── rbx3-run/        the soft-float ARM32 chroot: player, shims, DirectFB
```

| Script | Purpose |
|---|---|
| `install.sh` | Untars the deploy root, checks `rb.conf`, proves the chroot can execute a 32-bit binary, checks `/dev/fb0` and `/dev/snd/seq`, then runs `fix-dev.sh`. Run once per deploy. |
| `fix-dev.sh` | Binds `/dev /proc /sys /tmp` into the chroot, creates the device stubs rbp expects, the `/tmp/udev_*` FIFOs and the `etc/mtab` symlink. Run after **every** reboot. |
| `start-rb.sh` | Stops the services that would fight for the display or the stick (each behind a read-only guard, so the steady state reloads nothing), kills stale processes, runs `fix-dev.sh`, translates `rb.conf` into the shim environment, starts `edb_streamd` then `rbp`, waits for rbp to open its USB FIFO, records the display baseline, starts both watchers, then waits for rbp to exit. |
| `usb-watch.sh` | `start`/`stop`/`status`/`run`. Watches for USB mass storage, mounts it, binds it into the chroot at the path baked into the player, and notifies rbp through `/tmp/udev_usb1`, retrying until rbp opens `export.pdb`. |
| `display-watch.sh` | `start`/`stop`/`status`/`run`/`baseline` (`--dry-run` on the first two). Compares the framebuffer's geometry against the one `rbp` was launched with and restarts the unit when it changes — the driver and the pointer each read the real geometry once and hold it, so a monitor swap otherwise leaves a sheared or blank picture until someone intervenes. Bounded by a cooldown and a per-boot cap, so a flapping monitor cannot become a restart loop. |
| `boot-trim.sh` | `apply`/`revert`/`status`/`report` (`items`: `services cloudinit apt unit bootfiles`). The boot-time trim, run by `install.sh` and reversible: it persists the service masks so the launcher's step 1 stops reloading systemd for no-ops, turns cloud-init off, takes the apt timers off the boot path, and ensures the `/boot/firmware` tokens and directives. `report` prints the boot's monotonic milestones so a before/after is a diff. |
| `lib.sh` | Sourced by the other four — not run directly. |

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
