# 20 — the status page, and the configuration screen

`confscreen.py`, served by `rblive4-conf.service`, is the unit's page on the LAN. It is
reachable at **`http://<unit>:5904/`** by default (`RB_CONF_HTTP_PORT`).

## Why it is a service of its own

The page that already existed — the one `vncserve` serves — **cannot be the page you use to
turn VNC on**. `rblive4-vnc.service` is enabled only when `RB_VNC=1` (`install.sh`), so on a
unit with VNC off there is no `vncserve` and therefore no page at all. That is the
chicken-and-egg this service exists to break, and it is also why the page is **not** gated on
`RB_VNC` or on `RB_AUTOSTART`: it has to be up on a unit whose player is disabled and whose
viewer was never installed.

There is a second reason, and it is the sharper one. The viewer's page has no authentication
and its controls are **state-changing GETs** (`vnc_http.c` — `/session`, `/input`, `/mode`),
so anyone who can reach that port can start screen sharing, or turn clicks in the picture
into real presses on the glass — including the top-menu strip, where raw x 1884 is USB STOP.
**Measured, 2026-10-09**, by reading those routes. Closing that is part of this work: when the
write path lands, the viewer's page moves to loopback-only and this page owns the LAN port.

## What it does

**This page has no password, and there is nothing to opt into.** It had a sign-in — a session cookie, a
per-session CSRF value in every form, a 128-bit id held in memory, logins rate-limited, an empty
`RB_PASSWORD` failing closed with 503 — and the operator retired the lot. A configuration screen you
have to sign in to is one you stop using, and on a single-operator appliance the lock cost more than it
bought. So there is no `/login`, no `/logout`, no session, no CSRF token, and no `RB_CONF_AUTH`; the
whole mechanism is gone rather than switched off, which is why a conf still carrying `RB_CONF_AUTH=1`
from the field is inert and `doctor.sh` no longer reports a lock either way.

**What is left guarding the writes is a same-origin check**, and it is worth being exact about it: a
browser sends `Origin` on a cross-site POST, and the handler refuses one whose host is not its own, so a
page the operator merely *visits* cannot drive this one. It is not a secret and it does not stop a client
that sets its own headers.

**And the page does not say any of that.** It carried a paragraph about having no password, at the top of
every visit, and the operator had it taken off — which is the right shape: it was a notice about the page
rather than about the unit, it sat above the sections on every tab, and a page saying nothing about a lock
it does not have is not hiding one. `doctor.sh` still reports the posture in words for anyone who wants it
there, and this document is the long version. The footer took the same treatment for the same reason: it
had a second copy of the sentence, and a second copy is how it went on promising *"writes need the
password"* for a while after there was no password to need.

**The VNC client password is a different thing**, and still real: `RB_PASSWORD`, from which
`RB_VNC_PASSWORD` derives, and macOS's Screen Sharing will not connect without one. It has nothing to do
with this page now — an empty one does not stop a setting being changed.

It shows one section at a time, navigated from the left:

| section | what it is, and where it comes from |
|---|---|
| **status** | three headings in one tab — *player*, *device info* and *boot info*. The pid (found by walking `/proc/*/cmdline` for the loader plus an argv ending `/rbp`, skipping `edb_streamd`), how long it has been **running** (field 22 of `/proc/<pid>/stat` ÷ `SC_CLK_TCK`), the frame count, and the frame **rate** — and, under a *device info* heading in the same tab, the machine it runs on: uptime, load, memory, SoC temperature, `get_throttled` decoded into words, free space, the last health line — and, under a *boot info* heading, the last start: the tail of `/run/rblive4/boot.log` and the last `boot.stage`, the same stage stream the boot screen paints |
| **services** | one row per unit, in four columns: the unit, its **Status** (`is-active`), its **startup** (`is-enabled`) as a two-button pair with the state in force drawn as pressed, and its **actions** (start/restart) — because this is where you look when something is wrong |
| **viewer settings** | the three live switch files (`vnc.live`, `vnc.mode`, `vnc.input`), whether the RFB and viewer-page ports answer, the preview (pointed at the viewer, which owns the capture), and **the viewer's own buttons** — enable/disable it, and start/stop sharing |
| **vnc settings** | the viewer's own knobs, **editable** |
| **rbp settings** | the player's and the unit's, likewise editable, plus the depth pair and whether it agrees |

### The settings form is built from the editor's schema

Every field — its label, its help, its allowed values and whether it is editable at all — comes from
`confedit.SCHEMA`, which is the same object `confedit.write_local()` validates against. So **the form
cannot offer something the writer would refuse**, and a setting appears on the page by being added to
the schema and nowhere else. The keys that are *pairs* or need a shim rebuild (`RB_FB_LIE_BPP`,
`RB_DFB_PRESENT`, the audio-mirror trio, `RB_AUTOSTART`, the paths) are schema entries marked read-only
and rendered with their reason — shown, explained, and impossible to submit.

**A row is a label, a control, and a hint — and nothing else.** Each row used to carry a dim
`RB_KEY = value` line under its label, which printed the value a second time and the key on all twenty
rows, and it repeated the same *"restart the viewer to apply"* sentence on every one of them — so the
single line worth reading was buried in twenty copies of one that was not. Now the **key name lives in
the (i)**, which is where a reader about to hand-edit `rb.local.conf` is already looking; the **value is
only in the control that already shows it**; and the restart sentence is **said once per group**,
naming the units the schema says that group's *savable* settings belong to (a `service:` row is changed
by a button elsewhere on the page, so its unit is not named and no promise is made that this table cannot
keep). The hint column is left for the few rows with something short to say — *read-only*, *not editable
here: no config editor*, *changed with the buttons under viewer settings*, or *\(set\)* for the password, which
must never be printed but whose being set is worth knowing. A `service:` hint names the section the
button is really in, which is why it says *viewer settings* for the viewer and *under services* for
anything else: a hint that points confidently at the wrong section is worse than no hint.

**A save changes one line and restarts nothing.** `confedit` validates the value by type, refuses
anything with a shell metacharacter in it, locks the file, checks the result with `sh -n`, keeps a
one-generation `.prev`, and copies every other byte of the operator's file through untouched — verified
by comparing the file before and after a save round-trip. Restarting is a *separate, explicit* action on
the services screen, because a save that blanked the screen would be a trap.

What a save does instead is record which **unit owes a restart** (`/run/rblive4/conf.dirty.<unit>`,
stamped with that unit's `ActiveEnterTimestampMonotonic`), and the settings group says *saved, not
applied yet: restart rblive4-vnc* until that unit next starts — at which point the stamp no longer
matches, and the debt is discharged by itself. Both stamps are monotonic and on this boot, and the marker
is in `/run` (tmpfs), so a reboot clears both together and cannot leave a stale debt behind.

The sections are a real nav rather than a long scroll: one is shown at a time, and their ids live
*inside* the regions the poll re-reads, so the left-hand links keep pointing at something after every
refresh.

### The restart is two steps, and it is serialised

The button answers with a question — a **real page**, not a script dialog, and it works with the script
blocked — because this is the one control that acts on something the operator can *see*. Confirming it
starts `rblive4-boot` **first** and then restarts `rblive4`, so the screen is narrated rather than
simply blank for fifteen seconds. It is serialised by an `flock` **and a 20-second cooldown**, because
two overlapping `systemctl restart rblive4` invocations **wedge rbp on this unit** — measured, and the
reason this button was not built with the others; a second press is refused with the reason shown on
the page, at the button. Nothing it runs names the player path, because `start-rb.sh`'s `cleanup()`
kills by cmdline match.

### Every service row has its own start/restart button

**`status` and `startup` are different questions, and the page keeps them apart.** Status is
`is-active` — running *now*. Startup is `is-enabled` — will systemd start it at boot, i.e. is there a
symlink. A unit can be running and not set to return, and that is a normal state rather than an error.
The startup cell is a pair of buttons, both states always shown with the one in force drawn as pressed,
so the control states the value rather than only the value a press would give; the pressed one is
`.cur`. Enabling and disabling start and stop nothing — they write the symlink and stop there, which
is why `actions` is a separate column.

**Two units can be enabled but never disabled**, and their `disabled` button is *itself disabled* —
greyed and unpressable — with the reason on the page's usual (i) beside it. `rblive4`, because it is
the appliance: disabling it at boot from a browser could leave a unit that never starts rbp, which is
the rule `RB_AUTOSTART` is read-only for. And `rblive4-conf`, because disabling the page at boot takes
away the page you would use to turn it back on. Only the *disable* direction is refused, deliberately:
enabling is always permitted, so a unit that somehow came up disabled is recoverable from here.
`test_confscreen.py` pins the greying, the refusal, and that enabling still works.


One button per row, and the label follows the state: **restart** when the unit is running, **start** when
it is not. `systemctl restart` starts a stopped unit anyway, so one verb would do — but the button says
which it will be rather than making the operator know that.

**The unit name is the only value on this page that reaches `systemctl` as a variable**, so it is the
only place a form field could become an argument. It never does: the name is looked up in `UNITS`, a
tuple of literals, and anything else is refused with a 400 *before* a process is spawned. The tuple is
therefore the security boundary, not a list of things to display — `../../etc/passwd; reboot` and
`rblive4; reboot` are both just names that are not in it. There is no shell on any path; every branch
passes a constant argv.

**The player is not in the generic branch.** Its row posts to the two-step confirm above instead, and
`service_action()` routes `rblive4` through `restart_player()` — so it keeps the lock, the cooldown, and
the boot-screen-first step. Folding it into the generic path would have dropped all three.

**This page's own unit is fired and not waited for.** `systemctl restart rblive4-conf` kills the process
answering the request, so waiting on it would mean the browser never gets a reply and the operator sees a
failed button that in fact worked. It is spawned detached, and the redirect is already on its way out.

A restart of the viewer does **not** stop screen sharing: `vnc_live_get()` only reads the switch file,
and only a page's own sharing button writes it. Sharing *does* default to off after a power cycle,
because the switch lives in `/run` — `RB_VNC_LIVE` is the setting for units that should come up serving.

### The frame counter is the number that matters

`rbp` can be **running, holding the framebuffer, turning its render loop, and painting
nothing** — a black screen with a live process and no error anywhere. The page therefore
reports the count of `DS_HW_Glib3_DFB.c <1106>` lines in `rbp.log` (one per frame, ~46/s
while the picture works, and never while it is blank) **and its rate**, and says in words
which of three states the player is in:

* *rbp is NOT running*
* *rbp is running and painting*
* *rbp is running and drawing NOTHING (the blank-screen state)*

That third state is the defect this port has chased most; naming it on a page is worth more
than any single number on it.

### The depth pair is shown and never editable

`RB_FB_LIE_BPP` is **half of a pair** with the layer-format word inside the patched player.
A pair that disagrees is rbp SIGSEGV to a black screen with a systemd restart loop, and
nothing in `rbp.log` names the cause (`rb.conf`'s own note, and `start-rb.sh`'s check). So
the page shows both halves and whether they agree — and the form that will exist later will
not offer to change either.

## Configuration

| knob | default | what it does |
|---|---|---|
| `RB_CONF` | `1` | whether `install.sh` enables `rblive4-conf.service` |
| `RB_CONF_HTTP_PORT` | `5904` | the page's port |
| `RB_CONF_BIND` | `0.0.0.0` | what it binds |
| `RB_CONF_REFRESH_S` | `5` | how often the page re-reads its data, seconds; `0` leaves the poll out |

**The refresh re-reads the data, it does not reload the page.** A `<meta http-equiv=refresh>` was the
first version, and it reset the scroll position and discarded anything half-typed — the sign-in box it
was built around is gone, but a form in the settings still loses an edit every five seconds under a
reload, so the reason outlived the case that found it. So the page carries one small inline script that fetches
`/data` (drawn by the *same* function as the first load, so the page and its updates cannot drift) and
replaces **only the section being looked at**. That last part is not an optimisation: the first version
swapped the whole content region, which re-created every section — and the ones that were hidden come
back *without* `hidden`, so for an instant all eight were on screen, every five seconds. **It looked
exactly like a page reload**, which is the thing this was built to stop. The other sections keep what
they had and are refreshed when you switch to them.

The notice at the top is deliberately outside the re-read regions — it is about the page, not about the
data — and the page is complete without
the script: the first load has every figure in the HTML, so a browser that blocks it shows a correct page
that simply does not update itself. Still no CDN, no framework, no build step.

**`RB_PASSWORD` is the viewer's credential and nothing else now** — what a VNC client is asked
for, and what `RB_VNC_PASSWORD` derives from. It does not gate this page: that is the sign-in the
operator retired, and a unit whose `RB_PASSWORD` is still the shipped `password` is a unit whose
*VNC clients* are using a guessable password, which is a viewer problem with a viewer fix. It
defaults to `password` so the feature works out of the box — a deliberate temporary choice —
and `RB_VNC_PASSWORD` remains the same credential under its old name, deriving from
`RB_PASSWORD` unless set explicitly, so existing `rb.local.conf` files keep working. `doctor.sh`
warns while the value is still the placeholder, and warns separately if the two names have been
set apart.

`RB_CONF` and the port are the only two an install consults; the rest the service reads
itself at startup, and **every path and command it uses can be overridden by an environment
variable** — which is what lets `test_confscreen.py` run it against a fake unit on a
workstation.

## Tests

`scripts/device/test_confscreen.py`, run by `make -C scripts/device test`. It follows
`test_bootscreen.py`'s idiom: no framework, the module loaded by path so its globals can be
patched, and the daemon run as a **real subprocess** with fake `/proc`, fake sysfs, a fake
`systemctl`/`vcgencmd` and a fake `rbp.log`, asserting on real HTTP responses. It drives the
frame-counter state machine through actual requests — first poll, frames arriving, frames
stopped — because that is the check that separates "no player" from "a player painting
nothing", and it asserts that a POST writes nothing and that `rb.local.conf` is byte-identical
after every request.

## Not done yet

Stated plainly so that nobody reads this page as more than it is:

* **the viewer's page is still on the LAN, unauthenticated** — its `/session`, `/input` and `/mode`
  are state-changing GETs, and moving it to loopback-only is the next landing. It is the remaining
  unauthenticated way in;
* **no TLS**, and **no privilege drop** — the service runs as root, as every unit here does, so the
  write path runs as root too;
* the display-path knobs, the audio-mirror trio (`rb.conf` states a change needs the shim rebuilt,
  not a restart) and the Pro DJ Link knobs are deliberately out of scope — read-only, shown, and
  refused by the writer rather than merely absent from the form.
