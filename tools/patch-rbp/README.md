# tools/patch-rbp

## `rbp_patch.py`

Applies the complete set of interoperability patches to a stock
XDJ-RX3 v1.20 `rbp`.

```bash
# verify a stock binary
python3 rbp_patch.py /path/to/stock/rbp --check

# produce the patched player
python3 rbp_patch.py /path/to/stock/rbp -o rbp-audio
```

* Idempotent — safe to re-run on an already patched binary.
* Validates the expected stock instruction at every address; a wrong or
  foreign binary is rejected rather than corrupted.
* Reproduces the reference `rbp-audio` **byte-for-byte**
  (md5 `3706c68f7242779d46afa09f35a39acf`).

Code/data at `VA` maps to `file_offset = VA − 0x8000` (non-PIE ARM32 ELF).

## Patch groups

The full table lives in [`PATCHES.md`](PATCHES.md). In summary, the patches
exist because the stock RX3 binary assumes hardware the SC Live 4 does not have:

| Group | Why |
|---|---|
| **USB / mount / panel / startup** | SC Live 4 has none of the i.MX6 panel MCUs; several startup calls must be short-circuited, and `/proc/udev_*` paths become `/tmp/udev_*`. |
| **Browse / USB routing cave** | Provides a routine to force the Source/browse mode the UI needs on the SC Live 4. |
| **Key dispatch hardening** | `IKeyManager` throws from `FixedAddressArray::add()` when the panel never sends data; the throw is uncaught and aborts `rbp`. |
| **Power-manager NULL guards** | No Pioneer power-manager MCU; notification routines dereference `NULL` and crash the `UsbMountManager` thread. |
| **Display waveform gate** | The scrolling waveform is gated on a browse-caution id that never clears. |
| **Touch** | Browse-caution gate discards all touches; list drag-scroll has a state bug. |
| **Panel comm** | `PanelComPeerLinux::postMessage` waits forever for a front-panel thread that never starts → startup deadlock. |
| **Audio** | `scanForDevices()` skips the device list on non-i.MX6 CPUs. |
| **udev strings** | Move FIFO paths into the writable tmpfs. |

## Adding a patch

1. Find a zero-filled code cave in the executable segment if you need one.
   ARM modified immediates are easy to get wrong — encode with
   `imm8 ROR (2*rot)`, branches with
   `0xea000000 | ((target-(pc+8))>>2)`.
2. Append `(VA, stock_word, patched_word, note)` to `PATCHES`.
3. Verify: `python3 rbp_patch.py stock --check` → all applied.
4. Re-run against the reference binary and confirm the output md5 if you are
   changing the canonical build.
