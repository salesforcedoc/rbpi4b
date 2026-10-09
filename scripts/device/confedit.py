#!/usr/bin/env python3
"""confedit.py -- reading and writing rb.local.conf, and the schema that guards it.

WHAT THIS FILE IS FOR, AND WHY IT IS SEPARATE FROM THE PAGE. `rb.local.conf` is not a data
file. `rb.conf` sources it at its very end (`. $RB_DEPLOY_ROOT/rb.local.conf`), and every
device script sources rb.conf through `rb_load_conf` -- so on the unit, that file is
executed by `/bin/sh` AS ROOT. A value written into it is therefore CODE, and the difference
between a settings page and a remote-root-execution bug is entirely the validation in this
module. That is why the page and the editor are two files: this one is pure -- text in, text
out, with one small write -- so every rule can be tested against a fixture rather than
argued about, and `test_confedit.py` does exactly that.

THE SHAPE OF THE FILE IS THE OPERATOR'S, NOT OURS. install.sh creates rb.local.conf once and
never rewrites it, so everything in it beyond the stub was typed by a person: their comments,
their ordering, their inline notes. A settings page that regenerated the file would delete
their work on its first write. So this edits IN PLACE -- replacing one value, appending one
line under a marker if the key is absent, and copying every other byte through untouched.

THE RULES, in the order they apply:

  1. The key must be in the SCHEMA. There is no "set any RB_* value" path, because the
     interesting ones are pairs (RB_FB_LIE_BPP is half of a pair with the player's own
     build) or need a shim rebuild, and a form that offers them is a form that can produce a
     unit which does not start.
  2. Read-only keys are refused WITH A REASON, so the page can show the reason rather than
     silently omit a field.
  3. The value is validated by type, and every string value must first survive a charset
     blacklist: `;`, backticks, `$`, `|`, `&`, brackets, braces, quotes, backslash and any
     whitespace are refused outright. That is belt to rule 4's braces.
  4. Before the atomic rename, the new file is run through `sh -n`. A file that does not
     parse is discarded -- the guard that catches whatever rule 3's author did not think of.
  5. One generation of history is kept (`rb.local.conf.prev`), taken before the first write
     of a process and never overwritten within it. If it cannot be taken, the write is
     REFUSED -- the same rule install_artifact() states for artifacts: an install that cannot
     be taken back is worse than an install that did not happen.
  6. All of it happens under `flock` on a separate lock file, so two browser tabs cannot
     interleave a read-modify-write. The lock file is separate on purpose: the conf itself is
     replaced with `os.replace`, which swaps the inode, and a lock held on the old inode
     would not exclude a second writer.

A NOTE ON DUPLICATES. A file may assign the same key twice; shell sourcing means the LAST one
wins, so this replaces the last and leaves earlier ones alone. Replacing the first would
write a line that has no effect, which is the worst possible outcome for a settings page.
"""

import os
import re
import subprocess

# --- the schema ---------------------------------------------------------------------
#
# `restart` says what has to happen for the value to take effect, and it is the honest
# answer rather than a uniform one:
#   player  -- the launcher reads the conf once at start, so rblive4 must restart
#   viewer  -- vnc-run.sh reads it at start, so rblive4-vnc must restart
#   service -- an install-time decision (the unit's enablement), so systemctl enable/disable
#   live    -- a /run/rblive4 control file; nothing restarts
#   none    -- the page reads it live (the password the page itself checks)

B = "bool"
I = "int"
E = "enum"
S = "str"

SCHEMA = [
    # --- basic: the ones actually changed on this rig ---
    dict(key="RB_PREWARM", group="basic", type=B, restart="player", risk="low",
         label="Pre-warm player files",
         help="Read rbp's startup files into the page cache before launching it. With this "
              "off, the first start after a power-up can come up black until it is restarted."),
    dict(key="RB_POINT_KIND", group="basic", type=E, choices=["auto", "abs", "rel", "none"],
         restart="player", risk="low", label="Pointer type",
         help="How the pointer is found. 'auto' prefers an absolute touch panel and falls "
              "back to a relative mouse; 'rel' pins a mouse and will filter a touch panel out."),
    dict(key="RB_MIDI_MAP", group="basic", type=E, choices=["", "flx4", "jp21", "kbd", "none"],
         restart="player", risk="low", label="MIDI controller",
         help="Which surface the controls shim selects. Empty means the built-in default."),
    dict(key="RB_AUDIO_DEV", group="basic", type=S, pattern=r"^[A-Za-z0-9._:+,=-]{0,64}$",
         restart="player", risk="low", label="Audio device",
         help="ALSA device for the master output, e.g. hw:CARD=DDJFLX4,DEV=0. Empty derives "
              "it from the controller."),
    dict(key="RB_VERBOSE", group="basic", type=B, restart="player", risk="low",
         label="Verbose shim logging",
         help="Umbrella flag for the per-module verbose flags. Writes megabytes to /tmp, "
              "which is a 1.9 GB tmpfs."),
    dict(key="RB_VNC", group="basic", type=B, restart="service:rblive4-vnc", risk="low",
         label="Enable the VNC viewer",
         help="Installs and enables the viewer service, so the screen can be watched from "
              "the desk. This is what the page can turn on when nothing else is running."),
    dict(key="RB_BOOTSCREEN", group="basic", type=B, restart="service:rblive4-boot", risk="low",
         label="Boot progress screen",
         help="Draws the launcher's stages on the framebuffer from first light until rbp "
              "paints its first frame."),
    dict(key="RB_VNC_PORT", group="basic", type=I, lo=1024, hi=65535, restart="viewer",
         risk="low", label="VNC (RFB) port", help="The port a VNC client connects to."),
    dict(key="RB_VNC_FPS", group="basic", type=I, lo=1, hi=30, restart="viewer", risk="low",
         label="VNC frame rate",
         help="Capture ceiling while a client or the page preview is watching."),
    dict(key="RB_VNC_ZLIB_LEVEL", group="basic", type=I, lo=0, hi=9, restart="viewer",
         risk="low", label="Compression level",
         help="Deflate level for zlib. 0 sends every frame raw and costs the most bandwidth."),
    dict(key="RB_VNC_MODE", group="basic", type=E, choices=["raw", "hwjpeg"], restart="viewer",
         risk="low", label="Startup encoding",
         help="raw or hwjpeg. The live choice is the switch file, which the page sets."),
    dict(key="RB_VNC_PASSWORD", group="basic", type=S, secret=True,
         pattern=r"^[A-Za-z0-9._@%+=:-]{1,63}$", restart="viewer", risk="medium",
         label="VNC and page password",
         help="Also the password for this page's writes. macOS Screen Sharing requires one. "
              "No spaces or quotes, and it is visible in the process list."),

    # --- advanced: the measured-per-unit values ---
    dict(key="RB_POINT_SWAP_XY", group="advanced", type=B, restart="player", risk="low",
         label="Swap pointer axes", help="Measured per unit, by the four-corner procedure."),
    dict(key="RB_POINT_INVERT_X", group="advanced", type=B, restart="player", risk="low",
         label="Invert pointer X", help="Measured per unit, by the four-corner procedure."),
    dict(key="RB_POINT_INVERT_Y", group="advanced", type=B, restart="player", risk="low",
         label="Invert pointer Y", help="Measured per unit, by the four-corner procedure."),
    dict(key="RB_POINT_CURSOR", group="advanced", type=B, restart="player", risk="low",
         label="Draw a pointer arrow", help="For a relative device only."),
    dict(key="RB_POINT_MENU", group="advanced", type=B, restart="player", risk="low",
         label="Swipe-down top menu",
         help="Costs rbp the top-middle strip of its own screen while it is on."),
    dict(key="RB_POINT_FX_TOUCH", group="advanced", type=B, restart="player", risk="low",
         label="Beat FX touch zones", help="CH SELECT and effect-name touch areas."),
    dict(key="RB_POINT_HOTCUE_TOUCH", group="advanced", type=B, restart="player", risk="low",
         label="Hot cue pad touch", help="Triggers registered cues on a touch."),
    dict(key="RB_POINT_QUANTIZE_TAP", group="advanced", type=B, restart="player", risk="low",
         label="QUANTIZE tap on the deck",
         help="rbp binds no touch to that widget itself, so the shim sends the keycode."),
    dict(key="RB_POINT_MIN_DWELL_MS", group="advanced", type=I, lo=0, hi=2000,
         restart="player", risk="low", label="Minimum tap dwell (ms)",
         help="How long a short tap is held open before the release is sent."),
    dict(key="RB_LED_VU", group="advanced", type=B, restart="player", risk="low",
         label="VU meter bridge", help="Drive the controller's level meter from rbp."),
    dict(key="RB_LED_PADS", group="advanced", type=B, restart="player", risk="low",
         label="Pad illumination", help="Light the controller's pads."),
    dict(key="RB_LED_DISABLE", group="advanced", type=B, restart="player", risk="low",
         label="All LEDs off", help="Turns the whole panel dark; for diagnosis."),
    dict(key="RB_CTRL_KEEPALIVE", group="advanced", type=B, restart="player", risk="low",
         label="Controller keepalive", help="Send the FLX4's vendor keepalive."),

    # --- read-only: shown with a reason, refused here ---
    dict(key="RB_FB_LIE_BPP", group="readonly", type=S, readonly=True,
         label="Display depth the player is told",
         why="Half of a pair with the patched player's own layer format word. A value that "
             "disagrees makes rbp SIGSEGV to a black screen with a systemd restart loop, and "
             "nothing in its log names the cause."),
    dict(key="RB_DFB_PRESENT", group="readonly", type=E, readonly=True,
         label="Display present mode",
         why="Part of the display present path; on a mismatched panel it is a sheared or "
             "entirely black picture, deterministically."),
    dict(key="RB_AUDIO_MIRROR_DEV", group="readonly", type=S, readonly=True,
         label="HDMI audio mirror",
         why="rb.conf states that changing the audio-mirror values needs the shim REBUILT, "
             "not a restart. A field here would look like it worked and do nothing."),
    dict(key="RB_NETALIAS", group="readonly", type=B, readonly=True,
         label="Network alias (Pro DJ Link)",
         why="Retargets rbp's idea of the interface carrying your own ssh session. A drill "
             "instrument, not an operational setting."),
    dict(key="RB_AUTOSTART", group="readonly", type=B, readonly=True,
         label="Start the player at boot",
         why="Disabling this from a browser could leave the appliance not starting the "
             "player at all."),
    dict(key="RB_VNC_HTTP_PORT", group="readonly", type=I, readonly=True,
         label="Viewer page port",
         why="The viewer's own page port. Shown so it can be seen; moving a page's port from "
             "the page is how a page gets lost."),
    dict(key="RB_CONF_HTTP_PORT", group="readonly", type=I, readonly=True,
         label="Status page port",
         why="This page's own port. Same reason."),
]

BY_KEY = {e["key"]: e for e in SCHEMA}

# Every string value must survive this before the per-entry pattern is even tried. It is not
# a substitute for the pattern; it is the part of validation that belongs to the FILE rather
# than to the setting, because the file is shell.
FORBIDDEN = set(";<>`$|&(){}!'\"\\\n\r\t")

MARKER = "# --- written by the config page (these lines are managed; edit above) ---"

_ASSIGN = re.compile(r"^(?P<pre>\s*)(?P<key>[A-Za-z_][A-Za-z0-9_]*)"
                     r"(?P<eq>\s*=\s*)(?P<val>'[^']*'|\"[^\"]*\"|[^\s#]*)"
                     r"(?P<post>\s*(?:#.*)?)$")


def entry(key):
    return BY_KEY.get(key)


def validate(key, raw):
    """(ok, normalised, why). `why` is a sentence for the operator, not a stack trace."""
    e = BY_KEY.get(key)
    if e is None:
        return False, None, "not a setting this page knows about"
    if e.get("readonly"):
        return False, None, e.get("why", "read-only")
    if raw is None:
        return False, None, "no value given"
    raw = raw.strip()
    t = e["type"]
    if t == "bool":
        if raw in ("0", "1"):
            return True, raw, ""
        return False, None, "must be 0 or 1"
    if t == "int":
        if not re.fullmatch(r"-?[0-9]+", raw):
            return False, None, "must be a whole number"
        n = int(raw)
        if n < e.get("lo", -(1 << 62)) or n > e.get("hi", (1 << 62)):
            return False, None, "must be between %d and %d" % (e.get("lo"), e.get("hi"))
        return True, str(n), ""
    if t == "enum":
        if raw in e["choices"]:
            return True, raw, ""
        shown = ", ".join("(empty)" if c == "" else c for c in e["choices"])
        return False, None, "must be one of: " + shown
    # str: the file's own rules first, then the setting's.
    bad = sorted(set(raw) & FORBIDDEN)
    if bad:
        return False, None, ("contains characters that are not allowed in this file: "
                             + " ".join(repr(c) for c in bad))
    if re.search(r"\s", raw):
        return False, None, "must not contain whitespace"
    if not re.fullmatch(e.get("pattern", r".*"), raw):
        return False, None, e.get("hint", "does not match the expected shape")
    return True, raw, ""


def read_value(text, key):
    """The value in force: the LAST uncommented assignment, because that is the one shell
    sourcing will take. None if the key is not assigned at all."""
    found = None
    for line in text.splitlines():
        m = _ASSIGN.match(line)
        if m and m.group("key") == key:
            v = m.group("val")
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                v = v[1:-1]
            found = v
    return found


def set_value(text, key, value):
    """The text with `key` set to `value`, everything else preserved byte for byte.

    Replaces the LAST assignment (the effective one) and keeps any trailing inline comment.
    Appends under the marker when the key is absent, once -- a second write of the same key
    finds the line it added last time rather than adding another."""
    lines = text.split("\n")
    last = None
    for i, line in enumerate(lines):
        m = _ASSIGN.match(line)
        if m and m.group("key") == key:
            last = (i, m)
    if last is not None:
        i, m = last
        lines[i] = "%s%s%s%s%s" % (m.group("pre"), key, m.group("eq"), value, m.group("post"))
        return "\n".join(lines)
    if lines and lines[-1] != "":
        lines.append("")
    if MARKER not in lines:
        lines.append(MARKER)
    lines.append("%s=%s" % (key, value))
    out = "\n".join(lines)
    if not out.endswith("\n"):
        out += "\n"
    return out


def sh_syntax_ok(path, sh="/bin/sh"):
    """`sh -n` on a candidate file. The guard that catches what FORBIDDEN did not."""
    try:
        r = subprocess.run([sh, "-n", path], capture_output=True, text=True, timeout=5)
        return r.returncode == 0, (r.stderr or "").strip()
    except (OSError, subprocess.SubprocessError) as e:
        return False, str(e)


def write_local(path, key, value, sh="/bin/sh", lock=True):
    """Set one value in rb.local.conf, or refuse. Returns (ok, why).

    The order matters and is the whole safety argument: validate, lock, re-read, edit, write
    a temp beside the target, `sh -n` it, take the backup if it is not taken yet, fsync, and
    only then rename over the original."""
    e = BY_KEY.get(key)
    if e is None:
        return False, "not a setting this page knows about"
    ok, value, why = validate(key, value)
    if not ok:
        return False, why

    lockfd = None
    tmp = None
    try:
        if lock:
            import fcntl
            lockfd = os.open(path + ".lock", os.O_RDWR | os.O_CREAT, 0o644)
            fcntl.flock(lockfd, fcntl.LOCK_EX)
        # Re-read AFTER the lock: a hand edit to another line between our read and now must
        # survive, and only the target key may change.
        try:
            with open(path, "r", errors="replace") as f:
                text = f.read()
        except FileNotFoundError:
            text = ""
        new = set_value(text, key, value)
        tmp = "%s.new.%d" % (path, os.getpid())
        with open(tmp, "w") as f:
            f.write(new)
            f.flush()
            os.fsync(f.fileno())
        good, err = sh_syntax_ok(tmp, sh)
        if not good:
            return False, "the result would not parse as shell, so nothing was written: " + err
        prev = path + ".prev"
        if not os.path.exists(prev) and os.path.exists(path):
            try:
                with open(path, "rb") as src, open(prev, "wb") as dst:
                    dst.write(src.read())
            except OSError as ex:
                return False, ("could not keep a backup (%s), so nothing was written -- a "
                               "change that cannot be taken back is worse than no change" % ex)
        os.replace(tmp, path)
        tmp = None
        try:
            os.chmod(path, 0o644)
        except OSError:
            pass
        return True, ""
    except OSError as ex:
        return False, "could not write: %s" % ex
    finally:
        if tmp and os.path.exists(tmp):
            try:
                os.unlink(tmp)
            except OSError:
                pass
        if lockfd is not None:
            os.close(lockfd)          # closing the fd releases the flock
