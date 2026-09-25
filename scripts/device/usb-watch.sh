#!/bin/sh
# =============================================================================
# usb-watch.sh — SC Live 4 USB-A media port hotplug -> rbp (rekordbox player)
#
# Adapted from PrimeBox. On the SC Live 4 the media USB-A port(s) enumerate on
# the DWC OTG controller (usb1): a stick plugged in the back appears as
#   /sys/block/sda -> .../platform/ff540000.usb/usb1/1-1/.../block/sda
# (the control surface is a MIDI UART on card 0, NOT USB, so usb1 is free for
# media).  usb2 (EHCI) / usb3 (OHCI) are watched too for the other ports.
#
# On attach (any mass-storage device appears on a watched controller):
#   mount  /dev/sdX1 -> /media/usb1/sda1            (RX3-style vfat options)
#   bind   /media/usb1/sda1 -> /data/rbx3-run/media/usb1/sda1  (chroot view)
#   write  "mount /media/usb1/sda1" -> /tmp/udev_usb1          (rbp FIFO)
# On detach:
#   write  "umount /media/usb1/sda1" -> /tmp/udev_usb1
#   umount -l both mounts
#
# This mirrors the XDJ-RX3 udev rule 12-usb-memory-auto-mount.rules.
# The mount event alone is sufficient — never write to /tmp/udev_usbctn*
# ("connect" there triggers rbp's "USB Error. Remove the device." popup).
#
# Usage:  sh /data/usb-watch.sh start|stop|status|run
# Env:    USBWATCH_BUSES="1 2 3"   (sysfs usbN controllers to watch)
#         USBWATCH_POLL=1          (poll interval seconds)
# =============================================================================

MNT=/media/usb1/sda1
CH_MNT=/data/rbx3-run/media/usb1/sda1
FIFO=/tmp/udev_usb1
LOG=/data/usbwatch.log
PIDFILE=/tmp/usbwatch.pid
BUSES="${USBWATCH_BUSES:-1 2 3}"
POLL="${USBWATCH_POLL:-1}"
TIMEOUT=/data/timeout

log() { echo "$(date '+%F %T') $$ $*" >> "$LOG"; }

# --- locate the sd block device of a USB mass-storage device ----------------
find_media_sd() {
  for blk in /sys/block/sd*; do
    [ -e "$blk" ] || continue
    tgt=$(readlink "$blk" 2>/dev/null) || continue
    for b in $BUSES; do
      case "$tgt" in
        *"/usb$b/"*) echo "${blk##*/}"; return 0 ;;
      esac
    done
  done
  return 1
}

# --- wait for the first partition; fall back to whole-disk filesystem ------
find_partition() {
  dev=$1
  i=0
  while [ $i -lt 40 ]; do                 # up to 4 s (0.1 s steps)
    [ -b "/dev/${dev}1" ] && { echo "${dev}1"; return 0; }
    i=$((i + 1)); sleep 0.1
  done
  if blkid "/dev/$dev" >/dev/null 2>&1; then echo "$dev"; return 0; fi
  return 1
}

# --- tell rbp about a USB event (FIFO; rbp keeps it open O_RDWR) -----------
notify() {
  msg=$1
  if [ ! -p "$FIFO" ]; then
    log "notify: $FIFO missing (rbp down?) — skipping"
    return 1
  fi
  if "$TIMEOUT" 3 sh -c 'printf "%s" "$1" > "$2"' sh "$msg" "$FIFO" 2>/dev/null; then
    log "notify: $msg"
    return 0
  fi
  log "notify: FAILED to write '$msg' (rbp down?)"
  return 1
}

# --- send umount+then mount, retrying until rbp opens the DB ---------------
# rbp's DeviceSQL channel takes several seconds to come up after a (re)start;
# a mount event sent before that is silently lost. Keep re-notifying until
# rbp actually opens export.pdb (the analysis has started).
notify_mount_until_open() {
  n=0
  while [ $n -lt 12 ]; do
    sleep 4
    notify "umount $MNT"
    sleep 0.3
    notify "mount $MNT"
    sleep 4
    rbp=$(rbp_pid)
    if [ -n "$rbp" ] && ls -l /proc/$rbp/fd 2>/dev/null | grep -q "export.pdb"; then
      log "attach: rbp opened export.pdb (notify attempt $n)"
      return 0
    fi
    n=$((n + 1))
  done
  log "attach: rbp never opened export.pdb after retries"
  return 1
}

# --- mount + chroot bind + notify ------------------------------------------
attach() {
  dev=$1
  part=$(find_partition "$dev") || { log "attach: no usable partition on $dev"; return 1; }

  if mountpoint -q "$MNT"; then
    log "attach: $MNT already mounted (refreshing bind only)"
  else
    fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
    [ -n "$fstype" ] || fstype=vfat
    mkdir -p "$MNT"
    case "$fstype" in
      vfat)    mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8 "/dev/$part" "$MNT" ;;
      exfat)   mount -t exfat -o rw,noatime "/dev/$part" "$MNT" ;;
      hfsplus) mount -t hfsplus -o force,rw,noatime "/dev/$part" "$MNT" ;;
      *)       mount "/dev/$part" "$MNT" ;;
    esac
    rc=$?
    if [ $rc -ne 0 ]; then
      log "attach: mount /dev/$part -> $MNT failed rc=$rc"
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

  # bind must exist BEFORE rbp checks the stick's files (export.pdb etc.).
  # Reset PathDecider state with umount first, then send the native mount
  # notification, retrying until rbp opens the DB (its DeviceSQL channel needs
  # time to come up after a restart).
  notify_mount_until_open
  return 0
}

# --- notify rbp + release mounts ---------------------------------------------
detach() {
  log "detach: notifying rbp"
  notify "umount $MNT"
  sleep 1
  if mountpoint -q "$CH_MNT"; then umount -l "$CH_MNT"; log "detach: umount -l $CH_MNT"; fi
  if mountpoint -q "$MNT";    then umount -l "$MNT";    log "detach: umount -l $MNT";    fi
  rmdir "$CH_MNT" 2>/dev/null
  rmdir "$MNT" 2>/dev/null
}

rbp_pid() { ps w | awk '/\/root\/pdj\/rbp/ && !/sh -c/ && !/strace/ && !/awk/ {print $1; exit}'; }

run() {
  log "=== usb-watch run: watching buses [$BUSES], poll ${POLL}s ==="
  cur=""
  last_rbp=$(rbp_pid)

  while :; do
    dev=$(find_media_sd) || dev=""
    rbp=$(rbp_pid)

    if [ -n "$dev" ]; then
      if [ "$dev" != "$cur" ]; then
        [ -n "$cur" ] && detach
        log "attach: detected $dev on watched port"
        if attach "$dev"; then
          cur=$dev
        else
          cur=""
        fi
      elif [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ]; then
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
  echo "pid: $(cat "$PIDFILE" 2>/dev/null || echo none)"
  echo "media sd: $(find_media_sd || echo none)"
  echo "mounted: $(mountpoint -q "$MNT" && echo yes || echo no)"
  echo "chroot bind: $(mountpoint -q "$CH_MNT" && echo yes || echo no)"
  echo "--- log tail ---"
  tail -15 "$LOG" 2>/dev/null
}

case "$1" in
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  run)    run ;;
  *) echo "usage: $0 start|stop|status"; exit 1 ;;
esac
