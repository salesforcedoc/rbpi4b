#!/usr/bin/env python3
"""test_bootscreen.py — the host checks for bootscreen.py.

Run on the workstation (no /dev/fb0, no Pi):

    python3 scripts/device/test_bootscreen.py

Two halves. The first pins the pure frame builder and the sysfs readers. The
second runs the daemon FOR REAL as a subprocess, with a plain file standing in
for /dev/fb0 and a small fake sysfs tree — every path the daemon touches is
overridable (RB_FB, RB_FB_NAME, RB_FB_SYSFS, RB_BOOT_LOG, RB_RUN_DIR), which is
the only reason a loop with an mmap'd device can be tested off the device at all.

The loop half exists because of a real bug found on the unit on 2026-10-09: with
a stale frame already on fb0 the daemon read it as "rbp has drawn" and exited
before painting, so the screen silently never appeared. `test_first_tick_paints`
is that regression, and `test_exit_on_foreign_frame` is the behaviour the fix
must not break.
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BOOTSCREEN = os.path.join(HERE, "bootscreen.py")


def load_module():
    spec = importlib.util.spec_from_file_location("bootscreen", BOOTSCREEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --- tiny harness -------------------------------------------------------------

_fails = []


def ok(msg):
    print("ok   %s" % msg)


def check(cond, msg):
    if cond:
        ok(msg)
    else:
        _fails.append(msg)
        print("FAIL %s" % msg)


# --- a stand-in framebuffer ---------------------------------------------------

def make_fake(dirname, name="vc4drmfb", vs="1280,800", stride=2560, bpp=16):
    """Build a tree that looks like the Pi's fb0 to the daemon, and return the
    env that points it there."""
    fb = os.path.join(dirname, "fb0")
    sysfs = os.path.join(dirname, "fb0-sysfs")
    namef = os.path.join(dirname, "fb0-name")
    os.makedirs(sysfs, exist_ok=True)
    with open(fb, "wb") as f:
        f.write(b"\x00" * (2560 * 800))
    with open(namef, "w") as f:
        f.write(name + "\n")
    with open(os.path.join(sysfs, "virtual_size"), "w") as f:
        f.write(vs + "\n")
    with open(os.path.join(sysfs, "stride"), "w") as f:
        f.write("%d\n" % stride)
    with open(os.path.join(sysfs, "bits_per_pixel"), "w") as f:
        f.write("%d\n" % bpp)
    env = dict(os.environ)
    env.update(RB_FB=fb, RB_FB_NAME=namef, RB_FB_SYSFS=sysfs,
               RB_BOOT_LOG=os.path.join(dirname, "boot.log"),
               RB_RUN_DIR=os.path.join(dirname, "run"))
    return env


def start_daemon(env):
    return subprocess.Popen([sys.executable, BOOTSCREEN], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_exit(p, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if p.poll() is not None:
            return True
        time.sleep(0.05)
    return p.poll() is not None


def fb_is_black(path):
    with open(path, "rb") as f:
        return f.read() == b"\x00" * (2560 * 800)


def write_frame(path, value, w=1280, h=800, stride=2560):
    with open(path, "r+b") as f:
        f.write(struct.pack("<H", value) * (w * h))


def stage_of(env):
    try:
        with open(os.path.join(env["RB_RUN_DIR"], "boot.stage")) as f:
            return f.read().strip()
    except OSError:
        return ""


# --- pure checks --------------------------------------------------------------

def pure_checks(m):
    frame = m.render([" 1.00  x"], 1, "waiting for rbp", 3.0)
    check(len(frame) == m.SIZE, "render returns exactly SIZE bytes (%d)" % m.SIZE)

    # border lit on all four edges, interior mostly dark
    def px(buf, x, y):
        return struct.unpack_from("<H", buf, (y * m.STRIDE + x * m.BPP))[0]
    check(px(frame, m.W // 2, 0) != 0, "top border lit")
    check(px(frame, m.W // 2, m.H - 1) != 0, "bottom border lit")
    lit = sum(1 for i in range(0, len(frame), 2)
              if struct.unpack_from("<H", frame, i)[0] != 0)
    check(lit < m.W * m.H // 8, "interior mostly black (it is a log, not a fill)")

    # heartbeat toggles
    f1 = m.render([], 1, "waiting for rbp", 1.0)
    f2 = m.render([], 2, "waiting for rbp", 1.0)
    check(f1 != f2, "heartbeat toggles between odd and even ticks")

    # the honest line renders amber
    amber = m.render([], 1, "rbp is running, drawing nothing", 12.0)
    green = m.render([], 1, "waiting for rbp", 12.0)
    def has_colour(buf, colour):
        c = struct.pack("<H", colour)
        return any(buf[i:i + 2] == c for i in range(0, len(buf), 2))
    check(has_colour(amber, m.AMBER), "the drawing-nothing line renders amber")
    check(not has_colour(green, m.AMBER), "the waiting line does not render amber")

    # _split
    check(m._split(" 12.34  hello") == ("12.34", "hello"),
          "_split separates the stamp")
    check(m._split("no stamp here")[1] == "no stamp here",
          "_split leaves a stampless line whole")

    # glyphs: 7 rows, each a 5-bit mask
    allseven = all(len(g) == 7 and all(0 <= row < (1 << m.GLYPH_W) for row in g)
                   for g in m.GLYPHS.values())
    check(allseven, "every glyph is 7 rows of 5-bit masks")


def geometry_checks(m, tmp):
    # Patch the module's own paths, not the environment: they are read once at
    # import, so setting os.environ here would be a no-op (which is how the first
    # draft of this test "refused" everything -- there is no /sys on a Mac, so the
    # two refusal checks passed vacuously).
    def point_at(root, **kw):
        env = make_fake(root, **kw)
        m.FB_NAME = env["RB_FB_NAME"]
        m.FB_SYSFS = env["RB_FB_SYSFS"]

    point_at(os.path.join(tmp, "g-ok"))
    check(m.geometry_ok() == 2560 * 800,
          "geometry_ok accepts 1280x800/stride 2560/16 bpp")
    point_at(os.path.join(tmp, "g-bpp"), bpp=32)
    check(m.geometry_ok() is None, "geometry_ok refuses the 32-bpp lie")
    point_at(os.path.join(tmp, "g-mode"), vs="1920,1080")
    check(m.geometry_ok() is None, "geometry_ok refuses a different mode")


# --- loop checks (the daemon, running for real) -------------------------------

def test_clean_start_paints(tmp):
    env = make_fake(os.path.join(tmp, "l-clean"))
    p = start_daemon(env)
    try:
        time.sleep(1.0)
        check(not fb_is_black(env["RB_FB"]), "on a black fb0 it paints")
        check(p.poll() is None, "and keeps running while rbp has not drawn")
        check(stage_of(env).startswith("up"), "boot.stage says 'up'")
    finally:
        p.kill()


def test_first_tick_paints(tmp):
    """THE REGRESSION: a stale frame on fb0 must not be read as 'rbp drew'."""
    env = make_fake(os.path.join(tmp, "l-stale"))
    write_frame(env["RB_FB"], 0xF800)          # a red "leftover" frame
    p = start_daemon(env)
    try:
        time.sleep(1.0)
        check(p.poll() is None,
              "a stale non-black frame does NOT make it exit (regression)")
        check(not fb_is_black(env["RB_FB"]) and stage_of(env).startswith("up"),
              "it paints over the stale frame instead")
    finally:
        p.kill()


def test_exit_on_foreign_frame(tmp):
    env = make_fake(os.path.join(tmp, "l-foreign"))
    p = start_daemon(env)
    try:
        time.sleep(1.0)
        check(p.poll() is None, "running before the foreign frame")
        write_frame(env["RB_FB"], 0xF800)      # someone else draws
        check(wait_exit(p, 3.0), "a foreign non-black frame makes it exit")
        check(p.returncode == 0, "and the exit is 0, never a failure")
        check(stage_of(env).startswith("rbp-drew"),
              "boot.stage records 'rbp-drew'")
    finally:
        p.kill()


def test_black_clear_is_ignored(tmp):
    """An all-black clear that is not ours must not end the screen."""
    env = make_fake(os.path.join(tmp, "l-clear"))
    p = start_daemon(env)
    try:
        time.sleep(1.0)
        write_frame(env["RB_FB"], 0x0000)      # rbp's start-up clear
        time.sleep(1.0)
        check(p.poll() is None, "an all-black clear does not end the screen")
    finally:
        p.kill()


def test_bad_geometry_refuses(tmp):
    env = make_fake(os.path.join(tmp, "l-badgeom"), bpp=32)
    p = start_daemon(env)
    try:
        check(wait_exit(p, 3.0), "a geometry mismatch exits")
        check(p.returncode == 0, "and exits 0 too")
        check(fb_is_black(env["RB_FB"]), "and paints nothing (never garbage)")
        check(stage_of(env).startswith("bad-geom"), "boot.stage names bad-geom")
    finally:
        p.kill()


def test_waits_for_vc4drmfb(tmp):
    env = make_fake(os.path.join(tmp, "l-notvc4"), name="simplefb")
    p = start_daemon(env)
    try:
        time.sleep(1.0)
        check(p.poll() is None, "a non-vc4drmfb fb0 makes it wait, not paint")
        check(fb_is_black(env["RB_FB"]), "and it has not painted")
    finally:
        p.kill()


def test_stays_up_drawing_nothing(tmp):
    env = make_fake(os.path.join(tmp, "l-stay"))
    os.makedirs(env["RB_RUN_DIR"], exist_ok=True)
    with open(env["RB_BOOT_LOG"], "w") as f:
        f.write("7.07  launching rbp (log: /opt/rblive4/log/rbp.log)\n")
    env["RB_DRAW_S"] = "1.0"
    p = start_daemon(env)
    try:
        time.sleep(2.5)
        check(p.poll() is None,
              "with rbp launched and drawing nothing it STAYS UP past RB_DRAW_S")
        with open(env["RB_FB"], "rb") as f:
            buf = f.read()
        amber = struct.pack("<H", load_module().AMBER)
        check(any(buf[i:i + 2] == amber for i in range(0, len(buf), 2)),
              "and the honest line is on screen in amber")
    finally:
        p.kill()


def main():
    m = load_module()
    tmp = tempfile.mkdtemp(prefix="bootscreen-test.")
    saved = {k: os.environ.get(k) for k in
             ("RB_FB_NAME", "RB_FB_SYSFS")}
    try:
        pure_checks(m)
        geometry_checks(m, tmp)
        test_clean_start_paints(tmp)
        test_first_tick_paints(tmp)
        test_exit_on_foreign_frame(tmp)
        test_black_clear_is_ignored(tmp)
        test_bad_geometry_refuses(tmp)
        test_waits_for_vc4drmfb(tmp)
        test_stays_up_drawing_nothing(tmp)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if _fails:
        print("%d FAILED" % len(_fails))
        for f in _fails:
            print("  - %s" % f)
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
