# Notice, copyright and legal

**rblive4** is an independent interoperability/preservation project. It is
**not affiliated with, endorsed by, or sponsored by** Pioneer DJ, AlphaTheta
Corporation, Denon DJ, inMusic, or any of their subsidiaries.

The goal is to run the Pioneer DJ XDJ-RX3 standalone rekordbox player (`rbp`,
called `rb` internally) on a Denon DJ **SC Live 4** (`JP21` / `SCX-4`,
Rockchip RK3288, Engine OS), using the same soft-float-chroot + shim approach
as the related Denon DJ **Prime GO** port.

## What this repository contains

* Original shell scripts, C sources and documentation written for this project.
  These are licensed MIT (see `LICENSE`).
* Device-survey and port-plan documentation describing the SC Live 4 hardware
  and how the XDJ-RX3 player is made to run on it.
* Interoperability **patch instructions** for the `rbp` binary (addresses +
  replacement instructions), where the SC Live 4 differs from the Prime GO.

## What this repository does **not** contain

* No XDJ-RX3/CDJ firmware (`.UPD`), no decrypted firmware ISO, no `rootfs`, no
  `rbp`/`rb` executable, and no other Pioneer/AlphaTheta binaries.
* No Denon DJ / Engine OS files or binaries.
* No rekordbox music, playlists, analysis files or databases.
* No firmware decryption key of any kind, and no firmware acquisition or
  decryption tooling — those are out of scope here and handled by the related
  projects (see below).

You must own/obtain the hardware and the extracted firmware assets yourself.
The scripts here operate on files **you** supply.

## Related work this project builds on

* The **PrimeBox** XDJ-RX3 → Prime GO (RK3288) port — the source of the `rbp`
  patcher (`patch-rbp`), the patched DirectFB fbdev driver and the shared shim
  sources. The SC Live 4 is the same SoC family, so most of it is reused
  verbatim.
* The **rb2go** XDJ-RX3 → postmarketOS phone (aarch64) port — window-mode,
  input and USB-emulation notes.
* The **chromebit** XDJ-RX3 → ASUS Chromebit (RK3288, postmarketOS) port — a
  clean Linux RK3288 reference.
* **freelive4** — how root SSH on the SC Live 4 was obtained (the `/data`
  overlay + `/etc/ld.so.preload` method); this repo assumes that access is
  already in place.

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
*Prime GO* are trademarks of inMusic Brands, Inc. All trademarks are used here
in a descriptive, nominative sense only.

## Third-party components

| Component | License | Used for |
|---|---|---|
| DirectFB 1.4 | LGPL-2.1 | display stack (patched fbdev driver) |
| JUCE | GPL / commercial | audio + UI framework inside `rbp` |
| ALSA / alsa-lib | LGPL | audio |
| glibc 2.13 (RX3 rootfs) | LGPL | soft-float runtime |
| BusyBox | GPL-2.0 | runtime shell |

See each project for the full license text.
