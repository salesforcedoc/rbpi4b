# scripts/

Host-side build tooling and the on-device scripts for the SC Live 4 port.

```
scripts/
├── build-chroot.sh        assemble the soft-float chroot -> work/rbx3-run.tgz
├── patch-rbp-sclive4.py   SC Live 4-only rbp patch (getPcController NULL deref)
├── device/                scripts that run on the SC Live 4
└── shims/                 LD_PRELOAD shim sources (soft-float, JP21)
```

## `build-chroot.sh`

Runs on a Linux/WSL host. Stages the RX3 rootfs + gui + patched rbp + the shims
+ the DirectFB 1.4.16 stack + `directfbrc` + touch calibration, fixes exec bits,
and tars the result. Override asset paths with `RX3=` / `CHROMEBIT=` / `OUT=`.
See [docs/05](../docs/05-chroot.md).

## `patch-rbp-sclive4.py`

Applies the SC Live 4 `getPcController()` NULL-deref fix on top of the shared
`rbp-audio`:

```sh
python3 patch-rbp-sclive4.py rbp-audio -o rbp-audio-sclive4
```

See [docs/03](../docs/03-port-plan.md).

## `device/`

On-device scripts — see [`device/README.md`](device/README.md).

## `shims/`

The LD_PRELOAD shims — see [`shims/README.md`](shims/README.md).
