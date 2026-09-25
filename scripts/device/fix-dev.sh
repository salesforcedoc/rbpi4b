#!/bin/sh
# fix-dev.sh — bind mounts and device stubs the chroot needs, for the Pi 4.
#
# Run as root after every reboot, before starting rbp. start-rb.sh does both.
#
# rbp is the XDJ-RX3 player, so it opens its panel MCU, its power-manager, its
# touchscreen and an i.MX6 GPIO block *by name*, at paths compiled into the
# binary. None of them exist on a Pi, and rbp does not degrade gracefully when
# they are missing. Each one is therefore created as the cheapest thing that
# behaves correctly under the access pattern rbp uses — which is not always the
# obvious choice, hence the comment on each group.
#
# Paths come from rb.conf (RB_CHROOT and friends). See docs/13-raspberrypi4.md.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf
rb_require_root
rb_require_chroot

echo "fix-dev: chroot = $RB_CHROOT"

# --- kernel modules that were built into the RX3 kernel ----------------------
#
# On the XDJ-RX3 these are all compiled in, so their absence is a Pi-only
# failure mode. snd-seq is the important one: without /dev/snd/seq the sequencer
# cannot be opened, rbp starts normally and simply never receives a control
# event — the classic silent "controls do nothing" report.
modprobe snd-seq 2>/dev/null
if [ ! -e /dev/snd/seq ]; then
    echo "fix-dev: WARNING: /dev/snd/seq is still missing after modprobe snd-seq." >&2
    echo "  rbp will start but no controller input will reach it." >&2
    echo "  Check CONFIG_SND_SEQUENCER in the kernel and that /dev/snd exists." >&2
fi

# vfat/exfat mount helpers. Also built in on the RX3; on the Pi they may be
# modules, and a missing nls_cp437 turns a media mount into an opaque EINVAL.
modprobe vfat exfat nls_cp437 nls_iso8859-1 nls_utf8 2>/dev/null

# --- bind mounts ------------------------------------------------------------
#
# /tmp matters as much as the rest: rbp and the shims exchange runtime state
# through files and FIFOs in it (see the USB section below), and both sides must
# see the same /tmp — one inside the chroot, one outside.
umount "$RB_CHROOT/dev" 2>/dev/null
rm -rf "$RB_CHROOT/dev"
mkdir -p "$RB_CHROOT/dev"
mount --bind /dev "$RB_CHROOT/dev"
for d in proc sys tmp; do
    mountpoint -q "$RB_CHROOT/$d" || mount --bind "/$d" "$RB_CHROOT/$d"
done

echo "fix-dev: dev mounted: $(mountpoint -q "$RB_CHROOT/dev" && echo yes || echo NO)"
if [ -e "$RB_CHROOT/dev/fb0" ]; then
    echo "fix-dev: fb0: $(ls -l "$RB_CHROOT/dev/fb0" | awk '{print $1, $5, $6}')"
else
    echo "fix-dev: WARNING: $RB_CHROOT/dev/fb0 is missing (no display)." >&2
fi

# --- exec bits --------------------------------------------------------------
#
# The tree is extracted from a tarball on a vfat-formatted SD card at worst, and
# in every case the RX3 rootfs was assembled from a read-only cramfs image, so
# re-assert the bits rbp needs rather than trusting what survived.
chmod 755 "$RB_CHROOT/lib/ld-2.13.so" "$RB_CHROOT/lib/ld-linux.so.3" 2>/dev/null
chmod -R 755 "$RB_CHROOT/bin" "$RB_CHROOT/sbin" \
             "$RB_CHROOT/usr/bin" "$RB_CHROOT/usr/sbin" 2>/dev/null

# NOTE: /dev inside the chroot is a bind mount of the host's /dev, so everything
# created from here down is really being created in the HOST's /dev. That is
# intended — the chroot sees the same nodes either way — but it means these
# stubs are live on the Pi itself, not sealed inside the chroot. /dev is a
# tmpfs, so they are gone after a reboot, which is why this script recreates
# them every time rather than installing them once.

# --- FIFOs for polled SPI / hid devices -------------------------------------
#
# rbp polls these at full speed. A regular file makes the poll return
# immediately and forever, which pegs a core — under rbp's RT priority 98 that
# starves the render and audio threads. A FIFO with no writer blocks in poll()
# instead, which is what rbp actually wants.
for d in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0; do
    rm -f "$RB_CHROOT/dev/$d"
    mkfifo "$RB_CHROOT/dev/$d" 2>/dev/null || mknod "$RB_CHROOT/dev/$d" p
    chmod 666 "$RB_CHROOT/dev/$d" 2>/dev/null
done

# --- regular-file stubs for ioctl-only devices ------------------------------
#
# Same reasoning as the FIFOs but the opposite answer: these are opened and
# ioctl'd, not read, so a regular file is fine and a FIFO would hang the ioctl.
# tsc2007_2-0048 is the touchscreen node rbp expects — on the Pi the real
# pointer arrives through fbshim.so instead, so the node only has to exist.
for d in printkdrv0 tsc2007_2-0048 gpiodrv; do
    rm -f "$RB_CHROOT/dev/$d" 2>/dev/null
    touch "$RB_CHROOT/dev/$d"
    chmod 666 "$RB_CHROOT/dev/$d" 2>/dev/null
done

# paudiog0 must NOT exist: its presence makes JUCE take the USB-gadget-audio
# path and issue gadget ioctls this kernel has no idea about.
rm -f "$RB_CHROOT/dev/paudiog0"

# --- block /dev/mem ---------------------------------------------------------
#
# rbp maps what it believes are i.MX6 physical register windows. The addresses
# are wrong for a Pi but not harmless — the Pi's peripheral base is 0xFE000000,
# so an unlucky mapping can reach real hardware. audioshim.so also interposes the
# mmap and redirects it to anonymous memory; this is the second of the two
# defences, and the cheaper one to verify.
chmod 000 /dev/mem "$RB_CHROOT/dev/mem" 2>/dev/null

# --- udev FIFOs for USB stick detection -------------------------------------
#
# rbp watches /tmp/udev_usb1 for mount/umount text; usb-watch.sh writes to it.
# These are host-side paths (see the bind-mount note above) — rbp reaches them
# through the chroot's /tmp, which is the same directory.
for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
    [ -p "/tmp/$f" ] || { rm -f "/tmp/$f"; mkfifo "/tmp/$f"; chmod 666 "/tmp/$f"; }
done

# --- mtab symlink -----------------------------------------------------------
#
# getmntent()/vfs_getfsys go through /etc/mtab, and glibc 2.13 reads it as a
# regular file. Point it at /proc/mounts so the chroot sees the real mount table
# (including the media bind) instead of the stale copy in the tarball.
ln -sf /proc/mounts "$RB_CHROOT/etc/mtab" 2>/dev/null

echo "fix-dev: stubs done"
