# 11 — Runtime launcher

[`scripts/device/start-rb.sh`](../scripts/device/start-rb.sh) runs on the device
and is the normal way to bring up the player.

## What it does

1. `systemctl stop engine.service edisksd.service` — `engine` owns the display/
   audio/MIDI; `edisksd` will bus-reset a USB drive it doesn't manage.
2. Kill stale `rbp`/`edb_streamd`.
3. `sh /data/fix-dev.sh` — bind mounts + device stubs.
4. Copy `rbp-audio` + the shims into the chroot.
5. `rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer` — clear stale
   DeviceSQL locks (see [10](10-usb.md)).
6. Start `edb_streamd` (DeviceSQL) **before** rbp.
7. Start rbp, then `usb-watch.sh start`.

The rbp launch line (order matters):

```sh
chroot /data/rbx3-run env \
  PATH=/bin:/sbin:/usr/bin:/usr/sbin \
  DFB_ROTATE=left STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 \
  LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so \
  /lib/ld-linux.so.3 /root/pdj/rbp -a
```

* `PATH` must include `/bin` (the device default `/usr/bin:/usr/sbin` misses the
  chroot's busybox tools → rbp logs `sh: ls: not found`).
* `knobshim.so` **before** `audioshim.so` so the shared `g_speaker_gain` data
  symbol resolves.
* the speaker/booth level is fixed via `SPEAKER_GAIN` (see [09](09-audio.md)).

## Restarting

Always clear the DeviceSQL locks when restarting rbp, else the USB library
shows the generic "USB1" label instead of the volume name/track count:

```sh
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
```

> The kill pattern matches the loader comm (`ld-linux.so.3`), which both rbp and
> `edb_streamd` use. Do **not** use an inline `ps | grep '/root/pdj/rbp'` kill
> loop over SSH — it matches the SSH shell's own command line and kills the
> session.

## Restore Engine OS

```sh
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
systemctl start engine.service
```

## Autostart

There is no shipped autostart unit. A systemd unit under the `/etc` overlay
(`/data/system/etc/overlay/systemd/system/rb.service` + a
`multi-user.target.wants` symlink) can stop `engine` and run `start-rb.sh` at
boot; make it reversible (`systemctl stop rb; systemctl start engine`).
