# 11 — Runtime launcher

[`scripts/device/start-rb.sh`](../scripts/device/start-rb.sh) runs on the Pi, as
root, over SSH, and is the normal way to bring up the player. It deploys to
`$RB_DEPLOY_ROOT/start-rb.sh` (`/opt/rblive4/start-rb.sh`) with `lib.sh`,
`fix-dev.sh` and `usb-watch.sh` beside it.

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
2. **Kill stale processes** — rbp, `edb_streamd`, `gdbserver`, `usb-watch.sh`,
   by scanning `/proc/*/cmdline`.
3. **`fix-dev.sh`** — bind mounts, device stubs, FIFOs, `/dev/snd/seq`.
4. **Install overrides**, if any are present at the deploy root (below).
5. **Clear stale IPC and logs** — `/tmp/guard_LocalDBServer` and
   `/tmp/req_LocalDBServer` (a leftover guard from an unclean exit makes the next
   rbp believe a database server is already running), plus the old shim and
   driver logs.
6. **Translate `rb.conf` into the shim environment** (below).
7. **Start `edb_streamd`** inside the chroot, through the chroot's own loader,
   **before** rbp.
8. **Stop the USB watcher** while rbp initialises.
9. **Start rbp.**
10. **Wait for rbp's DeviceSQL channel**, detected by it opening the
    `/tmp/udev_usb1` FIFO, then start the watcher. A media mount event sent
    before that is silently lost — this is the same fact
    [10](10-usb.md) describes from the watcher's side.
11. **Sleep while rbp lives**, then clean up and report the exit.

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

Both this script and `usb-watch.sh` find processes by scanning
`/proc/*/cmdline` (`pids_matching()` in `lib.sh`), never `ps w | awk`. Two
reasons, one of them nasty:

* busybox `ps` and procps `ps` truncate the command line differently, so a
  pattern that works on one host silently matches nothing on another;
* a pattern loose enough to survive that truncation also matches the shell
  running it — an inline `ps | grep '/root/pdj/rbp'` kill loop **kills your own
  SSH session**, which is exactly what it did here.

The pattern passed in is the real command line, which for the player is
`"$RB_LOADER $RB_PLAYER …"` because it is started through the chroot's loader.

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

## Autostart

There is no shipped autostart unit, and the launcher stays manual. If you want
one, a plain systemd unit that runs `start-rb.sh` after `multi-user.target` works
because the script is already idempotent and already stops the services that
would conflict — but make it easy to disable, since a unit that takes over the
HDMI output at boot is a unit you cannot debug over a local console.
