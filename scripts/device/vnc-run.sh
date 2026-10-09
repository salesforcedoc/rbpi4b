#!/bin/sh
# vnc-run.sh — serve rbp's screen over VNC, with the raw/hwjpeg switch.
#
# This is the VNC counterpart of start-rb.sh and it is deliberately a small
# fraction of its size. It starts nothing in the chroot, touches no device, takes
# no master, and mounts nothing: it reads the screen and writes sockets. All the
# hardware it needs already exists by the time it runs, because it is watching a
# player that start-rb.sh brought up.
#
# WHY IT IS SEPARATE FROM start-rb.sh AND NOT PART OF IT. Two reasons, and the
# first is the one that matters:
#
#   * A crash in the viewer must not take the player with it. Under rblive4.service
#     a failed vncserve would be a failed rbp, and the operator's decks would go
#     silent because a status page threw. They are separate units so that they fail
#     separately.
#   * The viewer is optional. RB_VNC=0 must mean the process is not started at all
#     and costs exactly nothing, which a branch inside start-rb.sh cannot give as
#     cleanly as an uninstalled unit.
#
# It does read rbp's screen, so it wants rbp running — but it does NOT require it.
# vnc_capture opens /dev/fb0 and /dev/dri/card1 and composites whatever planes are
# lit; with no player that is a console-black screen with the drawers in it, which is
# a perfectly honest thing to serve and a useful thing to see while diagnosing why rbp
# is not up. So there is no ordering dependency on rblive4.service.
#
# Run as root. The DRM planes and /dev/fb0 are root-owned and opening the input
# nodes (when RB_VNC_INPUT is on) needs root besides.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

rb_load_conf

# The binary is built ON THE UNIT by install.sh, into $RB_DEPLOY_ROOT/vncserve/,
# and not carried in the tarball as a prebuilt object. That is not an oversight and
# not laziness: the cross-build image is soft-float armel while this unit's userland
# is hard-float armhf, so there is no toolchain in this tree that can produce this
# binary at all. See scripts/device/vncserve/Makefile for the long version.
BIN="$RB_DEPLOY_ROOT/vncserve/vncserve"
if [ ! -x "$BIN" ]; then
    echo "vnc-run: $BIN is missing or not executable" >&2
    echo "vnc-run: the deploy did not build it. Run install.sh again and read its" >&2
    echo "vnc-run: output: a compile failure there aborts the install loudly rather" >&2
    echo "vnc-run: than shipping a unit that starts nothing." >&2
    exit 1
fi

: "${RB_VNC_PORT:=5900}"
: "${RB_VNC_HTTP_PORT:=$((RB_VNC_PORT + 1))}"
: "${RB_VNC_FPS:=4}"
: "${RB_VNC_PASSWORD:=}"
: "${RB_VNC_MODE:=raw}"
: "${RB_VNC_MODE_FILE:=/run/rblive4/vnc.mode}"
# The deflate level for the compressing encodings (Tight 7 and zlib 6 -- one code
# path, see vncserve.c). 1 is zlib's own default and measured at 8% of one core for
# a live Screen Sharing client; 0 disables compression and sends everything Raw,
# which is a debugging setting and not a fallback anyone wants over wlan0.
# IT IS PASSED AS A FLAG AND NOT LEFT TO THE ENVIRONMENT because rb_load_conf sets a
# shell variable and does not export it, so an unexported RB_VNC_ZLIB_LEVEL in
# rb.conf would be read by nothing -- the server would silently take its own default
# and the config file would be a lie.
: "${RB_VNC_ZLIB_LEVEL:=1}"

# The control file's directory has to exist before the server starts, or the very
# first mode change is written into a directory that is not there and the switch
# silently does nothing. /run, not /tmp: this describes the running system, and a
# reboot is a legitimate way to be back at the default.
mkdir -p "$(dirname "$RB_VNC_MODE_FILE")" 2>/dev/null || true

if [ -z "$RB_VNC_PASSWORD" ]; then
    # Said loudly, because the failure it prevents is invisible from the Mac: macOS's
    # Screen Sharing will not connect to a server that offers no password at all, so
    # an empty password here is not "open", it is "does not work", and the error the
    # client shows does not mention passwords.
    echo "vnc-run: WARNING: RB_VNC_PASSWORD is empty. macOS's Screen Sharing refuses" >&2
    echo "vnc-run:          a server that does not ask for a password, so the client" >&2
    echo "vnc-run:          will fail to connect. Set RB_VNC_PASSWORD in" >&2
    echo "vnc-run:          \$RB_DEPLOY_ROOT/rb.local.conf." >&2
fi

echo "vnc-run: serving on $RB_VNC_PORT, page on $RB_VNC_HTTP_PORT," \
     "$RB_VNC_FPS fps," \
     "mode $RB_VNC_MODE, zlib level $RB_VNC_ZLIB_LEVEL, switch file $RB_VNC_MODE_FILE"

exec "$BIN" \
    --port "$RB_VNC_PORT" \
    --http-port "$RB_VNC_HTTP_PORT" \
    --fps "$RB_VNC_FPS" \
    --password "$RB_VNC_PASSWORD" \
    --mode "$RB_VNC_MODE" \
    --mode-file "$RB_VNC_MODE_FILE" \
    --zlib-level "$RB_VNC_ZLIB_LEVEL" \
    --log "${RB_LOG_DIR:-$RB_DEPLOY_ROOT/log}/vncserve.log"
