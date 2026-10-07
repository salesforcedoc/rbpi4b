#!/bin/sh
# uninstall.sh — take rbpi4b back off a Raspberry Pi 4.
#
# Run as root on the Pi:
#
#   sh /opt/rblive4/uninstall.sh --dry-run     # print the whole plan, change nothing
#   sh /opt/rblive4/uninstall.sh               # do it (asks once when run on a tty)
#   sh /opt/rblive4/uninstall.sh --yes         # do it without asking (scripted)
#
# The inverse of install.sh, and it has to be run in one particular ORDER, which
# is the whole reason it is a script and not a list of commands in a document:
#
#   1. stop the units          so nothing below is still holding a file open
#   2. boot-trim revert        BEFORE the deploy root goes -- it lives INSIDE it,
#                              and it is the only thing that knows how to put
#                              /boot/firmware and the disabled services back
#   3. unmount, and PROVE it   the deploy root has six mounts under it, one of
#                              which is the HOST's /dev; see the long note below
#   4. remove
#
# Step 3 is not a formality. On 2026-09-27 an unchecked `rm -rf "$CHROOT/dev"`
# behind a failed umount ran *through* the bind mount and deleted the host's
# device nodes -- /dev/null, /dev/ptmx, and the devpts instance's own ptmx --
# after which no SSH login could allocate a pty. That is the failure this script
# exists to make impossible, so it unmounts first and then REFUSES to delete a
# tree that still has anything mounted in it.
#
# What is deliberately NOT removed:
#
#   * the operator's USB media. /dev/sda1 is mounted twice under the deploy root
#     and must be unmounted to remove the tree, but it is unmounted only -- this
#     script never writes to, deletes from, or repairs that filesystem. It is
#     live production data.
#   * rb.local.conf, by default. It holds values measured on THIS unit and the
#     repository cannot restore it. It is copied to /root before the tree goes;
#     --purge removes it with everything else.
#
# RB_DEPLOY_ROOT overrides the tree:
#   sudo RB_DEPLOY_ROOT=/srv/rblive4 sh uninstall.sh

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
DEPLOY="${RB_DEPLOY_ROOT:-/opt/rblive4}"
KEEP_COPY="${RB_UNINSTALL_KEEP:-/root/rbpi4b-rb.local.conf}"

DRY_RUN=0
ASSUME_YES=0
PURGE=0

say()  { echo "uninstall: $*"; }
warn() { echo "uninstall: WARNING: $*" >&2; }
die()  { echo "uninstall: ERROR: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: uninstall.sh [--dry-run] [--yes] [--purge] [--help]

  (no flags)   ask once, then remove rbpi4b from this unit
  --dry-run    print every action and change nothing; run this first
  --yes        do not ask (for a scripted teardown)
  --purge      also delete rb.local.conf instead of copying it to /root

Environment:
  RB_DEPLOY_ROOT     the tree to remove            (default /opt/rblive4)
  RB_UNINSTALL_KEEP  where rb.local.conf is copied (default /root/rbpi4b-rb.local.conf)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1 ;;
        --yes|-y)  ASSUME_YES=1 ;;
        --purge)   PURGE=1 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
    shift
done

# --- dry-run-aware primitives ------------------------------------------------
#
# Every destructive act goes through one of these, so --dry-run cannot be
# half-implemented: a new rm added later without a wrapper is a visible bug
# rather than a silent one.

x_sh()   { if [ "$DRY_RUN" = 1 ]; then say "would run: $*"; else "$@"; fi; }
x_umount() { if [ "$DRY_RUN" = 1 ]; then say "would unmount $1"; else umount "$1"; fi; }
x_rm()   { if [ "$DRY_RUN" = 1 ]; then say "would remove: $*"; else rm -rf "$@"; fi; }

# systemctl calls are all best-effort: on a half-installed unit a unit may not
# exist, and a teardown that dies because it could not stop something that was
# never started is a teardown that leaves the machine half-done.
x_systemctl() {
    if [ "$DRY_RUN" = 1 ]; then
        say "would run: systemctl $*"
    else
        systemctl "$@" 2>/dev/null || true
    fi
}

# --- preconditions -----------------------------------------------------------

[ "$(id -u)" = "0" ] || die "must run as root (systemctl, umount, /boot/firmware)"

if [ ! -d "$DEPLOY" ]; then
    warn "$DEPLOY does not exist. Nothing to remove."
    warn "If rbpi4b was installed somewhere else, set RB_DEPLOY_ROOT."
    # Still worth clearing the unit: a deploy root can be deleted by hand and
    # leave systemd pointing at a script that is no longer there, which is a
    # unit that fails at every boot and says so only in the journal.
fi

# --- the plan, and one confirmation ------------------------------------------

_mounts_under() {
    # Every mountpoint at $DEPLOY or beneath it, deepest first. /proc/mounts
    # escapes a space in a path as \040; unescape before matching, or a deploy
    # root under a path with a space would silently match nothing and the mount
    # gate below would pass while mounts were still in place.
    sed 's/\\040/ /g' /proc/mounts 2>/dev/null |
        awk -v r="$DEPLOY" '$2 == r || substr($2, 1, length(r) + 1) == r "/" { print $2 }' |
        awk '{ print length($0) "\t" $0 }' | sort -rn | cut -f2- | grep -v '^$' || true
}

MOUNTS_NOW=$(_mounts_under | wc -l | tr -d ' ')

echo
say "this will remove rbpi4b from this unit:"
echo "    deploy root   $DEPLOY   ($MOUNTS_NOW mount(s) under it will be unmounted first)"
echo "    units         rblive4.service, healthwatch.service (stopped and disabled)"
echo "    unit files    /etc/systemd/system/{rblive4,healthwatch}.service"
echo "    state         /var/lib/rblive4"
echo "    journal drop-in  /etc/systemd/journald.conf.d/persistent.conf"
echo "    boot changes  reverted by boot-trim.sh revert (services, cloudinit, apt, unit, bootfiles)"
if [ "$PURGE" = 1 ]; then
    echo "    config        rb.local.conf DELETED (--purge)"
else
    echo "    config        rb.local.conf preserved at $KEEP_COPY"
fi
echo
say "NOT touched: your USB media. It is unmounted, never written to or repaired."
echo

if [ "$DRY_RUN" = 1 ]; then
    say "dry run -- nothing below will actually happen."
elif [ "$ASSUME_YES" != 1 ]; then
    if [ -t 0 ]; then
        printf 'uninstall: type "yes" to proceed: '
        read -r _answer
        [ "$_answer" = "yes" ] || die "not confirmed; nothing was changed"
    else
        die "refusing to run unattended without --yes (run --dry-run to see the plan)"
    fi
fi

# --- 1. stop the units -------------------------------------------------------
#
# healthwatch first: it is the watchdog that records why the unit wedges, and
# stopping the player under it would otherwise be logged as a wedge.
#
# `systemctl stop`, and never a pkill or a `kill` on a matched command line. The
# launcher's own cleanup() matches every /proc/*/cmdline on EVERY restart, so a
# teardown that kills the player by pattern can kill an unrelated process -- ssh
# sessions included -- and the unit's cgroup is the only thing that knows which
# processes are actually rbp's.

say "stopping the units"
x_systemctl stop healthwatch.service
x_systemctl stop rblive4.service
x_systemctl disable healthwatch.service
x_systemctl disable rblive4.service

# --- 2. boot trim, BEFORE anything is deleted --------------------------------
#
# boot-trim.sh keeps its state in /var/lib/rblive4 and its backup files beside
# the files it edited, so it can undo precisely. It knows how to restore the
# services it disabled and the /boot/firmware edits it made; this script does
# not, and must not guess. If it is already gone the revert is impossible --
# which is why this runs second and not last.

if [ -f "$DEPLOY/boot-trim.sh" ]; then
    say "reverting the boot trim (services, cloudinit, apt, unit, bootfiles)"
    x_sh sh "$DEPLOY/boot-trim.sh" revert || \
        warn "boot-trim.sh revert did not complete; /boot/firmware may still carry
  the trim. Re-run it by hand before the deploy root is deleted:
    sh $DEPLOY/boot-trim.sh revert"
else
    warn "$DEPLOY/boot-trim.sh is missing, so the boot-time changes could not be
  reverted automatically. /boot/firmware may still carry the trimmed cmdline and
  config, and the services the trim disabled are still disabled. The backups are
  named <file>.rblive4.bak -- restore them by hand:
    for f in /boot/firmware/cmdline.txt /boot/firmware/config.txt; do
        [ -f \"\$f.rblive4.bak\" ] && cp \"\$f.rblive4.bak\" \"\$f\"
    done"
fi

# --- 3. unmount, and PROVE it ------------------------------------------------
#
# THE PART THAT MATTERS. install.sh leaves six mounts under the deploy root:
#
#   rbx3-run/dev        devtmpfs, i.e. the HOST's /dev, bound in by fix-dev.sh
#   rbx3-run/proc       proc,   the host's
#   rbx3-run/sys        sysfs,  the host's
#   rbx3-run/tmp        tmpfs,  a ~1.9 GB RAM disk
#   media/usb1/sda1     the operator's USB stick
#   rbx3-run/media/usb1/sda1   the same stick, seen from inside the chroot
#
# An `rm -rf $DEPLOY` with any of those still mounted does not delete the mount
# point -- it descends through it and deletes what is on the OTHER SIDE. For
# /dev that is the host's device nodes; for the media mounts it is the
# operator's production data. So: unmount deepest-first, then re-read
# /proc/mounts and refuse to continue while anything is left.

say "unmounting everything under $DEPLOY"
# while-read, not `for m in $(...)`: a word-split loop would break on a path
# containing a space -- which is exactly the case the \040 unescaping above
# exists to handle, so splitting here would undo it.
_mounts_under | while IFS= read -r m; do
    [ -n "$m" ] || continue
    case "$m" in
        */media/usb1/sda1|*/media/usb2/sda1)
            say "  unmounting $m (your USB media -- the stick is released, not written to)" ;;
        */rbx3-run/dev)
            say "  unmounting $m (the host's /dev, bound into the chroot)" ;;
        *)
            say "  unmounting $m" ;;
    esac
    x_umount "$m" || warn "could not unmount $m"
done

if [ "$DRY_RUN" != 1 ]; then
    STILL=$(_mounts_under)
    if [ -n "$STILL" ]; then
        echo "$STILL" | sed 's/^/    /' >&2
        die "mounts are STILL present under $DEPLOY, so nothing has been deleted.
  Something is holding them open. The usual cause is rbp or a helper still
  running, or a shell sitting inside the chroot. Check for both and retry:
    systemctl status rblive4
    mount | grep $DEPLOY
  To proceed without this check is exactly how the host's /dev was wiped once
  before. This script will not do it."
    fi
    say "mount table clear: nothing is mounted under $DEPLOY"
fi

# --- 4. keep the local config ------------------------------------------------
#
# rb.local.conf is the one thing here the repository cannot reconstruct: values
# measured on this unit, including the ones the operator set by hand. It is
# absent from the tarball precisely so no deploy can overwrite it, and a
# teardown that deletes it silently discards that work.

if [ "$PURGE" = 1 ]; then
    say "rb.local.conf will be deleted (--purge)"
elif [ -f "$DEPLOY/rb.local.conf" ]; then
    say "preserving $DEPLOY/rb.local.conf -> $KEEP_COPY"
    if [ "$DRY_RUN" = 1 ]; then
        say "would copy rb.local.conf to $KEEP_COPY"
    else
        cp "$DEPLOY/rb.local.conf" "$KEEP_COPY" || \
            warn "could not copy rb.local.conf to $KEEP_COPY -- copy it by hand before continuing"
        say "  (put it back with: cp $KEEP_COPY <deploy root>/rb.local.conf)"
    fi
fi

# --- 5. unit files and the journal drop-in -----------------------------------

say "removing the unit files"
x_rm /etc/systemd/system/rblive4.service
x_rm /etc/systemd/system/healthwatch.service
# boot-trim's `revert unit` drop-in, if a revert left it behind.
x_rm /etc/systemd/system/rblive4.service.d
x_systemctl daemon-reload
x_systemctl reset-failed

# The persistent-journal drop-in. An earlier name for this file was
# 99-persistent.conf and some units carry both, byte for byte identical, from a
# deploy that predates the rename. Remove the twin only when it really is one --
# an unrelated drop-in that happens to sort first must not be deleted.
#
# The comparison is against the copy we SHIP, not against the file removed on the
# line above, so --dry-run reaches the same verdict as a real run.
JD=/etc/systemd/journald.conf.d
JD_OURS="$HERE/journald-persistent.conf"
if [ ! -f "$JD_OURS" ] && [ -f "$DEPLOY/journald-persistent.conf" ]; then
    JD_OURS="$DEPLOY/journald-persistent.conf"
fi
if [ -d "$JD" ]; then
    say "removing the persistent-journal drop-in"
    x_rm "$JD/persistent.conf"
    if [ -f "$JD/99-persistent.conf" ]; then
        if [ ! -f "$JD_OURS" ]; then
            # "differs from ours" and "we cannot tell" are different answers and
            # must not read as the same one -- the first is a judgement, the
            # second is a gap in this script's reach.
            warn "no copy of the drop-in to compare against (looked for
  $HERE/journald-persistent.conf and $DEPLOY/journald-persistent.conf), so
  $JD/99-persistent.conf cannot be identified either way -- leaving it. It sorts
  AFTER persistent.conf, so journald reads it INSTEAD."
        elif cmp -s "$JD_OURS" "$JD/99-persistent.conf"; then
            say "removing $JD/99-persistent.conf (the same file under its old name)"
            x_rm "$JD/99-persistent.conf"
        else
            warn "$JD/99-persistent.conf differs from the copy we ship, so it is not
  ours to judge -- leaving it alone."
        fi
    fi
    # Only ever succeeds when the directory is empty, and we are what created it.
    if [ "$DRY_RUN" = 1 ]; then
        say "would remove the directory $JD if it is empty by then"
    else
        rmdir "$JD" 2>/dev/null || true
    fi
    x_systemctl restart systemd-journald
fi

# --- 6. unit-side state ------------------------------------------------------

# boot-trim's record of what it applied. Nothing else reads it, and a stale one
# on a machine that no longer has the trim is misleading.
say "removing /var/lib/rblive4"
x_rm /var/lib/rblive4

# --- 7. the deploy root, now that nothing is mounted in it ------------------

if [ -d "$DEPLOY" ]; then
    say "removing $DEPLOY"
    x_rm "$DEPLOY"
else
    say "$DEPLOY is already gone"
fi

# --- 8. host-side leftovers fix-dev.sh made ----------------------------------
#
# The udev FIFOs live in the HOST's /tmp (the chroot's /tmp is a bind of it) and
# are recreated by fix-dev.sh on every start, so they are the one stub that
# outlives the tree. /dev/mem was locked to mode 000 as a precaution against
# rbp's i.MX6 mappings reaching real hardware; restore the kernel's own mode.

say "clearing host-side leftovers"
for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
    if [ -p "/tmp/$f" ]; then x_rm "/tmp/$f"; fi
done
if [ "$DRY_RUN" != 1 ] && [ -e /dev/mem ]; then
    chmod 640 /dev/mem 2>/dev/null || true
fi

# --- finish -----------------------------------------------------------------

echo
say "done."
echo
cat <<EOF
  Removed:  $DEPLOY, /var/lib/rblive4, the units and their unit files.
  Reverted: the boot trim (its own backup files were used and are consumed by
            the revert -- /boot/firmware/<file>.rblive4.bak).
  Kept:     $KEEP_COPY
EOF
if [ "$PURGE" = 1 ]; then
    echo "            (nothing -- --purge was given)"
fi
cat <<'EOF'

Two things a teardown cannot undo, both worth checking by hand:

  * /boot/firmware/cmdline.txt and config.txt. `boot-trim.sh revert bootfiles`
    restores them from their .rblive4.bak copies, and refuses if a file has been
    edited since -- which is the right behaviour and means a refusal left the
    file trimmed. Read them:
      grep -n 'rbpi4b\|video=' /boot/firmware/cmdline.txt /boot/firmware/config.txt

  * the services the trim disabled (a display manager, cloud-init, getty@tty1).
    `revert services` re-enables exactly what apply recorded and nothing else,
    so if the record was missing they stay disabled. Check with:
      systemctl is-enabled getty@tty1 lightdm cloud-init

A reboot is the honest test: nothing of rbpi4b should start, and the local
console (or the display manager that was there before) should come back.
EOF
[ "$DRY_RUN" = 1 ] && echo "
uninstall: that was a DRY RUN. Re-run without --dry-run to do it."
echo
