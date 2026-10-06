#!/bin/bash
# build-chroot.sh — assemble the soft-float XDJ-RX3 chroot for the Raspberry Pi 4.
#
# Run on a Linux host (WSL is fine). Produces work/rbpi4b-pi4.tgz, which holds
# the complete deploy root, ready for the Pi's install.sh to untar into
# /opt/rblive4:
#
#   rbx3-run/    the soft-float chroot: RX3 rootfs + patched rbp + gui + shims
#                + the patched DirectFB 1.4.16 stack
#   rb.conf      the device constants, copied verbatim from scripts/device/
#
# Shipping rb.conf in the tarball means deploying is one scp and one untar, and
# it is impossible to end up with a tarball and a config that disagree.
#
# The tarball is path-independent: nothing inside the chroot refers to where the
# chroot itself lives on the host, so the deploy root can be moved by editing
# RB_DEPLOY_ROOT in rb.conf alone.
#
# Inputs (all overridable through the environment):
#
#   RX3       directory with the extracted firmware tree
#               $RX3/XDJRX3-rootfs   soft-float userland (from rootfs.cramfs)
#               $RX3/XDJRX3/gui      the ISO's gui/ dir
#               $RX3/XDJRX3-gui      gui.tar.gz unpacked (the gui partition)
#   ROOTFS    explicit rootfs dir                (= $RX3/XDJRX3-rootfs)
#   GUI       explicit gui root, holding the four asset dirs below
#                                                (= searched for in both of the
#                                                   two roots above)
#   RBPAUDIO  patched player, from tools/patch-rbp (= $RX3/rbp-audio)
#   DFB       DirectFB 1.4.16 staging, from tools/build-directfb
#              $DFB/lib/...         core libs + directfb-1.4-6 modules
#   SHIMS     where the built shims live         (= scripts/shims, built here)
#   CONF      rb.conf to embed                   (= scripts/device/rb.conf)
#   OUT       output directory                   (= work)
#
# Example:
#   RX3=$PWD/extracted DFB=$PWD/work/dfb scripts/build-chroot.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

RX3="${RX3:-$REPO/extracted}"
ROOTFS="${ROOTFS:-$RX3/XDJRX3-rootfs}"
RBPAUDIO="${RBPAUDIO:-$RX3/rbp-audio}"
DFB="${DFB:-$REPO/work/dfb}"
DFBLIB="${DFBLIB:-$DFB/lib}"
SHIMS="${SHIMS:-$HERE/shims}"
# Where the loadable modules live *inside the chroot*, and the single source of
# truth for it: it is used both to stage them (step 5) and to write the
# `module-dir` line into directfbrc (step 6), so the two cannot drift apart.
#
# It has to be stated at all because the built tree's compile-time MODULEDIR is
# /lib/directfb-1.4-6 (`--libdir=/lib` in the DirectFB build), while this rootfs's
# own convention -- where step 5 puts them -- is $libdir/directfb-1.4-6 under
# /usr/lib. DirectFB searches MODULEDIR whenever a module path is relative, which
# is always ("systems", "inputdrivers", "wm"), so a mismatch means nothing loads
# and the failure is a long way from its cause: dfbinfo reports "No system
# found!", and rbp -- which sets DirectFBSetOption("quiet") and so never sees
# DirectFB's own explanation -- segfaults in init_Resource() dereferencing the
# NULL IDirectFB* that a failed DirectFBCreate() handed back.
DFB_MODDIR="${DFB_MODDIR:-/usr/lib/directfb-1.4-6}"
CONF="${CONF:-$HERE/device/rb.conf}"
OUT="${OUT:-$REPO/work}"

# The four gui asset directories are split across two roots, and neither root is
# a gui tree on its own -- so they are resolved one directory at a time.
#
#   $RX3/XDJRX3/gui    fontdata + imagedata   the ISO's gui/ directory, which the
#                                            firmware's own `update` binary reads
#                                            (it opens gui/fontdata/decker.ttf and
#                                            gui/imagedata/png/)
#   $RX3/XDJRX3-gui    pset + system          gui.tar.gz unpacked.  This tarball
#                                            IS the gui partition: the RX3 formats
#                                            its gui UBIFS volume (mtd10) from it
#                                            (see the ISO's pdj/ubifs_funcs.sh).
#
# rbp opens ten paths under /root/gui and every one of them is under pset/ or
# system/ -- nine under pset/ (eight fontdata/*.bin, imagedata/imagedata.dat) and
# system/fontdata/sazanami-gothic.ttf -- so pset/ and system/ are required and
# fontdata/imagedata are staged when a root has them.
#
# An earlier version of this picked the first root that merely *existed* and then
# broke inside `cp` at the first subdirectory it lacked, naming a path that read
# like a corrupt tree rather than a split one.
if [ -n "${GUI:-}" ]; then
  GUI_ROOTS="$GUI"                       # explicit root: all four, or it fails below
else
  GUI_ROOTS="$RX3/XDJRX3/gui $RX3/XDJRX3-gui"
fi

GUI_DIRS=""
GUI_MISSING=""
for d in pset system fontdata imagedata; do
  src=""
  for root in $GUI_ROOTS; do
    if [ -d "$root/$d" ]; then src="$root/$d"; break; fi
  done
  if [ -n "$src" ]; then
    GUI_DIRS="$GUI_DIRS $src"
  else
    case "$d" in
      pset|system) GUI_MISSING="$GUI_MISSING $d" ;;
      *) echo "[gui] $d: in neither $GUI_ROOTS -- optional, rbp does not open it" ;;
    esac
  fi
done
if [ -n "$GUI_MISSING" ]; then
  echo "build-chroot: gui assets missing:$GUI_MISSING" >&2
  echo "  rbp opens /root/gui/pset/fontdata/*.bin," >&2
  echo "  /root/gui/pset/imagedata/imagedata.dat and" >&2
  echo "  /root/gui/system/fontdata/sazanami-gothic.ttf, so pset/ and system/" >&2
  echo "  are required.  Both come from the firmware's images/gui.tar.gz --" >&2
  echo "  the tarball the RX3 formats its gui partition from.  Searched:" >&2
  for root in $GUI_ROOTS; do echo "    $root" >&2; done
  echo "  extract it:  mkdir -p $RX3/XDJRX3-gui &&" >&2
  echo "               tar -C $RX3/XDJRX3-gui -xzf $RX3/XDJRX3/images/gui.tar.gz" >&2
  exit 1
fi

# --- check inputs ----------------------------------------------------------
missing=""
for f in "$ROOTFS" "$RBPAUDIO" \
         "$DFBLIB/libdirectfb-1.4.so.0.0.0" \
         "$DFBLIB/libdirect-1.4.so.0.0.0" \
         "$DFBLIB/libfusion-1.4.so.0.0.0" \
         "$DFBLIB/directfb-1.4-6/systems/libdirectfb_fbdev.so" \
         "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so" \
         "$DFBLIB/directfb-1.4-6/wm/libdirectfbwm_default.so"; do
  [ -e "$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
  echo "build-chroot: missing required assets:" >&2
  for f in $missing; do echo "  $f" >&2; done
  echo >&2
  echo "obtain the extracted assets (docs/04-firmware-assets.md), patch the" >&2
  echo "player if needed (tools/patch-rbp/) and build DirectFB" >&2
  echo "(tools/build-directfb/); set RX3= / DFB= as needed." >&2
  exit 1
fi

# --- build the shims from source if needed ---------------------------------
for so in knobshim.so audioshim.so fbshim.so; do
  if [ ! -f "$SHIMS/$so" ]; then
    echo "== building shims (make -C scripts/shims) =="
    make -C "$SHIMS" RX3="$ROOTFS"
    break
  fi
done
if [ ! -f "$SHIMS/knobshim.so" ] || [ ! -f "$SHIMS/audioshim.so" ] || \
   [ ! -f "$SHIMS/fbshim.so" ]; then
  echo "build-chroot: shims are missing in $SHIMS (build failed?)" >&2
  exit 1
fi

STAGE="$(mktemp -d /tmp/rbx3-stage.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

# The stage is the *deploy root*: the chroot is a subdirectory of it, next to
# rb.conf. install.sh untars this straight into /opt/rblive4.
CHROOT="$STAGE/rbx3-run"
mkdir -p "$CHROOT"

echo "== staging to $STAGE =="

# 1. base rootfs (preserve symlinks; exec bits are restored below).
#    The reader must not abort the pipeline (set -o pipefail is on) over a file
#    whose permissions deny a read -- e.g. var/log/wtmp, which a Windows/WSL ACL
#    can deny even to root; those two are recreated empty just below. GNU tar has
#    --ignore-failed-read for exactly that. BSD tar has no such option and
#    refuses to run at all ("Option --ignore-failed-read is not supported"),
#    which is how this script failed on macOS, so probe for it rather than
#    assume it: the Linux behaviour is then unchanged.
echo "[1/7] copying RX3 rootfs..."
if tar --version 2>/dev/null | head -1 | grep -q GNU; then
  TAR_IGNORE="--ignore-failed-read"
else
  TAR_IGNORE=""
fi
tar -C "$ROOTFS" $TAR_IGNORE -cf - . | tar -C "$CHROOT" -xf -
touch "$CHROOT/var/log/wtmp" "$CHROOT/var/log/lastlog" 2>/dev/null || true

# 2. gui assets -> /root/gui  (rbp reads /root/gui/pset/... and /root/gui/system/...;
#    GUI_DIRS is the per-directory resolution done above, so a directory staged
#    here may come from either root)
echo "[2/7] copying gui assets..."
mkdir -p "$CHROOT/root/gui"
for d in $GUI_DIRS; do
  cp -a "$d" "$CHROOT/root/gui/"
done
# Strip the build host's own metadata.  The extracted firmware on a macOS host
# carries Finder's .DS_Store inside the asset directories, and these are asset
# directories rbp reads -- copying a build host's bookkeeping into them is not
# something the deploy root should depend on.  Deleting them here, after the
# copy, keeps the staging independent of how the host unpacked the firmware.
find "$CHROOT/root/gui" -name '.DS_Store' -type f -delete 2>/dev/null || true
ls -d "$CHROOT/root/gui"/*/ | sed 's/^/    /'

# 3. patched player -> /root/pdj/rbp  (shared patches + the SC Live 4
#    getPcController() NULL-deref fix; see scripts/patch-rbp-nopc.py)
echo "[3/7] patching + installing rbp..."
mkdir -p "$CHROOT/root/pdj"
"${PYTHON:-python3}" "$HERE/patch-rbp-nopc.py" "$RBPAUDIO" -o "$CHROOT/root/pdj/rbp"

# 4. shims -> usr/lib (LD_PRELOAD names) and root/pdj
echo "[4/7] installing shims..."
cp "$SHIMS/fbshim.so"      "$CHROOT/usr/lib/fbshim.so"
cp "$SHIMS/audioshim.so"   "$CHROOT/usr/lib/audioshim.so"
cp "$SHIMS/knobshim.so"    "$CHROOT/usr/lib/knobshim.so"
cp "$SHIMS/fbshim.so"      "$CHROOT/root/pdj/fbshim.so"
cp "$SHIMS/audioshim.so"   "$CHROOT/root/pdj/audioshim.so"
cp "$SHIMS/knobshim.so"    "$CHROOT/root/pdj/knobshim.so"
if [ -f "$SHIMS/crashcatch.so" ]; then
  cp "$SHIMS/crashcatch.so" "$CHROOT/usr/lib/crashcatch.so"
fi

# 5. DirectFB 1.4.16 core (soft-float, .so.0 sonames) over the stock 1.4.0 core,
#    plus the patched fbdev + input/wm modules in the 1.4-6 module dir.
echo "[5/7] installing DirectFB 1.4.16 stack..."
cp "$DFBLIB/libdirectfb-1.4.so.0.0.0" "$CHROOT/usr/lib/libdirectfb-1.4.so.0.0.0"
cp "$DFBLIB/libdirect-1.4.so.0.0.0"   "$CHROOT/usr/lib/libdirect-1.4.so.0.0.0"
cp "$DFBLIB/libfusion-1.4.so.0.0.0"   "$CHROOT/usr/lib/libfusion-1.4.so.0.0.0"
ln -sfn libdirectfb-1.4.so.0.0.0 "$CHROOT/usr/lib/libdirectfb-1.4.so.0"
ln -sfn libdirect-1.4.so.0.0.0   "$CHROOT/usr/lib/libdirect-1.4.so.0"
ln -sfn libfusion-1.4.so.0.0.0   "$CHROOT/usr/lib/libfusion-1.4.so.0"

mkdir -p "$CHROOT$DFB_MODDIR/systems" \
         "$CHROOT$DFB_MODDIR/inputdrivers" \
         "$CHROOT$DFB_MODDIR/wm"
cp "$DFBLIB/directfb-1.4-6/systems/libdirectfb_fbdev.so" \
   "$CHROOT$DFB_MODDIR/systems/libdirectfb_fbdev.so"
cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so" \
   "$CHROOT$DFB_MODDIR/inputdrivers/libdirectfb_linux_input.so"
cp "$DFBLIB/directfb-1.4-6/wm/libdirectfbwm_default.so" \
   "$CHROOT$DFB_MODDIR/wm/libdirectfbwm_default.so"
# keyboard module: prefer the freshly built 1.4.16 one, fall back to the RX3 1.4.0 copy
if [ -f "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" \
     "$CHROOT$DFB_MODDIR/inputdrivers/libdirectfb_keyboard.so"
elif [ -f "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" \
     "$CHROOT$DFB_MODDIR/inputdrivers/libdirectfb_keyboard.so"
fi

# 6. exec bits + mtab + directfbrc + rb.conf (exec bits are lost in Windows
#    extraction; rbp runs via the explicit loader but /bin/sh, edb_streamd etc.
#    still need +x).
#    directfbrc is MANDATORY and stays as-is on the Pi: this build is
#    --with-gfxdrivers=none --disable-devmem, so `no-hardware` is the verified
#    configuration. It also keeps DirectFB away from any DRM/GPU path, which is
#    what we want -- the display is the fbdev driver's business, not DirectFB's.
echo "[6/7] fixing permissions + directfbrc + rb.conf..."
chmod 755 "$CHROOT/lib/ld-2.13.so" "$CHROOT/lib/ld-linux.so.3" 2>/dev/null || true
chmod -R 755 "$CHROOT/bin" "$CHROOT/sbin" "$CHROOT/usr/bin" "$CHROOT/usr/sbin" 2>/dev/null || true
chmod 755 "$CHROOT/root/pdj/rbp"
chmod 644 "$CHROOT/usr/lib/"*.so "$CHROOT/root/pdj/"*.so 2>/dev/null || true
ln -sfn /proc/mounts "$CHROOT/etc/mtab"
mkdir -p "$CHROOT/usr/etc"
# module-dir is not optional here: the built tree's compile-time MODULEDIR
# (/lib/directfb-1.4-6) is not where step 5 staged the modules, and a relative
# module path is always searched in MODULEDIR. Written from $DFB_MODDIR so it
# names exactly the directory step 5 used. See the definition above for what a
# mismatch costs.
printf 'no-hardware\nno-cursor\nsystem=fbdev\nfbdev=/dev/fb0\nmodule-dir=%s\n' \
       "$DFB_MODDIR" > "$CHROOT/usr/etc/directfbrc"
cp "$CHROOT/usr/etc/directfbrc" "$CHROOT/etc/directfbrc"

# rb.conf travels with the tree, at the deploy root next to the chroot.
if [ ! -f "$CONF" ]; then
  echo "build-chroot: rb.conf not found at $CONF (set CONF=)" >&2
  exit 1
fi
cp "$CONF" "$STAGE/rb.conf"

# Identity pointing calibration. rbp reads these and applies an affine to
# whatever coordinates the panel path hands it, so they must be the identity for
# our own pointer transform to be the only one in play. The values are the
# logical 1280x800 geometry, which is the same on the Pi's HDMI display as it
# was on the SC Live 4's panel -- hence unchanged.
mkdir -p "$CHROOT/root/settings"
printf '0\n0\n320\n200\n1280\n800\n' > "$CHROOT/root/settings/TouchCalib_User.dat"
cp "$CHROOT/root/settings/TouchCalib_User.dat" "$CHROOT/root/settings/TouchCalib_Factory.dat"

# 6b. Strip the build host's metadata from the whole staged tree, not just the
#     gui dirs. A macOS extraction leaves an AppleDouble "._name" companion
#     beside every file it unpacked, and these are not inert once deployed:
#     DirectFB walks each module directory and tries to dlopen every entry, so
#     every "._libdirectfb_*.so" yields an "Unable to dlopen" error at startup.
#     They also survive a re-deploy -- install.sh untars over the existing tree
#     and tar never deletes -- so a target that has ever received a tarball built
#     on a host carrying them keeps them forever, and only a rebuild fixes it.
#     Swept here, after everything is staged, so the count covers all seven steps.
APPLE_FILES=$(find "$CHROOT" -name '._*' -type f 2>/dev/null | wc -l)
find "$CHROOT" -name '._*' -type f -delete 2>/dev/null || true
if [ "$APPLE_FILES" -gt 0 ]; then
  echo "    stripped $APPLE_FILES AppleDouble file(s) from the staged tree"
fi

# 7. verification
echo "[7/7] verifying..."
# The one invariant that has already failed once, silently, with the symptom
# three layers away from the cause: every module directfbrc can name must exist
# where directfbrc says it does. Asserted rather than assumed, because a
# mistyped or stale module-dir produces a perfectly good tarball.
MOD_MISSING=0
for m in systems/libdirectfb_fbdev.so \
         inputdrivers/libdirectfb_linux_input.so \
         wm/libdirectfbwm_default.so; do
  if [ ! -f "$CHROOT$DFB_MODDIR/$m" ]; then
    echo "  !! MISSING: $DFB_MODDIR/$m" >&2
    MOD_MISSING=1
  fi
done
echo "--- modules in $DFB_MODDIR (as directfbrc names it) ---"
ls "$CHROOT$DFB_MODDIR"/systems "$CHROOT$DFB_MODDIR"/inputdrivers \
   "$CHROOT$DFB_MODDIR"/wm 2>/dev/null | sed 's/^/    /'
if [ "$MOD_MISSING" != 0 ]; then
  echo "build-chroot: modules are not where directfbrc says they are --" >&2
  echo "  every DirectFB module load would fail on the target." >&2
  exit 1
fi
if command -v readelf >/dev/null 2>&1; then
  echo "--- fbdev module NEEDED ---"
  readelf -d "$CHROOT$DFB_MODDIR/systems/libdirectfb_fbdev.so" | grep -E "NEEDED|SONAME" || true
  echo "--- core sonames ---"
  for s in libdirectfb-1.4.so.0.0.0 libdirect-1.4.so.0.0.0 libfusion-1.4.so.0.0.0; do
    readelf -d "$CHROOT/usr/lib/$s" | grep SONAME || true
  done
fi
if command -v file >/dev/null 2>&1; then
  echo "--- loader + busybox + rbp ---"
  file "$CHROOT/lib/ld-2.13.so" "$CHROOT/bin/busybox" "$CHROOT/root/pdj/rbp" | sed 's#.*: #  #'
fi

echo "== tar -> $OUT/rbpi4b-pi4.tgz =="
mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/rbpi4b-pi4.tgz" .
echo "== done: $(du -h "$OUT/rbpi4b-pi4.tgz" | cut -f1) =="
echo
echo "deploy on the Pi:"
echo "  scp $OUT/rbpi4b-pi4.tgz pi@<host>:/tmp/"
echo "  ssh pi@<host> 'sudo sh /path/to/install.sh /tmp/rbpi4b-pi4.tgz'"
