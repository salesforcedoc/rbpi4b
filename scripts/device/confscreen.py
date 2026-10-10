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
VIEWER_UNIT = os.environ.get("RB_VNC_UNIT", "rblive4-vnc")
PLAYER_UNIT = os.environ.get("RB_PLAYER_UNIT", "rblive4")
BOOT_UNIT = os.environ.get("RB_BOOT_UNIT", "rblive4-boot")
# This page's own unit. Named here because two places have to treat it specially: it must
# never be waited for (see service_action) and the services table must not offer to stop the
# thing serving the page.
CONF_UNIT = os.environ.get("RB_CONF_UNIT", "rblive4-conf")
# The web client's bridge. It is a unit like any other and belongs in the same table -- it was
# missing from it, which made it the one service on the unit that could be neither started nor
# enabled from the page that exists to do exactly that.
WEBVNC_UNIT = os.environ.get("RB_WEBVNC_UNIT", "rblive4-webvnc")
RESTART_LOCK = os.environ.get("RB_CONF_RESTART_LOCK",
                              os.path.join(RUN_DIR, "conf-restart.lock"))
# Whether THIS PAGE embeds the preview image. The page's own preference and not a switch the
# viewer reads -- which is why it is `conf.preview` and not `vnc.*`: turning it on changes
# nothing about what is being served, only whether this page subscribes to a stream of it. In
# /run, so it survives a page restart and is cleared by a reboot like everything else here.
PREVIEW_FILE = os.environ.get("RB_CONF_PREVIEW_FILE",
                              os.path.join(RUN_DIR, "conf.preview"))
# Two restarts inside this many seconds are refused. Measured on `.239`: two overlapping
# `systemctl restart rblive4` invocations -- mine and the operator's landing together --
# wedge rbp outright, and one clean restart immediately after paints normally.
RESTART_COOLDOWN_S = int(os.environ.get("RB_CONF_RESTART_COOLDOWN_S", "20"))

# THE ONE LINE THIS PAGE SHOWS AFTER A WRITE. It used to live in a sign-in session; with no
# sign-in there is one page and one operator, so one slot is the honest shape. It is set by a
# write and taken by the next full render -- the poll must not consume it, or a message raised
# between two ticks would be eaten by a refresh nobody was looking at.
_FLASH = {"text": ""}

# The frame counter. This string is the whole frame-rate instrument on this unit; it is
# written once per frame with no message, so a line count IS a frame count.
DSHW = "DS_HW_Glib3_DFB.c <1106>"

# How often the page re-reads its DATA, in seconds. 0 leaves the poll out entirely. This is
# a fetch of /data and a swap of two divs, not a reload -- see POLL_JS for why that
# distinction earned a script.
REFRESH_S = int(os.environ.get("RB_CONF_REFRESH_S", "5"))

# The units worth showing, AND AT THE SAME TIME the whitelist the service button validates
# against: a unit name arrives from a form and becomes an argument to systemctl, so the only
# names that may reach an argv array are these. Built from the constants above rather than
# retyped, so a unit cannot be listed here under a spelling the rest of the file does not use.
UNITS = (PLAYER_UNIT, BOOT_UNIT, VIEWER_UNIT, WEBVNC_UNIT, "healthwatch", CONF_UNIT)

# THE INSTALL-TIME MIRROR OF A BOOT CHOICE. install.sh decides whether to install some units
# enabled or disabled from an RB_* line, so a bare `systemctl enable` is a choice the next
# install would quietly undo. Where such a line exists the button writes it too: the two
# mechanisms are kept in step rather than left to disagree in silence, which is a trap that
# would only spring weeks later, on an install.
BOOT_SETTING = {VIEWER_UNIT: "RB_VNC", BOOT_UNIT: "RB_BOOTSCREEN",
                WEBVNC_UNIT: "RB_VNC_WEB"}

# THE UNITS THE PAGE WILL NOT DISABLE, WITH THE REASON. They still show the pair -- both
# states, the one in force drawn as pressed -- but their `disabled` button is itself disabled:
# the direction the page will not take is GREYED rather than absent, and the reason rides on
# the (i) beside it, which is how this page shows anything it will not do.
#
# ONLY THE DISABLE DIRECTION IS REFUSED, and the asymmetry is the point: ENABLING is always
# allowed, so a unit that somehow came up disabled can be recovered from this page. Refusing
# both directions would leave the page able only ever to take a unit away.
#   the PLAYER -- RB_AUTOSTART is read-only for exactly this reason: disabling the appliance's
#     own player at boot, from a browser, could leave a unit that never starts rbp;
#   THIS PAGE -- disabling the page at boot takes away the page you would use to turn it back
#     on. That is not a warning, it is the end of the thread.
NEVER_DISABLE = {
    PLAYER_UNIT: "The player is the appliance. Disabling it at boot from a browser could "
                 "leave a unit that never starts rbp -- the rule RB_AUTOSTART is read-only "
                 "for. It is set in rb.local.conf.",
    CONF_UNIT: "Disabling this page at boot would take away the page you would use to turn "
               "it back on. Set it from a shell if you really mean it.",
}

# The settings the page reports today and will edit in the next slice. Read-only here.
# The settings, in the two groups the operator asked for. The split is by what they are
# ABOUT -- the viewer's own knobs against everything the player and the unit use -- because
# that is how they get looked for. CONF_KEYS is the union, and it is what the page fetches.
VNC_KEYS = ("RB_VNC", "RB_VNC_PORT", "RB_VNC_HTTP_PORT", "RB_VNC_FPS", "RB_VNC_MODE",
            "RB_VNC_LIVE", "RB_VNC_INPUT", "RB_PASSWORD")
RBP_KEYS = ("RB_PREWARM", "RB_POINT_KIND", "RB_MIDI_MAP", "RB_AUDIO_DEV", "RB_BOOTSCREEN",
            "RB_FB_LIE_BPP", "RB_VERBOSE", "RB_CONF_HTTP_PORT")
CONF_KEYS = VNC_KEYS + RBP_KEYS


def conf_keys():
    """Every key the page can show a value for: the list above, PLUS every key in the editor's
    schema.

    DERIVED, NOT LISTED, and that is a fix rather than tidiness. `conf_values()` only fetches
    the keys it is handed, and the settings table renders whatever the schema holds -- so a key
    added to the schema and forgotten here reads as ``(empty)``. Silently: RB_VNC_WEB was set
    to 1 on this unit and the page said "(empty)" for it, which is what an unset key looks
    like. A list that has to be kept in step with another list will not be."""
    keys = list(CONF_KEYS)
    if confedit:
        for e in confedit.SCHEMA:
            if e["key"] not in keys:
                keys.append(e["key"])
    return tuple(keys)


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


def writes_available():
    return confedit is not None


def set_flash(text):
    """Record the message the next full page render shows, once."""
    _FLASH["text"] = text


def take_flash():
    """Take that message and clear it -- the full render's half of the pair."""
    text = _FLASH["text"]
    _FLASH["text"] = ""
    return text


def peek_flash():
    """Read it WITHOUT clearing it -- the poll's half. A refresh racing a write must not eat a
    message the operator has not seen yet."""
    return _FLASH["text"]


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


def share_set(on):
    """The live sharing switch: what the viewer re-reads each turn. No restart, and it
    works whether or not the viewer is running -- the file IS the request."""
    ok, why = set_switch("live", "on" if on else "off")
    if not ok:
        return False, why
    return True, ("sharing on -- the screen is being served" if on else "sharing off")


def mode_set(value):
    """The live encoding switch: `raw` or `hwjpeg`, the choice the viewer re-reads each turn.

    THE VALUES ARE A WHITELIST, for the same reason the unit names in UNITS are: this is
    written straight into a file the viewer acts on, and a switch is not the place to find out
    that something unexpected arrived. The page only ever renders these two words."""
    if value not in ("raw", "hwjpeg"):
        return False, "the encoding is either raw or hwjpeg"
    ok, why = set_switch("mode", value)
    if not ok:
        return False, why
    return True, "encoding now %s -- the viewer picks it up on its next turn" % value


def _sw_int(lo, hi):
    """A validator for an integer switch: the word to write, or None to refuse."""
    def check(v):
        try:
            n = int(v)
        except (TypeError, ValueError):
            return None
        return str(n) if lo <= n <= hi else None
    return check


# THE LIVE SWITCHES THIS PAGE CAN WRITE, AND THE ONLY PLACE THEIR VALUES ARE CHECKED. Every row
# in the viewer settings table that is not an on/off pair is backed by an entry here, so a
# control cannot exist without a validator -- and adding one means adding both, in one place,
# rather than three near-identical handlers that drift apart. The names are the switch file's:
# set_switch writes /run/rblive4/vnc.<name>, which is what vncserve reads each turn.
LIVE_SWITCHES = {
    "fps": _sw_int(1, 30),
    "jpeg_dev": lambda v: v if v in ("/dev/video11", "/dev/video31") else None,
    "jpeg_quality": _sw_int(0, 100),
}


def preview_on():
    """Whether this page embeds the preview image. Off unless the file says otherwise: the
    stream costs the viewer a frame per tick while it is open, and a page that is cheap to leave
    open is the point of this one."""
    return _text(PREVIEW_FILE).strip() == "on"


def preview_set(on):
    """Turn the preview image on or off for this page. One file in /run, the same place and the
    same four-byte shape the viewer's own switches use."""
    try:
        os.makedirs(RUN_DIR, exist_ok=True)
        with open(PREVIEW_FILE, "w") as f:
            f.write("on\n" if on else "off\n")
        return True, ("the preview image is on -- this page is watching the viewer's stream"
                      if on else "the preview image is off")
    except OSError as e:
        return False, "could not write %s: %s" % (PREVIEW_FILE, e)


def input_set(on):
    """The live pointer-injection switch: whether a click in a VNC client's picture becomes a
    real press on the glass.

    IT IS THE ONE SWITCH HERE THAT CAN CHANGE THE GLASS RATHER THAN THE PICTURE. On, every
    client's mouse is the operator's finger; off, the picture is watch-only again. The page
    says so under the row rather than leaving it to be discovered."""
    ok, why = set_switch("input", "on" if on else "off")
    if not ok:
        return False, why
    return True, ("pointer injection on -- a click in the picture now presses the glass"
                  if on else "pointer injection off -- the picture is watch-only again")


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


def service_action(unit):
    """Start or restart ONE unit off the page's own list, and say which it did.

    THE UNIT IS A WHITELIST LOOKUP AND NEVER A STRING OFF THE WIRE. It arrives in a form
    body and it becomes an argument to systemctl, so the membership test below IS the
    security boundary: no name that is not a literal in UNITS can reach an argv array.
    A missing `unit` is just a name that is not in the tuple, so it is refused the same way.

    THE PLAYER KEEPS ITS OWN PATH, and that is not tidiness. `restart_player()` holds the
    lock, enforces the cooldown, and starts the boot screen first; routing the player row
    through the generic branch below would drop all three, and two overlapping restarts of
    rblive4 are measured to wedge rbp on this unit. So the player never takes that branch.

    THIS PAGE'S OWN UNIT IS FIRED AND NOT WAITED FOR. `systemctl restart rblive4-conf` kills
    the process answering this request, so waiting on it would mean the browser never gets a
    reply and the operator sees a failed button that in fact worked. Spawned detached, with
    the redirect already on its way out.

    NO SHELL, ANYWHERE: every branch passes a constant argv. The only variable is the unit,
    and the membership test above bounds it."""
    if unit not in UNITS:
        return False, "no service by that name on this page"
    if unit == PLAYER_UNIT:
        return restart_player()
    # `restart` is the honest verb for both cases -- systemd starts a stopped unit on a
    # restart -- but the label the operator reads follows the state, so say what it did.
    verb = "restart" if unit_state(unit)[0] == "active" else "start"
    if unit == CONF_UNIT:
        try:
            subprocess.Popen([SYSTEMCTL, verb, unit], start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            return False, "could not run systemctl: %s" % e
        return True, ("this page's own service is restarting -- it blinks for a few seconds"
                      if verb == "restart" else
                      "this page's own service is starting")
    rc, out = _run_rc([SYSTEMCTL, verb, unit], timeout=30)
    if rc != 0:
        return False, "systemctl %s %s returned %d: %s" % (verb, unit, rc, out)
    return True, "%s %s requested" % (unit, verb)


def boot_set(unit, on):
    """Enable or disable ONE unit's START AT BOOT, and say which it did.

    THIS IS THE `setting` COLUMN'S QUESTION, and it is not /service's. `systemctl is-enabled`
    asks whether a symlink will start the unit at boot; it says nothing about whether the unit
    is running. A unit can be running and not set to return -- which is precisely the state
    rblive4-vnc was in, and why this control exists.

    IT STARTS AND STOPS NOTHING. `enable` only writes the symlink; the unit keeps doing
    whatever it was doing. That separation is deliberate: the two questions are answered by
    two different buttons, so neither can surprise you about the other.

    The unit name is a whitelist lookup exactly as /service's is, no shell is involved, and
    NEVER_DISABLE is checked here as well as in the handler -- both read the same dict, so they
    cannot disagree about which units may not be disabled."""
    if unit not in UNITS:
        return False, "no service by that name on this page"
    if unit in NEVER_DISABLE and not on:
        return False, NEVER_DISABLE[unit]
    # RECORD IT FIRST, THEN DO IT -- the order `vnc_set` used, and for its reason: a change
    # that cannot be written down should not be made, or the two mechanisms part company and
    # the parting is invisible until an install.
    key = BOOT_SETTING.get(unit)
    if key and writes_available():
        ok, why = confedit.write_local(LOCAL_CONF, key, "1" if on else "0")
        if not ok:
            return False, ("could not record %s, so nothing was changed: %s" % (key, why))
    verb = "enable" if on else "disable"
    rc, out = _run_rc([SYSTEMCTL, verb, unit], timeout=30)
    if rc != 0:
        return False, "systemctl %s %s returned %d: %s" % (verb, unit, rc, out)
    return True, ("%s will start at boot" % unit if on
                  else "%s will NOT start at boot" % unit)


# --- there are no sessions ------------------------------------------------------------------
#
# THERE IS NO SIGN-IN ON THIS PAGE, by the operator's decision, so there is nothing to hold a
# session for: no cookie, no CSRF token, no expiry, no login rate limit. What protected the
# writes that remains is `_same_origin()` in the handler -- an Origin/Referer check, which is
# what this page ran on anyway once RB_CONF_AUTH defaulted off. That is a real limit and the
# page says it out loud at the top rather than implying a lock it does not have.
#
# The consequence worth naming: with no CSRF token, a write is refused only if the browser
# sends an Origin that is not this page's. A script that can set its own Origin is not stopped
# by that. The page is a LAN instrument on a single-operator appliance; this is the posture the
# operator chose, twice, and it is written down here so the next reader does not assume more.


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
/* A HEADER ROW MUST NOT LOOK LIKE A DATA ROW. It did: `th` was dim, normal-weight body text,
   and the columns under it hold the very same words -- a row of headings reading "unit active
   enabled" is indistinguishable from a service called "unit" that is active and enabled. So a
   header now takes the treatment the section headings already use: small, upper case, spaced. */
th { color: #6b7480; font-weight: normal; text-transform: uppercase; font-size: 11px;
     letter-spacing: .08em; }
.k { color: #8b94a3; width: 260px; }
/* The services table's action column: one small button per row, kept on one line so a
   long unit name or a wide button cannot push the column about. */
.act { white-space: nowrap; }
/* A two-button pair shows BOTH states and draws the one in force as pressed -- the same idea
   as an on/off pair anywhere else, and it means the control states the current value rather
   than only the value a press would give. */
.cur { background: #2c3a2c; border-color: #4a6b4a; color: #dceadc; }
/* A button the page will refuse, drawn as one: the startup pair SHOWS the direction it will not
   take rather than hiding it, so it has to look unavailable rather than merely be inert. */
button[disabled] { opacity: .4; cursor: not-allowed; }
/* The settings tables: label, then the control, then a short hint. The control column never
   wraps, so the column stays a column; the hint is dim and small because it is a note ABOUT
   the setting rather than part of it. */
.v { white-space: nowrap; }
.h { color: #6b7480; font-size: 12px; padding-left: 12px; }
.ok { color: #a3be8c; } .bad { color: #bf616a; } .warn { color: #ebcb8b; }
.dim { color: #6b7480; }
pre { background: #0d0f14; border: 1px solid #232833; border-radius: 4px; padding: 8px 10px;
      overflow-x: auto; color: #c8d0da; margin: 4px 0 0; max-width: 900px; }
.state { font-size: 15px; }
.note { color: #8b94a3; font-size: 12px; margin-top: 4px; }
/* The (i) that carries a setting's description. Hidden until hover or focus, in CSS alone --
   no script, nothing external, and it is in the DOM either way, so the text still exists for
   a screen reader, for a copy-paste, and for a browser with the CSS stripped. */
.info { display: inline-block; width: 15px; height: 15px; line-height: 15px; text-align: center;
        border-radius: 50%; border: 1px solid #4a5568; color: #8b94a3; font-size: 10px;
        cursor: help; position: relative; margin-left: 7px; vertical-align: 2px; font-style: normal; }
.info:hover, .info:focus { color: #eceff4; border-color: #8b94a3; outline: none; }
.info .tip { display: none; position: absolute; z-index: 9; left: -18px; top: 19px;
             width: 330px; max-width: 62vw; padding: 7px 10px; border-radius: 4px;
             background: #1b2733; border: 1px solid #33465c; color: #cfe3f5;
             font-size: 12px; line-height: 1.45; text-align: left; white-space: normal;
             box-shadow: 0 3px 10px rgba(0, 0, 0, .45); }
.info:hover .tip, .info:focus .tip { display: block; }
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


# --- what has been saved but not yet applied ------------------------------------------
#
# A saved line takes effect when the process that READS it next starts, and that is the
# operator's decision -- this page never restarts anything on a save, because a save that
# blanks the screen would be a trap. So each unit that owes a restart gets a stamp beside it,
# and the stamp is discharged by comparing it with the unit's own
# ActiveEnterTimestampMonotonic: when the unit next starts that number moves, and the debt is
# paid. Both are monotonic and on this boot, and the stamp lives in /run (tmpfs), so a reboot
# clears both together and cannot leave a stale debt behind.


def unit_for(restart):
    """The unit that has to restart for a value to take effect, or None when nothing does."""
    if restart == "player":
        return PLAYER_UNIT
    if restart == "viewer":
        return VIEWER_UNIT
    if isinstance(restart, str) and restart.startswith("service:"):
        return restart.split(":", 1)[1]
    return None


def apply_note(restart):
    """What applies a value, in the operator's words.

    THE `service:` CASE NAMES THE SECTION THE BUTTON IS ACTUALLY IN, and that is not
    book-keeping: the viewer's enable/disable button moved out of the actions section into
    viewer settings, and this sentence went on saying "under actions" -- a hint pointing at a
    section that no longer holds the button is worse than no hint, because it sends the reader
    somewhere confidently wrong. Every other `service:` row names a unit the services table
    starts and restarts, which is what applies it."""
    if restart == "player":
        return "restart the player to apply -- the button is under services"
    if restart == "viewer":
        return "restart the viewer to apply"
    if isinstance(restart, str) and restart.startswith("service:"):
        # EVERY `service:` ROW NOW HAS A REAL CONTROL, and it is the same one for all of them:
        # the enable/disable in the services table's startup column, which writes both the
        # systemd symlink and this line (see BOOT_SETTING). The sentence can name it exactly.
        unit = restart.split(":", 1)[1]
        return "changed by the enable/disable on the %s row under services" % unit
    return ""


def unit_stamp(unit):
    v = _run([SYSTEMCTL, "show", "-p", "ActiveEnterTimestampMonotonic", "--value", unit])
    return v.strip() or None


def pending_path(unit):
    return os.path.join(RUN_DIR, "conf.dirty." + unit)


def mark_pending(restart):
    unit = unit_for(restart)
    if not unit:
        return
    stamp = unit_stamp(unit)
    if stamp is None:
        return
    try:
        os.makedirs(RUN_DIR, exist_ok=True)
        with open(pending_path(unit), "w") as f:
            f.write(stamp + "\n")
    except OSError:
        pass


def pending_unit(unit):
    """True while a value saved for this unit has not been applied. Clears the stamp itself
    when the unit turns out to have started since."""
    try:
        with open(pending_path(unit)) as f:
            was = f.read().strip()
    except OSError:
        return False
    now = unit_stamp(unit)
    if now is None or now == was:
        return True
    try:
        os.unlink(pending_path(unit))
    except OSError:
        pass
    return False


def topic_pending(topic):
    units = {unit_for(e.get("restart", "")) for e in confedit.SCHEMA
             if confedit.topic_of(e["key"]) == topic and not e.get("readonly")} \
        if confedit else set()
    return sorted(u for u in units if u and pending_unit(u))


# --- the settings form ---------------------------------------------------------------


def _set_form(key, e, current):
    """One row's form, by the schema's TYPE -- so a value cannot be entered in a shape the
    validator will refuse without the operator being told why.

    No hidden CSRF field, because there is no session to hold one: the write is held back by
    the same-origin check in the handler and by nothing else. See the note where the sessions
    section used to be."""
    hid = '<input type=hidden name=key value="%s">' % esc(key)
    open_form = '<form method=post action=/set>' + hid
    t = e["type"]
    if t in ("enum", "bool"):
        choices = e["choices"] if t == "enum" else ["0", "1"]
        opts = "".join(
            '<option value="%s"%s>%s</option>'
            % (esc(c), " selected" if str(current) == str(c) else "",
               esc(c if c != "" else "(empty)"))
            for c in choices)
        return (open_form + '<select name=value>' + opts +
                '</select> <button>save</button></form>')
    if t == "int":
        return (open_form + '<input name=value type=number min="%s" max="%s" value="%s" '
                'size=6> <button>save</button></form>'
                % (esc(e.get("lo", "")), esc(e.get("hi", "")), esc(current)))
    if e.get("secret"):
        # Never pre-filled: the page does not know the password and must not print it, so a
        # blank field means "leave it alone" rather than "set it to empty".
        return (open_form + '<input name=value type=password placeholder="(unchanged)" '
                'size=18> <button>set</button></form>')
    return (open_form + '<input name=value value="%s" size=26> <button>save</button></form>'
            % esc(current))


def _info(text, key=""):
    """The (i) beside a label, carrying that setting's description on hover -- and the name it
    has in the file, because the reader about to hand-edit `rb.local.conf` is exactly the
    reader who hovers here, and printing the key on every row was half of the noise this table
    used to have.

    `tabindex=0` so it also appears on keyboard focus -- which is the only reason to bother:
    without it the description would be reachable by mouse alone."""
    parts = [esc(text)] if text else []
    if key:
        parts.append("<code>%s</code>" % esc(key))
    if not parts:
        return ""
    return ('<span class=info tabindex=0>i<span class=tip>%s</span></span>'
            % "<br>".join(parts))


def render_settings(a, conf, topic, auth):
    """One group of settings, EDITABLE.

    Every field is built from confedit's schema, which is the same object the writer validates
    against -- so the form cannot offer something `confedit.write_local()` would refuse, and a
    setting appears here by being added to the schema and nowhere else.

    THREE COLUMNS, AND ONLY THREE. It used to be label / a dim `KEY = value` line / the control
    / a note, which put the value on screen twice and printed the SAME sentence about
    restarting on all twenty rows -- so the one line that mattered was buried in twenty copies
    of one that did not. What is left is the label, the control, and a short hint only where
    there is something to say. Nothing was dropped: the key name moved into the (i), the value
    moved into the control that already shows it, and the restart sentence is said once, for
    the group, naming the units the schema says these settings belong to."""
    owed = topic_pending(topic)
    if owed:
        a('<div class="note bad">saved, not applied yet: restart %s</div>'
          % esc(", ".join(owed)))
    if not confedit:
        a('<div class=note>The config editor is not installed beside this page, so nothing '
          'here can be changed. Re-run install.sh.</div>')
        return
    # ONE restart sentence for the group. Named from the schema rather than written by hand,
    # so it cannot go stale when a setting changes which unit it belongs to -- and built only
    # from rows this form can actually SAVE, because a `service:` row is changed by a button
    # under actions and naming its unit here would promise this table something it cannot do.
    units = sorted(u for u in {unit_for(e.get("restart", "")) for e in confedit.SCHEMA
                               if confedit.topic_of(e["key"]) == topic
                               and not e.get("readonly")
                               and not (isinstance(e.get("restart"), str)
                                        and e["restart"].startswith("service:"))} if u)
    if units:
        a('<div class=note>Saved values are picked up when %s next starts &mdash; the '
          'buttons are on <a href="#services">services</a>.</div>' % esc(" or ".join(units)))
    a("<table>")
    for e in confedit.SCHEMA:
        k = e["key"]
        if confedit.topic_of(k) != topic:
            continue
        cur = confedit.reads_as(e) or (conf.get(k, "") or "(empty)")
        a("<tr><td class=k>%s%s</td>"
          % (esc(e.get("label", k)), _info(e.get("help") or e.get("why", ""), k)))
        if e.get("readonly"):
            a('<td class=v>%s</td><td class=h>read-only</td>' % esc(cur))
        elif not auth["writes"]:
            a('<td class=v>%s</td><td class=h>not editable here: no config editor</td>'
              % esc(cur))
        elif isinstance(e.get("restart"), str) and e["restart"].startswith("service:"):
            # No control: this one is set by a button elsewhere on the page, and a second way
            # to change it would be a second thing to keep honest. The hint names the section
            # that button is really in.
            a('<td class=v>%s</td><td class=h>%s</td>'
              % (esc(cur), esc(apply_note(e["restart"]))))
        else:
            hint = ("(set)" if conf.get(k) else "(not set)") if e.get("secret") else ""
            a('<td class=v>%s</td><td class=h>%s</td>'
              % (_set_form(k, e, conf.get(k, "")), esc(hint)))
        a("</tr>")
    a("</table>")


def render(conf, pid, frames, rate, facts, services, switches, depth, req_host="", auth=None,
           data_only=False):
    """The whole page, or -- with `data_only` -- just the two halves the poll re-reads.

    They are one function on purpose. The fragment MUST be drawn by the same code as the
    first load, or the page and its updates drift apart, and the drift shows up as a figure
    that changes when you reload and not otherwise. What separates them is only the shell:
    the head, the flash, the actions and the footer, none of which the poll touches."""
    auth = auth or {"flash": "", "writes": False}
    h = []
    a = h.append
    if not data_only:
        a("<!doctype html><html lang=en><head><meta charset=utf-8>")
        a('<meta name=viewport content="width=device-width,initial-scale=1">')
        a("<title>%s - rbp status</title><style>%s</style></head><body>"
          % (esc(hostname()), CSS))
        a("<h1>%s &mdash; rbp status</h1>" % esc(hostname()))
        # No "the data re-reads every Ns" any more: it described the machinery rather than the
        # page, and the poll is invisible when it works -- which is the whole design. The clock
        # stays, because a stale page and a live one look identical without it.
        a('<div class=sub>page %d &middot; %s</div>'
          % (PORT, esc(time.strftime("%H:%M:%S"))))
        if auth["flash"]:
            a('<div class="flash%s">%s</div>'
              % (" bad" if auth["flash"].startswith("FAILED") else "",
                 esc(auth["flash"])))
        # The sections, as items down the left, in the order they are read. There is no
        # notice above them: the page had one for a while -- a paragraph about having no
        # password -- and the operator took it off, which is the right shape for it. A page
        # that says nothing about a lock it does not have is not hiding anything; doctor.sh
        # reports it for anyone who wants it in words.
        a('<div id=layout><nav id=nav><b>%s</b>' % esc(hostname()))
        # The tab is called `status`; inside it the headings are still `player`,
        # `device info` and `boot info`, each naming the block it opens. The tab is the
        # whole of "how is it doing"; `player` was only ever the first third of that.
        for anchor, label in (("player", "status"), ("services", "services"),
                              ("viewer", "viewer settings"),
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
    # THE LOG IS NAMED, NOT QUOTED. The six-line tail was here because rbp's own log is the
    # only place some states are legible at all -- but a tail is a guess about which lines
    # matter, and the reader who wants it can open the file. The path says where; the frame
    # counter above says whether it is worth opening.
    a("<tr><td class=k>log</td><td>%s</td></tr>" % esc(RBP_LOG))
    a("</table>")
    # --- and the machine it is running on, in the same tab ---
    a("<h2>device info</h2><table>")
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

    # --- and the account of the last start, in the same tab ---
    # --- the launcher's own account of the last start ---
    stage = _text(BOOT_STAGE).strip()
    blog = tail(BOOT_LOG, 14)
    a("<h2>boot info</h2>")
    if stage:
        a('<div class=note>last boot-screen stage: %s</div>' % esc(stage))
    a("<pre>%s</pre>" % esc("\n".join(blog) if blog else "(no %s)" % BOOT_LOG))
    a("</section>")

    a("</section>")

    # --- services ---
    a('<section id=services>')
    # THE HEADINGS NAME WHAT THE READER IS ASKING, not the systemd verb behind it. "Status" is
    # `systemctl is-active` -- is it running NOW -- and "startup" is `systemctl is-enabled` --
    # will it come back at boot. The two differ, and that difference is the whole reason
    # the columns are separate: a unit can be running and not set to return (rblive4-vnc was,
    # after the button that used to set this was retired).
    a("<h2>services</h2><table><tr><th>unit</th><th>Status</th><th>startup</th>"
      "<th>actions</th></tr>")
    # ONE BUTTON PER ROW, and the player's row is the one that differs. Its button opens the
    # two-step confirm at /restart instead of firing /service directly, because that is the
    # control that blanks the screen for a quarter of a minute and pressing it by accident
    # should cost one more click; every other unit starts or restarts in one.
    can = auth["writes"]
    for u in UNITS:
        act, en = services.get(u, ("unknown", "unknown"))
        c = "ok" if act == "active" else ("dim" if act in ("inactive", "unknown") else "bad")
        if not can:
            btn = ""
        elif u == PLAYER_UNIT:
            btn = ('<form method=post action=/restart>'
                   '<button name=step value=ask>restart</button></form>')
        else:
            btn = ('<form method=post action=/service>'
                   '<input type=hidden name=unit value="%s">'
                   '<button>%s</button></form>'
                   % (esc(u), "restart" if act == "active" else "start"))
        # THE STARTUP COLUMN IS WHERE YOU CHANGE STARTUP. It reports `is-enabled`, so the
        # control that flips it belongs on that word rather than in another table or another
        # section. One button, labelled with what it will DO -- enable while it is disabled,
        # disable while it is enabled -- so the row cannot be misread. The two units whose boot
        # start is not the operator's to flip carry the page's usual (i) with the reason.
        if not can:
            # No editor, so nothing here can be written: state the value and stop.
            boot_cell = esc(en)
        else:
            # BOTH STATES AS BUTTONS, with the one in force drawn as pressed -- so the control
            # says what IS, not only what a press would do. On a NEVER_DISABLE unit the
            # `disabled` button carries `disabled`: greyed and unpressable, with the reason on
            # the (i) beside it.
            fixed = u in NEVER_DISABLE
            boot_cell = ('<form method=post action=/boot>'
                         '<input type=hidden name=unit value="%s">'
                         '<button name=action value=on%s>enabled</button>'
                         '<button name=action value=off%s disabled>disabled</button></form>'
                         % (esc(u),
                            " class=cur" if en == "enabled" else "",
                            " class=cur" if en != "enabled" else "")) if fixed else \
                        ('<form method=post action=/boot>'
                         '<input type=hidden name=unit value="%s">'
                         '<button name=action value=on%s>enabled</button>'
                         '<button name=action value=off%s>disabled</button></form>'
                         % (esc(u),
                            " class=cur" if en == "enabled" else "",
                            " class=cur" if en != "enabled" else ""))
            if fixed:
                boot_cell += _info(NEVER_DISABLE[u])
        a("<tr><td>%s</td><td class=%s>%s</td><td class=v>%s</td>"
          "<td class=act>%s</td></tr>"
          % (esc(u), c, esc(act), boot_cell, btn))
    a("</table>")
    if can:
        a('<div class=note><b>restart</b> stops the unit and starts it again -- on a unit '
          'that is not running it is a plain start, and the button says which it will be. '
          'The player is the destructive one: the screen goes dark for about fifteen seconds '
          'while the boot screen draws its stages, and a set in progress stops, which is why '
          'its button asks first and the others do not. Saving a setting never restarts '
          'anything; these buttons are the only thing here that does.</div>')
        a('<div class=note>The two buttons on a <b>startup</b> row are the two states, and the '
          'one the unit is in is drawn as pressed. They answer the other question from '
          '<b>restart</b>: whether it comes back after a power cut. Neither changes anything '
          'now -- a unit can be running and not set to return, which is what the two columns '
          'are for.</div>')
    elif auth["writes"]:
        a('<div class=note>Starting a service needs the password -- sign in under '
          '<a href="#actions">actions</a>.</div>')
    a("</section>")


    # --- the viewer: its switches, with a control on the two that are live, and its ports ---
    a('<section id=viewer>')
    a("<h2>viewer settings</h2><table>")
    # THE TWO LIVE SWITCHES CARRY THEIR OWN BUTTONS, on the row that reports them, so the state
    # and the thing that changes it are one line rather than two tables a paragraph apart. They
    # can, because they are ordinary files the viewer re-reads on its next turn: no restart, and
    # they work whether or not it is running.
    #
    # THE VIEWER'S OWN SERVICE IS DELIBERATELY NOT HERE. It used to have a row of its own --
    # "the viewer", with enable/disable -- which started the service AND wrote RB_VNC for the
    # next boot. Starting and restarting it is the services table's job now, and a second way to
    # do it would be a second thing to keep honest; RB_VNC itself is a read-only row next door,
    # with why.
    # A SWITCH FILE IS A SEED, NOT THE ONLY INPUT. With no file the viewer uses the RB_VNC_*
    # default that vnc-run.sh handed it at startup, so a row shows the EFFECTIVE state and
    # marks the ones that came from that default. "(absent)" told the reader nothing -- and for
    # pointer injection the nothing it told them was whether their screen was clickable, which
    # on this unit it was: the file was gone and RB_VNC_INPUT is 1.
    def eff(name, default):
        raw = switches.get(name)
        return (raw, False) if raw != "(absent)" else (default, True)

    live_v, live_d = eff("live", "on" if conf.get("RB_VNC_LIVE", "") == "1" else "off")
    mode_v, mode_d = eff("mode", conf.get("RB_VNC_MODE", "") or "raw")
    inp_v, inp_d = eff("input", "on" if conf.get("RB_VNC_INPUT", "") == "1" else "off")

    def pair(action, name, values, current):
        """One form, one button per value, with the value IN FORCE drawn as pressed.

        THERE IS NO SEPARATE STATE COLUMN, because the pair states the value itself -- the same
        shape the services table's startup cell uses. A row is then a thing and its control,
        rather than a thing, its value, and its control, with the value said twice."""
        if not auth["writes"]:
            return ""
        return ('<form method=post action=%s>%s</form>'
                % (action,
                   "".join('<button name=%s value=%s%s>%s</button>'
                           % (name, v, " class=cur" if v == current else "", v)
                           for v in values)))

    def cell(word, d, control):
        """The control, or the word when there is no control to have -- plus the (default) mark,
        which has to travel with the value wherever the value is shown."""
        return (control or esc(word)) + (" <span class=dim>(default)</span>" if d else "")

    a("<tr><td class=k>sharing (vnc.live)</td><td class=v>%s</td></tr>"
      % cell(live_v, live_d, pair("/share", "action", ("on", "off"), live_v)))
    a("<tr><td class=k>encoding (vnc.mode)</td><td class=v>%s</td></tr>"
      % cell(mode_v, mode_d, pair("/mode", "value", ("raw", "hwjpeg"), mode_v)))
    a("<tr><td class=k>pointer injection (vnc.input)</td><td class=v>%s</td></tr>"
      % cell(inp_v, inp_d, pair("/input", "value", ("on", "off"), inp_v)))
    # AND THE THREE THAT ARE LIVE WITHOUT BEING PAIRS: a rate, an encoder node and a quality.
    # They are switch files like the ones above -- the page writes them, the viewer reads them
    # every turn -- so a change lands on the NEXT FRAME and can be judged on the preview below,
    # rather than after a restart. A missing file falls back to the RB_VNC_* value the viewer
    # was started with, which is what a missing file means everywhere on this page.
    fps_v, fps_d = eff("fps", conf.get("RB_VNC_FPS", "") or "12")
    jdev_v, jdev_d = eff("jpeg_dev", conf.get("RB_VNC_JPEG_DEV", "") or "/dev/video11")
    jq_v, jq_d = eff("jpeg_quality", conf.get("RB_VNC_JPEG_QUALITY", "") or "0")

    def swfield(name, value, lo, hi):
        """A number in a box and a button, for a switch whose value is a number."""
        if not auth["writes"]:
            return ""
        return ('<form method=post action=/switch><input type=hidden name=name value="%s">'
                '<input name=value type=number min="%d" max="%d" value="%s" size=3> '
                '<button>set</button></form>' % (esc(name), lo, hi, esc(value)))

    def swpair(name, options, current):
        """A pair for a /switch row: one hidden field naming the switch, one button per value."""
        if not auth["writes"]:
            return ""
        return ('<form method=post action=/switch><input type=hidden name=name value="%s">%s'
                '</form>'
                % (esc(name),
                   "".join('<button name=value value="%s"%s>%s</button>'
                           % (esc(v), " class=cur" if v == current else "", esc(label))
                           for v, label in options)))

    a("<tr><td class=k>frame rate</td><td class=v>%s</td></tr>"
      % cell(fps_v, fps_d, swfield("fps", fps_v, 1, 30)))
    a("<tr><td class=k>JPEG encoder</td><td class=v>%s</td></tr>"
      % cell(jdev_v, jdev_d,
             swpair("jpeg_dev", (("/dev/video11", "video"), ("/dev/video31", "image")), jdev_v)))
    a("<tr><td class=k>JPEG quality</td><td class=v>%s</td></tr>"
      % cell(jq_v, jq_d, swfield("jpeg_quality", jq_v, 0, 100)))

    # THE PAGE'S OWN PREFERENCE, in the same shape as the switches above it -- a pair with the
    # value in force as pressed -- because a reader should not have to know which of these rows
    # writes a file the viewer reads and which one only writes a file this page reads.
    prev = "on" if preview_on() else "off"
    a("<tr><td class=k>preview</td><td class=v>%s</td></tr>"
      % cell(prev, False, pair("/preview", "action", ("on", "off"), prev)))
    rfb = conf.get("RB_VNC_PORT", "")
    pg = conf.get("RB_VNC_HTTP_PORT", "")
    a("<tr><td class=k>RFB port %s</td><td class=v>%s</td></tr>"
      % (esc(rfb), "listening" if port_open(rfb) else "not listening"))
    a("<tr><td class=k>viewer page port %s</td><td class=v>%s</td></tr>"
      % (esc(pg), "listening" if port_open(pg) else "not listening"))
    a("</table>")
    if live_d or mode_d or inp_d:
        a('<div class=note>A state marked <b>(default)</b> has no switch file: that is the '
          '<code>RB_VNC_*</code> value the viewer was started with, so it is what is really in '
          'force -- and what comes back after a power cut, because <code>/run</code> is '
          'cleared.</div>')
    # The paragraph that used to sit here -- pointer injection makes the picture an input
    # surface, and the top menu's seventh column is USB STOP -- is gone at the operator's ask.
    # The fact is not lost: vncserve's own :5902 page says it in full next to the same switch,
    # and docs/20-config-page.md records it. This page states the state.
    # AND THE IMAGE IS THE ROW'S TO SHOW. Two things can stop it besides the switch: the stream
    # is the VIEWER's, so it exists only while the viewer is running and sharing is on -- and
    # saying which of those is missing is worth more than a blank space where a picture was.
    if preview_on():
        if live_v == "on" and port_open(pg):
            # The preview belongs to the viewer (it owns the capture) and stays there; this page
            # only points at it. The host comes from the REQUEST, not from this unit's own
            # hostname: the browser is on the other end of the LAN and may not be able to resolve
            # `rpidev01` at all, whereas it demonstrably resolved whatever it typed to get here.
            host_only = req_host.rsplit(":", 1)[0] if req_host else hostname()
            a('<div class=shot><img src="//%s:%s/preview.mjpg" alt="screen preview"></div>'
              % (esc(host_only), esc(pg)))
        else:
            a('<div class=note>Nothing to show yet: the preview is the viewer\'s own stream, so '
              'it needs the viewer running and sharing ON. It is not encoded for a page nobody '
              'is watching.</div>')

    a("</section>")
    a("</div>")                      # end of the first live half

    a('<div id=data2>')


    # --- the settings, in the two groups the operator asked for: what the VIEWER is, and
    # what the player and the unit are. Both are shown read-only; the form is the next slice.
    a('<section id=vncsettings>')
    a("<h2>vnc settings</h2>")
    render_settings(a, conf, "vnc", auth)
    a("</section>")

    a('<section id=rbpsettings>')
    a("<h2>rbp settings</h2>")
    render_settings(a, conf, "rbp", auth)
    # The depth pair is the one reading that spans two sources -- the lie in the conf and the
    # word inside the deployed player -- so it is not a row in either schema group. Shown,
    # never editable: a pair that disagrees is rbp SIGSEGV to a black screen.
    a("<table>")
    lie = conf.get("RB_FB_LIE_BPP", "")
    a("<tr><td class=k>player's layer format word%s</td><td>%s</td></tr>"
      % (_info("Read from the two immediates the depth patch rewrites in the deployed player."),
         esc("not readable" if depth is None else "%s bpp" % depth)))
    if depth is not None and lie:
        agree = str(depth) == str(lie)
        # The DANGER stays visible when it is true ("NO - rbp will not start"); only the
        # explanation of what the pair is goes behind the (i).
        a("<tr><td class=k>depth pair agrees?%s</td><td class=%s>%s</td></tr>"
          % (_info("Shown and never editable: the pair is half of the player build, and a "
                   "value that disagrees is rbp SIGSEGV to a black screen."),
             "ok" if agree else "bad",
             "yes" if agree else "NO &mdash; rbp will not start until these match"))
    a("</table>")
    a("</section>")

    a("</div>")                      # end of the second live half

    if not data_only:
        a("</main></div>")           # close #main and #layout
        # NO FOOTER. It held a pointer to the doc, and a pointer the page does not need: the
        # reader of this page is standing at the machine, and the doc is in the tree beside
        # the script that draws it. It had also accumulated a duplicated sentence about the
        # password -- which is the shape a footer takes when it has nothing of its own to say.
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
        """A POST that says it came from somewhere else is refused.

        THIS IS THE ONLY LOCK ON THE WRITES NOW, and it is worth being exact about what it is:
        a browser sends `Origin` on a cross-site POST and this compares it to `Host`, so a
        page the operator merely *visits* cannot drive this one. It is not a secret and it does
        not stop a client that sets its own headers -- there is no token, because there is no
        session to keep one in. The page states that plainly at the top instead of implying a
        lock it does not have."""
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
        elif path in ("/share", "/mode", "/input", "/preview", "/switch", "/restart", "/service",
                      "/boot", "/set"):
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
        if path == "/share":
            self._share(form)
        elif path == "/mode":
            self._mode(form)
        elif path == "/input":
            self._input(form)
        elif path == "/preview":
            self._preview(form)
        elif path == "/switch":
            self._switch(form)
        elif path == "/restart":
            self._restart(form)
        elif path == "/service":
            self._service(form)
        elif path == "/boot":
            self._boot(form)
        elif path == "/set":
            self._set(form)
        else:
            self._send(404, self._msg("no such page here",
                                      'Try <a href="/">/</a> instead.'))

    def _share(self, form):
        """The live sharing switch. One click, no confirm and no restart -- the file IS the
        request, and the viewer re-reads it on its next turn."""
        action = form.get("action", "")
        if action not in ("on", "off"):
            self._send(400, self._msg("bad request", "action must be on or off."))
            return
        ok, msg = share_set(action == "on")
        set_flash(("sharing: " if ok else "FAILED: ") + msg)
        print("confscreen: share %s -> %s%s" % (action, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/#viewer")

    def _mode(self, form):
        """The live encoding switch, same shape as sharing: one click, no restart."""
        value = form.get("value", "")
        if value not in ("raw", "hwjpeg"):
            self._send(400, self._msg("bad request",
                                      "The encoding is either raw or hwjpeg."))
            return
        ok, msg = mode_set(value)
        set_flash(("encoding: " if ok else "FAILED: ") + msg)
        print("confscreen: mode %s -> %s%s" % (value, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/#viewer")

    def _context(self):
        """Everything the page and its fragment both need, gathered once. The frame RATE is
        computed from the interval between calls, so the poll IS the frame sampler -- which
        is why this is gathered per request rather than cached."""
        return (conf_values(conf_keys()), rbp_pid(), frames_since_last(), unit_facts(),
                {u: unit_state(u) for u in UNITS},
                {n: switch_state(n) for n in ("live", "mode", "input")},
                player_depth(), self.headers.get("Host", ""))

    def _auth(self, conf, consume_flash=False):
        """The write state for one render: whether the writer is installed, and the one message
        the last write left. `consume_flash` is what makes that message show once -- the page
        takes it, the poll only reads it, so a refresh cannot eat a message nobody has seen."""
        return {"writes": writes_available(),
                "flash": take_flash() if consume_flash else peek_flash()}

    def _input(self, form):
        """The pointer-injection switch. One click, no confirm -- the viewer re-reads the file
        on its next turn, and turning it off can only ever make the page safer."""
        value = form.get("value", "")
        if value not in ("on", "off"):
            self._send(400, self._msg("bad request", "Pointer injection is on or off."))
            return
        ok, msg = input_set(value == "on")
        set_flash(("pointer: " if ok else "FAILED: ") + msg)
        print("confscreen: input %s -> %s%s" % (value, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/#viewer")

    def _preview(self, form):
        """Whether THIS PAGE shows the preview image. It is the page's own preference, so it is
        the one control here that changes nothing about the unit."""
        action = form.get("action", "")
        if action not in ("on", "off"):
            self._send(400, self._msg("bad request", "The preview is either on or off."))
            return
        ok, msg = preview_set(action == "on")
        set_flash(("preview: " if ok else "FAILED: ") + msg)
        print("confscreen: preview %s -> %s%s" % (action, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/#viewer")

    def _switch(self, form):
        """One of the live switches that is not an on/off pair: the capture ceiling, and the
        encoder's node and quality.

        ONE HANDLER, NOT THREE, because the only thing that differs between them is the
        validator -- and the validator lives in LIVE_SWITCHES, which is also what the table is
        built from. A value this switch does not take is a 400 and the file is not touched."""
        name = form.get("name", "")
        check = LIVE_SWITCHES.get(name)
        if check is None:
            self._send(400, self._msg("bad request", "No switch by that name."))
            return
        value = check(form.get("value", ""))
        if value is None:
            self._send(400, self._msg("bad request",
                        "That is not a value this switch takes."))
            return
        ok, why = set_switch(name, value)
        set_flash(("switch: " if ok else "FAILED: ") +
                  (why if why else "%s is now %s" % (name, value)))
        print("confscreen: switch %s=%s -> %s" % (name, value, "ok" if ok else "FAILED"),
              flush=True)
        self._redirect("/#viewer")

    def _restart(self, form):
        """TWO STEPS, and the confirm is a real page rather than a script dialog: this is the
        one control on this page that acts on something the operator can SEE, and it blanks
        the screen for a quarter of a minute. Pressing it by accident should cost one more
        click, and the confirm has to work with the script blocked, like everything else
        here. The player's row in the services table posts here, which is why this is a button
        in a row rather than a control of its own."""
        if form.get("step", "ask") != "go":
            self._send(200, self._msg(
                "restart the player?",
                "This stops rbp and starts it again. The screen goes dark for about fifteen "
                "seconds, the boot screen draws its stages while it comes back, and a set in "
                "progress stops."
                "<p><form method=post action=/restart>"
                "<button name=step value=go>Yes, restart it</button></form> "
                '<a href="/#services">no, leave it alone</a></p>'))
            return
        ok, msg = restart_player()
        set_flash(("restart FAILED: " if not ok else "restart: ") + msg)
        print("confscreen: restart -> %s%s" % ("" if ok else "FAILED: ", msg), flush=True)
        # Land on the services tab, where the button was, so the rows that show the player
        # coming back are the rows the operator is looking at.
        self._redirect("/#services")

    def _service(self, form):
        """Start or restart one unit from the services table. One click, no confirm -- but
        the player never reaches here (its row posts to /restart), so the one control that
        blanks the screen keeps its second click.

        AN UNKNOWN UNIT IS 400 AND NOT A FLASH. The page only ever renders names out of
        UNITS, so a name that is not one of them did not come from this page -- and answering
        it with a redirect and a polite message would make a tampered request look like a
        working one. The membership test is repeated here (both read the same tuple, so they
        cannot disagree) because this is the reply, while service_action's own check is what
        actually stands between a form field and argv."""
        unit = form.get("unit", "")
        if unit not in UNITS:
            self._send(400, self._msg("bad request",
                        "No service by that name is on this page."))
            return
        ok, msg = service_action(unit)
        set_flash(("service: " if ok else "FAILED: ") + msg)
        # NOT the unit name in the log's first field: it is the operator's own string and it
        # has already been rejected if it is not one of ours, but a log line is read by
        # people and `service_action` has the only copy of the verdict that matters.
        print("confscreen: service -> %s%s" % ("" if ok else "FAILED: ", msg), flush=True)
        self._redirect("/#services")

    def _boot(self, form):
        """Enable or disable one unit's start at boot. It does not start or stop anything --
        /service does that, and keeping the two apart is the point of having two buttons."""
        action = form.get("action", "")
        if action not in ("on", "off"):
            self._send(400, self._msg("bad request", "That is either enable or disable."))
            return
        unit = form.get("unit", "")
        if unit not in UNITS:
            self._send(400, self._msg("bad request",
                        "No service by that name is on this page."))
            return
        if unit in NEVER_DISABLE and action == "off":
            # Their `disabled` button is rendered disabled, so a request to press it did not
            # come from this page -- and the reason is the reply rather than a bare refusal.
            self._send(400, self._msg("not from here", esc(NEVER_DISABLE[unit])))
            return
        ok, msg = boot_set(unit, action == "on")
        set_flash(("boot: " if ok else "FAILED: ") + msg)
        print("confscreen: boot %s -> %s%s" % (action, "" if ok else "FAILED: ", msg),
              flush=True)
        self._redirect("/#services")

    def _set(self, form):
        """Save ONE setting, through confedit -- which validates it by type, locks the file,
        keeps a one-generation backup, and copies every other byte of the operator's file
        through untouched.

        A SAVE NEVER RESTARTS ANYTHING. A save that blanked the screen would be a trap, so
        what it does instead is record which unit owes a restart; the settings section says so
        until that unit starts again."""
        key = form.get("key", "")
        value = form.get("value", "")
        e = confedit.BY_KEY.get(key) if confedit else None
        tab = "/#" + ((confedit.topic_of(key) + "settings") if e else "rbpsettings")

        if confedit is None:
            ok, text = False, "the config editor is not installed beside this page"
        elif e is None:
            ok, text = False, "%s is not a setting this page knows about" % key
        elif e.get("secret") and value == "":
            # A blank password field means "leave it alone", not "set it to empty" -- the page
            # never had the value to pre-fill, so it cannot tell the two apart any other way.
            ok, text = True, "%s left blank, so nothing was changed" % key
        else:
            done, why = confedit.write_local(LOCAL_CONF, key, value)
            if done:
                mark_pending(e.get("restart", ""))
                note = apply_note(e.get("restart", ""))
                ok = True
                text = ("saved %s=%s" % (key, "********" if e.get("secret") else value)
                        + (". " + note if note else ""))
            else:
                ok, text = False, "%s not saved: %s" % (key, why)

        print("confscreen: set %s -> %s%s" % (key, "" if ok else "FAILED: ", text), flush=True)
        set_flash(("" if ok else "FAILED: ") + text)
        self._redirect(tab)

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
