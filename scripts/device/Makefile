# scripts/device/Makefile
#
# Builds the on-device `timeout` helper (hard-float ARM, static). The rest of
# scripts/device/ is shell that runs as-is on the SC Live 4.
#
#   make CROSS=arm-linux-gnueabihf-

CROSS ?= arm-linux-gnueabihf-
CC    := $(CROSS)gcc

.PHONY: all clean
all: timeout

timeout: timeout.c
	$(CC) -O2 -static -Wall -Wextra -o $@ $<

clean:
	rm -f timeout
