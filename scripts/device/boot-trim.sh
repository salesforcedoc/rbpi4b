#!/bin/sh
# boot-trim.sh — the boot-time trim, applied by install.sh and reversible.
#
# What this is for: on the Pi 4 the time from power-on to the rekordbox UI was
# dominated by three things that have nothing to do with the player --
#
#   1. ~9.8 s of systemd daemon-reloads in start-rb.sh's step 1, spent re-masking
#      an already-masked udisks2 and "stopping" three units that are not
#      installed (each systemctl MUTATION reloads the manager);
#   2. cloud-init's three stages, which gate sysinit.target on a unit whose
#      first-boot job finished months ago and whose seed is still on the FAT
#      partition, so it redoes the work every boot;
#   3. the apt timers catching up a missed window at boot, because Persistent=true
#      fires the moment the timer is activated -- which is boot.
#
# Item 1 is fixed in start-rb.sh itself (a read-only guard per call). This script
# carries items 2 and 3, the service/unit decisions that make an early start safe,
# and the /boot/firmware edits -- so that install.sh APPLIES the trim instead of
# printing a recipe for someone to paste.
#
# Usage:  sh boot-trim.sh apply|revert [items]
#         sh boot-trim.sh status|report
# Items:  services cloudinit apt unit bootfiles   (default: all)
#
# apply is idempotent: a second run must produce an empty diff. revert undoes what
# apply recorded in its state file, item by item, and every item is now its own
# inverse — `unit` was the last exception, and it is a drop-in rather than a
# message since 2026-09-27 (see that item below). revert still says plainly when
# one of its steps found nothing to undo.
#
# status is read-only and genuinely useful unprivileged (it can gate a script: it
# exits 0 only when every item is applied). report needs no privileges either --
# it is a measurement, not a check, and always exits 0.
#
# Env:  RB_BOOT_DIR         default /boot/firmware
#       RB_BOOT_TRIM_STATE  default /var/lib/rblive4/boot-trim.state
#       RB_BOOT_TRIM_FORCE=1  proceed past the cloud-init network pre-flight

set -u

HERE=$(cd "$(dirname "$0")" && pwd)

# rb.conf is read for the service lists and the deploy root. This script does NOT
# go through lib.sh's rb_load_conf(): that function exits when rb.conf is missing,
# and boot-trim has to stay runnable from the repo (and from a target whose deploy
# has been removed) so its `status` can be read while deciding what to do. The
# defaults below mirror rb.conf's.
if [ -z "${RB_CONF_FILE:-}" ]; then
    for c in "$HERE/rb.conf" "$HERE/../rb.conf" "$HERE/../../rb.conf"; do
        if [ -f "$c" ]; then RB_CONF_FILE="$c"; break; fi
    done
fi
# shellcheck source=/dev/null
[ -n "${RB_CONF_FILE:-}" ] && [ -f "${RB_CONF_FILE:-}" ] && . "$RB_CONF_FILE"

BOOT_DIR="${RB_BOOT_DIR:-/boot/firmware}"
CMDLINE="$BOOT_DIR/cmdline.txt"
CONFIG="$BOOT_DIR/config.txt"
STATE="${RB_BOOT_TRIM_STATE:-/var/lib/rblive4/boot-trim.state}"
CLOUD_MARKER=/etc/cloud/cloud-init.disabled
CLOUD_UNITS="cloud-init-main cloud-init-local cloud-init-network cloud-config cloud-final"
SEED_FILES="user-data meta-data network-config"
UNIT_DST=/etc/systemd/system/rblive4.service

# The cmdline tokens and config.txt directives this trim ensures. Every one of
# them was already present on the first unit it ran on except `quiet loglevel=3`
# and the `ds=` removal -- so on an established unit `apply bootfiles` mostly
# VERIFIES. Its real job is a fresh target, where none of it is there yet.
CMDLINE_TOKENS="video=HDMI-A-1:1280x800@60 fbcon=map:1 console=tty3 consoleblank=0 vt.global_cursor_default=0 quiet loglevel=3"
# Prefixes whose tokens are removed rather than ensured: the imager's datasource
# hint. NOT what disables cloud-init -- the marker file is (see ci_apply) -- but it
# is the hint ds-identify would answer from if the marker were ever removed, and
# leaving it is a lie about the configuration either way.
CMDLINE_REMOVE="ds="
# Replaced in place, not appended: two video= tokens for one connector is a silent
# last-one-wins (docs/13), so a second must never be added.
CMDLINE_VIDEO_A1="video=HDMI-A-1:1280x800@60"
CONFIG_LINES="boot_delay=0
disable_splash=1"

ALL_ITEMS="services cloudinit apt unit bootfiles"

say()  { echo "boot-trim: $*"; }
warn() { echo "boot-trim: $*" >&2; }
note() { echo "  $*"; }

usage() {
    cat >&2 <<EOF
usage: $0 apply|revert|status|report [items]

  apply   [items]   make the changes (idempotent; prints every diff)
  revert  [items]   undo what apply recorded (every item is its own inverse)
  status  [items]   read-only: is each item applied, and the evidence
  report            the boot measurement (monotonic milestones, one boot)

items: $ALL_ITEMS
       (default: all of them)
EOF
}

# --- state -------------------------------------------------------------------
#
# What apply did, so revert can undo precisely instead of guessing at intent.
# Append-only, one action per line, item first: "services masked udisks2",
# "cloudinit moved-seed /root/cloud-init-seed-20260927". Read back in reverse.
# The state file is a convenience, not the truth: `status` probes the live system
# rather than this file, so a state file that goes missing costs revert its
# precision and nothing else.
state_add() {
    mkdir -p "$(dirname "$STATE")" 2>/dev/null || return 0
    echo "$*" >> "$STATE" 2>/dev/null || true
}

state_lines() { [ -f "$STATE" ] && cat "$STATE"; return 0; }

state_reverse() { state_lines | awk '{a[NR]=$0} END{for(i=NR;i>=1;i--) print a[i]}'; }

# Write PATH from stdin, only if it changes, via a temp in the same directory.
# $2 is the item name, for the state file.
write_if_changed() {
    _p=$1
    _item=$2
    _tmp="$_p.rblive4.new"
    cat > "$_tmp" || { warn "$_item: could not write $_tmp"; return 1; }
    if [ -f "$_p" ] && cmp -s "$_tmp" "$_p"; then
        rm -f "$_tmp"
        say "$_item: $_p unchanged"
        return 0
    fi
    mv "$_tmp" "$_p" || { warn "$_item: could not replace $_p"; return 1; }
    say "$_item: wrote $_p"
    state_add "$_item dropin $_p"
    return 0
}

_require_root() {
    [ "$(id -u)" = "0" ] || {
        echo "$0: apply and revert need root (systemctl mutations, /boot/firmware)" >&2
        echo "  status and report are read-only and do not." >&2
        exit 1
    }
}

# --- 1. services -------------------------------------------------------------

# Does the unit exist at all? `systemctl is-enabled` prints "not-found" for a
# unit that is not installed, and the difference matters: disabling something
# that is not there is a no-op that still reloads the manager.
unit_exists() { [ "$(systemctl show -p LoadState --value "$1" 2>/dev/null)" != "not-found" ]; }

# The enablement state as a string. `systemctl is-enabled` exits NON-ZERO for
# every state except "enabled" -- 1 for masked/disabled/static, 4 for a unit that
# does not exist -- so a `|| echo fallback` appends a second line and the value
# stops comparing equal to anything. Capture stdout, discard the status.
unit_state() {
    _s=$(systemctl is-enabled "$1" 2>/dev/null || true)
    [ -n "$_s" ] || _s=not-found
    echo "$_s"
}

svc_apply() {
    for s in ${RB_MASK_SERVICES:-udisks2 udisks2.service}; do
        if [ "$(unit_state "$s")" = "masked" ]; then
            say "services: $s is already masked"
            continue
        fi
        if systemctl mask "$s" >/dev/null 2>&1; then
            say "services: masked $s (it must not grab the media stick first)"
            state_add "services masked $s"
        else
            warn "services: could not mask $s"
        fi
    done

    for s in ${RB_STOP_SERVICES:-lightdm gdm3 sddm}; do
        if [ "$(unit_state "$s")" = "disabled" ]; then
            say "services: $s is already disabled"
        elif unit_exists "$s"; then
            if systemctl disable "$s" >/dev/null 2>&1; then
                say "services: disabled $s (a display manager would draw over the UI)"
                state_add "services disabled $s"
            else
                warn "services: could not disable $s"
            fi
        else
            say "services: $s is not installed -- nothing to disable"
        fi
    done

    if [ "${RB_DISABLE_GETTY:-0}" = "1" ]; then
        # Guarded on is-enabled, never is-active: `Conflicts=` does not disable.
        # A getty that is enabled-but-momentarily-inactive is exactly the state
        # that must be caught, or getty.target starts it near multi-user, the
        # bidirectional conflict stops rblive4, and Restart=always undoes that
        # ten seconds at a time. Never MASK it either -- masking breaks the
        # documented console-recovery idiom `systemctl start getty@tty1`.
        if [ "$(unit_state getty@tty1)" = "disabled" ]; then
            say "services: getty@tty1 is already disabled"
        elif systemctl disable --now getty@tty1 >/dev/null 2>&1; then
            say "services: disabled getty@tty1 (belt to fbcon=map:1's braces)"
            state_add "services disabled getty@tty1"
        else
            warn "services: could not disable getty@tty1"
        fi
    fi
}

# The pre-install state of the display managers is not knowable from here (they
# are not installed on this unit, and on one where they are, the operator may have
# enabled one deliberately), so revert restores only what it recorded.
svc_revert() {
    if [ ! -f "$STATE" ]; then
        warn "services: no state file at $STATE -- nothing recorded to undo."
        warn "  To unmask udisks2 by hand: systemctl unmask udisks2"
        return 1
    fi
    # Checked before the loop, not counted inside it: a `while` fed by a pipe runs
    # in a subshell, so a counter incremented in there is lost.
    if ! state_lines | grep -q '^services '; then
        say "services: nothing recorded for this item"
        return 0
    fi
    state_reverse | while read -r _item _act _what _rest; do
        [ "$_item" = "services" ] || continue
        case "$_act" in
            masked)
                systemctl unmask "$_what" >/dev/null 2>&1 && \
                    say "services: unmasked $_what" || \
                    warn "services: could not unmask $_what" ;;
            disabled)
                if systemctl enable "$_what" >/dev/null 2>&1; then
                    say "services: re-enabled $_what"
                elif [ "$_what" = "getty@tty1" ]; then
                    warn "services: could not re-enable $_what -- the console idiom"
                    warn "  'systemctl start getty@tty1' still works without it"
                else
                    warn "services: could not re-enable $_what (not installed here?)"
                fi ;;
        esac
    done
    return 0
}

# --- 2. cloud-init -----------------------------------------------------------

# Disabling cloud-init can only be safe if the network does not depend on it.
# On the measured unit it does not -- WiFi is a persistent NetworkManager keyfile
# whose mtime is the image build, not the boot -- but that has to be re-checked on
# the target actually being changed, because the failure mode is losing SSH to a
# machine with no monitor attached.
ci_preflight() {
    _bad=0
    if ! ls /etc/NetworkManager/system-connections/*.nmconnection >/dev/null 2>&1; then
        warn "cloudinit: no NetworkManager keyfile in /etc/NetworkManager/system-connections."
        warn "  The network may be brought up by cloud-init on this target, and disabling"
        warn "  it would take SSH with it. Refusing."
        _bad=1
    fi
    for f in /etc/netplan/*.yaml; do
        [ -f "$f" ] || continue
        # An empty stub is what netplan leaves beside an NM keyfile; content means
        # someone (likely cloud-init) wrote network config and we cannot tell
        # whether it survives without it.
        if [ -s "$f" ]; then
            warn "cloudinit: $f is not empty -- this target's network config may be"
            warn "  cloud-init's. Refusing."
            _bad=1
        fi
    done
    if [ "$_bad" = "1" ]; then
        if [ "${RB_BOOT_TRIM_FORCE:-0}" = "1" ]; then
            warn "cloudinit: RB_BOOT_TRIM_FORCE=1 -- proceeding anyway, on your head."
            return 0
        fi
        warn "  Set RB_BOOT_TRIM_FORCE=1 to override if you know the network is"
        warn "  independent, and read the SSH check in docs/13 first."
        return 1
    fi
    say "cloudinit: pre-flight ok -- the network is a persistent NM keyfile, not cloud-init's"
    return 0
}

ci_apply() {
    ci_preflight || return 1

    # The designed off-switch, and the switch that MATTERS. cloud-init's systemd
    # generator (cloud-init-generator) has exactly one output: the symlink
    # /run/systemd/generator.early/multi-user.target.wants/cloud-init.target. Its
    # only input is ds-identify's exit code, and ds-identify's is_disabled()
    # consults this marker BEFORE it looks for a datasource -- so the marker makes
    # it return 2, the generator deletes that symlink, multi-user.target stops
    # wanting cloud-init.target, and NOTHING pulls the five units or the
    # network-online.target behind them. Measured on the unit 2026-09-27 by running
    # the generator against a faked /proc/cmdline: with the marker, every variant
    # -- including the unit's own ds=nocloud token -- gives ds-identify rc=2 and
    # no symlink.
    #
    # Measured gotcha for anyone checking this by hand: ds-identify CACHES its
    # verdict in /run/cloud-init/.ds-identify.result and returns the cached value
    # unless it is given --force. A hand run without --force therefore reports the
    # BOOT's answer, not the current state -- which reads as "the marker does
    # nothing at all". Delete that file, or pass --force, before believing it.
    if [ -e "$CLOUD_MARKER" ]; then
        say "cloudinit: $CLOUD_MARKER already present"
    else
        mkdir -p "$(dirname "$CLOUD_MARKER")" && touch "$CLOUD_MARKER" && {
            say "cloudinit: created $CLOUD_MARKER"
            state_add "cloudinit marker $CLOUD_MARKER"
        }
    fi

    # DISABLE, not mask. This is belt, and the marker above is the mechanism --
    # cloud-init.target's own unit file names all five in a Wants=, so their
    # on-disk enablement symlinks are redundant with it; clearing them means
    # nothing re-arms the five if something ever wants cloud-init.target again.
    # Masking is wrong twice over: a masked unit that something Requires= makes
    # THAT unit fail, and it makes revert a lie.
    for u in $CLOUD_UNITS; do
        if [ "$(unit_state "$u")" = "disabled" ]; then
            say "cloudinit: $u is already disabled"
            continue
        fi
        if systemctl disable "$u" >/dev/null 2>&1; then
            say "cloudinit: disabled $u"
            state_add "cloudinit disabled $u"
        else
            warn "cloudinit: could not disable $u"
        fi
    done

    ci_seed_move || return 1
    return 0
}

# Move the seed OFF the FAT partition: it is only needed to re-image, and it may
# hold a WiFi PSK and an SSH public key. Moved, never deleted, and never printed
# -- not by apply, not by status, not by report.
ci_seed_move() {
    _present=""
    for f in $SEED_FILES; do
        [ -f "$BOOT_DIR/$f" ] && _present="$_present $f"
    done
    if [ -z "$_present" ]; then
        _old=$(ls -d /root/cloud-init-seed-* 2>/dev/null | tail -1)
        if [ -n "$_old" ]; then
            say "cloudinit: seed already moved ($_old)"
        else
            say "cloudinit: no seed on $BOOT_DIR -- nothing to move"
        fi
        return 0
    fi
    _dir=/root/cloud-init-seed-$(date +%Y%m%d)
    mkdir -p "$_dir" || { warn "cloudinit: could not create $_dir"; return 1; }
    chmod 700 "$_dir"
    for f in $_present; do
        if mv "$BOOT_DIR/$f" "$_dir/$f"; then
            chmod 600 "$_dir/$f"
            say "cloudinit: moved $f off $BOOT_DIR"
        else
            warn "cloudinit: could not move $f (it is still on $BOOT_DIR)"
        fi
    done
    say "cloudinit: seed is at $_dir (mode 700/600; contents never printed)"
    state_add "cloudinit moved-seed $_dir"
    return 0
}

ci_revert() {
    if [ -e "$CLOUD_MARKER" ]; then
        rm -f "$CLOUD_MARKER" && say "cloudinit: removed $CLOUD_MARKER"
    fi
    for u in $CLOUD_UNITS; do
        if systemctl enable "$u" >/dev/null 2>&1; then
            say "cloudinit: re-enabled $u"
        else
            warn "cloudinit: could not re-enable $u (is it still installed?)"
        fi
    done
    _dir=$(state_reverse | awk '$1=="cloudinit" && $2=="moved-seed" {print $3; exit}')
    if [ -n "$_dir" ] && [ -d "$_dir" ]; then
        for f in $SEED_FILES; do
            if [ -f "$_dir/$f" ]; then
                mv "$_dir/$f" "$BOOT_DIR/$f" && say "cloudinit: restored $f to $BOOT_DIR"
            fi
        done
        rmdir "$_dir" 2>/dev/null || true
    elif [ -n "$_dir" ]; then
        warn "cloudinit: the recorded seed directory $_dir is gone; restore it by hand"
    fi
    if [ "${_REVERT_BOOTFILES:-0}" = "1" ]; then
        say "cloudinit: the 'ds=' token comes back with the bootfiles revert below"
    else
        say "cloudinit: note -- the 'ds=' token comes back with: $0 revert bootfiles"
    fi
    return 0
}

ci_status() {
    _bad=0
    if [ -e "$CLOUD_MARKER" ]; then
        echo "  ok  $CLOUD_MARKER present"
    else
        echo "  --  $CLOUD_MARKER absent (cloud-init still runs)"
        _bad=1
    fi
    for u in $CLOUD_UNITS; do
        _s=$(unit_state "$u")
        if [ "$_s" = "disabled" ]; then
            echo "  ok  $u disabled"
        else
            echo "  --  $u is $_s"
            _bad=1
        fi
    done
    _seed=""
    for f in $SEED_FILES; do [ -f "$BOOT_DIR/$f" ] && _seed="$_seed $f"; done
    if [ -z "$_seed" ]; then
        _dir=$(ls -d /root/cloud-init-seed-* 2>/dev/null | tail -1)
        echo "  ok  seed moved off $BOOT_DIR${_dir:+ (at $_dir)}"
    else
        echo "  --  seed still on $BOOT_DIR:$_seed"
        _bad=1
    fi
    case "$(cat "$CMDLINE" 2>/dev/null)" in
        *ds=*) echo "  --  cmdline.txt still carries a ds= token" ;;
        *)     echo "  ok  no ds= token on the cmdline" ;;
    esac
    return $_bad
}

# --- 3. apt timers -----------------------------------------------------------

# Persistent=true is what makes a missed window fire the moment the timer is
# activated, which is boot. There is no OnBootSec to move, and adding one would
# not cancel the catch-up. The calendar window is untouched, so the update still
# runs whenever the unit is up at its slot -- deferred, not disabled.
apt_apply() {
    for t in apt-daily.timer apt-daily-upgrade.timer; do
        d="/etc/systemd/system/$t.d"
        mkdir -p "$d" || { warn "apt: could not create $d"; continue; }
        write_if_changed "$d/rblive4-defer.conf" apt <<'EOF'
# Installed by rblive4's boot-trim.sh. Boot cost, not policy: Persistent=true
# makes a missed window fire the moment the timer is activated, which is boot.
# The calendar window is untouched, so the update still runs whenever the unit is
# up at its slot -- deferred, not disabled.
#
# Undo with: sh boot-trim.sh revert apt
[Timer]
Persistent=false
EOF
    done
    systemctl daemon-reload 2>/dev/null || warn "apt: daemon-reload failed"
    return 0
}

apt_revert() {
    for t in apt-daily.timer apt-daily-upgrade.timer; do
        d="/etc/systemd/system/$t.d"
        if [ -f "$d/rblive4-defer.conf" ]; then
            rm -f "$d/rblive4-defer.conf" && say "apt: removed $d/rblive4-defer.conf"
            rmdir "$d" 2>/dev/null || true
        else
            say "apt: $d/rblive4-defer.conf is not there"
        fi
    done
    systemctl daemon-reload 2>/dev/null || warn "apt: daemon-reload failed"
    return 0
}

apt_status() {
    _bad=0
    for t in apt-daily.timer apt-daily-upgrade.timer; do
        _p=$(systemctl show -p Persistent --value "$t" 2>/dev/null)
        if [ "$_p" = "no" ]; then
            echo "  ok  $t Persistent=$_p"
        else
            echo "  --  $t Persistent=$_p (a missed window fires at boot)"
            _bad=1
        fi
    done
    # The one thing Persistent=false would not stop: a NetworkManager dispatcher
    # hook that starts apt on link-up. Report it so the blame reading is honest.
    _hook=$(grep -rl apt /etc/NetworkManager/dispatcher.d/ 2>/dev/null | tr '\n' ' ')
    if [ -n "$_hook" ]; then
        echo "  ??  an NM dispatcher hook mentions apt: $_hook"
        echo "      Persistent=false will not stop that one -- check blame after a reboot"
    fi
    return $_bad
}

# --- 4. the unit's ordering --------------------------------------------------
#
# The ordering is a property of the SHIPPED unit file — a deliberate design
# decision, so it travels with the launcher it orders, and no drop-in can subtract
# it. After=multi-user.target is why the launcher started 9 ms after
# multi-user.target at 22.5 s while everything it actually needs is up at
# basic.target (11.9 s). So `apply unit` installs/verifies that file rather than
# editing systemd state.
#
# `revert unit` used to be the one item that could not be undone, and it said so:
# reinstalling the shipped file reinstalls the new ordering, so it was a message
# rather than an action. The way back is a DROP-IN, and it works because of the
# same list semantics that stop one subtracting: After= is a list directive and
# drop-ins APPEND to list directives, so a drop-in that names multi-user.target
# restores the old ordering without touching the shipped unit — and survives a
# later install.sh, which overwrites that file unconditionally. `apply unit`
# removes the drop-in, so the two directions are each other's inverse.
ORDER_DROPIN=/etc/systemd/system/rblive4.service.d/rblive4-wait-for-multi-user.conf

# Is the unit ordered behind multi-user.target — i.e. is the PRE-trim ordering in
# force? Asked of systemd's merged view rather than by reading the two files,
# because that is what the boot will actually do, and because a drop-in that
# failed to apply is this item's one silent failure mode.
#   0 = yes, it waits for multi-user.target (the revert is in force)
#   1 = no, the trim's early start is in force
#   2 = systemd could not be asked, which is not evidence of either
#
# Read the polarity carefully: this asks about the OLD ordering, so for
# `unit_revert` a 0 is success and a 1 is failure — the opposite of most checks
# here. The name is spelled out for that reason.
unit_waits_for_multi_user() {
    _a=$(systemctl show -p After --value rblive4.service 2>/dev/null)
    [ -n "$_a" ] || return 2
    case "$_a" in
        *multi-user.target*) return 0 ;;
        *)                   return 1 ;;
    esac
}

unit_apply() {
    # Undo a previous revert FIRST, and before the checks below: with the drop-in
    # in place the unit's effective After= does name multi-user.target, so every
    # reading of "is the trim applied" is wrong until it is gone.
    if [ -f "$ORDER_DROPIN" ]; then
        rm -f "$ORDER_DROPIN" && say "unit: removed $ORDER_DROPIN (the revert's ordering)"
        rmdir "$(dirname "$ORDER_DROPIN")" 2>/dev/null || true
        systemctl daemon-reload 2>/dev/null || warn "unit: daemon-reload failed"
    fi
    if ! grep -q '^After=multi-user.target' "$UNIT_DST" 2>/dev/null; then
        say "unit: the installed unit does not wait for multi-user.target"
        return 0
    fi
    if [ -f "$HERE/rblive4.service" ] && ! grep -q '^After=multi-user.target' "$HERE/rblive4.service"; then
        if install -m 644 -o root -g root "$HERE/rblive4.service" "$UNIT_DST"; then
            say "unit: installed the shipped unit (no After=multi-user.target)"
            systemctl daemon-reload 2>/dev/null || warn "unit: daemon-reload failed"
            state_add "unit installed $UNIT_DST"
            return 0
        fi
        warn "unit: could not install $HERE/rblive4.service"
        return 1
    fi
    warn "unit: $UNIT_DST still waits for multi-user.target, and the copy beside"
    warn "  this script does not fix that. Install the current release:"
    warn "  sh $HERE/install.sh"
    return 1
}

unit_revert() {
    if [ ! -f "$UNIT_DST" ]; then
        warn "unit: $UNIT_DST is not installed; there is no ordering to revert."
        return 1
    fi
    if grep -q '^After=multi-user.target' "$UNIT_DST"; then
        say "unit: the installed unit already waits for multi-user.target"
        return 0
    fi
    mkdir -p "$(dirname "$ORDER_DROPIN")" || { warn "unit: could not create $(dirname "$ORDER_DROPIN")"; return 1; }
    write_if_changed "$ORDER_DROPIN" unit <<'EOF' || return 1
# Installed by rblive4's boot-trim.sh, by `revert unit`. It puts the launcher
# back behind multi-user.target -- the ordering the unit had before the boot trim.
#
# A drop-in rather than an edit to /etc/systemd/system/rblive4.service because
# After= is a LIST directive and drop-ins APPEND to list directives: this ADDS
# multi-user.target back to an ordering the shipped unit no longer names, and it
# therefore survives a later install.sh, which replaces that file unconditionally.
#
# Undo with: sh /opt/rblive4/boot-trim.sh apply unit
[Unit]
After=multi-user.target
EOF
    systemctl daemon-reload 2>/dev/null || warn "unit: daemon-reload failed"
    unit_waits_for_multi_user
    _rc=$?
    if [ "$_rc" != "0" ]; then
        warn "unit: the drop-in is written but systemd's merged After= does not name"
        warn "  multi-user.target, so the revert is not in force (helper returned $_rc:"
        warn "  1 = read the list and it was not there, 2 = could not read it)."
        warn "  Check with: systemctl show -p After rblive4.service"
        return 1
    fi
    say "unit: reverted -- the launcher waits for multi-user.target again"
    return 0
}

unit_status() {
    if [ ! -f "$UNIT_DST" ]; then
        echo "  --  $UNIT_DST is not installed"
        return 1
    fi
    if [ -f "$ORDER_DROPIN" ]; then
        echo "  --  REVERTED: $ORDER_DROPIN puts the launcher back behind"
        echo "      multi-user.target. Run 'apply unit' to remove it."
        return 1
    fi
    if grep -q '^After=multi-user.target' "$UNIT_DST"; then
        echo "  --  the unit still waits for multi-user.target"
        return 1
    fi
    unit_waits_for_multi_user
    case $? in
        1)  echo "  ok  no After=multi-user.target"
            echo "      runs after: $(systemctl show -p After --value rblive4.service 2>/dev/null)"
            return 0 ;;
        0)  echo "  --  the unit file looks trimmed, but systemd's merged After= still"
            echo "      names multi-user.target -- something else is ordering it."
            return 1 ;;
        *)  echo "  ??  could not read rblive4.service's After= list from systemd"
            return 1 ;;
    esac
}

# --- 5. /boot/firmware -------------------------------------------------------

# One backup per file, created once and NEVER overwritten -- otherwise a re-run
# turns the true original into a copy of a previous run's output. Matches the
# operator's own cmdline.txt.bak-1080p idiom already on that partition.
backup_once() {
    _f=$1
    _b="$1.rblive4.bak"
    if [ -f "$_b" ]; then
        say "bootfiles: keeping the existing backup $_b"
    else
        cp -p "$_f" "$_b" || { warn "bootfiles: could not create $_b"; return 1; }
        say "bootfiles: backed up $_f -> $_b (created once, never overwritten)"
        state_add "bootfiles backup $_b"
    fi
    return 0
}

# Rewrite cmdline.txt's tokens. Every /boot edit here goes through a temp file on
# the same partition and a mv over the original: /boot/firmware is FAT, which has
# no journal, on a unit with a documented under-voltage history, so an in-place
# truncate interrupted by a brownout is an unbootable unit.
cmdline_apply() {
    [ -f "$CMDLINE" ] || { warn "bootfiles: $CMDLINE not found"; return 1; }
    backup_once "$CMDLINE" || return 1

    _orig=$(cat "$CMDLINE")
    set -f                                  # no globbing: a token with * is data
    _out=""
    _seen_a1=0
    for _t in $_orig; do
        _drop=0
        for _p in $CMDLINE_REMOVE; do
            case "$_t" in "$_p"*) _drop=1 ;; esac
        done
        [ "$_drop" = "1" ] && continue
        case "$_t" in
            video=HDMI-A-1:*)
                # Canonicalised in place, once. Appending a second would be a
                # silent last-one-wins for the same connector.
                if [ "$_seen_a1" = "0" ]; then
                    _out="$_out $CMDLINE_VIDEO_A1"
                    _seen_a1=1
                fi
                continue ;;
        esac
        _out="$_out $_t"
    done
    for _d in $CMDLINE_TOKENS; do
        case " $_out " in
            *" $_d "*) ;;
            *) _out="$_out $_d" ;;
        esac
    done
    set +f
    _out=${_out# }                           # strip the single leading space

    _tmp="$CMDLINE.rblive4.new"
    printf '%s\n' "$_out" > "$_tmp" || { warn "bootfiles: could not write $_tmp"; return 1; }

    # Verify before replacing. A cmdline that boots is not something to discover
    # later, so: still exactly one line, and every root-finding token that was
    # there before is still there. Checking only the tokens the original actually
    # had, so a target with root=/dev/mmcblk0p2 is not asked for a PARTUUID.
    _ok=1
    if [ "$(wc -l < "$_tmp")" != "1" ]; then
        warn "bootfiles: refusing -- the candidate is $(wc -l < "$_tmp") lines, not 1"
        _ok=0
    fi
    for _need in "root=" "rootwait" "rootfstype=" "PARTUUID="; do
        case "$_orig" in
            *"$_need"*) ;;
            *) continue ;;
        esac
        case "$_out" in
            *"$_need"*) ;;
            *) warn "bootfiles: refusing -- '$_need' was in the original and is gone"
               _ok=0 ;;
        esac
    done
    if [ "$_ok" != "1" ]; then
        warn "bootfiles: $CMDLINE is UNTOUCHED; the candidate is at $_tmp"
        return 1
    fi
    if cmp -s "$_tmp" "$CMDLINE"; then
        rm -f "$_tmp"
        say "bootfiles: cmdline.txt unchanged"
        return 0
    fi
    mv "$_tmp" "$CMDLINE" || { warn "bootfiles: could not replace $CMDLINE"; return 1; }
    say "bootfiles: rewrote $CMDLINE"
    cmdline_show_diff
    return 0
}

# A token-at-a-time diff: on a one-line file a line diff is two 300-character
# lines, which is not a diff anyone can read.
cmdline_show_diff() {
    _b="$CMDLINE.rblive4.bak"
    [ -f "$_b" ] || return 0
    _t1=/tmp/boot-trim-old.$$
    _t2=/tmp/boot-trim-new.$$
    tr ' ' '\n' < "$_b" > "$_t1" 2>/dev/null
    tr ' ' '\n' < "$CMDLINE" > "$_t2" 2>/dev/null
    echo "  --- $_b (old)  +++ $CMDLINE (new)"
    diff -u "$_t1" "$_t2" 2>/dev/null | tail -n +3 | sed 's/^/  /' || true
    rm -f "$_t1" "$_t2"
}

config_apply() {
    [ -f "$CONFIG" ] || { warn "bootfiles: $CONFIG not found"; return 1; }
    backup_once "$CONFIG" || return 1

    # Only append when the LAST section header really is [all]. Appending after a
    # [cm4]/[cm5]/[pi5] header would silently apply a Pi-wide directive to one
    # model -- the file's own sections are why this check exists.
    _last=$(grep '^\[' "$CONFIG" 2>/dev/null | tail -1)
    if [ "$_last" != "[all]" ]; then
        warn "bootfiles: $CONFIG's last section is '${_last:-<none>}', not '[all]' --"
        warn "  not appending. Add the directives by hand inside [all] if you want them."
        return 1
    fi

    _add=""
    for _l in $CONFIG_LINES; do
        _k=${_l%%=*}
        if grep -q "^[[:space:]]*${_k}=" "$CONFIG" 2>/dev/null; then
            say "bootfiles: $_k is already set in config.txt"
        else
            _add="$_add$_l
"
        fi
    done
    if [ -z "$_add" ]; then
        say "bootfiles: config.txt unchanged"
        return 0
    fi

    _tmp="$CONFIG.rblive4.new"
    {
        cat "$CONFIG"
        printf '\n# rblive4 boot trim -- revert with: sh boot-trim.sh revert bootfiles\n'
        printf '%s' "$_add"
    } > "$_tmp" || { warn "bootfiles: could not write $_tmp"; return 1; }

    # Verify: the original must be an exact PREFIX of the candidate, which proves
    # this only appended. A failed write cannot silently truncate the file.
    _osz=$(wc -c < "$CONFIG")
    if ! head -c "$_osz" "$_tmp" 2>/dev/null | cmp -s - "$CONFIG"; then
        warn "bootfiles: refusing -- the candidate is not the original plus an append"
        warn "bootfiles: $CONFIG is UNTOUCHED; the candidate is at $_tmp"
        return 1
    fi
    mv "$_tmp" "$CONFIG" || { warn "bootfiles: could not replace $CONFIG"; return 1; }
    say "bootfiles: appended to $CONFIG"
    echo "  --- $CONFIG.rblive4.bak  +++ $CONFIG"
    diff -u "$CONFIG.rblive4.bak" "$CONFIG" 2>/dev/null | tail -n +3 | sed 's/^/  /' || true
    return 0
}

bootfiles_apply() {
    _rc=0
    cmdline_apply || _rc=1
    config_apply || _rc=1
    return $_rc
}

bootfiles_revert() {
    _rc=0
    for f in "$CMDLINE" "$CONFIG"; do
        _b="$f.rblive4.bak"
        if [ ! -f "$_b" ]; then
            say "bootfiles: no backup for $f -- nothing to restore"
            continue
        fi
        if cmp -s "$_b" "$f"; then
            say "bootfiles: $f already matches its backup"
            continue
        fi
        if cp -p "$_b" "$f"; then
            say "bootfiles: restored $f from $_b"
        else
            warn "bootfiles: could not restore $f"
            _rc=1
        fi
    done
    return $_rc
}

bootfiles_status() {
    _bad=0
    if [ -f "$CMDLINE" ]; then
        _line=$(cat "$CMDLINE")
        for _d in $CMDLINE_TOKENS; do
            case " $_line " in
                *" $_d "*) ;;
                *) echo "  --  cmdline.txt is missing: $_d"; _bad=1 ;;
            esac
        done
        for _p in $CMDLINE_REMOVE; do
            case " $_line " in
                *" $_p"*) echo "  --  cmdline.txt still has a ${_p}* token"; _bad=1 ;;
            esac
        done
        # The A-1/A-2 gap, reported by name rather than closed: the shipped recipe
        # has two video= tokens and this unit carries one, and adding the second
        # is S10.8's precondition, not a boot-time trim.
        case " $_line " in
            *" video=HDMI-A-2:"*) ;;
            *) echo "  --  no video=HDMI-A-2 token (deliberate; see docs/13 S10.8)" ;;
        esac
    else
        echo "  --  $CMDLINE not found"
        _bad=1
    fi
    if [ -f "$CONFIG" ]; then
        for _l in $CONFIG_LINES; do
            _k=${_l%%=*}
            if grep -q "^[[:space:]]*${_k}=" "$CONFIG" 2>/dev/null; then
                echo "  ok  config.txt: $_l"
            else
                echo "  --  config.txt: $_k not set"
                _bad=1
            fi
        done
    else
        echo "  --  $CONFIG not found"
        _bad=1
    fi
    # Both files carry a backup exactly once, or the revert path is not there.
    for f in "$CMDLINE" "$CONFIG"; do
        if [ -f "$f.rblive4.bak" ]; then
            echo "  ok  backup: $f.rblive4.bak"
        else
            echo "  --  no backup at $f.rblive4.bak"
            _bad=1
        fi
    done
    return $_bad
}

# --- dispatch ----------------------------------------------------------------

do_apply() {
    for _i in "$@"; do
        case "$_i" in
            services)  svc_apply ;;
            cloudinit) ci_apply || warn "cloudinit: item did not complete" ;;
            apt)       apt_apply ;;
            unit)      unit_apply || warn "unit: item did not complete" ;;
            bootfiles) bootfiles_apply || warn "bootfiles: item did not complete" ;;
            *) warn "unknown item: $_i"; usage; exit 1 ;;
        esac
    done
    echo
    say "apply done. A reboot is what makes the ordering and cloud-init changes real;"
    say "run '$0 report' before and after it and diff the two."
}

do_revert() {
    # ci_revert prints a note about the `ds=` token, which lives in cmdline.txt.
    # Whether that note is news depends on whether bootfiles is being put back in
    # THIS run -- and bootfiles reverts last, so the flag has to be computed before
    # the loop rather than consulted during it.
    _REVERT_BOOTFILES=0
    for _i in "$@"; do
        [ "$_i" = "bootfiles" ] && _REVERT_BOOTFILES=1
    done
    for _i in "$@"; do
        case "$_i" in
            services)  svc_revert || warn "services: item did not revert cleanly" ;;
            cloudinit) ci_revert ;;
            apt)       apt_revert ;;
            unit)      unit_revert || warn "unit: see the note above" ;;
            bootfiles) bootfiles_revert || warn "bootfiles: item did not revert cleanly" ;;
            *) warn "unknown item: $_i"; usage; exit 1 ;;
        esac
    done
    say "revert done. The unit's ordering and the reload count are measured, not"
    say "asserted: '$0 report' after a reboot is how the revert is confirmed."
}

do_status() {
    _bad=0
    for _i in "$@"; do
        echo "$_i:"
        case "$_i" in
            services)
                for s in ${RB_MASK_SERVICES:-udisks2 udisks2.service}; do
                    _s=$(unit_state "$s")
                    [ "$_s" = "masked" ] && echo "  ok  $s masked" || { echo "  --  $s is $_s"; _bad=1; }
                done
                _g=$(unit_state getty@tty1)
                [ "$_g" = "disabled" ] && echo "  ok  getty@tty1 disabled" || echo "  ..  getty@tty1 is $_g (RB_DISABLE_GETTY=${RB_DISABLE_GETTY:-unset})"
                ;;
            cloudinit) ci_status || _bad=1 ;;
            apt)       apt_status || _bad=1 ;;
            unit)      unit_status || _bad=1 ;;
            bootfiles) bootfiles_status || _bad=1 ;;
            *) warn "unknown item: $_i"; usage; exit 1 ;;
        esac
    done
    if [ "$_bad" = "0" ]; then
        say "status: every item checked is applied"
        return 0
    fi
    say "status: at least one item is NOT applied (see the '--' lines)"
    return 1
}

# --- report: the measurement --------------------------------------------------
#
# The deliverable that makes the trim verifiable. A fixed set of labelled
# MONOTONIC milestones, so a before/after is a diff rather than a re-derivation.
#
# Two traps this avoids by construction:
#   1. the clock is stepped FORWARD ~19 s by systemd-timesyncd's first sync (no
#      RTC battery), so no wall-clock delta appears here at all;
#   2. `systemd-analyze` excludes kernel time while the journal includes it, so
#      the target rows come from systemd's own ActiveEnterTimestampMonotonic
#      (microseconds) -- one base throughout -- and only the kernel row is quoted
#      from systemd-analyze, where it is the whole number.
do_report() {
    # name, value, [the row this one must not be earlier than]
    #
    # The predecessor is NAMED per row rather than implied by print order, because
    # the print order that reads best is not a total order -- the two are only the
    # same in the trimmed configuration. Measured counterexample: rblive4 starts
    # after basic.target in a trimmed boot (S11.5: 8.954 after 8.907) and after
    # multi-user.target in an untrimmed one (S11.6b: 13.953 after 13.950); and
    # multi-user.target precedes network-online.target in S11.6b (13.950 / 19.779)
    # while following it in S11.0 (22.549 / 22.532). Naming the predecessor checks
    # the real invariant in every configuration instead of warning on a correct one
    # -- which matters because `revert` lands the untrimmed configuration, so a
    # false warning there would send the next reader after a non-defect.
    row() {
        printf '%-14s %s\n' "$1" "$2"
        case "$2" in ''|n/a|*[!0-9.]*) return 0 ;; esac
        # A dash is not legal in a variable name, and three of these rows have one
        # (wait-online, net-online, multi-user), so the store key is sanitised.
        eval "_st_$(printf '%s' "$1" | tr -- '-' '_')=\$2"
        [ -n "${3:-}" ] || return 0
        eval "_pv=\${_st_$(printf '%s' "$3" | tr -- '-' '_'):-}"
        [ -n "$_pv" ] || return 0
        awk -v a="$_pv" -v b="$2" 'BEGIN{ exit (b+0 < a+0) }' || \
            echo "boot-trim: WARNING: '$1' ($2) is earlier than '$3' ($_pv), which it must follow -- a grep matched the wrong line, or a unit restarted" >&2
    }

    echo "=== inputs: what this measurement was taken against ==="
    printf '%-14s %s\n' boot-id  "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
    # The FILE is what this trim edits, so a report diff shows the trim; the
    # EFFECTIVE line is what the kernel actually got -- the firmware translates
    # it (serial0 -> ttyS0, and it adds 8250.nr_uarts=0), which is why the two
    # differ and why only this one can settle what the console= tokens do.
    printf '%-14s %s\n' cmdline  "$(cat "$CMDLINE" 2>/dev/null)"
    printf '%-14s %s\n' cmdline-eff "$(cat /proc/cmdline 2>/dev/null)"
    printf '%-14s %s\n' rb-conf  "$(sed -n 's/^RB_CONF_VERSION=//p' "${RB_CONF_FILE:-/opt/rblive4/rb.conf}" 2>/dev/null)"
    printf '%-14s %s\n' shim     "$(sha256sum "${RB_DEPLOY_ROOT:-/opt/rblive4}/libdirectfb_fbdev-rot16.so" 2>/dev/null | cut -c1-12)"
    echo
    echo "=== boot, seconds since boot (monotonic), one boot_id ==="
    # The order below is the dependency graph of the TRIMMED target, which is what
    # a reader wants to see; the third argument on each line names the row it must
    # not be earlier than, which is what is actually checked. See row().
    #
    #   boot chain   kernel -> boot-fw -> sysinit -> basic
    #   network      basic -> network -> wait-online -> net-online
    #                (wait-online is the SERVICE and net-online the TARGET: the
    #                 service ACTIVATES before the target is REACHED)
    #   multi-user   basic -> multi-user   (NOT via net-online: measured both ways)
    #   launcher     basic -> rblive4 -> launcher -> fix-dev -> rbp-launch ->
    #                rbp-ready -> usb-watch -> display-w
    #
    # The three chains are independent of each other in the trimmed target -- the
    # launcher no longer waits on the network or multi-user at all -- so nothing
    # checks a row against a row from another chain.
    _kt=$(systemd-analyze 2>/dev/null | sed -n 's/.*[+ ]\([0-9.]*\)s (kernel).*/\1/p')
    row kernel    "${_kt:-n/a}"
    row boot-fw   "$(ustamp boot-firmware.mount)"                  kernel
    row sysinit   "$(ustamp sysinit.target)"                      boot-fw
    row basic     "$(ustamp basic.target)"                        sysinit
    row rblive4   "$(ustamp rblive4.service)"                     basic
    row launcher  "$(ms 'start-rb: deploy root')"                 rblive4
    row network   "$(ustamp network.target)"                      basic
    row wait-online "$(ustamp NetworkManager-wait-online.service)" network
    row net-online "$(ustamp network-online.target)"              wait-online
    row multi-user "$(ustamp multi-user.target)"                  basic
    row fix-dev   "$(ms 'fix-dev: chroot')"                       launcher
    row rbp-launch "$(ms 'start-rb: launching rbp')"              fix-dev
    row rbp-ready "$(ms 'start-rb: rbp pid')"                     rbp-launch
    row usb-watch "$(ms 'start-rb: usb-watch started')"           rbp-ready
    row display-w "$(ms 'start-rb: display-watch started')"       usb-watch
    echo
    echo "=== the thing the trim is for ==="
    # The regression guard for the start-rb.sh step-1 guard. It reads 1, not 0:
    # the reload that remains is NetworkManager's own, and there is no reason for
    # this script to remove it. What the guard is actually asserting is that NONE
    # are attributable to the launcher -- the baseline boot had 5, three of them
    # the launcher's step 1 (see docs/13 S11.0-S11.3), and cloud-init owned one.
    printf '%-14s %s\n' reloads "$(journalctl -b --no-pager 2>/dev/null | grep -c 'Reload requested from client')"
    printf '%-14s %s\n' cloud-init "$(unit_state cloud-init-main)"
    printf '%-14s %s\n' apt-daily "$(systemctl show -p Persistent --value apt-daily.timer 2>/dev/null)"
    return 0
}

# systemd's own monotonic stamp for a unit, in seconds. 0 means the unit is not
# active this boot -- which is itself worth printing, not hiding as a 0.
ustamp() {
    systemctl show -p ActiveEnterTimestampMonotonic --value "$1" 2>/dev/null | \
        awk '{ if ($1 > 0) printf "%.3f", $1/1000000; else print "n/a" }'
}

# A monotonic timestamp from the journal, for the lines only the launcher prints.
# Field 2 of short-monotonic is the timestamp: "[   32.484396] rpidev01 ...".
# Rounded to 3 decimals so a report diff is not noise, and "n/a" (not 0.000)
# when the line is absent -- a missing watcher echo must not read as "at boot".
ms() {
    _v=$(journalctl -b -o short-monotonic --no-pager 2>/dev/null | grep -m1 -F "$1" | \
         awk '{print $2}' | tr -d '[]')
    if [ -n "$_v" ]; then printf '%.3f' "$_v"; else echo "n/a"; fi
}

case "${1:-}" in
  # status and report are read-only (they read systemd and the boot partition) and
  # are useful unprivileged; apply and revert mutate units and /boot/firmware.
  status) shift; do_status ${*:-$ALL_ITEMS} ;;
  report) do_report ;;
  apply)  shift; _require_root; do_apply ${*:-$ALL_ITEMS} ;;
  revert) shift; _require_root; do_revert ${*:-$ALL_ITEMS} ;;
  ""|-h|--help|help) usage; exit 1 ;;
  *) usage; exit 1 ;;
esac
