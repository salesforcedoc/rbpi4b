#!/bin/sh
# webvnc-run.sh -- rbp's screen in a browser: noVNC served, and bridged to the RFB port.
#
# WHY A WEB CLIENT AT ALL, when a native one already works:
#
#   * it needs no client software on the machine looking at the screen, which is the
#     difference between "watch it from the desk Mac" and "watch it from anything";
#   * and it is the FIRST client this unit can use its HARDWARE JPEG ENCODER with. Measured
#     2026-10-09: JPEG may only be sent to a client that *advertised* a quality level, and
#     Apple's Screen Sharing never does -- so every macOS session rides zlib instead and
#     /dev/video11 sits idle. noVNC's Tight does advertise one.
#
# WHAT IT IS NOT. websockify is a BRIDGE: it reframes WebSocket bytes to TCP and back, and
# does nothing else. The VNC session -- including the PASSWORD, end to end -- is negotiated
# between the browser and vncserve, exactly as a native client negotiates it. So this is not
# a new way in; it is the same door, reachable from a browser. With sharing OFF there is no
# RFB listener for it to reach, and a browser connection simply fails -- the same gate the
# native client has.
#
# THE PROOF IT WORKS, with no browser involved (run it here, from the unit):
#
#   python3 - <<'PY'
#   import base64, os, socket, time
#   s = socket.create_connection(("127.0.0.1", 5903), timeout=5)
#   k = base64.b64encode(os.urandom(16)).decode()
#   s.sendall(("GET /websockify HTTP/1.1\r\nHost: 127.0.0.1:5903\r\nUpgrade: websocket\r\n"
#              "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % k).encode())
#   buf = b""
#   while b"\r\n\r\n" not in buf: buf += s.recv(4096)
#   data = buf.split(b"\r\n\r\n", 1)[1]
#   while len(data) < 2: data += s.recv(4096)
#   print(data[2:2 + (data[1] & 0x7F)])      # -> b'RFB 003.008\n'
#   PY
#
# A CONTROL MATTERS HERE: also read the greeting straight off the RFB port. A probe that only
# ever sees the bridge cannot tell "the bridge works" from "my own parsing is wrong" -- the
# first version of this check ate the greeting in the same read as the handshake response and
# reported a timeout that looked exactly like a broken bridge.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
rb_load_conf

PORT=${RB_VNC_WEB_PORT:-5903}
ROOT=${RB_VNC_WEB_ROOT:-/usr/share/novnc}
VNC=${RB_VNC_PORT:-5901}
BIN=/usr/bin/websockify

if [ ! -x "$BIN" ]; then
    echo "webvnc-run: websockify is not installed." >&2
    echo "webvnc-run:   apt-get install novnc websockify" >&2
    exit 1
fi
if [ ! -f "$ROOT/vnc.html" ]; then
    echo "webvnc-run: no noVNC page at $ROOT/vnc.html -- is the novnc package installed?" >&2
    exit 1
fi

# Binds the LAN, not loopback: the browser is on the other end of it. That is the same
# exposure the RFB port already has, and no more -- the password is the viewer's.
echo "webvnc-run: noVNC on :$PORT from $ROOT, bridging to 127.0.0.1:$VNC" >&2
exec "$BIN" --web "$ROOT" "$PORT" "127.0.0.1:$VNC"
