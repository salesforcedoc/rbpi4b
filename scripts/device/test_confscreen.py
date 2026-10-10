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
                ": \"${RB_PASSWORD:=password}\"\n"
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


def cookie_of(headers):
    sc = headers.get("Set-Cookie", "")
    return sc.split(";", 1)[0] if sc else None


def csrf_of(body):
    m = re.search(r'name=csrf value="([0-9a-f]+)"', body)
    return m.group(1) if m else None


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
    check("shown read-only" in body,
          "the settings are shown read-only (the form is the next slice)")

    print("\n== the refresh re-reads the DATA, it does not reload the page ==")
    check("http-equiv=refresh" not in body, "there is no meta refresh anywhere")
    check("/data" in body and "<script>" in body,
          "the page's own script fetches /data instead")
    check("id=data1" in body and "id=data2" in body, "and swaps both live halves by id")
    check("running for" in body and "20m 24s" in body,
          "and the player section says how long rbp has been RUNNING (starttime, not the "
          "box's uptime)")

    print("\n== the sections are navigable from the left ==")
    anchors = re.findall(r'href="#([a-z0-9]+)"', body)
    check(len(anchors) >= 6, "the nav links the sections (%s)" % ", ".join(anchors))
    missing = [a for a in anchors if ("id=%s" % a) not in body]
    check(not missing, "and every link has a target on the page (%s)" % (missing or "none"))
    check("<nav id=nav>" in body, "the nav is a real element, before the content")

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
    for p in ("/login", "/logout", "/vnc", "/share"):
        s, b = get(port, p)
        check(s == 405, "GET %s is 405 (refused by method, not by a handler's check)" % p)
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
    check("action=/login" not in frag and "Sign in" not in frag,
          "but NOT the sign-in form -- re-fetching that is what ate a half-typed password")
    check("action=/vnc" not in frag, "and not the buttons either")
    check('id=data1' in frag and 'id=data2' in frag,
          "both live halves, under the ids the script swaps")
    check("id=nav" not in frag and 'href="#' not in frag,
          "and NO nav in it: the poll must never inject a second one")
    check("id=player" in frag and "id=settings" in frag,
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
    """The write path through real HTTP: a session, a CSRF value, and two actions that must
    each change exactly what they say and NOTHING else. The config file is checked before
    and after by content, because the whole promise of this page is that it edits the
    operator's file rather than regenerating it."""
    print("\n== signing in ==")
    conf = os.path.join(env["RB_DEPLOY_ROOT"], "rb.local.conf")
    calls = env["FAKE_LOG"]
    before = open(conf).read()
    if os.path.exists(calls):
        os.unlink(calls)

    s, _b, _h = post(port, "/vnc", {"action": "on"})
    check(s == 401, "a write with no session is 401")
    s, _b, _h = post(port, "/login", {"pw": "wrong"})
    check(s == 401, "a wrong password is 401")
    check(not os.path.exists(calls), "and nothing ran systemctl")
    check(open(conf).read() == before, "and the config is untouched")

    s, _b, h = post(port, "/login", {"pw": "password"})
    check(s == 303, "the right password redirects (303)")
    cookie = cookie_of(h)
    check(bool(cookie) and cookie.startswith("rblive4conf="), "and sets a session cookie")
    setcookie = h.get("Set-Cookie", "")
    check("HttpOnly" in setcookie and "SameSite=Strict" in setcookie,
          "which is HttpOnly and SameSite=Strict")

    _s, page = get_with(port, "/", cookie)
    csrf = csrf_of(page)
    check(bool(csrf), "the page then renders a CSRF value into its forms")
    check("Signed in" in page and "Sign out" in page, "and says it is signed in")
    check("Enable the viewer" in page and "Start sharing" in page, "and offers both actions")
    check("signed in -- the actions below" in page, "and shows the flash from the login")

    print("\n== the CSRF value is not decoration ==")
    s, _b, _h = post(port, "/vnc", {"action": "on", "csrf": "0" * 32}, cookie=cookie)
    check(s == 403, "a form with the WRONG csrf value is 403")
    s, _b, _h = post(port, "/vnc", {"action": "on"}, cookie=cookie)
    check(s == 403, "a form with NO csrf value is 403 too")
    s, _b, _h = post(port, "/vnc", {"action": "wibble", "csrf": csrf}, cookie=cookie)
    check(s == 400, "an action that is neither on nor off is 400")

    print("\n== enabling the viewer ==")
    s, _b, _h = post(port, "/vnc", {"action": "on", "csrf": csrf}, cookie=cookie)
    check(s == 303, "enabling redirects")
    after = open(conf).read()
    check("RB_VNC=1" in after, "RB_VNC=1 is in rb.local.conf")
    check("# machine-local" in after and "RB_VNC_FPS=7" in after,
          "and the operator's own lines are intact -- the page edited the file, it did not "
          "regenerate it")
    logged = open(calls).read() if os.path.exists(calls) else ""
    check("systemctl enable --now rblive4-vnc" in logged,
          "and systemd was asked to enable and start the viewer")
    check("/root/pdj" not in logged and "rbp -a" not in logged,
          "with no command line naming the player path (the cmdline-kill trap)")
    _s, page = get_with(port, "/", cookie)
    check("the viewer is enabled" in page, "and the page reports what it did")

    print("\n== the sharing switch, which needs no restart ==")
    s, _b, _h = post(port, "/share", {"action": "on", "csrf": csrf}, cookie=cookie)
    check(s == 303, "starting sharing redirects")
    live = os.path.join(env["RB_RUN_DIR"], "vnc.live")
    check(os.path.exists(live) and open(live).read() == "on\n",
          "and the switch file the viewer re-reads now says on")
    _s, page = get_with(port, "/", cookie)
    check("sharing (vnc.live)</td><td>on" in page, "and the page shows sharing on")

    print("\n== disabling it takes the switch with it ==")
    s, _b, _h = post(port, "/vnc", {"action": "off", "csrf": csrf}, cookie=cookie)
    check(s == 303, "disabling redirects")
    after = open(conf).read()
    check("RB_VNC=0" in after and "RB_VNC=1" not in after, "RB_VNC is now 0")
    check(open(live).read() == "off\n",
          "and the sharing switch is off too, so a later enable cannot come up already "
          "serving")
    check("systemctl disable --now rblive4-vnc" in open(calls).read(), "and it was disabled")

    print("\n== a POST that says it came from somewhere else ==")
    s, _b, _h = post(port, "/vnc", {"action": "on", "csrf": csrf}, cookie=cookie,
                     origin="http://evil.example")
    check(s == 403, "a cross-origin POST is refused")

    print("\n== and the rate limit ==")
    codes = [post(port, "/login", {"pw": "nope"})[0] for _ in range(7)]
    check(codes[0] == 401, "a wrong password is still 401 to begin with")
    check(429 in codes and codes[-1] == 429,
          "and repeated guesses are refused with 429 (%s)" % codes)


def test_no_password(root, port):
    """FAIL CLOSED. An empty password must never mean 'no authentication': it means the
    unit has no credential, and nothing is written."""
    print("\n== a unit with no password at all ==")
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
            check("No password is configured" in body,
                  "and says writes are refused rather than allowed")
        s, _b, _h = post(port, "/login", {"pw": ""})
        check(s == 503, "a sign-in with no password configured is 503")
        s, _b, _h = post(port, "/login", {"pw": "password"})
        check(s == 503, "and so is any other password")
        check(not os.path.exists(os.path.join(env["RB_RUN_DIR"], "vnc.live")),
              "and no switch file was created")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_no_auth(root, port):
    """`RB_CONF_AUTH=0`: writes with no password -- and SAID LOUDLY, on the page and in
    doctor.sh, rather than left as a lock nobody notices is missing."""
    print("\n== a unit that asks for no password ==")
    env = make_fake(root)
    with open(env["RB_LOCAL_CONF"], "a") as f:
        f.write("RB_CONF_AUTH=0\n")
    p = start_daemon(env, port)
    try:
        body = wait_up(port)
        check(body is not None, "the page answers")
        if body:
            check("unprotected" in body, "and says the writes are unprotected")
            check("Nothing to sign in to" in body, "and offers nothing to sign in to")
            check("action=/login" not in body, "no login form at all")
            check("Enable the viewer" in body, "with the buttons rendered")
        s, _b, _h = post(port, "/vnc", {"action": "on"})
        check(s == 303, "a write with no session and no CSRF value succeeds")
        conf = open(env["RB_LOCAL_CONF"]).read()
        check("RB_VNC=1" in conf, "and it wrote the setting")
        log = open(env["FAKE_LOG"]).read() if os.path.exists(env["FAKE_LOG"]) else ""
        check("systemctl enable --now rblive4-vnc" in log, "and it enabled the viewer")
        # The origin check STAYS ON: it costs the operator nothing and it stops another
        # website driving this unit through their browser.
        s, _b, _h = post(port, "/vnc", {"action": "off"}, origin="http://evil.example")
        check(s == 403, "but a cross-origin POST is still refused")
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

        test_states(tempfile.mkdtemp(prefix="confscreen-states."), free_port())
        test_no_password(tempfile.mkdtemp(prefix="confscreen-nopass."), free_port())
        test_no_auth(tempfile.mkdtemp(prefix="confscreen-noauth."), free_port())
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
