#!/usr/bin/env python3
"""
rbrowser.py -- the host half of the deck's browser window.

The shim cannot host a browser: it is an armel process inside the vendor chroot,
where installing one would mean writing into rbp's own rootfs. So chromium runs
HERE, on the host, and the shim draws its frames. The two hand each other files
through /tmp, which was measured on the unit to be the SAME MOUNT on both sides
(dev 35, ino 1 -- the shim's own knobshim.log is one file seen from both sides), so
there is no new mount, no new privilege and no SD-card wear. browser_link.h states
the protocol from the shim's end; this file is the other end of it.

    /tmp/rbwin.frame    this -> shim    the page, RGB565LE, under a header
    /tmp/rbwin.status   this -> shim    one `key value` per line
    /tmp/rbwin.cmd      shim -> this    one command per line

WHY THE PIPE AND NOT A PORT. `--remote-debugging-pipe` puts CDP on fd 3 (in) and
fd 4 (out) as NUL-separated JSON, which needs no websocket client and no listening
socket on a device that is also a music player. The cost is that chromium must
inherit exactly those two descriptor numbers, which is what the fd choreography in
start_chromium() is for and why it is commented rather than tidy.

WHY THE DECODE IS OUR OWN jpeg565 AND NOT ffmpeg. A screencast frame arrives as a
base64 JPEG and the shim wants RGB565LE, so something has to decode it. The plan
asked for one long-lived ffmpeg fed on `-f image2pipe`, and the unit says no twice
over, both measured 2026-10-04:

  * one long-lived ffmpeg cannot be fed on a pipe at all. With the input on a
    non-seekable pipe it never leaves avformat_find_stream_info() -- its own debug
    log ends at `bytes read:33266 seeks:0 nb_streams:1` with the whole JPEG in hand,
    and the process sits in anon_pipe_read until the input reaches EOF. Piping two
    or three images does not help, and neither does -fpsprobesize 0,
    -analyzeduration 0, -probesize, -f mjpeg, -fflags +nobuffer or -flags +low_delay.
  * and one ffmpeg per frame is unaffordable anyway: `ffmpeg -version` ALONE is
    337 ms on this Pi, all of it library loading -- the decode, the scale and the
    RGB565 conversion together add nothing measurable to that. 2.4 fps.

`jpeg565.c`, next to this file, is the decoder: libjpeg, which is handed one image
and knows its size, so there is no container to probe and nothing to seek. Measured
45 ms per frame end to end, and it decides the pixel format in the one place that
should. The Makefile beside it builds it; this driver builds it on first use if it
is missing. Its header comment carries the same two measurements.

WHAT THIS DOES NOT DO. It does not resize the page when the keyboard comes up --
the shim shows fewer rows of the same page, exactly as a phone does, so a click
coordinate is the page's own and needs no translation. And it writes nothing of the
operator's: its whole footprint is the three files above, a chromium profile
directory in /tmp, and the processes it starts.

Run it on the Pi:

    python3 tools/rbrowser/rbrowser.py --url https://example.com

and the deck's window (MENU_WINDOW=1 in the service drop-in) will show it. Stop it
with ^C: the shim notices within ten seconds and says so in the url field rather
than leaving a frozen page on the glass.

It needs chromium and the `jpeg565` helper built (gcc and libjpeg62-turbo-dev, whose
runtime library chromium already brings); it builds the helper itself the first time
if it is not there.
"""
import argparse
import base64
import errno
import fcntl
import json
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import time

# browser_link.h's three paths and its header layout. Kept in step by hand; the
# magic is spelled the same way there ('R','B','W','F' as a little-endian word).
FRAME = "/tmp/rbwin.frame"
STATUS = "/tmp/rbwin.status"
CMD = "/tmp/rbwin.cmd"
# chromium's own stderr. Not DEVNULL, for the reason start_chromium() gives.
CHROME_LOG = "/tmp/rbwin.chromium.log"
HDR = struct.Struct("<IIIII")           # magic, seq, w, h, fmt
MAGIC = 0x46574252

# menu_window.h's geometry: the window's content area is what the page is drawn
# into, and its width is the page's. If those constants move, this is the line that
# moves with them -- and the shim's frame check (w must match, h must be at least
# the rows it asked for) is what makes a mismatch visible rather than a picture
# that is quietly the wrong size.
PAGE_W = 1120
PAGE_H = 540

# How often the status file is rewritten even when nothing changed. The shim treats
# a status older than ten seconds as a dead browser (browser_link.c), so this has to
# be comfortably under that; it is also the poll interval for the focus generation.
STATUS_INTERVAL = 2.0

# The main loop's tick. It is what notices a command within a frame time of the
# operator's tap; the select on the CDP pipe is what actually wakes it.
TICK = 0.2

# THE STALL WATCHDOG, and why it needs an ARM rather than watching for silence.
# Silence is not a symptom: a still page emits nothing for minutes on end and that is
# the design (browser_link.c's seq gate exists for exactly that). What is a symptom is
# a command -- a finger on the glass, which implies a repaint -- with no frame behind
# it. Measured on the unit 2026-10-04: the pipe went silent for 31 straight minutes
# while chromium still answered CDP perfectly (a navigation landed and changed the
# URL in the status file) and its screencast emitted nothing at all, leaving the deck
# on a still picture. Nothing in this driver noticed; that is the defect this fixes.
STALL_S = 8.0

# The heartbeat, so the next freeze is legible from the journal instead of from
# /proc. The renderer count is in it because that is the number that separated the
# frozen session (31 renderers, 3.2 GB) from a healthy one (4-6, under 1 GB).
BEAT_S = 30.0
WHEEL_S = 5.0                   # see the scroll verb: a wheel's ack is not worth a stall

# THE DECK IS NOT A HEADLESS BROWSER, AND THE WEB IS ENTITLED TO BE TOLD SO.
# This is the whole of "the screen is stuck, i can't navigate anywhere" and of
# "google says it looks like a bot", and it was measured on the unit 2026-10-04 with
# two throwaway chromium runs differing only in this string:
#
#   cnn.com        headless -> 165 bytes, chromium's own "Unknown Error" page, and
#                             NOTHING else: a dead white page that ignores every tap,
#                             which is exactly what the operator was looking at when
#                             they reported the deck frozen
#                  this     -> 7.15 MB, "Breaking News, Latest News and Videos | CNN"
#   google search  headless -> a 5 KB stub with no title
#                  this     -> 1.83 MB of real results
#
# The version tracks the installed chromium on purpose: a UA that is newer than the
# engine is a different kind of lie. Chrome/154 is what this unit runs.
DESKTOP_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36")

# CDP key events for the named keys menu_window.c sends. The virtual key codes are
# Windows' -- CDP's own naming. Backspace and Enter are the only two the keyboard
# emits today; a table with one obvious place to add the next is cheaper than a
# branch somebody has to find first.
KEYS = {
    "Backspace": (8, "Backspace"),
    "Enter": (13, "Enter"),
    "Tab": (9, "Tab"),
    "Escape": (27, "Escape"),
    "ArrowLeft": (37, "ArrowLeft"),
    "ArrowRight": (39, "ArrowRight"),
}

# What the page reports about its own text inputs, installed before any page script
# runs. __rbGen is the shim's `focus` GENERATION: 0 when no editable element has
# focus, and a number that grows on each GAIN of focus. A boolean cannot answer the
# question the operator will actually ask -- dismiss the keyboard on a field that is
# still focused, then tap that field again -- because the boolean never changes.
FOCUS_JS = """
(function () {
  if (window.__rbInstalled) return;
  window.__rbInstalled = true;
  window.__rbGen = 0;
  window.__rbEl = null;
  function editable(e) {
    return !!e && (e.tagName === 'INPUT' || e.tagName === 'TEXTAREA' ||
                   e.isContentEditable);
  }
  document.addEventListener('focusin', function (ev) {
    if (editable(ev.target) && ev.target !== window.__rbEl) {
      window.__rbEl = ev.target;
      window.__rbGen = window.__rbGen + 1;
    }
  }, true);
  document.addEventListener('focusout', function (ev) {
    if (ev.target === window.__rbEl) {
      window.__rbEl = null;
      window.__rbGen = 0;
    }
  }, true);
})();
"""


# Set by the signal handler and read by Client.rpc(). A handler that only sets a
# flag leaves the process sitting in a 30-second wait-for-chromium after ^C; this is
# what makes the wait give up at the next tick instead. A list because a handler
# cannot rebind a module global.
_STOPPING = [False]


def log(msg):
    """Diagnostics to stderr only -- stdout is not ours to scribble on."""
    sys.stderr.write("rbrowser: %s\n" % msg)
    sys.stderr.flush()


def normalise_url(u):
    """A URL bar has to accept what people type. `example.com` is not a URL to
    chromium's parser, and a navigation that silently did nothing would read as a
    broken browser rather than a missing scheme."""
    u = u.strip()
    if not u:
        return ""
    for scheme in ("http://", "https://", "about:", "file://", "data:"):
        if u.startswith(scheme):
            return u
    if "://" in u:
        return u
    return "https://" + u


def renderer_count():
    """chromium's renderer processes, straight out of /proc.

    Not decoration: it is the one cheap number that named the freeze. The wedged
    session had 31 renderers and 3.2 GB resident on a 3.8 GB box, against 4-6 and
    under 1 GB for a fresh one -- the difference between a pipe that emits frames and
    one that does not, and nothing else in the journal showed it."""
    n = 0
    try:
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open("/proc/%s/cmdline" % pid, "rb") as f:
                    if b"--type=renderer" in f.read():
                        n += 1
            except OSError:
                continue                    # a process that exited mid-walk
    except OSError:
        return -1
    return n


class Pipe:
    """CDP over the two descriptors: NUL-separated JSON, one message each way, and
    no request/response pairing beyond the id chromium echoes back."""

    def __init__(self, rfd, wfd):
        self.rfd = rfd
        self.wfd = wfd
        self.buf = b""
        self.next_id = 1

    def send(self, method, params=None, session=None):
        msg = {"id": self.next_id, "method": method}
        self.next_id += 1
        if params is not None:
            msg["params"] = params
        if session:
            msg["sessionId"] = session
        try:
            os.write(self.wfd, json.dumps(msg).encode() + b"\0")
        except OSError as e:
            # EPIPE is chromium having gone. The caller's loop is what decides what
            # to do about it, and it decides by noticing the child is gone.
            log("write failed (%s)" % e)
            return None
        return msg["id"]

    def drain(self):
        """Every whole message available now. A partial one stays in the buffer --
        the pipe is a byte stream and a read can land mid-JSON."""
        out = []
        try:
            chunk = os.read(self.rfd, 1 << 20)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                return out
            raise
        if not chunk:
            return out
        self.buf += chunk
        while True:
            i = self.buf.find(b"\0")
            if i < 0:
                break
            raw, self.buf = self.buf[:i], self.buf[i + 1:]
            if not raw.strip():
                continue
            try:
                out.append(json.loads(raw.decode("utf-8", "replace")))
            except ValueError:
                log("undecodable message dropped (%d bytes)" % len(raw))
        return out

    def close(self):
        for fd in (self.rfd, self.wfd):
            try:
                os.close(fd)
            except OSError:
                pass


def decoder_binary():
    """jpeg565, next to this script -- built here if it is not there yet.

    Building on first use rather than asking the operator to run make: it is one
    `cc` line, the unit has gcc, and the alternative is a message telling them to do
    it. Loudly, though -- a compile error that this swallowed would look like a
    browser that will not open."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "jpeg565")
    if os.access(path, os.X_OK):
        return path
    log("building %s (one cc line; wants gcc and libjpeg62-turbo-dev)" % path)
    try:
        r = subprocess.run(["make", "-C", here, "jpeg565"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    except OSError as e:
        log("could not run make: %s" % e)
        return None
    if r.returncode != 0 or not os.access(path, os.X_OK):
        log(r.stdout.decode("utf-8", "replace").strip())
        return None
    return path


class Decoder:
    """The screencast's JPEGs into the shim's RGB565LE, one jpeg565 per frame -- the
    module docstring says why it is not ffmpeg and not one long-lived process.

    The JPEG goes to a tmpfs path and is REPLACED rather than overwritten in place:
    jpeg565 opens that name itself, and a half-written JPEG is a decode error rather
    than a torn picture. The path carries this process's pid so two drivers cannot
    eat each other's frames.

    A PROCESS PER FRAME IS CHEAP AT 45 ms, and it is bounded twice over anyway.
    `frame()` has a deadline and kills the child, so a decoder that wedges is an
    exception the loop survives rather than a hang -- which is also what makes ^C
    land in the main loop instead of inside a read that never returns. And the caller
    decodes only the NEWEST frame of a backlog, so a page that repaints faster than
    this can decode does not put the driver permanently behind."""

    def __init__(self, w, h, binary, timeout=5.0):
        self.w = w
        self.h = h
        self.n = w * h * 2
        self.binary = binary
        self.timeout = timeout
        self.path = "/tmp/rbwin.jpg.%d" % os.getpid()
        self.tmp = self.path + ".new"

    @staticmethod
    def _why(p):
        """jpeg565's one line of reason. Only read once its stdout has ended, which
        is the only way the caller gets here -- so the child is already done writing
        and this cannot block."""
        try:
            p.stdout.close()
            return p.stderr.read(200).decode("utf-8", "replace").strip()
        except OSError:
            return ""

    def frame(self, jpeg):
        with open(self.tmp, "wb") as f:
            f.write(jpeg)
        os.replace(self.tmp, self.path)
        p = subprocess.Popen(
            [self.binary, str(self.w), str(self.h), self.path],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        try:
            buf = b""
            end = time.time() + self.timeout
            while len(buf) < self.n:
                left = end - time.time()
                if left <= 0:
                    raise RuntimeError("jpeg565 did not decode %d bytes in %gs"
                                       % (self.n, self.timeout))
                r, _, _ = select.select([p.stdout], [], [], left)
                if not r:
                    continue
                chunk = p.stdout.read(self.n - len(buf))
                if not chunk:
                    raise RuntimeError("jpeg565 stopped after %d of %d bytes: %s"
                                       % (len(buf), self.n,
                                          self._why(p) or "no reason given"))
                buf += chunk
            return buf
        finally:
            p.stdout.close()
            p.stderr.close()
            try:
                p.wait(timeout=2)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

    def close(self):
        for path in (self.path, self.tmp):
            try:
                os.unlink(path)
            except OSError:
                pass


class FrameWriter:
    """The frame file, rewritten IN PLACE at a fixed size.

    In place and fixed-size, not written-and-renamed, because the shim MMAPS it: a
    rename would leave the shim reading an unlinked file, and a truncate would
    invalidate the mapping under it. The shim's torn-frame guard is a sequence word
    it reads before and after the copy, so the pixels go down FIRST and the header
    (with seq bumped) goes down LAST."""

    def __init__(self, w, h, bpp=16):
        self.w = w
        self.h = h
        self.bpp = bpp
        self.pixlen = w * h * (bpp // 8)
        self.seq = 0
        self.fd = os.open(FRAME, os.O_RDWR | os.O_CREAT, 0o644)
        os.ftruncate(self.fd, HDR.size + self.pixlen)
        # A header with seq 0 and a real geometry, so a shim that opens the file
        # before the first frame finds something coherent rather than a hole.
        self._header()

    def _header(self):
        os.pwrite(self.fd, HDR.pack(MAGIC, self.seq, self.w, self.h, self.bpp), 0)

    def write(self, pix):
        os.pwrite(self.fd, pix, HDR.size)
        self.seq += 1
        self._header()

    def close(self):
        os.close(self.fd)


def write_status(url, focus):
    """The status file, replaced atomically: the shim reads it with no lock, and a
    torn read would be a URL missing its tail -- which the shim would show in the
    address bar and then navigate to.

    `mono` is what tells the shim a browser is still there, and it is
    CLOCK_MONOTONIC rather than a wall clock or the file's mtime because it has to be
    comparable with the shim's own clock across two processes -- same kernel, same
    clock. (The shim cannot stat the file to find out for itself: the vendor's libc
    has no `stat`, which browser_link.c learned the hard way.)"""
    tmp = STATUS + ".tmp"
    with open(tmp, "w") as f:
        f.write("url %s\n" % url)
        f.write("focus %d\n" % focus)
        f.write("mono %d\n" % int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000))
    os.replace(tmp, STATUS)


def _park(fd):
    """Move a pipe end above the two numbers chromium insists on.

    os.pipe() hands back the LOWEST free descriptors, which on a process holding
    only 0, 1 and 2 are 3 and 4 -- exactly the numbers the next two os.dup2() calls
    are about to fill. Parking every end at 10 or above first is what stops one of
    those calls from clobbering a pipe end that is still needed."""
    nfd = fcntl.fcntl(fd, fcntl.F_DUPFD, 10)
    os.close(fd)
    return nfd


def chrome_why():
    """The last thing chromium said, for whatever is about to report its death."""
    try:
        with open(CHROME_LOG, errors="replace") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
    except OSError:
        return ""
    return lines[-1] if lines else ""


def clear_profile_lock(profile):
    """Remove a profile lock left by a chromium that was killed rather than asked to
    stop. Measured on the unit 2026-10-04: after the driver was terminated the lock
    still pointed at a dead pid (`SingletonLock -> rpidev01-3127`), and the next
    start died inside Target.createTarget without a word.

    Safe because ONE DRIVER PER DECK is the assumption this whole design already
    makes -- the frame file is a single path, so a second driver would be fighting
    the first for the glass rather than sharing the profile."""
    for name in ("SingletonLock", "SingletonSocket", "SingletonCookie"):
        try:
            os.unlink(os.path.join(profile, name))
        except OSError:
            pass


def start_chromium(binary, profile, url, w, h, extra):
    """Start chromium with CDP on fd 3 and fd 4, and hand back the two descriptors
    this process keeps.

    CHROMIUM HARDCODES 3 AND 4, so those numbers have to be right IN THE CHILD --
    pass_fds alone would keep the pipes open at whatever numbers they happened to
    get. So the pipes are moved out of the way, 3 and 4 are filled with the two
    ends the child needs, the child is started, and then whatever 3 and 4 were in
    this process is put back: they belong to the caller, not to us.
    """
    clear_profile_lock(profile)
    to_r, to_w = os.pipe()      # this process writes to_w -> chromium reads to_r
    fr_r, fr_w = os.pipe()      # chromium writes fr_w -> this process reads fr_r
    to_r, to_w, fr_r, fr_w = (_park(f) for f in (to_r, to_w, fr_r, fr_w))

    saved = {}
    for fd in (3, 4):
        try:
            saved[fd] = os.dup(fd)
        except OSError:
            saved[fd] = None
    try:
        os.dup2(to_r, 3)
        os.dup2(fr_w, 4)
        cmd = [binary, "--headless=new", "--remote-debugging-pipe",
               "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
               "--no-first-run", "--no-default-browser-check",
               # The UA is a LAUNCH flag rather than a CDP override because setup()
               # navigates to the first page before anything else can be said, and
               # because this is the form the two measurements above were taken in.
               "--user-agent=" + DESKTOP_UA,
               # The other half of a bot check, and cheap: `navigator.webdriver` is
               # what a page reads when the UA alone does not convince it. Whether it
               # was ever true here is unmeasured -- the UA alone unblocked both
               # sites -- so this is insurance, not the cure.
               "--disable-blink-features=AutomationControlled",
               # THE PAGE IS NEVER BACKGROUNDED, because a backgrounded page stops
               # committing frames and a screencast can then only ever repeat itself.
               # The wedge measured 2026-10-04 -- silent screencast, alive CDP -- is
               # what a throttled or occluded page looks like from here, and these
               # three are the standard answer to it. They cost nothing when they are
               # not needed, which is why they are unconditional.
               "--disable-background-timer-throttling",
               "--disable-backgrounding-occluded-windows",
               "--disable-renderer-backgrounding",
               "--user-data-dir=" + profile,
               "--window-size=%d,%d" % (w, h)] + extra + [url or "about:blank"]
        # CHROMIUM'S OWN STDERR GOES TO A FILE, NOT TO /dev/null. When it dies at
        # startup all this driver can say is "chromium exited during <method>", and
        # the reason -- a stale profile lock, a flag it does not know, a missing
        # library -- is in this stream and nowhere else. A window that will not open
        # is exactly the failure that costs an afternoon here.
        errlog = open(CHROME_LOG, "w")
        try:
            proc = subprocess.Popen(cmd, pass_fds=(3, 4),
                                    stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL,
                                    stderr=errlog)
        finally:
            errlog.close()
    finally:
        # 3 and 4 are ours again, and the two ends copied onto them are now the
        # child's -- close this process's copies below.
        for fd in (3, 4):
            os.close(fd)
        for fd, dup in saved.items():
            if dup is not None:
                os.dup2(dup, fd)
                os.close(dup)

    os.close(to_r)
    os.close(fr_w)
    return proc, to_w, fr_r


class Client:
    """The CDP conversation, with the two things a bare Pipe does not have: a way
    to wait for one answer, and a queue for events that arrive while waiting."""

    def __init__(self, proc, pipe):
        self.proc = proc
        self.pipe = pipe
        self.responses = {}
        self.events = []
        self.session = None
        self.url = ""
        self.focus = 0

    def _absorb(self, msgs):
        for m in msgs:
            if "id" in m and "method" not in m:
                self.responses[m["id"]] = m
            else:
                self.events.append(m)

    def pump(self, timeout):
        try:
            r, _, _ = select.select([self.pipe.rfd], [], [], timeout)
        except InterruptedError:
            return
        if r:
            self._absorb(self.pipe.drain())

    def rpc(self, method, params=None, session=None, timeout=30):
        mid = self.pipe.send(method, params, session)
        if mid is None:
            raise RuntimeError("could not ask chromium for %s" % method)
        end = time.time() + timeout
        while mid not in self.responses:
            if self.proc.poll() is not None:
                why = chrome_why()
                raise RuntimeError("chromium exited during %s%s"
                                   % (method, (": " + why) if why else ""))
            if _STOPPING[0]:
                raise RuntimeError("stopping during %s" % method)
            if time.time() > end:
                raise RuntimeError("no answer to %s in %gs" % (method, timeout))
            self.pump(0.2)
        msg = self.responses.pop(mid)
        if "error" in msg:
            raise RuntimeError("%s: %s" % (method, msg["error"].get("message")))
        return msg.get("result", {})

    def evaluate(self, expression):
        r = self.rpc("Runtime.evaluate",
                     {"expression": expression, "returnByValue": True},
                     session=self.session)
        return r.get("result", {}).get("value")


def do_command(cl, line):
    """One line of /tmp/rbwin.cmd. The line IS the protocol (browser_link.h says why
    it is text rather than a struct), so an unknown verb is ignored rather than
    fatal: a shim newer than this driver must not stop the browser dead."""
    parts = line.split(" ", 1)
    verb = parts[0].strip()
    arg = parts[1] if len(parts) > 1 else ""
    try:
        if verb == "nav":
            u = normalise_url(arg)
            if u:
                cl.rpc("Page.navigate", {"url": u}, session=cl.session)
        elif verb == "text":
            if arg:
                cl.rpc("Input.insertText", {"text": arg}, session=cl.session)
        elif verb == "key":
            k = KEYS.get(arg.strip())
            if k:
                vk, name = k
                for t in ("rawKeyDown", "keyUp"):
                    cl.rpc("Input.dispatchKeyEvent",
                           {"type": t, "windowsVirtualKeyCode": vk,
                            "nativeVirtualKeyCode": vk, "key": name, "code": name},
                           session=cl.session)
        elif verb == "click":
            xy = arg.split()
            x, y = int(xy[0]), int(xy[1])
            for t in ("mousePressed", "mouseReleased"):
                cl.rpc("Input.dispatchMouseEvent",
                       {"type": t, "x": x, "y": y, "button": "left",
                        "clickCount": 1}, session=cl.session)
        elif verb == "scroll":
            # A DRAG ON THE GLASS, AS A WHEEL. Input.dispatchMouseEvent is the only
            # call that scrolls a page over CDP -- there is no Input.scroll -- so the
            # finger becomes a wheel with deltaX/deltaY, which a page consumes exactly
            # as it would a real one (including its own smooth scrolling).
            #
            # THE SIGN IS NEGATED HERE, ONCE. The shim sends the finger's own travel
            # (+y is the finger moving DOWN); a wheel's deltaY is the opposite of what
            # the content does, so a finger dragging down -- which walks the page back
            # up -- is a NEGATIVE deltaY. Getting this wrong scrolls every page
            # backwards, which is why it is stated here rather than left implicit.
            xy = arg.split()
            dx, dy = int(xy[0]), int(xy[1])
            cl.rpc("Input.dispatchMouseEvent",
                   {"type": "mouseWheel",
                    # The point matters: the wheel goes to whatever element is under
                    # it, so the middle of the page is the honest place for a gesture
                    # the shim reports as a delta rather than a position.
                    "x": 560, "y": 270,
                    "deltaX": -dx, "deltaY": -dy,
                    "pointerType": "mouse"},
                   # A WHEEL IS BOUNDED, AND THAT IS THE WHOLE POINT. This call is
                   # answered only once chromium has PROCESSED the event, and a page
                   # whose main thread is busy with a video or an ad can leave the
                   # ack outstanding for far longer than 30 s. The driver has ONE
                   # loop: a 30-second wait inside it is 30 seconds of frames not
                   # acked, not decoded and not written, so the glass freezes while
                   # the browser is still perfectly alive. Measured 2026-10-04 -- a
                   # drag whose acks never came wedged the unit exactly this way.
                   # Five seconds then drop: a scroll the operator has already
                   # finished making is worth nothing by the time it is stale.
                   session=cl.session, timeout=WHEEL_S)
        elif verb == "hist":
            # THE PAGE'S OWN HISTORY, walked by the page. Page.navigate is no use
            # here -- it has no notion of "back" -- and the shim cannot know whether
            # there is an entry to go to, which is exactly why this is a request and
            # not something the window decides. history.go(0) is a reload, so a
            # zero is refused rather than passed on.
            n = int(arg)
            if n:
                cl.rpc("Runtime.evaluate",
                       {"expression": "history.go(%d)" % n}, session=cl.session)
        if verb in ("nav", "text", "key", "click", "hist", "scroll"):
            # ARMS THE STALL WATCHDOG. Every one of these is a finger on the glass --
            # or, for nav, the URL bar -- and a finger implies a repaint, so a frame
            # owed and not delivered is the one thing silence cannot explain away.
            # An unknown verb is deliberately not armed: a shim newer than this
            # driver must not be able to make it restart the screencast.
            cl.armed = True
            cl.deadline = time.time() + STALL_S
    except (RuntimeError, ValueError, IndexError) as e:
        # A tap that could not be delivered is worth saying out loud -- the operator
        # sees a keyboard that did nothing -- but it is not worth stopping for.
        log("command '%s' failed: %s" % (line, e))


def run_commands(cl, lines):
    """A BURST OF SCROLLS IS ONE SCROLL.

    The shim sends a line per MW_SCROLL_STEP of finger travel -- 12 logical px -- so
    one drag down a long page is dozens of them, and each is a CDP round trip
    chromium answers only after it has processed the wheel event. Measured on the
    unit 2026-10-04 late: the acks for a real drag came back slower than the finger
    made them, the queue could never drain, and the driver's single loop was left
    waiting on input that had already been overtaken by the finger -- the picture
    froze, the page stopped answering, and the operator's report was *"i don't think
    its running"*. Summing a consecutive run is the honest model as well as the
    cheap one (a gesture IS one movement, and the browser smooth-scrolls it either
    way): one drag becomes a handful of wheel events instead of dozens.

    Only a RUN is summed. Any other command fences it, so a tap in the middle of a
    drag keeps its place in the order.

    A line whose two numbers do not parse is dropped rather than fatal, the same
    rule the verbs follow: a shim newer than this driver must not stop the browser.
    """
    i = 0
    while i < len(lines):
        if lines[i].split(" ", 1)[0].strip() != "scroll":
            do_command(cl, lines[i])
            i += 1
            continue
        dx = dy = 0
        while i < len(lines) and lines[i].split(" ", 1)[0].strip() == "scroll":
            try:
                a, b = lines[i].split()[1:3]
                dx += int(a)
                dy += int(b)
            except (ValueError, IndexError) as e:
                log("command '%s' failed: %s" % (lines[i], e))
            i += 1
        if dx or dy:
            do_command(cl, "scroll %d %d" % (dx, dy))


def setup(cl, args):
    """The handshake, in the one order that works -- measured on the unit
    2026-10-04, because the obvious order does not.

    about:blank FIRST and the real page second, because the focus listener has to
    be installed before any page script runs: a page that focuses an input during
    load would otherwise never report it, and the keyboard would not come up for
    the first field on the page.

    THE SCREENCAST STARTS BEFORE THE NAVIGATION, and that is not a style choice.
    Starting it after one fails with "Not attached to an active page" -- the
    navigation has already swapped the renderer under the session, and the Page
    domain will not take a screencast on the target in that moment. Started first,
    it survives the navigation and frames for the new page arrive on the same
    session; asking again afterwards is an error ("already active"), which is how
    we know the session was never lost."""
    t = cl.rpc("Target.createTarget", {"url": "about:blank"})
    cl.session = cl.rpc("Target.attachToTarget",
                        {"targetId": t["targetId"], "flatten": True})["sessionId"]
    cl.rpc("Page.enable", session=cl.session)
    cl.rpc("Runtime.enable", session=cl.session)
    cl.rpc("Page.addScriptToEvaluateOnNewDocument", {"source": FOCUS_JS},
           session=cl.session)
    # THE VIEWPORT IS STATED, NOT INFERRED FROM THE WINDOW. Measured on the unit
    # 2026-10-04: with `--window-size=1120,540` the screencast reported a viewport of
    # **1120x453** -- headless=new is a real browser window, so `--window-size` counts
    # its chrome and the page gets the window minus ~87 px of toolbar. The frames then
    # arrive 1120x453 and jpeg565, which maps each axis independently, stretches them
    # to the shim's 1120x540: a 1.19x vertical stretch, which on the glass reads as
    # exactly what the operator called "the aspect ratio is wrong". Asking for the
    # frame size in `startScreencast` was never enough -- maxWidth/maxHeight only cap
    # the capture, they do not set the viewport. This does, so the page lays out at
    # the size it is shown at, the captions are 1:1 and jpeg565's resample is a no-op.
    cl.rpc("Emulation.setDeviceMetricsOverride",
           {"width": args.width, "height": args.height,
            "deviceScaleFactor": 1, "mobile": False},
           session=cl.session)
    # The params are kept on the client, not spelled twice: the stall watchdog
    # restarts the screencast with this exact dictionary, and a restart that asked
    # for something different from the start would be a second, quieter bug.
    cl.sc_params = {"format": "jpeg", "quality": args.quality,
                    "maxWidth": args.width, "maxHeight": args.height,
                    "everyNthFrame": args.every}
    cl.rpc("Page.startScreencast", cl.sc_params, session=cl.session)
    url = normalise_url(args.url)
    if url:
        cl.rpc("Page.navigate", {"url": url}, session=cl.session)


def jpeg_size(b):
    """The JPEG's OWN dimensions -- what jpeg565 will resample FROM, and therefore
    the only thing that decides whether the picture is stretched.

    jpeg565 maps each axis independently (`sx = x * in_w / out_w`), so a frame that
    arrives at a different SHAPE than the shim's frame is scaled non-uniformly -- a
    720x540 screencast shown in a 1120x540 window is a 1.56x horizontal stretch, and
    that is what "the aspect ratio is wrong" would look like from the deck. The
    screencast is asked for exactly the frame size, but asking is not measuring, so
    the first frame of a run reports what actually arrived (SOF marker walk, once)."""
    i, n = 2, len(b)
    while i + 9 < n:
        if b[i] != 0xFF:
            i += 1
            continue
        m = b[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            return ((b[i + 7] << 8) | b[i + 8], (b[i + 5] << 8) | b[i + 6])
        i += 2 + ((b[i + 2] << 8) | b[i + 3])
    return None


_FRAME_MEASURED = []


def measure_frame(params, jpeg, dec):
    """One line per run: what the screencast promised, and what it delivered."""
    if _FRAME_MEASURED:
        return
    _FRAME_MEASURED.append(True)
    md = params.get("metadata") or {}
    got = jpeg_size(jpeg)
    log("screencast: viewport %sx%s scale %s -> jpeg %sx%s, asked %dx%d"
        % (md.get("deviceWidth"), md.get("deviceHeight"),
           md.get("pageScaleFactor"),
           got[0] if got else "?", got[1] if got else "?",
           dec.w, dec.h))


def on_event(cl, dec, fw, ev):
    """One CDP event. Returns True when a frame was written, which is what asks for
    a status refresh."""
    method = ev.get("method")
    params = ev.get("params") or {}
    sid = ev.get("sessionId")
    if sid and sid != cl.session:
        return False                    # some other target's; not ours to touch
    if method == "Page.screencastFrame":
        cl.rpc("Page.screencastFrameAck", {"sessionId": params["sessionId"]},
               session=cl.session)
        jpeg = base64.b64decode(params["data"])
        measure_frame(params, jpeg, dec)
        fw.write(dec.frame(jpeg))
        cl.frames += 1
        cl.last_frame = time.time()
        cl.armed = False                # the watchdog's whole input: a frame landed
        if cl.stalls:
            log("screencast resumed after %d stall(s)" % cl.stalls)
            cl.stalls = 0
        return True
    if method == "Page.frameNavigated":
        frame = params.get("frame") or {}
        if not frame.get("parentId"):
            cl.url = frame.get("url", "")
            return True
    if method == "Page.loadEventFired":
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description="the deck's browser window, host half")
    ap.add_argument("--url", default="", help="the page to open first")
    ap.add_argument("--chromium", default="chromium", help="the chromium binary")
    ap.add_argument("--profile", default="/tmp/rbrowser-profile",
                    help="the user-data-dir (a tmpfs path, not the SD card)")
    ap.add_argument("--width", type=int, default=PAGE_W)
    ap.add_argument("--height", type=int, default=PAGE_H)
    ap.add_argument("--quality", type=int, default=70,
                    help="screencast JPEG quality; the quality/CPU dial")
    ap.add_argument("--every", type=int, default=1,
                    help="screencast everyNthFrame; 2 halves the CPU")
    ap.add_argument("--chromium-arg", action="append", default=[],
                    help="extra chromium argument (repeatable)")
    ap.add_argument("--keep-cmd", action="store_true",
                    help="do not truncate /tmp/rbwin.cmd at startup")
    args = ap.parse_args()

    if shutil.which(args.chromium) is None:
        log("%s is not on PATH -- Step 1's install is what puts it there"
            % args.chromium)
        return 1
    dec_bin = decoder_binary()
    if dec_bin is None:
        log("jpeg565 is not built and could not be built")
        return 1

    # A previous run's commands are not this run's. Truncated rather than removed:
    # it is a regular file the shim appends to by name, so a missing one is not the
    # failure mode to design around.
    if not args.keep_cmd:
        open(CMD, "w").close()

    proc, wfd, rfd = start_chromium(args.chromium, args.profile, args.url,
                                    args.width, args.height, args.chromium_arg)
    cl = Client(proc, Pipe(rfd, wfd))
    dec = None
    fw = None
    cmd_fd = -1
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True
        _STOPPING[0] = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    try:
        setup(cl, args)
        dec = Decoder(args.width, args.height, dec_bin)
        fw = FrameWriter(args.width, args.height)
        cmd_fd = os.open(CMD, os.O_RDONLY | os.O_NONBLOCK)
        carried = ""
        last = 0.0
        cl.frames = 0                   # frames WRITTEN, this run
        cl.last_frame = time.time()
        cl.armed = False                # the stall watchdog's two fields
        cl.deadline = 0.0
        cl.stalls = 0
        cl.next_beat = cl.last_frame + BEAT_S
        log("up: %dx%d on %s" % (args.width, args.height, FRAME))

        while not stopped and proc.poll() is None:
            cl.pump(TICK)

            # The command file is a REGULAR file, so select() on it never blocks
            # (it always reads ready). Polling it each tick instead costs one read
            # syscall and cannot busy-loop.
            lines = []
            while True:
                try:
                    data = os.read(cmd_fd, 4096)
                except OSError as e:
                    if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                        break
                    raise
                if not data:
                    break
                # THE TAIL IS KEPT: a read can land in the middle of a line, and
                # processing half a `scroll 12 1` as a command is worse than
                # waiting one tick for the rest of it.
                carried += data.decode("utf-8", "replace")
                parts = carried.split("\n")
                carried = parts.pop()
                lines.extend(p for p in parts if p.strip())
            if lines:
                run_commands(cl, lines)

            # A BACKLOG IS STALE BY DEFINITION: with one decode per frame, decoding
            # every frame of a page that repaints faster than this can decode would
            # put the driver further behind with each one, showing the operator a
            # page that is older the longer they look at it. So every screencast
            # frame is ACKED -- chromium stops sending if one is not -- but only the
            # newest is decoded.
            frames = [i for i, ev in enumerate(cl.events)
                      if ev.get("method") == "Page.screencastFrame"]
            for i in frames[:-1]:
                cl.rpc("Page.screencastFrameAck",
                       {"sessionId": cl.events[i]["params"]["sessionId"]},
                       session=cl.session)
                cl.events[i] = None

            wrote = False
            for ev in cl.events:
                if ev is None:
                    continue
                try:
                    if on_event(cl, dec, fw, ev):
                        wrote = True
                except RuntimeError as e:
                    # One frame that would not decode is not the browser dying, and
                    # the next one is a tenth of a second behind it.
                    log("frame dropped: %s" % e)
            cl.events.clear()

            now = time.time()
            if wrote or now - last >= STATUS_INTERVAL:
                # Polled, not event-driven: the page can focus a field with no
                # navigation and no frame, and the generation is what the shim's
                # keyboard hangs off.
                gen = cl.evaluate("window.__rbGen || 0")
                if isinstance(gen, (int, float)):
                    cl.focus = int(gen)
                write_status(cl.url, cl.focus)
                last = now

            # THE STALL WATCHDOG. Armed by a command (do_command), disarmed by a
            # frame (on_event), so it can only ever fire on a repaint that was due
            # and never came. The first expiry restarts the screencast, which is
            # cheap and cannot lose the page; a wedge that survives that keeps being
            # retried at a widening interval rather than every tick, so a pipe that
            # is genuinely dead cannot turn this into a log flood. What it does NOT
            # yet do is restart chromium -- by hand that is the proven cure, but the
            # threshold for killing a browser out from under a reading operator
            # should come from a heartbeat caught in the act, not from a guess.
            if cl.armed and now >= cl.deadline:
                cl.stalls += 1
                if cl.stalls == 1:
                    log("screencast stalled: no frame %gs after a command -- "
                        "restarting it" % STALL_S)
                else:
                    log("screencast still stalled (%d): chromium answers CDP but "
                        "emits no frames" % cl.stalls)
                try:
                    cl.rpc("Page.stopScreencast", session=cl.session)
                    cl.rpc("Page.startScreencast", cl.sc_params, session=cl.session)
                except RuntimeError as e:
                    log("screencast restart failed: %s" % e)
                cl.deadline = now + (STALL_S if cl.stalls < 2 else 4 * STALL_S)

            if now >= cl.next_beat:
                cl.next_beat = now + BEAT_S
                log("beat: frames=%d last=%.0fs stalls=%d renderers=%d url=%s"
                    % (cl.frames, now - cl.last_frame, cl.stalls,
                       renderer_count(), cl.url))

        if proc.poll() is not None and not stopped:
            why = chrome_why()
            log("chromium exited (%s)%s"
                % (proc.returncode, (": " + why) if why else ""))
    except RuntimeError as e:
        # ^C lands here as "stopping during ..." rather than as a failure to report.
        if stopped:
            log("stopped")
            return 0
        log(str(e))
        return 1
    finally:
        if cmd_fd >= 0:
            os.close(cmd_fd)
        if fw:
            fw.close()
        if dec:
            dec.close()
        cl.pipe.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
