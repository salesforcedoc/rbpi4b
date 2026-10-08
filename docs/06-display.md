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
video=HDMI-A-1:1280x800@60 video=HDMI-A-2:1280x800@60 fbcon=map:1 console=tty3 consoleblank=0 vt.global_cursor_default=0
```

The second `video=` token forces the same mode on the other micro-HDMI port; it
does not make moving the cable seamless (rbp opens `/dev/fb0`, and there is no
`fb1` to point `RB_FB_DEV` at if nothing was attached to `HDMI-A-2` at boot).
[13](13-raspberrypi4.md#the-hdmi-mode) carries the row and the reasoning.

**`install.sh` applies this block itself now**, through
[`boot-trim.sh`](../scripts/device/boot-trim.sh) — with one deliberate omission:
it ensures the `HDMI-A-1` token and **not** the A-2 one, which
`boot-trim.sh status bootfiles` names as a gap rather than closing
([13](13-raspberrypi4.md#boot-time)). The block above is what it implements, and
what a by-hand bring-up still needs.

**The token is a boot-time instruction, not a standing one.** It is re-added to
the connector's mode list on every hotplug re-probe and validated against the
CRTC/encoder rather than the sink's EDID — and on a replug it can be *refused*.
Measured 2026-09-26 (S10.5): a same-mode unplug/replug logged

```
vc4-drm gpu: [drm] User-defined mode not supported: "1280x800": 60 83496 1280 1344 1480 1680 800 801 804 828 0x20 0x6
```

and the CRTC went on driving exactly that mode, because what keeps a replug alive
is the fbdev **not being torn down**, not the mode being re-imposed. A
`systemctl restart rblive4` re-runs none of this either: the kernel's mode setup
happens at boot, so a restarted unit keeps whatever geometry the new monitor gave
it — which is precisely the case the `scale` rung and the display watcher exist
for. See [13](13-raspberrypi4.md#s10--the-display-drills) S10.5.

`fbcon=map:1` binds the kernel console to `fb1`, so nothing draws over the UI —
without it, kernel messages and a blinking cursor land on top of rekordbox.

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
>
> **The `scale` rung and the automatic upgrade below are a later addition and are
> a different state of affairs.** They are built and artefact-verified, the fit
> arithmetic has unit coverage on both sides of the copy (see
> [the two copies](#a-mismatch-selects-the-rung-by-itself)), and the code paths
> are exercised by tests that run with no hardware at all. **Four of the display
> drills have since been run on the unit** — S10.1 (1280×720), S10.2 (960×600 and
> 800×600), S10.3 (1920×1080) and S10.4 (1280×800, the no-regression row), on
> 2026-09-26 — so the rectangles in the worked table below are **measured where
> marked and predicted where not**, and the budget paragraph says which of its
> numbers stopped being estimates. The headline: a panel whose geometry differs
> from 1280×800 in either axis gets the whole UI, correctly placed and **without
> anyone selecting anything**, at 1.1–3.4 ms of a 16.67 ms frame. What the same
> rows found and did **not** fix is that a *downscale* loses single-pixel rules
> and thin glyph strokes at 0.75× and 0.625× — S10.2, and a decision rather than a
> defect. The `off` path on a matched 1280×800 panel — the path this port actually
> ships — is unchanged, and S10.4 is the row that proves it. The drill block is
> [13](13-raspberrypi4.md#s10--the-display-drills).

| `RB_DFB_PRESENT` | What it does | When |
|---|---|---|
| `off` | No blit at all — the layer surface **is** the fb page. Fastest. | **The measured Pi, and the default.** The fb *is* the logical surface — same size, same format, same stride — so there is nothing to correct and nothing to copy. **On a fb that disagrees with the logical geometry this is not a slower picture, it is a broken one** — a sheared image on a larger fb, and no UI at all on a smaller one — so the driver upgrades it by itself; see [A mismatch selects the rung by itself](#a-mismatch-selects-the-rung-by-itself). |
| `convert` | 1:1, format-driven: pixel-format conversion (565 → 8888) when the fb's format differs, and a whole-frame `memcpy` when it does not. | A 32 bpp fb — **not this port's path**, since the Pi's fb measured 16 bpp RGB565, which is `rbp`'s native format. It is still the one mode worth setting on this target for a different reason: on a 565 fb it is the same-format whole-frame `memcpy` into a **system-memory back buffer**, i.e. the double buffering `off` cannot have, at the cost of one 2 MB copy per flip. That is the tearing remedy S2.3 reaches for. |
| `letterbox` | 1:1 copy, centred, bars filled. | The fb is **larger** than 1280×800 in *both* axes. **Not this port's path** — the forced `video=` mode produced exactly 1280×800, so there is nothing to centre into and no bars to fill. Kept for a monitor that offers a bigger mode. |
| `crop` | Copies the top N rows 1:1 — a row budget on the copy that already exists, no resampling. | The fb is shorter than 1280×800 but not narrower. **Not this port's path** — it was written up for the 1280×720 fb the connector's *mode list* implied, and the panel turned out to take 1280×800 when it was actually asked. Kept for a display that caps at 720p. It has a **column** budget as well as a row one: a surface *wider* than the fb used to copy nothing at all, which made an explicit `crop` on a narrow panel a black screen. The other modes keep the refusal, because for them a fb narrower than the surface is a misconfiguration and a partial image would hide it. |
| `scale` | Uniform resample to fit the fb — aspect-fit, centred, bars in the unused axis. Nearest-neighbour, into the same pixel formats the other rungs use, with an exact fit falling through to the 1:1 body rather than resampling by 1.0. | The fb differs from 1280×800 in either axis. **This is the automatic choice for that case**, and the mode that makes a panel which is not 1280×800 usable at all. `RB_DFB_PRESENT=scale` selects it explicitly, and S10.1 has run it both ways: with `AUTO=0`, verifying the rung *before* anything selects it, and with the default, where the upgrade chose it unaided. `DFB_PRESENT_FIT=stretch` fills both axes instead of preserving the aspect — also verified on glass, at 1280×800 (where it is identical to `fit`) and at 1280×720 (full width, no bars, against `fit`'s 1152×720 with 64-px bars). |
| `rotate` | The previous target's portrait path. `RB_DFB_ROTATE=left` still selects it, with its angle. | Not used on a Pi. Kept so the driver has one code path, not two. Refused on a fb that is not 32 bpp, with a log line saying so, because its loops store 4-byte pixels. |

The Pi runs with **no present mode set**, which is `off` — and `off` is not a
degenerate case of the others, it is the case where the fb *is* the surface: no
copy, so no ~2 MB per frame of anything. The measurements settled that: the
*format* needs no work (16 bpp RGB565 is `rbp`'s own), the fb is the same
**width** as `rbp`'s surface, and — the part that was in doubt — it is the same
**height** too. The connector's mode list offered nothing ≥ 1280×800, so the
working assumption was a 1280×720 fb and a decision about the bottom 80 rows;
but a `video=` mode is programmed whether or not the list names it, and the panel
took 1280×800. There is no geometry left to bridge, so `crop` is dead on this
target rather than being the live ladder — and `scale` is dead on *this panel*
while being the rung that keeps the next panel alive, since a monitor that comes
up at another size is the case it exists for. See
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

### A mismatch selects the rung by itself

The mode ladder above is a ladder you had to climb by hand, and on a swapped
monitor that is the wrong shape of answer: nobody is at a console when the
operator moves the cable. So when the mode is `off` — the default, and the only
mode the shipped configuration names — and the real fb disagrees with the shim's
logical 1280×800, the driver picks the rung itself:

| the mismatch | what it selects |
|---|---|
| a different width, height **or** stride-in-pixels | `scale` |
| geometry identical, `bits_per_pixel` different | `convert` |
| nothing | `off`, unchanged |

This is arithmetic, not judgement, and it is the reason the `off` row in the
table above says *broken* rather than *slower*: under `off` the layer surface
**is** the fb page, so the pool hands out an offset in the real fb with the
logical pitch. A larger fb (1920×1080, stride 3840 bytes) passes the region test
and rbp then draws a 1280×800 image at a 2560-byte stride into a 3840-byte-stride
page — a diagonal smear, every frame. A smaller one (1280×720) fails `need_mem`
against the shimmed geometry, so there is no primary region and no UI at all,
which is [12](12-troubleshooting.md#a-monitor-that-is-not-1280800)'s "no UI at
all, but rbp is running" with a different cause. Neither is a degraded picture;
both are a dead screen, deterministically.

The upgrade is **only** for the default. An explicit `scale`, `letterbox`, `crop`,
`convert` or `rotate` is honoured as written, and `DFB_PRESENT_AUTO=0` turns the
whole thing off — that is the control S10.0 and S10.4 use.

The four knobs are `rb.conf`'s display block, which documents each:
`RB_DFB_PRESENT_AUTO`, `RB_DFB_PRESENT_FIT`, `RB_DFB_PRESENT_PX_BUDGET` and
`RB_DFB_PRESENT_SKIP`. The launcher strips the `RB_` and hands the rest to the
driver, so a name in this document and a name in `rb.conf` differ by exactly that
prefix.

Two things make this safe to have on the path that ships:

* **On a matched fb the branch is dead.** The shipped configuration is
  byte-for-byte what it was; S10.4 is the row that proves it, by requiring the
  same `PRESENT:` line and a pixel-identical capture.
* **It cannot present a rectangle wider or taller than the fb.** The `scale`
  rung's destination comes from one function, `fbdev_present_fit()`, whose step
  is a 16.16 value that is **floored**: `s <= (fw << 16) / sw`, so
  `sw * s <= fw << 16` holds, and the final shift — which **rounds to nearest**,
  `dw = (sw * s + 32768) >> 16` — adds less than one unit and therefore cannot
  push `dw` past `fw`. Both halves are load-bearing, and the second is the one
  that is easy to get wrong: with a floored *result* as well, every inexact fit
  loses a whole row and column — 1280×720 would come out 1151×719 and 1080p
  1727×1079 — leaving a one-pixel bar on two edges and none on the other two.
  Worked, for a 1280×800 source; the last column is what the unit actually did:

| real fb | s | dw×dh | bars | measured on the unit |
|---|---|---|---|---|
| 1280×800 16bpp | 1.0000 | 1280×800 | none — exactly the `off` case | **S10.4: `mode=off`, and no fit or average line at all** — no regression |
| 1280×720 | 0.9000 | 1152×720 | 64 columns each side | **S10.1: 2.76 ms avg, `fbdump` 0.9000** — whole UI, cursor inside the picture |
| 960×600 | 0.7500 | 960×600 | none (same 1.6 aspect) | **S10.2: 1.09–1.26 ms avg, worst 2.67 ms, `fbdump` 0.7500** — complete, but glyph strokes break |
| 800×600 | 0.6250 | 800×500 | 50 rows top and bottom | **S10.2: 1.23 ms avg, worst 2.60 ms** — complete, glyph strokes break badly |
| 1920×1080 | 1.3500 | 1728×1080 | 96 columns each side, 2.07 Mpx | **S10.3: 3.34 ms avg, worst 7.66 ms** on a fresh boot — the budget default stands |
| 1920×1200 | 1.5000 | 1920×1200 | none, 2.30 Mpx | not run |
| 2560×1440 | 1.8000 | 2304×1440 | 128 columns each side, 3.32 Mpx | not run (S10.3's second half) |
| 3840×2160 | 2.7000 | 3456×2160 | 192 columns each side, 7.47 Mpx | not run — 7.47 Mpx is ~3.6× the budget, so a skip **and** the loud warning are what to expect |

An exact fit is not resampled: when the destination comes out `dw == sw && dh ==
sh` the new arm falls into the existing 1:1 body. That is what a padded-stride fb
of the same geometry wants, and it keeps the `off`-equivalent case on the fastest
path. The bars are refilled on **every** present rather than once — and their cost
is still an estimate (~0.3 ms at 1080p): S10.3's average *includes* the bar fill
and does not separate it out, so nothing here should quote a measured share — and
self-healing, where a once-only fill would need a per-page done-bitmask and a proof
about who else may write there.

**The two copies.** The rectangle is decided by the same rule in two places:
`fbdev_present_fit()` in the driver, which places the picture, and `point_fit()` in
`scripts/shims/point_xform.c`, which places the mouse arrow inside it — because the
driver is cross-built for the target and the shim is a separate `LD_PRELOAD`
build, so they cannot share a header. Comments saying *a change to one is a change
to both* are a promise, not a check, and the drift they let through is invisible on
a 1280×800 panel (where both are the identity) and shows up only on the mismatched
one the fit exists for. So there is a check:
[`tools/fit-crosscheck.sh`](../tools/README.md#fit-crosschecksh--the-two-copies-of-one-rule)
extracts the driver's copy from the **tracked** `directfb-full.diff` and sweeps
both over 7,709,328 sizes in fit and stretch — measured, 0 differ. The shim's copy
also has unit coverage in `test_point`, and the *use* of the rectangle has coverage
in `test_cursor_dev`, which drives the production `fb_cursor.o` against a faked
kernel and asserts that the arrow lands at the picture's coordinates and that the
bars never change a byte.

**The frame budget.** A software resample is not free, and this one runs on rbp's
`SCHED_FIFO 98` render thread — the same thread
[13](13-raspberrypi4.md#the-present-path) records spinning a core at 100% when the
pan went unpaced, which is why it is scheduled at that priority and why its
budget is measured rather than hoped for. The cost is bounded by a
**pixel budget**:
`DFB_PRESENT_PX_BUDGET` (default 2,600,000 weighted output pixels, doubled for a
32 bpp destination) selects `skip = ceil(cost / budget)`, capped at 4, and the
pan is skipped along with the blit — a skipped pan would show a page that was
never drawn. The panel then updates at `60/skip` Hz while rbp's own loop keeps its
60 Hz and the panel keeps the last whole frame. That is the honest trade.

Three things about it are worth stating plainly, because they are where a reader
would otherwise guess:

* **The default survives its measurement.** It began as a guess from a cycle-count
  argument (565→565 at 2.07 Mpx ~6–8 ms of a 16.67 ms budget; 565→8888 12–18 ms;
  2560×1440 and 3840×2160 12–20 ms and 30–45 ms). **S10.3 then measured the case it
  was guessed for — 1728×1080, the same pixel count — at 3.34 ms average and
  7.66 ms worst frame, with `skip=1` and no skip needed**, so
  `DFB_PRESENT_PX_BUDGET` keeps its shipped 2,600,000. The 32 bpp figure and the 4K
  figure are **still estimates**: the `565→8888` arm and any 32 bpp fb cannot be
  reached on this unit (the `video=…-32` regression, [13](13-raspberrypi4.md)), and
  2560×1440 was not run. 1.09–3.34 ms is the measured range for the whole `565→565`
  arm across the four rectangles.
* **`DFB_PRESENT_SKIP` and the empty string are not the same setting.** Non-empty
  enters an override branch and `atoi`s it, capped at 8, and anything not `> 0` is
  ignored — so `"0"` leaves the skip at 1, which means "no skip", **not** "auto".
  Only unset or empty falls through to the budget. Hence the shipped default in
  `rb.conf` is the empty string, and saying so is the point of that comment.
* **A cost above twice the budget logs a warning naming the remedy** — set a
  `video=` mode near 1280×800. On a 4K panel that warning *is* the deliverable;
  pretending a 4-frame skip is a usable UI would not be.

Three one-shot lines go to `/tmp/dfbdig9.log`. First the decision, and **only**
when the upgrade fires:

```
PRESENT: auto -- real fb 1280x720 pitch 2560 does not match the logical 1280x800 pitch 2560, and 'off' cannot present that; using mode=scale
```

Then `mode`, the real geometry, the logical one, and the fit:

```
PRESENT: mode=scale angle=0 real_fb=1280x720 pitch=2560 bpp=16 pages=1 pan=0 (logical 1280x800) scale=1152x720 dx=64 dy=0 fit=fit skip=1
```

The suffix is appended **only** under `scale`, so the line a matching fb produces
is byte-for-byte what it was before the rung existed — which is what S10.4 diffs,
along with the *absence* of the `auto` line. And when the scale path is live, at
the 300th presented frame, the measured average:

```
PRESENT: 300 presented / 300 flips, avg 3.34 ms per present after 60 warmup, worst 7.66 ms (60 Hz budget 16.67 ms), skip 1
```

That line reports the window **after 60 warmup presents**, because rbp's own
bring-up inflates the earliest frames and a budget read off the whole window would
be pessimistic; it carries the **worst single present** beside the average, because
an average alone cannot be told apart from one freeze that the other frames are
averaging away; and it is gated by a `present_reported` flag rather than by
`present_done == 300`, because with `skip > 1` that count stalls at the trigger and
the equality held on every skipped flip — measured, eight identical lines in a row
at `skip=8`. All three were arrived at on the unit; the readings that forced them,
and the two high ones that did not reproduce, are in
[13](13-raspberrypi4.md#the-intermittent-high-reading).

Every one of them is written to the log file rather than to `D_ERROR`, because rbp
calls `DirectFBSetOption("quiet")` and every `D_ERROR` on this path is suppressed
in production. They cost nothing on the `off` path, which is the one that ships:
the clock calls are paid only under `scale`.

**Nothing notices a monitor on its own.** The driver reads the geometry once, at
open, and holds it; so does `fb_cursor.c`. A monitor swapped for one of a
*different* size therefore leaves a picture drawn to the old geometry until
something restarts the player, which is what
[`scripts/device/display-watch.sh`](../scripts/device/README.md) does — it polls
`/sys/class/graphics/fb0` and restarts the unit when the geometry stops matching
the one rbp was launched with, bounded by a cooldown and a per-boot cap. A
same-size unplug and replug never fires, by design. That script, not this one, is
where the hot-swap handling lives; see
[11 — Runtime launcher](11-runtime-launcher.md). **Its decision logic is now
measured — S10.6 drove it in `--dry-run` against a fake `RB_SYSFS_ROOT`, so a
mismatch, a missing framebuffer and a same-geometry return all produced exactly
the decisions described here, and it never fired across three different baselines
on the real `/sys`.** What is **not** measured is a *real* fire: the restart, the
journal around it, and the cooldown and per-boot cap, which `--dry-run` cannot
reach because it returns before the counter is written. S10.5, S10.9 and S10.10
are the physical rows that settle those
([13](13-raspberrypi4.md#s10--the-display-drills)) — until they run, the restart
half of this paragraph is design, not measurement.

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
`lib/direct/modules.c`'s module-loading probe into `/tmp/dfbdig4.log`. The `scale`
rung added two more of the same kind — the "upgraded `off`" notice, and the frame
average written at the 300th present — both of which fire once and neither of
which is reached on the `off` path this port ships. The rule is
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

## Watching the page: whose pixels are those?

When something looks wrong on the glass, the first question is **who drew it**,
and there are only three candidates on this port:

| writer | what it can leave behind |
|---|---|
| `rbp` | everything, and a real RX3 draws the same picture |
| the pointer arrow (`fb_cursor.c`) | a small saved-under rectangle at the pointer's last position, restored after rbp repainted over it |
| the swipe menu (`menu_draw.c`) | a stale band in rows 0..55, the region it saves and repairs |
| the **overlay plane** (`drmband.c`) | nothing in `/dev/fb0` at all — it is a separate plane composited *above* rbp's page, so a screenshot cannot see it |

**The overlay plane is a fourth writer and it is not in the page.** When the band, the
browser window, a side drawer or the USB STOP chooser is up, the shim owns a `vc4`
overlay plane on `card1` and composites it over rbp's primary.

**The UTILITY gesture is the one shim surface with no pixels at all.** It is input-side
only: it sits on a band of *rbp's own* UTILITY screen, swallows the reports it claims and
answers them with `K_SELECTOR` keycodes, so it neither owns a plane nor writes a row of
the page — and unlike the three above, nothing in `/dev/fb0` or in the plane list will
ever show it working. Its gate, geometry and rules are in [07](07-touch.md) § *Touch in
rbp's own UTILITY screen*; for a capture to prove anything there, rbp's own frame has to
move, which is why that drill is a frame diff and not a screenshot.

**The device is shared; the plane is not.** DRM master is one per open **file**, so a
second `drm_band_setup()` in a process was once refused `EBUSY` and the band, the window
and the drawer had to pass one plane between them. `drmband.c` now opens the card
**once** and refcounts the fd and the master (`dev_acquire()` / `dev_release()`), so each
band is only a buffer, a framebuffer and a plane of its own, and `pick_plane()` skips any
plane whose `crtc_id != 0` so two bands cannot land on one. That is what lets **both edge
drawers be out at the same time** — request 1, and the reason the two of them no longer
exclude each other.

The **band's** plane is still mutually exclusive with the drawers', and the reason has
changed from "one master" to the compositor itself: the band and a drawer overlap on the
glass at the top corners, and two overlays cannot share a `zpos`. Opening a drawer
therefore tears the band's plane down, and closing gives it back and re-reads the live
page so the strip is not a stale image. This is a property of the plane, not a bug, and
it is stated where the operator will meet it (`07-touch.md`).

Which surface owns which plane is legible from the log without any pixels at all,
because `drm_band_setup()` prints the geometry it was asked for: `1280x56` is the band,
`180x800` is a drawer, `560x127` is the USB STOP chooser's box (it was `560x271`
until the box went to two buttons on 2026-10-07), and the browser window
has its own size. A handover prints the old surface's teardown, the new one's setup, and
a line naming the direction (`side: a drawer is out -- the plane is handed over from the
band`). Measured on the unit with both drawers out: **two distinct plane ids on the same
crtc**, `plane 127` and `plane 138`, both `180x800`, neither reusing the other's. The
box takes whichever plane is free when it opens — measured on 2026-10-05 as `plane 138`,
the id a drawer had used a moment before, which is the freed plane being picked up again
rather than a collision.

**The box's own picture cannot be screenshotted either, and has its own reader.**
`work/boxshot2.py` fetches it the way any DRM client would — open `card1`, ask for the
framebuffer **by the id the shim logs**, map its dumb buffer and read it. That exists
because the obvious route does not work here and the failure looks like a bug in the thing
being measured: vc4's dumb buffers are `PFNMAP`-style mappings and `get_user_pages()`
refuses them, so `/proc/pid/mem` at a perfectly live plane address returns `EIO` — for the
band's plane exactly as much as the box's (measured on both, 2026-10-05). Reading by fb id
from a second client touches nothing: no `SetCrtc`, no master, no auth, and the plane rbp
is scanning out is never written.

On the `MENU_PLANE=0` fallback there is no overlay plane and the same drawings go into
rbp's own page and are repaired through the degraded path (`menu_blit()`), where they
shimmer exactly as the band's fallback does — and only on machines where the plane
cannot be had. A **drawer** on that route is copied from its off-lock image into the
page **every tick**, per row (the page's stride is not the image's), because rbp repaints
over it and there is no damage witness for a region this big; the paint is still
change-gated, only the copy is per-tick. The drawer's image pair and the reason the build
sits off the lock are in [07](07-touch.md) § *The fader's flicker*.

**The present path is not a candidate on the measured panel.** `RB_DFB_PRESENT`
defaults to `off` (`rb.conf:94`), and the upgrade to `scale` only fires when the
real fb disagrees with 1280×800 (`:107`) — and [`PRESENT:` records that it does
not](06-display.md). Under `off` the layer surface *is* the fb page, so rbp's
pixels reach the glass with nothing in between: no resample, no bars, no crop.
A symptom that looks like a scaling artifact cannot be one here.

`tools/fbwatch.py` answers the question by catching the frames instead of
asking:

```
python3 tools/fbwatch.py              # 120 s at 1 fps, then pull and convert
python3 tools/fbwatch.py --secs 60 --fps 2
```

It runs a loop **on the unit** that reads `/dev/fb0` into a RAM ring and prints a
line for every frame that *differs* from the one before it, with the range of
rows that differ — the 2 MB comparison is a single big-integer XOR, so it costs
nothing. The operator loads the track when it prints `GO`; the loop is
continuous, so the moment is caught rather than synchronised with, which is the
rule every on-glass probe on this port follows. At the end it writes the ring to
`/tmp/fbseq` on the unit, pulls only the changed frames plus the two before
each, and converts them to `work/fbseq/*.png` for the eye to settle. Reading the
row ranges first is what makes the pictures cheap: a span at 0..55 is the menu, a
small box at the pointer's last position is the arrow, and anything large and
structured across a deck is rbp's own redraw and belongs to the vendor.

`Ctrl-C` kills the remote loop by the pid it writes to `/tmp/fbwatch.pid` — not
by a pattern match, because a `pkill` pattern that also matches the caller's own
command line is how an ssh session kills itself on this rig. The frames land in
`/tmp` on the unit, which is tmpfs: pull them before rebooting, or lose them.

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
