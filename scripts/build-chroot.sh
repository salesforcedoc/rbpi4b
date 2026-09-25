#!/bin/bash
# build-chroot.sh — assemble the soft-float XDJ-RX3 chroot for the Raspberry Pi 4.
#
# Run on a Linux host (WSL is fine). Produces work/rblive4-pi4.tgz, which holds
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
#               $RX3/XDJRX3/gui      fontdata / imagedata / pset / system
#               (the gui assets are also accepted as a sibling
#                $RX3/XDJRX3-gui, which is how the ISO unpacks here)
#   ROOTFS    explicit rootfs dir                (= $RX3/XDJRX3-rootfs)
#   GUI       explicit gui dir                   (= first of the two above that exists)
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
CONF="${CONF:-$HERE/device/rb.conf}"
OUT="${OUT:-$REPO/work}"

# The gui assets live in a sibling directory when the ISO is unpacked here
# ($RX3/XDJRX3-gui) but nested under the firmware tree in some extractions.
if [ -z "${GUI:-}" ]; then
  for candidate in "$RX3/XDJRX3/gui" "$RX3/XDJRX3-gui"; do
    if [ -d "$candidate" ]; then GUI="$candidate"; break; fi
  done
  GUI="${GUI:-$RX3/XDJRX3/gui}"   # keep the original default for the error message
fi

# --- check inputs ----------------------------------------------------------
missing=""
for f in "$ROOTFS" "$GUI" "$RBPAUDIO" \
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
#    tar --ignore-failed-read skips odd files (e.g. var/log/wtmp) whose
#    Windows/WSL ACL can deny even root; recreate those as empty files.
echo "[1/7] copying RX3 rootfs..."
tar -C "$ROOTFS" --ignore-failed-read -cf - . | tar -C "$CHROOT" -xf -
touch "$CHROOT/var/log/wtmp" "$CHROOT/var/log/lastlog" 2>/dev/null || true

# 2. gui assets -> /root/gui  (rbp reads /root/gui/pset/... and /root/gui/system/...)
echo "[2/7] copying gui assets..."
mkdir -p "$CHROOT/root/gui"
for d in fontdata imagedata pset system; do
  cp -a "$GUI/$d" "$CHROOT/root/gui/"
done

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

mkdir -p "$CHROOT/usr/lib/directfb-1.4-6/systems" \
         "$CHROOT/usr/lib/directfb-1.4-6/inputdrivers" \
         "$CHROOT/usr/lib/directfb-1.4-6/wm"
cp "$DFBLIB/directfb-1.4-6/systems/libdirectfb_fbdev.so" \
   "$CHROOT/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"
cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so" \
   "$CHROOT/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so"
cp "$DFBLIB/directfb-1.4-6/wm/libdirectfbwm_default.so" \
   "$CHROOT/usr/lib/directfb-1.4-6/wm/libdirectfbwm_default.so"
# keyboard module: prefer the freshly built 1.4.16 one, fall back to the RX3 1.4.0 copy
if [ -f "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" \
     "$CHROOT/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so"
elif [ -f "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" \
     "$CHROOT/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so"
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
printf 'no-hardware\nno-cursor\nsystem=fbdev\nfbdev=/dev/fb0\n' > "$CHROOT/usr/etc/directfbrc"
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

# 7. verification
echo "[7/7] verifying..."
if command -v readelf >/dev/null 2>&1; then
  echo "--- fbdev module NEEDED ---"
  readelf -d "$CHROOT/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so" | grep -E "NEEDED|SONAME" || true
  echo "--- core sonames ---"
  for s in libdirectfb-1.4.so.0.0.0 libdirect-1.4.so.0.0.0 libfusion-1.4.so.0.0.0; do
    readelf -d "$CHROOT/usr/lib/$s" | grep SONAME || true
  done
fi
if command -v file >/dev/null 2>&1; then
  echo "--- loader + busybox + rbp ---"
  file "$CHROOT/lib/ld-2.13.so" "$CHROOT/bin/busybox" "$CHROOT/root/pdj/rbp" | sed 's#.*: #  #'
fi

echo "== tar -> $OUT/rblive4-pi4.tgz =="
mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/rblive4-pi4.tgz" .
echo "== done: $(du -h "$OUT/rblive4-pi4.tgz" | cut -f1) =="
echo
echo "deploy on the Pi:"
echo "  scp $OUT/rblive4-pi4.tgz pi@<host>:/tmp/"
echo "  ssh pi@<host> 'sudo sh /path/to/install.sh /tmp/rblive4-pi4.tgz'"
