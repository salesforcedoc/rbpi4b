# Notice, copyright and legal

**rbpi4b** is an independent interoperability/preservation project. It is
**not affiliated with, endorsed by, or sponsored by** Pioneer DJ, AlphaTheta
Corporation, Denon DJ, inMusic, Raspberry Pi Ltd, or any of their subsidiaries.

The goal is to run the Pioneer DJ XDJ-RX3 standalone rekordbox player (`rbp`,
called `rb` internally) on a **Raspberry Pi 4B**, using the same
soft-float-chroot + shim approach that the related Denon DJ **Prime GO** port
established. The project began as a port to a Denon DJ **SC Live 4**
(`JP21` / `SCX-4`, Rockchip RK3288, Engine OS); that target has been dropped and
the Pi is now the only one, but its survey and findings are retained here as the
reference the port was built from.

## What this repository contains

* Original shell scripts, C sources and documentation written for this project.
  These are licensed MIT (see `LICENSE`).
* Device-survey and port-plan documentation describing the SC Live 4 and
  Raspberry Pi 4B hardware, and how the XDJ-RX3 player is made to run on it.
* Interoperability **patch instructions** for the `rbp` binary (addresses +
  replacement instructions), and a patch against upstream DirectFB.

## What this repository does **not** contain

* No XDJ-RX3/CDJ firmware (`.UPD`), no decrypted firmware ISO, no `rootfs`, no
  `rbp`/`rb` executable, and no other Pioneer/AlphaTheta binaries.
* No Denon DJ / Engine OS files or binaries.
* No Raspberry Pi OS image or Raspberry Pi bootloader files.
* No rekordbox music, playlists, analysis files or databases.
* No firmware decryption key of any kind, and no firmware acquisition or
  decryption tooling — those are out of scope here and handled by the related
  projects (see below).

You must own/obtain the hardware and the extracted firmware assets yourself.
The scripts here operate on files **you** supply.

## Related work this project builds on

* The **PrimeBox** XDJ-RX3 → Prime GO (RK3288) port — the source of the `rbp`
  patcher (`patch-rbp`), the patched DirectFB fbdev driver and the shared shim
  sources. The SC Live 4 is the same SoC family as that target, and the Pi 4 is
  a different SoC entirely, but the parts that carry over are the ones that
  depend on the *player* rather than the hardware: the chroot recipe, the patch
  table and the shims' ABI addresses.
* The **rb2go** XDJ-RX3 → postmarketOS phone (aarch64) port — window-mode,
  input and USB-emulation notes.
* The **chromebit** XDJ-RX3 → ASUS Chromebit (RK3288, postmarketOS) port — a
  clean Linux RK3288 reference.
* **freelive4** — how root SSH on the SC Live 4 was obtained (the `/data`
  overlay + `/etc/ld.so.preload` method). That was necessary on the previous
  target; a Pi running Pi OS needs none of it, and this repo does not depend on
  it.

## Legal caveats

* Firmware acquisition and decryption are **not** part of this project; they
  may be restricted in your jurisdiction. Check your local law before using any
  external extraction tools.
* Patching and running a vendor application on third-party hardware may violate
  the vendor's EULA. This project is offered for research, repair,
  preservation and personal interoperability only.
* Installing this on your device can brick it or void its warranty. **You do
  everything at your own risk.**

## Trademarks

*Pioneer DJ*, *AlphaTheta*, *rekordbox*, *XDJ-RX3*, *CDJ* and related marks are
trademarks of their respective owners. *Denon DJ*, *SC Live 4*, *Engine OS* and
*Prime GO* are trademarks of inMusic Brands, Inc. *Raspberry Pi* and *Raspberry
Pi OS* are trademarks of Raspberry Pi Ltd. *DDJ-FLX4* is a Pioneer DJ product
name, referenced here only to say which controller the port targets. All
trademarks are used here in a descriptive, nominative sense only.

## Third-party components

| Component | License | Used for |
|---|---|---|
| DirectFB 1.4 | LGPL-2.1 | display stack (patched fbdev driver) |
| JUCE | GPL / commercial | audio + UI framework inside `rbp` |
| ALSA / alsa-lib | LGPL | audio |
| glibc 2.13 (RX3 rootfs) | LGPL | soft-float runtime |
| BusyBox | GPL-2.0 | runtime shell |

Raspberry Pi OS is the host system this project targets, not a component it
ships or links against, and no part of it is redistributed here. Staying on the
fbdev path — rather than moving to DirectFB's DRM/KMS system — is deliberate
partly because it introduces **no new third-party code**: the display path is
the same patched fbdev driver, presenting to a different framebuffer.

See each project for the full license text.
