#!/bin/sh
# lib.sh — shared pieces of the rblive4 device scripts.
#
# Sourced by fix-dev.sh, start-rb.sh and usb-watch.sh. Worth a file of its own
# because two things were previously duplicated in all three, in slightly
# different forms — which is how this repo ended up with two subtly different
# copies of the same dangerous `ps w | awk` process match:
#
#   1. locating and sourcing rb.conf, so the three scripts cannot disagree about
#      where the deploy root or the chroot is;
#   2. finding a process by command line, via /proc instead of ps.
#
# POSIX sh only — no bashisms, no `local` — because the launcher and the watcher
# run under the device's /bin/sh (dash on Pi OS).

# --- configuration ----------------------------------------------------------

# The schema these scripts understand. It is one change with rb.conf's own
# RB_CONF_VERSION, not two: bump this line and that variable together, because a
# guard that expects a version the shipped conf does not declare fails closed —
# every one of these scripts exits before doing anything, which is correct but
# looks like "the launcher is broken".
RB_CONF_SCHEMA=2

# Locate rb.conf, source it, and refuse to continue if it is missing or is a
# different schema. Dying here is deliberate: every path in these scripts comes
# from rb.conf, so continuing with the defaults would silently operate on the
# wrong tree — a failure that surfaces later as "mount worked but rbp sees
# nothing", which is far more expensive to diagnose than an error now.
rb_load_conf() {
    if [ -z "${RB_CONF_FILE:-}" ]; then
        for c in "$(dirname "$0")/rb.conf" "$(dirname "$0")/../rb.conf"; do
            if [ -f "$c" ]; then RB_CONF_FILE="$c"; break; fi
        done
    fi
    if [ -z "${RB_CONF_FILE:-}" ] || [ ! -f "$RB_CONF_FILE" ]; then
        echo "$0: cannot find rb.conf." >&2
        echo "  Looked next to this script ($(dirname "$0")/rb.conf), one level" >&2
        echo "  up, and at \$RB_CONF_FILE=${RB_CONF_FILE:-<unset>}." >&2
        echo "  On the Pi it ships at the deploy root, beside these scripts;" >&2
        echo "  re-run install.sh if it is missing." >&2
        exit 1
    fi

    . "$RB_CONF_FILE"

    if [ "${RB_CONF_VERSION:-0}" != "$RB_CONF_SCHEMA" ]; then
        echo "$0: $RB_CONF_FILE is schema v${RB_CONF_VERSION:-?}, expected v$RB_CONF_SCHEMA." >&2
        echo "  A stale rb.conf and a newer script disagree about variable names," >&2
        echo "  which would otherwise show up later as a wrong path, not an error." >&2
        exit 1
    fi

    # rb.conf sets all of these itself; repeat the two the scripts use for every
    # path, so a hand-edited config cannot leave RB_CHROOT empty and make a
    # `mount --bind` target the filesystem root.
    RB_DEPLOY_ROOT="${RB_DEPLOY_ROOT:-/opt/rblive4}"
    RB_CHROOT="${RB_CHROOT:-$RB_DEPLOY_ROOT/rbx3-run}"
}

rb_require_root() {
    [ "$(id -u)" = "0" ] || {
        echo "$0: must run as root (mount, mknod/mkfifo and chmod 000 /dev/mem)" >&2
        exit 1
    }
}

# Sanity-check RB_CHROOT before anything destructive uses it.
#
# fix-dev.sh runs `rm -rf "$RB_CHROOT/dev"` and then mounts on top of it. With an
# empty or relative RB_CHROOT that statement is `rm -rf /dev` — so the check is
# not tidiness, it is the difference between a boot-time script and a wiped
# device tree. Require an absolute path of at least two components that exists.
rb_require_chroot() {
    case "$RB_CHROOT" in
        /*/*) ;;
        *)  echo "$0: refusing to operate on RB_CHROOT='$RB_CHROOT'" >&2
            echo "  (not an absolute path with a directory component)." >&2
            echo "  Check RB_CHROOT in $RB_CONF_FILE." >&2
            exit 1 ;;
    esac
    if [ ! -d "$RB_CHROOT" ]; then
        echo "$0: RB_CHROOT='$RB_CHROOT' does not exist." >&2
        echo "  Run install.sh first, or correct RB_CHROOT in $RB_CONF_FILE." >&2
        exit 1
    fi
}

# --- process lookup ---------------------------------------------------------

# pids_matching PATTERN [EXCLUDE]
#
# Print the PID of every process whose command line contains PATTERN, skipping
# this shell and anything whose command line also contains EXCLUDE. PATTERN and
# EXCLUDE are shell glob patterns, not regexes.
#
# Why not `ps w | awk`, which this replaces: busybox ps and procps ps truncate
# the command line at different lengths, so one pattern silently matched on one
# target and not the other — and a pattern loose enough to survive that is loose
# enough to match the awk process doing the matching, i.e. to kill the shell it
# was launched from. docs/11 records exactly that failure. /proc/<pid>/cmdline is
# the kernel's own NUL-separated argv: complete, unambiguous, no quoting.
pids_matching() {
    _pat=$1
    _excl=${2:-}
    for _c in /proc/[0-9]*/cmdline; do
        [ -r "$_c" ] || continue
        _pid=${_c#/proc/}
        _pid=${_pid%/cmdline}
        [ "$_pid" = "$$" ] && continue
        # NULs become spaces so one case pattern can match the whole argv.
        # Failure here means the process exited mid-scan; that is not an error.
        _cmd=$(tr '\0' ' ' < "$_c" 2>/dev/null) || continue
        case "$_cmd" in *"$_pat"*) ;; *) continue ;; esac
        if [ -n "$_excl" ]; then
            case "$_cmd" in *"$_excl"*) continue ;; esac
        fi
        echo "$_pid"
    done
}
