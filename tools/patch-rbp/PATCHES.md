# rbp patch reference

Source of truth: [`rbp_patch.py`](rbp_patch.py). Every entry is
`(VA, stock_word, patched_word, note)`; the patcher verifies the stock word
before writing and is idempotent. `VA = file_offset + 0x8000`.

* stock: md5 `4f2efcfc0c9e3f539289f863acfddcc6`
* patched (`rbp-audio`): md5 `3706c68f7242779d46afa09f35a39acf`

Words are shown as little-endian u32 hex. `E320F000` is `nop`,
`E1A00000` is `mov r0,r0` (also a nop), `E12FFF1E` is `bx lr`.

---

## 1. Startup / panel / USB base

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x020af0` | `0A000022` | `E1000000` | short-circuit vendor startup branch |
| `0x020afc` | `EB0490F4` | `E1000000` | short-circuit vendor startup call |
| `0x020b08` | `0A000013` | `E1000000` | short-circuit vendor startup branch |
| `0x020c54` | `003FE218` | `64656D2F` | build `"/media/usb1/sda1"` (1/5) |
| `0x020c58` | `02435738` | `752F6169` | (2/5) |
| `0x020c5c` | `024355E8` | `2F316273` | (3/5) |
| `0x020c60` | `004E3054` | `31616473` | (4/5) |
| `0x020c64` | `004D52C0` | `004D5200` | (5/5) |
| `0x159ce8` | `E5C9300F` | `E320F000` | nop vendor status write |

## 2. Browse / USB routing code cave

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x09b678` | `00000000` | `E3A01000` | cave: `mov r1,#0` |
| `0x09b67c` | `00000000` | `EA0B8337` | cave: branch to `setBrowseMode(12)` |

## 3. Panel comm deadlock & touch startup

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x2bb87c` | `012FFF1E` | `E1A00000` | `IReceptionForMAIN::startUp` never early-returns on null global |
| `0x2d6cb0` | `18BD8038` | `E1A00000` | `TouchPanel::openDevice` never bails on `isThreadRunning` |
| `0x31ddb0` | `1A000014` | `E1A00000` | `startUp` never bails on panel flag |
| `0x31ddb8` | `0A00000E` | `EAFFFFFF` | `startUp` skips failing internal init |
| `0x3664b4` | `E0633000` | `E3A03000` | comm helper: `r3 = 0` |
| `0x366530` | `1A000004` | `EA000004` | `PanelComPeerLinux::postMessage` wait-for-panel loop removed (startup deadlock) |

## 4. Key dispatch / throw hardening ("fixthrow")

`rbp`'s key dispatcher throws when the front panel never registers keys, and
the exception is uncaught → `SIGABRT` / `SIGSEGV`. These make the dispatcher
tolerate a missing/full target array.

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x2cf7a0` | `E3A00004` | `E8BD8070` | `UiTimer` throw path → pop/return |
| `0x366cd0` | `E3A01002` | `E3001802` | `socketpair()` non-blocking (no UI stall) |
| `0x3779c0` | `E92D4FF8` | `E12FFF1E` | `UiTimer` callback immediate return |
| `0x37ad90` | `0A00014B` | `0A000007` | key-dispatch slot search (1/3) |
| `0x37ad94` | `E5900004` | `EA000019` | key-dispatch slot search (2/3) |
| `0x37adb0` | `1A000012` | `EA000012` | key-dispatch slot search (3/3) |
| `0x37b5d0` | `AA0002AB` | `E320F000` | `IKeyInput` bounds check nop |
| `0x37bc30` | `AA0000CC` | `EAFFFFF8` | `FixedAddressArray::add` throw → skip |
| `0x37bc38` | `DA0000CA` | `EAFFFFF6` | `FixedAddressArray::add` throw → skip |
| `0x37bf34` | `DA00000B` | `EA000052` | array bounds → no-throw path |
| `0x37bf68` | `E3A00004` | `EAF47DC2` | return "no free slot" via cave |
| `0x37c048` | `AA00000D` | `E320F000` | key-target array bound nop |
| `0x37c050` | `DA00000B` | `E320F000` | key-target array bound nop |
| `0x37c084` | `E3A00004` | `EAF47D7B` | return "no free slot" via cave |
| `0x37c364` | `0AFFFF46` | `E320F000` | key-target removal guard nop |
| `0x37c710` | `E58450A4` | `E58480A4` | `KeyManager` pending-bitmask init |

## 5. Power-manager NULL-`this` guards

On the SC Live 4 there is no Pioneer power-manager MCU, so the manager pointer is
`NULL`. These routines are called from the USB mount path; left unpatched they
dereference `NULL` and kill the `UsbMountManager` thread before the library
imports.

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x121d9c` | `E92D41F0` | `E3A00000` | PM helper → `mov r0,#0` |
| `0x121da0` | `EB01955A` | `E12FFF1E` | PM helper → `bx lr` |
| `0x12203c` | `E92D4038` | `E3A00000` | PM helper → `mov r0,#0` |
| `0x122040` | `EB0194B2` | `E12FFF1E` | PM helper → `bx lr` |
| `0x122078` | `E92D45F0` | `E3A00000` | PM helper → `mov r0,#0` |
| `0x12207c` | `E24DD00C` | `E12FFF1E` | PM helper → `bx lr` |
| `0x2c6bf0` | `E92D4038` | `E12FFF1E` | `notifyPermissionChanged` → `bx lr` |
| `0x2c7004` | `E92D4070` | `E12FFF1E` | `notifyPreparedToStandby` → `bx lr` |
| `0x32e728` | `E92D45F8` | `E12FFF1E` | USB/power notification helper → `bx lr` |
| `0x3871d0` | `E1A00006` | `E3A00000` | notification helper → `mov r0,#0` |

## 6. Display

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x24fc88` | `1A000004` | `E1A07004` | waveform gate (1/2) |
| `0x24fc8c` | `E5943070` | `EA00007E` | unconditionally create/render scrolling waveform |

## 7. Touch

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x2dc228` | `E1A07000` | `E3A07000` | `solveCoordToKey` ignores browse caution id |
| `0x2dc46c` | `0A000008` | `EA000008` | `touchOn` bypasses caution check |
| `0x363774` | `E5943030` | `EA000028` | list drag → always send scroll key |
| `0x363794` | `E5845030` | `E320F000` | stop zeroing the drag-scroll counter |

## 8. Audio

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x3c665c` | `1A000054` | `EA000054` | `ALSAAudioIODeviceType::scanForDevices`: always configure the RX3 device list (Rockchip CPU fails `board_is_rev`) |

> The stock binary already enables `DjEngineIF::initializeAudioDevice`
> (`0x104d0` is `bne`), so no patch is needed there — earlier work that
> *bypassed* it was a bug and has been reverted.

## 9. udev FIFO paths

The RX3 uses `/proc/udev_*`; on the SC Live 4 `/proc` is not writable, so the
paths move to `/tmp` (shared with the chroot).

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x4dede4` | `6F72702F` | `706D742F` | `/proc` → `/tmp` (`udev_usb1`) |
| `0x4dede8` | `64752F63` | `6564752F` | udev_usb1 (2/4) |
| `0x4dedec` | `755F7665` | `73755F76` | udev_usb1 (3/4) |
| `0x4dedf0` | `00316273` | `00003162` | udev_usb1 (4/4) |
| `0x4dedf4` | `6F72702F` | `706D742F` | udev_usb2 (1/4) |
| `0x4dedf8` | `64752F63` | `6564752F` | udev_usb2 (2/4) |
| `0x4dedfc` | `755F7665` | `73755F76` | udev_usb2 (3/4) |
| `0x4dee00` | `00326273` | `00003262` | udev_usb2 (4/4) |
| `0x4e0e54` | `6F72702F` | `706D742F` | udev_usbctn1 (1/5) |
| `0x4e0e58` | `64752F63` | `6564752F` | udev_usbctn1 (2/5) |
| `0x4e0e5c` | `755F7665` | `73755F76` | udev_usbctn1 (3/5) |
| `0x4e0e60` | `74636273` | `6E746362` | udev_usbctn1 (4/5) |
| `0x4e0e64` | `0000316E` | `00000031` | udev_usbctn1 (5/5) |
| `0x4e0e68` | `6F72702F` | `706D742F` | udev_usbctn2 (1/5) |
| `0x4e0e6c` | `64752F63` | `6564752F` | udev_usbctn2 (2/5) |
| `0x4e0e70` | `755F7665` | `73755F76` | udev_usbctn2 (3/5) |
| `0x4e0e74` | `74636273` | `6E746362` | udev_usbctn2 (4/5) |
| `0x4e0e78` | `0000326E` | `00000032` | udev_usbctn2 (5/5) |

## 10. Deliberately *not* patched

| Address | Reason |
|---|---|
| `0x104d0` | stock audio-init branch is already correct; do not bypass it |
| `0x1a4204/08` | the old RGB32 window patch is obsolete — the working display uses RGB16 surfaces + the patched fbdev driver |
