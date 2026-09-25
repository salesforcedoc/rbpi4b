# 06 — Display

The rekordbox UI renders full-screen on an HDMI monitor. `rbp` draws a logical
**1280×800 RGB565** surface through DirectFB, and the patched fbdev module
presents it to whatever `/dev/fb0` actually is.

## The stack

```
 rbp  ──renders RGB565 1280×800──►  DirectFB 1.4.16 (RX3 libs)
                                        │
                          patched libdirectfb_fbdev.so
                          • forces the real fb format, serialises ioctls
                          • presents the logical surface to the real fb
                                        ▼
                              /dev/fb0 (vc4drmfb)
 LD_PRELOAD fbshim.so: reports a 1280×800 RGB565 logical fb, 60 fps pacing
```

## What the framebuffer is (expected, not yet measured)

On the Pi 4B, `/dev/fb0` is **`vc4drmfb`** — DRM fbdev emulation, not a
hardware fb like the previous target's `rockchipdrmfb`. The expectation, from
`drm_fb_helper`'s behaviour and reports for `vc4`, is:

| Field | Expected |
|---|---|
| format | 16 bpp RGB565 — `rbp`'s own format |
| geometry | whatever `video=` asked for, i.e. 1280×800 if the sink accepts it |
| `yres_virtual` | `== yres` — no room to double-buffer |
| `ypanstep` | `0` — the fb cannot pan |

> **Not yet recorded.** These are expectations, not measurements: this document
> is written before the first run on the hardware. `tools/fbdump` prints the
> real values in one command, and S1.3 is the step that records them. `bpp` is
> the field that matters most — 16 means the present path is a plain per-row
> copy, 32 means it must convert 565 → 8888.

The previous target, for contrast, was a **DSI 800×1280 portrait panel** at
32 bpp with a triple-buffered 800×3840 virtual fb — the geometry is why its
present path was a rotation and the Pi's may be nothing at all.

## For reference: the previous target's facts

* `/dev/fb0` = `rockchipdrmfb`, 800×1280 portrait, 32 bpp, stride 3200,
  triple-buffered (`virtual_size = 800,3840`).
* DSI-1 connector, Mali lib bind-mounted at `/usr/lib/libmali.so.14.0`.
* `/tmp/dfbdig9.log` held `ROTINIT: real_fb=800x1280 pitch=3200
  (orig_var=1280x800)`.

The `ROTINIT` line is deliberately **kept** in the current patch: it is a
one-shot geometry record logged at start-up, and it is the cheapest way to see
what the driver decided. The Pi will log its own.

## Setting the mode

On Bookworm the `framebuffer_*` options in `config.txt` are **ignored** — the
mode comes from `video=` on the kernel command line, and `fbset -depth`/
`-xres` fails with `EINVAL`. The recipe, and why each token earns its place, is
in [13 — Raspberry Pi 4](13-raspberrypi4.md#the-hdmi-mode):

```
video=HDMI-A-1:1280x800@60 fbcon=map:1 console=tty3 consoleblank=0 vt.global_cursor_default=0
```

`fbcon=map:1` binds the kernel console to a `fb1` that does not exist, so
nothing draws over the UI — without it, kernel messages and a blinking cursor
land on top of rekordbox.

## The present path

`rbp` renders a logical **1280×800 RGB565** surface. The real framebuffer may
be a different size, a different pixel format, or both, and the present path is
what bridges that gap.

> **State of this section.** The four present modes below are the **design**, and
> `RB_DFB_PRESENT` is already the config knob for them — but **the driver does
> not read it yet.** What `directfb-full.diff` installs today is the previous
> target's rotation path (`rot_deg`), generalized to four present modes only
> once S1.3 and S2.1 have recorded what the Pi's framebuffer actually is. Treat
> `RB_DFB_PRESENT` as reserved until then. [13](13-raspberrypi4.md) carries the
> same framing.

| `RB_DFB_PRESENT` | What it will do | When |
|---|---|---|
| `off` | Writes straight into the fb, no conversion. Fastest, tears. | **First bring-up.** It proves the mode-set works with nothing else in the way. |
| `convert` | Pixel-format conversion only (565 → 8888). | The fb is 32 bpp. **Not this port's path** — the Pi's fb measured 16 bpp RGB565, which is `rbp`'s native format. Kept for a sink that presents 32 bpp. |
| `letterbox` | 1:1 copy, centred, with filled bars. | The fb is **larger** than 1280×800 in *both* axes, so the image fits inside it. |
| `scale` | **Not implemented.** Uniform downscale to fit, bars in the other axis. | The fb is **smaller** than 1280×800 in either axis. **This is the measured Pi case (1280×720)**: 800 > 720, so a 1:1 copy cannot fit and `letterbox` is unsatisfiable. |
| `rotate` | The previous target's portrait path (`RB_DFB_ROTATE=left` maps to it). | Not used on a Pi. Kept so the driver has one code path, not two. |
| *(no present mode)* | `rot_deg = 0` — what the driver does today: a plain write into the fb at the logical geometry. | Where the Pi starts, and where it can stay **if the mode is ≥ 1280×800**. |

That last row is where the Pi starts, but S1.3's measurement narrows what it
means: the *format* needs no work (16 bpp RGB565 is `rbp`'s own), so the whole
conversion half of the generalization is dead on this sink. The *geometry* does,
unless the connector is asked for a mode that fits — the fb came up 1280×720,
and a 1280×800 image does not fit in it at any 1:1 placement. The ladder is
therefore `off` → `letterbox` (with a mode ≥ 1280×800) → and only then `scale`,
which is a decision rather than a config change, because a software 0.9×
downscale at 60 fps or the `drmkms` hardware-scaler path are both real work. See
[13 — Raspberry Pi 4](13-raspberrypi4.md) S1.3 for the dump and the three
commands that settle which one is needed.

When the generalization is done it adds `present_mode`, `present_dx/dy`,
`present_pan`, `present_dst_bpp` and `fbdev_fill_bars()` to `FBDevShared`, and
hoists the `deg == 0` fast paths: 2 bpp → 2 bpp is a per-row `memcpy` (the Pi
case, ~2 MB per frame), 2 → 4 is the existing convert at two pixels per
iteration, 4 → 4 is a row `memcpy`. The `rot_deg` gates in `dfb_fbdev_pan()`,
`dfb_fbdev_test_mode()` and `fbdevLock()` move to `present_mode` gates.
`rot_scratch`/`rot_surface` are then allocated whenever `present_mode != OFF`,
not only when rotating.

### The frame pacer

`RB_PAN_PACER_MS` (default `16.666666`) paces `FBIOPAN_DISPLAY`. `vc4`'s
`drm_fbdev` returns from a pan **without waiting for vblank** — the same
situation that let `rbp`'s RT-priority-98 render thread spin a core at 100% on
the previous target. Without the pacer, that happens here too. `0` disables it.

### Why the shim's "lie" is load-bearing

The final `dfb_fbdev_set_mode()` read-back goes through the **intercepted**
`FBIOGET_VSCREENINFO`, so DirectFB's `current_var` is the logical
1280×800×16. The *real* geometry lives only in `shared->real_var`, read with a
raw `syscall(SYS_ioctl)` that bypasses the shim, and only the present path uses
it. Do not "clean this up".

## Required: `directfbrc`

`usr/etc/directfbrc` (and `/etc/directfbrc`) **must** contain:

```
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
```

`scripts/build-chroot.sh` writes this file. Without `no-hardware`, DirectFB
takes the GPU/dri path; the build is `--with-gfxdrivers=none --disable-devmem`
so `no-hardware` is the *verified* configuration, and on the previous target
skipping it oopsed a kernel that booted with `panic_on_oops=1`.

## Debug instrumentation: removed, and staying removed

The production build must not contain per-frame or per-input-event debug I/O.
Specifically, the following were removed and **keeping them out is mandatory**,
not cosmetic:

* `primaryFlipRegion()` used to `fopen` and `fwrite` the entire ~6 MB triple
  buffer to `/tmp/rot_surface.dump` **on every flip** — a severe throughput
  bug, not noise.
* `fbdev_surface_pool.c` did the same into `/tmp/win_surface.dump`.
* `wm/default/default.c` opened `/tmp/dfbdig*.log` on **every pointer event**
  and logged each axis motion.

What is kept, deliberately, is one-shot start-up instrumentation: the
`ROTINIT` geometry line, the `CreateEventBuffer`/`CreateInputEventBuffer`
traces, the module/layer/windowstack startup lines, and
`local_surface_pool.c`'s two-shot `localsurf.dump` (header only). The rule is
in [tools/build-directfb/README.md](../tools/build-directfb/README.md):
**instrumentation must fire once**; anything per-frame or writing a whole
surface does not belong in the tree.

## Artifacts

All supplied by the DirectFB build (see
[`tools/build-directfb/`](../tools/build-directfb/README.md) and
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh)):

| Piece | Source |
|---|---|
| DirectFB 1.4.16 core libs | DirectFB build tree |
| patched fbdev module | `libdirectfb_fbdev.so` |
| `libdirectfb_linux_input.so`, `libdirectfbwm_default.so` | display build modules |

Rotation, where it is used, is selected at runtime with `DFB_ROTATE=left`
(90° CCW). Wrong value = UI sideways.

## Notes

* Do **not** set `layer-size` or `layer-rotate` in `directfbrc`.
* A DRM fb cannot pan; the driver falls back to `FRONTONLY` and keeps the real
  `yres_virtual`. That is why the pacer exists rather than relying on
  triple-buffered pan-flipping.
* Measure before changing anything: `gcc -O2 -static -o fbdump tools/fbdump.c`
  on the Pi, then `./fbdump`. It is read-only and prints a plain-language
  verdict for the present path.
