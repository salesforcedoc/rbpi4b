#!/usr/bin/env python3
"""
aseqdump2dump.py - turn an `aseqdump` capture into a rbpi4b MIDI dump, and
print the inventory a map is written from.

The shim records what a control surface actually sends as a mididump
(scripts/shims/mididump.h): one event per line, `t TYPE ch=.. note=..`. That is
the file `make -C scripts/shims test` replays, so it is what a map is
regression-tested against. Until now the only way to get one was the shim
itself running under rbp on the device -- which means the map cannot be checked
against the real unit until display and audio already work.

`aseqdump` needs none of that: it is one command and the unit enumerating. So
this converts what it prints into the shim's own format:

    aseqdump -p DDJ-FLX4:0 | python3 tools/aseqdump2dump.py --stats -o flx4.dump

--stats is the learn half. It lists every (channel, note) and (channel, CC) the
capture contains -- in the order they first appeared, so the listing can be read
against the order the controls were pressed -- and for a relative control it
prints the arithmetic a map needs: the values decoded under both of the two
conventions controllers use, and which one this data supports. The wrong
convention turns a steady turn into alternating +/-63 steps, so the correct one
is the one whose largest step is small. That verdict is measured; the numbers in
a vendor list are not.

Two things it cannot do, both because aseqdump does not print them:

  * No timestamps. aseqdump prints events as they arrive and no clock, so the
    converted dump's times are SYNTHETIC -- evenly spaced at --rate. That is
    enough for every assertion a map fixture makes about *what* a control sends:
    test_flx4.c pins the jog's sign and its position accumulation, and says in
    its own header that the jog's speed is the one thing derived from real
    elapsed time and therefore not a contract. For a dump whose times are real,
    use the shim's own MIDI_DUMP on the device.
  * No event aseqdump does not name. A line this tool cannot convert is written
    into the dump as a comment -- visible in the file, skipped by the parser --
    and counted on stderr (--verbose lists them). Nothing is dropped silently,
    because "the control sent nothing" and "the converter ate it" must not look
    the same.

Usage:
    python3 aseqdump2dump.py [options] [capture-file]     # stdin if no file

    --stats            print the inventory to stderr; with no -o, no dump is
                       written (the run is a learn run, not a fixture run)
    -o FILE            write the dump here instead of stdout
    --rate SECONDS     synthetic gap between events (default 0.002)
    --revs N           for --stats: prints the counts-per-revolution arithmetic
                       for each relative control, assuming its forward run was
                       exactly N revolutions
    -v, --verbose      also list the lines that could not be converted

--source CLIENT:PORT is available but should usually be left off: aseqdump is
already subscribed to one port, and the client number changes when the unit is
replugged, so a remembered "28:0" is a capture that silently comes back empty.
If it IS given and nothing matches, the report says so and names the sources
that did arrive.

Read-only, host-side, no dependencies beyond python3.
"""
import argparse
import os
import re
import sys
from collections import Counter

DUMP_HEADER = "# rbpi4b midi dump v1 -- t is seconds since the first event"

# aseqdump's own chatter, which is not an event and must not be reported as one
# it failed to convert. A real event line always starts with CLIENT:PORT.
BANNERS = ("waiting for data", "source  event")

# aseqdump prints a source column, then a padded event name, then the data.
# Matching on the event name rather than splitting on runs of spaces keeps this
# working when a long client name widens a column.
RE_NOTEON = re.compile(
    r"^\s*(\d+:\d+)\s+Note on\s+(\d+),\s*note\s+(\d+),\s*velocity\s+(\d+)\s*$")
RE_NOTEOFF = re.compile(
    r"^\s*(\d+:\d+)\s+Note off\s+(\d+),\s*note\s+(\d+)\s*$")
RE_CC = re.compile(
    r"^\s*(\d+:\d+)\s+Control change\s+(\d+),\s*controller\s+(\d+),\s*value\s+(\d+)\s*$")
# An event with no channel or data -- "Clock", "Active sensing", "Stop".
RE_NAMED = re.compile(r"^\s*(\d+:\d+)\s+([A-Za-z][A-Za-z0-9 /]*?)\s*$")

# sndrv_seq_event_type values, from <sound/asequencer.h> as the shim builds
# against it. A name that is not here is NOT given a made-up type: a wrong type
# in a SKIP line is a wrong statement about what the surface sent, which is the
# one thing a bring-up dump exists to get right.
SKIP_TYPES = {
    "Note": 5,
    "Keypress": 8,
    "Pitch bend": 13,
    "Program change": 11,
    "Channel pressure": 12,
    "Aftertouch": 8,
    "Control 14": 14,
    "Song position": 20,
    "Song select": 21,
    "Time signature": 23,
    "Key signature": 24,
    "Start": 30,
    "Continue": 31,
    "Stop": 32,
    "Clock": 36,
    "Tune request": 40,
    "Reset": 41,
    "Active sensing": 42,
    "SysEx": 130,
}


def skip_type(name):
    """The ALSA event type for an aseqdump event name, or None if unknown.

    None rather than a default, deliberately: a SKIP line naming the wrong type
    is a wrong statement about what the surface sent, and that is the one thing
    a bring-up dump exists to get right.
    """
    if name in SKIP_TYPES:
        return SKIP_TYPES[name]
    # aseqdump prints the payload size on the same line: "SysEx 4 bytes".
    if name.startswith("SysEx"):
        return SKIP_TYPES["SysEx"]
    return None


class Capture:
    """What was seen, in first-appearance order."""

    def __init__(self):
        self.order = []                 # [(kind, ch, num)] as first seen
        self.notes = {}                 # (ch, note) -> [on, off, Counter(vel)]
        self.ccs = {}                   # (ch, cc)   -> Counter(val)
        self.sources = Counter()        # every CLIENT:PORT that sent something
        self.skipped = 0
        self.unmapped = []              # raw lines, for --verbose

    def note(self, ch, note, vel, on):
        key = (ch, note)
        if key not in self.notes:
            self.notes[key] = [0, 0, Counter()]
            self.order.append(("note", ch, note))
        e = self.notes[key]
        e[0 if on else 1] += 1
        if on:
            e[2][vel] += 1

    def cc(self, ch, cc, val):
        key = (ch, cc)
        if key not in self.ccs:
            self.ccs[key] = Counter()
            self.order.append(("cc", ch, cc))
        self.ccs[key][val] += 1

    @property
    def events(self):
        return sum(e[0] + e[1] for e in self.notes.values()) + \
               sum(sum(c.values()) for c in self.ccs.values())


def classify(line):
    """What an aseqdump line is: (kind, src, ...) with kind one of noteon,
    noteoff, cc, skip, unmapped.

    One place for the decision, so the source bookkeeping below cannot be
    forgotten in one branch and kept in another.
    """
    m = RE_NOTEON.match(line)
    if m:
        return ("noteon", m.group(1)) + tuple(map(int, m.groups()[1:]))
    m = RE_NOTEOFF.match(line)
    if m:
        return ("noteoff", m.group(1)) + tuple(map(int, m.groups()[1:]))
    m = RE_CC.match(line)
    if m:
        return ("cc", m.group(1)) + tuple(map(int, m.groups()[1:]))
    m = RE_NAMED.match(line)
    name = m.group(2) if m else None
    etype = skip_type(name) if name else None
    if etype is not None:
        return ("skip", m.group(1), etype)
    return ("unmapped", "", line.strip())


def parse(lines, want_source, cap, out, rate):
    """Convert, filling `cap` and writing mididump lines to `out`."""
    t = 0.0
    out.write(DUMP_HEADER + "\n")
    for raw in lines:
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.lower().lstrip().startswith(BANNERS):
            continue
        c = classify(line)
        kind, src = c[0], c[1]
        if kind != "unmapped":
            # Counted before the filter, so a --source that matches nothing can
            # say which sources did arrive instead of reporting an empty run.
            cap.sources[src] += 1
            if want_source and src != want_source:
                continue
        if kind == "noteon":
            _, _, ch, note, vel = c
            out.write("%.6f NOTEON ch=%d note=%d vel=%d\n" % (t, ch, note, vel))
            cap.note(ch, note, vel, True)
        elif kind == "noteoff":
            _, _, ch, note = c
            out.write("%.6f NOTEOFF ch=%d note=%d vel=0\n" % (t, ch, note))
            cap.note(ch, note, 0, False)
        elif kind == "cc":
            _, _, ch, cc, val = c
            out.write("%.6f CONTROLLER ch=%d cc=%d val=%d\n" % (t, ch, cc, val))
            cap.cc(ch, cc, val)
        elif kind == "skip":
            out.write("%.6f SKIP type=%d\n" % (t, c[2]))
            cap.skipped += 1
        else:
            # Recorded, not converted: a comment, so the line is in the file
            # and the replay skips it.
            out.write("# aseqdump-unmapped: %s\n" % c[2])
            cap.unmapped.append(c[2])
        t += rate
    return cap


def histogram(counter, limit=8):
    items = counter.most_common()
    shown = ", ".join("%d(%d)" % (v, n) for v, n in items[:limit])
    if len(items) > limit:
        shown += ", +%d more values" % (len(items) - limit)
    return shown


def steps(values):
    """(centred, twos) step lists for a relative control's value stream.

    centred: the 0x40-centred convention (v - 64), which is what the DDJ-FLX4's
    own MIDI list describes as "increases from 0x41 / decreases from 0x3F".
    twos: the two's-complement one the list describes as "increases from 0x01 /
    decreases from 0x7F", which map_flx4.c gives to the browse knob.
    """
    centred = [v - 64 for v in values]
    twos = [(v - 128 if v >= 64 else v) for v in values]
    return centred, twos


def report(cap, revs, want_source, stream, jog_cc=34):
    w = stream.write
    w("== capture: %d events\n" % cap.events)
    if cap.sources:
        w("   from %s\n"
          % ", ".join("%s (%d)" % (s, n)
                      for s, n in sorted(cap.sources.items())))
    if want_source and not cap.events:
        w("!! --source %s matched nothing: nothing above is from it. Drop the\n"
          "   option, or use the source as it is now named.\n" % want_source)
    if cap.unmapped:
        w("   %d line(s) could not be converted (kept as comments)\n"
          % len(cap.unmapped))
    if cap.skipped:
        w("   %d event(s) recorded as SKIP\n" % cap.skipped)

    w("\n-- in the order the controls first appeared --\n")
    for kind, ch, num in cap.order:
        if kind == "note":
            on, off, vels = cap.notes[(ch, num)]
            w("  ch%-2d note %-3d  on/off x%d/%d  velocity %s\n"
              % (ch, num, on, off, histogram(vels)))
        else:
            hist = cap.ccs[(ch, num)]
            w("  ch%-2d cc   %-3d  x%-4d values %s\n"
              % (ch, num, sum(hist.values()), histogram(hist)))

    w("\n-- relative controls: which convention the data supports --\n")
    for kind, ch, num in cap.order:
        if kind != "cc":
            continue
        hist = cap.ccs[(ch, num)]
        values = []
        for v, n in hist.items():
            values.extend([v] * n)
        values.sort()                       # not the order that matters here
        centred, twos = steps(values)
        # The verdict is about the size of the largest step, not the values: a
        # hand turn under the right convention takes small steps, and under the
        # wrong one every step is a jump near half the range.
        cmax = max(abs(d) for d in centred) if centred else 0
        tmax = max(abs(d) for d in twos) if twos else 0
        if cmax == tmax:
            continue                        # nothing to choose between
        is_jog = (num == jog_cc)
        # An absolute control -- a fader, a knob -- is not relative at all and
        # decodes to large steps under BOTH conventions, so it gets no verdict
        # and no arithmetic. A control that IS relative takes steps of a count
        # or two under the right convention and enormous ones under the wrong
        # one; that gap is what makes the verdict a measurement rather than a
        # preference. The named jog CC is always decoded, because "which
        # convention is the platter" is a question worth answering even if its
        # turn was too short to divide.
        if not is_jog and not (min(cmax, tmax) <= 2 and max(cmax, tmax) >= 16):
            continue
        w("  ch%d cc%d: %d events\n" % (ch, num, len(values)))
        for label, ds, mx in (("0x40-centred (v-64)", centred, cmax),
                              ("0x01/0x7F (two's complement)", twos, tmax)):
            fwd = sum(d for d in ds if d > 0)
            back = sum(d for d in ds if d < 0)
            w("      %-28s largest step %-3d  net %+d  forward %+d  back %+d\n"
              % (label, mx, sum(ds), fwd, back))
        pick = "0x40-centred" if cmax < tmax else "0x01/0x7F"
        w("      -> the data supports %s: its largest step is %d, against %d.\n"
          % (pick, min(cmax, tmax), max(cmax, tmax)))
        if is_jog and revs:
            ds = centred if cmax < tmax else twos
            fwd = sum(d for d in ds if d > 0)
            if len(values) < 16:
                w("      too few events to divide into a turn -- turn more\n")
            else:
                w("      if the forward run was exactly %g revolution(s): "
                  "counts/revolution = %g\n" % (revs, fwd / revs))
    w("\n")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?",
                    help="aseqdump output (default: stdin)")
    ap.add_argument("--stats", action="store_true",
                    help="print the inventory and the convention arithmetic")
    ap.add_argument("-o", "--output",
                    help="write the dump here (default: stdout)")
    ap.add_argument("--rate", type=float, default=0.002,
                    help="synthetic seconds between events (default 0.002)")
    ap.add_argument("--revs", type=float, default=0.0,
                    help="for --stats: revolutions the forward run covered")
    ap.add_argument("--jog-cc", type=int, default=34,
                    help="the platter's CC, whose counts/revolution is the one "
                         "that means anything (default 34, the DDJ-FLX4)")
    ap.add_argument("--source", default="",
                    help="only this CLIENT:PORT, e.g. 28:0")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="list the lines that could not be converted")
    args = ap.parse_args()

    if args.capture:
        with open(args.capture, "r") as f:
            lines = f.readlines()
    else:
        lines = sys.stdin

    cap = Capture()
    # With --stats and no -o this is a learn run: the dump is what the fixture
    # run wants, and printing it would bury the inventory in the terminal.
    if args.output:
        out = open(args.output, "w")
    elif args.stats:
        out = open(os.devnull, "w")
    else:
        out = sys.stdout

    try:
        parse(lines, args.source, cap, out, args.rate)
    except KeyboardInterrupt:
        # aseqdump is still running and the operator has stopped the turn. The
        # capture so far is the whole point of the run, so report it rather than
        # losing it to a traceback.
        if args.stats:
            sys.stderr.write("\n-- interrupted; reporting what arrived --\n")
            report(cap, args.revs, args.source, sys.stderr, args.jog_cc)
        return 130
    finally:
        if out is not sys.stdout:
            out.close()

    if args.stats:
        report(cap, args.revs, args.source, sys.stderr, args.jog_cc)
        if args.verbose and cap.unmapped:
            sys.stderr.write("-- lines not converted --\n")
            for line in cap.unmapped:
                sys.stderr.write("  %s\n" % line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
