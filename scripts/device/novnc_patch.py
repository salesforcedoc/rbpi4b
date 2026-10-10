#!/usr/bin/env python3
"""novnc_patch.py -- the edits this unit makes to the installed noVNC pages.

WHY A PATCHER RATHER THAN A VENDORED PAGE. The tree's precedent for third-party sources is
fetch-upstream-and-apply-the-diff (tools/build-directfb/README.md), not shipping a copy: a
vendored vnc_lite.html would look like ours, rot silently against an apt upgrade, and lose the
upstream fixes that came with it. These edits are anchored on text that changes only if noVNC
changes, each one is applied at most once, each file is backed up once before the first edit, and
a set of anchors that does not match is LOGGED AND SURVIVED -- the client must never be taken down
by a page tweak. The stamp each file gains at the end is what makes a re-run cheap and quiet, and
what makes an upgraded (replaced) file get patched again rather than skipped.

The three edits, all the operator's ask on 2026-10-10:

  1. THE DOT CURSOR IS ALWAYS ON, in vnc_lite.html. That page draws NO cursor at all when the
     server has not drawn one -- and on this unit the server is rbp, which does not -- so the
     operator's pointer is invisible and the picture looks dead. RFB's `showDotCursor` draws a
     dot instead, which is also what macOS Screen Sharing does with a client-side cursor.
  2. THE Ctrl-Alt-Del BUTTON GOES, in vnc_lite.html. Nothing on this unit answers Ctrl-Alt-Del --
     rbp is a DJ player, not a desktop -- and the button sits over the picture on a page whose
     entire job is the picture. Its CSS, its handler and its function go with it, so no dead code
     is left behind claiming a feature.
  3. THE "Running without HTTPS" WARNING GOES, in app/ui.js. This unit serves the client over
     plain HTTP on the LAN by design; the warning is not a finding, it is the permanent state of
     a working install, printed in red across the top of every page load. The `isSecureContext`
     test went with it rather than being left as a silent `if` -- a check whose only effect was
     the message is not a check worth keeping.

Usage: novnc_patch.py [root]      (default /usr/share/novnc)
Exit is always 0: a failure here is a page that is unpatched, never a client that is down.
"""
import os
import shutil
import sys

STAMP = "patched by novnc_patch.py v1"

# --- the edits. `old` must appear EXACTLY ONCE, or the edit is refused and said so. ---

LITE = "vnc_lite.html"
UI = "app/ui.js"

EDITS = [
    (LITE, "the dot cursor is always on",
     "                      { credentials: { password: password } });",
     "                      { credentials: { password: password },\n"
     "                        // ALWAYS ON: rbp draws no cursor of its own, so without this\n"
     "                        // the pointer is invisible. See scripts/device/novnc_patch.py.\n"
     "                        showDotCursor: true });"),

    (LITE, "the Send CtrlAltDel button's CSS",
     """        #sendCtrlAltDelButton {
            position: fixed;
            top: 0px;
            right: 0px;
            border: 1px outset;
            padding: 5px 5px 4px 5px;
            cursor: pointer;
        }

""", ""),

    (LITE, "the sendCtrlAltDel() function",
     """        // Since most operating systems will catch Ctrl+Alt+Del
        // before they get a chance to be intercepted by the browser,
        // we provide a way to emulate this key sequence.
        function sendCtrlAltDel() {
            rfb.sendCtrlAltDel();
            return false;
        }

""", ""),

    (LITE, "the button's click handler",
     """        document.getElementById('sendCtrlAltDelButton')
            .onclick = sendCtrlAltDel;

""", ""),

    (LITE, "the button itself",
     """        <div id="sendCtrlAltDelButton">Send CtrlAltDel</div>
""", ""),

    (UI, "the \"Running without HTTPS\" warning",
     """        // We rely on modern APIs which might not be available in an
        // insecure context
        if (!window.isSecureContext) {
            // FIXME: This gets hidden when connecting
            UI.showStatus(_("Running without HTTPS is not recommended, crashes or other issues are likely."), 'error');
        }

""", ""),
]

# Where the stamp goes in each file, so a re-run is one line rather than a re-scan of the anchors:
# before </body> in the page, at the end of the module. Both are comments the browser and the
# engine skip, and neither can be mistaken for upstream text.
STAMPS = {
    LITE: ("</body>", "<!-- %s; see scripts/device/novnc_patch.py -->\n</body>"),
    UI: ("", "\n// %s; see scripts/device/novnc_patch.py\n"),
}


def patch(root, report):
    by_file = {}
    for rel, what, old, new in EDITS:
        by_file.setdefault(rel, []).append((what, old, new))

    for rel in (LITE, UI):
        path = os.path.join(root, rel)
        try:
            with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
        except OSError as e:
            report.append("%s: not readable (%s) -- skipped" % (rel, e.strerror))
            continue

        if STAMP in text:
            report.append("%s: already patched" % rel)
            continue

        pending = list(by_file.get(rel, []))
        applied, refused = [], []
        for what, old, new in pending:
            n = text.count(old)
            if n == 0:
                refused.append("%s: NOT FOUND, this is not the noVNC these edits were written "
                               "against" % what)
                continue
            if n > 1:
                refused.append("%s: appears %d times, refusing an ambiguous edit" % (what, n))
                continue
            text = text.replace(old, new)
            applied.append(what)

        if refused:
            # Nothing is written: a half-patched page is worse than an unpatched one, and the
            # refusals above name what to re-derive.
            report.append("%s: LEFT ALONE -- %s" % (rel, "; ".join(refused)))
            continue

        anchor, stamp = STAMPS[rel]
        if anchor:
            if text.count(anchor) != 1:
                report.append("%s: no single %s to stamp before -- LEFT ALONE" % (rel, anchor))
                continue
            text = text.replace(anchor, stamp % STAMP)
        else:
            text += stamp % STAMP

        try:
            backup = path + ".rbpi4b-orig"
            if not os.path.exists(backup):
                shutil.copy2(path, backup)          # once, and never overwritten
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(text)
        except OSError as e:
            report.append("%s: could not be written (%s)" % (rel, e.strerror))
            continue

        for what in applied:
            report.append("%s: %s" % (rel, what))
        report.append("%s: backed up once to %s" % (rel, os.path.basename(path) + ".rbpi4b-orig"))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/novnc"
    report = []
    try:
        patch(root, report)
    except Exception as e:                                  # never take the client down
        report.append("unexpected failure: %s" % e)
    for line in report:
        print("novnc_patch: %s" % line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
