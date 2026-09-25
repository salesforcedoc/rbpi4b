#!/bin/sh
# usb-watch.sh — USB media hotplug -> rbp, on the Raspberry Pi 4.
#
# A USB stick plugged into the Pi must appear to rbp exactly as it would on an
# XDJ-RX3, because the path is compiled into the player:
#
#   mount  /dev/sdX1            -> $RB_MEDIA_MOUNT/sda1     (host side)
#   bind   $RB_MEDIA_MOUNT/sda1 -> $RB_CHROOT/media/usb1/sda1  (the chroot view)
#   write  "mount /media/usb1/sda1" -> /tmp/udev_usb1       (rbp's FIFO)
#
# The chroot path /media/usb1/sda1 is fixed — it is patch 1 of
# tools/patch-rbp/PATCHES.md — so RB_CHROOT_MEDIA is not really configurable; the
# mount point on the host side is.
#
# On detach the FIFO gets "umount", then both mounts are released lazily.
#
# This mirrors the XDJ-RX3's udev rule 12-usb-memory-auto-mount.rules. The mount
# event alone is enough — never write to /tmp/udev_usbctn* ("connect" there makes
# rbp raise "USB Error. Remove the device.").
#
# Usage:  sh usb-watch.sh start|stop|status|run
# Env:    USBWATCH_POLL=1     poll interval seconds (RB_USB_POLL_S)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf
rb_require_chroot

MNT="${RB_MEDIA_MOUNT:-$RB_DEPLOY_ROOT/media/usb1}/sda1"
# On the previous target these two were the same string (/media/usb1/sda1 was
# both the host mount point and the path rbp saw). They are different here, and
# the distinction matters: the FIFO messages below are read by rbp *inside* the
# chroot, so they must carry the chroot-side path, while the mount commands
# operate on the host-side one.
CHROOT_MEDIA_PATH="${RB_CHROOT_MEDIA:-/media/usb1/sda1}"
CH_MNT="$RB_CHROOT$CHROOT_MEDIA_PATH"
FIFO=/tmp/udev_usb1
LOG="${RB_LOG_DIR:-$RB_DEPLOY_ROOT/log}/usbwatch.log"
PIDFILE=/tmp/usbwatch.pid
POLL="${USBWATCH_POLL:-${RB_USB_POLL_S:-2}}"
TIMEOUT="${RB_USB_TIMEOUT:-timeout}"

mkdir -p "$(dirname "$LOG")" 2>/dev/null
log() { echo "$(date '+%F %T') $$ $*" >> "$LOG" 2>/dev/null; }

# --- locate the media block device ------------------------------------------
#
# The SC Live 4 version hardcoded bus numbers ("usb1 usb2 usb3") because its
# media port was a known controller. A Pi has no such mapping: every Type-A port
# hangs off one PCIe-attached xHCI controller plus the USB-C/OTG port on another,
# and the bus numbers vary by model and firmware. So the test is structural — a
# device is media if its sysfs path passes through a USB bus — and the removable
# flag decides between candidates rather than gating them.
#
# removable is a preference, not a requirement: a USB SSD or 2.5" enclosure
# commonly reports removable=0 while being perfectly good rekordbox media, and
# refusing it would refuse the one device the operator actually plugged in.
list_media_sds() {
    for blk in /sys/block/sd*; do
        [ -e "$blk" ] || continue
        tgt=$(readlink -f "$blk" 2>/dev/null) || continue
        case "$tgt" in *"/usb"[0-9]*) ;; *) continue ;; esac
        echo "${blk##*/}"
    done
}

find_media_sd() {
    first=""
    for d in $(list_media_sds); do
        [ -n "$first" ] || first="$d"
        if [ "$(cat "/sys/block/$d/removable" 2>/dev/null)" = "1" ]; then
            echo "$d"
            return 0
        fi
    done
    [ -n "$first" ] && echo "$first"
}

# --- wait for the first partition; fall back to a whole-disk filesystem ------
find_partition() {
    dev=$1
    i=0
    while [ "$i" -lt 40 ]; do                 # up to 4 s (0.1 s steps)
        [ -b "/dev/${dev}1" ] && { echo "${dev}1"; return 0; }
        i=$((i + 1))
        sleep 0.1
    done
    if blkid "/dev/$dev" >/dev/null 2>&1; then echo "$dev"; return 0; fi
    return 1
}

# --- tell rbp about a USB event (FIFO; rbp holds it open O_RDWR) -------------
notify() {
    msg=$1
    if [ ! -p "$FIFO" ]; then
        log "notify: $FIFO missing (rbp down?) — skipping"
        return 1
    fi
    # The write must not block forever if rbp is gone: the FIFO has no reader, and
    # opening it for write blocks until one appears.
    if "$TIMEOUT" 3 sh -c 'printf "%s" "$1" > "$2"' sh "$msg" "$FIFO" 2>/dev/null; then
        log "notify: $msg"
        return 0
    fi
    log "notify: FAILED to write '$msg' (rbp down?)"
    return 1
}

rbp_pid() { pids_matching "$RB_PLAYER" strace | head -1; }

# --- send umount+then mount, retrying until rbp opens the DB ----------------
# rbp's DeviceSQL channel takes several seconds to come up after a (re)start, and
# a mount event sent before that is silently lost. Keep re-notifying until rbp
# actually opens export.pdb, which means its analysis has started.
notify_mount_until_open() {
    n=0
    while [ "$n" -lt 12 ]; do
        sleep 4
        notify "umount $CHROOT_MEDIA_PATH"
        sleep 0.3
        notify "mount $CHROOT_MEDIA_PATH"
        sleep 4
        rbp=$(rbp_pid)
        if [ -n "$rbp" ] && ls -l "/proc/$rbp/fd" 2>/dev/null | grep -q "export.pdb"; then
            log "attach: rbp opened export.pdb (notify attempt $n)"
            return 0
        fi
        n=$((n + 1))
    done
    log "attach: rbp never opened export.pdb after retries"
    return 1
}

# --- mount /dev/sdX1 at $MNT ------------------------------------------------
mount_media() {
    part=$1
    fstype=$2
    mkdir -p "$MNT"
    case "$fstype" in
      vfat)
        # The RX3 kernel had the nls charset modules built in; on a Pi they are
        # modules (fix-dev.sh modprobes them). Retry once without codepage and
        # iocharset: a missing nls module fails the mount with a bare EINVAL, and
        # losing long-name handling beats losing the stick entirely.
        mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8 "/dev/$part" "$MNT" && return 0
        log "attach: vfat mount failed with codepage/iocharset, retrying without"
        mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,usefree "/dev/$part" "$MNT"
        ;;
      exfat)   mount -t exfat -o rw,noatime "/dev/$part" "$MNT" ;;
      hfsplus) mount -t hfsplus -o force,rw,noatime "/dev/$part" "$MNT" ;;
      *)       mount "/dev/$part" "$MNT" ;;
    esac
}

# --- mount + chroot bind + notify -------------------------------------------
attach() {
    dev=$1
    part=$(find_partition "$dev") || { log "attach: no usable partition on $dev"; return 1; }

    if mountpoint -q "$MNT"; then
        log "attach: $MNT already mounted (refreshing bind only)"
    else
        fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
        [ -n "$fstype" ] || fstype=vfat
        if ! mount_media "$part" "$fstype"; then
            log "attach: mount /dev/$part -> $MNT failed (fstype=$fstype)"
            return 1
        fi
        log "attach: mounted /dev/$part ($fstype) -> $MNT"
    fi

    mkdir -p "$CH_MNT"
    if ! mountpoint -q "$CH_MNT"; then
        if ! mount --bind "$MNT" "$CH_MNT"; then
            log "attach: chroot bind $MNT -> $CH_MNT failed"
            return 1
        fi
        log "attach: chroot bind ok ($CH_MNT)"
    fi

    # The bind must exist BEFORE rbp looks at the stick's files (export.pdb and
    # friends). Sending umount first resets rbp's PathDecider state so the mount
    # event is treated as a fresh attach rather than a no-op.
    notify_mount_until_open
    return 0
}

# --- notify rbp + release mounts --------------------------------------------
detach() {
    log "detach: notifying rbp"
    notify "umount $CHROOT_MEDIA_PATH"
    sleep 1
    if mountpoint -q "$CH_MNT"; then umount -l "$CH_MNT"; log "detach: umount -l $CH_MNT"; fi
    if mountpoint -q "$MNT";    then umount -l "$MNT";    log "detach: umount -l $MNT";    fi
    rmdir "$CH_MNT" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
}

run() {
    log "=== usb-watch run: poll ${POLL}s, media device -> $MNT ==="
    cur=""
    last_rbp=$(rbp_pid)

    while :; do
        dev=$(find_media_sd) || dev=""
        rbp=$(rbp_pid)

        if [ -n "$dev" ]; then
            if [ "$dev" != "$cur" ]; then
                [ -n "$cur" ] && detach
                cands=$(list_media_sds | tr '\n' ' ')
                log "attach: detected $dev (candidates: $cands)"
                if attach "$dev"; then
                    cur=$dev
                else
                    cur=""
                fi
            elif [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ]; then
                # rbp restarted underneath us: it has forgotten the mount.
                log "attach: rbp restarted ($last_rbp -> $rbp), re-notifying mount"
                notify_mount_until_open
            fi
        else
            if [ -n "$cur" ]; then
                detach
                cur=""
            fi
        fi
        last_rbp=$rbp
        sleep "$POLL"
    done
}

start() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "already running (pid $(cat "$PIDFILE"))"
        return 0
    fi
    log "=== usb-watch start ==="
    nohup sh "$0" run >/dev/null 2>&1 &
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
    echo "pid:          $(cat "$PIDFILE" 2>/dev/null || echo none)"
    echo "media sd:     $(find_media_sd || echo none)"
    echo "candidates:   $(list_media_sds | tr '\n' ' ')"
    echo "host mount:   $MNT $(mountpoint -q "$MNT" && echo '(mounted)' || echo '(not mounted)')"
    echo "chroot bind:  $CH_MNT $(mountpoint -q "$CH_MNT" && echo '(mounted)' || echo '(not mounted)')"
    echo "rbp pid:      ${_rbp:-none}"
    echo "--- log tail ---"
    # `|| true`: a missing log is not a failure of the status command, and
    # returning 1 there would make `status` look like it had broken.
    tail -15 "$LOG" 2>/dev/null || true
    return 0
}

case "${1:-}" in
  # status is read-only (it prints mounts and a log tail) and is genuinely useful
  # to run unprivileged; everything else mounts, binds or kills.
  status) status ;;
  start)  rb_require_root; start ;;
  stop)   rb_require_root; stop ;;
  run)    rb_require_root; run ;;
  *) echo "usage: $0 start|stop|status" >&2; exit 1 ;;
esac
