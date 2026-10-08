#!/bin/sh
# usb-watch.sh — USB media hotplug -> rbp, on the Raspberry Pi 4.
#
# A USB stick plugged into the Pi must appear to rbp exactly as it would on an
# XDJ-RX3. rbp has TWO media slots, so this script feeds both of them:
#
#   slot 1  mount  /dev/sdX1            -> $RB_MEDIA_MOUNT/sda1        (host side)
#           bind   $RB_MEDIA_MOUNT/sda1 -> $RB_CHROOT/media/usb1/sda1  (chroot view)
#           write  "mount /media/usb1/sda1" -> /tmp/udev_usb1          (rbp's FIFO)
#           write  the stick's volume label -> /tmp/udev_usb1.label    (rbp's row)
#
#   slot 2  mount  /dev/sdY1            -> $RB_MEDIA_MOUNT2/sda1
#           bind   $RB_MEDIA_MOUNT2/sda1 -> $RB_CHROOT/media/usb2/sda1
#           write  "mount /media/usb2/sda1" -> /tmp/udev_usb2
#           write  the stick's volume label -> /tmp/udev_usb2.label
#
# The label file is how the SOURCE screen stops saying "USB1" and "USB2": the
# shim reads it and puts it in rbp's own device-name field. It is written here,
# on the host, because blkid is the only reader on this rig that knows which of
# FAT12/16, FAT32 and exFAT is in front of it -- the volume label's offset in the
# boot sector differs between them, and both of the operator's sticks are FAT32,
# where hand-parsing byte 0x2b reads GPT garbage. It is `lsblk -f`'s LABEL column
# and the same string `blkid -s LABEL` prints; only /dev/disk/by-label escapes the
# space in "RBOX USB" as \x20.
#
# WHICH STICK IS WHICH is by position, not by preference: the candidates in
# /sys/block order, and the first one carrying a rekordbox export takes slot 1.
# That is the operator's own rule -- "the first stick as USB1 and the second as
# USB2" (2026-10-07). A device keeps its slot for as long as it is present, so
# pulling the first stick leaves the second on USB2 rather than renumbering it
# (a renumber would detach and re-attach the same stick, making rbp re-import
# the library for nothing).
#
# The chroot paths are the ones rbp parses out of the FIFO line: measured
# 2026-10-07, ui::UsbMountManager::getMountPath() returns the path the line
# carried, not a compiled literal. /media/usb1/sda1 is nonetheless patch 1 of
# tools/patch-rbp/PATCHES.md and must not change; the host-side mount points are
# the configurable ones.
#
# On detach the slot's FIFO gets "umount", then both of its mounts are released
# lazily.
#
# This mirrors the XDJ-RX3's udev rule 12-usb-memory-auto-mount.rules, which
# writes "mount /media/%E{usb_slot}/%k" to /proc/udev_%E{usb_slot} — one FIFO per
# slot, the path in the text. The mount event alone is enough — never write to
# /tmp/udev_usbctn* ("connect" there makes rbp raise "USB Error. Remove the
# device.").
#
# Usage:  sh usb-watch.sh start|stop|status|run
# Env:    USBWATCH_POLL=1     poll interval seconds (RB_USB_POLL_S)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf
rb_require_chroot

# The two slots. `sda1` is a LABEL, not the device: whatever /dev/sdX was put in
# the slot is mounted here, so the directory name is fixed and the device is not.
# On the previous target the host mount point and the chroot path were the same
# string (/media/usb1/sda1). They are different here, and the distinction
# matters: the FIFO messages below are read by rbp *inside* the chroot, so they
# must carry the chroot-side path, while the mount commands operate on the
# host-side one.
#
# SLOT_COUNT is the loop's view of the above; slot_mnt()/slot_chmnt()/slot_path()/
# slot_fifo() below are the accessors, so nothing below has to spell either slot's
# paths twice.
MNT="${RB_MEDIA_MOUNT:-$RB_DEPLOY_ROOT/media/usb1}/sda1"
CHROOT_MEDIA_PATH="${RB_CHROOT_MEDIA:-/media/usb1/sda1}"
CH_MNT="$RB_CHROOT$CHROOT_MEDIA_PATH"
FIFO=/tmp/udev_usb1
LABEL=/tmp/udev_usb1.label

MNT2="${RB_MEDIA_MOUNT2:-$RB_DEPLOY_ROOT/media/usb2}/sda1"
CHROOT_MEDIA_PATH2="${RB_CHROOT_MEDIA2:-/media/usb2/sda1}"
CH_MNT2="$RB_CHROOT$CHROOT_MEDIA_PATH2"
FIFO2=/tmp/udev_usb2
LABEL2=/tmp/udev_usb2.label

SLOT_COUNT=2
LOG="${RB_LOG_DIR:-$RB_DEPLOY_ROOT/log}/usbwatch.log"
PIDFILE=/tmp/usbwatch.pid
POLL="${USBWATCH_POLL:-${RB_USB_POLL_S:-2}}"
TIMEOUT="${RB_USB_TIMEOUT:-timeout}"
# Scratch mount point for probe_export() below. Deliberately not under $MNT: it
# must never be confused with the slot rbp reads, and it is created and removed
# around every probe.
PROBE_MNT="$RB_DEPLOY_ROOT/probe-mnt"

mkdir -p "$(dirname "$LOG")" 2>/dev/null
log() { echo "$(date '+%F %T') $$ $*" >> "$LOG" 2>/dev/null; }

# --- the slot accessors ------------------------------------------------------
slot_mnt()   { [ "$1" = 2 ] && echo "$MNT2"              || echo "$MNT"; }
slot_chmnt() { [ "$1" = 2 ] && echo "$CH_MNT2"            || echo "$CH_MNT"; }
slot_path()  { [ "$1" = 2 ] && echo "$CHROOT_MEDIA_PATH2" || echo "$CHROOT_MEDIA_PATH"; }
slot_fifo()  { [ "$1" = 2 ] && echo "$FIFO2"              || echo "$FIFO"; }
slot_label() { [ "$1" = 2 ] && echo "$LABEL2"             || echo "$LABEL"; }

# Which /dev node is mounted at a mount point right now (read-only; used by
# status() so that reporting never probes, and so that the readiness witness does
# not have to trust a variable).
mount_src() { awk -v m="$1" '$2 == m { print $1; exit }' /proc/mounts 2>/dev/null; }

# --- locate the media block devices -----------------------------------------
#
# The previous target hardcoded bus numbers ("usb1 usb2 usb3") because its media
# port was a known controller. A Pi has no such mapping: every Type-A port hangs
# off one PCIe-attached xHCI controller plus the USB-C/OTG port on another, and
# the bus numbers vary by model and firmware. So the test is structural — a device
# is media if its sysfs path passes through a USB bus.
#
# The order of this list is the slot order, which makes it part of the contract
# and not just an enumeration: /sys/block/sd* sorts as sda, sdb, ..., so at a cold
# start the first stick the kernel brought up is USB1.
list_media_sds() {
    for blk in /sys/block/sd*; do
        [ -e "$blk" ] || continue
        tgt=$(readlink -f "$blk" 2>/dev/null) || continue
        case "$tgt" in *"/usb"[0-9]*) ;; *) continue ;; esac
        echo "${blk##*/}"
    done
}

# --- wait for the first partition; fall back to a whole-disk filesystem ------
find_partition() {
    fpdev=$1
    i=0
    while [ "$i" -lt 40 ]; do                 # up to 4 s (0.1 s steps)
        [ -b "/dev/${fpdev}1" ] && { echo "${fpdev}1"; return 0; }
        i=$((i + 1))
        sleep 0.1
    done
    if blkid "/dev/$fpdev" >/dev/null 2>&1; then echo "$fpdev"; return 0; fi
    return 1
}

# --- can rbp actually read this device? --------------------------------------
#
# A device only ever takes a slot if it carries PIONEER/rekordbox/export.pdb.
# That single rule replaces the removable-bit preference that used to pick the
# device, and it fixes what that preference got wrong on this rig: the operator's
# library is an SSD enclosure that reports removable=0, so any cheap thumb drive
# out-ranked it. The damage was not only a wrong pick — a stick with no export
# mounted into the slot made notify_mount_until_open() flap umount/mount forever,
# because rbp never opens a DB that is not there, and the source then vanished
# from the UI entirely. "A second stick" read as "USB1 disappeared" (measured
# 2026-10-06). An export-less device now takes no slot at all.
#
# Mounted READ-ONLY and only ever looked at. The operator's library carries
# pre-existing FAT damage and nothing in this script may write to it; a read-only
# vfat mount cannot, and it needs no repair to succeed. Every failure -- no
# partition, unknown filesystem, nls module missing -- answers "no", which is the
# safe direction: an unreadable candidate never displaces a device that is
# already mounted and playing.
#
# Variable names are deliberately not the ones attach() uses: this is called from
# inside the poll loop, and sh has no local.
probe_export() {
    pdev=$1
    ppart=$(find_partition "$pdev") || return 1
    mkdir -p "$PROBE_MNT" 2>/dev/null || return 1
    pfstype=$(blkid -s TYPE -o value "/dev/$ppart" 2>/dev/null)
    [ -n "$pfstype" ] || pfstype=vfat
    if ! mount -t "$pfstype" -o ro "/dev/$ppart" "$PROBE_MNT" 2>/dev/null; then
        rmdir "$PROBE_MNT" 2>/dev/null
        return 1
    fi
    [ -e "$PROBE_MNT/PIONEER/rekordbox/export.pdb" ]
    prc=$?
    umount "$PROBE_MNT" 2>/dev/null
    rmdir "$PROBE_MNT" 2>/dev/null
    return $prc
}

# A device whose partition is already serving a slot has answered the question
# already: do not mount it a second time just to ask again.
dev_serves_slot() {
    awk -v a="$MNT" -v b="$MNT2" -v re="^/dev/$1[0-9]*\$" \
        '($2 == a || $2 == b) && $1 ~ re { found = 1 } END { exit !found }' \
        /proc/mounts 2>/dev/null
}

# --- slot assignment ---------------------------------------------------------
#
# Verdicts are memoised per epoch, because probe_export() MOUNTS: it must not run
# on every ${POLL}s tick. A "yes" and a "no" are both re-asked at the next epoch,
# which is cheap for a device already serving a slot (dev_serves_slot answers
# without mounting) and gives a device that was still enumerating when its epoch
# began another chance rather than writing it off for the life of the plug.
slot1=""
slot2=""
ASSIGN_CANDS="none"    # never a real candidate list: forces the first pass
ASSIGN_YES=""
ASSIGN_NO=""
ASSIGN_TRIES=0
ASSIGN_EPOCH=30        # polls per epoch ~= 60 s at POLL=2
candline=""

# "present AND carrying a rekordbox export?"
dev_exported() {
    # The empty string must be refused explicitly: the lists below are matched as
    # " x y " with surrounding spaces, so an empty name would match a double space
    # and answer "yes" for a device that does not exist. That is not theoretical —
    # it put the same stick in both slots on the first live run (2026-10-07).
    [ -n "$1" ] || return 1
    case " $candline " in *" $1 "*) ;; *) return 1 ;; esac
    case " $ASSIGN_YES " in *" $1 "*) return 0 ;; esac
    case " $ASSIGN_NO "  in *" $1 "*) return 1 ;; esac
    if dev_serves_slot "$1" || probe_export "$1"; then
        ASSIGN_YES="$ASSIGN_YES $1"
        return 0
    fi
    ASSIGN_NO="$ASSIGN_NO $1"
    return 1
}

assign_slots() {
    cands=$(list_media_sds)
    candline=$(echo "$cands" | tr '\n' ' ')

    if [ "$candline" != "$ASSIGN_CANDS" ]; then
        ASSIGN_CANDS=$candline
        ASSIGN_YES=""
        ASSIGN_NO=""
        ASSIGN_TRIES=0
    fi
    ASSIGN_TRIES=$((ASSIGN_TRIES + 1))
    if [ "$ASSIGN_TRIES" -ge "$ASSIGN_EPOCH" ]; then
        ASSIGN_YES=""
        ASSIGN_NO=""
        ASSIGN_TRIES=0
    fi

    # Sticky, and evaluated slot 1 first -- that ordering is what makes a lone
    # stick USB1 rather than USB2. A device that held a slot keeps it while it is
    # present; a free slot is filled by the first export-bearing candidate that is
    # not the other slot's holder. So pulling the first stick empties USB1 and
    # leaves the second exactly where it was, while plugging a new one in beside a
    # lone second-slot stick fills USB1.
    prev1=$slot1
    prev2=$slot2

    slot1=""
    if dev_exported "$prev1"; then
        slot1=$prev1
    else
        for d in $cands; do
            [ "$d" = "$prev2" ] && continue
            dev_exported "$d" || continue
            slot1=$d
            break
        done
    fi

    slot2=""
    if [ "$prev2" != "$slot1" ] && dev_exported "$prev2"; then
        slot2=$prev2
    else
        for d in $cands; do
            [ "$d" = "$slot1" ] && continue
            dev_exported "$d" || continue
            slot2=$d
            break
        done
    fi
}

# --- tell rbp about a USB event (FIFO; rbp holds it open O_RDWR) -------------
notify() {
    argfifo=$1
    argmsg=$2
    if [ ! -p "$argfifo" ]; then
        log "notify: $argfifo missing (rbp down?) — skipping"
        return 1
    fi
    # The write must not block forever if rbp is gone: the FIFO has no reader, and
    # opening it for write blocks until one appears.
    if "$TIMEOUT" 3 sh -c 'printf "%s" "$1" > "$2"' sh "$argmsg" "$argfifo" 2>/dev/null; then
        log "notify: $argmsg"
        return 0
    fi
    log "notify: FAILED to write '$argmsg' (rbp down?)"
    return 1
}

rbp_pid() { pids_matching "$RB_PLAYER" strace | head -1; }

# --- send umount+then mount, retrying until rbp opens the DB ----------------
# rbp's DeviceSQL channel takes several seconds to come up after a (re)start, and
# a mount event sent before that is silently lost. Keep re-notifying until rbp
# actually opens *this slot's* export.pdb, which means its analysis has started.
#
# The witness must name the slot: rbp holds both slots' DBs open at once, and
# /proc/<pid>/fd resolves those targets from *outside* the chroot, so slot 2's
# live fd would otherwise make slot 1 look ready (and vice versa). The path read
# here is therefore the chroot bind, $CH_MNT, which is what rbp's fd shows from
# here — measured 2026-10-07.
notify_mount_until_open() {
    nslot=$1
    nfifo=$(slot_fifo "$nslot")
    npath=$(slot_path "$nslot")
    nwant="$(slot_chmnt "$nslot")/PIONEER/rekordbox/export.pdb"
    n=0
    while [ "$n" -lt 12 ]; do
        sleep 4
        notify "$nfifo" "umount $npath"
        sleep 0.3
        notify "$nfifo" "mount $npath"
        sleep 4
        nmrbp=$(rbp_pid)
        if [ -n "$nmrbp" ] && ls -l "/proc/$nmrbp/fd" 2>/dev/null | grep -Fq "$nwant"; then
            log "attach: slot $nslot: rbp opened export.pdb (notify attempt $n)"
            return 0
        fi
        n=$((n + 1))
    done
    log "attach: slot $nslot: rbp never opened export.pdb after retries"
    return 1
}

# --- mount /dev/sdX1 at a slot's host mount point ---------------------------
mount_media() {
    mpart=$1
    mfstype=$2
    mmnt=$3
    mkdir -p "$mmnt"
    case "$mfstype" in
      vfat)
        # The RX3 kernel had the nls charset modules built in; on a Pi they are
        # modules (fix-dev.sh modprobes them). Retry once without codepage and
        # iocharset: a missing nls module fails the mount with a bare EINVAL, and
        # losing long-name handling beats losing the stick entirely.
        mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8 "/dev/$mpart" "$mmnt" && return 0
        log "attach: vfat mount failed with codepage/iocharset, retrying without"
        mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,usefree "/dev/$mpart" "$mmnt"
        ;;
      exfat)   mount -t exfat -o rw,noatime "/dev/$mpart" "$mmnt" ;;
      hfsplus) mount -t hfsplus -o force,rw,noatime "/dev/$mpart" "$mmnt" ;;
      *)       mount "/dev/$mpart" "$mmnt" ;;
    esac
}

# --- mount + chroot bind + notify -------------------------------------------
attach() {
    aslot=$1
    adev=$2
    apart=$(find_partition "$adev") || { log "attach: slot $aslot: no usable partition on $adev"; return 1; }
    amnt=$(slot_mnt "$aslot")
    achmnt=$(slot_chmnt "$aslot")

    # The slot's mount points must be freed first if they are holding some OTHER
    # device. A bind mount captures the mount it was made from, so mounting the
    # wanted stick over $amnt would leave $achmnt still showing the old contents —
    # and a leftover mount from a previous run is exactly how the same stick ended
    # up in both slots on the first live run (2026-10-07).
    acur=$(mount_src "$amnt")
    case "$acur" in
      "") ;;
      /dev/${adev}|/dev/${adev}[0-9]*)
        log "attach: slot $aslot: $amnt already mounted ($acur), refreshing bind only" ;;
      *)
        log "attach: slot $aslot: $amnt holds $acur, not $adev — releasing the slot"
        umount -l "$achmnt" 2>/dev/null
        umount -l "$amnt"   2>/dev/null ;;
    esac

    if ! mountpoint -q "$amnt"; then
        afstype=$(blkid -s TYPE -o value "/dev/$apart" 2>/dev/null)
        [ -n "$afstype" ] || afstype=vfat
        if ! mount_media "$apart" "$afstype" "$amnt"; then
            log "attach: slot $aslot: mount /dev/$apart -> $amnt failed (fstype=$afstype)"
            return 1
        fi
        log "attach: slot $aslot: mounted /dev/$apart ($afstype) -> $amnt"
    fi

    mkdir -p "$achmnt"
    if ! mountpoint -q "$achmnt"; then
        if ! mount --bind "$amnt" "$achmnt"; then
            log "attach: slot $aslot: chroot bind $amnt -> $achmnt failed"
            return 1
        fi
        log "attach: slot $aslot: chroot bind ok ($achmnt)"
    fi

    # The stick's own name, for rbp's device row (the shim reads this file from
    # inside the chroot -- /tmp is shared). Written only once the bind is up, and
    # removed in detach(), so it is never a name without a device behind it: the
    # shim takes an empty or absent file as "leave rbp's own USB1/USB2 alone".
    # A device with no volume label is the same case, which is why the empty
    # answer removes the file rather than writing an empty one.
    alabel=$(blkid -s LABEL -o value "/dev/$apart" 2>/dev/null)
    if [ -n "$alabel" ]; then
        printf '%s' "$alabel" > "$(slot_label "$aslot")" 2>/dev/null || true
        log "attach: slot $aslot: label '$alabel'"
    else
        rm -f "$(slot_label "$aslot")"
        log "attach: slot $aslot: /dev/$apart has no volume label"
    fi

    # The bind must exist BEFORE rbp looks at the stick's files (export.pdb and
    # friends). Sending umount first resets rbp's PathDecider state so the mount
    # event is treated as a fresh attach rather than a no-op.
    notify_mount_until_open "$aslot"
    return 0
}

# --- notify rbp + release mounts --------------------------------------------
detach() {
    dslot=$1
    dmnt=$(slot_mnt "$dslot")
    dchmnt=$(slot_chmnt "$dslot")
    log "detach: slot $dslot: notifying rbp"
    # The name goes before the umount, not after: the shim clears rbp's field on
    # its next 100 ms tick either way, and a name that outlives its device is the
    # one thing this file must never be.
    rm -f "$(slot_label "$dslot")"
    notify "$(slot_fifo "$dslot")" "umount $(slot_path "$dslot")"
    sleep 1
    if mountpoint -q "$dchmnt"; then umount -l "$dchmnt"; log "detach: slot $dslot: umount -l $dchmnt"; fi
    if mountpoint -q "$dmnt";    then umount -l "$dmnt";    log "detach: slot $dslot: umount -l $dmnt";    fi
    rmdir "$dchmnt" 2>/dev/null
    rmdir "$dmnt" 2>/dev/null
}

# --- the loop ---------------------------------------------------------------
#
# What is in each slot and what is attached to it are tracked separately
# (slot1/slot2 vs cur1/cur2): a slot whose device is unchanged is left completely
# alone, so a stable pair costs nothing per tick.
cur1=""
cur2=""
slot_cur()     { [ "$1" = 2 ] && echo "$cur2" || echo "$cur1"; }
slot_set_cur() { if [ "$1" = 2 ]; then cur2=$2; else cur1=$2; fi; }
slot_dev()     { [ "$1" = 2 ] && echo "$slot2" || echo "$slot1"; }

run() {
    log "=== usb-watch run: poll ${POLL}s, slots: usb1 -> $MNT, usb2 -> $MNT2 ==="
    cur1=""
    cur2=""
    last_rbp=$(rbp_pid)

    while :; do
        assign_slots
        rbp=$(rbp_pid)
        restarted=0
        [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ] && restarted=1

        # `sn`, not `n`: notify_mount_until_open() uses `n` for its own retry
        # counter and sh has no locals, so a shared name would reset this loop.
        sn=1
        while [ "$sn" -le "$SLOT_COUNT" ]; do
            want=$(slot_dev "$sn")
            have=$(slot_cur "$sn")

            if [ -n "$want" ]; then
                if [ "$want" != "$have" ]; then
                    [ -n "$have" ] && detach "$sn"
                    log "attach: slot $sn <- $want"
                    if attach "$sn" "$want"; then
                        slot_set_cur "$sn" "$want"
                    else
                        slot_set_cur "$sn" ""
                    fi
                elif [ "$restarted" = 1 ]; then
                    # rbp restarted underneath us: it has forgotten the mount.
                    log "attach: rbp restarted ($last_rbp -> $rbp), re-notifying slot $sn"
                    notify_mount_until_open "$sn"
                fi
            elif [ -n "$have" ]; then
                detach "$sn"
                slot_set_cur "$sn" ""
            fi

            sn=$((sn + 1))
        done

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

# Read-only: every line comes from /proc/mounts or a sysfs listing, so this never
# probes (probe_export mounts) and is safe to run unprivileged.
status() {
    _rbp=$(rbp_pid)
    _s1=$(mount_src "$MNT");  [ -n "$_s1" ] || _s1=none
    _s2=$(mount_src "$MNT2"); [ -n "$_s2" ] || _s2=none
    echo "pid:          $(cat "$PIDFILE" 2>/dev/null || echo none)"
    echo "slot 1:       $_s1 -> $MNT"
    echo "slot 2:       $_s2 -> $MNT2"
    echo "label 1:      $(cat "$LABEL" 2>/dev/null || echo none)"
    echo "label 2:      $(cat "$LABEL2" 2>/dev/null || echo none)"
    echo "candidates:   $(list_media_sds | tr '\n' ' ')"
    echo "host mount 1: $MNT $(mountpoint -q "$MNT" && echo '(mounted)' || echo '(not mounted)')"
    echo "chroot bind 1:$CH_MNT $(mountpoint -q "$CH_MNT" && echo '(mounted)' || echo '(not mounted)')"
    echo "host mount 2: $MNT2 $(mountpoint -q "$MNT2" && echo '(mounted)' || echo '(not mounted)')"
    echo "chroot bind 2:$CH_MNT2 $(mountpoint -q "$CH_MNT2" && echo '(mounted)' || echo '(not mounted)')"
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
