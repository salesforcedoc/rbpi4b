#!/bin/sh
# display-watch.sh — a monitor swap -> a restart of rbp, on the Raspberry Pi 4.
#
# rbp renders a logical 1280x800 surface; the patched DirectFB fbdev driver is
# what puts that surface on whatever panel is attached. Both read the panel's
# REAL geometry exactly once — the driver when it opens the framebuffer, and
# fb_cursor.c when it mmaps the page — and neither can be made to look again:
#
#   - the driver caches xres/yres/stride/bpp in system_initialize() and builds
#     its whole present path (and its buffer-mode choice) out of them;
#   - fb_cursor.c holds the mapping for the life of the process.
#
# So when the panel changes — a different monitor, a different mode after a
# replug, or a framebuffer the kernel tore down and rebuilt — rbp is still
# drawing at the geometry it was launched with. On a smaller fb that is a
# refused region test and NO UI AT ALL (the signature docs/12 records); on a
# larger one it is a 2560-byte stride into a 3840-byte page, i.e. a diagonal
# smear, every frame. Nothing in the stack notices.
#
# This does. It watches the framebuffer's geometry and, when it stops matching
# the one rbp was launched with, asks systemd to restart the unit. The restart
# is the FIX and not a workaround: re-reading the geometry in place would mean
# the driver and the cursor each growing a re-open path through code that
# already has a working answer, on the one thread (rbp's, at SCHED_FIFO 98)
# where a mistake is a frozen UI.
#
# What it deliberately does NOT do:
#
#   - It does not fire on a same-geometry unplug/replug. That is the operator's
#     rule — pulling the monitor and putting it back must be seamless — and it
#     is the common case, because a `video=` mode on the kernel cmdline is
#     re-added on every hotplug re-probe and validated against the CRTC rather
#     than the sink's EDID list (docs/13). RB_DISPLAY_RESTART_ON_RECONNECT=1
#     buys the one case that rule can leave black: a fbdev recreated at an
#     identical geometry, which rbp's held mapping no longer points at.
#   - It does not fold the connector's status into the string it compares. The
#     connector goes disconnected->connected on every replug, so including it
#     would make every replug a restart — the exact thing the rule forbids. The
#     connector state is logged alongside the decision, never compared.
#   - It does not require the chroot, unlike usb-watch.sh. This is the tool you
#     reach for when the player is already broken, so refusing to run because
#     RB_CHROOT is missing would refuse in the one case it exists for.
#   - It does not check that rbp came back. systemd's Restart=always and the
#     launcher own that; a second watcher for the same thing is how two things
#     end up fighting over one unit.
#
# It is a CHILD of start-rb.sh, not a unit of its own. A unit would have to
# handle the no-framebuffer-at-boot and missing-chroot cases itself, and would
# outlive the only process whose geometry it compares against. As a child it is
# killed by the very restart it asks for, which is fine — the evidence line is
# written first. What a unit would have bought, a counter that bounds a
# flapping monitor, is kept in a file instead, so it survives the restart too.
#
# Usage:  sh display-watch.sh start|stop|status|run|baseline [--dry-run]
# Env:    DISPLAYWATCH_POLL=1  poll interval seconds (RB_DISPLAY_POLL_S)
#         RB_SYSFS_ROOT=/sys   the tree the geometry and the connectors are
#                              read from; a drill points it at a fake tree
#         --dry-run            with start|run: log the decision, never restart

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf

FB_DEV="${RB_FB_DEV:-/dev/fb0}"
FBNAME=${FB_DEV##*/}
SYS="${RB_SYSFS_ROOT:-/sys}"
LOG="${RB_LOG_DIR:-$RB_DEPLOY_ROOT/log}/displaywatch.log"
PIDFILE=/tmp/displaywatch.pid
# The geometry rbp was launched with, written by start-rb.sh immediately before
# the launch and read back here as the thing to compare against. An absent file
# means "no baseline" and never fires — which is what makes a hand-started
# watcher safe, and is what S10.6 sets up on purpose.
BASEFILE=/tmp/displaywatch.geom
# `<count> <epoch> <boot id>`, in a FILE rather than in a variable. The restarts
# this bounds are the restarts that kill this process, so a counter held in
# memory would reset at exactly the moment it is needed.
FIRES=/tmp/displaywatch.fires
POLL="${DISPLAYWATCH_POLL:-${RB_DISPLAY_POLL_S:-2}}"

# The two numbers that make a flapping monitor cost something bounded. Between
# them a flap is capped at roughly one restart per 25 s (this cooldown, plus
# systemd's RestartSec=10, plus the launcher's ~15 s bring-up) and at FIRE_MAX
# per boot, after which the watcher says so in the log and stops asking. This
# cap is the property that matters more than the feature.
FIRE_COOLDOWN_S=30
FIRE_MAX=5

mkdir -p "$(dirname "$LOG")" 2>/dev/null
log() { echo "$(date '+%F %T') $$ $*" >> "$LOG" 2>/dev/null; }

# --- what the panel is, as one comparable string -----------------------------
#
# `<virtual_size> <stride> <bits_per_pixel>` — the three leaves tools/fbdump.c
# reads and the three docs/13 records for this kernel. Read with the `read`
# builtin instead of $(cat ...): this runs on every poll for the life of the
# session, and a fork per leaf per second for a value that changes twice a
# session is not worth paying for.
#
# `virtual_size` is what the comparison is really made of — see fb_present()
# below — but all three belong in the string, because a stride that changed
# while the size did not is a different present path and a stale mapping.
#
# A missing leaf becomes `-` rather than aborting the read, so the string stays
# comparable and the direction of the mistake is safe: a fb whose `stride` leaf
# has vanished is a fb being torn down, and a geometry that differs from the
# baseline makes this restart rather than hold a mapping of a going-away device.
geometry() {
    g=""
    for leaf in virtual_size stride bits_per_pixel; do
        v=""
        f="$SYS/class/graphics/$FBNAME/$leaf"
        [ -r "$f" ] && read -r v < "$f"
        g="$g ${v:--}"
    done
    echo "${g# }"
}

# The leaf the geometry is actually made of, i.e. "is there a framebuffer to
# describe at all". A missing virtual_size is the fb-disappeared case and not a
# geometry change, and the two are handled differently: the first fires nothing,
# the second fires when it comes back (see run()).
fb_present() { [ -r "$SYS/class/graphics/$FBNAME/virtual_size" ]; }

# --- what is plugged in; for the log line only -------------------------------
#
# `card*`, not `card0`: on this unit vc4 is card1 and card0 is v3d (docs/13),
# so a card0 glob finds nothing on the one machine this runs on. If nothing
# matches, the glob stays literal, `[ -r ]` is false, and this returns "" —
# which is why the callers interpolate it rather than testing it.
#
# A connector line that is empty is therefore also the tell for "the DRM tree
# is not where we think it is", and it is the reason this is logged rather than
# stored: the geometry is what decides, and this is what explains the decision
# to whoever reads the log afterwards.
connectors() {
    s=""
    for f in "$SYS"/class/drm/card*-HDMI-A-*/status; do
        [ -r "$f" ] || continue
        v=""
        read -r v < "$f"
        s="$s $(basename "$(dirname "$f")")=$v"
    done
    echo "${s# }"
}

# Same match usb-watch.sh uses: rbp is started through the chroot's loader, so
# `comm` is ld-linux.so.3 and a name match has to look at argv. The `strace`
# exclude keeps an strace wrapper from being taken for the player itself.
rbp_pid() { pids_matching "$RB_PLAYER" strace | head -1; }

baseline() {
    _b=""
    [ -f "$BASEFILE" ] && read -r _b < "$BASEFILE"
    echo "$_b"
}

# Write the baseline. Called by start-rb.sh, which is the whole reason this is a
# verb and not three reads inlined there: the string written here is the string
# the watcher compares against, and two implementations of one format would be
# free to disagree — which shows up as a restart loop and nothing else.
baseline_cmd() {
    _g=$(geometry)
    printf '%s\n' "$_g" > "$BASEFILE"
    log "baseline: '${_g}' (fb ${FB_DEV})"
    echo "$_g"
}

# --- the one destructive thing this does -------------------------------------
#
# The order is load-bearing and not stylistic. The cap, the cooldown, the rbp
# check and the evidence line all come first, and the systemctl call last,
# because this process lives in the unit's own cgroup: the restart it is asking
# for is the restart that kills it, so anything not written before that call is
# never written at all.
#
# Every refusal is logged with its reason, because a silent refusal here is
# indistinguishable from a watcher that is not running — the two look the same
# from outside and want different fixes.
fire() {
    _why=$1
    _epoch=$(date +%s)
    _boot=""
    [ -r /proc/sys/kernel/random/boot_id ] && read -r _boot < /proc/sys/kernel/random/boot_id
    _count=0
    _last=0
    _prev_boot=""
    [ -f "$FIRES" ] && read -r _count _last _prev_boot < "$FIRES"
    case "$_count" in ''|*[!0-9]*) _count=0 ;; esac
    case "$_last"  in ''|*[!0-9]*) _last=0  ;; esac

    # The cap is per BOOT, and this is what makes that true rather than a hope
    # that /tmp happens to be a tmpfs. start-rb.sh cannot clear this file:
    # under Restart=always it runs once per RESTART, so it would reset the very
    # counter that exists to bound them. A file that does not name the boot it
    # was written in keeps its count instead — we cannot tell whether the boot
    # changed, so we do not forget.
    if [ -n "$_boot" ] && [ -n "$_prev_boot" ] && [ "$_prev_boot" != "$_boot" ]; then
        log "boot changed ($_prev_boot -> $_boot): the restart counter resets"
        _count=0
        _last=0
    fi

    if [ "$_count" -ge "$FIRE_MAX" ]; then
        log "REFUSE: $_why — but this would be restart $((_count + 1)) and the cap is $FIRE_MAX."
        log "  The picture is stale for good; restart rblive4 by hand, or reboot."
        return 1
    fi
    if [ "$_last" -gt 0 ] && [ "$((_epoch - _last))" -lt "$FIRE_COOLDOWN_S" ]; then
        log "REFUSE: $_why — $((_epoch - _last))s after the last restart, cooldown ${FIRE_COOLDOWN_S}s."
        return 1
    fi

    _rbp=$(rbp_pid)
    if [ -z "$_rbp" ]; then
        log "REFUSE: $_why — rbp is not running (systemd Restart=always is already on it)."
        return 1
    fi

    log "RESTART $((_count + 1))/$FIRE_MAX: $_why (rbp pid $_rbp; connectors: $(connectors))"
    if [ "$DRY_RUN" = 1 ]; then
        log "  --dry-run: decided to restart, did not."
        return 0
    fi

    # Claimed before the call, not after: the restart can land between the two.
    printf '%s %s %s\n' "$((_count + 1))" "$_epoch" "$_boot" > "$FIRES"
    # --no-block queues the job and returns, so this is never mid-call when the
    # restart takes the process down.
    systemctl --no-block restart rblive4
    return 0
}

run() {
    # Read once. A baseline appearing mid-session is a restart arriving, and
    # that restart is about to kill this process anyway.
    if [ -f "$BASEFILE" ]; then
        have_base=1
        base=$(baseline)
    else
        have_base=0
        base=""
    fi

    log "=== display-watch run: fb $FB_DEV ($FBNAME), poll ${POLL}s, sysfs $SYS ==="
    log "connectors: $(connectors)"
    if [ "$have_base" = 1 ]; then
        log "baseline: '${base}'"
    else
        log "no baseline in $BASEFILE — reporting geometry changes, never restarting."
        log "  start-rb.sh writes that file at launch; 'baseline' writes one by hand."
    fi

    prev=""
    absent=0
    returned=0

    while :; do
        if ! fb_present; then
            [ "$absent" = 1 ] || log "framebuffer $FB_DEV is gone (connectors: $(connectors))"
            absent=1
            prev=""
            returned=0
            sleep "$POLL"
            continue
        fi

        now=$(geometry)

        if [ "$absent" = 1 ]; then
            absent=0
            returned=1
            log "framebuffer $FB_DEV is back at '${now}' (connectors: $(connectors))"
        fi

        # The debounce, and the only reason it is here: one round of a flapping
        # monitor reads as a change, and it should be the geometry that SURVIVES
        # the flap that pays for a restart. So nothing is decided until two
        # consecutive polls have agreed.
        if [ "$now" != "$prev" ]; then
            prev="$now"
            sleep "$POLL"
            continue
        fi

        ret="$returned"
        returned=0

        if [ "$now" != "$base" ]; then
            if [ "$have_base" = 1 ]; then
                fire "geometry is '${now}', not the '${base}' rbp was launched with"
            else
                log "geometry is '${now}'"
            fi
        elif [ "$have_base" = 1 ] && [ "$ret" = 1 ] &&
             [ "${RB_DISPLAY_RESTART_ON_RECONNECT:-0}" = "1" ]; then
            fire "the framebuffer came back at the launch geometry '${now}', and RB_DISPLAY_RESTART_ON_RECONNECT=1"
        fi

        # In memory only, so that a mismatched panel costs ONE decision rather
        # than one per poll: the restart is on its way, or it was refused with
        # the reason and the remedy in the log, and repeating either every two
        # seconds until it lands would bury the reason under its own copies.
        # The FILE is not touched here — start-rb.sh writes it, and the next
        # launch is what rewrites it.
        base="$now"
        sleep "$POLL"
    done
}

start() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "already running (pid $(cat "$PIDFILE"))"
        return 0
    fi
    log "=== display-watch start ==="
    # $DRY_ARGS is either empty or exactly "--dry-run" — deliberately unquoted so
    # an empty value contributes no argument at all. A started dry-run watcher
    # is what S10.10 uses to watch the debounce and the cap without taking the
    # unit down.
    nohup sh "$0" run $DRY_ARGS >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    sleep 1
    echo "started pid $(cat "$PIDFILE")"
}

stop() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
        echo "stopped"
    else
        echo "not running"
    fi
}

status() {
    _rbp=$(rbp_pid)
    _pres="present"
    fb_present || _pres="GONE"
    echo "pid:          $(cat "$PIDFILE" 2>/dev/null || echo none)"
    echo "framebuffer:  $FB_DEV ($_pres) at $SYS/class/graphics/$FBNAME"
    echo "geometry now: $(geometry)"
    if [ -f "$BASEFILE" ]; then
        echo "baseline:     $(baseline)   ($BASEFILE)"
    else
        echo "baseline:     none — $BASEFILE is absent, so this watcher never fires"
    fi
    echo "connectors:   $(connectors)"
    if [ -f "$FIRES" ]; then
        echo "restarts:     $(cat "$FIRES")   (count, epoch, boot id; cooldown ${FIRE_COOLDOWN_S}s, cap $FIRE_MAX per boot)"
    else
        echo "restarts:     none this boot   (cooldown ${FIRE_COOLDOWN_S}s, cap $FIRE_MAX per boot)"
    fi
    echo "rbp pid:      ${_rbp:-none}"
    echo "--- log tail ---"
    # `|| true`: a missing log is not a failure of the status command, and
    # returning 1 there would make `status` look like it had broken.
    tail -15 "$LOG" 2>/dev/null || true
    return 0
}

# --- arguments ---------------------------------------------------------------
#
# A verb plus an optional --dry-run, in either order, rather than the bare
# `case "$1"` usb-watch.sh uses: the flag has to survive into the child that
# `start` forks, and parsing it once up here is what makes that one code path
# instead of two.
DRY_RUN=0
DRY_ARGS=""
CMD=""
for a in "$@"; do
    case "$a" in
      --dry-run) DRY_RUN=1; DRY_ARGS="--dry-run" ;;
      -*) echo "$0: unknown option '$a'" >&2; exit 1 ;;
      *) [ -n "$CMD" ] || CMD="$a" ;;
    esac
done

case "$CMD" in
  # status is read-only (it prints three leaves and a log tail) and is genuinely
  # useful to run unprivileged; everything else writes a file or kills a pid.
  status)   status ;;
  baseline) rb_require_root; baseline_cmd ;;
  start)    rb_require_root; start ;;
  stop)     rb_require_root; stop ;;
  run)      rb_require_root; run ;;
  *) echo "usage: $0 start|stop|status|run|baseline [--dry-run]" >&2; exit 1 ;;
esac
