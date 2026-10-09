#!/usr/bin/env python3
"""test_confedit.py -- the conf editor and its schema, tested against a fixture that looks
like a file a person actually typed.

THE POINT OF THIS SUITE IS THE INJECTION CHECKS. `rb.local.conf` is sourced by /bin/sh as
root, so a value that reaches it with a `;`, a backtick or a `$(` in it is remote root code
execution, not a bad setting. Everything else here -- the byte-for-byte preservation, the
backup, the lock -- protects the operator's own file; those checks protect the unit itself.

The fixture below deliberately contains the awkward things a real file has: an inline
comment, a quoted value, a commented-out assignment of a key we later set, a duplicate key,
and a blank line. A writer that regenerates the file passes none of them.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
CONFEDIT = os.path.join(HERE, "confedit.py")

_fails = []
_checks = 0


def check(cond, msg):
    global _checks
    _checks += 1
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        _fails.append(msg)


def load():
    spec = importlib.util.spec_from_file_location("confedit_under_test", CONFEDIT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


FIXTURE = """# my local overrides for this unit
RB_VNC_FPS=7          # bumped from 4 for the desk
RB_POINT_INVERT_X=1

# RB_PREWARM=0 -- tried this, reverted after a black screen
RB_MIDI_MAP='flx4'
RB_VNC_PORT=5905
RB_VNC_PORT=5901
"""


def test_schema(m):
    print("\n== the schema is well formed ==")
    keys = [e["key"] for e in m.SCHEMA]
    check(len(keys) == len(set(keys)), "no key appears twice")
    check(all("label" in e and "help" in e for e in m.SCHEMA if not e.get("readonly")),
          "every writable entry has a label and help text")
    check(all("label" in e for e in m.SCHEMA if e.get("readonly")),
          "read-only entries have a label too, so the form can name them")
    check(all(e.get("why") for e in m.SCHEMA if e.get("readonly")),
          "every read-only entry says WHY it is read-only")
    check(all(not e.get("why") for e in m.SCHEMA if not e.get("readonly")),
          "and no writable entry carries a reason it is not written")
    for e in m.SCHEMA:
        if e.get("readonly"):
            continue
        if e["type"] == "enum":
            check(len(e["choices"]) >= 2, "%s has real choices" % e["key"])
        if e["type"] == "int":
            check(e["lo"] < e["hi"], "%s has a sane range" % e["key"])


def test_validate(m):
    print("\n== validation ==")
    check(m.validate("RB_PREWARM", "1")[0], "bool '1' is accepted")
    check(not m.validate("RB_PREWARM", "2")[0], "bool '2' is refused")
    check(not m.validate("RB_PREWARM", "yes")[0], "bool 'yes' is refused (the file wants 0/1)")
    check(m.validate("RB_VNC_FPS", "12")[0], "an int in range is accepted")
    check(not m.validate("RB_VNC_FPS", "999")[0], "an int out of range is refused")
    check(not m.validate("RB_VNC_FPS", "4.5")[0], "a float is refused")
    check(not m.validate("RB_VNC_FPS", "4; rm -rf /")[0], "an int with a command is refused")
    check(m.validate("RB_POINT_KIND", "auto")[0], "an enum member is accepted")
    check(not m.validate("RB_POINT_KIND", "wibble")[0], "a non-member is refused")
    check(m.validate("RB_MIDI_MAP", "")[0], "an empty enum member is allowed where offered")
    check(m.validate("RB_AUDIO_DEV", "hw:CARD=DDJFLX4,DEV=0")[0],
          "a real ALSA device name is accepted")
    check(not m.validate("RB_AUDIO_DEV", "hw:CARD=x DEV=0")[0], "whitespace is refused")
    check(not m.validate("RB_AUDIO_DEV", "a" * 100)[0], "an over-long value is refused")

    print("\n== and the injection strings, one at a time ==")
    for bad in ["$(reboot)", "`id`", "a;b", 'a"b', "a'b", "a|b", "a&b", "a>b", "a<b",
                "a\nb", "a\\b", "a$b", "a{b}", "a(b)", "a!b"]:
        ok, _v, why = m.validate("RB_AUDIO_DEV", bad)
        check(not ok, "refused: %r" % bad)

    print("\n== read-only keys refuse, and say why ==")
    ok, _v, why = m.validate("RB_FB_LIE_BPP", "16")
    check(not ok and "pair" in why, "RB_FB_LIE_BPP is refused and names the pair")
    ok, _v, why = m.validate("RB_AUTOSTART", "0")
    check(not ok and "player" in why, "RB_AUTOSTART is refused for a stated reason")
    ok, _v, why = m.validate("RB_NOPE_NOT_A_KEY", "1")
    check(not ok, "an unknown key is refused")


def test_read(m):
    print("\n== reading the effective value ==")
    check(m.read_value(FIXTURE, "RB_VNC_FPS") == "7", "an unquoted value reads")
    check(m.read_value(FIXTURE, "RB_MIDI_MAP") == "flx4", "a quoted value reads unquoted")
    check(m.read_value(FIXTURE, "RB_VNC_PORT") == "5901",
          "a duplicated key reads the LAST one, which is the one shell will take")
    check(m.read_value(FIXTURE, "RB_PREWARM") is None,
          "a commented-out assignment does NOT read as a value")
    check(m.read_value(FIXTURE, "RB_VERBOSE") is None, "an absent key is None")


def test_set(m):
    print("\n== writing, with the operator's file preserved ==")
    out = m.set_value(FIXTURE, "RB_VNC_FPS", "9")
    check("RB_VNC_FPS=9          # bumped from 4 for the desk" in out,
          "the value is replaced and its inline comment is kept")
    check("# my local overrides for this unit" in out, "the header comment survives")
    check("# RB_PREWARM=0 -- tried this, reverted after a black screen" in out,
          "a commented-out assignment of another key is untouched")
    check("RB_MIDI_MAP='flx4'" in out, "a quoted unrelated value is untouched")
    check("RB_VNC_PORT=5905" in out and "RB_VNC_PORT=5901" in out,
          "the earlier duplicate is left alone (only the effective one changes)")
    check(out.count("\n") == FIXTURE.count("\n"), "no line was added or lost")

    out2 = m.set_value(FIXTURE, "RB_VNC_PORT", "6001")
    check("RB_VNC_PORT=5905" in out2 and "RB_VNC_PORT=6001" in out2,
          "setting a duplicated key rewrites the LAST occurrence")

    print("\n== appending a key that is not there ==")
    out3 = m.set_value(FIXTURE, "RB_PREWARM", "1")
    check(m.MARKER in out3, "a marker block is created")
    check("RB_PREWARM=1" in out3, "and the line is appended")
    check("# RB_PREWARM=0 -- tried this, reverted after a black screen" in out3,
          "the commented-out one is still there and still commented")
    check(m.read_value(out3, "RB_PREWARM") == "1", "and the new value is the effective one")
    out4 = m.set_value(out3, "RB_PREWARM", "0")
    # Count ASSIGNMENT LINES, not substrings: the fixture's comment legitimately contains
    # the text "RB_PREWARM=0", so a substring count reports two and looks like a failure.
    assigns = sum(1 for ln in out4.splitlines() if ln.startswith("RB_PREWARM="))
    check(assigns == 1 and m.read_value(out4, "RB_PREWARM") == "0",
          "a second write of the same key replaces the appended line, not adds another")
    check(out4.count(m.MARKER) == 1, "and the marker still appears exactly once")
    check("# RB_PREWARM=0 -- tried this, reverted after a black screen" in out4,
          "and the operator's commented-out line is still exactly as they left it")


def test_write(tmp, m):
    print("\n== the write itself ==")
    path = os.path.join(tmp, "rb.local.conf")
    with open(path, "w") as f:
        f.write(FIXTURE)

    ok, why = m.write_local(path, "RB_VNC_FPS", "9")
    check(ok, "a valid write succeeds (%s)" % why)
    with open(path) as f:
        after = f.read()
    check(m.read_value(after, "RB_VNC_FPS") == "9", "and takes effect")
    check("# my local overrides for this unit" in after, "and the file is still the operator's")
    check(os.path.exists(path + ".prev"), "a backup was kept")
    with open(path + ".prev") as f:
        check(f.read() == FIXTURE, "and the backup is the file as it was")
    r = subprocess.run(["/bin/sh", "-c", '. "$1"; printf "%s" "$RB_VNC_FPS"', "sh", path],
                       capture_output=True, text=True)
    check(r.stdout == "9", "and sourcing the file yields the new value")

    print("\n== the injection attempt, end to end ==")
    canary = os.path.join(tmp, "canary")
    before = open(path).read()
    ok, why = m.write_local(path, "RB_AUDIO_DEV", "x; touch " + canary)
    check(not ok, "a value with a command in it is refused: %s" % why)
    check(open(path).read() == before, "and the file is byte-identical afterwards")
    check(not os.path.exists(canary), "and nothing ran")
    ok, why = m.write_local(path, "RB_POINT_KIND", "auto; touch " + canary)
    check(not ok and not os.path.exists(canary), "the same through an enum is refused too")

    print("\n== a file that would not parse is never installed ==")
    broken = os.path.join(tmp, "broken.conf")
    with open(broken, "w") as f:
        f.write("if [ 1 = 1; then\n")
    good, err = m.sh_syntax_ok(broken)
    check(not good and err, "sh -n catches an unterminated if")
    good, _ = m.sh_syntax_ok(path)
    check(good, "and passes a file this module wrote")

    print("\n== and the backup rule ==")
    dangling = os.path.join(tmp, "dangling", "rb.local.conf")
    os.makedirs(os.path.dirname(dangling))
    with open(dangling, "w") as f:
        f.write("RB_VNC_FPS=4\n")
    # A symlink whose target directory does not exist: os.path.exists() is False, so the
    # code tries to take the backup -- and cannot.
    os.symlink(os.path.join(tmp, "nowhere", "prev"), dangling + ".prev")
    ok, why = m.write_local(dangling, "RB_VNC_FPS", "5")
    check(not ok and "backup" in why,
          "a write is REFUSED when the backup cannot be taken: %s" % why)
    check(m.read_value(open(dangling).read(), "RB_VNC_FPS") == "4", "and the file is unchanged")


def test_create_and_lock(tmp, m):
    print("\n== a file that does not exist yet, and two writers ==")
    path = os.path.join(tmp, "fresh.conf")
    ok, why = m.write_local(path, "RB_PREWARM", "1")
    check(ok and os.path.exists(path), "writing a missing file creates it (%s)" % why)
    check("RB_PREWARM=1" in open(path).read(), "with the value in it")
    check(not os.path.exists(path + ".prev"),
          "and no backup, because there was nothing to take one of")

    path2 = os.path.join(tmp, "two.conf")
    with open(path2, "w") as f:
        f.write("# both\n")
    errs = []

    def w(key, val):
        ok, why = m.write_local(path2, key, val)
        if not ok:
            errs.append((key, why))

    ts = [threading.Thread(target=w, args=("RB_VNC_FPS", "6")),
          threading.Thread(target=w, args=("RB_POINT_KIND", "rel"))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    check(not errs, "two concurrent writers both succeed (%s)" % errs)
    text = open(path2).read()
    check(m.read_value(text, "RB_VNC_FPS") == "6" and m.read_value(text, "RB_POINT_KIND") == "rel",
          "and BOTH values are in the file -- neither writer's edit was lost")


def test_password_wiring(m):
    """ONE credential, under ONE name -- pinned by sourcing the shipped rb.conf, not by
    reading it. `RB_PASSWORD` is the knob (the page writes it); `RB_VNC_PASSWORD` is the
    name vnc-run.sh and doctor.sh already read, and it must COME OUT equal unless someone
    sets it apart deliberately. The operator asked for this by name on 2026-10-09."""
    print("\n== one password, two surfaces ==")
    conf = os.path.join(HERE, "rb.conf")
    tmp = tempfile.mkdtemp(prefix="confedit-pass.")

    def resolve(extra=None):
        env = dict(os.environ, RB_DEPLOY_ROOT=tmp)
        env.update(extra or {})
        r = subprocess.run(["/bin/sh", "-c",
                            '. "$1"; printf "%s|%s" "$RB_PASSWORD" "$RB_VNC_PASSWORD"',
                            "sh", conf], env=env, capture_output=True, text=True)
        return r.stdout

    try:
        check(resolve() == "password|password",
              "by default both names resolve to the shipped placeholder, so it works at once")
        check(resolve({"RB_PASSWORD": "s3cret"}) == "s3cret|s3cret",
              "setting RB_PASSWORD alone changes BOTH surfaces")
        check(resolve({"RB_VNC_PASSWORD": "viewer-only"}) == "password|viewer-only",
              "setting RB_VNC_PASSWORD alone still overrides the viewer, as before")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    e = m.BY_KEY.get("RB_PASSWORD")
    check(e is not None, "RB_PASSWORD is a key the page may write")
    check(e and e.get("secret"), "and it is marked secret, so the page never prints it")
    check("RB_VNC_PASSWORD" not in m.BY_KEY,
          "while RB_VNC_PASSWORD is not writable from the page -- it is the alias, not the knob")


def main():
    m = load()
    test_schema(m)
    test_validate(m)
    test_read(m)
    test_set(m)
    test_password_wiring(m)
    tmp = tempfile.mkdtemp(prefix="confedit-test.")
    try:
        test_write(tmp, m)
        test_create_and_lock(tmp, m)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

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
