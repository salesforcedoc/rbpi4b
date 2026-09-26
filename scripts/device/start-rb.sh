#!/bin/sh
# start-rb.sh — launch the XDJ-RX3 rekordbox player (rbp) on the Raspberry Pi 4.
#
# Run as root, over SSH. The UI takes over the HDMI framebuffer, so this is not
# something to launch from the console you intend to keep using: fbcon is mapped
# away from fb0 (see docs/13-raspberrypi4.md) and the launcher sleeps until rbp
# exits rather than letting a shell redraw over it.
#
# This script is also the single translation point between rb.conf's device-level
# variables (RB_*) and the environment the shims and the patched DirectFB driver
# actually read (see the SHIM_VARS block below). The shims never parse rb.conf.
#
# Idempotent: re-running it kills the previous instance first.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf
rb_require_root
rb_require_chroot

LOG_DIR="${RB_LOG_DIR:-$RB_DEPLOY_ROOT/log}"
mkdir -p "$LOG_DIR"
RBP_LOG="$LOG_DIR/rbp.log"
EDB_LOG="$LOG_DIR/edb_streamd.log"

echo "start-rb: deploy root $RB_DEPLOY_ROOT"
echo "start-rb: chroot      $RB_CHROOT"

# --- 1. stop desktop services that would fight us for the display ------------
#
# Engine OS and edisksd are gone — this is Pi OS — but the risk they represented
# is still here in two forms: a display manager drawing over fb0, and udisks2
# mounting the media stick before usb-watch.sh can bind it into the chroot (two
# mounts of one device, and rbp ends up reading a path the kernel has already
# given away).
for s in ${RB_STOP_SERVICES:-}; do
    systemctl stop "$s" 2>/dev/null
done
for s in ${RB_MASK_SERVICES:-}; do
    systemctl mask "$s" 2>/dev/null
done
if [ "${RB_DISABLE_GETTY:-0}" = "1" ]; then
    # The local console must never draw over the UI. cmdline.txt's fbcon=map:1 is
    # the primary mechanism; this removes the second way text reaches the screen.
    # It does not affect SSH, which is how the launcher is run.
    systemctl disable --now getty@tty1 2>/dev/null
fi

# --- 2. kill stale processes -------------------------------------------------
#
# Via /proc, not `ps w | awk`: see pids_matching() in lib.sh for why, and
# docs/11 for the session this used to kill. Patterns are the actual command
# lines — the player is started through the chroot's loader, so its argv is
# "$RB_LOADER $RB_PLAYER ...", which is what is matched here.
for p in $(pids_matching "$RB_PLAYER" strace) \
         $(pids_matching edb_streamd) \
         $(pids_matching gdbserver) \
         $(pids_matching usb-watch.sh); do
    kill -9 "$p" 2>/dev/null
done
sleep 1

# --- 3. device binds, stubs and FIFOs ---------------------------------------
sh "$HERE/fix-dev.sh" || { echo "start-rb: fix-dev.sh failed" >&2; exit 1; }

# --- 4. optional shim/player overrides --------------------------------------
#
# A clean deploy needs none of this: build-chroot.sh already placed the player,
# the shims and the patched fbdev module *inside* the chroot. But dropping a
# freshly built file at the deploy root and re-running this script is how you
# iterate on one shim without rebuilding the tarball, so honour it when present.
install_override() {
    src=$1
    dst=$2
    [ -f "$src" ] || return 0
    if cp "$src" "$dst"; then
        chmod 755 "$dst"
        echo "  override: $(basename "$src") -> $dst"
    else
        echo "  WARNING: could not install override $src" >&2
    fi
}
install_override "$RB_DEPLOY_ROOT/fbshim.so"     "$RB_CHROOT/usr/lib/fbshim.so"
install_override "$RB_DEPLOY_ROOT/knobshim.so"   "$RB_CHROOT/usr/lib/knobshim.so"
install_override "$RB_DEPLOY_ROOT/audioshim.so"  "$RB_CHROOT/usr/lib/audioshim.so"
install_override "$RB_DEPLOY_ROOT/rbp-audio"     "$RB_CHROOT/root/pdj/rbp"
install_override "$RB_DEPLOY_ROOT/libdirectfb_fbdev-rot16.so" \
                 "$RB_CHROOT/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"

# --- 5. clear stale IPC and logs --------------------------------------------
#
# These are host paths: the chroot's /tmp is a bind mount of this one (fix-dev.sh
# step 3), so both sides see the same files. rbp and the shims use them as
# rendezvous points — a leftover guard_LocalDBServer from an unclean exit makes
# the next rbp believe a database server is already running.
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
# The two shim logs are appended to, so a stale one would mix two runs together.
# The MIDI dump is not in this list: it is not a fixed path any more, and the
# shim opens it for writing (see RB_MIDI_DUMP in rb.conf), so a stale dump is
# truncated by the writer rather than by us.
rm -f /tmp/knobshim.log /tmp/audioshim.log
rm -f /tmp/dfbdig*.log /tmp/rot_surface.dump
# Rotated, not deleted. Under systemd's Restart=always a crash-loop relaunches
# this script every 10 s, and `rm -f` would therefore destroy the previous
# cycle's cause before anyone could read it -- the one artifact that says why rbp
# died would be erased on a 10-second timer. One cycle back is enough to break
# the loop, and `.prev` cannot grow without bound.
[ -f "$RBP_LOG" ] && mv -f "$RBP_LOG" "$RBP_LOG.prev"
[ -f "$EDB_LOG" ] && mv -f "$EDB_LOG" "$EDB_LOG.prev"

# --- 6. the shim environment -------------------------------------------------
#
# rb.conf's RB_<NAME> becomes <NAME> in the launched environment, which is the
# name the shim or the DirectFB driver reads. One list, so adding a variable to
# rb.conf costs a line here rather than a hand-written export that can drift
# out of step with it.
#
# Empty means unset, for every one of these. That is a property the consumers
# must honour (the DirectFB driver's DFB_ROTATE parser is the current example:
# `if (rot && rot[0] && strcmp(rot,"off"))`), because this loop exports the
# empty ones too — an unset RB_POINT_DEV has to mean "discover the pointer"
# rather than "look for a device named ''".
#
# The controls shim's flags are read the other way, BY VALUE (env_on/env_num in
# scripts/shims/shimutil.h), so 0 is a value and not a presence. That is what lets
# KNOB_VERBOSE, LED_VU and LED_DISABLE sit in this list: exporting
# KNOB_VERBOSE=0 turns logging off rather than on, and no separate workaround
# export is needed to say "off".
#
# A name belongs in this list iff it is a device-level setting worth a line in
# rb.conf. Diagnostics you set for one run (VU_TEST, LED_DUMP, MIDI_DUMP's
# contents, ...) must NOT be added: this loop exports every name in its list
# unconditionally, so being listed means an ad-hoc `LED_DUMP=1 sh start-rb.sh`
# would be overwritten with the empty string. Set those by exporting the shim
# name directly, which reaches rbp because nothing here uses `env -i`.
SHIM_VARS="
DFB_PRESENT DFB_ROTATE PAN_PACER_MS FB_DEV
POINT_KIND POINT_DEV POINT_DEBUG POINT_MIN_DWELL_MS POINT_MOUSE_SPEED
POINT_SWAP_XY POINT_INVERT_X POINT_INVERT_Y POINT_CURSOR POINT_CURSOR_MS
AUDIO_DEV AUDIO_CHANNELS AUDIO_MAP AUDIO_FMT AUDIO_MONITOR_PAIR
STARTUP_MUTE_MS STARTUP_FADE_MS SCHED_RT
MIDI_MAP MIDI_IN_MATCH MIDI_OUT_MATCH MIDI_DUMP MIDI_REPLAY
MIDI_REPLAY_SPEED KBD_DEV
LED_VU LED_VU_SEGMENTS LED_PADS LED_DISABLE PAD_BRIGHT JOG_PPR
JOG_SCALE JOG_REV JOG_IDLE_MS KNOB_SCALE TEMPO_REV
KNOB_VERBOSE JOG_VERBOSE TEMPO_VERBOSE LED_VERBOSE
SHMSTATE_STRICT
"
for v in $SHIM_VARS; do
    eval "val=\${RB_$v:-}"
    export "$v=$val"
done

# LD_PRELOAD is deliberately NOT exported here: it is passed inside the chroot at
# step 9 instead. The loader that has to act on it is the chroot's ld.so, and
# exporting it in this shell would also hand it to the HOST's chroot binary,
# whose glibc would then try to preload soft-float ARM32 libraries from paths
# that mean something different on the host.
#
# The list is ordered, and the order is load-bearing: fbshim first, and the audio
# shim after the controls shim because it reads globals the controls shim defines.
# A wrong order fails loudly rather than silently — see scripts/shims/shmstate.h.

echo "start-rb: LD_PRELOAD=$RB_LD_PRELOAD"
echo "start-rb: DFB_PRESENT=${DFB_PRESENT:-<unset>} DFB_ROTATE=${DFB_ROTATE:-<off>}"
echo "start-rb: AUDIO_DEV=$AUDIO_DEV MIDI_MAP=$MIDI_MAP POINT_KIND=$POINT_KIND"

# --- 7. start the EDB daemon inside the chroot ------------------------------
#
# rbp talks to edb_streamd over the files in /tmp cleared above. It is started
# through the chroot's own ld.so for the same reason rbp is: the binary is
# soft-float ARM32 against glibc 2.13.
export EDB_BIN=/usr/bin
nohup chroot "$RB_CHROOT" "$RB_LOADER" /usr/bin/edb_streamd >>"$EDB_LOG" 2>&1 &
sleep 1

# --- 8. stop the USB watcher while rbp initialises --------------------------
sh "$HERE/usb-watch.sh" stop 2>/dev/null

# --- 9. start rbp -----------------------------------------------------------
#
# `env` runs INSIDE the chroot, so LD_PRELOAD and PATH are in effect for ld.so
# and rbp only — never for the host process doing the chroot. The player is
# started through the chroot's loader rather than by exec because it is non-PIE,
# soft-float, and linked against glibc 2.13.
echo "start-rb: launching rbp (log: $RBP_LOG)"
nohup chroot "$RB_CHROOT" env \
      "PATH=/bin:/sbin:/usr/bin:/usr/sbin" \
      "LD_PRELOAD=$RB_LD_PRELOAD" \
      "$RB_LOADER" "$RB_PLAYER" -a \
      </dev/null >>"$RBP_LOG" 2>&1 &

# Wait for rbp's DeviceSQL channel to come up, detected by it opening the
# udev_usb1 FIFO. Sending a media mount event before this is silently lost, so
# the watcher is not started until rbp visibly holds the fd.
RBP=""
for i in $(seq 1 30); do
    RBP=$(pids_matching "$RB_PLAYER" | head -1)
    if [ -n "$RBP" ] && ls -l "/proc/$RBP/fd" 2>/dev/null | grep -q udev_usb1; then
        echo "start-rb: rbp pid $RBP ready (udev_usb1 fd open)"
        break
    fi
    sleep 0.5
done
if [ -z "$RBP" ]; then
    echo "start-rb: rbp is not running. Last lines of $RBP_LOG:" >&2
    tail -20 "$RBP_LOG" >&2 2>/dev/null
    exit 1
fi

# --- 10. USB watcher, then wait ---------------------------------------------
sh "$HERE/usb-watch.sh" start 2>/dev/null

# --- 11. cleanup, on every exit path ----------------------------------------
#
# Defined before the wait loop and installed as a signal handler, because
# `systemctl stop`/`restart` sends SIGTERM to THIS shell: without the trap the
# shell dies where it stands and everything it started -- rbp, edb_streamd, the
# watcher, and the media mounts -- is left running. The unit would then be
# "stopped" with a live player on screen, and the next start would fight it.
# This also makes docs/11's existing claim ("Ctrl-C the launcher") true, which it
# has not been: a Ctrl-C killed the shell and orphaned rbp the same way.
cleanup() {
    sh "$HERE/usb-watch.sh" stop 2>/dev/null
    for p in $(pids_matching "$RB_PLAYER") $(pids_matching edb_streamd); do
        kill -9 "$p" 2>/dev/null
    done
}
# `exit 0` and not the signal's default status: a stop is a successful stop, and
# a non-zero exit here would make systemd record a failure for a deliberate
# `systemctl stop`. Under Restart=always the exit status does not decide whether
# we relaunch, but it does decide what `systemctl status` reports afterwards.
trap 'cleanup; exit 0' TERM INT

# Sleep while rbp lives, so the launching shell does not redraw over the UI.
while kill -0 "$RBP" 2>/dev/null; do
    sleep 2
done

cleanup
echo "start-rb: rbp exited (see $RBP_LOG)"
