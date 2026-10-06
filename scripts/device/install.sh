#!/bin/sh
# install.sh — deploy rbpi4b onto a Raspberry Pi 4.
#
# Run as root on the Pi:
#
#   scp work/rbpi4b-pi4.tgz pi@<host>:/tmp/
#   scp -r scripts/device pi@<host>:/tmp/
#   ssh pi@<host> 'sudo sh /tmp/device/install.sh /tmp/rbpi4b-pi4.tgz'
#
# Untars the deploy root to RB_DEPLOY_ROOT (default /opt/rblive4), copies the
# device scripts in beside it, installs the systemd unit that starts the player
# at boot, and runs fix-dev.sh. Idempotent: re-running upgrades in place, and
# clears stale host metadata (see the AppleDouble sweep below) that an upgrade
# alone would leave behind.
#
# RB_AUTOSTART=0 (rb.conf, or rb.local.conf to override it per machine) installs
# the unit but leaves it disabled, for a target being brought up by hand.
#
# RB_DEPLOY_ROOT overrides the destination:
#   sudo RB_DEPLOY_ROOT=/srv/rblive4 sh install.sh /tmp/rbpi4b-pi4.tgz

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
TARBALL="${1:-/tmp/rbpi4b-pi4.tgz}"
DEPLOY="${RB_DEPLOY_ROOT:-/opt/rblive4}"

say()  { echo "install: $*"; }
warn() { echo "install: WARNING: $*" >&2; }
die()  { echo "install: ERROR: $*" >&2; exit 1; }

# `sh install.sh doctor` is the check-only entry point: it runs doctor.sh, which
# changes nothing at all, and exits with its status. Deliberately BEFORE the root
# and tarball checks below — a diagnostic is most useful on a unit where those
# two are exactly what is in question. (The sibling port has the same subcommand;
# see docs/17-rx3-flx4-comparison.md.)
if [ "${1:-}" = "doctor" ]; then
  shift
  exec sh "$HERE/doctor.sh" "$@"
fi

# --- preconditions ----------------------------------------------------------

[ "$(id -u)" = "0" ] || die "must run as root (the chroot needs /dev/mknod and bind mounts)"

[ -f "$TARBALL" ] || die "tarball not found: $TARBALL
  build it on the workstation with scripts/build-chroot.sh, then scp it here."

# A 64-bit kernel is not a problem, and on current Pi OS images it is the norm:
# the 32-bit image ships the arm64 kernel (-v8) with an armhf userland on it. What
# the chroot needs is not a 32-bit kernel but 32-bit *emulation* in the kernel
# (CONFIG_COMPAT), because rbp and the shims are 32-bit ARM ELF and bring their
# own 32-bit ld.so -- the userland's own bitness never enters into it. The check
# that matters is executing a 32-bit binary, which happens below; this line only
# reports the arrangement, so that an aarch64 `uname -m` is not read as a fault.
# (The measured unit is exactly this case: 6.18.50+rpt-rpi-v8, with
# `32-bit EL0 Support` in its CPU features line.)
say "kernel: $(uname -m) $(uname -r); userland: $(getconf LONG_BIT 2>/dev/null || echo '?')-bit
  A 64-bit kernel with a 32-bit userland is the supported arrangement; the chroot
  test below is what proves the kernel can run 32-bit ARM ELF (docs/13-raspberrypi4.md)."

# --- unpack -----------------------------------------------------------------

say "deploying to $DEPLOY"
mkdir -p "$DEPLOY"
tar -C "$DEPLOY" -xzf "$TARBALL"

[ -d "$DEPLOY/rbx3-run" ] || die "tarball did not contain rbx3-run/ -- is it a rbpi4b-pi4.tgz?"
[ -f "$DEPLOY/rb.conf" ]  || die "tarball did not contain rb.conf -- rebuild with scripts/build-chroot.sh"

# rb.conf's location on the device is the authority from here on: it is what the
# launcher reads, so install.sh follows it rather than assuming $DEPLOY.
RB_CONF_FILE="$DEPLOY/rb.conf"
. "$RB_CONF_FILE"

# The expected schema lives in lib.sh (RB_CONF_SCHEMA) rather than here, so
# there is one number and not two. This warning previously hardcoded "1" while
# rb.conf declared 2 — which is precisely the drift the version exists to catch,
# and it would have warned on every correct install.
#
# It stays a warning rather than a failure, unlike the other three scripts,
# which refuse to run on a mismatch (rb_load_conf): by this point the tarball is
# already unpacked, so completing the install is more useful than aborting it.
if [ -f "$HERE/lib.sh" ]; then
  . "$HERE/lib.sh"
  [ "${RB_CONF_VERSION:-0}" = "$RB_CONF_SCHEMA" ] || \
    warn "rb.conf reports schema v${RB_CONF_VERSION:-?}; these scripts expect v$RB_CONF_SCHEMA"
else
  warn "lib.sh is not beside install.sh, so rb.conf's schema could not be checked"
fi

CHROOT="${RB_CHROOT:-$DEPLOY/rbx3-run}"
LOG_DIR="${RB_LOG_DIR:-$DEPLOY/log}"

mkdir -p "$LOG_DIR" "${RB_MEDIA_MOUNT:-$DEPLOY/media/usb1}"

# Untarring over an existing tree upgrades in place but never *removes*
# anything, so a deploy root that has ever received a tarball built on a
# metadata-carrying host keeps those leftovers forever -- the tarball being clean
# on the next deploy does not clean the target. The ones that matter are macOS's
# AppleDouble "._name" companions: DirectFB walks each module directory and tries
# to dlopen every entry, so each "._libdirectfb_*.so" prints an "Unable to
# dlopen" error at startup and the module dir looks broken.
# build-chroot.sh now strips these from the tarball; this clears whatever an
# earlier deploy already left on the device. No legitimate file in this tree is
# named "._*", so the sweep cannot take anything real.
#
# -xdev, and deliberately not -prune: -delete implies -depth, and GNU find
# refuses to combine -depth with -prune ("-prune does nothing when -depth is in
# effect"), exits 1, and deletes nothing -- which is how the first version of
# this sweep reported success while removing not a single file. -xdev gets the
# "stay on this filesystem" behaviour the prune was for, and does it better: it
# steps over fix-dev.sh's /proc, /sys and /dev bind mounts (whose contents are
# the host's, and whose transient PID directories make the walk noisy) without
# ever descending into them.
STALE_META=$(find "$DEPLOY" -xdev -name '._*' -type f 2>/dev/null | wc -l)
if [ "$STALE_META" -gt 0 ]; then
  find "$DEPLOY" -xdev -name '._*' -type f -delete 2>/dev/null || true
  # Verify rather than assume: a sweep that silently removes nothing is exactly
  # the failure above, and it should not be able to happen twice.
  STALE_LEFT=$(find "$DEPLOY" -xdev -name '._*' -type f 2>/dev/null | wc -l)
  if [ "$STALE_LEFT" -gt 0 ]; then
    warn "$STALE_LEFT AppleDouble file(s) could not be removed. DirectFB will
  log 'Unable to dlopen' for each ._libdirectfb_*.so it walks past in the module
  directories. Remove them by hand:
    find $DEPLOY -xdev -name '._*' -type f -delete"
  else
    say "removed $STALE_META stale AppleDouble file(s) left by an earlier deploy"
  fi
fi

# --- device scripts ---------------------------------------------------------

# The launcher scripts live at the deploy root on the device, next to rb.conf,
# matching how they were laid out on the previous target. lib.sh is required —
# all four scripts source it for rb.conf loading and the /proc process lookup.
# The list is explicit rather than a glob: this directory also holds install.sh
# itself, plus README.md and the build-side inputs, none of which belong on the
# unit.
for s in lib.sh fix-dev.sh start-rb.sh usb-watch.sh display-watch.sh doctor.sh boot-trim.sh rb.conf; do
  if [ -f "$HERE/$s" ]; then
    cp "$HERE/$s" "$DEPLOY/$s"
    [ "$s" = "rb.conf" ] || chmod 755 "$DEPLOY/$s"
  else
    warn "$s not found in $HERE -- not installed"
  fi
done

# rb.conf, just replaced, is the SHIPPED default: it is overwritten here and again
# by the `tar -xzf` above, because it travels inside the tarball. A value measured
# on this unit therefore cannot live in it -- RB_POINT_KIND=rel, set by hand after
# the four-corner procedure, reverted to `auto` on the next install and the
# pointer went back to hunting for a touchscreen that is not there.
#
# rb.local.conf is the place for those: rb.conf sources it last, this script
# creates it once and never writes it again, and it is deliberately absent from
# the tarball so the extraction cannot reach it either.
if [ -f "$DEPLOY/rb.local.conf" ]; then
  say "keeping the existing rb.local.conf (local overrides are never overwritten)"
else
  cat > "$DEPLOY/rb.local.conf" <<'LOCAL'
# rb.local.conf — machine-local overrides for rb.conf. Sourced by rb.conf, last.
#
# This file is NOT part of the deploy tarball and install.sh never rewrites it, so
# unlike rb.conf it survives an upgrade. Put values MEASURED ON THIS UNIT here.
#
# Assign plainly (RB_POINT_KIND=rel), not with `:=`: this file is sourced after
# the shipped defaults, so a plain assignment is what overrides them.
#
# The per-variable comments in rb.conf say what each value means; the "local
# overrides" section at its end explains why this file exists.

# Example -- and the one that has actually been needed on this unit: this Pi has a
# mouse, not a touchscreen, so the pointer source is pinned rather than discovered.
# RB_POINT_KIND=rel
LOCAL
  chmod 644 "$DEPLOY/rb.local.conf"
  say "created $DEPLOY/rb.local.conf for machine-local overrides (empty)"
fi

# --- systemd autostart ------------------------------------------------------
#
# The unit is installed from here rather than shipped in the tarball, so the
# tarball stays exactly "what build-chroot.sh produced" and the unit travels with
# the launcher it starts. Until this existed the player only came up when someone
# SSHed in and ran start-rb.sh, which is what a reboot leaves you with: a machine
# that looks broken and is merely idle.

UNIT=/etc/systemd/system/rblive4.service
if [ -f "$HERE/rblive4.service" ]; then
  # `install`, not the cp+chmod loop above: that loop chmods 755 everything it
  # touches and a mode-755 unit file is a systemd warning, and it inherits the
  # source file's ownership -- which on a tarball built on a Mac is uid 501, not
  # root. Both are stated explicitly here.
  install -m 644 -o root -g root "$HERE/rblive4.service" "$UNIT"
  say "installed $UNIT"
else
  warn "rblive4.service is not in $HERE -- the player will NOT start at boot"
fi

# Persistent journal: the file's own header says why (a brownout's evidence is
# otherwise in RAM). Installed unconditionally; it is capped at 200M and the way
# to undo it is to delete the file.
if [ -f "$HERE/journald-persistent.conf" ]; then
  mkdir -p /var/log/journal
  install -d -m 755 /etc/systemd/journald.conf.d
  install -m 644 -o root -g root "$HERE/journald-persistent.conf" \
          /etc/systemd/journald.conf.d/persistent.conf
  say "journal is now persistent (/etc/systemd/journald.conf.d/persistent.conf)"
  systemctl restart systemd-journald 2>/dev/null || \
    warn "could not restart systemd-journald; the journal becomes persistent on the next boot"
fi

if command -v systemctl >/dev/null 2>&1; then
  # daemon-reload unconditionally: the unit file was just replaced, and an
  # enable/disable decision that never reaches systemd is worse than a reload
  # nobody needed.
  systemctl daemon-reload 2>/dev/null || warn "systemctl daemon-reload failed"
  if [ "${RB_AUTOSTART:-1}" = "1" ]; then
    if systemctl enable rblive4.service >/dev/null 2>&1; then
      say "rblive4.service enabled -- the player starts at boot (RB_AUTOSTART=1)"
    else
      warn "could not enable rblive4.service; start it by hand with
  systemctl enable --now rblive4.service"
    fi
  else
    systemctl disable rblive4.service >/dev/null 2>&1 || true
    say "rblive4.service installed but DISABLED (RB_AUTOSTART=${RB_AUTOSTART:-1})"
    say "  it is still startable by hand: systemctl start rblive4"
  fi
fi

# --- verify the chroot can execute (the real ABI test) ----------------------

# This is the check that matters: uname tells you about the kernel, but what
# actually has to work is a soft-float EABI5 ARM32 binary running against the
# chroot's glibc 2.13. Running one answers it definitively. A failure here means
# the kernel lacks 32-bit emulation -- use Pi OS Lite 32-bit.
say "testing the chroot..."
if ! chroot "$CHROOT" /bin/busybox echo "  chroot executes: ok" 2>/tmp/rbpi4b-chroot-test.err; then
  cat /tmp/rbpi4b-chroot-test.err >&2 2>/dev/null || true
  rm -f /tmp/rbpi4b-chroot-test.err
  die "the chroot could not execute a 32-bit binary.
  The most likely cause is a 64-bit kernel with 32-bit emulation disabled.
  Install Pi OS Lite 32-bit (armhf) -- see docs/13-raspberrypi4.md.
  Do NOT retry with a 64-bit userland: every shim and the player itself are
  ARM32 soft-float, and there is no 64-bit build of rbp."
fi
rm -f /tmp/rbpi4b-chroot-test.err

# --- kernel interfaces rbp needs --------------------------------------------

if [ ! -e /dev/fb0 ]; then
  warn "/dev/fb0 is missing. Under vc4-kms-v3d it comes from DRM fbdev
  emulation. Check that dtoverlay=vc4-kms-v3d is enabled and that dmesg says
  'fb0: vc4drmfb frame buffer device'. Without it there is no display, and
  rbp will exit early. See docs/13-raspberrypi4.md."
fi

# Missing /dev/snd/seq is the classic silent "controls do nothing" failure: the
# sequencer module is not autoloaded on every boot.
if [ ! -e /dev/snd/seq ]; then
  say "/dev/snd/seq is missing; loading snd-seq"
  modprobe snd-seq 2>/dev/null || true
  if [ ! -e /dev/snd/seq ]; then
    warn "/dev/snd/seq is STILL missing after modprobe snd-seq. rbp will start
  but no controller input will reach it. Check that the kernel has
  CONFIG_SND_SEQUENCER=y (or loadable) and that /dev/snd exists."
  fi
fi

if [ ! -d /dev/input ] || [ -z "$(ls /dev/input 2>/dev/null)" ]; then
  warn '/dev/input is empty: no keyboard, mouse or touchscreen. The keyboard map
  still drives playback without one, but pointing needs a device. Run
  tools/evdevdump --list once something is attached to find its node.'
fi

# --- boot trim --------------------------------------------------------------
#
# The first thing in this tree to edit /boot/firmware rather than print a recipe
# for someone to paste, and the place the boot-time changes are carried. It sits
# after the kernel-interface checks above on purpose: an operator should see a
# missing /dev/fb0 or /dev/snd/seq complaint BEFORE the trim reports success.
#
# Not fatal, and never has been: this follows the journald block's precedent of
# applying what it can and warning rather than dying, because a target that
# cannot be trimmed is still a target with a working player.
if [ -f "$HERE/boot-trim.sh" ] && [ "${RB_BOOT_TRIM:-1}" = "1" ]; then
  sh "$HERE/boot-trim.sh" apply || warn "boot trim did not complete; see its output"
elif [ ! -f "$HERE/boot-trim.sh" ]; then
  warn "boot-trim.sh is not in $HERE -- the boot-time trim was NOT applied"
else
  say "boot trim skipped (RB_BOOT_TRIM=${RB_BOOT_TRIM:-1})"
fi

# --- finish -----------------------------------------------------------------

say "running fix-dev.sh"
if [ -x "$DEPLOY/fix-dev.sh" ] || [ -f "$DEPLOY/fix-dev.sh" ]; then
  RB_DEPLOY_ROOT="$DEPLOY" RB_CONF_FILE="$RB_CONF_FILE" sh "$DEPLOY/fix-dev.sh" || \
    warn "fix-dev.sh reported a problem (see above)"
else
  warn "fix-dev.sh not found; the device binds and stubs are NOT in place and
  rbp will not start. Re-run this installer with scripts/device/ present."
fi

echo
say "installed."
echo
echo "  chroot:   $CHROOT"
echo "  config:   $RB_CONF_FILE"
echo "  overrides: $DEPLOY/rb.local.conf  (survives reinstalls; see the end of rb.conf)"
echo "  logs:     $LOG_DIR"
echo
# Single-quoted so nothing here is interpreted: the point is to print a command
# the operator can paste, not to run it.
cat <<'EOF'
The boot-time trim was applied above (unless RB_BOOT_TRIM=0). It is idempotent
and reversible, and it reports what it did rather than asking you to paste it:

  sh /opt/rblive4/boot-trim.sh status            # is each item applied, and the evidence
  sh /opt/rblive4/boot-trim.sh report            # the boot measurement, one boot, monotonic
  sh /opt/rblive4/boot-trim.sh revert [items]    # undo; each item is its own inverse
  items: services cloudinit apt unit bootfiles

`status bootfiles` is where a deliberate gap is named rather than closed: this
installer ships the video= token for HDMI-A-1 only. The token for HDMI-A-2 (the
other micro-HDMI port) is S10.8's precondition in docs/13-raspberrypi4.md, not a
boot-time trim -- forcing a mode on a port with nothing attached is not free, and
nothing here adds it for you.

One reboot is what makes the ordering and the cloud-init changes real. Before you
reboot, note the measurement you are comparing against -- the boot still running
is the OLD configuration, which is what makes it the right baseline:

  sh /opt/rblive4/boot-trim.sh report > /root/boot-before.txt

That overwrites a file of that name if one is already there. On a unit that has
one, keep it: a re-run of this installer would replace the original baseline with
a trimmed boot's numbers, and the comparison then reads as "nothing changed".

After the reboot the player starts on its own. Day to day:

  systemctl status rblive4      # is it up, and what is it doing
  systemctl restart rblive4     # re-launch (also the way to pick up a new build)
  systemctl stop rblive4        # stop, and release the media mounts
  systemctl start getty@tty1    # stop the player and get the local console back

To run it by hand instead (a target without the unit enabled):

  sh /opt/rblive4/start-rb.sh

When something looks wrong, check the unit without changing anything. It reports
each shim's deployed build against the copy rbp is actually loading, which is the
one question this tree has most often got wrong:

  sh /opt/rblive4/doctor.sh        # exit 0 = nothing failed; prints one paste-ready fix
EOF
echo
echo "  (paths above assume RB_DEPLOY_ROOT=$DEPLOY)"
