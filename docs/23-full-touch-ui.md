# 23 — Persistent touch navigation and deck switches

This optional integration brings the navigation and VINYL/SLIP/MT controls from
[salesforcedoc/full-touch-ui-xdj-rx3](https://github.com/salesforcedoc/full-touch-ui-xdj-rx3)
to the Pi's existing `fbshim.so` and pointer pipeline. It coexists with the FLX6
map and Toolkit panels but does not require the optional Toolkit bundle.
**Built and tested locally; Pi acceptance is pending.**

## Controls

| Button | Action |
| --- | --- |
| Back chevron | Native BACK inside lists; SOURCE, TAG LIST, INFO, SHORTCUT and UTILITY use their selector key to return to the decks. |
| SOURCE / BROWSE / TAG LIST / PLAYLIST / SEARCH | The corresponding native RX3 screen key. |
| Gear, tap | Native MENU (MY SETTINGS where supported by rbp). |
| Gear, hold at least 350 ms, then release | Native UTILITY settings; the shim sends the verified 500 ms MENU hold. |
| Lightning bolt | Native SHORTCUT screen. |
| VINYL, per deck | Toggle VINYL/CDJ jog mode. |
| SLIP, per deck | Toggle Slip. |
| MT, per deck | Toggle Master Tempo/key lock. |

The navigation is persistent. SEARCH has a compact MENU dropdown to preserve
the native search header and keyboard; an outside tap dismisses it. Deck switches
appear on the performance screen and its INFO/countdown views. Their blue state
comes from the native player, including changes made with the controller.

Use the same button for both edges of a tap. The deck switches now have larger
touch targets (logical y568–637 with a3px side margin), separate from their
painted rectangles. Small excursions during a deck tap are tolerated; lifting
on another control or outside the target cancels it. Native deck key edges are
separated by80ms, matching the reference UI. Navigation still cancels a slide-off;
a screen change or touch-device disconnect also cancels pending taps. Existing
shim panels take priority. The native touch-calibration screen is left alone.
The toolkit's KEY, STEMS and SAMPLES panels keep their own touch handling.

The old swipe menu remains available: begin a downward swipe in the top centre
**just below the persistent buttons** (logical rows 42–55 on the deck screen,
45–55 on other screens). Its gear still opens the Pi-specific mixer/USB/VNC
settings panel. The persistent gear opens the native RX3 menu instead.

## Build, deploy and enable

The normal [`build-chroot.sh`](../scripts/build-chroot.sh) build already compiles
this code into `fbshim.so`, and the main deployment in the
[tutorial](../TUTORIAL.md) carries it to the Pi. After that deployment, enable
`RB_POINT_FULL_UI=1` in `/opt/rblive4/rb.local.conf` and restart. The commands
below are the faster path for iterating on this shim without rebuilding the
main tarball.

On your workstation, from the repository root:

```sh
make -C scripts/shims RX3="$PWD/extracted/XDJRX3-rootfs" fbshim.so check
scp scripts/shims/fbshim.so scripts/device/start-rb.sh user@pi-host:/tmp/
```

Replace `user@pi-host` with your Pi's SSH username and hostname or IP address.
On the Pi, stop the player before replacing its shim:

```sh
sudo systemctl stop rblive4
sudo install -m 755 /tmp/fbshim.so /opt/rblive4/fbshim.so.next
sudo mv /opt/rblive4/fbshim.so.next /opt/rblive4/fbshim.so
sudo install -m 755 /tmp/start-rb.sh /opt/rblive4/start-rb.sh
sudo nano /opt/rblive4/rb.local.conf
```

Add or update:

```sh
RB_POINT_FULL_UI=1
```

Then:

```sh
sudo systemctl start rblive4
sudo tail -n 100 /tmp/pointsrc.log
```

Expect `full ui: primary Flip hooked`, followed by
`full ui: presenting navigation and deck controls (16 bpp)` or `(32 bpp)`.
Actions log the key and channel. The launcher must be updated alongside the
shim so the new flag reaches the player; older `rb.conf` files are supported.
No toolkit rebuild or new preload entry is needed.

For the accompanying FLX6 jog/Color parameter/button corrections, also build
and deploy `knobshim.so` alongside `fbshim.so` and the launcher. The FLX6 now
defaults to `RB_JOG_DIVISOR=3`; set it explicitly in `rb.local.conf` if desired.
This corrects pulse position and speed together; it does not slow the ring's
normal playback animation. See the [mapping inventory](flx6-mapping-audit.md)
for the new Slip, Reverse and Master Tempo bindings.

Set `RB_POINT_FULL_UI=0` and restart to restore the previous UI. It defaults off.

## Implementation and checks

`full_ui.c` owns the shared paint/hit geometry, gesture state and guarded layout
adjustments. `full_ui_runtime.c` hooks the logical primary surface's DirectFB
Flip method, paints before the real presentation, and reads native deck state.
RGB565, RGB32 and ARGB surfaces are supported; other formats and dimensions
are refused, including their touch targets. Scaling/rotation and the physical
framebuffer format remain the display driver's job. No additional DRM plane is
allocated. Native MY SETTINGS is moved below the bar and the Track Filter title
is moved clear, only after checking all original firmware 1.20 layout words.

`test_full_ui` covers geometry, key/channel mappings, holds, cancellation,
dropdown dismissal, layout guards and bounded painting at both pixel depths.
`test_full_ui_runtime` uses a mock DirectFB primary to exercise the production
Flip hook, input dispatch, modal priority, failed locks and unsupported formats.
The flag offsets and primary Flip call were checked against the extracted
firmware disassembly. These checks do not execute the player.

On the Pi, check every screen, Back within playlists and at their root, gear
tap/hold, SEARCH dropdown dismissal, both decks' indicators after MIDI changes,
Stems/Samples/KEY with dark and light themes, the existing swipe panels, and
screen/touch hot-plug. Check for stale pixels and idle-screen indicator updates.
Only those trials establish the integration's behavior on the glass.
