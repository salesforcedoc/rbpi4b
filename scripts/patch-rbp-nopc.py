#!/usr/bin/env python3
"""Additional interoperability patch for the XDJ-RX3 `rbp` binary.

The base patch set turns the stock XDJ-RX3 v1.20 `rbp` into `rbp-audio`
(md5 3706c68f7242779d46afa09f35a39acf). On the SC Live 4 one additional,
timing-dependent crash fires:

  JuceTimer -> NetworkMonitor::timerCallback() -> IUiObjManager::getPcController()
  dereferences a NULL PC-controller pointer at [NULL+0x9c]  (~1s after start,
  before fb0 opens).

Fix: make getPcController() return NULL immediately; the caller's NULL check
then skips the missing object.

    VA 0x31DF64:  e30636b0 -> e3a00000   mov r0, #0
    VA 0x31DF68:  e3403268 -> e12fff1e   bx  lr

Code/data at VA maps to file_offset = VA - 0x8000 (non-PIE ARM32 ELF), so the
file offsets are 0x315F64 / 0x315F68.

Usage:
    python3 patch-rbp-sclive4.py rbp-audio -o rbp-audio-sclive4
    python3 patch-rbp-sclive4.py rbp-audio --check
"""

import argparse
import struct
import sys

# (file_offset, stock_word, patched_word, note)
PATCHES = [
    (0x315F64, 0xE30636B0, 0xE3A00000, "getPcController(): mov r0,#0 (return NULL)"),
    (0x315F68, 0xE3403268, 0xE12FFF1E, "getPcController(): bx lr"),
]


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

    if args.check:
        for off, stock, patched, note in PATCHES:
            cur = struct.unpack_from("<I", data, off)[0]
            status = "OK (patched)" if cur == patched else "MISSING (stock)" if cur == stock else "UNKNOWN"
            print(f"  {off:#08x}: {status}  ({note})")
        return 0

    print(f"patching {args.input} ({len(data)} bytes)")
    apply(data)
    out = args.output or "rbp-audio-sclive4"
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
