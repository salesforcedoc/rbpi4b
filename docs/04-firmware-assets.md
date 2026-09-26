# 04 — Firmware assets (external)

rblive4 does **not** cover firmware acquisition, decryption or key handling —
those are handled by the related projects (see [NOTICE.md](../NOTICE.md)).
rblive4 starts from an already-extracted XDJ-RX3 v1.20 tree.

## What you need

| Asset | What it is |
|---|---|
| `XDJRX3-rootfs/` | soft-float glibc-2.13 userland extracted from `rootfs.cramfs` |
| `XDJRX3/gui/` | the ISO's `gui/` directory: `fontdata`, `imagedata` |
| `XDJRX3-gui/` | `images/gui.tar.gz` unpacked: `pset`, `system` |
| `rbp-audio` | the patched player: stock `pdj/rbp` + the shared interoperability patches |

The four GUI asset directories are **split across those two roots, and neither
root has all four**:

- `images/gui.tar.gz` *is* the GUI partition — the RX3 formats its `gui` UBIFS
  volume (mtd10) from that tarball, which is where `pset/` and `system/` come
  from. `rbp` opens ten paths under `/root/gui` and every one of them is under
  those two: nine under `pset/` (eight `fontdata/*.bin`, `imagedata/imagedata.dat`)
  and `system/fontdata/sazanami-gothic.ttf`.
- the ISO's own `gui/` directory holds the `fontdata`/`imagedata` the firmware's
  `update` binary reads (`gui/fontdata/decker.ttf`, `gui/imagedata/png/`).

`build-chroot.sh` therefore resolves each of the four directories separately,
from whichever root has it, and requires `pset/` and `system/`. A convenient
layout, matching its defaults:

```
extracted/
├── XDJRX3/                 ISO contents (XDJRX3/gui = fontdata, imagedata)
├── XDJRX3-gui/             images/gui.tar.gz unpacked (pset, system)
├── XDJRX3-rootfs/
└── rbp-audio
```

Unpacking the tarball is the one step the ISO does not do for you:

```bash
mkdir -p extracted/XDJRX3-gui
tar -C extracted/XDJRX3-gui -xzf extracted/XDJRX3/images/gui.tar.gz
```

`build-chroot.sh` reads that tree through `RX3=` (default `extracted/`) and then
applies the SC Live 4-specific `getPcController()` patch
([`scripts/patch-rbp-nopc.py`](../scripts/patch-rbp-nopc.py)) while
staging the chroot.

If you already have the **stock** `rbp`, the shared patch set in
[`tools/patch-rbp/`](../tools/patch-rbp/) turns it into `rbp-audio`:

```bash
python3 tools/patch-rbp/rbp_patch.py /path/to/stock/rbp -o extracted/rbp-audio
```

## What is *not* here

No `.UPD`, no decrypted ISO, no `rootfs`, no firmware decryption key and no
`rbp`/`rb` executable is committed to this repository. See
[NOTICE.md](../NOTICE.md).
