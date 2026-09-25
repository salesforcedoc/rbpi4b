# 06 — Display

The rekordbox UI renders full-screen on the 800×1280 panel, correctly rotated.

## Facts (live)

* `/dev/fb0` = `rockchipdrmfb`, **800×1280 portrait**, 32 bpp, stride 3200,
  triple-buffered (`virtual_size = 800,3840`).
* DSI-1 connector, `modes: 800x1280`. Mali lib bind-mounted at
  `/usr/lib/libmali.so.14.0`.

## The stack

```
 rbp  ──renders RGB565 1280×800──►  DirectFB 1.4.16 (RX3 libs)
                                        │
                          patched libdirectfb_fbdev.so (rot16)
                          • forces the real fb format, serialises ioctls
                          • rotates 1280×800 RGB565 → 800×1280 RGB32
                                        ▼
                              /dev/fb0 (rockchipdrmfb)
 LD_PRELOAD fbshim.so: reports a 1280×800 RGB565 logical fb, 60 fps pacing
```

On device, `/tmp/dfbdig9.log` contains
`ROTINIT: real_fb=800x1280 pitch=3200 (orig_var=1280x800)`.

## Required: `directfbrc`

`usr/etc/directfbrc` (and `/etc/directfbrc`) **must** contain:

```
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
```

Without `no-hardware`, DirectFB takes the GPU/dri path and **oopses the
kernel**; the SC Live 4 boots with `panic_on_oops=1`, so the unit reboots.
`scripts/build-chroot.sh` writes this file.

## Artifacts

All supplied by the shared RK3288 display build (see
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh)):

| Piece | Source |
|---|---|
| DirectFB 1.4.16 core libs | DirectFB build tree |
| patched fbdev module | `libdirectfb_fbdev-rot16.so` |
| `libdirectfb_linux_input.so`, `libdirectfbwm_default.so` | display build modules |

Rotation is selected at runtime with `DFB_ROTATE=left`.

## Notes

* Do **not** set `layer-size` or `layer-rotate` in `directfbrc`.
* The DRM fb cannot pan; the driver falls back to `FRONTONLY` and keeps the
  real `yres_virtual`.
* Rotation direction: `DFB_ROTATE=left` (90° CCW). Wrong value = UI sideways.
