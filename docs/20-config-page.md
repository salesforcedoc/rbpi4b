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

## What it does today: nothing but look

**This landing is read-only.** No request changes a byte of the unit's state; there is no
write path, no password and no restart button yet, and `POST /` is refused by method (405)
rather than by a check inside a handler that could later be got wrong. The configuration
form, its authentication and its restart button are the next landing.

It shows:

| section | what it is, and where it comes from |
|---|---|
| **player** | the pid (found by walking `/proc/*/cmdline` for the loader plus an argv ending `/rbp`, skipping `edb_streamd`), the frame count, and the frame **rate** |
| **services** | `is-active` and `is-enabled` for `rblive4`, `rblive4-boot`, `rblive4-vnc`, `healthwatch`, `rblive4-conf` |
| **launcher** | the tail of `/run/rblive4/boot.log` and the last `boot.stage` — the same stage stream the boot screen paints |
| **viewer** | the three live switch files (`vnc.live`, `vnc.mode`, `vnc.input`), whether the RFB and viewer-page ports answer, and the preview (pointed at the viewer, which owns the capture) |
| **unit** | uptime, load, memory, SoC temperature, `get_throttled` decoded into words, free space, the last health line |
| **settings** | the resolved values of the curated knobs, read-only, plus the depth pair and whether it agrees |

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
`/data` (the live halves, drawn by the *same* function as the first load, so they cannot drift) and swaps
its contents into `#data1`/`#data2`. The interactive part — the sign-in box and the buttons — is
deliberately outside those regions, which is the point. The page is complete without the script: the first
load has every figure in the HTML, so a browser that blocks it shows a correct page that simply does not
update itself. Still no CDN, no framework, no build step.

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

## Not done yet (the next landing)

Stated plainly so that nobody reads this page as more than it is:

* **no write path** — nothing is edited, no password, no restart button;
* **the viewer's page is still on the LAN** and still unauthenticated, because it has not yet
  moved to loopback-only;
* **no TLS** and **no privilege drop** — the service runs as root, as every unit here does,
  so an authenticated write endpoint running as root is the residual risk to design away when
  the write path lands;
* the display-path knobs, the audio-mirror trio (`rb.conf` states a change needs the shim
  rebuilt, not a restart) and the Pro DJ Link knobs are deliberately out of scope.
