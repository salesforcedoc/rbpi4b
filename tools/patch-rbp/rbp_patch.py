#!/usr/bin/env python3
"""
rbp_patch.py - apply the interoperability patches to a stock
XDJ-RX3 `rbp` binary.

rblive4 does NOT ship Pioneer/AlphaTheta binaries. You must supply your own
extracted XDJ-RX3 `pdj/rbp` (see docs/04-firmware-assets.md) and run this
script against it.

The table below is the *complete* set of instruction-level changes that
turn the stock `rbp` from firmware v1.20 into the "rbp-audio" build used
by rblive4 (display, touch, controls, USB and audio all working).

Usage:
    python3 rbp_patch.py <stock-rbp> [-o rbp-audio] [--check]

    --check   only verify that the input already has / can take the patches;
              do not write an output file.

Notes:
    * rbp is a non-PIE ARM32 ELF.  virtual address (VA) = file offset + 0x8000.
    * The patcher is idempotent: running it on an already-patched binary is a
      no-op ("already patched") and never double-applies.
    * Every patch records the expected *stock* word so a wrong/foreign binary
      is rejected instead of corrupted.
"""
import argparse
import struct
import sys

LOAD_BIAS = 0x8000

# (VA, stock_word, patched_word, note)
# stock_word is verified before patching; patched_word is written.
PATCHES = [
    # --- base USB / browse / panel / startup fixes ("rbp-dbsfix" lineage) ---
    (0x020AF0, 0x0A000022, 0xE1000000, "dbsfix: short-circuit vendor startup check"),
    (0x020AFC, 0xEB0490F4, 0xE1000000, "dbsfix: short-circuit vendor startup call"),
    (0x020B08, 0x0A000013, 0xE1000000, "dbsfix: short-circuit vendor startup branch"),
    (0x020C54, 0x003FE218, 0x64656D2F, "dbsfix: '/media/usb1/sda1' mount path string (1/5)"),
    (0x020C58, 0x02435738, 0x752F6169, "dbsfix: mount path string (2/5)"),
    (0x020C5C, 0x024355E8, 0x2F316273, "dbsfix: mount path string (3/5)"),
    (0x020C60, 0x004E3054, 0x31616473, "dbsfix: mount path string (4/5)"),
    (0x020C64, 0x004D52C0, 0x004D5200, "dbsfix: mount path string (5/5)"),
    (0x159CE8, 0xE5C9300F, 0xE320F000, "dbsfix: NOP vendor status write"),
    (0x2BB87C, 0x012FFF1E, 0xE1A00000, "touch A/F: IReceptionForMAIN::startUp never early-returns"),
    (0x2D6CB0, 0x18BD8038, 0xE1A00000, "touch D: TouchPanel::openDevice never bails"),
    (0x31DDB0, 0x1A000014, 0xE1A00000, "touch E: startUp never bails on panel flag"),
    (0x31DDB8, 0x0A00000E, 0xEAFFFFFF, "touch B: startUp skips failing internal init"),
    (0x366530, 0x1A000004, 0xEA000004, "touch G: PanelComPeerLinux::postMessage deadlock fix"),

    # --- code cave used by the browse/USB routing patches ---
    (0x09B678, 0x00000000, 0xE3A01000, "usb: code cave (mov r1,#0)"),
    (0x09B67C, 0x00000000, 0xEA0B8337, "usb: code cave branch -> setBrowseMode(12)"),

    # --- key dispatch / throw-hardening ("rbp-fixthrow" lineage) ---
    (0x2CF7A0, 0xE3A00004, 0xE8BD8070, "fixthrow: UiTimer throw path -> pop/return"),
    (0x366CD0, 0xE3A01002, 0xE3001802, "fixthrow: socketpair() non-blocking"),
    (0x3779C0, 0xE92D4FF8, 0xE12FFF1E, "notimer: UiTimer callback -> immediate return (bx lr)"),
    (0x37AD90, 0x0A00014B, 0x0A000007, "fixthrow: KeyInput dispatch slot search (1/3)"),
    (0x37AD94, 0xE5900004, 0xEA000019, "fixthrow: KeyInput dispatch slot search (2/3)"),
    (0x37ADB0, 0x1A000012, 0xEA000012, "fixthrow: KeyInput dispatch slot search (3/3)"),
    (0x37B5D0, 0xAA0002AB, 0xE320F000, "fixthrow: IKeyInput bounds check NOP"),
    (0x37BC30, 0xAA0000CC, 0xEAFFFFF8, "fixthrow: FixedAddressArray::add throw -> skip"),
    (0x37BC38, 0xDA0000CA, 0xEAFFFFF6, "fixthrow: FixedAddressArray::add throw -> skip"),
    (0x37BF34, 0xDA00000B, 0xEA000052, "fixthrow: FixedAddressArray bounds -> no-throw path"),
    (0x37BF68, 0xE3A00004, 0xEAF47DC2, "fixthrow: return no-free-slot via cave"),
    (0x37C048, 0xAA00000D, 0xE320F000, "fixthrow: key-target array bound NOP"),
    (0x37C050, 0xDA00000B, 0xE320F000, "fixthrow: key-target array bound NOP"),
    (0x37C084, 0xE3A00004, 0xEAF47D7B, "fixthrow: return no-free-slot via cave"),
    (0x37C364, 0x0AFFFF46, 0xE320F000, "fixthrow: key-target removal guard NOP"),
    (0x37C710, 0xE58450A4, 0xE58480A4, "fixthrow: KeyManager pending-bitmask init"),

    # --- power-manager NULL-this guards (no Pioneer PM hardware on SC Live 4) ---
    (0x121D9C, 0xE92D41F0, 0xE3A00000, "pm: IPowerManager helper -> mov r0,#0"),
    (0x121DA0, 0xEB01955A, 0xE12FFF1E, "pm: IPowerManager helper -> bx lr"),
    (0x12203C, 0xE92D4038, 0xE3A00000, "pm: IPowerManager helper -> mov r0,#0"),
    (0x122040, 0xEB0194B2, 0xE12FFF1E, "pm: IPowerManager helper -> bx lr"),
    (0x122078, 0xE92D45F0, 0xE3A00000, "pm: IPowerManager helper -> mov r0,#0"),
    (0x12207C, 0xE24DD00C, 0xE12FFF1E, "pm: IPowerManager helper -> bx lr"),
    (0x2C6BF0, 0xE92D4038, 0xE12FFF1E, "pm: notifyPermissionChanged -> bx lr (NULL this)"),
    (0x2C7004, 0xE92D4070, 0xE12FFF1E, "pm: notifyPreparedToStandby -> bx lr (NULL this)"),
    (0x32E728, 0xE92D45F8, 0xE12FFF1E, "pm: USB/power notification helper -> bx lr"),
    (0x3871D0, 0xE1A00006, 0xE3A00000, "pm: notification helper -> mov r0,#0"),

    # --- display: middle scrolling waveform enabled unconditionally ---
    (0x24FC88, 0x1A000004, 0xE1A07004, "display: waveform gate (1/2)"),
    (0x24FC8C, 0xE5943070, 0xEA00007E, "display: waveform gate -> always render"),

    # --- touch: caution gate + playlist drag-scroll deadlock ---
    (0x2DC228, 0xE1A07000, 0xE3A07000, "touch: solveCoordToKey ignores caution id"),
    (0x2DC46C, 0x0A000008, 0xEA000008, "touch: touchOn bypasses caution check"),
    (0x363774, 0xE5943030, 0xEA000028, "touch: playlist drag -> always scroll"),
    (0x363794, 0xE5845030, 0xE320F000, "touch: drag-scroll counter NOP"),

    # --- panel / comm helpers ---
    (0x3664B4, 0xE0633000, 0xE3A03000, "panel: comm helper -> r3 = 0"),

    # --- audio: force RX3 device list on non-i.MX6 CPU ---
    (0x3C665C, 0x1A000054, 0xEA000054, "audio: ALSAAudioIODeviceType::scanForDevices force RX3 list"),

    # --- udev FIFO paths: /proc/udev_* -> /tmp/udev_* ---
    (0x4DEDE4, 0x6F72702F, 0x706D742F, "udev: '/proc/udev_usb1' -> '/tmp/udev_usb1' (1/4)"),
    (0x4DEDE8, 0x64752F63, 0x6564752F, "udev: usb1 path (2/4)"),
    (0x4DEDEC, 0x755F7665, 0x73755F76, "udev: usb1 path (3/4)"),
    (0x4DEDF0, 0x00316273, 0x00003162, "udev: usb1 path (4/4)"),
    (0x4DEDF4, 0x6F72702F, 0x706D742F, "udev: '/proc/udev_usb2' -> '/tmp/udev_usb2' (1/4)"),
    (0x4DEDF8, 0x64752F63, 0x6564752F, "udev: usb2 path (2/4)"),
    (0x4DEDFC, 0x755F7665, 0x73755F76, "udev: usb2 path (3/4)"),
    (0x4DEE00, 0x00326273, 0x00003262, "udev: usb2 path (4/4)"),
    (0x4E0E54, 0x6F72702F, 0x706D742F, "udev: '/proc/udev_usbctn1' -> '/tmp/udev_usbctn1' (1/5)"),
    (0x4E0E58, 0x64752F63, 0x6564752F, "udev: ctn1 path (2/5)"),
    (0x4E0E5C, 0x755F7665, 0x73755F76, "udev: ctn1 path (3/5)"),
    (0x4E0E60, 0x74636273, 0x6E746362, "udev: ctn1 path (4/5)"),
    (0x4E0E64, 0x0000316E, 0x00000031, "udev: ctn1 path (5/5)"),
    (0x4E0E68, 0x6F72702F, 0x706D742F, "udev: '/proc/udev_usbctn2' -> '/tmp/udev_usbctn2' (1/5)"),
    (0x4E0E6C, 0x64752F63, 0x6564752F, "udev: ctn2 path (2/5)"),
    (0x4E0E70, 0x755F7665, 0x73755F76, "udev: ctn2 path (3/5)"),
    (0x4E0E74, 0x74636273, 0x6E746362, "udev: ctn2 path (4/5)"),
    (0x4E0E78, 0x0000326E, 0x00000032, "udev: ctn2 path (5/5)"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rbp", help="stock rbp binary extracted from the firmware")
    ap.add_argument("-o", "--output", default="rbp-audio",
                    help="output path (default: rbp-audio)")
    ap.add_argument("--check", action="store_true",
                    help="verify only, do not write anything")
    args = ap.parse_args()

    with open(args.rbp, "rb") as fh:
        data = bytearray(fh.read())

    applied = already = 0
    for va, old, new, note in PATCHES:
        off = va - LOAD_BIAS
        if off < 0 or off + 4 > len(data):
            print(f"  !! VA 0x{va:06x} outside file", file=sys.stderr)
            return 2
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == new:
            already += 1
            continue
        if cur != old:
            print(f"  !! mismatch at VA 0x{va:06x}: expected 0x{old:08x}, "
                  f"found 0x{cur:08x}  ({note})", file=sys.stderr)
            print("     wrong rbp version or already modified by another tool",
                  file=sys.stderr)
            return 2
        struct.pack_into("<I", data, off, new)
        applied += 1

    print(f"[+] patches: {applied} applied, {already} already present, "
          f"{len(PATCHES)} total")
    if args.check:
        print("[i] --check only, no output written")
        return 0

    with open(args.output, "wb") as fh:
        fh.write(data)
    print(f"[+] wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
