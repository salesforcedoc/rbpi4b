# extracted/ (not tracked)

This directory is where **you** put the firmware artefacts you extract. It is
listed in `.gitignore`; nothing here is committed, because these are
copyrighted Pioneer/AlphaTheta files.

After following [docs/01-firmware-extraction.md](../docs/01-firmware-extraction.md)
it should look like:

```
extracted/
├── XDJRX3.iso              decrypted firmware ISO
├── XDJRX3/                 ISO contents (images/, pdj/, gui/, lib/, usr/, …)
├── XDJRX3-gui/             gui.tar.gz contents (fonts! required by rbp)
├── XDJRX3-rootfs/          rootfs.cramfs contents (glibc 2.13, edb_streamd, …)
└── stock-rbp               pdj/rbp, md5 4f2efcfc0c9e3f539289f863acfddcc6
```

The patched player and the built shims are produced from these files:

```
rbp-audio                   python3 tools/patch-rbp/rbp_patch.py stock-rbp -o rbp-audio
scripts/shims/*.so          make -C scripts/shims RX3=$PWD/extracted/XDJRX3-rootfs
```
