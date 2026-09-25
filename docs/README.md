# docs/

Documentation for the XDJ-RX3 `rb` → **Raspberry Pi 4B** port, with the previous
Denon SC Live 4 target retained as the reference the port started from.

**Start with [13 — Raspberry Pi 4](13-raspberrypi4.md)** if you are bringing a
unit up; it is the target document and links out to the rest.

| # | Document | What it covers |
|---|---|---|
| 00 | [overview](00-overview.md) | goal, architecture, why it works |
| 01 | [device-survey](01-device-survey.md) | live SC Live 4 survey (the previous target — reference) |
| 02 | [hardware](02-hardware.md) | XDJ-RX3 vs Prime GO vs SC Live 4 vs Pi 4 |
| 03 | [port-plan](03-port-plan.md) | reuse map + what changes + build order |
| 04 | [firmware-assets](04-firmware-assets.md) | the extracted assets you need (external) |
| 05 | [chroot](05-chroot.md) | the soft-float glibc-2.13 chroot, on the Pi |
| 06 | [display](06-display.md) | DirectFB fbdev: the present modes + `directfbrc` |
| 07 | [touch](07-touch.md) | pointing: the fake tsc2007, evdev discovery, the axis transform |
| 08 | [controls](08-controls.md) | control surface → rbp keycodes, LEDs, VU; the keyboard fallback (`MIDI_MAP=kbd`) |
| 09 | [audio](09-audio.md) | the stream model, `plughw`, the channel map |
| 10 | [usb](10-usb.md) | USB stick + rekordbox database |
| 11 | [runtime-launcher](11-runtime-launcher.md) | launch sequence / systemd integration |
| 12 | [troubleshooting](12-troubleshooting.md) | symptom → cause → fix |
| 13 | [raspberrypi4](13-raspberrypi4.md) | **the target**: image, `cmdline.txt`, present modes, pointing, audio, controls, USB, launcher, bring-up order |
| 15 | [flx4-midi](15-flx4-midi.md) | the DDJ-FLX4 map: the tables, their source, the dump procedure, and what is still unverified |

There is no document 14: the port plan numbers the FLX4 runbook 15 and defines
nothing at 14, so the gap is left rather than renumbered.

Documents 01 and 04 are the raw survey of the previous target and the shared
extracted assets; they are correct as written and deliberately unchanged.
The matching PrimeBox doc is the upstream reference for anything shared; links
are in [03-port-plan.md](03-port-plan.md).

## How to read a claim in these documents

The tree is mid-port. Where something has been run on hardware, the document
says so ("Facts (live)", a recorded `fbdump` block, a log line). Where it has
not, it says that too, and names what would settle it. A number in a table with
no measurement behind it is marked as expected, not recorded — treat the two
differently, because only one of them has been tested.
