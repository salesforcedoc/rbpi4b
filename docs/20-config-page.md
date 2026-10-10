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

**It reads, and it acts — behind a password that can be turned off.** Reads are open on the LAN.
Every write is a POST that needs a session: `/login` against `RB_PASSWORD` (read live from the conf),
a 128-bit id held in memory behind an `HttpOnly; SameSite=Strict` cookie, a per-session CSRF value in
every form, a cross-origin POST refused, a GET on a write endpoint answered **405 by method**, logins
rate-limited, and an EMPTY password **failing closed** with 503. `RB_CONF_AUTH=0` removes the password
entirely — and then the page says so at the top of its actions and `doctor.sh` warns, because that is a
choice rather than an accident, not a silent switch.

It shows one section at a time, navigated from the left:

| section | what it is, and where it comes from |
|---|---|
| **player** | the pid (found by walking `/proc/*/cmdline` for the loader plus an argv ending `/rbp`, skipping `edb_streamd`), how long it has been **running** (field 22 of `/proc/<pid>/stat` ÷ `SC_CLK_TCK`), the frame count, and the frame **rate** |
| **services** | `is-active` and `is-enabled` for `rblive4`, `rblive4-boot`, `rblive4-vnc`, `healthwatch`, `rblive4-conf` — and **the restart button**, because this is where you look when the player is wrong |
| **launcher** | the tail of `/run/rblive4/boot.log` and the last `boot.stage` — the same stage stream the boot screen paints |
| **viewer** | the three live switch files (`vnc.live`, `vnc.mode`, `vnc.input`), whether the RFB and viewer-page ports answer, and the preview (pointed at the viewer, which owns the capture) |
| **actions** | sign in or out, **enable/disable the viewer**, and **start/stop sharing** |
| **unit** | uptime, load, memory, SoC temperature, `get_throttled` decoded into words, free space, the last health line |
| **vnc settings** | the viewer's own knobs, **editable** |
| **rbp settings** | the player's and the unit's, likewise editable, plus the depth pair and whether it agrees |

### The settings form is built from the editor's schema

Every field — its label, its help, its allowed values and whether it is editable at all — comes from
`confedit.SCHEMA`, which is the same object `confedit.write_local()` validates against. So **the form
cannot offer something the writer would refuse**, and a setting appears on the page by being added to
the schema and nowhere else. The keys that are *pairs* or need a shim rebuild (`RB_FB_LIE_BPP`,
`RB_DFB_PRESENT`, the audio-mirror trio, `RB_AUTOSTART`, the paths) are schema entries marked read-only
and rendered with their reason — shown, explained, and impossible to submit.

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
first version, and it resets the scroll position and discards anything half-typed into the sign-in box
every few seconds — to update some numbers. So the page carries one small inline script that fetches
`/data` (drawn by the *same* function as the first load, so the page and its updates cannot drift) and
replaces **only the section being looked at**. That last part is not an optimisation: the first version
swapped the whole content region, which re-created every section — and the ones that were hidden come
back *without* `hidden`, so for an instant all eight were on screen, every five seconds. **It looked
exactly like a page reload**, which is the thing this was built to stop. The other sections keep what
they had and are refreshed when you switch to them.

The interactive part — the sign-in box and the buttons — is deliberately outside the re-read regions,
which is the point: re-fetching them is what ate a half-typed password. And the page is complete without
the script: the first load has every figure in the HTML, so a browser that blocks it shows a correct page
that simply does not update itself. Still no CDN, no framework, no build step.

**The credential is `RB_PASSWORD`, and it is one password for two surfaces** — the page's
writes, and the viewer's VNC clients. It defaults to `password` so the feature works out of
the box, which is a deliberate temporary choice: it is the first thing anyone would guess, and
it now stands between the LAN and editing this unit's configuration and restarting the player.
`RB_VNC_PASSWORD` is the same credential under its old name and **derives** from `RB_PASSWORD`
unless set explicitly, so existing `rb.local.conf` files keep working; `doctor.sh` warns while
the value is still the placeholder, and warns separately if the two names have been set apart.

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
