# jpeg565 -- the JPEG decode the browser window needs on the HOST.
#
# Built ON THE UNIT, not in the shim's Docker image: that image targets armel
# (armv5t, soft float) for the vendor chroot, and this runs in the host's armhf
# userland, which is a different ABI. The unit has gcc (14.2) and needs
# `libjpeg62-turbo-dev` for the header -- the runtime library is already there,
# pulled in by chromium.
#
#   make            build ./jpeg565
#   make check      build, then decode a JPEG and report the frame and the time
#   make install    put it next to the driver, where rbrowser.py looks for it

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

jpeg565: jpeg565.c
	$(CC) $(CFLAGS) -o $@ jpeg565.c -ljpeg

check: jpeg565
	@test -n "$(JPG)" || { echo "make check JPG=<a jpeg> [W=1120 H=540]"; exit 1; }
	@time ./jpeg565 $(or $(W),1120) $(or $(H),540) $(JPG) > /tmp/jpeg565.raw
	@echo "$$(stat -c %s /tmp/jpeg565.raw) bytes (want $$(( $(or $(W),1120) * $(or $(H),540) * 2 )))"

clean:
	rm -f jpeg565 /tmp/jpeg565.raw

.PHONY: check clean
