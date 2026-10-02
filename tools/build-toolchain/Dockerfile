# rblive4 shim build environment.
#
# The shims must be soft-float EABI5 ARM32 linked against the XDJ-RX3 glibc 2.13,
# so the cross toolchain here is arm-linux-gnueabi (armel/soft-float) -- NOT
# arm-linux-gnueabihf. Build once, then use it for `make -C scripts/shims`:
#
#   docker build -t rblive4-build tools/build-toolchain/
#   docker run --rm -v "$PWD:/src" -w /src rblive4-build \
#     make -C scripts/shims RX3=extracted/XDJRX3-rootfs test
#
# It is committed on purpose, and this file is the only copy: the tutorial offers
# the container as the way to build the shims when the float-ABI toolchain is
# awkward to install on the host, and that offer is only reproducible if the
# recipe for the image travels with the repository. The same toolchain can be
# installed directly instead -- `gcc-arm-linux-gnueabi` + `libc6-dev-armel-cross`,
# which is what the RUN below installs -- and `qemu-user`, for the static ARM test
# binaries the Makefile's `test` target runs.
#
# No libasound2-dev here, deliberately: no shim ever includes <alsa/...>. Every
# snd_* call is resolved at runtime with dlsym(RTLD_DEFAULT, ...), so the only
# ALSA header needed is <sound/asequencer.h>, which is a *kernel* header and
# comes with linux-libc-dev. Adding libasound2-dev would silently pull the host
# (arm64) headers into an armel build -- a trap worth not setting.
#
# That is also why tools/pcmprobe.c needs no headers of its own: it declares the
# snd_pcm_* prototypes it uses and links the CHROOT's libasound, the copy the
# audio shim dlopens, so the answer it prints is the one the target library gives.

FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-linux-gnueabi \
        libc6-dev-armel-cross \
        binutils-arm-linux-gnueabi \
        binutils \
        make \
        file \
        python3 \
        qemu-user \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS=arm-linux-gnueabi-
ENV QEMU_LD_PREFIX=/src/extracted/XDJRX3-rootfs
