# 07 — Touchscreen

The SC Live 4's ILI2117 reports the same 2048×2048 raw range as the Prime GO,
so the `fbshim-tsc.so` transform is reused **unchanged**; the identity touch
calibration files below are what make rbp accept the taps.

## Stack

`rbp` expects a tsc2007 resistive panel at `/dev/tsc2007_2-0048`.
`fbshim-tsc.so` intercepts `open("/dev/tsc2007_2-0048")` and serves a pipe that
a reader thread fills from `/dev/input/event0` (ILI2117) with the 6-byte RX3
protocol, including the rotation transform and debounce burst.

## Verified on the SC Live 4

* ILI2117 on `/dev/input/event0`, raw axes `[0, 2048)`.
* Live events confirmed with `tools/touchdump` (static ARM):
  ```
  axis ABS_MT_POSITION_X min=0 max=2048
  axis ABS_MT_POSITION_Y min=0 max=2048
  ... type=3 code=53 (X) value=970, code=54 (Y) value=1608 ...
  ```

## Required files

`rbp` reads `root/settings/TouchCalib_User.dat` (fallback
`TouchCalib_Factory.dat`). The shim's transform assumes an **identity**
calibration, so both files must contain:

```
0
0
320
200
1280
800
```

Line order is load-bearing (offX, offY, scaleX, scaleY, checkX=1280,
checkY=800). `scripts/build-chroot.sh` installs them.

## Notes

* The browse-caution gate patches (`0x2dc228`/`0x2dc46c`) are in `rbp-audio`;
  `knobshim2` additionally clears the caution id at `0x05a191fc` while USB1 is
  mounted.
* Verify raw events with `tools/touchdump /dev/input/event0`.
