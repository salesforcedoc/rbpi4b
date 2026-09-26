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

## What the framebuffer is (measured)

On the Pi 4B, `/dev/fb0` is **`vc4drmfb`** — DRM fbdev emulation, not a
hardware fb like the previous target's `rockchipdrmfb`. Measured on the unit
(S1.3 and S2.1, raw output in [13](13-raspberrypi4.md#the-hdmi-mode)):

| Field | Measured |
|---|---|
| format | 16 bpp RGB565 — `rbp`'s own format |
| geometry | 1280×800, exactly what `video=` asked for |
| `line_length` | 2560 — no stride padding |
| `yres_virtual` | `== yres` — one page, no room to double-buffer |
| `ypanstep` | **`1/1`** — the fb claims it can pan, and cannot |

The last two rows are the whole story of this section: the fb is `rbp`'s logical
surface to the byte, **and** it reports panning it does not have. The format half
of the present path is dead on this target — no conversion, no crop, no
letterbox. The **page** half is not: `ypanstep 1/1` is the value the driver used
to decide how many buffers the primary layer may have, and the answer it gives is
three, in a fb that holds one. See [the present path](#the-present-path).

The previous target, for contrast, was a **DSI 800×1280 portrait panel** at
32 bpp with a triple-buffered 800×3840 virtual fb — the geometry is why its
present path was a rotation and the Pi's is nothing at all.

## For reference: the previous target's facts

* `/dev/fb0` = `rockchipdrmfb`, 800×1280 portrait, 32 bpp, stride 3200,
  triple-buffered (`virtual_size = 800,3840`).
* DSI-1 connector, Mali lib bind-mounted at `/usr/lib/libmali.so.14.0`.
* `/tmp/dfbdig9.log` held `ROTINIT: real_fb=800x1280 pitch=3200
  (orig_var=1280x800)` — now the `PRESENT:` line, which records the same real
  geometry plus the mode, the fb bpp, the page count and the pan decision.

That one-shot start-up line is deliberately **kept**: it is the cheapest way to
see what the driver decided, and it is the first thing to read when the UI does
not appear. The Pi logs its own; on the measured unit it reads

```
PRESENT: mode=off angle=0 real_fb=1280x800 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800)
```

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

> **State of this section: written, built, and confirmed on the device (S2.2).**
> The generalization below is in the patch (`directfb-full.diff`,
> `systems/fbdev/`), the trees reproduce from it byte for byte, and the stack
> builds clean with no new compiler warnings. The on-device half has now
> happened: the Pi draws the rekordbox UI with `PRESENT: mode=off … pages=1
> pan=0`. Two deploy-level problems had to be cleared first — a missing
> `module-dir` and stray AppleDouble files — and neither was a defect in the
> code below; both are written up in
> [13](13-raspberrypi4.md#bring-up-order) and [12](12-troubleshooting.md), and
> the `module-dir` requirement is in
> [Required: directfbrc](#required-directfbrc).

| `RB_DFB_PRESENT` | What it does | When |
|---|---|---|
| `off` | No blit at all — the layer surface **is** the fb page. Fastest. | **The measured Pi, and the default.** The fb *is* the logical surface — same size, same format, same stride — so there is nothing to correct and nothing to copy. |
| `convert` | 1:1, format-driven: pixel-format conversion (565 → 8888) when the fb's format differs, and a whole-frame `memcpy` when it does not. | A 32 bpp fb — **not this port's path**, since the Pi's fb measured 16 bpp RGB565, which is `rbp`'s native format. It is still the one mode worth setting on this target for a different reason: on a 565 fb it is the same-format whole-frame `memcpy` into a **system-memory back buffer**, i.e. the double buffering `off` cannot have, at the cost of one 2 MB copy per flip. That is the tearing remedy S2.3 reaches for. |
| `letterbox` | 1:1 copy, centred, bars filled. | The fb is **larger** than 1280×800 in *both* axes. **Not this port's path** — the forced `video=` mode produced exactly 1280×800, so there is nothing to centre into and no bars to fill. Kept for a monitor that offers a bigger mode. |
| `crop` | Copies the top N rows 1:1 — a row budget on the copy that already exists, no resampling. | The fb is shorter than 1280×800 but not narrower. **Not this port's path** — it was written up for the 1280×720 fb the connector's *mode list* implied, and the panel turned out to take 1280×800 when it was actually asked. Kept for a display that caps at 720p. |
| `scale` | **Not implemented**, and it says so and presents without a blit. Uniform downscale to fit, bars in the other axis. | The fb is **smaller** than 1280×800 in either axis. **Not this port's path**, for the same reason as `crop` — and it is the one rung that would always be a decision rather than a config change, because a software 0.9× downscale at 60 fps or the `drmkms` hardware-scaler path are both real work. |
| `rotate` | The previous target's portrait path. `RB_DFB_ROTATE=left` still selects it, with its angle. | Not used on a Pi. Kept so the driver has one code path, not two. Refused on a fb that is not 32 bpp, with a log line saying so, because its loops store 4-byte pixels. |

The Pi runs with **no present mode set**, which is `off` — and `off` is not a
degenerate case of the others, it is the case where the fb *is* the surface: no
copy, so no ~2 MB per frame of anything. The measurements settled that: the
*format* needs no work (16 bpp RGB565 is `rbp`'s own), the fb is the same
**width** as `rbp`'s surface, and — the part that was in doubt — it is the same
**height** too. The connector's mode list offered nothing ≥ 1280×800, so the
working assumption was a 1280×720 fb and a decision about the bottom 80 rows;
but a `video=` mode is programmed whether or not the list names it, and the panel
took 1280×800. There is no geometry left to bridge, so `crop` and `scale` are
dead on this target rather than being the live ladder. See
[13 — Raspberry Pi 4](13-raspberrypi4.md) S1.3 and S2.1 for the dump and the
mode list.

**What needed the work was pages, not pixels.** `fbdump` reports `ypanstep 1/1`
alongside `yres_virtual == yres` and an `smem_len` of exactly one frame
(`2048000 == 2560 × 800`) — one page, so `FBIOPAN_DISPLAY` has nowhere to pan to.
But `primaryInitLayer()` forced `config->buffermode = DLBM_TRIPLE`, and
`dfb_fbdev_mode_to_var()`'s guard against a non-pannable fb tests `ypanstep == 0`,
which this fb does not report. The guard missed, the `yres_virtual` tripling
happened, and `dfb_fbdev_test_mode()`'s `need_mem` came out at
`2560 × 2400 = 6,144,000` against `smem_len` `2,048,000` — a `DFB_LIMITEXCEEDED`
that fails `primaryTestRegion()` and takes the primary surface with it, leaving
`rbp` with nothing to draw on. Nothing about that is Pi-specific in kind: it is
what happens whenever a fb reports panning it has no pages for.

The buffer mode is therefore decided from the **page count**, not from
`ypanstep`. On this fb that yields a single `FRONTONLY` buffer,
`need_mem == smem_len` exactly, and — since a single-buffered primary surface
*is* the visible page — `rbp` drawing straight into the fb with every flip a
no-op pan. That is the same outcome `off` wants, arrived at from the other side:
the page count says one buffer, and one buffer means there is nothing to present
*from*. Add `debug=FBDev/Mode` to `usr/etc/directfbrc` to see the arithmetic
rather than infer it. Both halves of that line matter and neither is guessable:
the parser only splits an option at `=`, so a bare `debug FBDev_Mode` is passed
through as one name and rejected with `Invalid option 'debug FBDev_Mode'!` — a
warning it then **ignores**, which is why a typo here looks like the debug output
simply being absent. And the name it must be given is the domain's own name from
its `D_DEBUG_DOMAIN`, which for this one is `FBDev/Mode` with a slash, not the
`FBDev_Mode` C symbol.

The page count is read at open, from the **real** geometry (raw
`syscall(SYS_ioctl)`, see [below](#why-the-shims-lie-is-load-bearing)) and stored
as `present_pages` with `present_pan = present_pages > 1`. It gates four things,
and every one of them used to ask `ypanstep`:

* `primaryInitLayer()`'s `config->buffermode` — `FRONTONLY` unless the fb holds
  three pages;
* `dfb_fbdev_mode_to_var()`'s buffer-mode switch, which is what stopped
  `yres_virtual` tripling in a one-page fb;
* `dfb_fbdev_test_mode()`, which refuses a `TRIPLE`/`BACKVIDEO` request the fb
  cannot hold — so a request that gets that far falls back to the layer's own
  default instead of allocating buffers 2 and 3 past the end of the mmap;
* the pan and blit index in `primarySetRegion()`/`primaryFlipRegion()`, where a
  single-page fb forces the offset to 0: with one page there is no back page to
  draw into.

Two arithmetic bugs fell out with it, both of which had been latent in the
rotate path:

* The blit index is `lock->offset / (lock->pitch * h)`. It was
  `lock->offset / (sw * 4 * sh)` — a hardcoded 4 bytes per pixel against a
  surface that is `rbp`'s 16 bpp, so the index came out at half its true value.
* The pan target row was `idx * orig_var.yres`, the *logical* height, where the
  blit used `idx * fh`, the real one. Two different page strides for the same
  page — the fb's own (`rot_fh`) is the right one for both.

The generalization proper — the modes in the table — is `fbdev_present_primary()`
in place of `fbdev_rotate_primary()`: `mode`, `deg`, and where the surface sits
inside the fb (`present_dx/dy`), with `fbdev_fill_bars()`/`fbdev_fill_span()` for
the letterbox case. The `deg == 0` fast paths are the point on a target that
presents at 60 fps, where a frame is 2 MB: same format with no offset and equal
strides is **one `memcpy` of the whole frame**; same format with a different
stride is a per-row `memcpy`; 2 → 4 is the existing convert hoisted to two pixels
per iteration; 4 → 2 is new and is the only place that ever needed it.
`PRESENT_CROP` is that same row copy with the row count clamped to the fb's
height, so it costs no new pixel code at all. The rotation loops themselves are
the original ones, verbatim, behind a guard.

`rot_scratch`/`rot_surface` are allocated whenever `present_mode != OFF`, not only
when rotating, and that condition is load-bearing rather than stylistic: with
`off` there is no scratch surface wanted, and the correct outcome is `FRONTONLY`
with **nothing allocated** — which is the Pi. `fbdevLock()`'s pitch/address
override is gated the same way, and for the same reason: under every mode except
`off` the primary layer draws into `rot_surface` in system memory, and under `off`
it draws into the fb page.

### The frame pacer

`RB_PAN_PACER_MS` (default `16.666666`) paces `FBIOPAN_DISPLAY`. `vc4`'s
`drm_fbdev` returns from a pan **without waiting for vblank** — the same
situation that let `rbp`'s RT-priority-98 render thread spin a core at 100% on
the previous target. Without the pacer, that happens here too. `0` disables it.

### Why the shim's "lie" is load-bearing

The final `dfb_fbdev_set_mode()` read-back goes through the **intercepted**
`FBIOGET_VSCREENINFO`, so DirectFB's `current_var` is the logical 1280×800×16.
The *real* geometry lives only in `shared->real_var`, read with a raw
`syscall(SYS_ioctl)` that bypasses the shim, and only the present path uses it.
Do not "clean this up".

The geometry is read at open **unconditionally**, not only when a present mode is
set, and that is not tidiness either: `present_pages` — the buffer-mode decision
— is computed from it, so a build that only looked at the real fb when asked to
rotate would keep making the page decision from `ypanstep` in every other
configuration, which is the bug this section is about. The stride used for it is
the raw `line_length` for the same reason: the shim rewrites
`fix.line_length` to the *logical* 2560, and on a 32 bpp fb that is half the fb's
own stride, which would double the page count.

## Required: `directfbrc`

`usr/etc/directfbrc` (and `/etc/directfbrc`) **must** contain:

```
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
module-dir=/usr/lib/directfb-1.4-6
```

`scripts/build-chroot.sh` writes this file. Without `no-hardware`, DirectFB
takes the GPU/dri path; the build is `--with-gfxdrivers=none --disable-devmem`
so `no-hardware` is the *verified* configuration, and on the previous target
skipping it oopsed a kernel that booted with `panic_on_oops=1`.

`module-dir` is the fifth line and it is not optional. DirectFB searches the
**compile-time** `MODULEDIR` for every module whose path is relative — which is
all of them: `systems`, `inputdrivers`, `wm`. That value is baked in by the
DirectFB build's `--libdir`, and this build uses `--libdir=/lib`, so `MODULEDIR`
is `/lib/directfb-1.4-6` — while `scripts/build-chroot.sh` stages the modules at
`/usr/lib/directfb-1.4-6`, which is where this rootfs's own DirectFB 1.4.0
modules live. Nothing reconciled the two, so *no module loaded at all*:
`dfbinfo` reports `No system found!` and `DirectFBCreate() failed`, and `rbp`
crashes rather than reporting it (see [12](12-troubleshooting.md) for that
crash's signature). `module-dir` overrides `MODULEDIR` at runtime, and
`build-chroot.sh` now writes it from the same variable it stages with, so the
path named and the path used cannot drift apart — with a build-time assertion in
step 7 that fails the build if the modules are not where the file says.

Note the name is `module-dir`, a config *key* — unlike the `debug=` lines, this
one the parser splits correctly *and* honours.

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
`PRESENT` geometry/mode line, the `CreateEventBuffer`/`CreateInputEventBuffer`
traces, the module/layer/windowstack startup lines,
`local_surface_pool.c`'s two-shot `localsurf.dump` (header only), and
`lib/direct/modules.c`'s module-loading probe into `/tmp/dfbdig4.log`. The rule is
in [tools/build-directfb/README.md](../tools/build-directfb/README.md):
**instrumentation must fire once**; anything per-frame or writing a whole
surface does not belong in the tree.

The module probe earns its place by being the only thing that can see this
failure at all. A module-path mismatch is invisible from outside: `rbp` sets
`DirectFBSetOption("quiet")`, which suppresses DirectFB's own diagnostics, so all
the operator gets is a segfault in `init_Resource()` and a bare
`DS_HW_Glib3_DFB.c <293>:` with no message body. `dfbdig4.log` names the exact
directory searched and, on a miss, `opendir failed` with the errno — which is how
`MODULEDIR` was caught pointing at a directory that did not exist. It fires once
per module directory per process, three lines at most, all at startup. Keep it;
if a future change makes module loading fail again, this file is where to look
first.

## Artifacts

All supplied by the DirectFB build (see
[`tools/build-directfb/`](../tools/build-directfb/README.md) and
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh)):

| Piece | Source |
|---|---|
| DirectFB 1.4.16 core libs | DirectFB build tree |
| patched fbdev module | `libdirectfb_fbdev.so` |
| `libdirectfb_linux_input.so`, `libdirectfbwm_default.so` | display build modules |

The present mode is selected at runtime with `DFB_PRESENT`
(`off`/`convert`/`letterbox`/`crop`/`rotate`); the launcher's `RB_DFB_PRESENT`
in `rb.conf` is what exports it, and empty means unset — which is `off`.
`DFB_ROTATE=left` (90° CCW) still works and means `rotate` plus the angle, since
that is what the launcher has always set. A wrong angle = UI sideways; on a Pi,
`rotate` on a 16 bpp fb is refused rather than attempted, with a log line saying
so.

`FB_DEV` (the launcher's `RB_FB_DEV`) selects the framebuffer path, for the case
where the display is not `/dev/fb0`. It sits after `directfbrc`'s `fbdev=` and
before `FRAMEBUFFER`, so the documented DirectFB option still wins and the device
can be moved from `rb.conf` without editing a file inside the chroot.

## Notes

* Do **not** set `layer-size` or `layer-rotate` in `directfbrc`.
* A fb with one page falls back to `FRONTONLY` and keeps the real
  `yres_virtual`; `ypanstep` is not consulted for that any more (see
  [the present path](#the-present-path)). That is why the pacer exists rather
  than relying on triple-buffered pan-flipping: a single-buffered layer still
  pans, and `vc4` returns from a pan without waiting for vblank.
* Measure before changing anything: `gcc -O2 -static -o fbdump tools/fbdump.c`
  on the Pi, then `./fbdump`. It is read-only and prints a plain-language
  verdict for the present path.
