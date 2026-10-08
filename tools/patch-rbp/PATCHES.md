# rbp patch reference

Source of truth: [`rbp_patch.py`](rbp_patch.py). Every entry is
`(VA, stock_word, patched_word, note)`; the patcher verifies the stock word
before writing and is idempotent. `VA = file_offset + 0x8000`.

* stock: md5 `4f2efcfc0c9e3f539289f863acfddcc6`
* patched (`rbp-audio`): md5 `3dda2d4e10187a75bfc16a7b4f16f192`
* fully patched, 16 bpp (`rbp-nopc`, second stage applied): md5 `97aa2223c4ca5906b66389420f29d03f`
* fully patched, 32 bpp (`rbp-nopc` + third stage): md5 `990404244853a7d613be3d469ad1bc1d`
  — **the default and what the unit runs**
* **superseded** (`rbp-nopc`, 2026-10-06 → 2026-10-07): md5 `18a64bc4d0ffd1cbd35f3a6ea447fca8`
  — ran and played correctly, but its `getPcController()` stub silently disabled Pro
  DJ Link on both routes. Section 11 explains why. `doctor.sh` still names it. 

The last two are the same build at two depths, and *which one is correct depends on
`RB_FB_LIE_BPP` in rb.conf* — see section 12. The first three are produced by
[`tools/patch-rbp/rbp_patch.py`](rbp_patch.py) and
[`scripts/patch-rbp-nopc.py`](../../scripts/patch-rbp-nopc.py), the second and third
of them staged by `scripts/build-chroot.sh`; see sections 11 and 12 below.

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

There is no Pioneer power-manager MCU on the SC Live 4 or the Pi 4, so the
manager pointer is `NULL`. These routines are called from the USB mount path;
left unpatched they dereference `NULL` and kill the `UsbMountManager` thread
before the library imports.

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

## 6. Display — REMOVED 2026-10-08

Two words in `ui_PLAYMODE_Set` @`0x24fb80` forced the play-mode window's centre
down the create/render path unconditionally, bypassing a gate that reads the
browse-caution id `uxth([0x05a191fc])` and two flags:

```
24fc88  bne 24fca0      →  E1A07004  mov r7,r4
24fc8c  ldr r3,[r4,#0x70] →  EA00007E  b 24fe8c   (the create path)
```

The gate is **open on this unit anyway**: measured live on both the Pi and the
reference, `[0x05a191fc]` is 0, `[0x032b2a8b] & 2` is 0 and `[0x522af4]` is 0, so
stock takes the same path. The patch was a workaround for an id that a second
workaround already clears — `ctrlshim.c` (in `knobshim.so`) writes 0 to
`0x05a191fc` whenever USB1 is mounted, and §7's touch patches are a third.

Removed because it was not free: with the two words present the play-mode centre
is repainted at startup and **the boot logo is stamped over**; with them back to
stock the logo persists to the deck view. Reverted on the unit 2026-10-08 and
confirmed on the glass. Stock behaviour when a deck is empty is simply an
unpainted centre — a loaded track still draws the full scrolling waveform.

| VA | stock | patched | purpose |
|---|---|---|---|
| — | — | — | *(no words; the pair `0x24fc88`/`0x24fc8c` was removed)* |

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
| `0x3c665c` | `1A000054` | `EA000054` | `ALSAAudioIODeviceType::scanForDevices`: always configure the RX3 device list (the Rockchip CPU and the Pi 4 both fail `board_is_rev`) |

> The stock binary already enables `DjEngineIF::initializeAudioDevice`
> (`0x104d0` is `bne`), so no patch is needed there — earlier work that
> *bypassed* it was a bug and has been reverted.

## 9. udev FIFO paths

The RX3 uses `/proc/udev_*`; neither the SC Live 4 nor a normal Pi OS install
has a writable `/proc`, so the paths move to `/tmp` (shared with the chroot).

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

---

## 11. Second stage: no PC controller

Applied by [`scripts/patch-rbp-nopc.py`](../../scripts/patch-rbp-nopc.py) to the
`rbp-audio` output, not part of `rbp_patch.py`. It is a separate stage because it
is not a Rockchip/board fix but a "no PC controller is attached" fix: `rbp`'s
network monitor calls `IUiObjManager::getPcController()` about a second after
start, and the getter dereferences `[NULL+0x9c]` because the `UiObjManager` global
(`0x026867C0`, `.bss`) is not published until the UI is built — while
`NetworkManager::initialize()` arms a 1000 ms timer from `main`, so the first tick
can beat it. Address `0x9c` is unmapped here (the ELF's first `PT_LOAD` starts at
`0x8000`), so the symptom is a process that dies instantly with no display and no
log.

**The fault is inside the getter, not at its call site.** `timerCallback` already
handles a NULL return correctly — `ldrbne r3,[r0,#0x72]` / `moveq r3,r0`
@`0x3921a8`. So each getter only has to *return* NULL instead of *faulting*, and
that fits the stock five-word slot exactly: no branch, no shared helper, no
function moved, every entry address unchanged.

| VA | stock | patched | purpose |
|---|---|---|---|
| `0x31df3c` | `E30636B0` | `E59F3058` | `getNet(): ldr r3,[pc,#88]` → pool `0x026866B0` |
| `0x31df40` | `E3403268` | `E5933110` | `getNet(): r3 = [r3,#0x110]` (UiObjManager) |
| `0x31df44` | `E5933110` | `E1B00003` | `getNet(): movs r0,r3` — sets Z from r3 |
| `0x31df48` | `E5930094` | `15930094` | `getNet(): ldrne r0,[r3,#0x94]` |
| `0x31df50` | `E30636B0` | `E59F3044` | `getSettings(): ldr r3,[pc,#68]` |
| `0x31df54` | `E3403268` | `E5933110` | `getSettings(): r3 = UiObjManager` |
| `0x31df58` | `E5933110` | `E1B00003` | `getSettings(): movs r0,r3` |
| `0x31df5c` | `E5930098` | `15930098` | `getSettings(): ldrne r0,[r3,#0x98]` |
| `0x31df64` | `E30636B0` | `E59F3030` | `getPcController(): ldr r3,[pc,#48]` |
| `0x31df68` | `E3403268` | `E5933110` | `getPcController(): r3 = UiObjManager` |
| `0x31df6c` | `E5933110` | `E1B00003` | `getPcController(): movs r0,r3` |
| `0x31df70` | `E593009C` | `1593009C` | `getPcController(): ldrne r0,[r3,#0x9c]` |

The three `bx lr` words (`0x31df4c`, `0x31df60`, `0x31df74`) are unchanged and are
listed in the patcher for completeness. `movs` is the flag-setter that replaces the
dropped `cmp`; flags are caller-saved across a `bl`, so setting them is free. The
three pc-relative offsets were checked against the target's own literal at
`0x31df9c` — ARM reads `pc` as instruction+8, so `0x31df9c - (entry+8)` is 88, 68
and 48. That literal is shared with `getPcChController` @`0x31df78` and is left
alone.

Idempotent, and it refuses a file too short to contain the patch offsets rather
than throwing. The resulting md5 is pinned at the top of this file.

### The stub this replaces, and what it broke

The first version of this stage patched only `getPcController`, to a permanent
`mov r0,#0; bx lr` (`E3A00000`/`E12FFF1E`). It stopped the crash, and it is
byte-identical to the `mov r0,#0; bx lr` thunks Pioneer ships for its own
unimplemented functions, which is exactly why the damage went unnoticed.

But `NetworkMonitor::timerCallback` @`0x392160` reads the Pro DJ Link gate
`ui::PcController::isUsbBConnected()` (`PcController+0x72`) through that getter
**once a second**. A stubbed getter makes the gate structurally unreadable: `rbp`
never calls `operateConnectNetwork` and never calls `checkNetworkConnectionChange`,
so `NetworkMonitor`'s own IP field (`+0x1c`) is never filled either — which in turn
makes the out-of-band `NETALIAS_CONNECT` route refuse forever (`no-ip`). One 2-word
patch disabled Pro DJ Link on **both** routes, silently, on every host that
carried it.

`getNet` and `getSettings` dereference the same NULL `UiObjManager` at `+0x94` and
`+0x98`; they are only lucky that no 1-second timer calls them before the UI is up.
They are patched too.

> This patch is **not** device-specific. It applies unchanged to any host with no
> Pioneer PC-controller link, which is why the file is named `nopc` and not
> `sclive4`.

---

## 12. Third stage: the layer pixel format (the depth pair)

Applied by [`scripts/patch-rbp-depth.py`](../../scripts/patch-rbp-depth.py) to the
output of section 11, not part of `rbp_patch.py`. It is a separate stage because it
is not a fix at all: it is a **choice**, and the only one in this tree that has a
matching half outside the binary.

`DS_HW_Core_Layer_Create` builds the DirectFB layer description in two
instructions, and the format it names is what rbp renders its whole UI in:

| build | instructions | `DSPF_*` | |
|---|---|---|---|
| 16 bpp | `movw r3,#0x801` / `movt r3,#0x20` | `0x00200801` RGB16 | the RX3's own framebuffer |
| 32 bpp | `movw r3,#0xc03` / `movt r3,#0x40` | `0x00400c03` RGB32 | **the default**, and what the unit runs |

| VA | file offset | 16 bpp | 32 bpp | purpose |
|---|---|---|---|---|
| `0x1a3ab8` | `0x19bab8` | `E3003801` | `E3003C03` | `movw r3,#0x801` / `#0xc03` |
| `0x1a3ac0` | `0x19bac0` | `E3403020` | `E3403040` | `movt r3,#0x20` / `#0x40` |

**The other half is `RB_FB_LIE_BPP`** (rb.conf), which is the depth the
framebuffer shim reports in `FBIOGET_VSCREENINFO` — `fb_lie_bpp()` in
`scripts/shims/fb_shim.c`, together with the `line_length` that must move with it
(`1280 * bpp / 8`, so 2560 or 5120). rbp and DirectFB both size their layers from
that lie. The two must agree, and neither can see the other:

* word 16 + lie 32 (or the mirror) → DirectFB's `DS_HW` plugin refuses the layer
  and rbp segfaults **before the panel opens**: black screen, systemd
  restart-looping every 10 s, and nothing in `rbp.log` that names the depth.
* 32 bpp on a 16-bpp panel changes DirectFB's present mode from `off` to
  `convert` (a bpp-only mismatch at unchanged geometry is AUTO-FALLBACK's
  `PRESENT_CONVERT` case), which is what makes the panel path legal here at all.

Two guards keep the pair together, because a silent mismatch is the one failure
nobody can read off the glass:

* `build-chroot.sh` reads `RB_FB_LIE_BPP` out of **the same rb.conf it embeds** and
  passes it to this patcher, so a tarball cannot carry a mismatched pair.
* `start-rb.sh` § 4b reads the byte at `0x19bab8` out of the player that is about
  to run (`01` = 16 bpp, `03` = 32 bpp) and **refuses to launch** on a positive
  mismatch, printing both fixes. A byte that is neither is not a build this tree
  produced and is only noted, so the deploy-root override's whole purpose —
  iterating on one file without a rebuild — still works.

**What 32 bpp costs**, measured on `.239` 2026-10-08, both arms 25 s from a fresh
restart on the same screen (frame rate from the `FB_VSYNC_RATE` counter in
`fb_shim.c`):

| | 16 bpp | 32 bpp |
|---|---|---|
| frame rate | 56.5/s | ~46/s |
| rbp CPU | 36 % of one core | 67 % |

Two parts: rbp's own renderer writes 4 MB a frame instead of 2, and DirectFB takes
one full-frame `rgb32_to_rgb565` convert per present. The convert exists **only**
because the panel is 16 bpp (the whole DirectFB build is `-march=armv5t`, so it is
a scalar loop on a board that has NEON).

**What it buys**: the deck-1 bottom strip's right vertical edge is drawn whole
rather than at 1/4 and 1/8 coverage. That edge is rbp's own 16-bpp render path, so
this word *is* the fix rather than one way to reach it — there is no DirectFB knob
for it.

The patcher is idempotent **in both directions** (`--bpp 16` reverts) and validates
the word it finds at every address, so it aborts on a foreign binary rather than
corrupting it. Round-tripping `97aa2223…` → `99040424…` → `97aa2223…` is
byte-exact.
