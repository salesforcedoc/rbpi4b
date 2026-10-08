# scripts/

Host-side build tooling and the on-device scripts for the Raspberry Pi 4 port.

```
scripts/
├── build-chroot.sh        assemble the soft-float deploy root -> work/rbpi4b-pi4.tgz
├── patch-rbp-nopc.py      second-stage rbp patch (getPcController NULL deref)
├── patch-rbp-depth.py     third-stage rbp patch (layer pixel format / depth pair)
├── device/                scripts that run on the Pi
└── shims/                 LD_PRELOAD shim sources (soft-float ARM32) + Makefile
```

## `build-chroot.sh`

Runs on a Linux/WSL host. Stages the RX3 rootfs + gui + patched rbp + the shims
+ the DirectFB 1.4.16 stack + `directfbrc` + touch calibration + `rb.conf`, fixes
exec bits, and tars the result for the Pi's `install.sh` to untar into
`/opt/rblive4`. Override asset paths with `RX3=` / `DFB=` / `OUT=`.

See [docs/05](../docs/05-chroot.md).

## `patch-rbp-nopc.py`

Applies the `getPcController()` NULL-deref fix on top of the shared `rbp-audio`:

```sh
python3 patch-rbp-nopc.py rbp-audio -o rbp-nopc
```

The name says what it does, not which device it is for: the RX3 build dereferences
a controller object that does not exist on any non-Pioneer target, and the Pi
fails the same board check the previous target did.

See [docs/03](../docs/03-port-plan.md).

## `patch-rbp-depth.py`

Sets rbp's DirectFB layer pixel format — `DSPF_RGB16` (the RX3's own depth) or
`DSPF_RGB32` — which is two instructions in `DS_HW_Core_Layer_Create` and the
whole of the difference between the deck-1 strip frame's right vertical edge
being drawn whole and being drawn at 1/4 and 1/8 coverage:

```sh
python3 patch-rbp-depth.py rbp-nopc                  # -> 32 bpp, in place
python3 patch-rbp-depth.py rbp-nopc --bpp 16         # -> back to 16 bpp
```

**This is half of a pair and cannot choose for itself.** The other half is
`RB_FB_LIE_BPP` in `rb.conf`, the depth the framebuffer shim reports in
`FBIOGET_VSCREENINFO`; a mismatch makes `DS_HW` refuse the layer and kills rbp
before it paints, so `build-chroot.sh` reads that variable out of the `rb.conf` it
embeds and `start-rb.sh` re-checks the pair before every launch. Idempotent in
both directions, and it validates the word at every address, so it aborts on a
foreign binary instead of corrupting it.

See [PATCHES.md § 12](../tools/patch-rbp/PATCHES.md).

## `device/`

On-device scripts — see [`device/README.md`](device/README.md). The entry point
is `install.sh`; the launcher is `start-rb.sh`.

## `shims/`

The LD_PRELOAD shims — see [`shims/README.md`](shims/README.md). Four are built:
`fbshim.so` (fb ioctls + the fake tsc2007 + pointing), `knobshim.so` (controls:
the rbp bridge, the LED/meter paths and the MIDI maps), `audioshim.so` (rbp's
ALSA contract onto the configured card) and `crashcatch.so` (SIGSEGV reporting
for bring-up).
