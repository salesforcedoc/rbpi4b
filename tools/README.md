# tools/

Workstation-side tooling. Nothing here runs on the SC Live 4.

| Tool | Language | Purpose |
|---|---|---|
| [`patch-rbp/`](patch-rbp/) | Python | apply the interoperability patches to a stock `rbp` |
| [`build-directfb/`](build-directfb/) | C / patch | patched DirectFB 1.4.16 (core + fbdev + modules) for the Rockchip fb |
| [`touchdump.c`](touchdump.c) | C | static ARM tool: dump an evdev touchscreen's ABS ranges + live events |

Firmware acquisition, decryption and key handling are out of scope for rblive4;
start from the extracted assets described in
[`docs/04-firmware-assets.md`](../docs/04-firmware-assets.md).

## `patch-rbp`

`rbp_patch.py` contains the complete, verified instruction table that turns the
stock v1.20 `rbp` (md5 `4f2efcfc0c9e3f539289f863acfddcc6`) into `rbp-audio`
(md5 `3706c68f7242779d46afa09f35a39acf`). It is idempotent and validates the
stock words before writing. [`PATCHES.md`](patch-rbp/PATCHES.md) explains what
each patch does.

```bash
python3 tools/patch-rbp/rbp_patch.py /path/to/stock/rbp -o extracted/rbp-audio
```

The SC Live 4-specific `getPcController()` patch is applied later by
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh) via
[`scripts/patch-rbp-sclive4.py`](../scripts/patch-rbp-sclive4.py).

## `build-directfb`

Contains `directfb-full.diff`, the complete patch against DirectFB 1.4.16. No
upstream DirectFB sources are shipped; fetch them and apply the diff, then
install to `work/dfb` (see the [README](build-directfb/README.md)).

## `touchdump`

Static ARM tool used to verify the ILI2117 touchscreen:

```bash
arm-linux-gnueabihf-gcc -O2 -static -o touchdump touchdump.c
```
