#!/usr/bin/env python3
"""Additional interoperability patch for the XDJ-RX3 `rbp` binary.

The base patch set turns the stock XDJ-RX3 v1.20 `rbp` into `rbp-audio`
(md5 3706c68f7242779d46afa09f35a39acf). On any host with no Pioneer PC-controller
link -- the SC Live 4, and now the Pi 4 -- one additional, timing-dependent crash
fires:

  JuceTimer -> NetworkMonitor::timerCallback() -> IUiObjManager::getPcController()
  dereferences a NULL UiObjManager pointer at [NULL+0x9c]  (~1s after start,
  before fb0 opens).

The UiObjManager global (VA 0x026867C0, .bss) is not published until the UI is
built, but NetworkManager::initialize() arms a 1000 ms juce::Timer from `main`,
so the timer's FIRST tick can beat it. Address 0x9c is unmapped on this target:
the ELF's first PT_LOAD starts at 0x8000, not 0.

    three consecutive getters, 5 words each (VA 0x31DF3C, 0x31DF50, 0x31DF64):
        movw r3, #0x66b0 / movt r3, #0x268   ; -> 0x026866B0
        ldr  r3, [r3, #0x110]                ; r3 = UiObjManager (may be NULL)
        ldr  r0, [r3, #0x94/0x98/0x9c]        ; <- faults at 0x9c when r3 == 0
        bx   lr

WHY THE OBVIOUS FIX WAS WRONG. An earlier revision of this file replaced
getPcController with `mov r0, #0; bx lr` -- a permanent stub returning NULL. That
does stop the crash, and it is byte-identical to the `mov r0,#0; bx lr` thunks
Pioneer ships for its own unimplemented functions, which is exactly why the
damage was invisible. But the stub did not merely defuse a crash:

  NetworkMonitor::timerCallback() @0x392160 evaluates the Pro DJ Link gate
  `ui::PcController::isUsbBConnected()` (PcController+0x72) EVERY SECOND, and it
  reads it THROUGH getPcController(). A stubbed getPcController makes the gate
  structurally unreadable: rbp can never see it, never calls
  operateConnectNetwork, and never calls checkNetworkConnectionChange -- so the
  NetworkMonitor's own IP field (+0x1c) is never filled, which in turn makes the
  out-of-band NETALIAS_CONNECT route refuse forever ('no-ip'). The stub disabled
  Pro DJ Link on BOTH routes, silently, on every host that carried it.

WHAT THIS PATCH DOES NOW. The crash is inside the getter, not at its call site:
timerCallback already handles a NULL return correctly (`ldrbne r3,[r0,#114]` /
`moveq r3,r0` @0x3921a8). So each getter only has to RETURN NULL instead of
FAULTING, and that fits inside the stock 5-word slot with room to spare -- no
branch, no shared helper, no byte moved:

    ldr   r3, [pc, #N]     ; the existing 0x026866B0 pool word @0x31DF9C
    ldr   r3, [r3, #0x110]; r3 = UiObjManager (may be 0)
    movs  r0, r3          ; r0 = r3, and set Z from it
    ldrne r0, [r3, #off]  ; r0 = *(r3 + off) -- only when r3 != 0
    bx    lr              ; NULL -> r0 stays 0, caller's existing check sees it

`movs` is the flag-setter that replaces the dropped `cmp`; AAPCS makes flags
caller-saved across a `bl`, so setting them here is free. Every function's ENTRY
ADDRESS IS UNCHANGED (all 18 `bl getPcController` sites, and any address-taken
reference, still land correctly), each returns exactly the stock value when the
UiObjManager exists, and each returns NULL instead of faulting during the
startup window. rbp then takes the same "no PC controller attached" path it
takes on real hardware with nothing plugged into USB-B.

All three getters are patched, not just getPcController: getNet and getSettings
dereference the same NULL UiObjManager at +0x94/+0x98 and are only lucky that no
1-second timer calls them before the UI is up. Guarding them is a no-op when the
UI exists and removes the same latent fault when it does not.

Code/data at VA maps to file_offset = VA - 0x8000 (non-PIE ARM32 ELF).

There is nothing Denon- or Pi-specific about this: it is the "no PC controller
exists yet" fix, which is why the file is named for what it does rather than for
a device.

Usage:
    python3 patch-rbp-nopc.py rbp-audio -o rbp-nopc
    python3 patch-rbp-nopc.py rbp-audio --check
"""

import argparse
import struct
import sys

# (file_offset, stock_word, patched_word, note)
#
# Words are from `arm-linux-gnueabi-as` on the equivalent assembly, not
# hand-encoded. The pc-relative offsets are the load-bearing part and were
# checked against the target's own pool word: each `ldr r3,[pc,#N]` must land on
# 0x31DF9C, which holds 0x026866B0 (verified present and unmodified in both stock
# and rbp-audio). ARM reads pc as instruction+8, so N = 0x31DF9C - (entry + 8):
#   0x31DF3C -> 88 (0x58)   0x31DF50 -> 68 (0x44)   0x31DF64 -> 48 (0x30)
PATCHES = [
    # -- ui::IUiObjManager::getNet() @0x31DF3C, member +0x94 -------------------
    (0x315F3C, 0xE30636B0, 0xE59F3058, "getNet(): ldr r3,[pc,#88] -> 0x026866B0"),
    (0x315F40, 0xE3403268, 0xE5933110, "getNet(): r3 = [r3,#0x110] UiObjManager"),
    (0x315F44, 0xE5933110, 0xE1B00003, "getNet(): movs r0,r3 (Z = NULL test)"),
    (0x315F48, 0xE5930094, 0x15930094, "getNet(): ldrne r0,[r3,#0x94]"),
    (0x315F4C, 0xE12FFF1E, 0xE12FFF1E, "getNet(): bx lr"),
    # -- ui::IUiObjManager::getSettings() @0x31DF50, member +0x98 -------------
    (0x315F50, 0xE30636B0, 0xE59F3044, "getSettings(): ldr r3,[pc,#68] -> 0x026866B0"),
    (0x315F54, 0xE3403268, 0xE5933110, "getSettings(): r3 = [r3,#0x110] UiObjManager"),
    (0x315F58, 0xE5933110, 0xE1B00003, "getSettings(): movs r0,r3 (Z = NULL test)"),
    (0x315F5C, 0xE5930098, 0x15930098, "getSettings(): ldrne r0,[r3,#0x98]"),
    (0x315F60, 0xE12FFF1E, 0xE12FFF1E, "getSettings(): bx lr"),
    # -- ui::IUiObjManager::getPcController() @0x31DF64, member +0x9c ---------
    (0x315F64, 0xE30636B0, 0xE59F3030, "getPcController(): ldr r3,[pc,#48] -> 0x026866B0"),
    (0x315F68, 0xE3403268, 0xE5933110, "getPcController(): r3 = [r3,#0x110] UiObjManager"),
    (0x315F6C, 0xE5933110, 0xE1B00003, "getPcController(): movs r0,r3 (Z = NULL test)"),
    (0x315F70, 0xE593009C, 0x1593009C, "getPcController(): ldrne r0,[r3,#0x9c]"),
    (0x315F74, 0xE12FFF1E, 0xE12FFF1E, "getPcController(): bx lr"),
]


def check_size(data):
    """A truncated or foreign file would otherwise die in struct.unpack_from
    with a bare traceback, which reads as a bug in the patcher rather than
    'wrong input file'."""
    need = max(off for off, _, _, _ in PATCHES) + 4
    if len(data) < need:
        raise SystemExit(
            f"  {len(data)} bytes is too short: the patch table needs at least "
            f"{need}. This is not the XDJ-RX3 rbp binary."
        )


def apply(data):
    for off, stock, patched, note in PATCHES:
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == patched:
            print(f"  {off:#08x}: already patched ({note})")
        elif cur == stock:
            struct.pack_into("<I", data, off, patched)
            print(f"  {off:#08x}: {stock:#010x} -> {patched:#010x}  ({note})")
        else:
            raise SystemExit(
                f"  {off:#08x}: expected {stock:#010x} (stock) or {patched:#010x} "
                f"(patched), found {cur:#010x} — wrong/foreign binary?"
            )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    check_size(data)

    if args.check:
        for off, stock, patched, note in PATCHES:
            cur = struct.unpack_from("<I", data, off)[0]
            status = "OK (patched)" if cur == patched else "MISSING (stock)" if cur == stock else "UNKNOWN"
            print(f"  {off:#08x}: {status}  ({note})")
        return 0

    print(f"patching {args.input} ({len(data)} bytes)")
    apply(data)
    out = args.output or "rbp-nopc"
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
