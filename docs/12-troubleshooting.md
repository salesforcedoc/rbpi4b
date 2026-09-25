# 12 — Troubleshooting

Symptom → cause → fix.

## Display / boot

| Symptom | Cause | Fix |
|---|---|---|
| Kernel **oops/reboot** as soon as rbp starts | `directfbrc` missing → DirectFB takes the GPU/dri path (`panic_on_oops=1`) | install `usr/etc/directfbrc` with `no-hardware` ([06](06-display.md)) |
| UI sideways | wrong `DFB_ROTATE` | use `DFB_ROTATE=left` |
| `EINVAL` on `FBIOPUT_VSCREENINFO` | 16 bpp modeset on the fixed 32 bpp DRM fb | use the patched fbdev module |

## Startup / controls

| Symptom | Cause | Fix |
|---|---|---|
| rbp exits ~1 s after start, `crash.log` shows `[NULL+0x9c]` | `getPcController()` NULL deref | apply `scripts/patch-rbp-sclive4.py` ([03](03-port-plan.md)) |
| `sh: ls: not found` in `rbp-p.log` | chroot launched without `/bin` in `PATH` | set `PATH=/bin:/sbin:/usr/bin:/usr/sbin` |
| PLAY does nothing, CUE fires an effect | wrong MIDI channel map | decks are on MIDI ch 4/5 ([08](08-controls.md)) |
| Faders/EQs dead, PLAY works | absolute controls sent as `OP_ROTATE` | send `OP_VALUE` for fader/trim/EQ/xfader ([08](08-controls.md)) |
| Taps do nothing | `TouchCalib_User.dat` missing/wrong | install identity calib ([07](07-touch.md)) |

## USB

| Symptom | Cause | Fix |
|---|---|---|
| Stick never detected | `usb-watch.sh` not running, or wrong bus | SC Live 4 media port is **usb1** (`USBWATCH_BUSES="1 2 3"`) ([10](10-usb.md)) |
| Generic "USB1", 0 tracks after a restart | stale DeviceSQL guard/req locks | `rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer`, then re-notify ([10](10-usb.md)) |
| Stick ejects after ~30 s | `edisksd.service` running | stop it in the launcher |

## Audio

| Symptom | Cause | Fix |
|---|---|---|
| No sound on the built-in speakers | audioshim opened 4 channels; speakers are ch 6/7 | 8-channel audioshim mirroring master→6/7 ([09](09-audio.md)) |
| Main/headphone distortion | S24 samples not sign-extended before gain scaling | sign-extend `(l<<8)>>8` before scaling ([09](09-audio.md)) |
| No headphone cue | cue bus (rbp device 1) not routed | route to ch 4/5 + drive `MixerEngine::setMixerChHeadphoneCue` ([09](09-audio.md)) |
| Main VU pinned at max / lags channels | wrong S24 full-scale; separate meter source | true S24 peak + rbp's own master meter ([09](09-audio.md)) |
| Phasing when cue mix is centred on the cued track | summing rbp's master + cue streams (separate ALSA devices, not sample-aligned) | route rbp's phone stream with rbp's master cue on ([09](09-audio.md)) |
| No MASTER CUE button on the SC Live 4 | XDJ-RX3 has one; SC Live 4 doesn't | strips 3/4 PFL toggle `setMasterOutHeadphoneCue` ([09](09-audio.md)) |
| Loud ~100 ms noise on the speakers at startup | codec/DSP start transient | all-channel `STARTUP_MUTE_MS` mute + fade ([09](09-audio.md)) |
| `-EBUSY` opening `hw:1,0` | `engine.service` holds the codec | stop it first |
| Knob volume → loud distortion | software gain on the ch 6/7 stream | run fixed `SPEAKER_GAIN=1.0` ([09](09-audio.md)) |

## Tooling / build

| Symptom | Cause | Fix |
|---|---|---|
| `cannot find libpthread_nonshared.a` | RX3 rootfs lacks the dev archive | `scripts/shims/Makefile` creates empty stubs in `compat/` |
| `GLIBC_2.17`/`GLIBC_2.34` in a shim | linked against host glibc | link the RX3 libs; verify with `objdump -T … | grep GLIBC` |
| `tar: invalid option -- 'z'` on device | busybox tar | `zcat file.tgz \| tar x -C dir` |
| `amidi: Device or resource busy` | the shim's sequencer subscription holds the raw MIDI | capture via the shim's `KNOB_VERBOSE` log instead |
| Exec bits lost on `/bin/*` | Windows/WSL extraction | `scripts/build-chroot.sh` restores them (or `chmod -R 755`) |

## Diagnostic tools

* `tools/touchdump` (static ARM) — ILI2117 ranges + live events.
* `crashcatch.so` — SIGSEGV `pc`/`lr` → `/tmp/crash.log`.
* `KNOB_VERBOSE=1` → every MIDI event + keycode in `/tmp/knobshim.log`.
* `cat /tmp/audioshim.log` — negotiated params + `sg`/peaks.
* `cat /tmp/dfbdig*.log` — DirectFB bring-up (`ROTINIT` line).
* `aplay`/`amixer`/`alsactl` for audio probing. No `strace`/`gdb` on device —
  cross-build and `scp` if needed.
