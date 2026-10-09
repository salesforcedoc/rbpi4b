#!/bin/sh
# lib.sh — shared pieces of the rbpi4b device scripts.
#
# Sourced by fix-dev.sh, start-rb.sh, usb-watch.sh and display-watch.sh. Worth a
# file of its own
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
# Every path these scripts touch is derived from RB_CHROOT, and fix-dev.sh
# `mount --bind`s over $RB_CHROOT/dev. With an empty or relative RB_CHROOT that
# target is /dev itself -- and fix-dev.sh used to `rm -rf` it first, which on
# 2026-09-27 emptied a live unit's device tree when the umount in front of it
# failed (that rm -rf is gone; the comment at the line records what it did). The
# check is not tidiness: this whole tree is one bad variable away from the
# filesystem root. Require an absolute path of at least two components that
# exists.
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

# --- boot progress ----------------------------------------------------------

# rb_bootmsg MESSAGE [MESSAGE...]
#
# Append one timestamped line to the boot-progress log the early screen tails
# (bootscreen.py, started by rblive4-boot.service). It writes ONLY to that file,
# never to stdout: the launcher's own `echo` lines already give the journal
# record, so repeating them here would double every line in a report.
#
# A REGULAR FILE, never a FIFO, and that is the whole reason this is a function
# rather than a one-liner at each call site. Opening a reader-less FIFO for write
# BLOCKS in open() until a reader appears — usb-watch.sh's notify() guards against
# exactly that with `[ -p ]` + `timeout 3` — and during boot the reader (the
# screen) may not be up yet, so a blocking write here would stall the launcher.
# An O_APPEND write to a regular file never blocks, never loses the first line,
# and needs no reader.
#
# The stamp is /proc/uptime's first field — seconds since boot, monotonic and
# boot-relative, matching boot-trim.sh's milestones — NOT the wall clock: the wall
# clock is not set yet this early, and a 32-bit `long` CLOCK_MONOTONIC in ns wraps
# every 4.3 s (docs/13's long-clock note).
#
# NEVER fatal. A missing directory or a read-only /run is a silent no-op; the
# screen is a diagnostic, and a diagnostic must not be able to stop the player.
# The `mkdir -p` is defensive and closes a real race: nothing creates /run/rblive4
# at boot (only vnc-run.sh does, and only when RB_VNC=1), and the gap between
# basic.target and the launcher is 70 ms — so the earliest calls could otherwise
# run before any directory exists. install.sh also ships a tmpfiles.d entry that
# makes the directory before either writer.
rb_bootmsg() {
    _t=$(cut -d' ' -f1 /proc/uptime 2>/dev/null) || _t=
    [ -n "$_t" ] || _t=0
    _bl=${RB_BOOT_LOG:-/run/rblive4/boot.log}
    mkdir -p "${_bl%/*}" 2>/dev/null
    printf '%s  %s\n' "$_t" "$*" >>"$_bl" 2>/dev/null || :
}
