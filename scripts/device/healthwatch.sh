#!/bin/bash
# healthwatch.sh -- record *why* the unit wedges.
#
# The failure this exists for: the box stays pingable and answers mDNS, TCP :22
# accepts, but sshd never sends a banner -- and the journal simply stops. Both
# occurrences so far ended only with a power cycle and left nothing usable: the
# journal dies at the same instant, so any journal-based logger goes blind exactly
# when it is needed, and the device's own logs (displaywatch, usbwatch, rbp.log)
# only write on events, so an idle wedge leaves no trail in them either.
#
# So this writes to DISK (/opt/rblive4/log/health.log -- survives a reboot) and to
# TMPFS (/tmp/health.log -- survives a disk stall; read it *before* rebooting), and
# it probes the symptom itself with a loopback connection to :22, watching for the
# banner. The first round that fails while the previous one passed is the moment of
# the wedge, and a full snapshot is written then, from inside the box.
#
# Every round carries its own sequence number, so a GAP in the log is itself
# evidence: it means this process was not scheduled either.
#
# Fields, one line per round:
#   seq=<n> up=<s> load=<l1/l5/l15> run=<runnable>/<total>  the box
#   mem=<avail> pids=<n>                                   memory and process count
#   rbp=<pid>:<ticks>                                      the player and its CPU
#   snd=<n> usb=<n>                                        is the FLX4 still there
#   jlag=<s>                                               since journald last wrote
#   disk=<ms>/<ms>                                         write+fsync on / and /opt
#   ssh=<OK|NOBANNER|FAIL>                                 every SSH_EVERY rounds
#   kerr=<n> thr=<hex>                                     kernel complaints, power
#
# Knobs: HEALTH_PERIOD (default 15 s), HEALTH_SSH_EVERY (default 4 -> every 60 s).
# The ssh probe is deliberately sparse: each one forks an sshd-session and leaves a
# "Connection closed ... [preauth]" line in the journal.

LOG=/opt/rblive4/log/health.log
TMPLOG=/tmp/health.log
INCDIR=/opt/rblive4/log
SCRATCH_ROOT=$INCDIR/.health-scratch-root
SCRATCH_OPT=$INCDIR/.health-scratch-opt

PERIOD=${HEALTH_PERIOD:-15}
SSH_EVERY=${HEALTH_SSH_EVERY:-4}

now_iso() { date '+%Y-%m-%dT%H:%M:%S%z'; }

say() {
  printf '%s\n' "$1" >> "$LOG"
  printf '%s\n' "$1" >> "$TMPLOG"
  sync "$LOG" 2>/dev/null
}

rotate() {
  local n
  n=$(stat -c %s "$LOG" 2>/dev/null) || return 0
  if [ "$n" -gt 2000000 ]; then
    mv -f "$LOG" "$LOG.1" 2>/dev/null
    say "$(now_iso) [rotated: health.log passed 2 MB]"
  fi
}

# Seconds since journald last wrote. Grows without bound if journald dies, which
# is the second half of the signature and separates "logging died" from "the whole
# box died".
journal_lag() {
  local f m
  f=$(ls -t /var/log/journal/*/system.journal 2>/dev/null | head -1)
  [ -n "$f" ] || { printf 'na'; return; }
  m=$(stat -c %Y "$f" 2>/dev/null)
  [ -n "$m" ] || { printf 'na'; return; }
  printf '%s' "$(( $(date +%s) - m ))"
}

# Write + fsync one byte, and time it. This is the storage-stall detector: on a
# healthy SD it is 1-3 ms, and if the card stalls it becomes seconds.
disk_ms() {
  local t0 t1
  t0=$(date +%s%N)
  printf 'x' >> "$1" 2>/dev/null
  sync "$1" 2>/dev/null
  t1=$(date +%s%N)
  printf '%s' "$(( (t1 - t0) / 1000000 ))"
}

# The symptom probe: the same path the operator's ssh takes, but over loopback, so
# it measures sshd itself and not the network.
ssh_probe() {
  local line=""
  exec 3<>/dev/tcp/127.0.0.1/22 2>/dev/null || { printf 'FAIL:connect'; return; }
  IFS= read -r -t 5 -u 3 line
  exec 3<&- 3>&- 2>/dev/null
  case "$line" in
    SSH-*) printf 'OK' ;;
    '')    printf 'NOBANNER' ;;
    *)     printf 'ODD' ;;
  esac
}

# rbp's comm is "ld-linux.so.3", not "rbp" -- the player is launched through the
# loader, so `pgrep -x rbp` matches nothing at all and reads as "the player is not
# running" while it is.
#
# The match walks /proc rather than shelling out to `pgrep -f`, and deliberately
# builds no literal path. start-rb.sh's cleanup() cmdline-matches every process on
# every restart, so a probe whose OWN command line names the player path can be
# killed mid-round. Walking /proc with bash builtins names nothing, and costs no
# forks. edb_streamd shares the loader, so the second cmdline word must end in
# "/rbp" -- it is at /usr/bin/edb_streamd.
#
# mapfile -d '' rather than $(< cmdline): a command substitution silently DROPS the
# NUL bytes that separate the words, which concatenates them into one string that
# matches nothing. That bug reads as "the player is not running" while it is, which
# is the same false negative comm got.
rbp_stat() {
  local q c s out="" w
  for q in /proc/[0-9]*; do
    [ -r "$q/comm" ] || continue
    c=$(< "$q/comm")
    [ "$c" = "ld-linux.so.3" ] || continue
    w=(); mapfile -d '' -t w < "$q/cmdline" 2>/dev/null
    case "${w[1]-}" in */rbp) ;; *) continue ;; esac
    s=$(awk '{print $14 + $15}' "$q/stat" 2>/dev/null)
    [ -n "$s" ] && out="$out${q#/proc/}:$s,"
  done
  printf '%s' "${out:-none}"
}

snapshot() {
  local f="$INCDIR/health-incident-$(date +%Y%m%d-%H%M%S).log"
  {
    echo "=== healthwatch incident $(now_iso) : the sshd banner stopped ==="
    echo "--- /proc/loadavg"; cat /proc/loadavg 2>/dev/null
    echo "--- memory"; grep -E 'MemTotal|MemFree|MemAvailable|SwapFree' /proc/meminfo 2>/dev/null
    echo "--- top CPU (pid ppid stat pcpu etime comm)"
    ps -eo pid,ppid,stat,pcpu,etime,comm --sort=-pcpu 2>/dev/null | head -20
    echo "--- sshd (pid ppid stat wchan etime cmd)"
    ps -eo pid,ppid,stat,wchan:20,etime,args 2>/dev/null | grep -E 'sshd|COMMAND' | head -15
    echo "--- rbp (pid stat wchan etime threads)"
    ps -eLo pid,stat,wchan:20,etime,nlwp,comm 2>/dev/null | grep -E 'rbp|COMMAND' | head -15
    echo "--- all D-state (uninterruptible) processes"
    ps -eo pid,stat,wchan:24,comm 2>/dev/null | awk '$2 ~ /^D/'
    echo "--- sockets"; ss -tnp 2>/dev/null | head -20
    echo "--- disk latency now"
    printf '    / = %s ms, /opt = %s ms\n' "$(disk_ms "$SCRATCH_ROOT")" "$(disk_ms "$SCRATCH_OPT")"
    echo "--- dmesg tail"; dmesg 2>/dev/null | tail -60
    echo "--- vmstat 1 3"; vmstat 1 3 2>/dev/null
    echo "--- throttled"; vcgencmd get_throttled 2>/dev/null || echo na
    echo "=== end incident ==="
  } >> "$f" 2>&1
  cp -f "$f" "$INCDIR/health-incident-LATEST.log" 2>/dev/null
  cat "$f" >> "$TMPLOG" 2>/dev/null
  echo "  snapshot -> $f"
}

main() {
  # one instance only
  exec 9>"$INCDIR/.health.lock" 2>/dev/null
  flock -n 9 2>/dev/null || { echo "healthwatch: another instance is running"; exit 0; }

  mkdir -p "$INCDIR" 2>/dev/null
  : > "$SCRATCH_ROOT" 2>/dev/null
  : > "$SCRATCH_OPT" 2>/dev/null

  say "$(now_iso) === healthwatch started (period ${PERIOD}s, ssh probe every ${SSH_EVERY} rounds) ==="
  say "$(now_iso) fields: seq up load run mem pids rbp snd usb jlag disk ssh kerr thr"

  local seq=0 prev_ssh="OK"
  while :; do
    seq=$((seq + 1))
    rotate

    local up load l1 l5 l15 runnable mem pids snd usb thr kerr
    up=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    read -r l1 l5 l15 runnable _ < /proc/loadavg 2>/dev/null
    mem=$(awk '/^MemAvailable/{print int($2/1024)"M"}' /proc/meminfo 2>/dev/null)
    pids=$(ls /proc 2>/dev/null | grep -c '^[0-9]*$')
    snd=$(ls /dev/snd 2>/dev/null | grep -c '^controlC')
    usb=$(ls /sys/bus/usb/devices 2>/dev/null | grep -cE '^[0-9]+-[0-9.]+$')
    thr=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2); [ -n "$thr" ] || thr=na
    kerr=$(dmesg 2>/dev/null | grep -ciE 'mmc[0-9].*(error|timeout)|I/O error|EXT4-fs error|blk_update_request')

    # The disk probes are the slowest part and the only ones that can block; if the
    # log stops mid-round with no line, that is the finding.
    local d_root d_opt
    d_root=$(disk_ms "$SCRATCH_ROOT")
    d_opt=$(disk_ms "$SCRATCH_OPT")

    local ssh_field=""
    if [ $((seq % SSH_EVERY)) -eq 1 ]; then
      local r
      r=$(ssh_probe)
      ssh_field=" ssh=$r"
      if [ "$r" != "OK" ] && [ "$prev_ssh" = "OK" ]; then
        say "$(now_iso) seq=$seq !!! SSHD STOPPED ANSWERING (probe=$r) -- taking a snapshot"
        snapshot
      fi
      prev_ssh=$r
    fi

    say "$(now_iso) seq=$seq up=$up load=$l1/$l5/$l15 run=$runnable mem=$mem pids=$pids rbp=$(rbp_stat) snd=$snd usb=$usb jlag=$(journal_lag) disk=$d_root/$d_opt${ssh_field} kerr=$kerr thr=$thr"
    sleep "$PERIOD"
  done
}

main "$@"
