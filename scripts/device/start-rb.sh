#!/bin/sh
# start-rb.sh — launch the XDJ-RX3 rekordbox player on the Denon SC Live 4.
# Adapted from PrimeBox (the SC Live 4 runs the same Buildroot/systemd base).

# 1. Stop Engine OS + disk service (releases audio, USB, controls)
systemctl stop engine.service edisksd.service 2>/dev/null
sleep 1

# 2. Kill any stale rb processes
for p in $(ps w | awk '$0 ~ /[s]trace|[r]oot\/pdj\/[r]bp|[e]db_streamd|[g]dbserver|[u]sb-watch/ {print $1}'); do
    kill -9 $p 2>/dev/null
done
sleep 1

# 3. Setup device binds, stubs, and FIFOs
sh /data/fix-dev.sh

# 4. Deploy binary and shims into the chroot
cp /data/rbp-audio /data/rbx3-run/root/pdj/rbp
chmod 755 /data/rbx3-run/root/pdj/rbp

cp /data/knobshim2.so /data/rbx3-run/root/pdj/knobshim.so
cp /data/knobshim2.so /data/rbx3-run/usr/lib/knobshim.so
chmod 755 /data/rbx3-run/usr/lib/knobshim.so /data/rbx3-run/root/pdj/knobshim.so

cp /data/audioshim.so /data/rbx3-run/root/pdj/audioshim.so
cp /data/audioshim.so /data/rbx3-run/usr/lib/audioshim.so
chmod 755 /data/rbx3-run/usr/lib/audioshim.so /data/rbx3-run/root/pdj/audioshim.so

cp /data/fbshim-tsc.so /data/rbx3-run/root/pdj/fbshim.so
cp /data/fbshim-tsc.so /data/rbx3-run/usr/lib/fbshim.so
chmod 755 /data/rbx3-run/usr/lib/fbshim.so /data/rbx3-run/root/pdj/fbshim.so

cp /data/libdirectfb_fbdev-rot16.so /data/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
chmod 755 /data/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so

# 5. Clean stale IPC and logs
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer /tmp/knobshim.log /tmp/audioshim.log /tmp/dfbdig*.log /tmp/rot_surface.dump
rm -f /data/rbp-p.log

# 6. Start EDB daemon inside chroot
export EDB_BIN=/usr/bin
nohup chroot /data/rbx3-run /lib/ld-linux.so.3 /usr/bin/edb_streamd > /data/edb_d.log 2>&1 &
sleep 1

# 7. Stop USB watcher during startup
sh /data/usb-watch.sh stop 2>/dev/null

# 8. Start rbp cleanly inside chroot
nohup chroot /data/rbx3-run env PATH=/bin:/sbin:/usr/bin:/usr/sbin DFB_ROTATE=left STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 JOG_VERBOSE=1 TEMPO_VERBOSE=1 LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so /lib/ld-linux.so.3 /root/pdj/rbp -a </dev/null >/data/rbp-p.log 2>&1 &

echo "launched rbp, waiting for initialization..."
RBP=""
for i in $(seq 1 30); do
  RBP=$(ps w | awk '/\/root\/pdj\/rbp/ && !/sh -c/ && !/awk/ {print $1; exit}')
  if [ -n "$RBP" ] && ls -l /proc/$RBP/fd 2>/dev/null | grep -q udev_usb1; then
    echo "RBP=$RBP ready (udev_usb1 fd opened)"
    break
  fi
  sleep 0.5
done

# 9. Start USB watcher once rbp is ready
sh /data/usb-watch.sh start 2>/dev/null

# 10. Wait while rbp is running (so launcher does not redraw on top of rbp)
if [ -n "$RBP" ]; then
  while kill -0 $RBP 2>/dev/null; do
    sleep 2
  done
fi

# Cleanup on exit
sh /data/usb-watch.sh stop 2>/dev/null
for p in $(ps w | awk '$0 ~ /[r]oot\/pdj\/[r]bp|[e]db_streamd/ {print $1}'); do
    kill -9 $p 2>/dev/null
done
