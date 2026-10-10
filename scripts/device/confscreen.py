#!/usr/bin/env python3
"""confscreen.py -- the unit's status page, and (next) its configuration page.

WHY THIS IS NOT PART OF THE VIEWER. The control page that exists today is served by
`vncserve`, and `rblive4-vnc.service` is enabled only when `RB_VNC=1` -- so on a unit with
VNC off there is no page at all, and the page cannot be the thing you use to turn VNC on.
There is a second reason, and it is the sharper one: that page's `/session`, `/input` and
`/mode` routes are state-changing GETs with no authentication (vnc_http.c:460,481,507), so
anyone on the LAN can start screen sharing or turn clicks in the picture into real presses
on the glass -- including the top-menu strip, where raw x 1884 is USB STOP. This process is
the page that outlives the viewer, and the place the authenticated write path will live.

WHAT THIS FIRST LANDING DOES. It READS and reports; it does not write anything, anywhere.
No configuration is edited, nothing is restarted, and no request can change a byte of this
unit's state. That is deliberate: the page, the bind, and the status readings are worth
reviewing on their own before anything starts editing `rb.local.conf`. The write path, the
auth and the restart button are the next slice.

THE ONE NUMBER THAT MATTERS MOST is the frame counter. `rbp` can be running, holding the
framebuffer and turning its render loop, and painting NOTHING -- a black screen with a live
process and no error anywhere. Its own log says so: the DS_HW_Glib3_DFB.c <1106> line is
written once per frame, ~46/s while the picture works and never while it is blank. So the
page reports the count and its rate, and says in words which of the three states the player
is in: not running / running and painting / running and NOT painting.

EVERY PATH AND COMMAND IS OVERRIDABLE BY AN ENVIRONMENT VARIABLE, and that is what makes it
testable anywhere: `test_confscreen.py` runs this as a real subprocess against fake /proc,
fake sysfs, a fake systemctl and a fake rbp.log, and asserts on what the page says.
"""

import html
import os
import socket
import subprocess
import sys
import secrets
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The editor for rb.local.conf, imported from beside this file rather than reimplemented.
# It is the one module that knows how to write that file safely -- validated, locked,
# backed up, and preserving every byte that is not the value being changed -- and the write
# path is the part of this page that can do damage, so it uses the tested one. If it is
# missing, writes are refused rather than attempted some other way.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import confedit
except Exception:
    confedit = None

# --- where everything is -----------------------------------------------------------
# Device defaults, every one of them overridable so the test can run this off the unit.
DEPLOY_ROOT = os.environ.get("RB_DEPLOY_ROOT", "/opt/rblive4")
CONF_FILE = os.environ.get("RB_CONF_FILE", os.path.join(DEPLOY_ROOT, "rb.conf"))
RUN_DIR = os.environ.get("RB_RUN_DIR", "/run/rblive4")
LOG_DIR = os.environ.get("RB_LOG_DIR", os.path.join(DEPLOY_ROOT, "log"))
PROC = os.environ.get("RB_PROC", "/proc")
SYSFS = os.environ.get("RB_SYSFS", "/sys")
RBP_LOG = os.environ.get("RB_RBP_LOG", os.path.join(LOG_DIR, "rbp.log"))
HEALTH_LOG = os.environ.get("RB_HEALTH_LOG", "/tmp/health.log")
BOOT_LOG = os.environ.get("RB_BOOT_LOG", os.path.join(RUN_DIR, "boot.log"))
BOOT_STAGE = os.environ.get("RB_BOOT_STAGE", os.path.join(RUN_DIR, "boot.stage"))
CHROOT = os.environ.get("RB_CHROOT", os.path.join(DEPLOY_ROOT, "rbx3-run"))
PLAYER = os.environ.get("RB_PLAYER_CHROOT", "/root/pdj/rbp")
SH = os.environ.get("RB_SH", "/bin/sh")
SYSTEMCTL = os.environ.get("RB_SYSTEMCTL", "systemctl")
VCGENCMD = os.environ.get("RB_VCGENCMD", "vcgencmd")
HOSTNAME_FILE = os.environ.get("RB_HOSTNAME_FILE", "/etc/hostname")

PORT = int(os.environ.get("RB_CONF_HTTP_PORT", "5904"))
BIND = os.environ.get("RB_CONF_BIND", "0.0.0.0")

# --- the write path's own places ---------------------------------------------------
LOCAL_CONF = os.environ.get("RB_LOCAL_CONF", os.path.join(DEPLOY_ROOT, "rb.local.conf"))
SESSION_TTL_S = int(os.environ.get("RB_CONF_SESSION_S", str(8 * 3600)))
LOGIN_MAX_FAILS = int(os.environ.get("RB_CONF_LOGIN_MAX", "5"))
LOGIN_WINDOW_S = int(os.environ.get("RB_CONF_LOGIN_WINDOW_S", "60"))
COOKIE = "rblive4conf"
VIEWER_UNIT = os.environ.get("RB_VNC_UNIT", "rblive4-vnc")
PLAYER_UNIT = os.environ.get("RB_PLAYER_UNIT", "rblive4")
BOOT_UNIT = os.environ.get("RB_BOOT_UNIT", "rblive4-boot")
RESTART_LOCK = os.environ.get("RB_CONF_RESTART_LOCK",
                              os.path.join(RUN_DIR, "conf-restart.lock"))
# Two restarts inside this many seconds are refused. Measured on `.239`: two overlapping
# `systemctl restart rblive4` invocations -- mine and the operator's landing together --
# wedge rbp outright, and one clean restart immediately after paints normally.
RESTART_COOLDOWN_S = int(os.environ.get("RB_CONF_RESTART_COOLDOWN_S", "20"))

# Sessions live in MEMORY ONLY. This process is the only thing that needs them, and a page
# restart should not leave a browser holding a credential the unit has forgotten.
# {sid: {"csrf": ..., "exp": ..., "flash": ...}}
_SESSIONS = {}
_LOGIN_FAILS = {}          # client address -> monotonic times of recent failures

# The frame counter. This string is the whole frame-rate instrument on this unit; it is
# written once per frame with no message, so a line count IS a frame count.
DSHW = "DS_HW_Glib3_DFB.c <1106>"

# How often the page re-reads its DATA, in seconds. 0 leaves the poll out entirely. This is
# a fetch of /data and a swap of two divs, not a reload -- see POLL_JS for why that
# distinction earned a script.
REFRESH_S = int(os.environ.get("RB_CONF_REFRESH_S", "5"))

# The units worth showing. Names are literals, so nothing here is ever interpolated into
# a command -- every systemctl call below passes a constant argv.
UNITS = ("rblive4", "rblive4-boot", "rblive4-vnc", "healthwatch", "rblive4-conf")

# The settings the page reports today and will edit in the next slice. Read-only here.
# The settings, in the two groups the operator asked for. The split is by what they are
# ABOUT -- the viewer's own knobs against everything the player and the unit use -- because
# that is how they get looked for. CONF_KEYS is the union, and it is what the page fetches.
VNC_KEYS = ("RB_VNC", "RB_VNC_PORT", "RB_VNC_HTTP_PORT", "RB_VNC_FPS", "RB_VNC_MODE",
            "RB_VNC_LIVE", "RB_VNC_INPUT", "RB_PASSWORD")
RBP_KEYS = ("RB_PREWARM", "RB_POINT_KIND", "RB_MIDI_MAP", "RB_AUDIO_DEV", "RB_BOOTSCREEN",
            "RB_FB_LIE_BPP", "RB_VERBOSE", "RB_CONF_AUTH", "RB_CONF_HTTP_PORT")
CONF_KEYS = VNC_KEYS + RBP_KEYS


def _text(path, default="", limit=None):
    """Read a small file, or return `default`. Never raises: this page reports a unit that
    may be in any state, and a status page that dies because a log is missing is worse than
    one that says the log is missing."""
    try:
        with open(path, "r", errors="replace") as f:
            data = f.read() if limit is None else f.read(limit)
        return data
    except OSError:
        return default


def _run(argv, timeout=5):
    """Run a constant argv and return stdout, or "" -- never a shell, never user input."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return (r.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def conf_values(keys=CONF_KEYS):
    """The RESOLVED values, the way the device scripts see them.

    Sourced rather than parsed, because rb.conf is a shell file whose `:=` defaults and
    whose `rb.local.conf` override at the end are only resolved by sourcing it -- and that
    is exactly how every device script reads it. The key names are this module's own
    constants, so nothing from a request is ever near this command line."""
    prog = '. "$1" >/dev/null 2>&1\n'
    for k in keys:
        prog += 'printf "%%s\\n" "${%s-}"\n' % k
    try:
        r = subprocess.run([SH, "-c", prog, "sh", CONF_FILE],
                           capture_output=True, text=True, timeout=5)
        lines = (r.stdout or "").splitlines()
    except (OSError, subprocess.SubprocessError):
        return {k: "" for k in keys}
    vals = dict(zip(keys, lines))
    for k in keys:
        vals.setdefault(k, "")
    return vals


def rbp_pid():
    """The player's pid, found by its loader command line.

    The loader alone is NOT an identity: `edb_streamd` is loader-started too and sorts
    first in pid order. The discriminator is the second argv, which ends `/rbp`. The walk
    happens in this process, on strings -- never a `pgrep` whose own command line could
    contain the player path, because start-rb.sh's cleanup() kills by cmdline match and a
    command that names it can kill its own caller."""
    try:
        entries = os.listdir(PROC)
    except OSError:
        return None
    for e in entries:
        if not e.isdigit():
            continue
        try:
            with open(os.path.join(PROC, e, "cmdline"), "rb") as f:
                parts = f.read().split(b"\0")
        except OSError:
            continue
        if len(parts) < 2:
            continue
        if not parts[0].endswith(b"ld-linux.so.3"):
            continue
        if not parts[1].endswith(b"/rbp"):
            continue
        return int(e)
    return None


def cpu_ticks(pid):
    """utime+stime for a pid, from /proc/<pid>/stat. Field 2 is `(comm)` and may hold
    spaces and brackets, so the split starts after the LAST ')' -- the classic trap."""
    try:
        with open(os.path.join(PROC, str(pid), "stat"), "r", errors="replace") as f:
            data = f.read()
    except OSError:
        return None
    try:
        fields = data[data.rindex(")") + 2:].split()
        return int(fields[11]) + int(fields[12])
    except (ValueError, IndexError):
        return None


def rbp_uptime(pid):
    """How long the player has been RUNNING, which is not the unit's uptime and is the more
    useful of the two: a player that restarted an hour ago is a different machine from one
    that has been up since the power cut.

    It is field 22 (`starttime`) of /proc/<pid>/stat -- clock ticks since boot, so the
    conversion needs the tick rate (SC_CLK_TCK, 100 here) subtracted from the system uptime.
    Reading the field without the tick rate is the way this is usually got wrong."""
    if pid is None:
        return None
    try:
        with open(os.path.join(PROC, str(pid), "stat"), "r", errors="replace") as f:
            data = f.read()
        start_ticks = int(data[data.rindex(")") + 2:].split()[19])
        up = float(_text(os.path.join(PROC, "uptime")).split()[0])
        hz = float(os.sysconf("SC_CLK_TCK"))
    except (OSError, ValueError, IndexError):
        return None
    secs = up - (start_ticks / hz)
    return secs if secs >= 0 else None


def human_duration(secs):
    """Seconds as `2h 14m`, or `13m 04s` under an hour -- and `14s` under a minute, because
    a player that has just come back is the case where the exact number matters."""
    if secs is None:
        return "&mdash;"
    s = int(secs)
    if s >= 3600:
        return "%dh %02dm" % (s // 3600, (s % 3600) // 60)
    if s >= 60:
        return "%dm %02ds" % (s // 60, s % 60)
    return "%ds" % s


def frame_count():
    """How many frames rbp has drawn since it started. None if there is no log at all."""
    if not os.path.exists(RBP_LOG):
        return None
    n = 0
    try:
        with open(RBP_LOG, "r", errors="replace") as f:
            for line in f:
                if DSHW in line:
                    n += 1
    except OSError:
        return None
    return n


_last_frames = None          # (count, monotonic) from the previous poll
_frames_rate = None


def frames_since_last():
    """The count, and the rate since the previous poll. The rate is what tells 'painting'
    from 'running and painting nothing', and it is kept here rather than recomputed because
    a status page is polled, not streamed."""
    global _last_frames, _frames_rate
    n = frame_count()
    now = time.monotonic()
    rate = None
    if n is not None and _last_frames is not None:
        prev_n, prev_t = _last_frames
        dt = now - prev_t
        if dt > 0.05:
            rate = (n - prev_n) / dt
    if n is not None:
        _last_frames = (n, now)
    _frames_rate = rate
    return n, rate


def unit_state(unit):
    """(active, enabled) for a unit. `is-enabled` prints `enabled`/`disabled`/`static`/
    `masked`; a unit that does not exist at all answers `not-found` for both."""
    active = _run([SYSTEMCTL, "is-active", unit]) or "unknown"
    enabled = _run([SYSTEMCTL, "is-enabled", unit]) or "unknown"
    return active, enabled


def port_open(port, host="127.0.0.1", timeout=0.4):
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return True
    except (OSError, ValueError, TypeError):
        return False


def switch_state(name):
    """One of the live `/run/rblive4/vnc.*` switch files, as the viewer reads them."""
    return _text(os.path.join(RUN_DIR, "vnc." + name)).strip() or "(absent)"


def decode_throttled(raw):
    """`vcgencmd get_throttled` decoded into words. Bits 0-3 are the CURRENT state, 16-19
    are 'has occurred at some point this boot' -- the distinction the operator needs before
    a show, since an under-voltage that has occurred is a supply to look at."""
    try:
        v = int(raw.strip(), 16)
    except (ValueError, AttributeError):
        return None
    now = [n for b, n in ((0, "under-voltage"), (1, "arm capped"), (2, "throttled"),
                          (3, "soft temp limit")) if v & (1 << b)]
    was = [n for b, n in ((16, "under-voltage"), (17, "arm capped"), (18, "throttled"),
                          (19, "soft temp limit")) if v & (1 << b)]
    return {"raw": raw.strip(), "now": now, "was": was}


def player_depth():
    """The player's layer pixel format, read from the two immediates the depth patch
    rewrites. It is HALF OF A PAIR with RB_FB_LIE_BPP, and a pair that disagrees is rbp
    SIGSEGV to a black screen -- so the page shows both and whether they agree, before
    anyone is tempted to restart."""
    try:
        with open(os.path.join(CHROOT, PLAYER.lstrip("/")), "rb") as f:
            f.seek(0x19BAB8)
            b0 = f.read(1)
            f.seek(0x19BAC0)
            b1 = f.read(1)
    except OSError:
        return None
    if not b0 or not b1:
        return None
    pair = (b0[0], b1[0])
    if pair == (0x01, 0x20):
        return 16
    if pair == (0x03, 0x40):
        return 32
    return "not a build this tree produced"


def unit_facts():
    """uptime, load, memory, temperature -- each guarded, because a status page must report
    a missing file rather than fail on it."""
    facts = {}
    up = _text(os.path.join(PROC, "uptime")).split()
    if up:
        try:
            facts["uptime"] = float(up[0])
        except ValueError:
            pass
    load = _text(os.path.join(PROC, "loadavg")).split()
    if len(load) >= 3:
        facts["load"] = " / ".join(load[:3])
    mem = {}
    for line in _text(os.path.join(PROC, "meminfo")).splitlines():
        p = line.split()
        if len(p) >= 2 and p[0].rstrip(":") in ("MemTotal", "MemAvailable", "Buffers", "Cached"):
            try:
                mem[p[0].rstrip(":")] = int(p[1]) // 1024
            except ValueError:
                pass
    if mem:
        facts["mem"] = mem
    t = _text(os.path.join(SYSFS, "class/thermal/thermal_zone0/temp")).strip()
    if t:
        try:
            facts["temp_c"] = int(t) / 1000.0
        except ValueError:
            pass
    raw = _run([VCGENCMD, "get_throttled"])
    if raw.startswith("throttled="):
        facts["throttled"] = decode_throttled(raw.split("=", 1)[1])
    return facts


def tail(path, n=12):
    lines = _text(path).splitlines()
    return lines[-n:]


def free_space(path):
    try:
        st = os.statvfs(path)
        total = st.f_blocks * st.f_frsize
        avail = st.f_bavail * st.f_frsize
        if total:
            return avail, total, (avail * 100.0 / total)
    except OSError:
        pass
    return None


def hostname():
    h = _text(HOSTNAME_FILE).strip()
    return h or socket.gethostname()


# --- the write path -----------------------------------------------------------------
#
# Every action below is reached ONLY through a POST carrying a session and a CSRF value,
# and each one does exactly one of two things: writes rb.local.conf through confedit, or
# hands systemd a CONSTANT argv. No value from a request is ever interpolated into a
# command line, and no command line here names the player path -- start-rb.sh's cleanup()
# kills by cmdline match, so a command that mentions it can kill its own caller.


def password():
    """The credential, read LIVE from the conf so that changing it on this page takes
    effect at once. Sourcing the shipped file is how every device script reads it, and it
    is the only way to resolve the `:=` defaults and the rb.local.conf override."""
    return conf_values(("RB_PASSWORD",)).get("RB_PASSWORD", "")


def writes_available():
    return confedit is not None


def auth_required():
    """Whether a write needs the password. `RB_CONF_AUTH=0` turns it off, which is the
    operator's call to make on their own LAN -- and it is read from the conf rather than
    from this process's environment, so the file that decides it is the same file that says
    everything else about this unit."""
    return conf_values(("RB_CONF_AUTH",)).get("RB_CONF_AUTH", "1") != "0"


def _run_rc(argv, timeout=25):
    """(rc, output) for a constant argv. Used where the EXIT STATUS is the answer -- an
    enable that failed must never be reported as an enable."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()
    except (OSError, subprocess.SubprocessError) as e:
        return 127, str(e)


def set_switch(name, value):
    """One of the live /run/rblive4/vnc.* files the viewer re-reads every turn. Written the
    way vnc_live.c writes it -- a word and a newline -- and created when it is not there
    yet, which it will not be on a unit whose viewer has never run."""
    path = os.path.join(RUN_DIR, "vnc." + name)
    try:
        os.makedirs(RUN_DIR, exist_ok=True)
        with open(path, "w") as f:
            f.write(value + "\n")
        return True, ""
    except OSError as e:
        return False, "could not write %s: %s" % (path, e)


def vnc_set(on):
    """Turn the viewer on or off for real, in TWO halves, and both are load-bearing.
    `systemctl enable --now` is what makes it run now; the line in rb.local.conf is what
    makes it come back after a power cut, because the thing that reads that line at boot is
    install.sh, and this process is not install.sh."""
    if not writes_available():
        return False, ("the config editor is not installed beside this page, so nothing "
                       "was changed")
    ok, why = confedit.write_local(LOCAL_CONF, "RB_VNC", "1" if on else "0")
    if not ok:
        return False, "could not record the choice, so nothing was changed: %s" % why
    if not on:
        # Leave the sharing switch off with it: a later enable must not come up already
        # serving the screen.
        set_switch("live", "off")
    rc, out = _run_rc([SYSTEMCTL, "enable" if on else "disable", "--now", VIEWER_UNIT])
    if rc != 0:
        return False, ("recorded RB_VNC=%s, but systemctl %s returned %d: %s"
                       % ("1" if on else "0", "enable" if on else "disable", rc, out))
    return True, ("the viewer is enabled and starting -- it will be on port %s, and its "
                  "own page at :%s"
                  % (conf_values(("RB_VNC_PORT",)).get("RB_VNC_PORT", "?"),
                     conf_values(("RB_VNC_HTTP_PORT",)).get("RB_VNC_HTTP_PORT", "?"))
                  if on else "the viewer is stopped and will not start at boot")


def share_set(on):
    """The live sharing switch: what the viewer re-reads each turn. No restart, and it
    works whether or not the viewer is running -- the file IS the request."""
    ok, why = set_switch("live", "on" if on else "off")
    if not ok:
        return False, why
    return True, ("sharing on -- the screen is being served" if on else "sharing off")


def restart_player():
    """Ask systemd for exactly ONE restart of the player, and refuse a second one too soon.

    The lock and the cooldown are not politeness. Two overlapping `systemctl restart
    rblive4` invocations wedge rbp on this unit -- measured, with the survivor coming up
    blank and staying blank -- and the two that did it were seconds apart, which is why the
    cooldown is longer than the call it guards.

    Nothing here names the player path: `start-rb.sh`'s cleanup() kills by cmdline match, so
    a command line that mentions it can kill its own caller."""
    try:
        import fcntl
    except ImportError:
        return False, "fcntl is not available, so a restart cannot be serialised"
    import time as _time
    fd = None
    try:
        os.makedirs(RUN_DIR, exist_ok=True)
        fd = os.open(RESTART_LOCK, os.O_RDWR | os.O_CREAT, 0o644)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        if fd is not None:
            os.close(fd)
        return False, "a restart is already in progress"
    try:
        last = _text(RESTART_LOCK).strip()
        try:
            last = float(last)
        except ValueError:
            last = 0.0
        waited = _time.time() - last
        if last and waited < RESTART_COOLDOWN_S:
            return False, ("a restart happened %d s ago; wait %d s and try again"
                           % (waited, RESTART_COOLDOWN_S - waited))
        # The boot screen FIRST. `systemctl restart rblive4` does not pull in
        # rblive4-boot.service (Before= with nothing requiring it), so without this the
        # restart blanks the screen and shows nothing at all for ~15 s. Best effort: a unit
        # that is not installed is not a reason to refuse the restart.
        if conf_values(("RB_BOOTSCREEN",)).get("RB_BOOTSCREEN", "1") != "0":
            _run_rc([SYSTEMCTL, "start", BOOT_UNIT], timeout=10)
        rc, out = _run_rc([SYSTEMCTL, "restart", PLAYER_UNIT], timeout=30)
        if rc != 0:
            return False, "systemctl restart returned %d: %s" % (rc, out)
        try:
            os.ftruncate(fd, 0)
            os.lseek(fd, 0, os.SEEK_SET)
            os.write(fd, ("%f\n" % _time.time()).encode())
        except OSError:
            pass
        return True, "restart requested -- the player is coming back now"
    finally:
        os.close(fd)          # closing the fd releases the flock


# --- sessions -----------------------------------------------------------------------
#
# A session is a random id in an HttpOnly, SameSite=Strict cookie, mapped IN MEMORY to a
# CSRF value and an expiry. SameSite=Strict is the CSRF defence that does not depend on a
# token surviving a redirect; the CSRF value is the belt to that, because a form a browser
# is made to post from another site cannot read this page to learn it.


def new_session():
    for sid in [s for s, v in _SESSIONS.items() if v["exp"] < time.monotonic()]:
        _SESSIONS.pop(sid, None)
    sid = secrets.token_hex(16)
    _SESSIONS[sid] = {"csrf": secrets.token_hex(16),
                      "exp": time.monotonic() + SESSION_TTL_S, "flash": ""}
    return sid


def get_session(sid):
    if not sid:
        return None
    s = _SESSIONS.get(sid)
    if s is None:
        return None
    if s["exp"] < time.monotonic():
        _SESSIONS.pop(sid, None)
        return None
    return s


def drop_session(sid):
    _SESSIONS.pop(sid, None)


def rate_limited(addr):
    now = time.monotonic()
    fails = [t for t in _LOGIN_FAILS.get(addr, []) if now - t < LOGIN_WINDOW_S]
    _LOGIN_FAILS[addr] = fails
    return len(fails) >= LOGIN_MAX_FAILS


def note_fail(addr):
    _LOGIN_FAILS.setdefault(addr, []).append(time.monotonic())


# --- the page ----------------------------------------------------------------------
CSS = """
:root { color-scheme: dark; }
body { margin: 0; padding: 16px 18px 40px; background: #12141a; color: #d8dee9;
       font: 14px/1.5 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
h1 { font-size: 17px; margin: 0 0 2px; color: #eceff4; letter-spacing: .02em; }
h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .12em; color: #8b94a3;
     margin: 22px 0 6px; border-bottom: 1px solid #262a33; padding-bottom: 4px; }
.sub { color: #8b94a3; font-size: 12px; margin-bottom: 12px; }
table { border-collapse: collapse; width: 100%; max-width: 900px; }
th, td { text-align: left; padding: 3px 14px 3px 0; vertical-align: top; }
th { color: #8b94a3; font-weight: normal; }
.k { color: #8b94a3; width: 260px; }
.ok { color: #a3be8c; } .bad { color: #bf616a; } .warn { color: #ebcb8b; }
.dim { color: #6b7480; }
pre { background: #0d0f14; border: 1px solid #232833; border-radius: 4px; padding: 8px 10px;
      overflow-x: auto; color: #c8d0da; margin: 4px 0 0; max-width: 900px; }
.state { font-size: 15px; }
.note { color: #8b94a3; font-size: 12px; margin-top: 4px; }
.shot { margin-top: 6px; } .shot img { max-width: 100%; border: 1px solid #232833; }
.flash { margin: 8px 0; padding: 6px 9px; border-radius: 4px; background: #16232e;
         border: 1px solid #2b4257; color: #cfe3f5; }
.flash.bad { background: #3a1f22; border-color: #5c2b2f; color: #eda6ac; }
form { display: inline; }
button { font: inherit; padding: 3px 9px; margin-right: 6px; border-radius: 4px;
         border: 1px solid #38414f; background: #1e232c; color: #d8dee9; cursor: pointer; }
button:hover { background: #262d38; }
input[type=password] { font: inherit; padding: 3px 7px; border-radius: 4px;
         border: 1px solid #38414f; background: #0d0f14; color: #d8dee9; }
footer { margin-top: 28px; color: #6b7480; font-size: 12px; }
/* The sections are navigable from the left, and it is anchors and CSS rather than script:
   it survives the data poll (the ids are inside the swapped regions and are re-created with
   the same names), and it works with the script blocked, which is the rule here. */
#layout { display: flex; gap: 26px; align-items: flex-start; }
#nav { position: sticky; top: 14px; flex: 0 0 168px; display: flex; flex-direction: column;
       border-right: 1px solid #232833; padding-right: 14px; }
#nav b { color: #8b94a3; font-weight: normal; margin-bottom: 8px; }
#nav a { color: #a9b4c2; text-decoration: none; padding: 3px 0; }
#nav a:hover { color: #eceff4; }
#nav a.on { color: #eceff4; font-weight: bold; }
#main { min-width: 0; flex: 1 1 auto; }
h2 { scroll-margin-top: 14px; }
@media (max-width: 720px) {
  #layout { display: block; }
  #nav { position: static; flex-direction: row; flex-wrap: wrap; gap: 0 14px;
         border-right: 0; border-bottom: 1px solid #232833; padding: 0 0 8px; margin-bottom: 10px; }
  #nav b { width: 100%; }
}
"""

# THE ONLY SCRIPT, and it re-reads the DATA rather than the page. A `<meta http-equiv=refresh>`
# is one line of HTML and no script at all, and it was the first version of this page -- but it
# reloads the whole document, which throws away the scroll position and anything half-typed
# into the sign-in box, every few seconds, to update some numbers. So this fetches `/data` (the
# live halves of the page, as HTML) and swaps their contents in place.
#
# It stays inside the rule this page family follows -- no CDN, no framework, no build step --
# and the page is complete without it: the first load already carries every one of these
# figures in the HTML, so a browser that blocks the script shows a correct page that simply
# does not update itself. RB_CONF_REFRESH_S=0 leaves the script out entirely.
POLL_JS = """
<script>
// TWO SMALL JOBS, and both are here because neither can be done in CSS alone.
//
// 1. THE TABS. The nav shows one section at a time. With the script blocked every section is
//    shown -- that is the page's "complete without it" property, not a broken fallback.
// 2. THE POLL. It re-reads the DATA, because a meta refresh threw away the scroll position and
//    anything half-typed into the sign-in box every few seconds.
//
// IT SWAPS ONLY THE SECTION YOU ARE LOOKING AT. The first version replaced the whole content
// region on every tick, which re-created every section -- and the ones you cannot see come back
// WITHOUT `hidden`, so for an instant the page showed all eight at once, every five seconds. It
// looked exactly like a page reload, which is the very thing this was built to stop. The other
// sections keep what they had and are refreshed when you switch to them.
(function () {
  var tabs = [].slice.call(document.querySelectorAll('#nav a[href^="#"]'));
  var sections = [].slice.call(document.querySelectorAll('main section'));
  var current = (location.hash || '#player').slice(1);

  function known(id) {
    return sections.some(function (s) { return s.id === id; });
  }

  function applyTab() {
    var here = known(current);
    sections.forEach(function (s) { s.hidden = here && s.id !== current; });
    tabs.forEach(function (a) {
      a.classList.toggle('on', a.getAttribute('href') === '#' + current);
    });
  }

  function tick() {
    fetch('/data', {cache: 'no-store'}).then(function (r) { return r.text(); }).then(function (t) {
      var box = document.createElement('div');
      box.innerHTML = t;
      var fresh = box.querySelector('#' + current);
      var here = document.getElementById(current);
      if (fresh && here) { here.innerHTML = fresh.innerHTML; }
    }).catch(function () {});
  }

  function go(id) {
    current = known(id) ? id : 'player';
    applyTab();
    history.replaceState(null, '', '#' + current);
    tick();               // the section you just opened may not have been read for a while
  }

  tabs.forEach(function (a) {
    a.addEventListener('click', function (e) {
      e.preventDefault();
      go(a.getAttribute('href').slice(1));
    });
  });
  // Any other fragment change -- an in-content link, the back button -- switches too.
  window.addEventListener('hashchange', function () { go((location.hash || '#player').slice(1)); });

  applyTab();
  var ms = %d * 1000;
  if (ms) { setInterval(tick, ms); }
  var flash = document.querySelector('.flash');
  if (flash) { setTimeout(function () { flash.style.display = 'none'; }, 8000); }
})();
</script>
"""


def esc(s):
    return html.escape(str(s if s is not None else ""))


def player_state(pid, frames, rate):
    """The three states, in words -- because 'a black screen' and 'no player' look identical
    from the front and are entirely different problems."""
    if pid is None:
        return "bad", "rbp is NOT running"
    if rate is not None and rate <= 0:
        return "bad", "rbp is running and drawing NOTHING (the blank-screen state)"
    if rate is None:
        return "warn", "rbp is running; too soon to tell whether it is painting"
    return "ok", "rbp is running and painting"


def _conf_rows(a, conf, keys):
    """The value rows for one settings group. A secret is never printed -- `(set)` or
    `(empty)` is all the page says about it, whatever `RB_PASSWORD` currently is."""
    for k in keys:
        v = conf.get(k, "")
        shown = "(empty)" if v == "" else v
        if k in ("RB_PASSWORD", "RB_VNC_PASSWORD"):
            shown = "(set)"
        a("<tr><td class=k>%s</td><td>%s</td></tr>" % (esc(k), esc(shown)))


def render_actions(a, auth):
    """The interactive half: sign in, and the two buttons. Kept OUT of the live region on
    purpose -- re-fetching this would throw away anything half-typed into the sign-in box,
    which is the whole reason the page polls the data instead of reloading itself. Nothing
    here changes on its own: signing in navigates, and the buttons navigate."""
    a('<section id=actions>')
    a("<h2>actions</h2>")
    if not auth["writes"]:
        a('<div class=note>The config editor is not installed beside this page, so nothing '
          'here will write. Re-run install.sh.</div>')
    elif not auth.get("required", True):
        # Turned off on purpose by the operator, and said loudly rather than left implicit:
        # a page that silently has no lock is one nobody re-checks.
        a('<div class="note bad">Writes on this unit are <b>unprotected</b> '
          '(<code>RB_CONF_AUTH=0</code>): anyone who can reach this page can change a '
          'setting, enable or stop the viewer, and start sharing the screen. No password '
          'is asked for, and the VNC client password is separate.</div>')
    elif not auth["password_set"]:
        a('<div class="note bad">No password is configured (<code>RB_PASSWORD</code> is '
          'empty), so writes are REFUSED rather than allowed. Set one in <code>%s</code> '
          'and restart this page.</div>' % esc(LOCAL_CONF))
    if not auth["signed_in"]:
        a('<div class=note>Reading is open on this LAN. Changing anything -- including '
          'starting the viewer -- needs the password.</div>')
        a('<form method=post action=/login>'
          # No placeholder: the shipped default password is the word "password", and a
          # form that spells it out advertises the credential to anyone who loads the
          # page. doctor.sh tells the OWNER it is still the placeholder; the page does not
          # need to tell a passer-by.
          '<label>password <input type=password name=pw autocomplete=current-password>'
          '</label> <button>Sign in</button></form>')
    else:
        csrf = '<input type=hidden name=csrf value="%s">' % esc(auth["csrf"])
        if auth.get("required", True):
            a('<div class=note>Signed in. '
              '<form method=post action=/logout><button>Sign out</button></form></div>')
        else:
            a('<div class=note>Nothing to sign in to: this unit does not ask for a '
              'password.</div>')
        a("<table>")
        a("<tr><td class=k>the viewer</td><td>"
          '<form method=post action=/vnc>' + csrf +
          '<button name=action value=on>Enable the viewer</button>'
          '<button name=action value=off>Stop and disable it</button></form>'
          '<div class=note>Enabling is two things: it starts the viewer now, and writes '
          'RB_VNC=1 so it comes back after a power cut.</div></td></tr>')
        a("<tr><td class=k>screen sharing</td><td>"
          '<form method=post action=/share>' + csrf +
          '<button name=action value=on>Start sharing</button>'
          '<button name=action value=off>Stop sharing</button></form>'
          '<div class=note>The live switch the viewer re-reads each turn. No restart, and '
          'it works whether or not the viewer is running.</div></td></tr>')
        a("</table>")
    a("</section>")


def render(conf, pid, frames, rate, facts, services, switches, depth, req_host="", auth=None,
           data_only=False):
    """The whole page, or -- with `data_only` -- just the two halves the poll re-reads.

    They are one function on purpose. The fragment MUST be drawn by the same code as the
    first load, or the page and its updates drift apart, and the drift shows up as a figure
    that changes when you reload and not otherwise. What separates them is only the shell:
    the head, the flash, the actions and the footer, none of which the poll touches."""
    auth = auth or {"signed_in": False, "csrf": "", "flash": "", "writes": False,
                    "password_set": False}
    h = []
    a = h.append
    if not data_only:
        a("<!doctype html><html lang=en><head><meta charset=utf-8>")
        a('<meta name=viewport content="width=device-width,initial-scale=1">')
        a("<title>%s - rbp status</title><style>%s</style></head><body>"
          % (esc(hostname()), CSS))
        a("<h1>%s &mdash; rbp status</h1>" % esc(hostname()))
        a('<div class=sub>page %d &middot; %s &middot; the data re-reads every %ds</div>'
          % (PORT, esc(time.strftime("%H:%M:%S")), REFRESH_S))
        if auth["flash"]:
            a('<div class="flash%s">%s</div>'
              % (" bad" if auth["flash"].startswith("FAILED") else "",
                 esc(auth["flash"])))
        # The sections, as items down the left. In PAGE order, which is the order they are
        # read in -- the actions sit between the viewer and the unit because the buttons
        # belong next to what they act on.
        a('<div id=layout><nav id=nav><b>%s</b>' % esc(hostname()))
        for anchor, label in (("player", "player"), ("services", "services"),
                              ("launcher", "launcher"), ("viewer", "viewer"),
                              ("actions", "actions"), ("unit", "unit"),
                              ("vncsettings", "vnc settings"),
                              ("rbpsettings", "rbp settings")):
            a('<a href="#%s">%s</a>' % (anchor, esc(label)))
        a("</nav><main id=main>")

    a('<div id=data1>')

    # --- the player, first, because it is the thing that is either working or not ---
    cls, words = player_state(pid, frames, rate)
    a('<section id=player>')
    a("<h2>player</h2>")
    a('<div class="state %s">%s</div>' % (cls, esc(words)))
    a("<table>")
    a("<tr><td class=k>pid</td><td>%s</td></tr>" % esc(pid if pid is not None else "&mdash;"))
    a("<tr><td class=k>running for</td><td>%s</td></tr>" % human_duration(rbp_uptime(pid)))
    if frames is not None:
        a("<tr><td class=k>frames drawn (DS_HW lines)</td><td>%d</td></tr>" % frames)
    if rate is not None:
        a("<tr><td class=k>frame rate</td><td>%.1f/s</td></tr>" % rate)
    a("<tr><td class=k>log</td><td>%s (last line)</td></tr>" % esc(RBP_LOG))
    a("</table>")
    lt = tail(RBP_LOG, 6)
    if lt:
        a("<pre>%s</pre>" % esc("\n".join(lt)))
    a("</section>")

    # --- services ---
    a('<section id=services>')
    a("<h2>services</h2><table><tr><th>unit</th><th>active</th><th>enabled</th></tr>")
    for u in UNITS:
        act, en = services.get(u, ("unknown", "unknown"))
        c = "ok" if act == "active" else ("dim" if act in ("inactive", "unknown") else "bad")
        a("<tr><td>%s</td><td class=%s>%s</td><td class=dim>%s</td></tr>"
          % (esc(u), c, esc(act), esc(en)))
    a("</table>")
    # The restart lives HERE because this is where you look when the player is wrong: the
    # row above says whether rblive4 is up, and the button that fixes it is under it.
    if auth["writes"] and auth["signed_in"]:
        a('<div class=note>Restarting stops the player and starts it again. The screen goes '
          'dark for about fifteen seconds, and the boot screen draws its stages while it '
          'comes back. A set in progress stops.</div>')
        a('<form method=post action=/restart>'
          '<input type=hidden name=csrf value="%s">'
          '<button name=step value=ask>Restart the player&hellip;</button></form>'
          % esc(auth["csrf"]))
    elif auth["writes"]:
        a('<div class=note>Restarting the player needs the password -- sign in under '
          '<a href="#actions">actions</a>.</div>')
    a("</section>")

    # --- the launcher's own account of the last start ---
    stage = _text(BOOT_STAGE).strip()
    blog = tail(BOOT_LOG, 14)
    a('<section id=launcher>')
    a("<h2>launcher</h2>")
    if stage:
        a('<div class=note>last boot-screen stage: %s</div>' % esc(stage))
    a("<pre>%s</pre>" % esc("\n".join(blog) if blog else "(no %s)" % BOOT_LOG))
    a("</section>")

    # --- the viewer, and its switches ---
    a('<section id=viewer>')
    a("<h2>viewer</h2><table>")
    a("<tr><td class=k>sharing (vnc.live)</td><td>%s</td></tr>" % esc(switches.get("live")))
    a("<tr><td class=k>encoding (vnc.mode)</td><td>%s</td></tr>" % esc(switches.get("mode")))
    a("<tr><td class=k>pointer injection (vnc.input)</td><td>%s</td></tr>"
      % esc(switches.get("input")))
    rfb = conf.get("RB_VNC_PORT", "")
    pg = conf.get("RB_VNC_HTTP_PORT", "")
    a("<tr><td class=k>RFB port %s</td><td>%s</td></tr>"
      % (esc(rfb), "listening" if port_open(rfb) else "not listening"))
    a("<tr><td class=k>viewer page port %s</td><td>%s</td></tr>"
      % (esc(pg), "listening" if port_open(pg) else "not listening"))
    a("</table>")
    if switches.get("live") == "on" and port_open(pg):
        # The preview belongs to the viewer (it owns the capture) and stays there; this
        # page only points at it. The host comes from the REQUEST, not from this unit's
        # own hostname: the browser is on the other end of the LAN and may not be able to
        # resolve `rpidev01` at all, whereas it demonstrably resolved whatever it typed
        # to get here. In the next slice the viewer's page moves to loopback and this
        # becomes a proxy through this same port, so no cross-port URL remains.
        host_only = req_host.rsplit(":", 1)[0] if req_host else hostname()
        a('<div class=shot><img src="//%s:%s/preview.mjpg" alt="screen preview"></div>'
          % (esc(host_only), esc(pg)))

    a("</section>")
    a("</div>")                      # end of the first live half

    if not data_only:
        render_actions(a, auth)

    a('<div id=data2>')

    # --- the unit ---
    a('<section id=unit>')
    a("<h2>unit</h2><table>")
    if "uptime" in facts:
        a("<tr><td class=k>uptime</td><td>%.0f s</td></tr>" % facts["uptime"])
    if "load" in facts:
        a("<tr><td class=k>load</td><td>%s</td></tr>" % esc(facts["load"]))
    if "mem" in facts:
        m = facts["mem"]
        a("<tr><td class=k>memory</td><td>%s MB available of %s MB</td></tr>"
          % (esc(m.get("MemAvailable", "?")), esc(m.get("MemTotal", "?"))))
    if "temp_c" in facts:
        a("<tr><td class=k>SoC temperature</td><td>%.1f &deg;C</td></tr>" % facts["temp_c"])
    th = facts.get("throttled")
    if th:
        now = ", ".join(th["now"]) or "none"
        was = ", ".join(th["was"]) or "none"
        c = "bad" if th["now"] else "dim"
        a("<tr><td class=k>power (get_throttled %s)</td><td class=%s>now: %s<br>has occurred: %s</td></tr>"
          % (esc(th["raw"]), c, esc(now), esc(was)))
    fs = free_space(DEPLOY_ROOT)
    if fs:
        a("<tr><td class=k>free space on %s</td><td>%.1f GB of %.1f GB (%.0f%%)</td></tr>"
          % (esc(DEPLOY_ROOT), fs[0] / 1e9, fs[1] / 1e9, fs[2]))
    hl = tail(HEALTH_LOG, 1)
    if hl:
        a("<tr><td class=k>health (last)</td><td>%s</td></tr>" % esc(hl[-1]))
    a("</table>")
    a("</section>")

    # --- the settings, in the two groups the operator asked for: what the VIEWER is, and
    # what the player and the unit are. Both are shown read-only; the form is the next slice.
    a('<section id=vncsettings>')
    a('<h2>vnc settings <span class=dim>(shown read-only)</span></h2><table>')
    _conf_rows(a, conf, VNC_KEYS)
    a("</table></section>")

    a('<section id=rbpsettings>')
    a('<h2>rbp settings <span class=dim>(shown read-only)</span></h2><table>')
    _conf_rows(a, conf, RBP_KEYS)
    lie = conf.get("RB_FB_LIE_BPP", "")
    a("<tr><td class=k>player's layer format word</td><td>%s</td></tr>"
      % esc(("not readable" if depth is None else "%s bpp" % depth)))
    if depth is not None and lie:
        agree = str(depth) == str(lie)
        a("<tr><td class=k>depth pair agrees?</td><td class=%s>%s</td></tr>"
          % ("ok" if agree else "bad",
             "yes" if agree else "NO &mdash; rbp will not start until these match"))
    a("</table>")
    a('<div class=note>The depth pair is shown and never editable: it is half of the '
      'player build, and a value that disagrees is rbp SIGSEGV to a black screen.</div>')
    a("</section>")
    a("</div>")                      # end of the second live half

    if not data_only:
        a("</main></div>")           # close #main and #layout
        a("<footer>%s See <code>docs/20-config-page.md</code>.</footer>"
          % ("reading is open; writes need the password."
             if auth["writes"] else
             "read-only: the config editor is not installed beside this page."))
        a(POLL_JS % REFRESH_S)
        a("</body></html>")
    return "".join(h)


class Handler(BaseHTTPRequestHandler):
    server_version = "rblive4-conf"
    sys_version = ""

    def log_message(self, *a):
        """No per-request logging: the page is polled every few seconds and journald is for
        things that changed, not for things that were asked again."""

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        raw = body.encode("utf-8", "replace")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def _redirect(self, where="/", cookie=None):
        """303 back to the page after a write, rather than a rendered reply -- so that a
        browser refresh re-reads the page instead of re-posting the action."""
        self.send_response(303)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.send_header("Connection", "close")
        self.end_headers()

    def _msg(self, title, text):
        """A tiny page for anything that is not the page: a refusal, or a reason one could
        not be carried out. `text` is ours and may carry markup."""
        return ("<!doctype html><html><head><meta charset=utf-8><title>%s</title>"
                "<style>%s</style></head><body><h1>%s</h1><div class=note>%s</div>"
                "<p><a href=\"/\">back to the page</a></p></body></html>"
                % (esc(title), CSS, esc(title), text))

    # --- request parsing -------------------------------------------------------------

    def _cookies(self):
        out = {}
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k.strip()] = v.strip()
        return out

    def _session(self):
        return get_session(self._cookies().get(COOKIE))

    def _form(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0 or n > 8192:
            return {}
        try:
            raw = self.rfile.read(n).decode("utf-8", "replace")
        except OSError:
            return {}
        return {k: v[0]
                for k, v in urllib.parse.parse_qs(raw, keep_blank_values=True).items()}

    def _same_origin(self):
        """A POST that says it came from somewhere else is refused. `SameSite=Strict` on
        the cookie is the real defence; this is the second lock, for a browser that sends
        `Origin` anyway."""
        origin = self.headers.get("Origin")
        if not origin:
            return True
        try:
            host = urllib.parse.urlsplit(origin).netloc
        except ValueError:
            return False
        return host == (self.headers.get("Host") or "")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html", "/status"):
            self._send(200, self.page())
        elif path == "/data":
            # The live halves, for the page's own poll. Never a full document: this is what
            # the browser swaps into #data1/#data2 so a refresh does not reload the page.
            self._send(200, self.data())
        elif path == "/healthz":
            self._send(200, "ok\n", "text/plain; charset=utf-8")
        elif path in ("/login", "/logout", "/vnc", "/share", "/restart"):
            # A state change is never a GET. Refused as a METHOD here, so that no handler
            # can later be got wrong into acting on one -- which is the bug the viewer's
            # own page has, where /session, /input and /mode are state-changing GETs.
            self._send(405, self._msg("that is a POST",
                                      "Nothing on this page changes on a GET."))
        else:
            self._send(404, self._msg("no such page here",
                                      'Try <a href="/">/</a> instead.'))

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if not self._same_origin():
            self._send(403, self._msg("refused",
                                      "That request did not come from this page."))
            return
        form = self._form()
        if path == "/login":
            self._login(form)
        elif path == "/logout":
            drop_session(self._cookies().get(COOKIE))
            self._redirect("/", cookie="%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"
                                     % COOKIE)
        elif path in ("/vnc", "/share"):
            self._action(path, form)
        elif path == "/restart":
            self._restart(form)
        else:
            self._send(404, self._msg("no such page here",
                                      'Try <a href="/">/</a> instead.'))

    def _login(self, form):
        if not auth_required():
            # Nothing to sign in to. Say so rather than 401, which would look like a wrong
            # password and send the operator hunting for one.
            self._redirect("/")
            return
        pw = password()
        addr = self.client_address[0]
        if not pw:
            # FAIL CLOSED. An empty password must never mean "no authentication" -- it
            # means the unit has no credential, so nothing is written.
            self._send(503, self._msg(
                "no password is set",
                "This unit has no <code>RB_PASSWORD</code>, so writes are refused rather "
                "than allowed. Set one in <code>%s</code> and restart this page."
                % esc(LOCAL_CONF)))
            return
        if rate_limited(addr):
            self._send(429, self._msg("too many attempts",
                                      "Wait a minute, then try again."))
            return
        given = form.get("pw", "")
        if not (given and secrets.compare_digest(given.encode(), pw.encode())):
            note_fail(addr)
            print("confscreen: rejected a sign-in from %s" % addr, flush=True)
            self._send(401, self._msg("wrong password", "Nothing was changed."))
            return
        sid = new_session()
        _LOGIN_FAILS.pop(addr, None)      # a good password clears the strike count
        get_session(sid)["flash"] = "signed in -- the actions below now do what they say"
        self._redirect("/", cookie="%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict"
                                 % (COOKIE, sid, SESSION_TTL_S))

    def _action(self, path, form):
        if auth_required():
            s = self._session()
            if s is None:
                self._send(401, self._msg("sign in first",
                            "Reading is open on this page; changing anything needs the "
                            "password."))
                return
            if not secrets.compare_digest(form.get("csrf", "").encode(),
                                          s["csrf"].encode()):
                self._send(403, self._msg("stale form",
                            "That form did not come from this session. Reload the page and "
                            "try again."))
                return
        s = self._session()
        action = form.get("action", "")
        if action not in ("on", "off"):
            self._send(400, self._msg("bad request", "action must be on or off."))
            return
        on = action == "on"
        ok, msg = vnc_set(on) if path == "/vnc" else share_set(on)
        # There may be no session at all: with RB_CONF_AUTH=0 there is nothing to sign in
        # to, so the flash has nowhere to live and the redirect simply shows the new state.
        if s is not None:
            s["flash"] = ("the viewer: " if path == "/vnc" else "sharing: ") + \
                         ("" if ok else "FAILED: ") + msg
        print("confscreen: %s %s -> %s%s" % (path, action, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/")

    def _context(self):
        """Everything the page and its fragment both need, gathered once. The frame RATE is
        computed from the interval between calls, so the poll IS the frame sampler -- which
        is why this is gathered per request rather than cached."""
        return (conf_values(), rbp_pid(), frames_since_last(), unit_facts(),
                {u: unit_state(u) for u in UNITS},
                {n: switch_state(n) for n in ("live", "mode", "input")},
                player_depth(), self.headers.get("Host", ""))

    def _auth(self, conf, consume_flash=False):
        """The auth state for one render. `consume_flash` is what makes a flash show once:
        the page takes it and clears it, while the poll -- which must not consume anything --
        only reads it."""
        s = self._session()
        required = conf.get("RB_CONF_AUTH", "1") != "0"
        out = {"signed_in": (not required) or s is not None,
               "required": required,
               "csrf": (s or {}).get("csrf", ""),
               "flash": (s or {}).get("flash", ""),
               "writes": writes_available(),
               # From the values already gathered, so a poll costs no extra fork.
               "password_set": bool(conf.get("RB_PASSWORD", ""))}
        if s and consume_flash:
            s["flash"] = ""
        return out

    def _restart(self, form):
        """TWO STEPS, and the confirm is a real page rather than a script dialog: this is the
        one control on this page that acts on something the operator can SEE, and it blanks
        the screen for a quarter of a minute. Pressing it by accident should cost one more
        click, and the confirm has to work with the script blocked, like everything else
        here."""
        s = self._session()
        if auth_required():
            if s is None:
                self._send(401, self._msg("sign in first",
                            "Restarting the player needs the password."))
                return
            if not secrets.compare_digest(form.get("csrf", "").encode(), s["csrf"].encode()):
                self._send(403, self._msg("stale form",
                            "That form did not come from this session. Reload the page."))
                return
        if form.get("step", "ask") != "go":
            csrf = ('<input type=hidden name=csrf value="%s">'
                    % esc((s or {}).get("csrf", "")))
            self._send(200, self._msg(
                "restart the player?",
                "This stops rbp and starts it again. The screen goes dark for about fifteen "
                "seconds, the boot screen draws its stages while it comes back, and a set in "
                "progress stops."
                "<p><form method=post action=/restart>" + csrf +
                "<button name=step value=go>Yes, restart it</button></form> "
                '<a href="/#services">no, leave it alone</a></p>'))
            return
        ok, msg = restart_player()
        if s is not None:
            s["flash"] = ("restart FAILED: " if not ok else "restart: ") + msg
        print("confscreen: restart -> %s%s" % ("" if ok else "FAILED: ", msg), flush=True)
        # Land on the services tab, where the button was: the rows show the player coming
        # back, and with auth off there is no session to flash a message into.
        self._redirect("/#services")

    def page(self):
        conf, pid, (frames, rate), facts, services, switches, depth, host = self._context()
        return render(conf, pid, frames, rate, facts, services, switches, depth, host,
                      self._auth(conf, consume_flash=True))

    def data(self):
        """Just the live halves. NO session work here on purpose: this is fetched every few
        seconds by the page's own script, so it must not consume a flash or change any state
        -- but it DOES carry the auth state, because the restart button lives in the services
        half and its CSRF value has to be right in a fragment too."""
        conf, pid, (frames, rate), facts, services, switches, depth, host = self._context()
        return render(conf, pid, frames, rate, facts, services, switches, depth, host,
                      self._auth(conf), data_only=True)


def main():
    try:
        srv = ThreadingHTTPServer((BIND, PORT), Handler)
    except OSError as e:
        # Loudly, and with a non-zero exit: a page that is silently absent is worse than
        # one that refuses to start, because the operator reloads it for ten minutes.
        print("confscreen: cannot bind %s:%d: %s" % (BIND, PORT, e), flush=True)
        return 1
    srv.daemon_threads = True
    print("confscreen: serving %s:%d -- reads open, writes need the password%s"
          % (BIND, PORT, "" if writes_available() else
             " (no config editor beside this page, so writes are refused)"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
