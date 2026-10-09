#!/usr/bin/env python3
"""test_confscreen.py -- the status page, tested as a real process against a fake unit.

THE IDIOM IS test_bootscreen.py'S, deliberately: no framework, module loaded by path so its
globals can be patched, and the daemon run as a REAL SUBPROCESS with every path, port and
command overridable by an environment variable. Everything the page reads -- /proc, /sys,
systemctl, vcgencmd, rb.conf, rbp.log, the switch files -- is a file this test writes, so
the whole page can be exercised on a workstation and a check that fails says which fact the
page got wrong.

THE CHECK THAT MATTERS MOST is the frame counter, because it is the difference between "no
player" and "a player that is running and painting nothing" -- two states that look
identical from the front of the unit. So the test drives the real rate logic through real
HTTP requests: first poll (no previous sample), then frames appended, then frames stopped.
"""

import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CONFSCREEN = os.path.join(HERE, "confscreen.py")

_fails = []
_checks = 0


def ok(msg):
    global _checks
    _checks += 1
    print("  ok   %s" % msg)


def check(cond, msg):
    global _checks
    _checks += 1
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        _fails.append(msg)


def load_module():
    """Load confscreen.py by path, so this test can patch its module globals for the pure
    checks (the same trick test_bootscreen.py uses)."""
    spec = importlib.util.spec_from_file_location("confscreen_under_test", CONFSCREEN)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


FAKE_SH = r"""#!/bin/sh
# A fake tool, installed as both `systemctl` and `vcgencmd` on a shim PATH. It records
# every argv it is handed, so the test can assert that nothing ever names the player path.
printf '%s\n' "$(basename "$0") $*" >> "$FAKE_LOG"
case "$(basename "$0")" in
  systemctl)
    unit=$2
    case "$1" in
      is-active)  cat "$FAKE_UNITS/$unit.active"  2>/dev/null || echo inactive ;;
      is-enabled) cat "$FAKE_UNITS/$unit.enabled" 2>/dev/null || echo disabled ;;
      *) : ;;
    esac ;;
  vcgencmd) cat "$FAKE_THROTTLED" 2>/dev/null || echo "throttled=0x0" ;;
esac
exit 0
"""


def make_fake(root, frames=40, pid=4242, depth=(0x03, 0x40), live="on"):
    """A unit on disk: a conf, a player log, a /proc, a /sys, switch files, and a player
    binary carrying a layer-format word. Returns the env the daemon should run with."""
    deploy = os.path.join(root, "opt")
    os.makedirs(os.path.join(deploy, "log"))
    os.makedirs(os.path.join(deploy, "rbx3-run", "root", "pdj"))
    run = os.path.join(root, "run")
    os.makedirs(run)
    proc = os.path.join(root, "proc")
    os.makedirs(os.path.join(proc, str(pid)))
    os.makedirs(os.path.join(proc, "4001"))          # edb_streamd: loader-started too
    sysfs = os.path.join(root, "sys", "class", "thermal", "thermal_zone0")
    os.makedirs(sysfs)
    units = os.path.join(root, "units")
    os.makedirs(units)

    # The shipped conf, with the operator's own override sourced from its tail -- the shape
    # the real rb.conf has, so the page's `conf_values()` is exercised for real.
    with open(os.path.join(deploy, "rb.conf"), "w") as f:
        f.write(": \"${RB_VNC:=1}\"\n: \"${RB_VNC_PORT:=5901}\"\n"
                ": \"${RB_VNC_HTTP_PORT:=5902}\"\n: \"${RB_VNC_FPS:=4}\"\n"
                ": \"${RB_VNC_MODE:=raw}\"\n: \"${RB_VNC_LIVE:=off}\"\n"
                ": \"${RB_VNC_INPUT:=0}\"\n: \"${RB_BOOTSCREEN:=1}\"\n"
                ": \"${RB_PREWARM:=1}\"\n: \"${RB_POINT_KIND:=auto}\"\n"
                ": \"${RB_MIDI_MAP:=}\"\n: \"${RB_AUDIO_DEV:=}\"\n"
                ": \"${RB_FB_LIE_BPP:=32}\"\n: \"${RB_VERBOSE:=0}\"\n"
                "if [ -f \"$RB_DEPLOY_ROOT/rb.local.conf\" ]; then\n"
                "  . \"$RB_DEPLOY_ROOT/rb.local.conf\"\nfi\n")
    with open(os.path.join(deploy, "rb.local.conf"), "w") as f:
        f.write("# machine-local\nRB_VNC_FPS=7\n")

    with open(os.path.join(deploy, "log", "rbp.log"), "w") as f:
        f.write("ALSA lib pcm_hw.c:1401:(_snd_pcm_hw_open) Invalid value for card\n")
        for _ in range(frames):
            f.write(".../plugins/driver/DS_HW_Glib3_DFB.c <1106>:\n")

    with open(os.path.join(proc, str(pid), "cmdline"), "wb") as f:
        f.write(b"/lib/ld-linux.so.3\x00/root/pdj/rbp\x00-a\x00")
    # field 3 onward after `(comm)`; utime and stime are fields 14 and 15.
    with open(os.path.join(proc, str(pid), "stat"), "wb") as f:
        f.write(b"%d (rbp) S " % pid + b"0 " * 10 + b"100 200 " + b"0 " * 8)
    with open(os.path.join(proc, "4001", "cmdline"), "wb") as f:
        f.write(b"/lib/ld-linux.so.3\x00/usr/bin/edb_streamd\x00")
    with open(os.path.join(proc, "4001", "stat"), "wb") as f:
        f.write(b"4001 (edb_streamd) S " + b"0 " * 20)
    with open(os.path.join(proc, "uptime"), "w") as f:
        f.write("1234.56 999.00\n")
    with open(os.path.join(proc, "loadavg"), "w") as f:
        f.write("0.49 0.14 0.05 1/250 12345\n")
    with open(os.path.join(proc, "meminfo"), "w") as f:
        f.write("MemTotal:        3795000 kB\nMemAvailable:    3308000 kB\n")

    with open(os.path.join(sysfs, "temp"), "w") as f:
        f.write("50100\n")
    # 0x50005 = bits 0 and 18: under-voltage NOW, throttled has occurred.
    with open(os.path.join(root, "throttled"), "w") as f:
        f.write("throttled=0x50005\n")

    with open(os.path.join(run, "boot.log"), "w") as f:
        f.write("15.56  pre-warmed rbp files (2.3s)\n15.59  launching rbp\n")
    with open(os.path.join(run, "boot.stage"), "w") as f:
        f.write("launching rbp\n")
    for name, val in (("live", live), ("mode", "raw"), ("input", "off")):
        with open(os.path.join(run, "vnc." + name), "w") as f:
            f.write(val + "\n")

    with open(os.path.join(root, "health.log"), "w") as f:
        f.write("seq=5 up=72 load=0.68 kerr=0 thr=0x0\n")

    player = os.path.join(deploy, "rbx3-run", "root", "pdj", "rbp")
    with open(player, "wb") as f:
        f.seek(0x19BAB8)
        f.write(bytes([depth[0]]))
        f.seek(0x19BAC0)
        f.write(bytes([depth[1]]))

    shim = os.path.join(root, "shim")
    os.makedirs(shim)
    for tool in ("systemctl", "vcgencmd"):
        p = os.path.join(shim, tool)
        with open(p, "w") as f:
            f.write(FAKE_SH)
        os.chmod(p, 0o755)

    with open(os.path.join(units, "rblive4.active"), "w") as f:
        f.write("active\n")
    with open(os.path.join(units, "rblive4.enabled"), "w") as f:
        f.write("enabled\n")
    with open(os.path.join(units, "rblive4-vnc.active"), "w") as f:
        f.write("inactive\n")

    env = dict(os.environ)
    env.update(
        RB_DEPLOY_ROOT=deploy,
        RB_CONF_FILE=os.path.join(deploy, "rb.conf"),
        RB_RUN_DIR=run,
        RB_LOG_DIR=os.path.join(deploy, "log"),
        RB_PROC=proc,
        RB_SYSFS=os.path.join(root, "sys"),
        RB_RBP_LOG=os.path.join(deploy, "log", "rbp.log"),
        RB_HEALTH_LOG=os.path.join(root, "health.log"),
        RB_CHROOT=os.path.join(deploy, "rbx3-run"),
        RB_HOSTNAME_FILE=os.path.join(root, "hostname"),
        RB_CONF_REFRESH_S="0",
        PATH=shim + ":" + os.environ.get("PATH", ""),
        FAKE_LOG=os.path.join(root, "calls.log"),
        FAKE_UNITS=units,
        FAKE_THROTTLED=os.path.join(root, "throttled"),
    )
    with open(env["RB_HOSTNAME_FILE"], "w") as f:
        f.write("rpidev01\n")
    return env


def start_daemon(env, port):
    e = dict(env)
    e["RB_CONF_HTTP_PORT"] = str(port)
    e["RB_CONF_BIND"] = "127.0.0.1"
    return subprocess.Popen([sys.executable, CONFSCREEN], env=e,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def wait_up(port, seconds=8.0):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=1) as r:
                return r.read().decode("utf-8", "replace")
        except Exception:
            time.sleep(0.15)
    return None


def get(port, path="/"):
    """urlopen raises on 4xx/5xx, and several of these checks are *about* a refusal, so the
    error body is the payload here rather than an exception to let through."""
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=3) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def test_pure(m):
    """The parsers, without a socket in the way."""
    print("\n== pure readers ==")
    # 0x50005 is bits 18, 16, 2 and 0 -- throttled and under-voltage in BOTH halves. The
    # first version of this test read it as "under-voltage now, throttled once", which is
    # the kind of arithmetic slip the decoder exists to make impossible; hence the
    # single-bit cases below, which pin the bit-to-word mapping rather than one value.
    check(m.decode_throttled("0x1")["now"] == ["under-voltage"],
          "bit 0 => under-voltage NOW")
    check(m.decode_throttled("0x40000")["was"] == ["throttled"],
          "bit 18 => throttled has occurred")
    check(m.decode_throttled("0x40000")["now"] == [],
          "and bit 18 is not a current condition")
    check(m.decode_throttled("0x50005")["now"] == ["under-voltage", "throttled"],
          "0x50005 => under-voltage AND throttled now")
    check(m.decode_throttled("0x50005")["was"] == ["under-voltage", "throttled"],
          "and both as having occurred")
    check(m.decode_throttled("0x0")["now"] == [], "0x0 is nothing current")
    check(m.decode_throttled("not hex") is None, "garbage is None, not a crash")

    root = tempfile.mkdtemp(prefix="confscreen-pure.")
    try:
        env = make_fake(root)
        m.PROC = env["RB_PROC"]
        m.RBP_LOG = env["RB_RBP_LOG"]
        m.CHROOT = env["RB_CHROOT"]
        m.PLAYER = "/root/pdj/rbp"
        check(m.rbp_pid() == 4242, "rbp is found by its loader cmdline")
        check(m.rbp_pid() != 4001, "edb_streamd is NOT mistaken for the player")
        check(m.cpu_ticks(4242) == 300, "utime+stime read past the (comm) field")
        check(m.frame_count() == 40, "the frame counter counts DS_HW lines")
        check(m.player_depth() == 32, "the layer-format word reads as 32 bpp")

        with open(os.path.join(env["RB_CHROOT"], "root/pdj/rbp"), "r+b") as f:
            f.seek(0x19BAB8)
            f.write(b"\x01")
            f.seek(0x19BAC0)
            f.write(b"\x20")
        check(m.player_depth() == 16, "the 16-bpp word reads as 16")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_page(env, port):
    print("\n== the page ==")
    body = wait_up(port)
    check(body is not None, "the page answers")
    if body is None:
        return
    check("rpidev01" in body, "the hostname is on it")
    check("rbp is running and painting" in body or "too soon to tell" in body,
          "the player section states the player's condition in words")
    check("4242" in body, "the player's pid is shown")
    check("frames drawn" in body, "the frame count is shown")
    check("rblive4-vnc" in body and "inactive" in body, "service states are shown")
    check("pre-warmed rbp files" in body, "the launcher's own stage log is shown")
    check("launching rbp" in body, "the last boot-screen stage is shown")
    check("50.1" in body, "the SoC temperature is shown")
    check("under-voltage" in body, "the throttle bits are decoded into words")
    check("yes" in body, "the depth pair is reported as agreeing")
    check("RB_FB_LIE_BPP" in body, "the settings are listed")
    check("7" in body, "the rb.local.conf override is resolved (RB_VNC_FPS=7)")
    check("read-only" in body, "the page says it is read-only")

    print("\n== the frame counter decides the state ==")
    frames0 = "running and painting" in body or "too soon" in body
    check(frames0, "first poll: no rate yet, so it does not claim to know")
    log = env["RB_RBP_LOG"]
    time.sleep(0.12)
    with open(log, "a") as f:
        for _ in range(20):
            f.write(".../plugins/driver/DS_HW_Glib3_DFB.c <1106>:\n")
    _s, body = get(port)
    check("running and painting" in body, "frames arriving => 'painting'")
    time.sleep(0.12)
    _s, body = get(port)
    check("drawing NOTHING" in body, "frames stopped => 'running and drawing NOTHING'")

    print("\n== no write path, and a refusal that is not a handler check ==")
    req = urllib.request.Request("http://127.0.0.1:%d/" % port, data=b"x=1", method="POST")
    try:
        urllib.request.urlopen(req, timeout=3)
        check(False, "POST should be refused")
    except urllib.error.HTTPError as e:
        check(e.code == 405, "POST / is 405 (this landing has no write path)")
    _s, body = get(port, "/nope")
    check("no such page" in body, "an unknown path 404s")

    print("\n== 405 must not have touched anything ==")
    check(not os.path.exists(os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf.new")),
          "no temp file was created")
    with open(os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")) as f:
        check(f.read() == "# machine-local\nRB_VNC_FPS=7\n",
              "the config file is byte-identical after every request")


def test_states(root, port):
    """A player that is not running, a depth pair that disagrees, and a power reading that
    is the one value `lstrip('0x')` mangles -- '0x0' strips to nothing, and the page then
    printed 'get_throttled 0x'. The fixture above uses 0x50005, which lstrip happens to
    survive, so that bug needed THIS value to show up."""
    print("\n== a unit that is misbehaving ==")
    env = make_fake(root, pid=5150, depth=(0x01, 0x20))
    with open(env["FAKE_THROTTLED"], "w") as f:
        f.write("throttled=0x0\n")
    shutil.rmtree(os.path.join(env["RB_PROC"], "5150"))
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page still answers with no player at all")
        if body:
            check("rbp is NOT running" in body, "and says so")
            check("NO &mdash; rbp will not start" in body or "NO —" in body,
                  "a disagreeing depth pair is called out as fatal")
            check("get_throttled 0x0" in body,
                  "a zero throttle reading prints as 0x0, not as a bare 0x")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_bind_failure(env, port):
    """The port is taken. A page that is silently absent is worse than one that refuses."""
    print("\n== the port is already taken ==")
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(1)
    try:
        p = start_daemon(env, port)
        out, _ = p.communicate(timeout=8)
        check(p.returncode != 0, "it exits non-zero rather than pretending to serve")
        check("cannot bind" in (out or ""), "and says why")
    finally:
        s.close()


def main():
    m = load_module()

    test_pure(m)

    root = tempfile.mkdtemp(prefix="confscreen-test.")
    daemon = None
    try:
        port = free_port()
        env = make_fake(root)
        daemon = start_daemon(env, port)
        test_page(env, port)

        test_states(tempfile.mkdtemp(prefix="confscreen-states."), free_port())
        # A port of its own: the daemon above is still holding the one it was given.
        test_bind_failure(env, free_port())
    finally:
        if daemon:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
        shutil.rmtree(root, ignore_errors=True)

    print()
    if _fails:
        print("%d FAILED of %d checks:" % (len(_fails), _checks))
        for f in _fails:
            print("  - %s" % f)
        return 1
    print("all checks passed (%d)" % _checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
