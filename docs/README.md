# docs/

Documentation for the XDJ-RX3 `rb` → Denon SC Live 4 port.

| # | Document | What it covers |
|---|---|---|
| 00 | [overview](00-overview.md) | goal, architecture, why it works |
| 01 | [device-survey](01-device-survey.md) | live SC Live 4 survey (kernel, display, touch, audio, controls, USB) |
| 02 | [hardware](02-hardware.md) | XDJ-RX3 vs Prime GO vs SC Live 4 |
| 03 | [port-plan](03-port-plan.md) | reuse map + what changes + build order |
| 04 | [firmware-assets](04-firmware-assets.md) | the extracted assets you need (external) |
| 05 | [chroot](05-chroot.md) | soft-float glibc-2.13 chroot on the SC Live 4 |
| 06 | [display](06-display.md) | DirectFB fbdev on the 800×1280 portrait panel + `directfbrc` |
| 07 | [touch](07-touch.md) | ILI2117 → tsc2007 shim + TouchCalib |
| 08 | [controls](08-controls.md) | SC Live 4 MIDI → rbp keycodes, LEDs, VU |
| 09 | [audio](09-audio.md) | JP21 8-channel codec + built-in speakers |
| 10 | [usb](10-usb.md) | USB stick + rekordbox database |
| 11 | [runtime-launcher](11-runtime-launcher.md) | launch sequence / systemd integration |
| 12 | [troubleshooting](12-troubleshooting.md) | symptom → cause → fix |

The matching PrimeBox doc is the upstream reference for anything shared; links
are in [03-port-plan.md](03-port-plan.md).
