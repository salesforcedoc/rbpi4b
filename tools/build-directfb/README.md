# tools/build-directfb

Patched **DirectFB 1.4.16** for a DRM-fbdev-emulation framebuffer.

`rbp` renders through DirectFB. The stock RX3 fbdev driver assumes a 16 bpp,
1280×800, pannable i.MX6 framebuffer. A `drm_fb_helper` framebuffer (the
Rockchip `rockchipdrmfb` this port started on, and the Raspberry Pi's
`vc4drmfb`) has a fixed bpp, no panning and no rotation. Without changes, the
modeset is rejected (`EINVAL`), DirectFB corrupts its layer bookkeeping, and
`rbp` crashes (on the Rockchip, sometimes rebooting the device).

The diff is written against `drm_fb_helper`'s behaviour generally, not against
one SoC: the present path decides what to do from the geometry it *reads back*
rather than from assumptions about the panel. It has been run on the Rockchip
(Prime GO / SC Live 4); the Raspberry Pi 4 is the current target.

## Files

| File | What |
|---|---|
| `directfb-full.diff` | all port modifications against DirectFB 1.4.16 |

There are **no upstream DirectFB source files in this repository**. The diff is
the only third-party-derived artefact; DirectFB is LGPL-2.1 and the diff (and
any build you make from it) remains under the LGPL. You fetch the pristine
DirectFB tree yourself and apply the diff.

## What the patch changes

1. **Serialise all fb ioctls** with a mutex. The DRM fb is unsafe under
   concurrent `FBIOPUT_VSCREENINFO`/`FBIOPAN_DISPLAY`.
2. **Force the real fb format** in `dfb_fbdev_set_mode()` and
   `dfb_fbdev_test_mode()`: before `FBIOPUT_VSCREENINFO`, overwrite
   `bits_per_pixel` and the colour bitfields from a live
   `FBIOGET_VSCREENINFO`. The kernel then accepts the modeset and the region
   test passes, so window creation succeeds.
3. **Use the read-back state** after a rejected/clamped modeset for
   `shared->current_var` — never the rejected request (which corrupted the
   internal geometry).
4. **Decide the buffer mode from the page count**, not from `ypanstep`: a fb
   that reports panning but holds one frame gets `FRONTONLY`, keeping the real
   `yres_virtual`. `vc4` is exactly that fb (`ypanstep 1/1`,
   `smem_len == line_length × yres`), and the `ypanstep` test let the driver ask
   for three buffers in a fb that has one. See
   [docs/06](../docs/06-display.md#the-present-path).
5. **A general present path** in `fbdev_present_primary()`, replacing
   `fbdev_rotate_primary()`: the logical surface can be copied/converted into the
   physical fb (`DFB_PRESENT` = `convert`/`letterbox`/`crop`), rotated
   (`DFB_ROTATE`, still 90/270/180, still via a scratch buffer), or not presented
   at all (`off` — the layer surface *is* the fb page). Where a copy is needed it
   goes from a system-memory source surface, which is what eliminates tearing.
   The `deg == 0` fast paths are deliberate: same format and stride is one
   `memcpy` of the whole frame, so a 1280×800 frame is ~2 MB per flip rather
   than a per-pixel loop at 60 fps.
6. **Read the real fb geometry at open, always**, with a raw
   `syscall(SYS_ioctl)`: the shim lies about the logical size, and both the
   present path and the page count above need the truth.
7. **Forward `fstat()` to the kernel** (`lib/direct/compat_shim.c` and
   `systems/fbdev/compat_shim.c`, both new files) so the objects reference only
   symbols a glibc 2.13 target actually exports — see *The `fstat` forward*
   below.

The diff also touches core DirectFB — `lib/direct/modules.c`,
`src/core/{core,layer_context,local_surface_pool,system,windowstack}.c`,
`src/directfb.c`, `src/idirectfb.c`, `wm/default/default.c` — and two
`Makefile.am`s (`lib/direct`, `systems/fbdev`) — build the whole tree, not just
the fbdev module, so the patched core libs and modules are produced together.

## Build

Requires a DirectFB 1.4.x tree (the diff is against 1.4.16),
`autoconf`/`automake`, `libtool`, `pkg-config`, a **native** compiler for
fluxcomp (step 2) and the soft-float EABI5 cross compiler.

```bash
# 0. toolchain
sudo apt-get install gcc-arm-linux-gnueabi libc6-dev-armel-cross \
    autoconf automake libtool pkg-config patchelf g++

# 1. source
#    There is no 1.4.16 *tag* in deniskropp/DirectFB — only DIRECTFB_1_7_*
#    — so pin the 1.4 branch by commit. This commit is
#    DIRECTFB_MICRO_VERSION=16, i.e. what this diff was written against.
git clone https://github.com/deniskropp/DirectFB.git directfb
cd directfb
git checkout 363739298335a0f76dea2de1c9bc523293242baa   # branch directfb-1.4

# 2. fluxcomp — the IDL compiler, from its own package
#    src/core/*.flux are compiled by fluxcomp into the CoreDFB.c/.h and friends
#    that src/core/Makefile.am lists among its sources, and fluxcomp is NOT part
#    of DirectFB: it moved to its own package in 2011, and only the distribution
#    tarballs ship the generated files. Without it the build stops at the first
#    .flux file with "fluxcomp: command not found" and make reports only
#    "Error 127" — which reads like a broken rule, not a missing package.
#
#    fluxcomp runs on THIS machine and emits C that the cross compiler then
#    builds for the target, so build it natively. CXX=g++ is explicit because
#    the DirectFB environment below exports a cross CC/CFLAGS carrying a
#    --sysroot, and a native gcc handed those would go looking for armel crt1.o.
#    (DirectFB's ChangeLog has the same note — "build: set CXX=g++ for fluxcomp,
#    to avoid the cross compiler from being used".) Run this before those
#    exports, or use env -u CC -u CFLAGS -u LDFLAGS. And note that this package's
#    autogen.sh is a bare "autoreconf -fi" that never runs configure, so the
#    configure below is not optional.
#
#    Its configure warns "*** libdirect not found -- building without libdirect".
#    That is expected (DirectFB is not installed yet) and harmless: it disables
#    fluxcomp's own debug output, not the C it generates.
cd ..                        # the shell is inside directfb/ from step 1
git clone https://github.com/deniskropp/flux.git flux
cd flux
git checkout 10ad2ebc78b396032714b839f200848ea0dd9503   # 1.4.4, the last flux
autoreconf -fi
CXX=g++ ./configure && make -j"$(nproc)"
export PATH="$PWD/src:$PATH"
cd ../directfb

# 3. apply the patch
#    The patch CREATES two files (the fstat forwards), so apply it to a tree
#    that does not have them yet: `patch` refuses to create a file that already
#    exists, and the "File systems/fbdev/compat_shim.c already exists" it prints
#    reads like a bad diff rather than a stale tree. Re-running over an
#    already-patched tree needs them removed first.
rm -f lib/direct/compat_shim.c systems/fbdev/compat_shim.c
patch -p1 < /path/to/rbpi4b/tools/build-directfb/directfb-full.diff

# 4. configure against the RX3 sysroot so everything references only
#    GLIBC_2.4/2.7 symbols (glibc 2.13 target).
#
#    The two -L flags are load-bearing, and they are not obvious. --sysroot does
#    NOT put the sysroot's libraries ahead of the cross toolchain's own libc:
#    GCC's built-in directories come after every -L, so
#
#      arm-linux-gnueabi-gcc --sysroot=$RX3 -print-file-name=libc.so
#      -> /usr/arm-linux-gnueabi/lib/libc.so        (the BUILD machine's glibc 2.36)
#
#    A link that resolves against that stamps every reference with the version it
#    found there: pthread_* and dlopen at GLIBC_2.34 (glibc folded libpthread and
#    libdl into libc at 2.34), fstat at GLIBC_2.33, fcntl at GLIBC_2.28. None of
#    those exist on glibc 2.13, so the build succeeds and the objects then fail
#    to load in the chroot. With the -L flags first, the same objects come out
#    stamped GLIBC_2.4 only, and the one reference the target cannot satisfy is
#    fstat — which the patch forwards (see the next section).
#
#    -Wl,-rpath-link only tells ld where to find a NEEDED library's own
#    dependencies. It does not change which libc the symbols resolve against;
#    only -L does that.
export RX3=/path/to/extracted/XDJRX3-rootfs
export CC=arm-linux-gnueabi-gcc
export CFLAGS="-march=armv5t -mfloat-abi=soft --sysroot=$RX3"
export LDFLAGS="--sysroot=$RX3 -L$RX3/lib -L$RX3/usr/lib -Wl,-rpath-link,$RX3/lib:$RX3/usr/lib"

./autogen.sh \
    --host=arm-linux-gnueabi \
    --prefix=/usr --libdir=/lib \
    --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
    --with-gfxdrivers=none \
    --disable-osx --disable-devmem

make -j"$(nproc)"

# 5. stage into work/dfb, then fix the sonames up (next section)
make install DESTDIR="$PWD/../work/dfb"
# make install names these libdirectfb-1.4.so.6.0.10 with SONAME .so.6; the
# soname fix-up below is what turns them into the names scripts/build-chroot.sh
# requires (it rejects anything but .so.0.0.0, at build-chroot.sh:64-69):
# -> work/dfb/lib/libdirectfb-1.4.so.0.0.0
#    work/dfb/lib/libdirect-1.4.so.0.0.0
#    work/dfb/lib/libfusion-1.4.so.0.0.0
#    work/dfb/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
#    work/dfb/lib/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so
#    work/dfb/lib/directfb-1.4-6/wm/libdirectfbwm_default.so
```

## The `fstat` forward (`compat_shim.c`)

The patch adds two files, and they exist because of exactly one symbol:

```c
int
fstat (int fd, struct stat *buf)
{
     return syscall (SYS_fstat, fd, buf);
}
```

**Why it is needed.** Our headers are glibc 2.36 — the RX3 rootfs ships no
`/usr/include` at all, so there is nothing else to compile against — while our
libc is the target's 2.13. glibc 2.33 introduced a real `fstat@GLIBC_2.33`; 2.13,
like every glibc before 2.33, exported only `__fxstat()`/`__fxstat64()`, which is
what *its* headers emit for an `fstat()` call. (The stock fbdev module on the
device references `__fxstat@GLIBC_2.4` — i.e. upstream never needed an `fstat`
symbol here either.) So nothing in the chroot can resolve our reference, and the
target's loader reports `undefined symbol: fstat` the first time the call is
reached. DirectFB calls `fstat()` from three files in three build units:

| Call site | Lands in | Covered by |
|---|---|---|
| `lib/direct/stream.c` | `libdirect-1.4.so` | `lib/direct/compat_shim.c` |
| `src/media/idirectfbfont.c` | `libdirectfb-1.4.so` | `libdirect`'s export, at load time |
| `systems/fbdev/vt.c` | `libdirectfb_fbdev.so` | `systems/fbdev/compat_shim.c` |

Hence the duplicate. `libdirect` fixes its own reference *and* exports the symbol
for `libdirectfb` to bind against; the fbdev module keeps its own copy so that a
`dlopen`'d module does not depend on load order to resolve. Keep the two in step.

**Why `syscall()` is byte-exact.** On ARM, glibc's `struct stat` *is* the kernel's
`struct stat`, and glibc's own `fstat()` does no translation: it passes the
caller's buffer straight to the kernel (a tail call into `fstatat()` with
`AT_EMPTY_PATH`; `sys_fstat` and `sys_fstatat` share `cp_new_stat()`). Disassembly
of glibc 2.36's arm `fstat` — the implementation that pairs with the headers we
compile against — is `mov r2, r1; … b fstatat@@GLIBC_2.33` with `r3 = 0x1000`.
So `syscall(SYS_fstat, fd, buf)` fills precisely the bytes glibc would have
filled, `st_rdev` included — and `st_rdev` is load-bearing here:
`systems/fbdev/vt.c` computes the console minor as
`(sbf.st_rdev & 0xFF) >> 5`. `syscall()` itself is `GLIBC_2.4`.

A *runtime* struct-layout experiment is not available on this target: any test
binary we can build carries a `__libc_start_main@GLIBC_2.34` reference from the
container's libc and cannot run under the chroot's glibc at all. Disassembly of
the implementation is the strongest evidence obtainable here.

**`fcntl()` needs no shim.** 2.13 exports `fcntl@GLIBC_2.4`, and our one call site
(`systems/fbdev/fbdev.c`, `F_SETFD`/`FD_CLOEXEC`) carries no `struct`, so it never
reaches for `fcntl64()`. Forwarding `fcntl` would add a varargs/`struct flock`
conversion hazard for no gain. `__fdelt_chk` is likewise absent from the tree —
nothing here is built with `_FORTIFY_SOURCE`.

## Soname fix-up

`make install` names the core libraries `libdirectfb-1.4.so.6.0.10` with SONAME
`libdirectfb-1.4.so.6` (libtool's `-version-info 6:0:0`). Nothing on the target
uses `.so.6`: `rbp`'s own `DT_NEEDED` entries and the stock DirectFB libraries
being replaced are all `.so.0`, and `scripts/build-chroot.sh` requires the
`.so.0.0.0` names outright. Three things have to change, not one — the earlier
version of this section rewrote only the modules' NEEDED entries, which leaves
`libdirectfb` looking for a `libdirect-1.4.so.6` that no longer exists, so the
module fails to load with no error beyond "cannot open shared object file":

```bash
cd "$PWD/../work/dfb"
# rename the file and retarget its SONAME
for base in libdirect libfusion libdirectfb; do
  patchelf --set-soname "$base-1.4.so.0" "lib/$base-1.4.so.6.0.10"
  mv "lib/$base-1.4.so.6.0.10" "lib/$base-1.4.so.0.0.0"
  rm -f "lib/$base-1.4.so.6" "lib/$base-1.4.so"
  ln -sf "$base-1.4.so.0.0.0" "lib/$base-1.4.so.0"
done
# then rewrite every NEEDED entry that pointed at the old sonames -- in the core
# libs as well as the modules
for f in lib/lib*-1.4.so.0.0.0 lib/directfb-1.4-6/*/*.so; do
  for base in libdirect libfusion libdirectfb; do
    arm-linux-gnueabi-readelf -d "$f" | grep -q "\[$base-1.4.so.6\]" || continue
    patchelf --replace-needed "$base-1.4.so.6" "$base-1.4.so.0" "$f"
  done
done
```

`patchelf` is architecture-agnostic and works on these ARM objects. The `readelf`
guard matters: `patchelf --replace-needed` fails on a library that does not have
that entry, and most modules need only some of the three.

## Verify

Every staged object must be soft-float and reference only `GLIBC_2.4`/`2.7`.
There are two distinct ways to fail that, and they need two different checks,
because neither one finds the other's bug:

```bash
# (a) VERSIONED BUT TOO NEW.  objdump -T is the only thing that shows these:
#     they carry a version, so a grep for a symbol name never matches them.
for f in work/dfb/lib/*.so* work/dfb/lib/directfb-1.4-6/*/*.so; do
  printf '%-46s %s\n' "$(basename "$f")" \
    "$(arm-linux-gnueabi-objdump -T "$f" 2>/dev/null \
       | grep -o 'GLIBC_[0-9.]*' | sort -uV | tr '\n' ' ')"
done

# (b) UNVERSIONED.  nm -D prints a versioned undefined symbol as name@GLIBC_x.y,
#     so grepping for a bare name matches only the unversioned references --
#     which is precisely the set that needs a forwarder.
for f in work/dfb/lib/*.so* work/dfb/lib/directfb-1.4-6/*/*.so; do
  bad=$(arm-linux-gnueabi-nm -D --undefined-only "$f" 2>/dev/null \
        | grep -E ' (fstat|__fdelt_chk|__xstat|stat|open|mmap|fcntl)$' || true)
  [ -n "$bad" ] && printf '  %-32s %s\n' "$(basename "$f")" "$(echo $bad)"
done
```

(a) must print `GLIBC_2.4` (and `GLIBC_2.7` for the three core libs), nothing
higher. (b) must print exactly one line — `libdirectfb-1.4.so.0.0.0 fstat` — and
that line is the designed case: `libdirect` exports the forward and the loader
binds it at load time.

Both loops are globs over a directory, and **a glob silently skips a file that
is not there**: the earlier version of this tree's verification listed the core
libs under names that did not exist yet at that point, so all three were skipped
without a word and `GLIBC_2.34` references survived three builds unnoticed.
Check the file count, not just the table.

The build harness runs all of this, with the counts and two
`fstat`-is-defined assertions that cannot pass while skipping an object:

```bash
docker build -t rbpi4b-dfb -f work/build-dfb.Dockerfile work/
docker run --rm -v "$PWD:/w" -w /w rbpi4b-dfb sh work/dfb-build.sh
```

`work/` is not committed (see [work/README.md](../../work/README.md)); the
commands above are the committed recipe, and the harness is the same recipe
wrapped in checks. It ends with steps 5–7 — the artefact list, the version
table with its count, and the `fstat` binding map.

Then confirm the soname fix-up took, because the loader reports a missing
library rather than a wrong soname, so this failure does not point at itself:

```bash
for f in work/dfb/lib/libdirect*-1.4.so.0.0.0 work/dfb/lib/directfb-1.4-6/*/*.so; do
  printf '%-30s %s\n' "$(basename "$f")" \
    "$(arm-linux-gnueabi-readelf -d "$f" | grep -E 'SONAME|NEEDED.*direct' \
       | tr -s ' ' | tr '\n' ' ')"
done
```

Every SONAME must be `.so.0`, and no NEEDED entry may still say `.so.6`.

## Gotchas

* DirectFB 1.4's dependency tracking is broken. After editing `fbdev.c`,
  always delete the object before rebuilding:

  ```bash
  rm -f systems/fbdev/fbdev.lo systems/fbdev/.libs/fbdev.o
  make -C systems/fbdev
  ```

* The modules must be soft-float and reference only `GLIBC_2.4`/`GLIBC_2.7`.
* **`--sysroot` does not put the sysroot's libraries first.** GCC's built-in
  directories come after every `-L`, so a link without an explicit
  `-L$RX3/lib -L$RX3/usr/lib` resolves against the *build machine's* glibc and
  stamps `pthread_*`/`dlopen` at `GLIBC_2.34`, `fstat` at `2.33`, `fcntl` at
  `2.28`. The build then succeeds and the objects refuse to load in the chroot,
  with an error that names none of this. `-Wl,-rpath-link` does not substitute
  for the `-L`s.
* **A version stamp is invisible to a name grep.** `nm -D` prints a versioned
  undefined symbol as `name@GLIBC_x.y`, so `grep fstat` matches only unversioned
  references; `objdump -T` shows the versions but says nothing about which
  references are unversioned. Run both checks — see *Verify*.
* `patch` refuses to create a file that already exists. The patch adds two, so a
  tree that already has them needs them removed before the patch is applied
  again; otherwise the failure reads like a corrupt diff.
* Do not set `layer-size` in `directfbrc` (historically caused a 2×/half-width
  bug); do not rely on `layer-rotate` (unimplemented).
* The present mode is read from `DFB_PRESENT` (`off`/`convert`/`letterbox`/
  `crop`/`scale`/`rotate`) in `system_initialize`, and the rotation direction from
  `DFB_ROTATE` (`left`/`right`/`180`/`off`), which still means `rotate` plus its
  angle — both for the launcher that has always set it and for anyone who
  learned it here. `rotate` on a fb that is not 32 bpp is refused with a log
  line, because its loops store 4-byte pixels.
* `scale` resamples the logical surface into the fb — aspect-fit and centred, or
  stretched with `DFB_PRESENT_FIT=stretch` — and its rectangle comes from one
  helper, `fbdev_present_fit()`, which the one-shot `PRESENT:` line also calls, so
  the drawn rectangle and the logged one cannot disagree. An exact fit falls
  through to the 1:1 body rather than resampling by 1.0. It also selects itself:
  when `DFB_PRESENT` is `off` (the default) and the real fb disagrees with the
  shim's logical geometry, `off` cannot present at all — a sheared image on a
  larger fb, no UI on a smaller one — so the driver upgrades to `scale`, or to
  `convert` when only the pixel format differs. `DFB_PRESENT_AUTO=0` disables
  that, and on a fb that matches the shim the branch is dead by construction.
  `DFB_PRESENT_PX_BUDGET` (default 2,600,000 weighted output pixels) and
  `DFB_PRESENT_SKIP` bound what the resample may cost per frame; both are in
  [06's present path](../../docs/06-display.md#a-mismatch-selects-the-rung-by-itself).
* The framebuffer path can come from `FB_DEV` as well: `dfb_config->fb_device`
  (the `fbdev=` option) still wins, then `FB_DEV`, then `FRAMEBUFFER`, then
  `/dev/fb0`. rbpi4b exports it from `rb.conf`'s `RB_FB_DEV`.
* `fluxcomp: command not found`, reported by `make` as `Error 127` — the flux
  package is missing. It is needed for a git build only, and the failure never
  says so; see step 2. (`Error 127` is the shell's "command not found", so a
  bare `Error 127` from a rule is always a missing tool, never a bad hunk.)

## Debug instrumentation policy

Bringing a display up on a new SoC is guesswork without ground truth, so this
diff carries `fopen("/tmp/dfbdig*.log", …)` diagnostics. They are kept to a
rule, because the same mechanism that makes them useful during bring-up makes
them a throughput bug if it stays on a hot path:

* **Kept — one-shot, at start-up or first use.** The geometry record
  (`PRESENT: mode=… real_fb=…`), the fb/layer/windowstack init traces, the
  `API: Create*EventBuffer` traces. These run once, cost a few kilobytes in
  `/tmp`, and are the only evidence available when a modeset or a driver probe
  fails on a target nobody has tried yet. `src/core/local_surface_pool.c`'s
  two-shot `localsurf.dump` (guarded by `static int dumps`) is in this class:
  two writes, then never again. The `scale` rung's two added lines are also in
  it, and each fires exactly once: the upgrade notice when `off` is replaced, and
  the frame average at the 300th present. The frame *timing* is the one thing
  here that touches a hot path, and it is paid only under `PRESENT_SCALE` — the
  `off` path, which is the verified configuration, reads no clock at all.
* **Removed — anything that runs per frame, per input event, or writes a
  whole surface.** In particular `primaryFlipRegion()` used to `fwrite` the
  ~6 MB triple buffer to `/tmp/rot_surface.dump` on *every flip*, and
  `wm/default/default.c` opened a log file per pointer event. Both are
  throughput bugs, not noise, and both are gone.

So: when adding instrumentation here, make it fire once. If you need a
per-frame trace, gate it behind an environment variable and say so in the
comment, rather than leaving it unconditional.

## Maintaining the diff

`directfb-full.diff` is regenerated from two trees, never hand-edited:

```bash
diff -up --label a/FILE --label b/FILE pristine/FILE patched/FILE
```

prefixed with `diff --git a/FILE b/FILE`. Hand-editing hunks invites arithmetic
errors in the `@@` headers — and GNU `patch` silently *refuses* a hunk whose
line count is internally consistent but whose match score its heuristic scores
low, which is easy to produce by hand and hard to notice. The diff carries no
`index` lines (they are meaningless for a fetched tree), so `patch -p1` is the
supported applier; `git apply` also takes it.

**New files need git's three-line form**, because `patch -p1` has to be told the
file is new rather than a modification of something it cannot find:

```
diff --git a/lib/direct/compat_shim.c b/lib/direct/compat_shim.c
new file mode 100644
--- /dev/null
+++ b/lib/direct/compat_shim.c
@@ -0,0 +1,50 @@
```

A hunk that names a file nothing creates is the trap this diff actually fell
into: `systems/fbdev/Makefile.am` listed `compat_shim.c` among its sources while
no creation hunk existed anywhere in the file, so `make` stopped at "No rule to
make target 'compat_shim.c'", which reads like a Makefile problem, not a patch
problem. Every `+++ b/…` line must be either an existing file or a file with a
creation hunk.

**Generate it on the host, under BSD `diff`.** GNU diffutils truncates the
trailing text of a `diff -p` hunk header — it prints
`direct_modules_register( DirectModuleDir` where BSD prints the whole line — so
regenerating inside the Linux container rewrites ~400 lines of header text that
nobody changed and buries the real change in review. The committed file's style
is BSD `diff`'s. The maintainer's script for this is the working tree's
`work/mkdiff.sh` (uncommitted, like the rest of `work/`): it holds the file list
in review order and emits both forms above.

After any change, verify against a pristine tree:

```bash
patch -p1 --dry-run < tools/build-directfb/directfb-full.diff
```

and check it reports as many files as the diff has headers — `patch` says
"checking file" for a creation too, so the count is what distinguishes them:

```bash
grep -c '^diff --git' tools/build-directfb/directfb-full.diff
```

