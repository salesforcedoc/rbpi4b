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
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
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
# every argv it is handed, so the test can assert that nothing ever names the player path --
# and that a save never restarts or enables anything.
printf '%s\n' "$(basename "$0") $*" >> "$FAKE_LOG"
case "$(basename "$0")" in
  systemctl)
    verb=$1
    for a in "$@"; do unit=$a; done     # the unit is the LAST argument, not the second
    case "$verb" in
      is-active)  cat "$FAKE_UNITS/$unit.active"  2>/dev/null || echo inactive ;;
      is-enabled) cat "$FAKE_UNITS/$unit.enabled" 2>/dev/null || echo disabled ;;
      show)       cat "$FAKE_UNITS/$unit.since"   2>/dev/null || echo 0 ;;
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
                ": \"${RB_PASSWORD:=password}\"\n"
                "# The fixture OPTS IN to the page's password, so the auth suites have a lock\n"
                "# to test against. The SHIPPED default is 0 (open) -- pinned by\n"
                "# test_default_open, and by test_unreadable_conf for the case where the conf\n"
                "# cannot be read at all.\n"
                ": \"${RB_CONF_AUTH:=1}\"\n"
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
    # AFTER the `(comm)`, so index 0 here is file field 4 (state is written above): utime
    # (field 14) is index 10, stime (15) index 11, and starttime (22) index 18 -- which is
    # what rbp's own uptime is derived from.
    st = [0] * 20
    st[10], st[11], st[18] = 100, 200, 1000
    with open(os.path.join(proc, str(pid), "stat"), "wb") as f:
        f.write(b"%d (rbp) S " % pid + b" ".join(str(v).encode() for v in st))
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
    for u, since in (("rblive4", "4000000"), ("rblive4-vnc", "5000000"),
                     ("rblive4-boot", "3000000")):
        with open(os.path.join(units, u + ".since"), "w") as f:
            f.write(since + "\n")

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
        RB_LOCAL_CONF=os.path.join(deploy, "rb.local.conf"),
        RB_HOSTNAME_FILE=os.path.join(root, "hostname"),
        RB_CONF_REFRESH_S="5",
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


def get_with(port, path, cookie):
    """(status, body), discarding the headers -- for a GET that carries a session."""
    status, body, _h = _open(urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, path)), cookie)
    return status, body


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Let a 303 arrive AS a 303. urllib follows redirects by default, which would hide both
    the status and the Set-Cookie carrying the session -- the two things these checks are
    about."""

    def redirect_request(self, *a, **k):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def _open(req, cookie=None):
    if cookie:
        req.add_header("Cookie", cookie)
    try:
        with _OPENER.open(req, timeout=3) as r:
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)


def post(port, path, data, cookie=None, origin=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 data=urllib.parse.urlencode(data).encode(), method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    if origin:
        req.add_header("Origin", origin)
    return _open(req, cookie)


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
        check("RB_VNC_WEB" in m.conf_keys(),
              "and conf_keys() covers the schema, so a key it added cannot render as "
              "(empty) while the unit has it set")
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
    print("\n== the settings, as values an unsigned visitor may read ==")
    vnc = body.split("section id=vncsettings")[1].split("</section>")[0]
    rbp = body.split("section id=rbpsettings")[1].split("</section>")[0]
    check("RB_VNC_FPS" in vnc and "RB_PREWARM" in rbp and "RB_VNC_FPS" not in rbp,
          "each key is listed in its own group")
    check("action=/set" in vnc and "action=/set" in rbp,
          "and the forms are simply there -- there is no visitor state for the page to "
          "withhold a control over")
    check("sign in" not in vnc.lower() and "sign in" not in rbp.lower(),
          "and nothing anywhere says to sign in, because there is nothing to sign in to")
    lie_row = [r for r in rbp.split("<tr>") if "RB_FB_LIE_BPP" in r][0]
    check("read-only" in lie_row and "class=tip" in lie_row,
          "the depth pair's lie is read-only, with its reason on the (i)")
    check("<div class=note>The depth pair" not in rbp,
          "and the depth-pair paragraphs are gone from the visible page too")
    check("Shown and never editable" in rbp and "class=tip" in rbp,
          "while their text is still in the DOM, inside a tip")
    check("class=info" in rbp and "class=tip" in rbp,
          "descriptions sit behind an (i) instead of eating the label column")
    check('pair with the patched player' in rbp,
          "and the text is still in the DOM -- a hidden tip is not a lost description")

    print("\n== the refresh re-reads the DATA, it does not reload the page ==")
    check("http-equiv=refresh" not in body, "there is no meta refresh anywhere")
    check("/data" in body and "<script>" in body,
          "the page's own script fetches /data instead")
    check("id=data1" in body and "id=data2" in body, "the live data sits in two regions")
    check("getElementById(current)" in body and "data1', 'data2'" not in body,
          "and the poll replaces ONLY the section being looked at -- swapping the two regions "
          "whole re-created every section, and the hidden ones came back visible for an "
          "instant on every tick, which is what made it look like a page reload")
    check("running for" in body and "20m 24s" in body,
          "and the player section says how long rbp has been RUNNING (starttime, not the "
          "box's uptime)")

    print("\n== the sections are tabs down the left ==")
    # The NAV only: the page also carries in-content links (#actions, from the services
    # screen's sign-in hint), and counting those would be counting the wrong thing.
    nav = body.split("<nav id=nav>")[1].split("</nav>")[0]
    anchors = re.findall(r'href="#([a-z0-9]+)"', nav)
    check(anchors == ["player", "services", "launcher", "viewer", "vncsettings",
                      "rbpsettings"],
          "the nav lists every section, in page order (%s)" % ", ".join(anchors))
    missing = [a for a in anchors if ("section id=%s" % a) not in body]
    check(not missing,
          "and each one is a <section>, so the nav can show it alone (%s)"
          % (missing or "none"))
    pl = body.split("section id=player")[1].split("</section>")[0]
    check("device info" in pl and "SoC temperature" in pl and "get_throttled" in pl,
          "and the device's own readings are inside the player tab, under `device info`")
    check("section id=unit" not in body and "#unit" not in body,
          "with no unit section or tab left anywhere")
    check("<footer>" not in body, "and no footer -- the page names no doc to go and read")
    check("<nav id=nav>" in body, "the nav is a real element")
    check("classList" in body, "and the script marks which one you are looking at")

    print("\n== settings, split in two ==")
    vnc = body.split("section id=vncsettings")[1].split("</section>")[0]
    rbp = body.split("section id=rbpsettings")[1].split("</section>")[0]
    check("RB_VNC_FPS" in vnc and "RB_PASSWORD" in vnc,
          "vnc settings holds the viewer's own knobs")
    check("RB_VNC_FPS" not in rbp, "and the viewer's knobs are not in rbp settings")
    check("RB_PREWARM" in rbp and "RB_POINT_KIND" in rbp,
          "rbp settings holds the player's")
    check("depth pair agrees" in rbp, "including the depth pair, which is the player's build")

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

    print("\n== a state change is never a GET, and unknown paths are not actions ==")
    for p in ("/share", "/mode", "/restart", "/service", "/set"):
        s, b = get(port, p)
        check(s == 405, "GET %s is 405 (refused by method, not by a handler's check)" % p)
    for p in ("/login", "/logout", "/vnc"):
        s, b = get(port, p)
        check(s == 404, "GET %s is 404 -- a retired endpoint is gone, not hidden" % p)
    req = urllib.request.Request("http://127.0.0.1:%d/nonsense" % port, data=b"x=1",
                                 method="POST")
    try:
        urllib.request.urlopen(req, timeout=3)
        check(False, "POST to an unknown path should not be accepted")
    except urllib.error.HTTPError as e:
        check(e.code == 404, "POST to an unknown path 404s")
    _s, body = get(port, "/nope")
    check("no such page" in body, "an unknown path 404s")

    print("\n== refusals must not have touched anything ==")
    check(not os.path.exists(os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf.new")),
          "no temp file was created")
    with open(os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")) as f:
        check(f.read() == "# machine-local\nRB_VNC_FPS=7\n",
              "the config file is byte-identical after every request so far")
    check(os.path.exists(os.path.join(env["RB_RUN_DIR"], "vnc.live"))
          and open(os.path.join(env["RB_RUN_DIR"], "vnc.live")).read() == "on\n",
          "and the switch file the fixture wrote is unchanged")


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


def test_data_fragment(env, port):
    """The poll must return DATA, not a document. If it returned the whole page the browser
    would be swapping in a second <html> -- and, worse, a second copy of the sign-in form,
    which is the very thing a reload was eating."""
    print("\n== the fragment the poll fetches ==")
    s, frag = get(port, "/data")
    check(s == 200, "GET /data answers")
    check("<html" not in frag and "<script" not in frag,
          "and is a fragment, not a document: no <html>, no script")
    check("rbp is" in frag, "it carries the player's own state words")
    check("frames drawn" in frag and "running for" in frag,
          "the frame counter and how long rbp has been running")
    check("name=pw" not in frag and "name=csrf" not in frag,
          "and none of the write chrome: re-fetching a half-typed field is exactly what the "
          "poll exists to avoid")
    check("action=/share" in frag and "action=/mode" in frag,
          "and the viewer's switch buttons DO ride along: they are content, in the viewer "
          "section, with nothing half-typed to lose")
    check('id=data1' in frag and 'id=data2' in frag,
          "both live halves, under the ids the script swaps")
    check("id=nav" not in frag and "<nav" not in frag,
          "and NO nav in it: the poll must never inject a second one")
    check("id=player" in frag and "id=rbpsettings" in frag,
          "while the section ids it does carry keep the left-hand links working after a "
          "swap")

    print("\n== and fetching it changes nothing ==")
    conf = os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")
    before = open(conf).read()
    for _ in range(3):
        get(port, "/data")
    check(open(conf).read() == before, "the config is untouched by polling")
    # `--now` is what separates a mutation from a read: the fragment does call
    # `systemctl is-active`/`is-enabled` on every unit, which is the point of it.
    log = open(env["FAKE_LOG"]).read() if os.path.exists(env["FAKE_LOG"]) else ""
    check("--now" not in log, "and nothing was enabled or disabled")


def test_write_path(env, port):
    """The write path through real HTTP: two actions that must each change exactly what they
    say and NOTHING else. The config file is checked before and after by content, because the
    whole promise of this page is that it edits the operator's file rather than regenerating it.

    THERE IS NO SIGN-IN TO TEST ANY MORE. No password, no session, no CSRF token -- so what used
    to be three suites of sign-in checks collapses into "a write needs nothing but the form".
    Two things still have to hold and are checked here: a POST that says it came from somewhere
    else is refused, and the page does not pretend to be locked."""
    print("\n== no sign-in: a write needs nothing but the form ==")
    conf = os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")
    calls = env["FAKE_LOG"]
    before = open(conf).read()
    if os.path.exists(calls):
        os.unlink(calls)

    live = os.path.join(env["RB_RUN_DIR"], "vnc.live")
    live_before = open(live).read() if os.path.exists(live) else None
    s, _b, _h = post(port, "/share", {"action": "wibble"})
    check(s == 400, "an action that is neither on nor off is 400")
    check((open(live).read() if os.path.exists(live) else None) == live_before,
          "and a rejected request changed nothing")
    check(open(conf).read() == before, "nor touched the config")

    _s, page = get(port, "/")
    check("name=csrf" not in page and "name=pw" not in page,
          "the page renders no CSRF field and no password field anywhere -- there is no "
          "session to hold one and no sign-in to fill one")
    check("Sign in" not in page and "Sign out" not in page,
          "and no sign-in form or sign-out button")
    check("no password" not in page and "unprotected" not in page,
          "and the page says nothing about a lock at all -- the notice was taken off")
    check("section id=actions" not in page and "#actions" not in page,
          "and the actions section is gone entirely")

    print("\n== the two live switches carry their own buttons ==")
    vsec = page.split("section id=viewer")[1].split("</section>")[0]
    check("action=/share" in vsec, "sharing's on/off sits on the sharing row")
    check("action=/mode" in vsec and "value=hwjpeg" in vsec,
          "and raw/hwjpeg sits on the encoding row")
    check("action=/vnc" not in vsec and "Enable the viewer" not in vsec,
          "with no enable/disable-the-viewer row: that is the services table's job now")
    check("viewer settings" in vsec, "the section is titled viewer settings")
    check('href="#viewer">viewer settings<' in page, "as is its nav entry")

    print("\n== the settings are forms, and the read-only ones are not ==")
    sv = page.split("section id=vncsettings")[1].split("</section>")[0]
    sr = page.split("section id=rbpsettings")[1].split("</section>")[0]
    check("action=/set" in sv and "action=/set" in sr, "both groups carry save forms")
    check('name=key value="RB_VNC_FPS"' in sv,
          "addressed by key, so one handler serves every setting")
    check("picked up when rblive4-vnc next starts" in sv,
          "and the group names the unit a saved value waits for -- said ONCE, not on every "
          "row")
    check('href="#services"' in sv, "pointing at the buttons that do it")
    check("<code>RB_VNC_FPS</code>" in sv,
          "the key name rides in the (i) beside the label, so hand-editing the file is still "
          "possible without printing it on twenty rows")
    check("RB_VNC_FPS = " not in sv,
          "and no `KEY = value` line doubling what the control already displays")
    check("class=info" in sv and "class=tip" in sv,
          "the descriptions are (i) tooltips, not visible paragraphs")
    lie_row = [r for r in sr.split("<tr>") if "RB_FB_LIE_BPP" in r][0]
    check("action=/set" not in lie_row,
          "the read-only ones are STILL offered no form -- the writer would refuse them")

    print("\n== the sharing switch, which is a button and nothing else ==")
    live = os.path.join(env["RB_RUN_DIR"], "vnc.live")
    s, _b, _h = post(port, "/share", {"action": "on"})
    check(s == 303, "starting sharing redirects")
    check(os.path.exists(live) and open(live).read() == "on\n",
          "and the switch file the viewer re-reads now says on")
    _s, page = get(port, "/")
    check("value=on class=cur>on<" in page,
          "and the page shows sharing on -- the pair draws the value in force as pressed")
    s, _b, _h = post(port, "/share", {"action": "off"})
    check(s == 303, "stopping it redirects")
    check(open(live).read() == "off\n", "and the file says off")
    s, _b, _h = post(port, "/share", {"action": "wibble"})
    check(s == 400, "and anything that is not on or off is 400")
    check(open(live).read() == "off\n", "with the file untouched")

    print("\n== the encoding switch, same shape ==")
    mode = os.path.join(env["RB_RUN_DIR"], "vnc.mode")
    s, _b, _h = post(port, "/mode", {"value": "hwjpeg"})
    check(s == 303, "choosing hwjpeg redirects")
    check(open(mode).read() == "hwjpeg\n", "and the mode file the viewer re-reads says so")
    _s, page = get(port, "/")
    check("value=hwjpeg class=cur>hwjpeg<" in page, "which the page shows the same way")
    s, _b, _h = post(port, "/mode", {"value": "raw"})
    check(s == 303 and open(mode).read() == "raw\n", "and back to raw")
    s, _b, _h = post(port, "/mode", {"value": "tiff; rm -rf /"})
    check(s == 400, "while a value that is neither is 400 -- the words are a whitelist")
    check(open(mode).read() == "raw\n", "and the file is untouched")

    print("\n== pointer injection, the one that changes the glass ==")
    inp = os.path.join(env["RB_RUN_DIR"], "vnc.input")
    s, _b, _h = post(port, "/input", {"value": "off"})
    check(s == 303, "turning pointer injection off redirects")
    check(open(inp).read() == "off\n", "and the switch file says off")
    _s, page = get(port, "/")
    check("action=/input" in page, "the row carries its own on/off")
    check("input surface" in page and "USB STOP" in page,
          "and the page says what it does to the glass, USB STOP included")
    s, _b, _h = post(port, "/input", {"value": "yes"})
    check(s == 400, "while a value that is neither on nor off is 400")
    check(open(inp).read() == "off\n", "with the file untouched")

    print("\n== a missing switch file shows the DEFAULT, not '(absent)' ==")
    # This is the state this unit was actually in: no vnc.input file, RB_VNC_INPUT=1, and the
    # page saying "(absent)" -- which told the reader nothing, least of all that their screen
    # was clickable.
    os.unlink(inp)
    with open(env["RB_LOCAL_CONF"], "a") as f:
        f.write("RB_VNC_INPUT=1\n")
    _s, page = get(port, "/")
    vsec = page.split("section id=viewer")[1].split("</section>")[0]
    check("(absent)" not in vsec,
          "no row reads '(absent)' -- it said nothing about what was in force")
    irow = [r for r in vsec.split("<tr>") if "pointer injection" in r][0]
    check("value=on class=cur>on<" in irow,
          "it shows the RB_VNC_INPUT default that is really in effect, as the pressed button")
    check("(default)" in vsec and "has no switch file" in vsec,
          "and marks it as the default, with the sentence that explains it")
    text = open(env["RB_LOCAL_CONF"]).read().replace("RB_VNC_INPUT=1\n", "")
    with open(env["RB_LOCAL_CONF"], "w") as f:      # read FIRST: open(..,"w") truncates
        f.write(text)

    print("\n== the viewer's own service is NOT on this page's switch rows ==")
    check("action=/vnc" not in page, "there is no enable/disable-the-viewer form left")
    s, _b, _h = post(port, "/vnc", {"action": "on"})
    check(s == 404, "and the endpoint is gone rather than hidden")

    print("\n== and the one lock that is left ==")
    s, _b, _h = post(port, "/share", {"action": "on"}, origin="http://evil.example")
    check(s == 403, "a POST that says it came from another site is refused")


def test_no_password(root, port):
    """A unit with no RB_PASSWORD at all.

    This used to be the fail-closed suite: an empty password must not mean "no authentication",
    so every write was refused. There is no authentication to fail closed on any more -- the
    page has none at all -- and RB_PASSWORD is now only the VNC client's password, so an empty
    one is a fact about the VIEWER and nothing to do with this page. Which is exactly what is
    checked: the page keeps working, and the settings row says the password is not set."""
    print("\n== a unit with no RB_PASSWORD at all ==")
    env = make_fake(root)
    conffile = env["RB_CONF_FILE"]
    text = open(conffile).read().replace(': "${RB_PASSWORD:=password}"\n', "")
    with open(conffile, "w") as f:
        f.write(text)
    # The fixture writes the switch files; this unit must not CREATE any, so start from none.
    for n in ("live", "mode", "input"):
        try:
            os.unlink(os.path.join(env["RB_RUN_DIR"], "vnc." + n))
        except OSError:
            pass
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page still answers")
        if body:
            check("No password is configured" not in body,
                  "and does NOT refuse writes over it -- that was the sign-in era")
            check("(not set)" in body,
                  "while the password row reports the VNC password is not set")
        s, _b, _h = post(port, "/set", {"key": "RB_VNC_FPS", "value": "5"})
        check(s in (200, 303), "a setting can still be changed")
        check("RB_VNC_FPS=5" in open(env["RB_LOCAL_CONF"]).read(), "and it was written")
        check(not os.path.exists(os.path.join(env["RB_RUN_DIR"], "vnc.live")),
              "and none of it created a switch file")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_no_auth(root, port):
    """Writes with no password -- which is now the only way this page works, and said loudly on
    the page rather than left as a lock nobody notices is missing.

    The fixture also drops a LEFTOVER `RB_CONF_AUTH=0` into rb.local.conf, deliberately: units
    in the field carry that line, and a key the page no longer knows about has to be inert
    rather than fatal or visible."""
    print("\n== a unit whose conf still carries the retired RB_CONF_AUTH ==")
    env = make_fake(root)
    with open(env["RB_LOCAL_CONF"], "a") as f:
        f.write("RB_CONF_AUTH=0\n")
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page answers")
        if body:
            check("RB_CONF_AUTH" not in body,
                  "and the retired key is nowhere on it")
            check("name=pw" not in body and "name=csrf" not in body,
                  "with no sign-in machinery on it")
            check("action=/share" in body and "action=/mode" in body,
                  "with the viewer's switch buttons rendered")
            check("RB_CONF_AUTH" not in body,
                  "and the retired key is not shown as a setting")
        s, _b, _h = post(port, "/share", {"action": "on"})
        check(s == 303, "a write with no session and no CSRF value succeeds")
        live = os.path.join(env["RB_RUN_DIR"], "vnc.live")
        check(open(live).read() == "on\n", "and it wrote the switch the viewer reads")
        # The origin check STAYS ON: it costs the operator nothing and it stops another
        # website driving this unit through their browser.
        s, _b, _h = post(port, "/share", {"action": "off"}, origin="http://evil.example")
        check(s == 403, "but a cross-origin POST is still refused")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_restart(env, port):
    """The restart is TWO STEPS and it is serialised -- because two overlapping restarts wedge
    rbp on this unit, measured, and that is why this button was not built with the others."""
    print("\n== the restart button lives in the services table ==")
    calls = env["FAKE_LOG"]
    if os.path.exists(calls):
        os.unlink(calls)

    _s, page = get(port, "/")
    services = page.split("section id=services")[1].split("</section>")[0]
    check("action=/restart" in services and "name=step value=ask" in services,
          "the player's restart button is in its row in the services section")

    print("\n== it asks first ==")
    s, body, _h = post(port, "/restart", {"step": "ask"})
    check(s == 200, "the button answers with a question, not an action")
    check("restart the player?" in body, "which says what it will do")
    check("fifteen seconds" in body, "including that the screen goes dark")
    log = open(calls).read() if os.path.exists(calls) else ""
    check("restart rblive4" not in log, "and nothing has been restarted yet")

    print("\n== and only then restarts ==")
    s, _b, _h = post(port, "/restart", {"step": "go"})
    check(s == 303, "confirming restarts and redirects")
    log = open(calls).read() if os.path.exists(calls) else ""
    check("systemctl start rblive4-boot" in log and "restart rblive4" in log
          and log.index("rblive4-boot") < log.index("restart rblive4"),
          "the boot screen is started FIRST, in that order, so the restart is narrated "
          "like a boot")
    check("/root/pdj" not in log and "rbp -a" not in log,
          "with no command line naming the player path (the cmdline-kill trap)")

    print("\n== and a second one too soon is refused ==")
    s, _b, _h = post(port, "/restart", {"step": "go"})
    check(s == 303, "it redirects rather than acting")
    _s, page = get(port, "/")
    check("restart happened" in page and "wait" in page,
          "and the page says why: a restart just happened")
    check(open(calls).read().count("restart rblive4") == 1,
          "and systemd was NOT asked a second time")


def test_services(env, port):
    """A start/restart button on every service row -- and the two rules that make it safe:
    the unit is a WHITELIST LOOKUP, and the player is not in the generic branch.

    The unit name is the only value on this page that reaches systemctl as a variable, so it
    is the only place where a form field could become an argument. These checks exist to pin
    that down rather than to describe the markup."""
    print("\n== a start/restart button on every service row ==")
    calls = env["FAKE_LOG"]
    if os.path.exists(calls):
        os.unlink(calls)
    units_dir = env["FAKE_UNITS"]
    # One running and one stopped, so the two labels can be told apart.
    for u, state in (("rblive4-vnc", "active"), ("healthwatch", "active"),
                     ("rblive4-boot", "inactive"), ("rblive4-conf", "inactive")):
        with open(os.path.join(units_dir, u + ".active"), "w") as f:
            f.write(state)

    _s, page = get(port, "/")
    services = page.split("section id=services")[1].split("</section>")[0]
    for u in ("rblive4-boot", "healthwatch", "rblive4-conf", "rblive4-vnc",
              "rblive4-webvnc"):
        check(('name=unit value="%s"' % u) in services,
              "a row button posts the unit name: %s" % u)
    wrow = [r for r in services.split("<tr>") if ">rblive4-webvnc</td>" in r][0]
    check("action=/boot" in wrow,
          "and the web client's bridge has a row of its own -- it was the one service with no "
          "row at all")
    prow = [r for r in services.split("<tr>") if ">rblive4</td>" in r][0]
    check("action=/service" not in prow,
          "and the PLAYER has no /service form at all -- it keeps its own guarded path")
    check("action=/restart" in prow, "its row posts to the two-step confirm instead")

    print("\n== the unit name is a whitelist, not a string off the wire ==")
    s, _b, _h = post(port, "/service", {"unit": "sshd"})
    check(s == 400, "a unit that is not on this page is 400, not a polite redirect")
    log = open(calls).read() if os.path.exists(calls) else ""
    check("sshd" not in log,
          "and nothing ran systemctl for it -- an arbitrary unit name never reaches argv")
    for evil in ("../../etc/passwd; reboot", "rblive4; reboot", "rblive4-vnc ", ""):
        s, _b, _h = post(port, "/service", {"unit": evil})
        check(s == 400, "refused: %r" % evil)
    log = open(calls).read() if os.path.exists(calls) else ""
    check("passwd" not in log and "reboot" not in log,
          "and none of them reached a process")

    print("\n== a running unit is restarted, a stopped one is started ==")
    s, _b, _h = post(port, "/service", {"unit": "rblive4-vnc"})
    check(s == 303, "the running viewer redirects")
    s, _b, _h = post(port, "/service", {"unit": "rblive4-boot"})
    check(s == 303, "the stopped boot screen redirects")
    log = open(calls).read()
    check("systemctl restart rblive4-vnc" in log, "the running one was RESTARTED")
    check("systemctl start rblive4-boot" in log, "the stopped one was STARTED")
    check("/root/pdj" not in log and "rbp -a" not in log,
          "and no command line names the player path (the cmdline-kill trap)")
    _s, page = get(port, "/")
    check("rblive4-boot start requested" in page,
          "and the page flashes which service it acted on and which verb it chose")

    print("\n== the startup column carries the enable/disable ==")
    for u, st in (("rblive4-vnc", "disabled"), ("healthwatch", "enabled"),
                  ("rblive4", "enabled"), ("rblive4-conf", "enabled")):
        with open(os.path.join(units_dir, u + ".enabled"), "w") as f:
            f.write(st)
    _s, page = get(port, "/")
    services = page.split("section id=services")[1].split("</section>")[0]
    check("startup</th>" in services and "actions</th>" in services,
          "the two control columns are headed startup and actions")
    check("action=/boot" in services, "its cell carries the control")
    vrow = [r for r in services.split("<tr>") if ">rblive4-vnc</td>" in r][0]
    hrow = [r for r in services.split("<tr>") if ">healthwatch</td>" in r][0]
    check('value=on' in vrow and 'value=off' in vrow
          and '>enabled</button>' in vrow and '>disabled</button>' in vrow,
          "the cell shows BOTH states as buttons, not just the one a press would give")
    check('value=off class=cur>disabled<' in vrow,
          "and the state the unit is IN is drawn as pressed (the viewer is disabled)")
    check('value=on class=cur>enabled<' in hrow and 'value=off class=cur' not in hrow,
          "and an enabled unit has ENABLED pressed instead -- one pressed, one not")

    print("\n== two units can be enabled but never disabled ==")
    for u in ("rblive4", "rblive4-conf"):
        row = [r for r in services.split("<tr>") if (">%s</td>" % u) in r][0]
        check("action=/boot" in row, "the %s row shows the pair like every other" % u)
        offbtn = [b for b in row.split("<button") if "value=off" in b][0]
        onbtn = [b for b in row.split("<button") if "value=on" in b][0]
        check("disabled" in offbtn,
              "with its DISABLED button itself disabled -- the direction the page refuses")
        check("disabled" not in onbtn, "while its ENABLED button is live")
        check("class=info" in row and "class=tip" in row,
              "and the reason still rides on an (i)")
    s, _b, _h = post(port, "/boot", {"unit": "rblive4", "action": "off"})
    check(s == 400, "a request to disable the player is refused even so")
    check("disable rblive4" not in (open(calls).read() if os.path.exists(calls) else ""),
          "and systemd was never asked")
    s, _b, _h = post(port, "/boot", {"unit": "rblive4", "action": "on"})
    check(s == 303, "but ENABLING one of them is allowed -- that direction cannot lock "
                    "anyone out")

    print("\n== enable is not start ==")
    if os.path.exists(calls):
        os.unlink(calls)
    conf = os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")
    s, _b, _h = post(port, "/boot", {"unit": "rblive4-vnc", "action": "on"})
    check(s == 303, "enabling the viewer redirects")
    log = open(calls).read() if os.path.exists(calls) else ""
    check("systemctl enable rblive4-vnc" in log, "and systemd was asked to enable it")
    check("RB_VNC=1" in open(conf).read(),
          "and RB_VNC was recorded too -- install.sh re-applies this, so a bare enable would "
          "be undone by the next install")
    check("--now" not in log and "restart rblive4" not in log,
          "WITHOUT starting or stopping anything: enable writes the symlink and stops there")
    s, _b, _h = post(port, "/boot", {"unit": "rblive4-webvnc", "action": "off"})
    check(s == 303, "the web client can be disabled from its own row")
    check("systemctl disable rblive4-webvnc" in open(calls).read(),
          "which asks systemd to disable it")
    check("RB_VNC_WEB=0" in open(conf).read(),
          "and writes its install-time mirror, so an install cannot quietly undo it")
    s, _b, _h = post(port, "/boot", {"unit": "rblive4-vnc", "action": "wibble"})
    check(s == 400, "an action that is neither is 400")
    s, _b, _h = post(port, "/boot", {"unit": "sshd", "action": "on"})
    check(s == 400, "and a unit that is not on the page is 400")

    print("\n== this page's own unit is answered, never waited for ==")
    with open(os.path.join(units_dir, "rblive4-conf.active"), "w") as f:
        f.write("active")
    s, _b, _h = post(port, "/service", {"unit": "rblive4-conf"})
    check(s == 303, "restarting this page's own service still answers the request")
    for _ in range(60):                 # it is fired detached, so give it a moment
        if "restart rblive4-conf" in open(calls).read():
            break
        time.sleep(0.05)
    check("restart rblive4-conf" in open(calls).read(),
          "and it did ask systemd -- just without waiting to be killed by it")


def test_set(env, port):
    """Saving one setting, through the editor -- and the promise that a save changes ONE line
    of the operator's file and restarts nothing."""
    print("\n== saving a setting ==")
    conf = os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")
    if os.path.exists(env["FAKE_LOG"]):
        os.unlink(env["FAKE_LOG"])       # the log has the other suites' calls in it
    before = open(conf).read()
    _s, page = get(port, "/")
    check("action=/set" in page, "the save form is on the page with no sign-in at all")

    s, _b, _h = post(port, "/set", {"key": "RB_FB_LIE_BPP", "value": "16"})
    check(s == 303, "a read-only key is refused (with a message, see below)")
    _s, page = get(port, "/")
    check("pair" in page and "FAILED" in page, "and the page says why")
    check(open(conf).read() == before, "the file is untouched by all of that")

    print("\n== values the schema refuses ==")
    for bad in ("999", "4; rm -rf /", "$(reboot)", "`id`"):
        post(port, "/set", {"key": "RB_VNC_FPS", "value": bad})
    check(open(conf).read() == before,
          "an out-of-range value and three injections leave the file BYTE-IDENTICAL")
    s, _b, _h = post(port, "/set", {"key": "RB_PASSWORD", "value": ""})
    check(s == 303, "a blank password field is accepted as 'leave it alone'")
    check(open(conf).read() == before, "and changes nothing")

    print("\n== saving for real ==")
    s, _b, _h = post(port, "/set", {"key": "RB_VNC_FPS", "value": "9"})
    check(s == 303, "a valid save redirects")
    after = open(conf).read()
    # Relative to what the file was, not to a pristine one: the earlier suites have already
    # written to it, and the promise being checked is "one value, everything else identical".
    check(after == before.replace("RB_VNC_FPS=7", "RB_VNC_FPS=9"),
          "and the file is EXACTLY as it was with one value changed")
    check(os.path.exists(conf + ".prev"), "a one-generation backup was kept")
    _s, page = get(port, "/")
    check("saved RB_VNC_FPS=9" in page, "the page says what it saved")
    check("restart the viewer to apply" in page, "and what applies it")
    check("saved, not applied yet" in page, "and marks the group as not yet applied")

    print("\n== and nothing was restarted to do it ==")
    log = open(env["FAKE_LOG"]).read() if os.path.exists(env["FAKE_LOG"]) else ""
    check("--now" not in log, "a save enables and disables nothing")
    check("restart rblive4" not in log, "and restarts nothing")

    print("\n== the marker is discharged by the unit starting again ==")
    with open(os.path.join(env["FAKE_UNITS"], "rblive4-vnc.since"), "w") as f:
        f.write("9000000\n")            # ActiveEnterTimestampMonotonic moved: it restarted
    _s, page = get(port, "/")
    check("saved, not applied yet" not in page,
          "and the page stops saying so once the viewer has restarted")


def test_default_open(root, port):
    """The SHIPPED default, which is now the ONLY behaviour: no password for configuration. A
    configuration screen you have to sign in to is one you stop using -- and the operator
    retired the opt-in rather than leaving it off.

    Note what is NOT here: a conf carrying the retired `RB_CONF_AUTH=1`. It is set to 1 in the
    fixture on purpose, because a unit that was locked in the field will still have it, and the
    page must ignore it rather than start demanding a password it has no way to take."""
    print("\n== the shipped default: no password, and no opt-in to one ==")
    env = make_fake(root)          # the fixture leaves RB_CONF_AUTH=1 in the conf, on purpose
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page answers")
        if body:
            check("name=pw" not in body, "and asks for nothing")
            check("This page has" not in body,
                  "and carries no notice about itself")
            check("RB_CONF_AUTH" not in body,
                  "even though the conf still says RB_CONF_AUTH=1 -- the key is retired")
        s, _b, _h = post(port, "/set", {"key": "RB_VNC_FPS", "value": "6"})
        check(s in (200, 303), "a setting can be changed with no session at all")
        check("RB_VNC_FPS=6" in open(env["RB_LOCAL_CONF"]).read(), "and it was written")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_unreadable_conf(root, port):
    """THE CASE THE OPERATOR ACTUALLY HIT, kept because the lesson outlived the code.

    A conf that cannot be read does not yield a MISSING value, it yields an EMPTY one -- and
    the sign-in this page used to have tested for an explicit `=1` precisely because the first
    version tested `!= "0"` and turned that empty value into a sign-in form, making a card
    failure look like a policy. The sign-in is gone; what is still worth pinning is that a conf
    the page cannot read leaves it answering and honest rather than inventing a control."""
    print("\n== a conf the page cannot read ==")
    env = make_fake(root)
    with open(env["RB_CONF_FILE"], "w") as f:
        f.write("if [ 1 = 1; then\n")        # sourcing this fails: every value comes back empty
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page still answers when the conf cannot be read")
        if body:
            check("name=pw" not in body,
                  "and does NOT invent a sign-in form out of the failure")
            check("class=v>(empty)</td>" in body,
                  "while the values it cannot read are visibly empty -- not blank, and not "
                  "invented")
    finally:
        p.terminate()
        p.wait(timeout=5)


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
        test_data_fragment(env, port)
        test_write_path(env, port)
        test_set(env, port)
        test_restart(env, port)
        test_services(env, port)

        test_states(tempfile.mkdtemp(prefix="confscreen-states."), free_port())
        test_no_password(tempfile.mkdtemp(prefix="confscreen-nopass."), free_port())
        test_no_auth(tempfile.mkdtemp(prefix="confscreen-noauth."), free_port())
        test_default_open(tempfile.mkdtemp(prefix="confscreen-default."), free_port())
        test_unreadable_conf(tempfile.mkdtemp(prefix="confscreen-noconf."), free_port())
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
