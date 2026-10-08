# 04 — Firmware assets (external)

rbpi4b does **not** cover firmware acquisition, decryption or key handling —
those are handled by the related projects (see [NOTICE.md](../NOTICE.md)).
rbpi4b starts from an already-extracted XDJ-RX3 v1.20 tree.

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
python3 scripts/patch-rbp-nopc.py extracted/rbp-audio -o extracted/rbp-nopc
```

**Four binaries, and only the last one runs.** Each is a distinct md5 worth
knowing, because a unit that misbehaves can be holding any of them and they fail
differently:

| build | md5 | what it is |
|---|---|---|
| stock `rbp` | `4f2efcfc0c9e3f539289f863acfddcc6` | Pioneer's XDJ-RX3 v1.20, untouched — deadlocks or aborts on this hardware |
| `rbp-audio` | `3dda2d4e10187a75bfc16a7b4f16f192` | + 66 words of interoperability patches; still has the `getPcController()` NULL-deref |
| `rbp-nopc` (superseded) | `18a64bc4d0ffd1cbd35f3a6ea447fca8` | + 2 words; **runs and plays, but Pro DJ Link is silently dead** — see [PATCHES.md § 11](../tools/patch-rbp/PATCHES.md) |
| `rbp-nopc` | `97aa2223c4ca5906b66389420f29d03f` | + 12 words; **this is what the chroot runs** |

Both stage-1 and final hashes moved on **2026-10-08**, when the § 6 waveform gate
was dropped (it cost the boot logo); the pre-revert pair, `3706c68f` / `3cecd92a`,
is no longer produced by anything in this tree.

`tools/patch-rbp/PATCHES.md` pins all four and lists every word, and
`doctor.sh` compares the installed player against them by name rather than only
reporting that it differs. The names matter past bookkeeping: the deploy-root
override that `start-rb.sh` copies into the chroot at every launch is called
**`rbp-audio`** — stage 1 — so a stale file of that name at the deploy root
silently reinstates the crash on the next restart.

## What is *not* here

No `.UPD`, no decrypted ISO, no `rootfs`, no firmware decryption key and no
`rbp`/`rb` executable is committed to this repository. See
[NOTICE.md](../NOTICE.md).
