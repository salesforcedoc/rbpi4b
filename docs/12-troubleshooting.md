# 12 — Troubleshooting

Symptom → cause → fix. Rows are grouped by subsystem; the ones marked
**(previous target)** are kept because they explain a design decision in this
port, even though the hardware that produced them is gone.

## Nothing works at all

| Symptom | Cause | Fix |
|---|---|---|
| `fix-dev.sh` reports it cannot bind /device nodes are missing | not run as root, or `/dev` was rebuilt by a reboot | run `install.sh`/`start-rb.sh` as root; `fix-dev.sh` must run **after every reboot** ([05](05-chroot.md)) |
| rbp exits ~1 s after start, `crash.log` shows `[NULL+0x9c]` | `getPcController()` NULL deref | apply `scripts/patch-rbp-nopc.py` ([03](03-port-plan.md)) |
| `chroot … /bin/sh -c 'echo ok'` fails | the kernel cannot execute 32-bit ARM ELF (no `32-bit EL0`/`CONFIG_COMPAT`), or the chroot's exec bits are missing | `dmesg \| grep '32-bit EL0'` — `uname -m` is **not** the test, a 64-bit kernel is fine; reinstall Pi OS Lite 32-bit and rebuild the tarball ([05](05-chroot.md)) |
| `sh: ls: not found` in `rbp.log` | chroot launched without `/bin` in `PATH` | `PATH=/bin:/sbin:/usr/bin:/usr/sbin` — the launcher sets it ([11](11-runtime-launcher.md)) |

## Display

| Symptom | Cause | Fix |
|---|---|---|
| No UI at all, but rbp is running | a framebuffer-memory shortfall, not a mode-set refusal — and on the measured Pi it is **fixed in the patch**, so a recurrence means the fix is not in the running tree (a stale `libdirectfb_fbdev.so`, or a chroot rebuilt before it). The fb has **one page** (`yres_virtual == yres`, `smem_len == 2560 × 800`); the stock driver forced `DLBM_TRIPLE` and its guard against a non-pannable fb tests `ypanstep == 0`, which this fb does not report, so `yres_virtual` tripled and the primary region test failed on `need_mem` 6,144,000 > `smem_len` 2,048,000. The patched driver decides the buffer mode from the **page count** instead, which is a `FRONTONLY` layer here | read the `PRESENT:` line in `/tmp/dfbdig9.log`: `pages=1 pan=0` says the page decision was taken from the real geometry. `pages=3 pan=1` says the module is the old one — verify the sha256 in the chroot against `work/dfb/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` and re-deploy. For the arithmetic itself, add `debug=FBDev/Mode` to `usr/etc/directfbrc` and read `log/rbp.log` (the `=` and the slash are load-bearing: the parser splits options only at `=`, and the domain's declared name is `FBDev/Mode`, so `debug FBDev_Mode` is rejected as invalid and then ignored); `fbdump` says what the fb really is ([06](06-display.md), [13](13-raspberrypi4.md)) |
| rbp exits ~1 s after start, **`crash.log` shows `addr=0x30 r00=0x00000000`** (not the `[NULL+0x9c]` row above) | `DirectFBCreate()` failed, so it returned a NULL `IDirectFB*` — and rbp then dereferences it instead of stopping. `init_Resource()`'s error path prints a bare `DS_HW_Glib3_DFB.c <293>:` (the format string is literally `'%s <%d>:\n\t'`, with no body to lose) and then falls through into the success continuation, loading the NULL pointer and taking `vtable+0x30`. The **reason** `DirectFBCreate` failed is almost always that no module loaded at all | check the modules first, not the crash: `chroot /opt/rblive4/rbx3-run /usr/bin/arm-none-linux-gnueabi-dfbinfo` — `No system found!` means it. Then `cat /tmp/dfbdig4.log`: it names each directory searched and `opendir failed` on a miss. If the directory is `/lib/directfb-1.4-6/...` and does not exist, `module-dir` is missing from `usr/etc/directfbrc` — the build's `MODULEDIR` is `/lib/...` while the modules are staged at `/usr/lib/...` ([06](06-display.md#required-directfbrc)). Do **not** chase the NULL deref: it is a second, latent bug in rbp that a working DirectFB never reaches |
| `Unable to dlopen '…/._libdirectfb_fbdev.so'` and the same for the other modules | AppleDouble files from a macOS build host, left in the module directories. They are not inert: DirectFB walks each module dir and tries to dlopen every entry, so each one logs an error and the directory looks broken | `find /opt/rblive4 -xdev -name '._*' -type f -delete`. A re-deploy alone will **not** clear them — untar upgrades in place and never deletes — which is why `install.sh` and `build-chroot.sh` now both sweep them. If they are back after a fresh install, the tarball itself is carrying them: `tar -tzf work/rblive4-pi4.tgz \| grep '\._'` |
| `dmesg` shows `No compatible format found` | the mode was forced with a depth suffix | drop the `-16`/`-32` from the `video=` token; the 16 bpp default is the one we want ([13](13-raspberrypi4.md#the-hdmi-mode)) |
| Kernel text, a cursor, or boot messages over the UI | `fbcon=map:1` / `vt.global_cursor_default=0` not applied | `cat /proc/cmdline` — the tokens must be on the single existing line in `/boot/firmware/cmdline.txt` |
| Screen blanked mid-session | `consoleblank` unset | add `consoleblank=0`; a blank panel is a diagnostic red herring, not a crash |
| UI torn, or the render thread at 100% CPU | the `FBIOPAN_DISPLAY` pacer disabled or too small | `RB_PAN_PACER_MS` (default `16.666666`); `vc4` returns from a pan without waiting for vblank ([06](06-display.md)) |
| `EINVAL` from `fbset -depth`/`-xres` | **expected on Bookworm** — those are ignored | use `video=` on the kernel command line instead |
| UI sideways **(previous target)** | wrong `DFB_ROTATE` | that was the portrait panel; the Pi has no rotation. `RB_DFB_ROTATE=left` → `PRESENT_ROTATE, 90` still exists for it |
| Kernel oops/reboot as soon as rbp starts **(previous target)** | `directfbrc` missing → DirectFB took the GPU/dri path, and that unit booted `panic_on_oops=1` | keep `no-hardware` in `directfbrc`; the build is `--with-gfxdrivers=none --disable-dev-mem` and this is the verified configuration ([06](06-display.md)) |

## Pointing

| Symptom | Cause | Fix |
|---|---|---|
| The pointer does not move | no pointer device found | `tools/evdevdump --list`; set `POINT_KIND`/`POINT_DEV` from its verdict line ([07](07-touch.md)) |
| Fast clicks do nothing, slow ones work | rbp's UI debounce is longer than the click | raise `POINT_MIN_DWELL_MS` (default 45). This is the most likely report on this target |
| Clicks land mirrored or transposed | the axis algebra is wrong for this device | `POINT_SWAP_XY`/`POINT_INVERT_X`/`POINT_INVERT_Y`. **Do not derive it on paper** — measure with `POINT_DEBUG=1`, click the four corners, read `/tmp/pointsrc.log` |
| Anything affine that the flags cannot express | — | a non-identity `root/settings/TouchCalib_*.dat` ([07](07-touch.md)) |
| The pointer works until it is unplugged, then never returns | — | it should re-scan on `ENODEV`; if it does not, that is a bug in `pointsrc.c`, not config |
| Taps do nothing at all **(previous target)** | `TouchCalib_User.dat` missing/wrong | install the identity calib ([07](07-touch.md)) |

## Controls

| Symptom | Cause | Fix |
|---|---|---|
| **No controls at all, UI fine** | `/dev/snd/seq` missing — the classic silent failure | `fix-dev.sh` modprobes `snd-seq` and fails loudly; check `ls -l /dev/snd/seq` ([08](08-controls.md)) |
| No controls, and a keyboard is all that is attached | `MIDI_MAP` is not `kbd`, so nothing is reading `/dev/input/event*` | set `RB_MIDI_MAP=kbd` in `rb.conf` — it needs no controller, no MIDI and no `/dev/snd/seq` ([08](08-controls.md#the-keyboard-map-rb_midi_mapkbd)) |
| `MIDI_MAP=kbd` and the keyboard works but the mouse's right button and wheel do not | `RB_KBD_DEV` pins the reader to one node, so only that device is read | unset `RB_KBD_DEV` so every node with `EV_KEY` is opened ([08](08-controls.md#the-keyboard-map-rb_midi_mapkbd)) |
| `MIDI_MAP=kbd` and nothing works, but the pointer does | `/dev/input` is empty, or the node has no `EV_KEY` to report | `tools/evdevdump --list`; `/tmp/knobshim.log` names every node the reader opened and says when it found none ([08](08-controls.md#the-keyboard-map-rb_midi_mapkbd)) |
| The controller is plugged in but never matched | the port name does not contain `RB_MIDI_IN_MATCH` | `aseqdump -l` for the real name, then set `RB_MIDI_IN_MATCH` ([15](15-flx4-midi.md)) |
| Controls work, LEDs do not | the output route fell back to rawmidi, or LED output is disabled | check `/tmp/knobshim.log` for which route is in use; `LED_DISABLE` must not be set ([08](08-controls.md)) |
| `amidi: Device or resource busy` | the shim holds the output device exclusively **on the rawmidi fallback path** | capture through the shim's `KNOB_VERBOSE` log instead ([08](08-controls.md)) |
| A control does the wrong thing, or works backwards | wrong note/CC number, or wrong jog/pitch polarity | that is what `RB_MIDI_DUMP` and `TEMPO_VERBOSE`/`JOG_VERBOSE` exist for; the values are **calibrated**, not derived ([15](15-flx4-midi.md)) |
| Faders/EQs dead, PLAY works **(previous target)** | absolute controls sent as `OP_ROTATE` | send `OP_VALUE` for fader/trim/EQ/xfader ([08](08-controls.md)) |

## Audio

| Symptom | Cause | Fix |
|---|---|---|
| No sound at all | no device opened | read `/tmp/audioshim.log`: it logs every candidate tried and each result. Check `aplay -L` and `/proc/asound/cards` for the card id, then fix `RB_AUDIO_DEV` ([09](09-audio.md)) |
| **Silent, with a log that looks healthy** | the format the shim asks for is not one the card accepts. Because a wrong `snd_pcm_format_t` reaches the card as a bare `-EINVAL`, the symptom is silence and nothing more — this was a real defect on this port, and the shim's hand-rolled `SND_PCM_FORMAT_S24_3LE` was **10** (`S32_LE`) instead of **32** for a long time | read the `format constants verified:` line: it must name `S24_3LE=32`. A `FORMAT CONSTANT WRONG:` line says which one and what to fix. `tools/pcmprobe` prints the whole mapping measured from libasound ([09](09-audio.md)) |
| A storm of `written=-22`, megabytes of log, no sound | the shim is answering libasound's own plugin setup calls with fake success, so libasound spins setting up a device it can never finish | the shim forwards those calls now (`is_forwardable()`) — a recurrence means the running `audioshim.so` predates that. Measured after the fix: 7 `prepare` calls per run where there had been 1,060,024 |
| `-EINVAL` at `hw_params`, or the second pair has the master folded into it | **channel count ≠ the card's own.** On a `hw:` device it surfaces as `-EINVAL`; on the previous target, where a `route` plugin sat in between, it instead **downmixed** — logical channels > hw channels folds FL/FR into the surround pair and puts garbage on the second output | `RB_AUDIO_CHANNELS=auto`, or a number ≤ the card's own; ask for exactly as many as the card has. On a `plughw` device the shim negotiates **10000** and gets it wrong — that is why `RB_AUDIO_DEV` is `hw:` ([09](09-audio.md)) |
| Main/headphone distortion | S24 samples not sign-extended before gain scaling | sign-extend `(l<<8)>>8` before scaling ([09](09-audio.md)) |
| Phasing when the cue mix is centred on the cued track | summing rbp's master + cue streams (separate ALSA devices, not sample-aligned) | route rbp's phone stream with rbp's master cue on — never sum ([09](09-audio.md)) |
| No headphone cue | the phone stream is not mapped | `RB_AUDIO_MAP`'s `headphones=` pair, plus a channel cue via `me_set_cue()` ([09](09-audio.md)) |
| `-EBUSY` opening the device | two things can, and both were seen here: (1) something else holds the USB audio — only rbp should, so check for a second player process ([11](11-runtime-launcher.md)); (2) **the shim closed a libasound-internal handle**, which it now forwards instead of swallowing. The log line to look for is `closed the master's handle … res=0` on every open | if a second player is not it, the shim is the cause: the opens must all read `res=0` and each needs its matching close |
| A burst or click when playback starts | the interface's own transient | `RB_STARTUP_MUTE_MS`/`RB_STARTUP_FADE_MS`, both `0` by default on the Pi ([09](09-audio.md)) |
| No VU meters on the controller | **expected** — the FLX4 has none | `RB_LED_VU=0`, and the meter hook is not installed at all ([09](09-audio.md), [15](15-flx4-midi.md)) |
| The FLX4's MASTER CUE button seems to do nothing at first | rbp has no PFL keycode, so the state is asserted by the shim, not by the panel | the button toggles `me_set_master_cue()` from `me_get_cue()`-style engine state, so it cannot disagree with the startup assertion; check `/tmp/knobshim.log` for the `master cue -> ON/OFF` line ([09](09-audio.md), [15](15-flx4-midi.md)) |

## USB

| Symptom | Cause | Fix |
|---|---|---|
| Stick never detected | not on a USB bus, or the watcher is not running | `usb-watch.sh status` prints the candidates it can see ([10](10-usb.md)) |
| The stick mounts but rbp shows nothing, and `usbwatch.log` ends `attach: rbp never opened export.pdb after retries` | first check the **stick**, not the code: that message is the watcher reporting honestly that rbp never imported a database. If the stick has no `PIONEER/rekordbox/export.pdb` — a plain copy of audio files, or one with an empty `PIONEER/USBANLZ` — then it is not a rekordbox-exported device and *nothing* will ever open `export.pdb`. Seen on this port's bring-up | `find <mountpoint> -iname '*.pdb'`; if it is empty, re-export with rekordbox's *Export to USB Device*. Only if a stick that *does* carry `export.pdb` still fails is it the timing cause below |
| The stick mounts but rbp shows nothing, with `export.pdb` present | rbp's DeviceSQL channel was not up when the event was sent | the watcher re-notifies until rbp opens `export.pdb`; check `usbwatch.log` ([10](10-usb.md)) |
| Stick ejects ~30 s after mounting | `udisks2` claimed it | it must be masked — `RB_MASK_SERVICES` ([10](10-usb.md), [11](11-runtime-launcher.md)) |
| `mount` fails with a bare `EINVAL` | the `nls` charset modules are not loaded | `fix-dev.sh` modprobes them; the watcher also retries without `codepage`/`iocharset` ([10](10-usb.md)) |
| Generic "USB1", 0 tracks after a restart | stale DeviceSQL guard/req locks | `rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer`; the launcher does this ([10](10-usb.md)) |
| `USB Error. Remove the device.` | something wrote to `/tmp/udev_usbctn*` | never write "connect" there — the mount event alone is the protocol ([10](10-usb.md)) |

## Tooling / build

| Symptom | Cause | Fix |
|---|---|---|
| `cannot find libpthread_nonshared.a` | the RX3 rootfs lacks the dev archive | `scripts/shims/Makefile` creates empty stubs in `compat/` |
| `GLIBC_2.17`/`GLIBC_2.34` in a shim | linked against host glibc | link the RX3 libs; `make check` fails on `GLIBC > 2.7` and on `Tag_ABI_VFP_args` (a hard-float shim would never load) |
| A shim loads but a global is missing | wrong `LD_PRELOAD` order | `fbshim:knobshim:audioshim` — the audio shim reads globals the controls shim defines, and `SHMSTATE_STRICT=1` makes a mismatch fail loudly ([11](11-runtime-launcher.md)) |
| A shim does nothing, no error | loaded by the host loader instead of the chroot's | `LD_PRELOAD` is set inside the chroot by the launcher, never exported ([11](11-runtime-launcher.md)) |
| A flag in `rb.conf` has no effect | read by presence, and exported empty (see [11](11-runtime-launcher.md)) | the workaround lines in `start-rb.sh` cover the affected names |
| Exec bits lost on `/bin/*` | Windows/WSL extraction | `scripts/build-chroot.sh` restores them (or `chmod -R 755`) |

## Diagnostic tools

| Tool | What it tells you |
|---|---|
| `tools/fbdump` | `/dev/fb0`'s real geometry, format, pan step — the present path's inputs ([06](06-display.md)) |
| `tools/evdevdump --list` | every input device's name, caps and absinfo, plus a ready-to-paste `POINT_KIND`/`POINT_DEV` verdict ([07](07-touch.md)) |
| `crashcatch.so` | SIGSEGV `pc`/`lr` → `/tmp/crash.log`. Load it when bringing a target up |
| `KNOB_VERBOSE=1` | every MIDI event + keycode, and every evdev triple + keycode → `/tmp/knobshim.log` |
| `RB_MIDI_MAP=kbd` | the keyboard fallback: keyboard and mouse, no controller and no MIDI — the surface to use when nothing is plugged in ([08](08-controls.md#the-keyboard-map-rb_midi_mapkbd)) |
| `RB_MIDI_DUMP` / `MIDI_REPLAY` | record the surface, then replay it on a bench ([15](15-flx4-midi.md)) |
| `/tmp/audioshim.log` | device candidates and their results, negotiated params, the pair map, peaks — and the `format constants verified:` line, which is what catches a wrong hand-rolled `snd_pcm_format_t` before it becomes silence ([09](09-audio.md)) |
| `tools/pcmprobe` | every `snd_pcm_format_t` value named by libasound, with its width and physical size, and a card's accepted formats by name — the measurement that fixed the `S24_3LE` constant ([09](09-audio.md)) |
| `/tmp/dfbdig9.log` | the one-shot `PRESENT: mode=… real_fb=…×… pitch=… bpp=… pages=… pan=… (logical …)` line from the fbdev driver — the first thing to read when no UI appears ([06](06-display.md)) |
| `/tmp/pointsrc.log` | `POINT_DEBUG=1`'s per-event `(down, raw_x, raw_y, lx, ly)` |
| `usb-watch.sh status` | media candidates, both mounts, rbp pid, log tail |

Only the **start-up** DirectFB log survives: the per-flip and per-input-event
debug traces were removed from the patch, and keeping them out is mandatory
([06](06-display.md)). There is no `strace`/`gdb` on a Lite image by default —
cross-build and `scp` if you need one.
