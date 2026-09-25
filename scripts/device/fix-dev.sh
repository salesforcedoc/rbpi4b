#!/bin/sh
# fix-dev.sh — prepare /data/rbx3-run on the SC Live 4 (bind mounts, device
# stubs, FIFOs). Adapted from PrimeBox; the device is identical in every way
# that matters (RK3288, same /dev tree).
#
# Run after every reboot, before starting rbp.

# --- bind mounts -----------------------------------------------------------
umount /data/rbx3-run/dev 2>/dev/null
rm -rf /data/rbx3-run/dev
mkdir -p /data/rbx3-run/dev
mount --bind /dev /data/rbx3-run/dev
mountpoint -q /data/rbx3-run/proc || mount --bind /proc /data/rbx3-run/proc
mountpoint -q /data/rbx3-run/sys  || mount --bind /sys  /data/rbx3-run/sys
mountpoint -q /data/rbx3-run/tmp  || mount --bind /tmp  /data/rbx3-run/tmp
echo "fb0: $(ls -la /data/rbx3-run/dev/fb0 | awk '{print $1}')"
echo "dev mounted: $(mountpoint -q /data/rbx3-run/dev && echo yes)"

# --- exec bits (Windows extraction stripped them; re-assert on the device) --
chmod 755 /data/rbx3-run/lib/ld-2.13.so /data/rbx3-run/lib/ld-linux.so.3 2>/dev/null
chmod -R 755 /data/rbx3-run/bin /data/rbx3-run/sbin \
             /data/rbx3-run/usr/bin /data/rbx3-run/usr/sbin 2>/dev/null

# --- FIFOs for polled SPI/hid devices (prevent 100% CPU busy-spin) ---------
for d in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0; do
  rm -f /data/rbx3-run/dev/$d
  mkfifo /data/rbx3-run/dev/$d 2>/dev/null || mknod /data/rbx3-run/dev/$d p
  chmod 666 /data/rbx3-run/dev/$d 2>/dev/null
done

# --- regular-file stubs for ioctl-only devices + polled GPIO ----------------
for d in printkdrv0 tsc2007_2-0048 gpiodrv; do
  rm -f /data/rbx3-run/dev/$d 2>/dev/null
  touch /data/rbx3-run/dev/$d
  chmod 666 /data/rbx3-run/dev/$d 2>/dev/null
done
# paudiog0 must NOT exist -> JUCE skips gadget-audio ioctls
rm -f /data/rbx3-run/dev/paudiog0
# Block /dev/mem: rbp maps i.MX6 phys regs; on Rockchip that is real hardware
chmod 000 /dev/mem /data/rbx3-run/dev/mem 2>/dev/null

# --- udev FIFOs for USB stick detection ------------------------------------
for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
  [ -p /tmp/$f ] || { rm -f /tmp/$f; mkfifo /tmp/$f; chmod 666 /tmp/$f; }
done

# --- mtab symlink for POSIX getmntent / vfs_getfsys -------------------------
ln -sf /proc/mounts /data/rbx3-run/etc/mtab 2>/dev/null

echo "stubs done"
