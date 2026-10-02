# 11 — Runtime launcher

[`scripts/device/start-rb.sh`](../scripts/device/start-rb.sh) runs on the Pi, as
root, over SSH, and is the normal way to bring up the player. It deploys to
`$RB_DEPLOY_ROOT/start-rb.sh` (`/opt/rblive4/start-rb.sh`) with `lib.sh`,
`fix-dev.sh`, `usb-watch.sh` and `display-watch.sh` beside it.

It is **not** something to launch from the console you intend to keep using: the
UI takes over the HDMI framebuffer, and the launcher sleeps until rbp exits
rather than letting a shell redraw over it. `fbcon=map:1` on the kernel command
line is what keeps the console off the screen at all
([13](13-raspberrypi4.md#the-hdmi-mode)).

## What it does

1. **Stop the services that would fight us** — `$RB_STOP_SERVICES` (default
   `lightdm gdm3 sddm`: a display manager drawing over `fb0`), mask
   `$RB_MASK_SERVICES` (default `udisks2 udisks2.service`: it will mount the
   media stick before `usb-watch.sh` can bind it into the chroot, and then two
   mounts of one device leave rbp reading a path the kernel has given away), and
   — when `RB_DISABLE_GETTY=1` — `systemctl disable --now getty@tty1`. SSH is
   unaffected, which is how the launcher is run.

   On the previous target this step was `systemctl stop engine.service
   edisksd.service`; the *risk* it addressed is identical, only the names
   changed.

   **Every call here is behind a read-only guard, and that is a measured
   boot-time fix.** Each systemctl *mutation* makes systemd reload its entire
   manager: 1.8–3.6 s per call on the Pi 4, **9.84 s of a 15.4 s launcher**,
   spent re-masking an already-masked `udisks2` and "stopping" three units that
   are not installed. A read-only `is-enabled`/`is-active` is 31–40 ms and
   reloads nothing, so the steady-state path is now the read-only one; the
   mutations remain for a target where `install.sh` has not run. `boot-trim.sh
   apply services` persists those same decisions at install time, and `boot-trim.sh
   report`'s `reloads` row is the regression test. It reads **1**, not 0: the
   remaining reload is NetworkManager's own, which is not the launcher's to
   remove. What is attributable to the launcher is **0**, and that is what the
   row is read against. [13](13-raspberrypi4.md#boot-time) has the measurement.

   The getty guard is on `is-enabled`, **not** `is-active`, and `getty@tty1` is
   never *masked*: `Conflicts=` does not disable, so an enabled-but-inactive getty
   is exactly the state that must be caught (otherwise `getty.target` starts it
   near multi-user, the conflict stops the player, and `Restart=always` undoes
   that every 10 s), while masking it would break the documented console-recovery
   idiom `systemctl start getty@tty1`.

2. **Kill stale processes** — rbp, `edb_streamd`, `gdbserver`, `usb-watch.sh` and
   `display-watch.sh`, by scanning `/proc/*/cmdline`.
3. **`fix-dev.sh`** — bind mounts, device stubs, FIFOs, `/dev/snd/seq`.
4. **Install overrides**, if any are present at the deploy root (below).
5. **Clear stale IPC and logs** — `/tmp/guard_LocalDBServer` and
   `/tmp/req_LocalDBServer` (a leftover guard from an unclean exit makes the next
   rbp believe a database server is already running), plus the old shim and
   driver logs, plus the display watcher's baseline
   (`/tmp/displaywatch.geom`) — a previous boot's geometry must not suppress a
   restart this boot needs. Its fire counter (`/tmp/displaywatch.fires`) is
   deliberately **not** cleared: that counter exists to bound a flapping monitor
   to N restarts per boot, and clearing it here would hand the flap a fresh
   budget every time it won a restart.
6. **Translate `rb.conf` into the shim environment** (below).
7. **Start `edb_streamd`** inside the chroot, through the chroot's own loader,
   **before** rbp.
8. **Stop the USB watcher** while rbp initialises.
9. **Record the display baseline, then start rbp.** The baseline is written as
   the *last* thing before the launch, deliberately not after rbp is confirmed
   up: a monitor that changed during rbp's own bring-up would otherwise be
   written down as the geometry rbp launched with, and that change would never
   fire.
10. **Wait for rbp's DeviceSQL channel**, detected by it opening the
    `/tmp/udev_usb1` FIFO, then start the two watchers. A media mount event sent
    before that is silently lost — this is the same fact
    [10](10-usb.md) describes from the watcher's side. The display watcher
    starts with it, and for the same reason: it refuses to fire while rbp is not
    running, since systemd's `Restart=always` is already relaunching rbp during
    bring-up. The launcher prints its own distinct line for each watcher
    (`start-rb: usb-watch started`, `start-rb: display-watch started`) because
    both watchers log the byte-identical `started pid N` — without them
    `boot-trim.sh report`'s two watcher rows would be unattributable.
11. **Sleep while rbp lives**, then clean up and report the exit. Both watchers
    are stopped here, which is what makes "no watcher is left behind" true
    (S10.6). When the exit *is* the restart the display watcher asked for, the
    request has already reached systemd, so stopping it here cancels nothing.

## The rbp launch line

```sh
nohup chroot "$RB_CHROOT" env \
      "PATH=/bin:/sbin:/usr/bin:/usr/sbin" \
      "LD_PRELOAD=$RB_LD_PRELOAD" \
      "$RB_LOADER" "$RB_PLAYER" -a \
      </dev/null >>"$RBP_LOG" 2>&1 &
```

* `env` runs **inside** the chroot, so `LD_PRELOAD` and `PATH` are in effect for
  ld.so and rbp only — never for the host process doing the `chroot`. Exporting
  `LD_PRELOAD` in the launcher shell would hand it to the *host's* `chroot`
  binary, whose glibc would try to preload soft-float ARM32 libraries from paths
  that mean something different on the host.
* `PATH` must include `/bin`: the chroot's busybox tools live there and rbp logs
  `sh: ls: not found` without it.
* The player is started through the chroot's loader rather than by exec because
  it is non-PIE, soft-float, and linked against glibc 2.13.
* **The `LD_PRELOAD` order is load-bearing**:
  `fbshim.so:knobshim.so:audioshim.so`. The audio shim reads globals
  (`g_master_gain`, `g_vu_peak`, …) that the controls shim defines. A wrong order
  fails loudly rather than silently — see
  [`scripts/shims/shmstate.h`](../scripts/shims/shmstate.h) and the
  `SHMSTATE_STRICT` guard.

## `rb.conf` → the shim environment

`rb.conf`'s `RB_<NAME>` becomes `<NAME>` in the launched environment, which is
the name the shim or the patched DirectFB driver reads. That translation happens
in **one list** (`SHIM_VARS`), so adding a variable to `rb.conf` costs a line
there rather than a hand-written `export` that drifts out of step with it. **The
shims never parse `rb.conf`.**

**Empty means unset**, for every one of those variables — and the loop exports
the empty ones too, so the *consumers* have to honour it: an unset `POINT_DEV`
must mean "discover the pointer", not "look for a device named `''`".

> **A landmine, and its fix.** A handful of the older shim variables are still
> read by **presence** (`getenv("JOG_VERBOSE") != NULL`), and since the loop
> exports every name in its list, `JOG_VERBOSE=0` from `rb.conf` switches
> verbose logging **on**. `start-rb.sh` works around the affected names today
> (exporting them only when they should be on), and the fix is to move the
> consumers to value semantics — `shimutil.h`'s `env_on()`/`env_num()` — at
> which point the workaround lines collapse into `SHIM_VARS`. Until that is
> finished, the workaround is what makes `RB_VERBOSE=0` and `RB_LED_PADS=0`
> behave.

## Overrides

A clean deploy needs none of this: `build-chroot.sh` already placed the player,
the shims and the patched fbdev module *inside* the chroot. But dropping a
freshly built file at the deploy root and re-running the script is how you
iterate on one shim without rebuilding the tarball:

| Drop this at `$RB_DEPLOY_ROOT` | It replaces |
|---|---|
| `knobshim.so`, `fbshim.so`, `audioshim.so` | the same name in `$RB_CHROOT/usr/lib` |
| `rbp-audio` | `$RB_CHROOT/root/pdj/rbp` |
| `libdirectfb_fbdev-rot16.so` | `$RB_CHROOT/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` |

The last one keeps the previous target's filename; the file it replaces is
plain `libdirectfb_fbdev.so` ([06](06-display.md)).

## Finding processes: never `ps | awk`

Both this script, `usb-watch.sh` and `display-watch.sh` find processes by
scanning `/proc/*/cmdline` (`pids_matching()` in `lib.sh`), never `ps w | awk`.
Two reasons, one of them nasty:

* busybox `ps` and procps `ps` truncate the command line differently, so a
  pattern that works on one host silently matches nothing on another;
* a pattern loose enough to survive that truncation also matches the shell
  running it — an inline `ps | grep '/root/pdj/rbp'` kill loop **kills your own
  SSH session**, which is exactly what it did here.

The pattern passed in is the real command line, which for the player is
`"$RB_LOADER $RB_PLAYER …"` because it is started through the chroot's loader.

## The display watcher, and why it is a child

A monitor swapped for one of a **different size** leaves rbp drawing to the old
geometry: the driver reads the real framebuffer once, at open, and `fb_cursor.c`
reads it once too and holds the mapping. Nothing in either notices. So the
launcher starts [`display-watch.sh`](../scripts/device/README.md) beside the USB
watcher, and it polls `/sys/class/graphics/fb0` and the HDMI connector's `status`
against the baseline step 9 wrote, restarting the unit when the geometry stops
matching.

It is a **child of the launcher rather than its own unit**, and that is a
deliberate choice with one non-obvious consequence: the restart it asks for is a
restart of the very unit whose cgroup contains it, so `KillMode=mixed` SIGKILLs
the watcher as it fires. That is fine — the `systemctl --no-block restart` has
already been queued by systemd, the evidence line is written *before* the call,
and the log line is the whole point. A unit would instead have to outlive the only
process whose geometry it compares against, and handle the no-framebuffer-at-boot
and missing-chroot cases itself.

What the child design appears to lose is the counter that bounds a flapping
monitor, and the answer is that the counter lives in a **file**
(`/tmp/displaywatch.fires`), not in a variable. With the two-poll debounce, the
cooldown, and that persisted per-boot cap, a monitor that is being plugged and
unplugged repeatedly costs a bounded number of restarts and then stops —
which is the one property here that matters more than the feature.

**Measured, 2026-09-26 — the decisions, not the restart.** Run in `--dry-run`
against a fake `RB_SYSFS_ROOT`, the watcher produced exactly the decisions above:
a mismatched geometry fired **once**, a poll after the debounce agreed, and the
poll after that decided nothing; a tree with no `virtual_size` logged the
framebuffer gone **once** and never fired; the leaf returning at the launch
geometry fired **nothing** at the default and **once** with
`RB_DISPLAY_RESTART_ON_RECONNECT=1`. On the real `/sys` it started on every launch
of that session's drills and never fired across three different baselines. **The
cooldown and the cap are still unmeasured**, and `--dry-run` cannot reach them: it
returns *before* the counter is written, so a dry-run sequence stays at zero
fires. Exercising them needs real restarts, which is why that is
[S10.10](13-raspberrypi4.md#s10--the-display-drills)'s physical drill.

**It cannot recurse.** Firing requires the geometry to differ from the baseline;
the restart replaces the launcher that wrote the baseline, and the new launcher
writes it again from the same readings before it launches. A change that lands
during the new launcher's own bring-up makes the baseline stale by exactly that
change and costs one extra restart; the launch after that is stable.

`RB_DISPLAY_POLL_S` (default 2) and `RB_DISPLAY_RESTART_ON_RECONNECT` (default 0)
are in `rb.conf`. The second is the one place a **same-mode** replug could still
leave a black screen — a framebuffer torn down and recreated at identical
geometry, which no geometry comparison can see — and it exists so that outcome is
a config change rather than a code change.

## Restarting

Always clear the DeviceSQL locks when restarting rbp, or the USB library shows
the generic "USB1" label instead of the volume name and track count:

```sh
sh /opt/rblive4/start-rb.sh          # idempotent: it kills the previous instance first
```

There is no "restore stock" step on the Pi — nothing else uses the display or
the audio device once the launcher is running, so returning to the desktop is
just stopping rbp (Ctrl-C the launcher, or `kill` the player) and restarting the
display manager if you want a console:

```sh
systemctl unmask udisks2
systemctl start lightdm        # if it was installed
```

On the unit the usual way is `systemctl restart rblive4` — see below. Ctrl-C now
genuinely cleans up: `start-rb.sh` installs its `cleanup()` as a SIGTERM/SIGINT
handler, so the shell that dies does not orphan rbp, `edb_streamd`, the watchers
and the media mounts behind it. Before that it did, and a "stopped" unit with a
live player on screen then fought the next start.

## Autostart

The unit **is** shipped: [`scripts/device/rblive4.service`](../scripts/device/rblive4.service),
installed and enabled by `install.sh`, so the player comes up on its own after a
reboot. `RB_AUTOSTART=0` (in `rb.conf`, or in `rb.local.conf` to override it per
machine) installs the unit but leaves it disabled — that is the setting for a
target being brought up by hand.

Four properties of the unit are load-bearing rather than decorative:

* `Restart=always` / `RestartSec=10` is the backstop under everything else: a
  player that exits for any reason comes back, which is what makes the display
  watcher's restart route a request rather than a mechanism.
* `KillMode=mixed` is what lets the launcher's `cleanup()` run on a stop while
  the processes it started are reaped afterwards — and it is what SIGKILLs the
  display watcher by the restart it asked for, which is intended.
* There is deliberately **no** `ConditionPathExists=/dev/fb0`. A unit that
  refuses to start when no monitor is attached is a unit that cannot be debugged
  over SSH, and `/dev/fb0` is absent at boot on this target when nothing is
  plugged in. What rbp does in that case is measured by S10.9, not assumed.
* **`After=basic.target`, not `After=multi-user.target`.** The unit used to wait
  for multi-user and started 9 ms after it, which bought nothing: the launcher
  needs the framebuffer, `/dev`, `/proc`, `/sys` and `/tmp`, all of which are up at
  `basic.target`, and no network, no media mount and no other service. Removing the
  line leaves DefaultDependencies to supply
  `After=basic.target`/`Requires=sysinit.target`, so the job still belongs to the
  multi-user transaction but starts as soon as what it uses is up;
  `Type=simple` means `multi-user.target` is not delayed by it in turn. The
  ordering is stated explicitly in the unit rather than left to the default,
  because the whole change rests on it. `WantedBy=multi-user.target` stays, so
  the enablement symlink and `systemctl enable` remain valid.

  **How much it bought is ~5 s in the configuration the unit ships, and the
  12.9 s first quoted here was an artefact** — `multi-user`'s *monotonic* stamp
  minus `basic`'s *userspace* one, which inflates the span by the kernel term
  (2.243 s). In one basis the baseline span is 10.634 s and the earlier
  pre-change boot's is 9.056 s; what the reorder saves is that span, so it is a
  property of the boot rather than of the change, and once cloud-init is off it
  measures 4.4–5.7 s.
  [13](13-raspberrypi4.md#boot-time) carries the series. What does not depend on
  the configuration is *what starts when*: rblive4 starts at `basic` + ~0.05 s
  rather than `multi-user` + ~0.005 s in every boot since, and that is why three
  trimmed boots reached `rbp-ready` within 0.234 s of each other.

  **The one assumption this makes** is that `/dev/fb0` exists by
  `basic.target`. `systemd-udev-trigger` only *queues* coldplug, so that is
  asserted by measurement ([13](13-raspberrypi4.md#boot-time), S11.4), not by
  construction. If it were ever missing, rbp exits and `Restart=always` retries
  every 10 s, each retry re-running `fix-dev.sh`'s chmod over the chroot during
  boot. The escape hatch is `Wants=` **and** `After=` on
  `systemd-udev-settle.service` — the `Wants=` is mandatory, since `After=` alone
  does not pull a unit in.

  `Conflicts=` is symmetric, which is what makes the earlier start need
  `install.sh`'s persisted disables: whichever of `rblive4` and
  `display-manager` starts *second* stops the first, and before this change
  `rblive4` always started second. The `systemctl disable` of each display
  manager and of `getty@tty1` that `boot-trim.sh apply services` writes is
  therefore load-bearing on a target that has one installed and enabled, and
  inert on this unit, where none of the three exists.
