# tools/pi-bringup/

Scratch instruments for driving the port's **input** path on the Pi: a keyboard
whose keycodes and hold times can be chosen, a raw reader that shows what the
kernel actually emits, and a probe that reports what rbp did about it. They run
**on the unit**, as root, and they are the tools behind
[docs/16-input-and-hotplug.md](../../docs/16-input-and-hotplug.md).

| Script | Runs on | Purpose |
|---|---|---|
| [`vkeyd.py`](vkeyd.py) | the Pi | a virtual keyboard (`/dev/uinput`) that presses keys read from a FIFO, with a hold time you choose |
| [`rawread.py`](rawread.py) | the Pi | print every raw event from one `/dev/input/eventN`, independently of the shims |
| [`stateprobe.sh`](stateprobe.sh) | the Pi | watch rbp's own state words through `/proc/<pid>/mem` and log them when they change |
| [`drive-keys.sh`](drive-keys.sh) | the Pi | restart the player, drive a key sequence, capture `/dev/fb0` after each key |

## Why a synthetic keyboard

A human presses a key for about 100 ms. `evdev_io.c`'s reader was found to be
losing the release of any press that short — it closes and reopens every device
after each poll round, and a release that lands in the closed window is gone —
so `map_kbd.c`'s held-state latch stayed set and the *next* press of that key
was ignored. A human is precisely the wrong instrument for measuring a
100 ms-wide window; a hold time you can dial is the right one. `vkeyd.py` takes
`<keycode>` or `<keycode>@<hold-ms>`:

```sh
ssh root@<pi> 'mkfifo /tmp/vkeyfifo'
scp vkeyd.py stateprobe.sh drive-keys.sh root@<pi>:/tmp/
ssh root@<pi> 'setsid nohup python3 /tmp/vkeyd.py >/tmp/vkeyd.log 2>&1 &'
ssh root@<pi> 'sh /tmp/drive-keys.sh'
```

`drive-keys.sh` restarts the player for you. **It no longer has to.** When these
scripts were written the reader discovered devices once and only rescanned when
the set changed, so a device created after rbp started was never enumerated — every
test had to begin with a restart and a ~16 s wait. That was a defect in the reader
and it is fixed: a device hot-plugged into a running player is now picked up within
a second, and `drive-keys.sh`'s restart is a left-over convenience rather than a
requirement. Start `vkeyd.py` whenever you like.

If a test *does* seem to need the restart, that is a finding, not a procedure —
it is exactly how the hot-add gap was first noticed.

Then fetch and compare the frames (`docs/06-display.md` has the geometry); a
*screen that did not change* is a result, not a failure — it is how the up/down
sign turned out to be unmeasurable on the SOURCE panel, which has nothing to
move between.

## Traps these scripts exist on the far side of

* **Never put a `pkill -f`/`pgrep -f` pattern in the same command as the plain
  name it matches.** Three times in one session a pattern like `[v]keyd.py`
  matched the *command string itself*, because that same line also contained the
  plain path for the `cat` writing the file, and killed the session. Split the
  kill into its own invocation (`for old in $(pgrep -f "[s]p[.]sh"); do kill
  "$old"; done`) or kill by pid. `pgrep -f rbp` has the same problem, which
  `docs/13-raspberrypi4.md` records for `pkill -f "/root/pdj/rbp"`.
* **`for line in rf` on a FIFO ends at the first EOF.** When the writer closes,
  the reader gets EOF and the loop exits — after which the next `echo > fifo`
  blocks forever waiting for a reader that no longer exists. Reopen the FIFO
  inside a `while True`.
* **A reader thread's device list is a snapshot.** `stateprobe.sh` and
  `drive-keys.sh` find the player's pid by matching the first argv element of
  `/proc/*/cmdline`, not the process name: the launcher execs rbp through the
  loader, so `comm` is `ld-linux.so.3`, and a pattern matching the *path* also
  matches your own SSH command line.

These scripts are bring-up scratch and are not part of the deploy. They are
committed because the measurements in `docs/16` should be reproducible, and
because the next person to touch input will need the same lever.
