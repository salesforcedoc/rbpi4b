# 04 — Firmware assets (external)

rblive4 does **not** cover firmware acquisition, decryption or key handling —
those are handled by the related projects (see [NOTICE.md](../NOTICE.md)).
rblive4 starts from an already-extracted XDJ-RX3 v1.20 tree.

## What you need

| Asset | What it is |
|---|---|
| `XDJRX3-rootfs/` | soft-float glibc-2.13 userland extracted from `rootfs.cramfs` |
| `XDJRX3/gui/` | `fontdata`, `imagedata`, `pset`, `system` from the GUI partition |
| `rbp-audio` | the patched player: stock `pdj/rbp` + the shared interoperability patches |

A convenient layout, matching the defaults in
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh):

```
extracted/
├── XDJRX3-rootfs/
├── XDJRX3/gui/
└── rbp-audio
```

`build-chroot.sh` reads that tree through `RX3=` (default `extracted/`) and then
applies the SC Live 4-specific `getPcController()` patch
([`scripts/patch-rbp-sclive4.py`](../scripts/patch-rbp-sclive4.py)) while
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
