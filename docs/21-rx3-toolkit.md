# 21 — Optional RX3 toolkit features on the Pi

This port integrates [Tratosca/rx3-toolkit](https://github.com/Tratosca/rx3-toolkit),
pinned to cc17a1fde3016a44440948e50bde72f430da34d9. Its checkout, bundle and user
assets live under the ignored work/ directory. No firmware, key or compiled
library is committed. Stems and its waveform module are now available as an
additional opt-in; see [Stems preparation and controls](22-stems.md).

The user confirmed the expanded toolkit deployment works on the Pi after updating
its scripts. Stems was added afterward and still needs Pi audio/control testing.

## Available features

All switches default off. Existing rb.local.conf overrides continue to work.
Key Sync automatically enables its Key Shift and Enhanced Key Match dependencies.

| Setting in rb.local.conf | Behavior when enabled |
| --- | --- |
| RB_TOOLKIT_BEATJUMP32=1 | Beat Jump pads7/8 become −32/+32 **beats**, replacing −8/+8. No MIDI remapping. |
| RB_TOOLKIT_IMMEDIATE=1 | Consecutive Beat Jumps take the immediate native path instead of waiting for quantization. |
| RB_TOOLKIT_DECODER_SLEEP=1 | Send native `bufsleep` commands for both decks after startup; default100000ns, instead of1ms. Replies are logged, not an independent readback. |
| RB_TOOLKIT_KEYSHIFT=1 | KEY tab, per-deck −12..+12 semitone down/reset/up. |
| RB_TOOLKIT_KEY_SYNC=1 | KEY panel proposes/applies a shift using the MASTER reference and configured matching rules. |
| RB_TOOLKIT_KEY_MATCH=1 | Additional harmonic matches in BROWSE. |
| RB_TOOLKIT_STEMS=1 | Prepared instrumental/vocal/drum controls, Slip Loop pads and waveform adaptation. See [Stems](22-stems.md). |
| RB_TOOLKIT_SAMPLES=1 | SAMPLES panel routes physical pads to a prepared bank; eight slots, four simultaneous voices total. |
| RB_TOOLKIT_BROWSE_COLUMNS=1 | Third BROWSE column with sortable heading. Use physical LOAD buttons. |
| RB_TOOLKIT_SEARCH_LATIN=1 | Accent-insensitive search, for example NINO finds NIÑO. |
| RB_TOOLKIT_HIDE_TITLES=1 | Touchscreen eye controls hide/show each deck title (upstream name: asshole-mode). |
| RB_TOOLKIT_LOGO=1 | Replace the performance-screen wordmark with prepared user artwork. Requires artwork first. |
| RB_TOOLKIT_THEME=switch | Start dark; Utility DISPLAY MODE can switch to light. Also accepts off, dark, light, wait. |
| RB_TOOLKIT_NOW_PLAYING=1 | Native track state as UDP JSON on port50123 to a configured IPv4 destination. |
| RB_TOOLKIT_RENDER_PROBE=1 | Additional runtime rendering/key diagnostics; requires a runtime module. Leave off normally. |

Additional settings:

```sh
RB_TOOLKIT_KEY_SYNC_RANGE=1          # 1..12 semitones
RB_TOOLKIT_KEY_SYNC_MODE=identical   # identical or harmonic
RB_TOOLKIT_KEY_MATCH_RULES=2         # bitmask: 2=+2, 4=+7, 8=experimental +4 Camelot
RB_TOOLKIT_BROWSE_FIELD=13           # 7=artist, 11=duration, 13=BPM, 15=key
RB_TOOLKIT_SAMPLES_DIR=/root/pdj/rx3-samples  # INSIDE the chroot
RB_TOOLKIT_STEMS_DIR=auto            # RX3_STEMS on the track's own USB
RB_TOOLKIT_NOW_PLAYING_HOST=127.0.0.1 # receiver on Pi; replace with computer IPv4
RB_TOOLKIT_DECODER_SLEEP_NS=100000
```

Key-match bit0 is reserved and masked out, matching upstream behavior. Rules
are harmonic suggestions, not a promise that every transition will sound good.
The physical FLX6 assignments remain in place. The new panels provide access to
the added functions. Stems uses primary Slip Loop pads5/6/7 only for a loaded prepared track.
Samples has priority while its panel owns the pads; tap
SAMPLES again or select a physical pad mode to restore native pad routing.
Playing loop/latch samples continue across panel changes. The rebuilt knobshim
bridges shim pad/mode/SHIFT events to the toolkit, including shifted pads,
without sending synthetic native SHIFT events. Held sample pads receive releases
on controller disconnect. No bank is reassigned permanently. Their LEDs are written
through native LED state, which the existing FLX6 LED bridge reads.

The FLX6 channel4 volume fader controls the shared sampler VOL from0 to100,
using the same level as the SAMPLES touchscreen slider. It works with the
panel closed too, including for playing loops/latches. Both input paths update
the same value: moving the physical fader takes over immediately, without
pickup. Startup/reconnect position reports can also apply its physical level.
Deploy the matching `knobshim.so` and Toolkit bundle for this mapping; an older
bundle without the callback leaves the fader inactive. Channel4 trim, EQ,
Color FX and PFL are not sampler controls. The fader mapping has host tests and
ARM build checks; its physical response still needs Pi verification.

The stock RX3's Telnet and USB gadget network setup is not installed on the Pi:
use its existing SSH access. Its player stdout/stderr already goes to the host
rbp log; runtime hook diagnostics and a snapshot command are available below.
No stock autoexec.bin installer, service replacement or firmware flashing is used.

## Build on the workstation

The repository [tutorial](../TUTORIAL.md) presents this as the optional second
build after the main chroot. This section is the detailed Toolkit-only path.

The pinned checkout already exists in this workspace. On a fresh clone:

```sh
git clone https://github.com/Tratosca/rx3-toolkit.git work/rx3-toolkit
git -C work/rx3-toolkit checkout --detach cc17a1fde3016a44440948e50bde72f430da34d9
```

From the rbpi4b repository root:

```sh
python3 scripts/build-rx3-toolkit.py
make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs" knobshim.so
```

Requires extracted stock firmware1.20, RX3 rootfs libraries, Python3 and
arm-linux-gnueabi-gcc/binutils. Optional paths are listed by --help. The builder
checks the stock SHA1, source pin, clean tracked upstream source, baseline
16/32-bit player variants, guarded patch words, direct hook guards and library
imports against the player's actual DT_NEEDED libraries. Runtime guards remain
active for all modules, including shared-service hooks and native data records.
One upstream unused diagnostic variable warning is allowed under GCC.

Output work/rx3-toolkit-pi/ includes the complete optional core library, dark/light panel
assets, glyphs, receiver script, schema2 manifest and licenses. It includes no
player executable or user audio/artwork. Generated composition/process/network
and pad-input adapter sources are included in source/; upstream tracked files stay untouched.
The build uses ARMv7 softfp/NEON with register-compatible soft-float calls.

## Deploy and enable

Deploy the main tarball first. The sequence below then installs the optional
bundle without replacing the verified baseline player. It is the same second
deployment shown in the [tutorial](../TUTORIAL.md).

Copy the rebuilt bundle, **launcher/helper and knobshim.so** from your computer.
Replace `user@pi-host` with your Pi's SSH username and hostname or IP address:

```sh
scp -r work/rx3-toolkit-pi user@pi-host:/tmp/
scp scripts/device/start-rb.sh scripts/device/rx3-toolkit.py scripts/shims/knobshim.so user@pi-host:/tmp/
```

On the Pi:

```sh
sudo systemctl stop rblive4
sudo install -m 755 /tmp/start-rb.sh /tmp/rx3-toolkit.py /tmp/knobshim.so /opt/rblive4/
sudo mkdir -p /opt/rblive4/rx3-toolkit
sudo cp -a /tmp/rx3-toolkit-pi/. /opt/rblive4/rx3-toolkit/
sudo nano /opt/rblive4/rb.local.conf
```

Edit existing entries rather than leaving conflicting duplicates. For the new
features that need no user assets:

```sh
RB_TOOLKIT_BEATJUMP32=1
RB_TOOLKIT_KEYSHIFT=1
RB_TOOLKIT_IMMEDIATE=1
RB_TOOLKIT_DECODER_SLEEP=1
RB_TOOLKIT_KEY_SYNC=1
RB_TOOLKIT_KEY_MATCH=1
RB_TOOLKIT_SAMPLES=1
RB_TOOLKIT_BROWSE_COLUMNS=1
RB_TOOLKIT_SEARCH_LATIN=1
RB_TOOLKIT_HIDE_TITLES=1
RB_TOOLKIT_THEME=switch
RB_TOOLKIT_NOW_PLAYING=1
# Set RB_TOOLKIT_NOW_PLAYING_HOST to the listening computer's IPv4,
# or keep its default127.0.0.1 to receive on the Pi itself.
```

Samples initially create an empty bank with default settings; add your sounds
as described next. Enable RB_TOOLKIT_LOGO=1 only after installing artwork.
Start and inspect:

```sh
sudo systemctl start rblive4
sudo tail -n 80 /tmp/rx3-toolkit.log
sudo journalctl -u rblive4 -n 40 --no-pager
```

The new helper's startup message contains `features=... theme=...`. If the
journal instead shows `(32-beat=1 keyshift=1; baseline unchanged)`, that launch
used the older helper, which ignores the additional feature settings. Copy both
updated scripts as well as the bundle and shim, then inspect a **new** startup.
The hook log is root-owned (mode0600), so read it with sudo.

No rb.conf replacement is needed for incremental deployment: the launcher/helper
supply defaults for older schema3 configs. Future full installs copy the helper
via install.sh. All modules are opt-in; the bundle itself enables none.

## Prepare Samples and a logo

On your computer, supply up to eight input files in pad order:

```sh
python3 scripts/prepare-rx3-toolkit-assets.py --samples kick.wav snare.wav vocal.wav
```

If ffmpeg is available, the helper converts inputs to stereo PCM16/44100Hz and
keeps the first eight seconds. Without ffmpeg, supply files already in that
format, at most eight seconds each. Invalid audio is refused before publication.
Use --mode once/hold/loop/latch and --volume (0..100) to choose bank defaults.
Output goes to ignored work/rx3-toolkit-user-assets/. settings.ini also supports
per-slot pad1.mode (0once,1hold,2loop,3latch), pad1.gain (0..200),
pad1.color=#RRGGBB and shift.silence=1. Repeat with pad2..pad8. SHIFT silence
uses the optional FLX6-to-toolkit SHIFT bridge; it does not change native shifted controls.

For a PNG/JPEG/WebP logo (requires Pillow), or a preconverted .rgb565:

```sh
python3 scripts/prepare-rx3-toolkit-assets.py --logo my-logo.png
```

Optional --logo-light supplies matching light artwork. The converter fits within
492x70, writes RX3LOGO1 plus RGB565 pixels and uses native magenta transparency.
Both variants must have the same resulting dimensions. Existing supplied slots
are replaced; older sample slots/artwork are retained until explicitly removed.
The output directory contains only extras supplied by you.

Copy the assets:

```sh
scp -r work/rx3-toolkit-user-assets user@pi-host:/tmp/
```

On the Pi, while stopped:

```sh
sudo systemctl stop rblive4
sudo cp -a /tmp/rx3-toolkit-user-assets/. /opt/rblive4/rbx3-run/root/pdj/
# Enable RB_TOOLKIT_LOGO=1 in rb.local.conf if artwork was installed.
sudo systemctl start rblive4
```

You can also copy a bank prepared by the upstream desktop app: the active bank
is RX3_RUNTIME/samples/banks/<name>/ on its export, with the name stored in
RX3_RUNTIME/samples/active. Copy that bank's numbered WAVs and settings.ini into
the chroot sample directory. This port loads the chosen bank at player startup;
changing banks or replacing a USB-backed bank requires a player restart. Keeping
the bank on the Pi avoids depending on USB mount timing. No automatic mid-set
bank switching or USB-content execution is introduced.

### Samples load and light the pads but are silent

The pinned upstream Samples configuration publisher omits per-pad gain, mode
and SHIFT-silence settings. Without this port's build adaptation, voices load
and advance normally but inherit zero gain. Raising the panel volume cannot
make those voices audible. `build-rx3-toolkit.py` now publishes all parsed bank
settings, including the default gain of100 when no `padN.gain` is specified.
Explicitly saved zero gains remain valid mutes.

For a bundle built before this correction, rebuild with
`python3 scripts/build-rx3-toolkit.py`, redeploy the complete
`work/rx3-toolkit-pi/` directory while `rblive4` is stopped, then start the
service. No sample conversion or main-chroot rebuild is needed. The build
keeps the pinned upstream tracked source unchanged and includes the adapted
Samples source in the bundle's `source/` directory. Host regression tests cover
publication of gains, all four modes and SHIFT-silence while preserving a live
touchscreen volume adjustment. The user confirmed audible sample playback on
the Pi after deploying this correction.

## Now Playing and diagnostics

The Pi adapter sends the upstream JSON datagram format to
RB_TOOLKIT_NOW_PLAYING_HOST:50123. It defaults to loopback and does not configure
an RX3 USB gadget link. With the feature enabled and a receiver IP configured,
run this on the receiving computer from the workstation repo:

```sh
python3 work/rx3-toolkit-pi/now_playing.py
```

The receiver listens on UDP50123. Heartbeats arrive roughly every two seconds;
track load/status/on-air changes trigger updates. The feature supplies metadata,
not a streaming overlay. Permit inbound UDP50123 on that computer if required.
For a receiver on the Pi, its staged copy is:

```sh
python3 /opt/rblive4/rbx3-run/root/pdj/rx3-now-playing.py
```

Diagnostics on the Pi:

```sh
sudo tail -n 80 /tmp/rx3-toolkit.log
sudo journalctl -u rblive4 -n 80 --no-pager
sudo python3 /opt/rblive4/rx3-toolkit.py --chroot /opt/rblive4/rbx3-run \
    --report /tmp/rx3-toolkit-session.json
```

The snapshot contains bounded current/previous hook logs and the PID ready
marker. The existing host rbp log records continuous player output. Hook logs
rotate once at the next core launch. RB_TOOLKIT_RENDER_PROBE=1 adds diagnostics;
turn it off after investigating a problem.

## Startup guards and rollback

The baseline RB_PLAYER is never modified. The helper derives
RB_PLAYER.toolkit/rbp from its verified baseline each launch, applies only the
enabled patch groups and stages checksum-verified assets. Unknown players,
bad patches, corrupt bundle files and invalid user assets are refused before
publishing the derived executable. Logo and Samples are validated separately
from the bundle because they contain user content.

Any selected runtime feature adds librx3_core.so at the end of the existing
preload chain. The core must publish /tmp/rx3-performance.ready with the current
player PID. It refuses readiness when a selected module fails installation.
The explicit loader leaves /proc/self/comm as ld-linux.so.3, so the Pi adapter
checks exact argv against RX3_PI_PLAYER/RX3_PI_LOADER supplied by the launcher.
This was the initial deployment failure; keep both bundle and launcher current.

Disable individual switches and restart to remove their behavior. To return to
the baseline, set **all enabled RB_TOOLKIT_* feature flags to0 and THEME=off**,
then restart. Turning off only the initial two flags does not disable the new
features. Disabling Key Shift alone also leaves it active if Key Sync is enabled.
No binary restoration or generated-file deletion is needed.

Try the new functions on both decks: Samples once/hold/loop/latch and LEDs,
KEY sync/reset, Browse scroll/sort/load, search, title-eye controls, Utility
theme switching, logo placement and UDP receiver. Check playback/loops/cue,
headphones, waveforms, CPU/audio stability and reconnect with them combined.
Browse Columns has an upstream hardware performance concern; turn it off if
scrolling slows down. Both supported display-depth variants need hardware
acceptance if used. Host checks cannot establish audible correctness.

Host checks:

```sh
python3 scripts/device/test_rx3_toolkit.py
sh tools/conf-crosscheck.sh
make -C scripts/device test
```

Tests cover feature toggles/dependencies, rollback, baseline preservation,
asset validation, guarded words, preload order, shell quoting, PID readiness,
real host-loader process recognition and Pi IPv4 byte order. They never launch
the firmware player or alter services.

Upstream runtime code is MPL-2.0. Keep the bundle's license/notices, pinned
source reference and modified-source folder when distributing it. Integration
scripts and Pi adapter headers remain this project's code.
