#!/bin/bash
# build-chroot.sh — assemble the soft-float XDJ-RX3 chroot for the SC Live 4.
#
# Run on a Linux host (WSL is fine). Produces work/rbx3-run.tgz, the complete
# /data/rbx3-run tree: RX3 rootfs + rbp-audio + gui + shims + the patched
# DirectFB 1.4.16 stack (for the 800x1280 portrait panel).
#
# Inputs (all overridable through the environment):
#
#   RX3       directory with the extracted firmware tree
#               $RX3/XDJRX3-rootfs   soft-float userland (from rootfs.cramfs)
#               $RX3/XDJRX3/gui      fontdata / imagedata / pset / system
#   ROOTFS    explicit rootfs dir                (= $RX3/XDJRX3-rootfs)
#   GUI       explicit gui dir                   (= $RX3/XDJRX3/gui)
#   RBPAUDIO  patched player, from tools/patch-rbp (= $RX3/rbp-audio)
#   DFB       DirectFB 1.4.16 staging, from tools/build-directfb
#              $DFB/lib/...         core libs + directfb-1.4-6 modules
#   SHIMS     where the built shims live         (= scripts/shims, built here)
#   OUT       output directory                   (= work)
#
# Example:
#   RX3=$PWD/extracted DFB=$PWD/work/dfb scripts/build-chroot.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

RX3="${RX3:-$REPO/extracted}"
ROOTFS="${ROOTFS:-$RX3/XDJRX3-rootfs}"
GUI="${GUI:-$RX3/XDJRX3/gui}"
RBPAUDIO="${RBPAUDIO:-$RX3/rbp-audio}"
DFB="${DFB:-$REPO/work/dfb}"
DFBLIB="${DFBLIB:-$DFB/lib}"
SHIMS="${SHIMS:-$HERE/shims}"
OUT="${OUT:-$REPO/work}"

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
for so in knobshim2.so audioshim.so fbshim-tsc.so; do
  if [ ! -f "$SHIMS/$so" ]; then
    echo "== building shims (make -C scripts/shims) =="
    make -C "$SHIMS" RX3="$ROOTFS"
    break
  fi
done
if [ ! -f "$SHIMS/knobshim2.so" ] || [ ! -f "$SHIMS/audioshim.so" ] || \
   [ ! -f "$SHIMS/fbshim-tsc.so" ]; then
  echo "build-chroot: shims are missing in $SHIMS (build failed?)" >&2
  exit 1
fi

STAGE="$(mktemp -d /tmp/rbx3-stage.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

echo "== staging to $STAGE =="

# 1. base rootfs (preserve symlinks; exec bits are restored below).
#    tar --ignore-failed-read skips odd files (e.g. var/log/wtmp) whose
#    Windows/WSL ACL can deny even root; recreate those as empty files.
echo "[1/7] copying RX3 rootfs..."
tar -C "$ROOTFS" --ignore-failed-read -cf - . | tar -C "$STAGE" -xf -
touch "$STAGE/var/log/wtmp" "$STAGE/var/log/lastlog" 2>/dev/null || true

# 2. gui assets -> /root/gui  (rbp reads /root/gui/pset/... and /root/gui/system/...)
echo "[2/7] copying gui assets..."
mkdir -p "$STAGE/root/gui"
for d in fontdata imagedata pset system; do
  cp -a "$GUI/$d" "$STAGE/root/gui/"
done

# 3. patched player -> /root/pdj/rbp  (shared patches + the SC Live 4
#    getPcController() NULL-deref fix; see scripts/patch-rbp-sclive4.py)
echo "[3/7] patching + installing rbp..."
mkdir -p "$STAGE/root/pdj"
"${PYTHON:-python3}" "$HERE/patch-rbp-sclive4.py" "$RBPAUDIO" -o "$STAGE/root/pdj/rbp"

# 4. shims -> usr/lib (LD_PRELOAD names) and root/pdj
echo "[4/7] installing shims..."
cp "$SHIMS/fbshim-tsc.so"  "$STAGE/usr/lib/fbshim.so"
cp "$SHIMS/audioshim.so"   "$STAGE/usr/lib/audioshim.so"
cp "$SHIMS/knobshim2.so"   "$STAGE/usr/lib/knobshim.so"
cp "$SHIMS/fbshim-tsc.so"  "$STAGE/root/pdj/fbshim.so"
cp "$SHIMS/audioshim.so"   "$STAGE/root/pdj/audioshim.so"
cp "$SHIMS/knobshim2.so"   "$STAGE/root/pdj/knobshim.so"
if [ -f "$SHIMS/crashcatch.so" ]; then
  cp "$SHIMS/crashcatch.so" "$STAGE/usr/lib/crashcatch.so"
fi

# 5. DirectFB 1.4.16 core (soft-float, .so.0 sonames) over the stock 1.4.0 core,
#    plus the patched fbdev + input/wm modules in the 1.4-6 module dir.
echo "[5/7] installing DirectFB 1.4.16 stack..."
cp "$DFBLIB/libdirectfb-1.4.so.0.0.0" "$STAGE/usr/lib/libdirectfb-1.4.so.0.0.0"
cp "$DFBLIB/libdirect-1.4.so.0.0.0"   "$STAGE/usr/lib/libdirect-1.4.so.0.0.0"
cp "$DFBLIB/libfusion-1.4.so.0.0.0"   "$STAGE/usr/lib/libfusion-1.4.so.0.0.0"
ln -sfn libdirectfb-1.4.so.0.0.0 "$STAGE/usr/lib/libdirectfb-1.4.so.0"
ln -sfn libdirect-1.4.so.0.0.0   "$STAGE/usr/lib/libdirect-1.4.so.0"
ln -sfn libfusion-1.4.so.0.0.0   "$STAGE/usr/lib/libfusion-1.4.so.0"

mkdir -p "$STAGE/usr/lib/directfb-1.4-6/systems" \
         "$STAGE/usr/lib/directfb-1.4-6/inputdrivers" \
         "$STAGE/usr/lib/directfb-1.4-6/wm"
cp "$DFBLIB/directfb-1.4-6/systems/libdirectfb_fbdev.so" \
   "$STAGE/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"
cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so" \
   "$STAGE/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so"
cp "$DFBLIB/directfb-1.4-6/wm/libdirectfbwm_default.so" \
   "$STAGE/usr/lib/directfb-1.4-6/wm/libdirectfbwm_default.so"
# keyboard module: prefer the freshly built 1.4.16 one, fall back to the RX3 1.4.0 copy
if [ -f "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$DFBLIB/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so" \
     "$STAGE/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so"
elif [ -f "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" ]; then
  cp "$ROOTFS/usr/lib/directfb-1.4-0/inputdrivers/libdirectfb_keyboard.so" \
     "$STAGE/usr/lib/directfb-1.4-6/inputdrivers/libdirectfb_keyboard.so"
fi

# 6. exec bits + mtab + directfbrc (lost in Windows extraction; rbp runs via
#    explicit loader but /bin/sh, edb_streamd etc. still need +x).
#    directfbrc is MANDATORY: no-hardware keeps DirectFB off the Mali/dri path
#    (an oops there reboots the device, which boots with panic_on_oops=1).
echo "[6/7] fixing permissions + directfbrc..."
chmod 755 "$STAGE/lib/ld-2.13.so" "$STAGE/lib/ld-linux.so.3" 2>/dev/null || true
chmod -R 755 "$STAGE/bin" "$STAGE/sbin" "$STAGE/usr/bin" "$STAGE/usr/sbin" 2>/dev/null || true
chmod 755 "$STAGE/root/pdj/rbp"
chmod 644 "$STAGE/usr/lib/"*.so "$STAGE/root/pdj/"*.so 2>/dev/null || true
ln -sfn /proc/mounts "$STAGE/etc/mtab"
mkdir -p "$STAGE/usr/etc"
printf 'no-hardware\nno-cursor\nsystem=fbdev\nfbdev=/dev/fb0\n' > "$STAGE/usr/etc/directfbrc"
cp "$STAGE/usr/etc/directfbrc" "$STAGE/etc/directfbrc"

# identity touch calibration (rbp reads root/settings/TouchCalib_{User,Factory}.dat
# and needs these exact values or touch coords are wrong / rejected)
mkdir -p "$STAGE/root/settings"
printf '0\n0\n320\n200\n1280\n800\n' > "$STAGE/root/settings/TouchCalib_User.dat"
cp "$STAGE/root/settings/TouchCalib_User.dat" "$STAGE/root/settings/TouchCalib_Factory.dat"

# 7. verification
echo "[7/7] verifying..."
if command -v readelf >/dev/null 2>&1; then
  echo "--- fbdev module NEEDED ---"
  readelf -d "$STAGE/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so" | grep -E "NEEDED|SONAME" || true
  echo "--- core sonames ---"
  for s in libdirectfb-1.4.so.0.0.0 libdirect-1.4.so.0.0.0 libfusion-1.4.so.0.0.0; do
    readelf -d "$STAGE/usr/lib/$s" | grep SONAME || true
  done
fi
if command -v file >/dev/null 2>&1; then
  echo "--- loader + busybox + rbp ---"
  file "$STAGE/lib/ld-2.13.so" "$STAGE/bin/busybox" "$STAGE/root/pdj/rbp" | sed 's#.*: #  #'
fi

echo "== tar -> $OUT/rbx3-run.tgz =="
mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/rbx3-run.tgz" .
echo "== done: $(du -h "$OUT/rbx3-run.tgz" | cut -f1) =="
